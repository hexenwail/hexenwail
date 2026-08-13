/* soundfont.c -- shared SoundFont discovery for the MIDI synths
 *
 * Factored out of midi_fluid.c: that translation unit is compiled only when
 * FluidSynth is present, but the libTiMidity codec that covers the builds
 * without FluidSynth needs the very same search.  One soundfont serves both
 * synths.
 *
 * Copyright (C) 2025-2026  uHexen2 contributors
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
#include "soundfont.h"

#include <sys/stat.h>
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <unistd.h>	/* readlink */
#endif

cvar_t	snd_soundfont = {"snd_soundfont", "", CVAR_ARCHIVE};

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

void SF_RegisterCvar (void)
{
	static qboolean registered = false;

	if (registered)
		return;
	registered = true;
	Cvar_RegisterVariable(&snd_soundfont);
}

/* Check if a file exists without FluidSynth's noisy error output */
qboolean SF_FileExists (const char *path)
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
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
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
#else
	return NULL;
#endif
}

const char *SF_FindSoundFont (void)
{
	static char sf_path[MAX_OSPATH];
	const char *exedir;
	int i;

	/* 1. user-specified path */
	if (snd_soundfont.string[0])
	{
		if (SF_FileExists(snd_soundfont.string))
			return snd_soundfont.string;
		Con_Printf("snd_soundfont '%s' not found\n", snd_soundfont.string);
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
			if (SF_FileExists(sf_path))
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
			if (SF_FileExists(sf_path))
				return sf_path;
		}
	}

#ifdef SOUNDFONT_PATH
	/* 3. compile-time path (Nix builds) */
	if (SF_FileExists(SOUNDFONT_PATH))
		return SOUNDFONT_PATH;
#endif

	/* 4. common system paths */
	for (i = 0; sf_paths[i]; i++)
	{
		if (SF_FileExists(sf_paths[i]))
			return sf_paths[i];
	}

	return NULL;
}
