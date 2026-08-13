/*
 * snd_mem.c -- wav sound caching
 *
 * Copyright (C) 1996-2001 Id Software, Inc.
 * Copyright (C) 2010-2011 O. Sezer <sezero@users.sourceforge.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"

/*
===============================================================================

BAND-LIMITED RESAMPLER (uhexen2-og0w)

Sounds are resampled once at load time to shm->speed.  The legacy path picks
the nearest source sample, which leaves the spectral images of the source
right where they land -- for the 11025 -> 44100 case that covers nearly every
Hexen II asset, the first image sits about 11 dB below full scale.  That is
the grit you hear on loud 8-bit sfx.

snd_resample 1 replaces the nearest-sample pick with a Blackman-windowed sinc
polyphase filter.  Measured against a 128-tap double-precision reference over
all 285 8-bit mono sounds in pak0:

    point sampling (legacy)      10.66 dB SNR
    Catmull-Rom (reverted r5)    22.29 dB SNR
    16-tap windowed sinc         30.28 dB SNR

and total image energy for a 3 kHz tone drops 77 dB below the legacy path.

Three things sank the earlier Catmull-Rom attempt (c5a81cdeb, reverted in
d5d3bc614) and all three are handled here:

  1. It stored back into 8 bits, throwing away the precision the interpolation
     had just bought.  The band-limited path stores 16-bit -- see
     SND_ResampledWidth(), which S_LoadSound uses for the allocation so the
     two cannot disagree.
  2. It truncated on store ('sample >> 8'), which floors rather than rounds and
     put a -0.35 LSB DC offset on every sound.  This rounds.
  3. Its kernel overshot by 25%, so loud material hard-clipped (18.7% of output
     samples on a full-scale square).  A windowed sinc only exceeds full scale
     on genuine inter-sample peaks of already-full-scale source: 0.085% of
     samples across pak0, and clamping those costs 0.16 dB of the 30.28 above.
     That is cheap enough not to need headroom scaling, which would have cost
     every sound ~2 dB of level.

Defaults to off.  The change is squarely in taste territory -- removing the
images makes the sfx smoother, and "smoother" and "muffled" are the same
measurement -- and this bead has already been closed once on measurements
alone and reopened after a listening test.  uhexen2-edqp tracks the flip.

===============================================================================
*/

#define	SND_RESAMPLE_TAPS	16
#define	SND_RESAMPLE_PHASES	128

static float	snd_rs_bank[SND_RESAMPLE_PHASES][SND_RESAMPLE_TAPS];
static float	snd_rs_builtfor = 0.0f;	/* stepscale the bank holds, 0 = none */

static double SND_Sinc (double x)
{
	if (fabs(x) < 1e-12)
		return 1.0;
	x *= M_PI;
	return sin(x) / x;
}

/*
================
SND_BuildResampleBank

One phase per SND_RESAMPLE_PHASES-th of a source sample.  Cached on
stepscale: every asset in the game shares one, so this runs once.
================
*/
static void SND_BuildResampleBank (float stepscale)
{
	/* Cut off at the lower of the two Nyquists so that downsampling
	 * band-limits instead of aliasing -- the legacy path never did. */
	double	fc = (stepscale > 1.0f) ? 0.5 / stepscale : 0.5;
	double	halfw = SND_RESAMPLE_TAPS / 2.0;
	int	p, t;

	if (snd_rs_builtfor == stepscale)
		return;

	for (p = 0; p < SND_RESAMPLE_PHASES; p++)
	{
		double	frac = (double)p / SND_RESAMPLE_PHASES;
		double	sum = 0.0;

		for (t = 0; t < SND_RESAMPLE_TAPS; t++)
		{
			double	x = (t - (halfw - 1)) - frac;
			double	u = (x + halfw) / (2.0 * halfw);
			double	w, v;

			if (u < 0.0) u = 0.0; else if (u > 1.0) u = 1.0;
			w = 0.42 - 0.5 * cos(2.0 * M_PI * u) + 0.08 * cos(4.0 * M_PI * u);
			v = 2.0 * fc * SND_Sinc (2.0 * fc * x) * w;
			snd_rs_bank[p][t] = (float)v;
			sum += v;
		}
		/* Normalise each phase to unity DC gain, otherwise the residual
		 * ripple between phases shows up as a level wobble. */
		if (sum != 0.0)
		{
			for (t = 0; t < SND_RESAMPLE_TAPS; t++)
				snd_rs_bank[p][t] = (float)(snd_rs_bank[p][t] / sum);
		}
	}

	snd_rs_builtfor = stepscale;
}

/* Source fetch, clamped at both ends, normalised to 16-bit. */
static int SND_FetchSample (const byte *data, int inwidth, int n, int i)
{
	if (i < 0) i = 0; else if (i >= n) i = n - 1;
	if (inwidth == 2)
		return LittleShort ( ((const short *)data)[i] );
	return (int)( (unsigned char)(data[i]) - 128 ) << 8;
}

