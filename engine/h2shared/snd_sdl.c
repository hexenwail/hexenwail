/*
 * snd_sdl.c - SDL audio driver for Hexen II: Hammer of Thyrion (uHexen2)
 * based on implementations found in the quakeforge and ioquake3 projects.
 *
 * Copyright (C) 1999-2005 Id Software, Inc.
 * Copyright (C) 2005-2012 O.Sezer <sezero@users.sourceforge.net>
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
#include "snd_sys.h"

#if HAVE_SDL_SOUND

#include "snd_sdl.h"
#include "sdl_inc.h"

/* whether to use hunk for dma buffer memory, either 1 or 0  */
#define USE_HUNK_ALLOC		0

static char s_sdl_driver[] = "SDLAudio";

static int	buffersize;

static SDL_AudioStream	*audio_stream = NULL;


static void SDLCALL paint_audio (void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
	int	pos, tobufend;
	int	len1, len2;
	int	len = additional_amount;
	Uint8	*tmpbuf;

	(void)userdata;
	(void)total_amount;

	if (!shm)
	{	/* shouldn't happen, but just in case */
		/* Put silence into the stream */
		tmpbuf = (Uint8 *) calloc(1, len);
		if (tmpbuf) {
			SDL_PutAudioStreamData(stream, tmpbuf, len);
			free(tmpbuf);
		}
		return;
	}

	tmpbuf = (Uint8 *) malloc(len);
	if (!tmpbuf) return;

	pos = (shm->samplepos * (shm->samplebits / 8));
	if (pos >= buffersize)
		shm->samplepos = pos = 0;

	tobufend = buffersize - pos;  /* bytes to buffer's end. */
	len1 = len;
	len2 = 0;

	if (len1 > tobufend)
	{
		len1 = tobufend;
		len2 = len - len1;
	}

	memcpy(tmpbuf, shm->buffer + pos, len1);

	if (len2 <= 0)
	{
		shm->samplepos += (len1 / (shm->samplebits / 8));
	}
	else
	{	/* wraparound? */
		memcpy(tmpbuf + len1, shm->buffer, len2);
		shm->samplepos = (len2 / (shm->samplebits / 8));
	}

	if (shm->samplepos >= buffersize)
		shm->samplepos = 0;

	SDL_PutAudioStreamData(stream, tmpbuf, len);
	free(tmpbuf);
}

