/* MP3 decoding support using dr_mp3 (public domain, single header).
 * Replaces the old libmad-based decoder.
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

#if defined(USE_CODEC_MP3)
#include "snd_codec.h"
#include "snd_codeci.h"
#include "snd_mp3.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

/* Private data */
typedef struct
{
	drmp3		decoder;
	byte		*filedata;	/* entire MP3 file in memory */
	size_t		filesize;
	drmp3_uint64	total_frames;
} mp3_priv_t;

static qboolean S_MP3_CodecInitialize (void)
{
	return true;
}

static void S_MP3_CodecShutdown (void)
{
}

static qboolean S_MP3_CodecOpenStream (snd_stream_t *stream)
{
	mp3_priv_t	*p;
	size_t		filesize;
	byte		*filedata;

	if (mp3_skiptags(stream) < 0)
	{
		Con_Printf("Corrupt mp3 file (bad tags.)\n");
		return false;
	}

	/* Read entire file into memory (dr_mp3 works on memory buffers) */
	filesize = stream->fh.length - FS_ftell(&stream->fh);
	filedata = (byte *)malloc(filesize);
	if (!filedata)
	{
		Con_Printf("Insufficient memory for MP3 audio\n");
		return false;
	}

	if (FS_fread(filedata, 1, filesize, &stream->fh) != filesize)
	{
		free(filedata);
		Con_Printf("Failed to read MP3 file\n");
		return false;
	}

#if defined(CODECS_USE_ZONE)
	p = (mp3_priv_t *)Z_Malloc(sizeof(mp3_priv_t), Z_SECZONE);
#else
	p = (mp3_priv_t *)calloc(1, sizeof(mp3_priv_t));
	if (!p)
	{
		free(filedata);
		Con_Printf("Insufficient memory for MP3 audio\n");
		return false;
	}
#endif

	if (!drmp3_init_memory(&p->decoder, filedata, filesize, NULL))
	{
		free(filedata);
#if defined(CODECS_USE_ZONE)
		Z_Free(p);
#else
		free(p);
#endif
		Con_Printf("%s is not a valid mp3 file\n", stream->name);
		return false;
	}

	p->filedata = filedata;
	p->filesize = filesize;

	stream->info.rate = p->decoder.sampleRate;
	stream->info.channels = p->decoder.channels;
	stream->info.bits = 16;
	stream->info.width = 2;
	stream->priv = p;

	if (stream->info.channels != 1 && stream->info.channels != 2)
	{
		Con_Printf("Unsupported number of channels %d in %s\n",
			   stream->info.channels, stream->name);
		drmp3_uninit(&p->decoder);
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

static int S_MP3_CodecReadStream (snd_stream_t *stream, int bytes, void *buffer)
{
	mp3_priv_t	*p = (mp3_priv_t *)stream->priv;
	int		frames_to_read;
	drmp3_uint64	frames_read;

	/* bytes requested -> frames (frame = one sample per channel, 16-bit) */
	frames_to_read = bytes / (stream->info.channels * stream->info.width);

	frames_read = drmp3_read_pcm_frames_s16(&p->decoder, frames_to_read, (drmp3_int16 *)buffer);

	return (int)(frames_read * stream->info.channels * stream->info.width);
}

static void S_MP3_CodecCloseStream (snd_stream_t *stream)
{
	mp3_priv_t *p = (mp3_priv_t *)stream->priv;

	drmp3_uninit(&p->decoder);
	free(p->filedata);
#if defined(CODECS_USE_ZONE)
	Z_Free(p);
#else
	free(p);
#endif
	S_CodecUtilClose(&stream);
}

static int S_MP3_CodecRewindStream (snd_stream_t *stream)
{
	mp3_priv_t *p = (mp3_priv_t *)stream->priv;

	if (!drmp3_seek_to_pcm_frame(&p->decoder, 0))
		return -1;
	return 0;
}

snd_codec_t mp3_codec =
{
	CODECTYPE_MP3,
	true,	/* always available (built-in decoder) */
	"mp3",
	S_MP3_CodecInitialize,
	S_MP3_CodecShutdown,
	S_MP3_CodecOpenStream,
	S_MP3_CodecReadStream,
	S_MP3_CodecRewindStream,
	NULL, /* jump */
	S_MP3_CodecCloseStream,
	NULL
};

#endif	/* USE_CODEC_MP3 */
