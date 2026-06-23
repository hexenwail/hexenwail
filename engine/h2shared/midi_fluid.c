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
#include <unistd.h>	/* readlink */

static fluid_settings_t	*fs_settings;
static fluid_synth_t	*fs_synth;
static fluid_player_t	*fs_player;
static int		fs_sfont_id = -1;
static int		fs_rate = 0;		/* synth render rate; matches shm->speed so S_RawSamples never resamples */
static qboolean		fs_paused = false;	/* when set, FMIDI_Advance renders nothing (silence) */

/* Synth output gain is held constant; bgmvolume is applied downstream in
 * S_RawSamples (see FMIDI_Advance), so applying it here too would double it. */
#define FS_SYNTH_GAIN		1.0f
/* Per-write render block (stereo frames).  Kept small so the scratch buffer
 * stays a modest static allocation; FMIDI_Advance loops to fill the ring. */
#define FS_RENDER_FRAMES	1024
static short		fs_buf[FS_RENDER_FRAMES * 2];	/* interleaved stereo s16 */

static cvar_t	snd_soundfont = {"snd_soundfont", "", CVAR_ARCHIVE};

/* Common SoundFont search paths.  Includes the Flatpak app prefix
 * (/app/share/...) so a soundfont bundled with the Flatpak is found even
 * though /usr there is the runtime, not the app. */
