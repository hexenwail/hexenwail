/*
 * Background music handling for Hexen II: Hammer of Thyrion (uHexen2)
 * Handles streaming music as raw sound samples and runs the midi driver
 *
 * Copyright (C) 1999-2005 Id Software, Inc.
 * Copyright (C) 2010-2018 O.Sezer <sezero@users.sourceforge.net>
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
 *
 */

#include "quakedef.h"
#include "snd_codec.h"
#include "bgmusic.h"
#include "cdaudio.h"
#include "midi_drv.h"

#define MIDI_DIRNAME	"midi"
#define MUSIC_DIRNAME	"music"

qboolean	bgmloop;
cvar_t		bgm_extmusic = {"bgm_extmusic", "1", CVAR_ARCHIVE};

static qboolean	no_extmusic= false;
static float	old_volume = -1.0f;

static midi_driver_t *midi_drivers = NULL;

typedef enum _bgm_player
{
	BGM_NONE = -1,
	BGM_MIDIDRV = 1,
	BGM_STREAMER
} bgm_player_t;

typedef struct music_handler_s
{
	unsigned int	type;	/* 1U << n (see snd_codec.h)	*/
	bgm_player_t	player;	/* Enumerated bgm player type	*/
	int	is_available;	/* -1 means not present		*/
	const char	*ext;	/* Expected file extension	*/
	const char	*dir;	/* Where to look for music file */
	struct music_handler_s	*next;
} music_handler_t;

static music_handler_t wanted_handlers[] =
{
	{ CODECTYPE_VORBIS,BGM_STREAMER,-1,  "ogg", MUSIC_DIRNAME, NULL },
	{ CODECTYPE_OPUS, BGM_STREAMER, -1, "opus", MUSIC_DIRNAME, NULL },
	{ CODECTYPE_MP3,  BGM_STREAMER, -1,  "mp3", MUSIC_DIRNAME, NULL },
	{ CODECTYPE_FLAC, BGM_STREAMER, -1, "flac", MUSIC_DIRNAME, NULL },
	{ CODECTYPE_WAV,  BGM_STREAMER, -1,  "wav", MUSIC_DIRNAME, NULL },
	{ CODECTYPE_MOD,  BGM_STREAMER, -1,  "it",  MUSIC_DIRNAME, NULL },
	{ CODECTYPE_MOD,  BGM_STREAMER, -1,  "s3m", MUSIC_DIRNAME, NULL },
	{ CODECTYPE_MOD,  BGM_STREAMER, -1,  "xm",  MUSIC_DIRNAME, NULL },
	{ CODECTYPE_MOD,  BGM_STREAMER, -1,  "mod", MUSIC_DIRNAME, NULL },
	{ CODECTYPE_UMX,  BGM_STREAMER, -1,  "umx", MUSIC_DIRNAME, NULL },
/* midi must be last before NULL terminator. */
#define MIDIDRIVER_MID (1U << 31) /* special, comes before CODECTYPE_MID */
	{ MIDIDRIVER_MID, BGM_MIDIDRV,  -1,  "mid", MIDI_DIRNAME,  NULL },
	{ CODECTYPE_MID,  BGM_STREAMER, -1,  "mid", MIDI_DIRNAME,  NULL },
	{ CODECTYPE_NONE, BGM_NONE,     -1,   NULL,         NULL,  NULL }
};

static music_handler_t *music_handlers = NULL;

#define ANY_CODECTYPE	0xFFFFFFFF
#define MIDI_TYPES	(CODECTYPE_MID | MIDIDRIVER_MID)
#define MIDITYPE(x)	(((x) & MIDI_TYPES) != 0)
#define CDRIP_TYPES	(CODECTYPE_VORBIS | CODECTYPE_MP3 | CODECTYPE_FLAC | CODECTYPE_WAV | CODECTYPE_OPUS)
#define CDRIPTYPE(x)	(((x) & CDRIP_TYPES) != 0)

typedef struct midi_handle_s
{
	void	*handle;
	midi_driver_t	*driver;
} midi_handle_t;

/* only one of bgmstream and mid_handle is supposed to be
 * active, not both. */
static midi_handle_t midi_handle;
static snd_stream_t *bgmstream = NULL;

