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

/* --- SF3 (compressed SoundFont) detection --------------------------------
 *
 * SF3 is FluidSynth's compressed SoundFont: an ordinary RIFF/sfbk container
 * whose sample data is an Ogg Vorbis bitstream rather than raw 16-bit PCM.
 * The container is structurally valid SF2, so a synth with no Vorbis decoder
 * does not fail cleanly on one -- it reads the bitstream bytes as PCM and
 * plays them, i.e. loud noise.
 *
 * The vendored libTiMidity is exactly such a synth.  Its SoundFont support is
 * Takashi Iwai's 1997 SBK/SF2 extension (libs/timidity/README.sf); sndfont.c
 * load_from_file() freads sample bytes straight into the sample buffer with no
 * decoder anywhere in the path, and its only sampletype test is `& 0x8000` for
 * ROM samples (sndfont.c:591), which the Vorbis flag slips past.  So the engine
 * has to refuse the file before handing it over.  uhexen2-d4e7.
 *
 * Two independent markers, either one conclusive:
 *
 *   - INFO/ifil wMajor >= 3.  Measured, not assumed: FluidR3_GM2-2.sf2 reads
 *     2.2 and VintageDreamsWaves-v2.sf3 reads 3.0.  Note this is the MAJOR
 *     field: an SF3 is version 3.0, not 2.3.
 *   - any pdta/shdr sfSampleType carrying bit 0x10, the per-sample Vorbis
 *     flag.  Catches a file that kept an SF2 version stamp.
 *
 * Sniffing the container rather than the extension is the point: snd_soundfont
 * is a free-form user cvar, so an SF3 can arrive named .sf2 just as easily.
 *
 * Every read is sequential and checked, and each chunk size is clamped to the
 * bytes its parent actually has left, so a truncated or hostile file ends the
 * walk instead of driving a wild seek, an underflow, or an allocation.
 */

#define SF_SAMPLETYPE_VORBIS	0x10	/* sfSampleType bit: Vorbis-compressed */
#define SF_SHDR_RECORD_SIZE	46	/* 20-byte name + 26 bytes of fields */
#define SF_MAX_CHUNK		0x7ffffff0u	/* keeps size+pad inside a long */

#define SF_LIST_INFO		1
#define SF_LIST_PDTA		2

static qboolean SF_ReadU16 (FILE *f, unsigned int *out)
{
	unsigned char b[2];
	if (fread(b, 1, sizeof(b), f) != sizeof(b))
		return false;
	*out = (unsigned int)b[0] | ((unsigned int)b[1] << 8);
	return true;
}

static qboolean SF_ReadU32 (FILE *f, unsigned int *out)
{
	unsigned char b[4];
	if (fread(b, 1, sizeof(b), f) != sizeof(b))
		return false;
	*out = (unsigned int)b[0] | ((unsigned int)b[1] << 8) |
	       ((unsigned int)b[2] << 16) | ((unsigned int)b[3] << 24);
	return true;
}

/* Scan a pdta/shdr chunk for a sample flagged Vorbis-compressed.  Leaves the
 * stream wherever it stopped -- callers treat the position as spent. */
static qboolean SF_ShdrHasVorbis (FILE *f, unsigned int size)
{
	unsigned int type;

	while (size >= SF_SHDR_RECORD_SIZE)
	{
		/* sfSampleType is the last field of the record */
		if (fseek(f, SF_SHDR_RECORD_SIZE - 2, SEEK_CUR) != 0)
			return false;
		if (!SF_ReadU16(f, &type))
			return false;
		if (type & SF_SAMPLETYPE_VORBIS)
			return true;
		size -= SF_SHDR_RECORD_SIZE;
	}
	return false;
}

/* Walk one LIST's subchunks for whichever marker that list carries.  Returns
 * false when the walk cannot go on -- a short read, or a chunk that left the
 * stream at an unknown offset. */
static qboolean SF_ScanList (FILE *f, int listtype, unsigned int size,
			     qboolean *compressed)
{
	char id[4];
	unsigned int csize, padded, major;

	while (size >= 8)
	{
		if (fread(id, 1, sizeof(id), f) != sizeof(id))
			return false;
		if (!SF_ReadU32(f, &csize))
			return false;
		size -= 8;
		if (csize > size)
			csize = size;		/* a size larger than the list */
		padded = csize + (csize & 1);
		if (padded > size)
			padded = size;		/* pad byte the list never had */

		if (listtype == SF_LIST_INFO && csize >= 4 && !memcmp(id, "ifil", 4))
		{
			if (!SF_ReadU16(f, &major))
				return false;
			if (major >= 3)
			{
				*compressed = true;
				return true;
			}
			if (fseek(f, (long)(padded - 2), SEEK_CUR) != 0)
				return false;
		}
		else if (listtype == SF_LIST_PDTA && !memcmp(id, "shdr", 4))
		{
			*compressed = SF_ShdrHasVorbis(f, csize);
			return false;		/* position is spent either way */
		}
		else if (fseek(f, (long)padded, SEEK_CUR) != 0)
			return false;

		size -= padded;
	}
	return true;
}

