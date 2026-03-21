/* midi_fluid.c -- FluidSynth MIDI driver for Linux
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
#include "bgmusic.h"
#include "midi_drv.h"

#include <fluidsynth.h>
#include <sys/stat.h>

static fluid_settings_t	*fs_settings;
static fluid_synth_t	*fs_synth;
static fluid_audio_driver_t	*fs_adriver;
static fluid_player_t	*fs_player;
static int		fs_sfont_id = -1;

static cvar_t	snd_soundfont = {"snd_soundfont", "", CVAR_ARCHIVE};

/* Common SoundFont search paths on Linux */
static const char *sf_paths[] = {
	"/usr/share/soundfonts/default.sf2",
	"/usr/share/soundfonts/FluidR3_GM.sf2",
	"/usr/share/soundfonts/FluidR3_GS.sf2",
	"/usr/share/sounds/sf2/FluidR3_GM.sf2",
	"/usr/share/sounds/sf2/FluidR3_GS.sf2",
	"/usr/share/sounds/sf2/default-GM.sf2",
	"/usr/share/sounds/sf3/FluidR3_GM.sf3",
	"/usr/share/sounds/sf3/default-GM.sf3",
	NULL
};

/* Check if a file exists without FluidSynth's noisy error output */
static qboolean file_exists (const char *path)
{
	struct stat st;
	return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/* Try to find a soundfont, checking in order:
 * 1. snd_soundfont cvar (user override)
 * 2. data1/soundfont.sf2 (game directory)
 * 3. SOUNDFONT_PATH compile-time define (Nix builds)
 * 4. Common system paths
 */
static const char *find_soundfont (void)
{
	static char sf_path[MAX_OSPATH];
	int i;

	/* 1. user-specified path */
	if (snd_soundfont.string[0])
	{
		if (file_exists(snd_soundfont.string))
			return snd_soundfont.string;
		Con_Printf("FluidSynth: snd_soundfont '%s' not found\n", snd_soundfont.string);
	}

	/* 2. check game directory for bundled soundfont */
	q_snprintf(sf_path, sizeof(sf_path), "%s/data1/soundfont.sf2", host_parms->basedir);
	if (file_exists(sf_path))
		return sf_path;

#ifdef SOUNDFONT_PATH
	/* 3. compile-time path (Nix builds) */
	if (file_exists(SOUNDFONT_PATH))
		return SOUNDFONT_PATH;
#endif

	/* 4. common system paths */
	for (i = 0; sf_paths[i]; i++)
	{
		if (file_exists(sf_paths[i]))
			return sf_paths[i];
	}

	return NULL;
}

/* Suppress FluidSynth's log output during normal operation */
static void fs_log_suppress (int level, const char *message, void *data)
{
	(void)level;
	(void)message;
	(void)data;
}

static qboolean FMIDI_Init (void)
{
	const char *sf;

	Cvar_RegisterVariable(&snd_soundfont);

	/* suppress FluidSynth's built-in logging */
	fluid_set_log_function(FLUID_PANIC, fs_log_suppress, NULL);
	fluid_set_log_function(FLUID_ERR, fs_log_suppress, NULL);
	fluid_set_log_function(FLUID_WARN, fs_log_suppress, NULL);
	fluid_set_log_function(FLUID_INFO, fs_log_suppress, NULL);
	fluid_set_log_function(FLUID_DBG, fs_log_suppress, NULL);

	/* find soundfont before creating the synth */
	sf = find_soundfont();
	if (!sf)
	{
		Con_Printf("FluidSynth: no soundfont found, MIDI disabled\n");
		Con_Printf("  Set snd_soundfont or place soundfont.sf2 in game directory\n");
		return false;
	}

	fs_settings = new_fluid_settings();
	if (!fs_settings)
	{
		Con_DPrintf("FluidSynth: couldn't create settings\n");
		return false;
	}

	/* use PipeWire/PulseAudio driver */
	fluid_settings_setstr(fs_settings, "audio.driver", "pipewire");

	fs_synth = new_fluid_synth(fs_settings);
	if (!fs_synth)
	{
		Con_DPrintf("FluidSynth: couldn't create synth\n");
		delete_fluid_settings(fs_settings);
		fs_settings = NULL;
		return false;
	}

	/* load the soundfont we found */
	fs_sfont_id = fluid_synth_sfload(fs_synth, sf, 1);
	if (fs_sfont_id == FLUID_FAILED)
	{
		Con_Printf("FluidSynth: failed to load %s\n", sf);
		delete_fluid_synth(fs_synth);
		delete_fluid_settings(fs_settings);
		fs_synth = NULL;
		fs_settings = NULL;
		return false;
	}

	Con_Printf("FluidSynth: loaded %s\n", sf);

	/* create audio driver (starts audio thread) */
	fs_adriver = new_fluid_audio_driver(fs_settings, fs_synth);
	if (!fs_adriver)
	{
		/* pipewire failed, try pulseaudio */
		fluid_settings_setstr(fs_settings, "audio.driver", "pulseaudio");
		fs_adriver = new_fluid_audio_driver(fs_settings, fs_synth);
	}
	if (!fs_adriver)
	{
		/* pulseaudio failed, try alsa */
		fluid_settings_setstr(fs_settings, "audio.driver", "alsa");
		fs_adriver = new_fluid_audio_driver(fs_settings, fs_synth);
	}
	if (!fs_adriver)
	{
		Con_Printf("FluidSynth: couldn't create audio driver\n");
		delete_fluid_synth(fs_synth);
		delete_fluid_settings(fs_settings);
		fs_synth = NULL;
		fs_settings = NULL;
		return false;
	}

	Con_Printf("FluidSynth MIDI driver initialized\n");
	return true;
}

static void FMIDI_Shutdown (void)
{
	if (fs_player)
	{
		delete_fluid_player(fs_player);
		fs_player = NULL;
	}
	if (fs_adriver)
	{
		delete_fluid_audio_driver(fs_adriver);
		fs_adriver = NULL;
	}
	if (fs_synth)
	{
		delete_fluid_synth(fs_synth);
		fs_synth = NULL;
	}
	if (fs_settings)
	{
		delete_fluid_settings(fs_settings);
		fs_settings = NULL;
	}
	fs_sfont_id = -1;
}

static void *FMIDI_Open (const char *filename)
{
	FILE		*f;
	long		len;
	void		*buf;
	int		ret;

	if (!fs_synth)
		return NULL;

	/* stop any current playback */
	if (fs_player)
	{
		fluid_player_stop(fs_player);
		delete_fluid_player(fs_player);
		fs_player = NULL;
	}

	/* read the MIDI file through the engine's filesystem */
	ret = FS_OpenFile(filename, &f, NULL);
	if (!f || ret < 0)
		return NULL;

	len = ret;
	buf = malloc(len);
	if (!buf)
	{
		fclose(f);
		return NULL;
	}
	if ((long)fread(buf, 1, len, f) != len)
	{
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);

	fs_player = new_fluid_player(fs_synth);
	if (!fs_player)
	{
		free(buf);
		return NULL;
	}

	if (fluid_player_add_mem(fs_player, buf, (size_t)len) != FLUID_OK)
	{
		free(buf);
		delete_fluid_player(fs_player);
		fs_player = NULL;
		return NULL;
	}
	free(buf);

	fluid_player_set_loop(fs_player, -1);	/* loop forever */
	fluid_player_play(fs_player);

	return (void *)fs_player;
}

static void FMIDI_Advance (void **handle)
{
	/* FluidSynth runs its own audio thread, nothing to do here */
	(void)handle;
}

static void FMIDI_Rewind (void **handle)
{
	if (fs_player)
	{
		fluid_player_stop(fs_player);
		fluid_player_seek(fs_player, 0);
		fluid_player_play(fs_player);
	}
	(void)handle;
}

static void FMIDI_Close (void **handle)
{
	if (fs_player)
	{
		fluid_player_stop(fs_player);
		fluid_synth_all_sounds_off(fs_synth, -1);
		delete_fluid_player(fs_player);
		fs_player = NULL;
	}
	if (handle)
		*handle = NULL;
}

static void FMIDI_Pause (void **handle)
{
	/* FluidSynth has no pause — just stop the player */
	if (fs_player)
		fluid_player_stop(fs_player);
	(void)handle;
}

static void FMIDI_Resume (void **handle)
{
	if (fs_player)
		fluid_player_play(fs_player);
	(void)handle;
}

static void FMIDI_SetVol (void **handle, float value)
{
	if (fs_synth)
		fluid_synth_set_gain(fs_synth, value);
	(void)handle;
}

static midi_driver_t midi_fluidsynth =
{
	false,
	"FluidSynth",
	FMIDI_Init,
	FMIDI_Shutdown,
	FMIDI_Open,
	FMIDI_Advance,
	FMIDI_Rewind,
	FMIDI_Close,
	FMIDI_Pause,
	FMIDI_Resume,
	FMIDI_SetVol,
	NULL
};

qboolean MIDI_Init (void)
{
	if (FMIDI_Init())
	{
		midi_fluidsynth.available = true;
		BGM_RegisterMidiDRV(&midi_fluidsynth);
		return true;
	}
	return false;
}

void MIDI_Cleanup (void)
{
	FMIDI_Shutdown();
	midi_fluidsynth.available = false;
}