/* bgm_remap: maps a numeric CD track to a named music file.  Lets mappers
 * escape the 8-bit track ceiling and lets users override individual tracks
 * via the console.  Empty string = no remap, fall through to track%02d.<ext>. */
#define BGM_NUM_REMAPS 256
static char bgm_remap_names[BGM_NUM_REMAPS][32];

static void BGM_Play_f (void)
{
	if (Cmd_Argc() == 2) {
		BGM_Play (Cmd_Argv(1));
	}
	else {
		Con_Printf ("music <musicfile>\n");
	}
}

static void BGM_Pause_f (void)
{
	BGM_Pause ();
}

static void BGM_Resume_f (void)
{
	BGM_Resume ();
}

static void BGM_Loop_f (void)
{
	if (Cmd_Argc() == 2) {
		if (q_strcasecmp(Cmd_Argv(1),  "0") == 0 ||
		    q_strcasecmp(Cmd_Argv(1),"off") == 0)
			bgmloop = false;
		else if (q_strcasecmp(Cmd_Argv(1), "1") == 0 ||
			 q_strcasecmp(Cmd_Argv(1),"on") == 0)
			bgmloop = true;
		else if (q_strcasecmp(Cmd_Argv(1),"toggle") == 0)
			bgmloop = !bgmloop;

		if (bgmstream) bgmstream->loop = bgmloop;
	}

	if (developer.value >= 2)
	{
		if (bgmloop)
			Con_Printf("Music will be looped\n");
		else
			Con_Printf("Music will not be looped\n");
	}
}

static void BGM_Stop_f (void)
{
	BGM_Stop();
}

static void BGM_Jump_f (void)
{
	if (Cmd_Argc() != 2) {
		Con_Printf ("music_jump <ordernum>\n");
	}
	else if (bgmstream) {
		S_CodecJumpToOrder(bgmstream, atoi(Cmd_Argv(1)));
	}
}

static void BGM_Remap_f (void)
{
	int track, argc = Cmd_Argc();

	if (argc < 2)
	{
		Con_Printf("usage: bgm_remap <track> <name|->\n");
		Con_Printf("       bgm_remap reset    (clears all remaps)\n");
		Con_Printf("       bgm_remap list     (show active remaps)\n");
		return;
	}
	if (argc == 2)
	{
		const char *arg = Cmd_Argv(1);
		if (!q_strcasecmp(arg, "reset"))
		{
			memset(bgm_remap_names, 0, sizeof(bgm_remap_names));
			Con_Printf("bgm_remap: cleared\n");
		}
		else if (!q_strcasecmp(arg, "list"))
		{
			int i, count = 0;
			for (i = 0; i < BGM_NUM_REMAPS; i++)
			{
				if (bgm_remap_names[i][0])
				{
					Con_Printf("  %3d -> %s\n", i, bgm_remap_names[i]);
					count++;
				}
			}
			if (count == 0)
				Con_Printf("(no remaps)\n");
		}
		else
		{
			Con_Printf("bgm_remap: unknown subcommand '%s'\n", arg);
		}
		return;
	}
	track = atoi(Cmd_Argv(1));
	if (track < 0 || track >= BGM_NUM_REMAPS)
	{
		Con_Printf("bgm_remap: track must be 0-%d\n", BGM_NUM_REMAPS - 1);
		return;
	}
	if (!strcmp(Cmd_Argv(2), "-"))
	{
		bgm_remap_names[track][0] = '\0';
		Con_Printf("bgm_remap: %d cleared\n", track);
	}
	else
	{
		q_strlcpy(bgm_remap_names[track], Cmd_Argv(2), sizeof(bgm_remap_names[0]));
		Con_Printf("bgm_remap: %d -> %s\n", track, bgm_remap_names[track]);
	}
}

void BGM_RegisterMidiDRV (void *drv)
{
	midi_driver_t *driver = (midi_driver_t *) drv;
	driver->next = midi_drivers;
	midi_drivers = driver;
}

