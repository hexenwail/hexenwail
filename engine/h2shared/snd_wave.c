/* WAV decoding support using dr_wav (public domain, single header).
 *
 * Copyright (C) 2025  uHexen2 contributors
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
#include "snd_codec.h"
#include "snd_codeci.h"
#include "snd_wave.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"

/* Private data */
typedef struct
{
	drwav		decoder;
	byte		*filedata;	/* entire WAV file in memory */
	size_t		filesize;
} wav_priv_t;

static qboolean S_WAV_CodecInitialize (void)
{
	return true;
}

static void S_WAV_CodecShutdown (void)
{
}

static qboolean S_WAV_CodecOpenStream (snd_stream_t *stream)
{
	wav_priv_t	*p;
	size_t		filesize;
	byte		*filedata;

	/* Read entire file into memory */
	filesize = stream->fh.length - FS_ftell(&stream->fh);
	filedata = (byte *)malloc(filesize);
	if (!filedata)
	{
		Con_Printf("Insufficient memory for WAV audio\n");
		return false;
	}

	if (FS_fread(filedata, 1, filesize, &stream->fh) != filesize)
	{
		free(filedata);
		Con_Printf("Failed to read WAV file\n");
		return false;
	}

#if defined(CODECS_USE_ZONE)
	p = (wav_priv_t *)Z_Malloc(sizeof(wav_priv_t), Z_SECZONE);
#else
	p = (wav_priv_t *)calloc(1, sizeof(wav_priv_t));
	if (!p)
	{
		free(filedata);
		Con_Printf("Insufficient memory for WAV audio\n");
		return false;
	}
#endif

	if (!drwav_init_memory(&p->decoder, filedata, filesize, NULL))
	{
		free(filedata);
#if defined(CODECS_USE_ZONE)
		Z_Free(p);
#else
		free(p);
#endif
		Con_Printf("%s is not a valid wav file\n", stream->name);
		return false;
	}

	p->filedata = filedata;
	p->filesize = filesize;

	stream->info.rate = p->decoder.sampleRate;
	stream->info.channels = p->decoder.channels;
	stream->info.bits = 16;	/* always decode to 16-bit */
	stream->info.width = 2;
	stream->priv = p;

	if (stream->info.channels != 1 && stream->info.channels != 2)
	{
		Con_Printf("Unsupported number of channels %d in %s\n",
			   stream->info.channels, stream->name);
		drwav_uninit(&p->decoder);
		free(filedata);
#if defined(CODECS_USE_ZONE)
		Z_Free(p);
#else
		free(p);
#endif
		return false;
	}

	return true;
}

static int S_WAV_CodecReadStream (snd_stream_t *stream, int bytes, void *buffer)
{
	wav_priv_t	*p = (wav_priv_t *)stream->priv;
	int		frames_to_read;
	drwav_uint64	frames_read;

	frames_to_read = bytes / (stream->info.channels * stream->info.width);

	frames_read = drwav_read_pcm_frames_s16(&p->decoder, frames_to_read, (drwav_int16 *)buffer);

	return (int)(frames_read * stream->info.channels * stream->info.width);
}

static void S_WAV_CodecCloseStream (snd_stream_t *stream)
{
	wav_priv_t *p = (wav_priv_t *)stream->priv;

	drwav_uninit(&p->decoder);
	free(p->filedata);
#if defined(CODECS_USE_ZONE)
	Z_Free(p);
#else
	free(p);
#endif
	S_CodecUtilClose(&stream);
}

static int S_WAV_CodecRewindStream (snd_stream_t *stream)
{
	wav_priv_t *p = (wav_priv_t *)stream->priv;

	if (!drwav_seek_to_pcm_frame(&p->decoder, 0))
		return -1;
	return 0;
}

snd_codec_t wav_codec =
{
	CODECTYPE_WAV,
	true,	/* always available (built-in decoder) */
	"wav",
	S_WAV_CodecInitialize,
	S_WAV_CodecShutdown,
	S_WAV_CodecOpenStream,
	S_WAV_CodecReadStream,
	S_WAV_CodecRewindStream,
	NULL, /* jump */
	S_WAV_CodecCloseStream,
	NULL
};
