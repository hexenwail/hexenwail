/* FLAC decoding support using dr_flac (public domain, single header).
 * Replaces the old libFLAC-based decoder.
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

#if defined(USE_CODEC_FLAC)
#include "snd_codec.h"
#include "snd_codeci.h"
#include "snd_flac.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"

/* Private data */
typedef struct
{
	drflac		*decoder;
	byte		*filedata;	/* entire FLAC file in memory */
	size_t		filesize;
} flac_priv_t;

static qboolean S_FLAC_CodecInitialize (void)
{
	return true;
}

static void S_FLAC_CodecShutdown (void)
{
}

static qboolean S_FLAC_CodecOpenStream (snd_stream_t *stream)
{
	flac_priv_t	*p;
	size_t		filesize;
	byte		*filedata;

	/* Read entire file into memory */
	filesize = stream->fh.length - FS_ftell(&stream->fh);
	filedata = (byte *)malloc(filesize);
	if (!filedata)
	{
		Con_Printf("Insufficient memory for FLAC audio\n");
		return false;
	}

	if (FS_fread(filedata, 1, filesize, &stream->fh) != filesize)
	{
		free(filedata);
		Con_Printf("Failed to read FLAC file\n");
		return false;
	}

#if defined(CODECS_USE_ZONE)
	p = (flac_priv_t *)Z_Malloc(sizeof(flac_priv_t), Z_SECZONE);
#else
	p = (flac_priv_t *)calloc(1, sizeof(flac_priv_t));
	if (!p)
	{
		free(filedata);
		Con_Printf("Insufficient memory for FLAC audio\n");
		return false;
	}
#endif

	p->decoder = drflac_open_memory(filedata, filesize, NULL);
	if (!p->decoder)
	{
		free(filedata);
#if defined(CODECS_USE_ZONE)
		Z_Free(p);
#else
		free(p);
#endif
		Con_Printf("%s is not a valid flac file\n", stream->name);
		return false;
	}

	p->filedata = filedata;
	p->filesize = filesize;

	stream->info.rate = p->decoder->sampleRate;
	stream->info.channels = p->decoder->channels;
	stream->info.bits = 16;	/* always decode to 16-bit */
	stream->info.width = 2;
	stream->priv = p;

	if (stream->info.channels != 1 && stream->info.channels != 2)
	{
		Con_Printf("Unsupported number of channels %d in %s\n",
			   stream->info.channels, stream->name);
		drflac_close(p->decoder);
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

static int S_FLAC_CodecReadStream (snd_stream_t *stream, int bytes, void *buffer)
{
	flac_priv_t	*p = (flac_priv_t *)stream->priv;
	int		frames_to_read;
	drflac_uint64	frames_read;

	/* bytes requested -> frames (frame = one sample per channel, 16-bit) */
	frames_to_read = bytes / (stream->info.channels * stream->info.width);

	frames_read = drflac_read_pcm_frames_s16(p->decoder, frames_to_read, (drflac_int16 *)buffer);

	return (int)(frames_read * stream->info.channels * stream->info.width);
}

static void S_FLAC_CodecCloseStream (snd_stream_t *stream)
{
	flac_priv_t *p = (flac_priv_t *)stream->priv;

	drflac_close(p->decoder);
	free(p->filedata);
#if defined(CODECS_USE_ZONE)
	Z_Free(p);
#else
	free(p);
#endif
	S_CodecUtilClose(&stream);
}

static int S_FLAC_CodecRewindStream (snd_stream_t *stream)
{
	flac_priv_t *p = (flac_priv_t *)stream->priv;

	if (!drflac_seek_to_pcm_frame(p->decoder, 0))
		return -1;
	return 0;
}

snd_codec_t flac_codec =
{
	CODECTYPE_FLAC,
	true,	/* always available (built-in decoder) */
	"flac",
	S_FLAC_CodecInitialize,
	S_FLAC_CodecShutdown,
	S_FLAC_CodecOpenStream,
	S_FLAC_CodecReadStream,
	S_FLAC_CodecRewindStream,
	NULL, /* jump */
	S_FLAC_CodecCloseStream,
	NULL
};

#endif	/* USE_CODEC_FLAC */