qboolean BGM_Init (void)
{
	music_handler_t *handlers = NULL;
	int i;

	memset (&midi_handle, 0, sizeof(midi_handle_t));

	Cvar_RegisterVariable(&bgm_extmusic);
	Cmd_AddCommand("music", BGM_Play_f);
	Cmd_AddCommand("music_pause", BGM_Pause_f);
	Cmd_AddCommand("music_resume", BGM_Resume_f);
	Cmd_AddCommand("music_loop", BGM_Loop_f);
	Cmd_AddCommand("music_stop", BGM_Stop_f);
	Cmd_AddCommand("music_jump", BGM_Jump_f);
	Cmd_AddCommand("bgm_remap", BGM_Remap_f);

	if (COM_CheckParm("-noextmusic") != 0)
		no_extmusic = true;

	bgmloop = true;

	for (i = 0; wanted_handlers[i].type != CODECTYPE_NONE; i++)
	{
		switch (wanted_handlers[i].player)
		{
		case BGM_MIDIDRV:
			wanted_handlers[i].is_available =
				(midi_drivers == NULL) ? -1 : 1;
			break;
		case BGM_STREAMER:
			wanted_handlers[i].is_available =
				S_CodecIsAvailable(wanted_handlers[i].type);
			break;
		case BGM_NONE:
		default:
			break;
		}
		if (wanted_handlers[i].is_available != -1)
		{
			if (handlers)
			{
				handlers->next = &wanted_handlers[i];
				handlers = handlers->next;
			}
			else
			{
				music_handlers = &wanted_handlers[i];
				handlers = music_handlers;
			}
		}
	}

	return true;
}

void BGM_Shutdown (void)
{
	BGM_Stop();
/* sever our connections to
 * midi_drv and snd_codec */
	music_handlers = NULL;
	midi_drivers = NULL;
}

static int BGM_Play_mididrv (const char *filename)
{
	midi_driver_t *drv = midi_drivers;
	while (drv)
	{
		if (drv->available)
		{
			midi_handle.handle = drv->mididrv_open (filename);
			if (midi_handle.handle != NULL)
			{
				midi_handle.driver = drv;
				if (bgmvolume.value == 0.0f) /* don't bother advancing */
					drv->mididrv_pause(&midi_handle.handle);
				return 0;
			}
		}
		drv = drv->next;
	}
	return -1;
}

static void BGM_Play_noext (const char *filename, unsigned int allowed_types)
{
	char tmp[MAX_QPATH];
	music_handler_t *handler;

	handler = music_handlers;
	while (handler)
	{
		if (! (handler->type & allowed_types))
		{
			handler = handler->next;
			continue;
		}
		if (!handler->is_available)
		{
			handler = handler->next;
			continue;
		}
		q_snprintf(tmp, sizeof(tmp), "%s/%s.%s",
			   handler->dir, filename, handler->ext);
		switch (handler->player)
		{
		case BGM_MIDIDRV:
			if (BGM_Play_mididrv(tmp) == 0)
				return;		/* success */
			/* BGM_MIDIDRV is followed by CODECTYPE_MID streamer.
			 * Even if the midi driver failed, we may still have
			 * a chance with the streamer if it's available... */
			if (! (handler->next && handler->next->is_available))
				break;
			handler = handler->next;
		case BGM_STREAMER:
			bgmstream = S_CodecOpenStreamType(tmp, handler->type, bgmloop);
			if (bgmstream)
				return;		/* success */
			break;
		case BGM_NONE:
		default:
			break;
		}
		handler = handler->next;
	}

	Con_Printf("Couldn't handle music file %s\n", filename);
}

void BGM_Play (const char *filename)
{
	char tmp[MAX_QPATH];
	const char *ext;
	music_handler_t *handler;

	BGM_Stop();

	if (music_handlers == NULL)
		return;

	if (!filename || !*filename)
	{
		Con_DPrintf("null music file name\n");
		return;
	}

	ext = COM_FileGetExtension(filename);
	if (! *ext)	/* try all things */
	{
		BGM_Play_noext(filename, ANY_CODECTYPE);
		return;
	}

	/* use the filename as is */
	handler = music_handlers;
	while (handler)
	{
		if (handler->is_available &&
		    !q_strcasecmp(ext, handler->ext))
			break;
		handler = handler->next;
	}
	if (!handler)
	{
		Con_Printf("Unhandled extension for %s\n", filename);
		return;
	}
	q_snprintf(tmp, sizeof(tmp), "%s/%s", handler->dir, filename);
	switch (handler->player)
	{
	case BGM_MIDIDRV:
		if (BGM_Play_mididrv(tmp) == 0)
			return;		/* success */
		/* BGM_MIDIDRV is followed by CODECTYPE_MID streamer.
		 * Even if the midi driver failed, we may still have
		 * a chance with the streamer if it's available... */
		if (! (handler->next && handler->next->is_available))
			break;
		handler = handler->next;
	case BGM_STREAMER:
		bgmstream = S_CodecOpenStreamType(tmp, handler->type, bgmloop);
		if (bgmstream)
			return;		/* success */
		break;
	case BGM_NONE:
	default:
		break;
	}

	Con_Printf("Couldn't handle music file %s\n", filename);
}