static qboolean S_SDL_Init (dma_t *dma)
{
	SDL_AudioSpec spec;
	int		tmp, val;
	int		obtained_freq, obtained_channels, obtained_bits;
	char	drivername[128];

	if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		Con_Printf("Couldn't init SDL audio: %s\n", SDL_GetError());
		return false;
	}

	/* Set up the desired format */
	spec.freq = desired_speed;
	spec.format = (desired_bits == 16) ? SDL_AUDIO_S16 : SDL_AUDIO_U8;
	spec.channels = desired_channels;

	/* Open the audio device with a stream callback */
	audio_stream = SDL_OpenAudioDeviceStream(
		SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		&spec,
		paint_audio,
		NULL
	);

	if (!audio_stream)
	{
		Con_Printf("Couldn't open SDL audio: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return false;
	}

	/* Query what we actually got */
	{
		int src_format_tmp;
		SDL_AudioFormat fmt;
		if (!SDL_GetAudioStreamFormat(audio_stream, &spec, NULL))
		{
			Con_Printf("Couldn't query audio stream format: %s\n", SDL_GetError());
			SDL_DestroyAudioStream(audio_stream);
			audio_stream = NULL;
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			return false;
		}
		obtained_freq = spec.freq;
		obtained_channels = spec.channels;
		fmt = spec.format;

		/* Determine bits from format */
		switch (fmt)
		{
		case SDL_AUDIO_S8:
		case SDL_AUDIO_U8:
			obtained_bits = 8;
			break;
		case SDL_AUDIO_S16:
			obtained_bits = 16;
			break;
		default:
			Con_Printf ("Unsupported audio format received (%u)\n", (unsigned)fmt);
			SDL_DestroyAudioStream(audio_stream);
			audio_stream = NULL;
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			return false;
		}
	}

	memset ((void *) dma, 0, sizeof(dma_t));
	shm = dma;

	/* Fill the audio DMA information block */
	shm->samplebits = obtained_bits;
	shm->signed8 = (spec.format == SDL_AUDIO_S8);
	if (obtained_freq != desired_speed)
		Con_Printf ("Warning: Rate set (%d) didn't match requested rate (%d)!\n", obtained_freq, desired_speed);
	shm->speed = obtained_freq;
	shm->channels = obtained_channels;

	/* Calculate samples: use a reasonable buffer size based on frequency */
	{
		int samples_per_callback;
		if (obtained_freq <= 11025)
			samples_per_callback = 256;
		else if (obtained_freq <= 22050)
			samples_per_callback = 512;
		else if (obtained_freq <= 44100)
			samples_per_callback = 1024;
		else if (obtained_freq <= 56000)
			samples_per_callback = 2048;
		else
			samples_per_callback = 4096;

		tmp = (samples_per_callback * obtained_channels) * 10;
	}
	if (tmp & (tmp - 1))
	{	/* make it a power of two */
		val = 1;
		while (val < tmp)
			val <<= 1;

		tmp = val;
	}
	shm->samples = tmp;
	shm->samplepos = 0;
	shm->submission_chunk = 1;

	Con_Printf ("SDL audio spec  : %d Hz, %d bits, %d channels\n",
			obtained_freq, obtained_bits, obtained_channels);
	{
		const char *driver = SDL_GetCurrentAudioDriver();
		if (driver)
			q_strlcpy(drivername, driver, sizeof(drivername));
		else
			strcpy(drivername, "(UNKNOWN)");
	}
	buffersize = shm->samples * (shm->samplebits / 8);
	Con_Printf ("SDL audio driver: %s, %d bytes buffer\n", drivername, buffersize);

#if USE_HUNK_ALLOC
	shm->buffer = (unsigned char *) Hunk_AllocName(buffersize, "sdl_audio");
#else
	shm->buffer = (unsigned char *) calloc (1, buffersize);
	if (!shm->buffer)
	{
		SDL_DestroyAudioStream(audio_stream);
		audio_stream = NULL;
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		shm = NULL;
		Con_Printf ("Failed allocating memory for SDL audio\n");
		return false;
	}
#endif

	/* Start playback */
	SDL_ResumeAudioStreamDevice(audio_stream);

	return true;
}

static int S_SDL_GetDMAPos (void)
{
	return shm->samplepos;
}

static void S_SDL_Shutdown (void)
{
	if (shm)
	{
		Con_Printf ("Shutting down SDL sound\n");
		if (audio_stream)
		{
			SDL_DestroyAudioStream(audio_stream);
			audio_stream = NULL;
		}
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
#if !USE_HUNK_ALLOC
		if (shm->buffer)
			free (shm->buffer);
#endif
		shm->buffer = NULL;
		shm = NULL;
	}
}

static void S_SDL_LockBuffer (void)
{
	if (audio_stream)
		SDL_LockAudioStream (audio_stream);
}

static void S_SDL_Submit (void)
{
	if (audio_stream)
		SDL_UnlockAudioStream(audio_stream);
}

static void S_SDL_BlockSound (void)
{
	if (audio_stream)
		SDL_PauseAudioStreamDevice(audio_stream);
}

static void S_SDL_UnblockSound (void)
{
	if (audio_stream)
		SDL_ResumeAudioStreamDevice(audio_stream);
}

snd_driver_t snddrv_sdl =
{
	S_SDL_Init,
	S_SDL_Shutdown,
	S_SDL_GetDMAPos,
	S_SDL_LockBuffer,
	S_SDL_Submit,
	S_SDL_BlockSound,
	S_SDL_UnblockSound,
	s_sdl_driver,
	SNDDRV_ID_SDL,
	false,
	NULL
};

#endif	/* HAVE_SDL_SOUND */