/*
================
SND_ResampledWidth

The store width ResampleSfx will use.  S_LoadSound sizes the cache block
with this, so the two must be asked the same question -- getting this
wrong overruns the allocation by half the sound.
================
*/
int SND_ResampledWidth (int inwidth, float stepscale)
{
	if (loadas8bit.integer)
		return 1;
	/* Only the interpolating path needs the extra precision; at
	 * stepscale 1 there is nothing between the samples to resolve. */
	if (snd_resample.integer && stepscale != 1.0f)
		return 2;
	return inwidth;
}

/*
================
ResampleSfx
================
*/
static void ResampleSfx (sfx_t *sfx, int inrate, int inwidth, byte *data)
{
	int		outcount, insamples;
	int		srcsample;
	float	stepscale;
	int		i;
	int		sample, samplefrac, fracstep;
	sfxcache_t	*sc;

	sc = (sfxcache_t *) Cache_Check (&sfx->cache);
	if (!sc)
		return;

	stepscale = (float)inrate / shm->speed;	// this is usually 0.5, 1, or 2

	insamples = sc->length;
	outcount = sc->length / stepscale;
	sc->length = outcount;
	if (sc->loopstart != -1)
		sc->loopstart = sc->loopstart / stepscale;

	sc->speed = shm->speed;
	sc->width = SND_ResampledWidth (inwidth, stepscale);
	sc->stereo = 0;

// resample / decimate to the current source rate

	if (stepscale == 1 && inwidth == 1 && sc->width == 1)
	{
// fast special case
		for (i = 0; i < outcount; i++)
			((signed char *)sc->data)[i] = (int)( (unsigned char)(data[i]) - 128);
	}
	else if (snd_resample.integer && stepscale != 1.0f)
	{
// band-limited case
		SND_BuildResampleBank (stepscale);

		for (i = 0; i < outcount; i++)
		{
			double		pos = (double)i * stepscale;
			int		s = (int)pos;
			int		ph = (int)((pos - s) * SND_RESAMPLE_PHASES);
			const float	*k;
			float		acc = 0.0f;
			int		t;

			if (ph >= SND_RESAMPLE_PHASES)
				ph = SND_RESAMPLE_PHASES - 1;
			k = snd_rs_bank[ph];
			for (t = 0; t < SND_RESAMPLE_TAPS; t++)
			{
				acc += k[t] * (float)SND_FetchSample (data, inwidth, insamples,
						s + t - (SND_RESAMPLE_TAPS / 2 - 1));
			}

			/* Round, don't truncate.  Clamp the inter-sample peaks. */
			sample = (int)floor ((double)acc + 0.5);
			if (sample > 32767) sample = 32767;
			else if (sample < -32768) sample = -32768;

			if (sc->width == 2)
			{
				((short *)sc->data)[i] = (short)sample;
			}
			else
			{
				int b = (sample + 128) >> 8;	/* round to 8 bits too */
				if (b > 127) b = 127;
				else if (b < -128) b = -128;
				((signed char *)sc->data)[i] = (signed char)b;
			}
		}
	}
	else
	{
// general case
		samplefrac = 0;
		fracstep = stepscale*256;
		for (i = 0; i < outcount; i++)
		{
			srcsample = samplefrac >> 8;
			samplefrac += fracstep;
			if (inwidth == 2)
				sample = LittleShort ( ((short *)data)[srcsample] );
			else
				sample = (int)( (unsigned char)(data[srcsample]) - 128) << 8;
			if (sc->width == 2)
				((short *)sc->data)[i] = sample;
			else
				((signed char *)sc->data)[i] = sample >> 8;
		}
	}
}

//=============================================================================

/*
==============
S_LoadSound
==============
*/
sfxcache_t *S_LoadSound (sfx_t *s)
{
	char	namebuffer[256];
	byte	*data;
	wavinfo_t	info;
	int		len;
	float	stepscale;
	sfxcache_t	*sc;
	byte	stackbuf[1*1024];		// avoid dirtying the cache heap

// see if still in memory
	sc = (sfxcache_t *) Cache_Check (&s->cache);
	if (sc)
		return sc;

//	Con_Printf ("%s: %x\n", __thisfunc__, (int)stackbuf);

// load it in
	q_strlcpy(namebuffer, "sound/", sizeof(namebuffer));
	q_strlcat(namebuffer, s->name, sizeof(namebuffer));

//	Con_Printf ("loading %s\n",namebuffer);

	data = FS_LoadStackFile(namebuffer, stackbuf, sizeof(stackbuf), NULL);

	if (!data)
	{
		Con_Printf ("Couldn't load %s\n", namebuffer);
		return NULL;
	}

	info = GetWavinfo (s->name, data, fs_filesize);
	if (info.channels != 1)
	{
		Con_Printf ("%s is a stereo sample\n",s->name);
		return NULL;
	}

	if (info.width != 1 && info.width != 2)
	{
		Con_Printf("%s is not 8 or 16 bit\n", s->name);
		return NULL;
	}

	stepscale = (float)info.rate / shm->speed;
	len = info.samples / stepscale;

	/* Must be the width ResampleSfx will actually store with, not the
	 * source width: the band-limited path widens 8-bit input to 16. */
	len = len * SND_ResampledWidth (info.width, stepscale) * info.channels;

	if (info.samples == 0 || len == 0)
	{
		Con_Printf("%s has zero samples\n", s->name);
		return NULL;
	}

	sc = (sfxcache_t *) Cache_Alloc ( &s->cache, len + sizeof(sfxcache_t), s->name);
	if (!sc)
		return NULL;

	sc->length = info.samples;
	sc->loopstart = info.loopstart;
	sc->speed = info.rate;
	sc->width = info.width;
	sc->stereo = info.channels;

	ResampleSfx (s, sc->speed, sc->width, data + info.dataofs);

	return sc;
}