void BGM_PlayMIDIorMusic (const char *filename)
{
/* instead of searching by the order of music_handlers, do so by
 * the order of searchpath priority: the file from the searchpath
 * with the highest path_id is most likely from our own gamedir
 * itself.  this way, if a mod has egyp1 as a mp3 or a midi, which
 * is below *.ogg in the music_handler order, the mp3 or midi will
 * still have priority over egyp1.ogg from, say, data1.
 */
	char tmp[MAX_QPATH];
	const char *ext, *dir;
	unsigned int path_id, prev_id, type;
	qboolean try_midi_stream;
	music_handler_t *handler;

	if (music_handlers == NULL)
		return;

	BGM_Stop();

	if (!filename || !*filename)
	{
		Con_DPrintf("null music file name\n");
		return;
	}

	ext = COM_FileGetExtension(filename);
	if (*ext != '\0')
	{
		BGM_Play(filename);
		return;
	}

	{
	char subdirs[8][64];
	char chosen_sub[64];
	int num_subdirs, s;
	qboolean midi_won;

	num_subdirs = FS_ListSearchSubdirs(MUSIC_DIRNAME, subdirs, 8);
	chosen_sub[0] = '\0';

	prev_id = 0;
	type = 0;
	dir = ext = NULL;
	handler = music_handlers;
	try_midi_stream = false;
	midi_won = false;
	while (handler)
	{
		if (! handler->is_available)
			goto _next;
		if (! MIDITYPE(handler->type) &&
		     (no_extmusic || !bgm_extmusic.value))
			goto _next;
		/* Probe flat handler->dir/<file>.<ext> first (s = -1), then
		 * each music subdir.  Highest path_id across all candidates wins. */
		for (s = -1; s < num_subdirs; s++)
		{
			const char *sub = (s < 0) ? "" : subdirs[s];
			if (*sub)
				q_snprintf(tmp, sizeof(tmp), "%s/%s/%s.%s",
					   handler->dir, sub, filename, handler->ext);
			else
				q_snprintf(tmp, sizeof(tmp), "%s/%s.%s",
					   handler->dir, filename, handler->ext);
			if (! FS_FileExists(tmp, &path_id))
				continue;
			if (path_id > prev_id)
			{
				prev_id = path_id;
				type = handler->type;
				ext = handler->ext;
				dir = handler->dir;
				q_strlcpy(chosen_sub, sub, sizeof(chosen_sub));
				if (handler->type == MIDIDRIVER_MID)
				{
					if (handler->next &&
					    handler->next->is_available)
						try_midi_stream = true;
					midi_won = true;
				}
			}
		}
		if (midi_won)
			break;
	_next:
		handler = handler->next;
	}
	if (ext == NULL)
		Con_Printf("Couldn't handle music file %s\n", filename);
	else
	{
		if (*chosen_sub)
			q_snprintf(tmp, sizeof(tmp), "%s/%s/%s.%s", dir, chosen_sub, filename, ext);
		else
			q_snprintf(tmp, sizeof(tmp), "%s/%s.%s", dir, filename, ext);
		switch (type)
		{
		case MIDIDRIVER_MID:
			if (BGM_Play_mididrv(tmp) == 0)
				return;		/* success */
			/* BGM_MIDIDRV is followed by CODECTYPE_MID streamer.
			 * Even if the midi driver failed, we may still have
			 * a chance with the streamer if it's available... */
			if (!try_midi_stream)
				break;
			type = CODECTYPE_MID;
		default:
			bgmstream = S_CodecOpenStreamType(tmp, type, bgmloop);
			if (bgmstream)
				return;		/* success */
		}
		Con_Printf("Couldn't handle music file %s\n", tmp);
	}
	}
}