static const char *sf_paths[] = {
	/* Flatpak-bundled soundfont (see flatpak/io.github.hexenwail.hexenwail.yml) */
	"/app/share/soundfonts/default.sf2",
	"/app/share/soundfonts/FluidR3_GM.sf2",
	"/app/share/sounds/sf2/FluidR3_GM.sf2",
	/* System locations (Debian/Ubuntu/Mint, Fedora, Arch, etc.) */
	"/usr/share/soundfonts/default.sf2",
	"/usr/share/soundfonts/FluidR3_GM.sf2",
	"/usr/share/soundfonts/FluidR3_GS.sf2",
	"/usr/share/sounds/sf2/FluidR3_GM.sf2",
	"/usr/share/sounds/sf2/FluidR3_GS.sf2",
	"/usr/share/sounds/sf2/default-GM.sf2",
	"/usr/share/sounds/sf2/TimGM6mb.sf2",
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

/* Resolve the directory the running executable lives in (via /proc/self/exe).
 * basedir is getcwd(), which is not necessarily the install dir, so a
 * soundfont bundled next to the binary won't be found through basedir if the
 * game was launched from elsewhere.  Returns NULL on failure. */
static const char *exe_dir (void)
{
	static char dir[MAX_OSPATH];
	ssize_t len;
	char *slash;

	len = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
	if (len <= 0 || len >= (ssize_t)sizeof(dir) - 1)
		return NULL;
	dir[len] = '\0';
	slash = strrchr(dir, '/');
	if (!slash)
		return NULL;
	*slash = '\0';
	return dir;
}

/* Try to find a soundfont, checking in order:
 * 1. snd_soundfont cvar (user override)
 * 2. soundfont.sf2/.sf3 next to the binary or in basedir / data1
 * 3. SOUNDFONT_PATH compile-time define (Nix builds)
 * 4. Common system paths
 */
static const char *find_soundfont (void)
{
	static char sf_path[MAX_OSPATH];
	const char *exedir;
	int i;

	/* 1. user-specified path */
	if (snd_soundfont.string[0])
	{
		if (file_exists(snd_soundfont.string))
			return snd_soundfont.string;
		Con_Printf("FluidSynth: snd_soundfont '%s' not found\n", snd_soundfont.string);
	}

	/* 2a. next to the executable itself (portable installs: the bundled
	 * soundfont ships here, regardless of the working directory). */
	exedir = exe_dir();
	if (exedir)
	{
		static const char *exe_names[] = {
			"%s/soundfont.sf2", "%s/soundfont.sf3", NULL
		};
		int n;
		for (n = 0; exe_names[n]; n++)
		{
			q_snprintf(sf_path, sizeof(sf_path), exe_names[n], exedir);
			if (file_exists(sf_path))
				return sf_path;
		}
	}

	/* 2b. check game directory for a bundled soundfont, in a few common
	 * spots and extensions: next to the binary and inside data1. */
	{
		static const char *base_names[] = {
			"%s/soundfont.sf2", "%s/soundfont.sf3",
			"%s/data1/soundfont.sf2", "%s/data1/soundfont.sf3",
			NULL
		};
		int n;
		for (n = 0; base_names[n]; n++)
		{
			q_snprintf(sf_path, sizeof(sf_path), base_names[n], host_parms->basedir);
			if (file_exists(sf_path))
				return sf_path;
		}
	}

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

	/* Render MIDI ourselves and feed the engine's SDL mixer (see
	 * FMIDI_Advance) instead of spinning up FluidSynth's own audio thread.
	 * That thread can fail to reach the device inside the Flatpak sandbox,
	 * killing MIDI even though SFX/OGG (which use the SDL mixer) play fine.
	 * shm is set by S_Init, which runs before MIDI_Init; fall back to a sane
	 * rate if sound failed to start (in which case FMIDI_Advance also bails). */
	fs_rate = (shm && shm->speed) ? shm->speed : 44100;
	fluid_settings_setnum(fs_settings, "synth.sample-rate", (double)fs_rate);
	fluid_settings_setint(fs_settings, "synth.audio-channels", 1);	/* one stereo pair */
	/* Advance the song by samples rendered (manual pull), not by a wall-clock
	 * timer thread; this keeps playback tempo tied to our render calls and
	 * freezes cleanly while paused (we simply stop calling write). */
	fluid_settings_setstr(fs_settings, "player.timing-source", "sample");

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

	/* Fixed gain; bgmvolume is applied in S_RawSamples downstream. */
	fluid_synth_set_gain(fs_synth, FS_SYNTH_GAIN);

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
	fs_paused = false;
	fluid_player_play(fs_player);

	return (void *)fs_player;
}

static void FMIDI_Advance (void **handle)
{
	int	bufferSamples;
	int	frames;

	(void)handle;

	if (!fs_synth || !fs_player || fs_paused || !shm)
		return;
	if (bgmvolume.value <= 0)	/* paused-equivalent: emit nothing */
		return;
	/* Loop is forever (-1), so PLAYING is the only state we render; bail
	 * otherwise so we never push trailing silence into the ring. */
	if (fluid_player_get_status(fs_player) != FLUID_PLAYER_PLAYING)
		return;

	/* Mirror BGM_UpdateStream: render only the free space in the shared raw
	 * ring this frame, in capped blocks, so it never overflows. */
	if (s_rawend < paintedtime)
		s_rawend = paintedtime;

	while (s_rawend < paintedtime + MAX_RAW_SAMPLES)
	{
		bufferSamples = MAX_RAW_SAMPLES - (s_rawend - paintedtime);
		frames = (bufferSamples > FS_RENDER_FRAMES) ? FS_RENDER_FRAMES : bufferSamples;

		fluid_synth_write_s16(fs_synth, frames, fs_buf, 0, 2, fs_buf, 1, 2);
		/* rate == shm->speed -> no resample; bgmvolume baked in here. */
		S_RawSamples(frames, fs_rate, 2, 2, (byte *)fs_buf, bgmvolume.value);
	}
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
	fs_paused = false;
	if (handle)
		*handle = NULL;
}

static void FMIDI_Pause (void **handle)
{
	/* With sample-timed playback the song only advances when FMIDI_Advance
	 * renders; gating on this flag freezes it and emits silence. */
	fs_paused = true;
	(void)handle;
}

static void FMIDI_Resume (void **handle)
{
	fs_paused = false;
	(void)handle;
}

static void FMIDI_SetVol (void **handle, float value)
{
	/* No-op: bgmvolume is applied in S_RawSamples (FMIDI_Advance); synth gain
	 * stays fixed so volume isn't applied twice. */
	(void)handle;
	(void)value;
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