/*
===============================================================================

WAV loading

===============================================================================
*/

static byte	*data_p;
static byte	*iff_end;
static byte	*last_chunk;
static byte	*iff_data;
static int	iff_chunk_len;

static short GetLittleShort (void)
{
	short val = 0;
	val = *data_p;
	val = val + (*(data_p+1)<<8);
	data_p += 2;
	return val;
}

static int GetLittleLong (void)
{
	int val = 0;
	val = *data_p;
	val = val + (*(data_p+1)<<8);
	val = val + (*(data_p+2)<<16);
	val = val + (*(data_p+3)<<24);
	data_p += 4;
	return val;
}

static void FindNextChunk (const char *name)
{
	while (1)
	{
	// Need at least 8 bytes for a chunk
		if (last_chunk + 8 >= iff_end)
		{
			data_p = NULL;
			return;
		}

		data_p = last_chunk + 4;
		iff_chunk_len = GetLittleLong();
		if (iff_chunk_len < 0 || iff_chunk_len > iff_end - data_p)
		{
			data_p = NULL;
			Con_DPrintf("bad \"%s\" chunk length (%d)\n", name, iff_chunk_len);
			return;
		}
		last_chunk = data_p + ((iff_chunk_len + 1) & ~1);
		data_p -= 8;
		if (!strncmp((char *)data_p, name, 4))
			return;
	}
}

static void FindChunk (const char *name)
{
	last_chunk = iff_data;
	FindNextChunk (name);
}

#if 0
static void DumpChunks (void)
{
	char	str[5];

	str[4] = 0;
	data_p = iff_data;
	do
	{
		memcpy (str, data_p, 4);
		data_p += 4;
		iff_chunk_len = GetLittleLong();
		Con_Printf ("0x%x : %s (%d)\n", (int)(data_p - 4), str, iff_chunk_len);
		data_p += (iff_chunk_len + 1) & ~1;
	} while (data_p < iff_end);
}
#endif

/*
============
GetWavinfo
============
*/
wavinfo_t GetWavinfo (const char *name, byte *wav, int wavlength)
{
	wavinfo_t	info;
	int	i;
	int	format;
	int	samples;

	memset (&info, 0, sizeof(info));

	if (!wav)
		return info;

	iff_data = wav;
	iff_end = wav + wavlength;

// find "RIFF" chunk
	FindChunk("RIFF");
	if (!(data_p && !strncmp((char *)data_p + 8, "WAVE", 4)))
	{
		Con_Printf("%s missing RIFF/WAVE chunks\n", name);
		return info;
	}

// get "fmt " chunk
	iff_data = data_p + 12;
#if 0
	DumpChunks ();
#endif

	FindChunk("fmt ");
	if (!data_p)
	{
		Con_Printf("%s is missing fmt chunk\n", name);
		return info;
	}
	data_p += 8;
	format = GetLittleShort();
	if (format != WAV_FORMAT_PCM)
	{
		Con_Printf("%s is not Microsoft PCM format\n", name);
		return info;
	}

	info.channels = GetLittleShort();
	info.rate = GetLittleLong();
	data_p += 4 + 2;
	i = GetLittleShort();
	if (i != 8 && i != 16)
		return info;
	info.width = i / 8;

// get cue chunk
	FindChunk("cue ");
	if (data_p)
	{
		data_p += 32;
		info.loopstart = GetLittleLong();
	//	Con_Printf("loopstart=%d\n", sfx->loopstart);

	// if the next chunk is a LIST chunk, look for a cue length marker
		FindNextChunk ("LIST");
		if (data_p)
		{
			if (!strncmp((char *)data_p + 28, "mark", 4))
			{	// this is not a proper parse, but it works with cooledit...
				data_p += 24;
				i = GetLittleLong();	// samples in loop
				info.samples = info.loopstart + i;
		//		Con_Printf("looped length: %i\n", i);
			}
		}
	}
	else
		info.loopstart = -1;

// find data chunk
	FindChunk("data");
	if (!data_p)
	{
		Con_Printf("%s is missing data chunk\n", name);
		return info;
	}

	data_p += 4;
	samples = GetLittleLong() / info.width;

	if (info.samples)
	{
		if (samples < info.samples)
			Sys_Error ("%s has a bad loop length", name);
	}
	else
		info.samples = samples;

	info.dataofs = data_p - wav;

	return info;
}