void BGM_PlayCDtrack (byte track, qboolean looping)
{
/* instead of searching by the order of music_handlers, do so by
 * the order of searchpath priority: the file from the searchpath
 * with the highest path_id is most likely from our own gamedir
 * itself. This way, if a mod has track02 as a *.mp3 file, which
 * is below *.ogg in the music_handler order, the mp3 will still
 * have priority over track02.ogg from, say, data1.
 */
	char tmp[MAX_QPATH];
	const char *ext;
	unsigned int path_id, prev_id, type;
	music_handler_t *handler;
	char subdirs[8][64];
	char chosen_sub[64];
	int num_subdirs, s;

	BGM_Stop();
	Con_DPrintf("BGM: PlayCDtrack %d (loop=%d)\n", (int)track, (int)looping);

	/* If a filename remap is set, route through the named-music path. */
	if (track < BGM_NUM_REMAPS && bgm_remap_names[track][0])
	{
		const char *name = bgm_remap_names[track];
		Con_DPrintf("BGM: remap track %d -> %s\n", (int)track, name);
		bgmloop = looping;
		BGM_PlayMIDIorMusic(name);
		return;
	}

	if (CDAudio_Play(track, looping) == 0)
		return;			/* success */
	Con_DPrintf("BGM: CD audio failed, trying file fallback\n");

	if (music_handlers == NULL)
	{
		Con_Printf("BGM: no music handlers registered\n");
		return;
	}

	if (no_extmusic || !bgm_extmusic.value)
	{
		Con_Printf("BGM: external music disabled (bgm_extmusic=%g, no_extmusic=%d)\n",
			   bgm_extmusic.value, (int)no_extmusic);
		return;
	}

	num_subdirs = FS_ListSearchSubdirs(MUSIC_DIRNAME, subdirs, 8);
	chosen_sub[0] = '\0';

	prev_id = 0;
	type = 0;
	ext  = NULL;
	handler = music_handlers;
	while (handler)
	{
		if (! handler->is_available)
			goto _next;
		if (! CDRIPTYPE(handler->type))
			goto _next;
		/* Probe flat MUSIC_DIRNAME/trackNN.ext, then each subdir. */
		for (s = -1; s < num_subdirs; s++)
		{
			const char *sub = (s < 0) ? "" : subdirs[s];
			if (*sub)
				q_snprintf(tmp, sizeof(tmp), "%s/%s/track%02d.%s",
					   MUSIC_DIRNAME, sub, (int)track, handler->ext);
			else
				q_snprintf(tmp, sizeof(tmp), "%s/track%02d.%s",
					   MUSIC_DIRNAME, (int)track, handler->ext);
			if (! FS_FileExists(tmp, &path_id))
				continue;
			if (path_id > prev_id)
			{
				prev_id = path_id;
				type = handler->type;
				ext = handler->ext;
				q_strlcpy(chosen_sub, sub, sizeof(chosen_sub));
			}
		}
	_next:
		handler = handler->next;
	}
	if (ext == NULL)
		Con_Printf("BGM: no music file for track %d (looked in %s/ for track%02d.{ogg,mp3,flac,wav,...})\n",
			   (int)track, MUSIC_DIRNAME, (int)track);
	else
	{
		if (*chosen_sub)
			q_snprintf(tmp, sizeof(tmp), "%s/%s/track%02d.%s",
					MUSIC_DIRNAME, chosen_sub, (int)track, ext);
		else
			q_snprintf(tmp, sizeof(tmp), "%s/track%02d.%s",
					MUSIC_DIRNAME, (int)track, ext);
		Con_DPrintf("BGM: opening %s\n", tmp);
		bgmstream = S_CodecOpenStreamType(tmp, type, bgmloop);
		if (! bgmstream)
			Con_Printf("BGM: couldn't decode %s\n", tmp);
		else
			Con_DPrintf("BGM: playing %s\n", tmp);
	}
}