qboolean SF_IsCompressed (const char *path)
{
	FILE *f;
	char id[4], form[4];
	unsigned int size, padded, remaining;
	qboolean compressed = false;
	int listtype;

	f = fopen(path, "rb");
	if (!f)
		return false;	/* unreadable: existence is SF_FileExists's job */

	/* "RIFF" <size> "sfbk" */
	if (fread(id, 1, sizeof(id), f) != sizeof(id) || memcmp(id, "RIFF", 4) ||
	    !SF_ReadU32(f, &remaining) ||
	    fread(form, 1, sizeof(form), f) != sizeof(form) || memcmp(form, "sfbk", 4))
	{
		fclose(f);
		return false;	/* not a SoundFont at all -- not our verdict */
	}
	if (remaining < 4)
	{
		fclose(f);
		return false;
	}
	if (remaining > SF_MAX_CHUNK)
		remaining = SF_MAX_CHUNK;
	remaining -= 4;		/* the "sfbk" form id, already read */

	while (!compressed && remaining >= 8)
	{
		if (fread(id, 1, sizeof(id), f) != sizeof(id))
			break;
		if (!SF_ReadU32(f, &size))
			break;
		remaining -= 8;
		if (size > remaining)
			size = remaining;
		padded = size + (size & 1);
		if (padded > remaining)
			padded = remaining;

		/* Only LIST chunks carry what we are looking for. */
		listtype = 0;
		if (size >= 4 && !memcmp(id, "LIST", 4))
		{
			if (fread(form, 1, sizeof(form), f) != sizeof(form))
				break;
			if (!memcmp(form, "INFO", 4))
				listtype = SF_LIST_INFO;
			else if (!memcmp(form, "pdta", 4))
				listtype = SF_LIST_PDTA;

			if (listtype && !SF_ScanList(f, listtype, size - 4, &compressed))
				break;		/* verdict, if any, is in *compressed */
			if (listtype)
			{
				remaining -= padded;
				continue;
			}
			if (fseek(f, (long)(padded - 4), SEEK_CUR) != 0)
				break;
		}
		else if (fseek(f, (long)padded, SEEK_CUR) != 0)
			break;

		remaining -= padded;
	}

	fclose(f);
	return compressed;
}

/* One gate for every candidate the search considers: it has to exist, and on a
 * synth with no Vorbis decoder it must not be an SF3. */
static qboolean SF_Usable (const char *path, qboolean allow_compressed)
{
	if (!SF_FileExists(path))
		return false;
	if (!allow_compressed && SF_IsCompressed(path))
	{
		Con_DPrintf("soundfont: skipping compressed SF3 '%s'\n", path);
		return false;
	}
	return true;
}


/* allow_compressed: true for a synth that decodes SF3 (FluidSynth via
 * libsndfile), false for one that does not (libTiMidity).  See SF_IsCompressed
 * above for why a false here has to sniff files rather than trust extensions. */
const char *SF_FindSoundFont (qboolean allow_compressed)
{
	static char sf_path[MAX_OSPATH];
	const char *exedir;
	int i;

	/* 1. user-specified path */
	if (snd_soundfont.string[0])
	{
		if (!SF_FileExists(snd_soundfont.string))
			Con_Printf("snd_soundfont '%s' not found\n", snd_soundfont.string);
		else if (allow_compressed || !SF_IsCompressed(snd_soundfont.string))
			return snd_soundfont.string;
		else	/* explicit request, so say so out loud, then keep looking */
			Con_Printf("snd_soundfont '%s' is a compressed SF3, which this "
				   "build's MIDI synth cannot decode; ignoring it\n",
				   snd_soundfont.string);
	}

	/* 2a. next to the executable itself (portable installs: the bundled
	 * soundfont ships here, regardless of the working directory). */
	exedir = Sys_GetExeDir();
	if (exedir)
	{
		static const char *exe_names[] = {
			"%s/soundfont.sf2", "%s/soundfont.sf3", NULL
		};
		int n;
		for (n = 0; exe_names[n]; n++)
		{
			q_snprintf(sf_path, sizeof(sf_path), exe_names[n], exedir);
			if (SF_Usable(sf_path, allow_compressed))
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
			/* FS_GetBasedir(), not host_parms->basedir: -basedir moves
			 * the game data and leaves host_parms->basedir on the launch
			 * cwd, so the latter would miss a soundfont sitting beside
			 * the data the player actually asked for.  uhexen2-jk53. */
			q_snprintf(sf_path, sizeof(sf_path), base_names[n], FS_GetBasedir());
			if (SF_Usable(sf_path, allow_compressed))
				return sf_path;
		}
	}

#ifdef SOUNDFONT_PATH
	/* 3. compile-time path (Nix builds) */
	if (SF_Usable(SOUNDFONT_PATH, allow_compressed))
		return SOUNDFONT_PATH;
#endif

	/* 4. common system paths */
	for (i = 0; sf_paths[i]; i++)
	{
		if (SF_Usable(sf_paths[i], allow_compressed))
			return sf_paths[i];
	}

	return NULL;
}