void BGM_Stop (void)
{
	if (midi_handle.handle)
	{
		midi_handle.driver->mididrv_close (& midi_handle.handle);
		midi_handle.handle = NULL;
		midi_handle.driver = NULL;
	}
	if (bgmstream)
	{
		bgmstream->status = STREAM_NONE;
		S_CodecCloseStream(bgmstream);
		bgmstream = NULL;
		s_rawend = 0;
	}
}

void BGM_Pause (void)
{
	if (midi_handle.handle)
		midi_handle.driver->mididrv_pause (& midi_handle.handle);
	if (bgmstream)
	{
		if (bgmstream->status == STREAM_PLAY)
			bgmstream->status = STREAM_PAUSE;
	}
}

void BGM_Resume (void)
{
	if (midi_handle.handle)
		midi_handle.driver->mididrv_resume (& midi_handle.handle);
	if (bgmstream)
	{
		if (bgmstream->status == STREAM_PAUSE)
			bgmstream->status = STREAM_PLAY;
	}
}

static void BGM_UpdateStream (void)
{
	qboolean did_rewind = false;
	int	res;	/* Number of bytes read. */
	int	bufferSamples;
	int	fileSamples;
	int	fileBytes;
	byte	raw[16384];

	if (bgmstream->status != STREAM_PLAY)
		return;

	/* don't bother playing anything if musicvolume is 0 */
	if (bgmvolume.value <= 0)
		return;

	/* see how many samples should be copied into the raw buffer */
	if (s_rawend < paintedtime)
		s_rawend = paintedtime;

	while (s_rawend < paintedtime + MAX_RAW_SAMPLES)
	{
		bufferSamples = MAX_RAW_SAMPLES - (s_rawend - paintedtime);

		/* decide how much data needs to be read from the file */
		fileSamples = bufferSamples * bgmstream->info.rate / shm->speed;
		if (!fileSamples)
			return;

		/* our max buffer size */
		fileBytes = fileSamples * (bgmstream->info.width * bgmstream->info.channels);
		if (fileBytes > (int) sizeof(raw))
		{
			fileBytes = (int) sizeof(raw);
			fileSamples = fileBytes /
					  (bgmstream->info.width * bgmstream->info.channels);
		}

		/* Read */
		res = S_CodecReadStream(bgmstream, fileBytes, raw);
		if (res < fileBytes)
		{
			fileBytes = res;
			fileSamples = res / (bgmstream->info.width * bgmstream->info.channels);
		}

		if (res > 0)	/* data: add to raw buffer */
		{
			S_RawSamples(fileSamples, bgmstream->info.rate,
							bgmstream->info.width,
							bgmstream->info.channels,
							raw, bgmvolume.value);
			did_rewind = false;
		}
		else if (res == 0)	/* EOF */
		{
			if (bgmloop)
			{
				if (did_rewind)
				{
					Con_Printf("Stream keeps returning EOF.\n");
					BGM_Stop();
					return;
				}

				res = S_CodecRewindStream(bgmstream);
				if (res != 0)
				{
					Con_Printf("Stream seek error (%i), stopping.\n", res);
					BGM_Stop();
					return;
				}
				did_rewind = true;
			}
			else
			{
				BGM_Stop();
				return;
			}
		}
		else	/* res < 0: some read error */
		{
			Con_Printf("Stream read error (%i), stopping.\n", res);
			BGM_Stop();
			return;
		}
	}
}

void BGM_Update (void)
{
	if (old_volume != bgmvolume.value)
	{
		if (bgmvolume.value < 0)
			Cvar_SetQuick (&bgmvolume, "0");
		else if (bgmvolume.value > 1)
			Cvar_SetQuick (&bgmvolume, "1");
		old_volume = bgmvolume.value;
		if (midi_handle.handle)
		{
			midi_handle.driver->mididrv_setvol (& midi_handle.handle, bgmvolume.value);
			if (bgmvolume.value == 0.0f) /* don't bother advancing */
				midi_handle.driver->mididrv_pause (& midi_handle.handle);
			else
				midi_handle.driver->mididrv_resume(& midi_handle.handle);
		}
	}
	if (midi_handle.handle)
		midi_handle.driver->mididrv_advance (& midi_handle.handle);
	if (bgmstream)
		BGM_UpdateStream ();
}

