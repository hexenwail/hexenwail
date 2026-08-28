/*
 * quakefs.c -- Hexen II filesystem
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
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
#include "pakfile.h"
#include "bgmusic.h"
#include <errno.h>
#ifdef PLATFORM_WINDOWS
#include <windows.h>	/* MultiByteToWideChar/MAX_PATH: only the client gets these free via glheader.h */
#include <io.h>
#else
#include <dirent.h>
#endif
#include "filenames.h"
#include "hashindex.h"
#include "miniz.h"

/* Open file with UTF-8 path support on Windows */
static FILE *FS_FOpen (const char *path, const char *mode)
{
#ifdef PLATFORM_WINDOWS
	wchar_t widepath[MAX_PATH];
	wchar_t widemode[16];
	int pathlen, modelen;

	pathlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, widepath, MAX_PATH);
	modelen = MultiByteToWideChar(CP_UTF8, 0, mode, -1, widemode, 16);

	if (pathlen == 0 || modelen == 0)
		return NULL;

	return _wfopen(widepath, widemode);
#else
	return fopen(path, mode);
#endif
}

typedef struct
{
	char	name[MAX_QPATH];
	int	filepos, filelen;
} pakfiles_t;

typedef struct pack_s
{
	char	filename[MAX_OSPATH];
	FILE	*handle;
	int	numfiles;
	pakfiles_t	*files;
	hashindex_t	hash;
} pack_t;

/* A .pk3/.zip searchpath entry.  Deliberately shaped like pack_t so the two
 * lookup paths in FS_OpenFile_Internal stay recognisably the same code.
 *
 * The split that matters is per entry, not per archive: a STORED entry (method
 * 0) is a contiguous uncompressed run inside the archive, so it is served
 * exactly like a .pak entry -- reopen the archive, fseek to the data, hand back
 * the FILE *.  Costs nothing and keeps O(1) streaming for the case that needs
 * it most, since already-compressed media (ogg/mp3/opus) is what people store
 * rather than deflate.  A DEFLATED entry has no byte offset to seek to and must
 * be inflated into memory. */
/* Cap on archives mounted from one gamedir.  Generous next to how many pk3s a
 * Hexen II install would plausibly carry, and it keeps the scan buffer off the
 * heap without risking a silent truncation nobody notices. */
#define MAX_PK3_PER_DIR		64

/* Entry-count ceiling for one archive.  MAX_FILES_IN_PACK is the .pak
 * equivalent; a pk3 has no format limit, so this is ours.  Refusing the whole
 * archive rather than truncating it keeps the failure loud -- a half-mounted
 * archive would fail lookups that the file listing says should work. */
#define MAX_FILES_IN_ZIP	65536

static int FS_CompareZipNames (const void *a, const void *b)
{
	return q_strcasecmp ((const char *) a, (const char *) b);
}

typedef struct
{
	char	name[MAX_QPATH];
	mz_uint	index;		/* miniz central-directory index */
	int	filelen;	/* uncompressed size */
	int	filepos;	/* data offset for STORED entries, -1 until resolved,
				 * -2 for entries that must be inflated */
} zipfiles_t;

typedef struct zippack_s
{
	char	filename[MAX_OSPATH];
	FILE	*handle;
	mz_zip_archive	archive;
	int	numfiles;
	zipfiles_t	*files;
	hashindex_t	hash;
} zippack_t;

typedef struct searchpath_s
{
	unsigned int	path_id;	/* identifier assigned to the game directory
					 *	Note that <install_dir>/game1 and
					 *	<userdir>/game1 have the same id. */
	char		filename[MAX_OSPATH];
	struct pack_s		*pack;	/* only one of filename / pack / zip is used */
	struct zippack_s	*zip;
	struct searchpath_s	*next;
} searchpath_t;

static searchpath_t	*fs_searchpaths;
static searchpath_t	*fs_base_searchpaths;	/* without gamedirs */
/* The base searchpath WITHOUT the mission pack: the state right after step 1
 * of FS_Init added data1.  fs_base_searchpaths cannot serve here because a
 * -game launch folds portals into the base (FS_Init step 2), so unwinding to
 * it leaves the mission pack on the path forever -- base-game maps then read
 * portals content, gamecode included.  Host_Game_f rebuilds from this mark
 * instead and re-adds portals only when the destination wants it.
 * uhexen2-5vb6. */
static searchpath_t	*fs_base_nomp_searchpaths;

/* State left behind by the most recent FS_OpenFile_Internal(), in the same
 * "read it immediately after the call" idiom as fs_filesize, file_from_pak and
 * fs_lastfile_source.
 *
 * Set only when the lookup landed on a DEFLATED archive entry, which is the one
 * case that cannot be handed back as a FILE * -- there is no byte offset in the
 * archive that holds the decompressed bytes.  FS_OpenFile then returns the
 * uncompressed length with *file == NULL, and the caller inflates via
 * FS_ReadZipIntoBuffer() instead of reading.  NULL for every other outcome,
 * including a STORED archive entry, which is served like a pak member. */
static zippack_t	*fs_lastzip;
static const zipfiles_t	*fs_lastzipentry;

static const char	*fs_basedir;
static char	fs_gamedir[MAX_OSPATH];
static char	fs_userdir[MAX_OSPATH];
char	fs_gamedir_nopath[MAX_QPATH];

/* path_id the "portals" gamedir was assigned, or 0 if it was never added.
 * Unlike data1's, it is not a constant: it depends on what else is already on
 * the path, and Host_Game_f can add portals again at runtime with a different
 * one.  See FS_GetPortalsPathID(). */
static unsigned int	fs_portals_path_id;

unsigned int	gameflags;

cvar_t	oem = {"oem", "0", CVAR_ROM};
cvar_t	registered = {"registered", "0", CVAR_ROM};

typedef struct
{
	int	numfiles;
	unsigned int	crc;
	long	size;
	const char	*dirname;
} pakdata_t;

#define	MAX_PAKDATA	5 /* pak0...4 */
static pakdata_t pakdata[MAX_PAKDATA] =
{
	{ 696,	34289,	22704056, "data1"	},	/* pak0.pak, registered, up-to-date, v1.11
							 *	MD5: c9675191e75dd25a3b9ed81ee7e05eff	*/
	{ 523,	2995 ,	75601170, "data1"	},	/* pak1.pak, registered, up-to-date, v1.11
							 *	MD5: c2ac5b0640773eed9ebe1cda2eca2ad0	*/
	{ 183,	4807 ,	17742721, "data1"	},	/* pak2.pak, oem (Matrox m3D bundle) v1.10
							 *	MD5: 99e0054861e94f66fc8e0e29416859c9	*/
	{ 245,	1478 ,	49089114, "portals"	},	/* pak3.pak, Portal of Praevus expansion pack
							 *	MD5: 77ae298dd0dcd16ab12f4a68067ff2c3	*/
	{ 102,	41062,	10780245, "hw"		}	/* pak4.pak, hexenworld, versions 0.14 - 0.15
							 *	MD5: 88109ee385d9723ac5f1015e034a44dd	*/
};

static pakdata_t demo_pakdata[] =
{
	{ 797,	22780,	27750257, "data1"	}	/* pak0.pak, demo v1.11 from Nov. 1997
							 *	MD5: 8e598d82bf53436ed7a0e133aa4b9f09	*/
};

static pakdata_t oem0_pakdata[] =	/* Continent of Blackmarsh */
{
	{ 697,	9787 ,	22720659, "data1"	}	/* pak0.pak, oem (Matrox m3D bundle) v1.10
							 *	MD5: 8c9c6118117baca7b9349d477403fcc0	*/
};

static pakdata_t old_pakdata[] =
{
	{ 697,	53062,	21714275, "data1"	},	/* pak0.pak, original cdrom (1.03) version
							 *	MD5: b53c9391d16134cb3baddc1085f18683	*/
	{ 525,	47762,	76958474, "data1"	},	/* pak1.pak, original cdrom (1.03) version
							 *	MD5: 9a2010aafb9c0fe71c37d01292030270	*/
	{ 701,	20870,	23537707, "data1"	},	/* pak0.pak, original demo v0.42 from Aug. 1997
							 *	(h2.exe -> console -> version says 1.07!)
							 *	MD5: 208643a09193dafbca4b851762479438	*/
/* !!! FIXME:  I don't have the original v1.08 of Continent of Blackmarsh. I only know the file sizes.	*/
	{ -1,	0,	22719295, "data1"	},	/* pak0.pak, original oem (Matrox m3D) v1.08
							 *	MD5: ????????????????????????????????	*/
	{ -1,	0,	17739969, "data1"	},	/* pak2.pak, original oem (Matrox m3D) v1.08
							 *	MD5: ????????????????????????????????	*/
	{  98,	25864,	10678369, "hw"	},		/* pak4.pak, Hexen2World v0.11 (ugh..)
							 *	MD5: c311a30ac8ee1f112019723b4fe42268	*/
	{  40,	48258,	 3357888, "hw"	},		/* pak4.pak, Hexen2World v0.09 (ugh ugh ugh!)
							 *	MD5: 7708da4323f668cf9c71f99315704baa	*/
};

/* this graphic needs to be in the pak file to use registered features */
static const unsigned short pop[] =
{
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x6600, 0x0000, 0x0000, 0x0000, 0x6600, 0x0000,
	0x0000, 0x0066, 0x0000, 0x0000, 0x0000, 0x0000, 0x0067, 0x0000,
	0x0000, 0x6665, 0x0000, 0x0000, 0x0000, 0x0000, 0x0065, 0x6600,
	0x0063, 0x6561, 0x0000, 0x0000, 0x0000, 0x0000, 0x0061, 0x6563,
	0x0064, 0x6561, 0x0000, 0x0000, 0x0000, 0x0000, 0x0061, 0x6564,
	0x0064, 0x6564, 0x0000, 0x6469, 0x6969, 0x6400, 0x0064, 0x6564,
	0x0063, 0x6568, 0x6200, 0x0064, 0x6864, 0x0000, 0x6268, 0x6563,
	0x0000, 0x6567, 0x6963, 0x0064, 0x6764, 0x0063, 0x6967, 0x6500,
	0x0000, 0x6266, 0x6769, 0x6a68, 0x6768, 0x6a69, 0x6766, 0x6200,
	0x0000, 0x0062, 0x6566, 0x6666, 0x6666, 0x6666, 0x6562, 0x0000,
	0x0000, 0x0000, 0x0062, 0x6364, 0x6664, 0x6362, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0062, 0x6662, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0061, 0x6661, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x6500, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x6400, 0x0000, 0x0000, 0x0000
};

static char *FSERR_MakePath_BUF (const char *caller, int linenum, int base,
				 char *buf, size_t siz, const char *path);
static char *FSERR_MakePath_VABUF (const char *caller, int linenum, int base,
				char *buf, size_t siz, const char *format, ...)
							FUNC_PRINTF(6,7);
static char *do_MakePath_VA (int base, int *error, char *buf, size_t siz,
					const char *format, va_list args)
							FUNC_PRINTF(5,0);

/*
All of Quake's data access is through a hierchal file system, but the contents
of the file system can be transparently merged from several sources.

The "base directory" is the path to the directory holding the exe and all game
directories.  The sys_* files pass this to host_init in quakeparms_t->basedir.
This can be overridden with the "-basedir" command line parm to allow code
debugging in a different directory.  The base directory is only used during
filesystem initialization.

The "game directory" is the first tree on the search path and directory that
all generated files (savegames, screenshots, demos, config files) will be saved
to.  This can be overridden with the "-game" command line parameter.  The game
directory can never be changed while quake is executing.  This is a precacution
against having a malicious server instruct clients to write files over areas
they shouldn't.
*/


static unsigned int check_known_paks (int paknum, int numfiles, unsigned short crc)
{
	if (paknum >= MAX_PAKDATA)
		return GAME_MODIFIED;

	if (strcmp(fs_gamedir_nopath, pakdata[paknum].dirname) != 0)
		return GAME_MODIFIED;	/* Raven didn't ship like that */

	if (numfiles != pakdata[paknum].numfiles)
	{
		switch (paknum)
		{
		case 0:	/* demo ?? */
			if (numfiles == demo_pakdata[0].numfiles &&
					crc == demo_pakdata[0].crc)
				return GAME_DEMO;
			/* oem ?? */
			if (numfiles == oem0_pakdata[0].numfiles &&
					crc == oem0_pakdata[0].crc)
				return GAME_OEM0;
			/* old version of demo ?? */
			if (numfiles == old_pakdata[2].numfiles &&
					crc == old_pakdata[2].crc)
				return GAME_OLD_DEMO;
			/* old cdrom version ?? */
			if (numfiles == old_pakdata[0].numfiles &&
					crc == old_pakdata[0].crc)
				return GAME_OLD_CDROM0;
			/* old oem version ?? */
			if (numfiles == old_pakdata[3].numfiles &&
					crc == old_pakdata[3].crc)
				return GAME_OLD_OEM0;
			/* not original: */
			return GAME_MODIFIED;
		case 1:	/* old cdrom version ?? */
			if (numfiles == old_pakdata[1].numfiles &&
					crc == old_pakdata[1].crc)
				return GAME_OLD_CDROM1;
			/* not original: */
			return GAME_MODIFIED;
		case 2:	/* old oem version ?? */
			if (numfiles == old_pakdata[4].numfiles &&
					crc == old_pakdata[4].crc)
				return GAME_OLD_OEM2;
			/* not original: */
			return GAME_MODIFIED;
		case 4:	/* old HW version ?? */
			if (numfiles == old_pakdata[5].numfiles &&
					crc == old_pakdata[5].crc)
				return GAME_HEXENWORLD;
			/* not original: */
			return GAME_MODIFIED;
		default:/* not original */
			return GAME_MODIFIED;
		}
	}

	if (crc != pakdata[paknum].crc)
		return GAME_MODIFIED;	/* not original */

	/* both crc and numfiles are good, we are still original */
	switch (paknum)
	{
	case 0:	/* pak0 of full version 1.11 */
		return GAME_REGISTERED0;
	case 1:	/* pak1 of full version 1.11 */
		return GAME_REGISTERED1;
	case 2:	/* bundle version */
		return GAME_OEM2;
	case 3:	/* mission pack */
		return GAME_PORTALS;
	case 4:	/* hexenworld */
		return GAME_HEXENWORLD;
	}

	return GAME_MODIFIED;	/* we shouldn't reach here */
}

/*
=================
FS_LoadPackFile

Takes a path to a pak file.  Loads the header and directory.
=================
*/
static pack_t *FS_LoadPackFile (const char *packfile, int paknum, qboolean base_fs)
{
	dpackheader_t	header;
	int	i, numpackfiles, key;
	pakfiles_t	*newfiles;
	pack_t		*pack;
	FILE		*packhandle;
	dpackfile_t	info[MAX_FILES_IN_PACK];
	unsigned short	crc;

	packhandle = FS_FOpen (packfile, "rb");
	if (!packhandle)
		return NULL;

	fread (&header, 1, sizeof(header), packhandle);
	if (header.id[0] != 'P' || header.id[1] != 'A' ||
	    header.id[2] != 'C' || header.id[3] != 'K')
	{
		Sys_Printf ("WARNING: %s is not a packfile, ignored\n", packfile);
		goto pak_error;
	}

	header.dirofs = LittleLong (header.dirofs);
	header.dirlen = LittleLong (header.dirlen);

	numpackfiles = header.dirlen / sizeof(dpackfile_t);

	if (header.dirlen < 0 || header.dirofs < 0)
	{
		Sys_Error ("Invalid packfile %s (dirlen: %i, dirofs: %i)",
					packfile, header.dirlen, header.dirofs);
	}
	if (!numpackfiles)
	{
		Sys_Printf ("WARNING: %s has no files, ignored\n", packfile);
		goto pak_error;
	}
	if (numpackfiles > MAX_FILES_IN_PACK)
	{
		Sys_Printf ("WARNING: %s has %i files (max. allowed is %i), ignored\n",
					packfile, numpackfiles, MAX_FILES_IN_PACK);
		goto pak_error;
	}

	newfiles = (pakfiles_t *) Z_Malloc (numpackfiles * sizeof(pakfiles_t), Z_MAINZONE);

	fseek (packhandle, header.dirofs, SEEK_SET);
	fread (info,  1, header.dirlen, packhandle);

	/* crc the directory */
	CRC_Init (&crc);
	for (i = 0; i < header.dirlen; i++)
		CRC_ProcessByte (&crc, ((byte *)info)[i]);

	/* check for modifications */
	if (base_fs)
		gameflags |= check_known_paks (paknum, numpackfiles, crc);
	else	gameflags |= GAME_MODIFIED;

	pack = (pack_t *) Z_Malloc (sizeof(pack_t), Z_MAINZONE);
	/* get the hash table size from the number of files in the pak */
	for (i = 1; i < MAX_FILES_IN_PACK; i <<= 1)
	{
		if (i > numpackfiles)
			break;
	}
	Hash_Allocate(&pack->hash, i);

	/* parse the directory */
	for (i = 0; i < numpackfiles; i++)
	{
		qerr_strlcpy(__thisfunc__, __LINE__, newfiles[i].name, info[i].name, MAX_QPATH);
		newfiles[i].filepos = LittleLong(info[i].filepos);
		newfiles[i].filelen = LittleLong(info[i].filelen);
		key = Hash_GenerateKeyString (&pack->hash, newfiles[i].name, false);
		Hash_Add (&pack->hash, key, i);
	}

	qerr_strlcpy(__thisfunc__, __LINE__, pack->filename, packfile, MAX_OSPATH);
	pack->handle = packhandle;
	pack->numfiles = numpackfiles;
	pack->files = newfiles;

	Sys_Printf ("Added packfile %s (%i files)\n", packfile, numpackfiles);
	return pack;
pak_error:
	fclose (packhandle);
	return NULL;
}


/*
================
FS_ZipRead

miniz I/O callback.  We do not use miniz's own stdio layer: Ironwail's trimmed
copy is built with MINIZ_NO_STDIO, and we want FS_FOpen anyway so that UTF-8
archive paths keep working on Windows.
================
*/
static size_t FS_ZipRead (void *opaque, mz_uint64 ofs, void *buf, size_t n)
{
	FILE	*f = (FILE *) opaque;

	if (fseek (f, (long) ofs, SEEK_SET) != 0)
		return 0;
	return fread (buf, 1, n, f);
}


/*
================
FS_ZipDataOffset

Resolve where a STORED entry's bytes actually start.

The central directory records the offset of the entry's *local* header, not of
its data, and the local header carries its own name and extra-field lengths
which are permitted to differ from the central copies -- so the data offset
cannot be computed from central-directory information alone.  Read the 30-byte
local header and use its lengths.

Done lazily on first open rather than for every entry at mount time: a large
pk3 would otherwise pay thousands of seeks up front for entries the map may
never touch.
================
*/
#define ZIP_LOCALHDR_SIZE	30
#define ZIP_LOCALHDR_SIG	0x04034b50

static int FS_ZipDataOffset (zippack_t *zip, mz_uint64 localhdr)
{
	byte	hdr[ZIP_LOCALHDR_SIZE];
	unsigned int	sig;
	unsigned int	namelen, extralen;

	if (fseek (zip->handle, (long) localhdr, SEEK_SET) != 0)
		return -1;
	if (fread (hdr, 1, ZIP_LOCALHDR_SIZE, zip->handle) != ZIP_LOCALHDR_SIZE)
		return -1;

	/* little-endian on the wire regardless of host byte order */
	sig = (unsigned int)hdr[0] | ((unsigned int)hdr[1] << 8) |
	      ((unsigned int)hdr[2] << 16) | ((unsigned int)hdr[3] << 24);
	if (sig != ZIP_LOCALHDR_SIG)
		return -1;

	namelen  = (unsigned int)hdr[26] | ((unsigned int)hdr[27] << 8);
	extralen = (unsigned int)hdr[28] | ((unsigned int)hdr[29] << 8);

	localhdr += ZIP_LOCALHDR_SIZE + namelen + extralen;
	if (localhdr > (mz_uint64) 0x7fffffff)
		return -1;	/* past what fseek()/filepos can address; inflate instead */

	return (int) localhdr;
}


/*
================
FS_LoadZipFile

Mount a .pk3/.zip as a searchpath.  Returns NULL (quietly enough) on anything
malformed -- a bad archive should cost the user a warning, not a Sys_Error,
because unlike pak0.pak these are optional content the engine never shipped.
================
*/
static zippack_t *FS_LoadZipFile (const char *zipfile)
{
	zippack_t	*zip;
	FILE		*handle;
	long		filesize;
	mz_uint		i, numentries;
	int		numfiles, hashsize, key;
	zipfiles_t	*newfiles;
	mz_zip_archive_file_stat	stat;

	handle = FS_FOpen (zipfile, "rb");
	if (!handle)
		return NULL;

	if (fseek (handle, 0, SEEK_END) != 0)
		goto zip_error;
	filesize = ftell (handle);
	if (filesize <= 0)
		goto zip_error;

	zip = (zippack_t *) Z_Malloc (sizeof(zippack_t), Z_MAINZONE);
	memset (zip, 0, sizeof(zippack_t));
	zip->handle = handle;
	zip->archive.m_pRead = FS_ZipRead;
	zip->archive.m_pIO_opaque = handle;

	if (!mz_zip_reader_init (&zip->archive, (mz_uint64) filesize, 0))
	{
		Sys_Printf ("WARNING: %s is not a valid zip archive, ignored\n", zipfile);
		Z_Free (zip);
		goto zip_error;
	}

	/* mz_zip_reader_get_num_files() sits inside a large block Ironwail
	 * disabled; m_total_files is the public field it would have returned. */
	numentries = zip->archive.m_total_files;
	if (!numentries)
	{
		Sys_Printf ("WARNING: %s has no files, ignored\n", zipfile);
		mz_zip_reader_end (&zip->archive);
		Z_Free (zip);
		goto zip_error;
	}
	if (numentries > MAX_FILES_IN_ZIP)
	{
		Sys_Printf ("WARNING: %s has %u files (max. allowed is %i), ignored\n",
				zipfile, (unsigned int) numentries, MAX_FILES_IN_ZIP);
		mz_zip_reader_end (&zip->archive);
		Z_Free (zip);
		goto zip_error;
	}

	newfiles = (zipfiles_t *) Z_Malloc (numentries * sizeof(zipfiles_t), Z_MAINZONE);

	/* smallest power of two that covers the entry count, floor of 16 */
	for (hashsize = 16; (mz_uint) hashsize < numentries; hashsize <<= 1)
		;
	Hash_Allocate (&zip->hash, hashsize);

	numfiles = 0;
	for (i = 0; i < numentries; i++)
	{
		if (!mz_zip_reader_file_stat (&zip->archive, i, &stat))
			continue;
		if (stat.m_is_directory)
			continue;
		if (!stat.m_is_supported)
		{	/* encrypted, or a compression method miniz cannot decode */
			Sys_Printf ("WARNING: %s: unsupported entry %s, skipped\n",
					zipfile, stat.m_filename);
			continue;
		}
		if (stat.m_uncomp_size > (mz_uint64) 0x7fffffff)
		{
			Sys_Printf ("WARNING: %s: entry %s too large, skipped\n",
					zipfile, stat.m_filename);
			continue;
		}
		if (strlen(stat.m_filename) >= MAX_QPATH)
		{	/* a name we could never look up: every FS entry point takes a
			 * MAX_QPATH buffer, so this entry is unreachable by design */
			Sys_Printf ("WARNING: %s: entry name too long, skipped: %s\n",
					zipfile, stat.m_filename);
			continue;
		}

		q_strlcpy (newfiles[numfiles].name, stat.m_filename, MAX_QPATH);
		newfiles[numfiles].index = i;
		newfiles[numfiles].filelen = (int) stat.m_uncomp_size;
		/* STORED entries get a real offset resolved on first open; anything
		 * else is flagged as needing inflation.  -1 means "not yet resolved". */
		newfiles[numfiles].filepos = (stat.m_method == 0) ? -1 : -2;

		/* Hash_GenerateKeyString(caseSensitive=false) matches how the pak path
		 * indexes, so a pk3 authored on a case-sensitive filesystem resolves
		 * the same way a pak does. */
		key = Hash_GenerateKeyString (&zip->hash, newfiles[numfiles].name, false);
		Hash_Add (&zip->hash, key, numfiles);
		numfiles++;
	}

	if (!numfiles)
	{
		Sys_Printf ("WARNING: %s has no usable files, ignored\n", zipfile);
		Hash_Free (&zip->hash);
		Z_Free (newfiles);
		mz_zip_reader_end (&zip->archive);
		Z_Free (zip);
		goto zip_error;
	}

	q_strlcpy (zip->filename, zipfile, MAX_OSPATH);
	zip->numfiles = numfiles;
	zip->files = newfiles;

	/* A pk3 is by definition content the original game never shipped. */
	gameflags |= GAME_MODIFIED;

	Sys_Printf ("Added archive %s (%i files)\n", zipfile, numfiles);
	return zip;

zip_error:
	fclose (handle);
	return NULL;
}


/*
================
FS_ZipReadEntry

Inflate a DEFLATED entry straight into a caller-supplied buffer.  buf must hold
entry->filelen bytes.  Returns false on a corrupt stream.
================
*/
static qboolean FS_ZipReadEntry (zippack_t *zip, const zipfiles_t *entry, void *buf)
{
	if (!entry->filelen)
		return true;	/* nothing to do; an empty entry is not an error */
	return mz_zip_reader_extract_to_mem (&zip->archive, entry->index, buf,
						(size_t) entry->filelen, 0) ? true : false;
}


/*
================
FS_UnwindSearchpaths

Pops and frees every searchpath entry above `mark', leaving fs_searchpaths
at `mark'.  The three callers that used to open-code this loop -- FS_Gamedir,
Host_Game_f's reset and FS_Init's mission pack rollback -- differ only in
whether they narrate what they removed.

Centralised because of fs_portals_path_id: an id that outlives the entries it
names is worse than no id at all, since PR_ShouldSubstituteProgs matches on it
to decide the file it found belongs to the mission pack.  A removal path that
forgot to clear it would hand that gate a stale answer.  uhexen2-5vb6.
================
*/
static void FS_UnwindSearchpaths (searchpath_t *mark, qboolean verbose)
{
	searchpath_t	*next;

	while (fs_searchpaths != mark)
	{
		if (fs_searchpaths->pack)
		{
			if (verbose)
				Sys_Printf ("Removed packfile %s\n", fs_searchpaths->pack->filename);
			fclose (fs_searchpaths->pack->handle);
			Z_Free (fs_searchpaths->pack->files);
			Hash_Free(&fs_searchpaths->pack->hash);
			Z_Free (fs_searchpaths->pack);
		}
		else if (fs_searchpaths->zip)
		{
			if (verbose)
				Sys_Printf ("Removed archive %s\n", fs_searchpaths->zip->filename);
			mz_zip_reader_end (&fs_searchpaths->zip->archive);
			fclose (fs_searchpaths->zip->handle);
			Z_Free (fs_searchpaths->zip->files);
			Hash_Free(&fs_searchpaths->zip->hash);
			Z_Free (fs_searchpaths->zip);
		}
		else if (verbose)
		{
			Sys_Printf ("Removed path %s\n", fs_searchpaths->filename);
		}
		if (fs_portals_path_id && fs_searchpaths->path_id == fs_portals_path_id)
			fs_portals_path_id = 0;
		next = fs_searchpaths->next;
		Z_Free (fs_searchpaths);
		fs_searchpaths = next;
	}
}


/*
================
FS_AddGameDirectory

Sets fs_gamedir, fs_userdir and fs_gamedir_nopath.  adds the directory
to the head of the path, then loads and adds pak1.pak pak2.pak ...
================
*/
static void FS_AddGameDirectory (const char *dir, qboolean base_fs)
{
	unsigned int	path_id;
	searchpath_t	*search;
	pack_t		*pak;
	char	pakfile[MAX_OSPATH];
	qboolean do_userdir = false;
	int	i;

	qerr_strlcpy(__thisfunc__, __LINE__, fs_gamedir_nopath, dir,
						sizeof(fs_gamedir_nopath));
	FSERR_MakePath_BUF (__thisfunc__, __LINE__, FS_BASEDIR,
				fs_gamedir, sizeof(fs_gamedir), dir);
	FSERR_MakePath_BUF (__thisfunc__, __LINE__, FS_USERBASE,
				fs_userdir, sizeof(fs_userdir), dir);

/* assign a path_id to this game directory */
	if (fs_searchpaths)
		path_id = fs_searchpaths->path_id << 1;
	else	path_id = 1U;

	/* Recorded where it is assigned rather than recomputed by walking the
	 * searchpath later: this is the one place that sees every add, including
	 * Host_Game_f's runtime one. */
	if (!q_strcasecmp(dir, "portals"))
		fs_portals_path_id = path_id;

#if DO_USERDIRS
add_pakfile:
#endif
/* add any pak files in the format pak0.pak pak1.pak, ...
 * unlike Quake, Hexen II can't stop at first unavailable
 * pak: the mission pack has only pak3, hw has only pak4.
 */
	for (i = 0; i < 10; i++)
	{
		FSERR_MakePath_VABUF (__thisfunc__, __LINE__,
					(do_userdir) ? FS_USERDIR : FS_GAMEDIR,
					pakfile, sizeof(pakfile), "pak%i.pak", i);
		pak = FS_LoadPackFile (pakfile, i, base_fs);
		if (!pak) continue;
		search = (searchpath_t *) Z_Malloc (sizeof(searchpath_t), Z_MAINZONE);
		search->path_id = path_id;
		search->pack = pak;
		search->next = fs_searchpaths;
		fs_searchpaths = search;
	}

/* add any .pk3 archives in this directory, alphabetically.
 *
 * Pushed after the numbered paks and before the loose directory, so the
 * resulting search order is: loose files, then pk3s, then paks.  That mirrors
 * QSS and FTE -- an archive can override pak content, a loose file can override
 * both -- and it is the order people already expect from Quake engines.
 *
 * Load order within the pk3s is ascending by name, which (because each is
 * pushed onto the head) makes the alphabetically *last* archive win.  Same
 * convention as pak1 overriding pak0.  uhexen2-pzha.
 */
	{
		fsfind_t	find;
		const char	*findname;
		char		zipnames[MAX_PK3_PER_DIR][MAX_QPATH];
		int		numzips = 0, j;
		const char	*scandir = (do_userdir) ? fs_userdir : fs_gamedir;

		findname = Sys_FindFirstFile (&find, scandir, "*.pk3");
		while (findname)
		{
			if (numzips == MAX_PK3_PER_DIR)
			{
				Sys_Printf ("WARNING: more than %i pk3 files in %s, ignoring the rest\n",
						MAX_PK3_PER_DIR, scandir);
				break;
			}
			q_strlcpy (zipnames[numzips], findname, MAX_QPATH);
			numzips++;
			findname = Sys_FindNextFile (&find);
		}
		Sys_FindClose (&find);

		/* Sys_FindFirstFile makes no ordering promise -- readdir order is
		 * filesystem order, not alphabetical -- so sort rather than assume. */
		if (numzips > 1)
			qsort (zipnames, numzips, MAX_QPATH, FS_CompareZipNames);

		for (j = 0; j < numzips; j++)
		{
			zippack_t	*zip;

			FSERR_MakePath_VABUF (__thisfunc__, __LINE__,
						(do_userdir) ? FS_USERDIR : FS_GAMEDIR,
						pakfile, sizeof(pakfile), "%s", zipnames[j]);
			zip = FS_LoadZipFile (pakfile);
			if (!zip) continue;
			search = (searchpath_t *) Z_Malloc (sizeof(searchpath_t), Z_MAINZONE);
			search->path_id = path_id;
			search->zip = zip;
			search->next = fs_searchpaths;
			fs_searchpaths = search;
		}
	}

/* add the directory itself to the search path.  unlike Quake,
 * Hexen II does this ~after~ adding the pakfiles in this dir,
 * so that the dir itself will be placed above the pakfiles in
 * the search order which, in turn, will allow override files.
 */
	search = (searchpath_t *) Z_Malloc (sizeof(searchpath_t), Z_MAINZONE);
	if (do_userdir)
		qerr_strlcpy(__thisfunc__, __LINE__, search->filename, fs_userdir, MAX_OSPATH);
	else	qerr_strlcpy(__thisfunc__, __LINE__, search->filename, fs_gamedir, MAX_OSPATH);
	search->path_id = path_id;
	search->next = fs_searchpaths;
	fs_searchpaths = search;

	if (do_userdir)
		return;
	do_userdir = true;

#if DO_USERDIRS
/* add user's directory to the search path and
 * add any pak files in the user's directory.
 */
	if (strcmp(fs_gamedir, fs_userdir))
	{
		Sys_mkdir (fs_userdir, true);
		goto add_pakfile;
	}
#endif
}

/*
================
FS_Gamedir

Sets the gamedir and path to a different directory.

Hexen II uses this for setting the game directory from a -game
command line argument.

HexenWorld uses this to set the gamedir on both server and client
sides while the game is running: The client calls this upon every
map change from within CL_ParseServerData(), and the Server calls
this upon a gamedir command from within SV_Gamedir_f().
================
*/
#ifdef H2W
static inline void set_hw_dir (void) { /* helper for FS_Gamedir () */
	qerr_strlcpy(__thisfunc__, __LINE__, fs_gamedir_nopath, "hw",
					sizeof(fs_gamedir_nopath));
	FSERR_MakePath_BUF (__thisfunc__, __LINE__, FS_BASEDIR,
				fs_gamedir, sizeof(fs_gamedir), "hw");
	FSERR_MakePath_BUF (__thisfunc__, __LINE__, FS_USERBASE,
				fs_userdir, sizeof(fs_userdir), "hw");
	#ifdef SERVERONLY
	/* change the *gamedir serverinfo properly */
	Info_SetValueForStarKey (svs.info, "*gamedir", "hw", MAX_SERVERINFO_STRING);
	#endif
}
#endif

void FS_Gamedir (const char *dir)
{
	if (!*dir || !strcmp(dir, ".") || strstr(dir, "..") || strstr(dir, "/") || strstr(dir, "\\") || strstr(dir, ":"))
	{
		if (!host_initialized)
			Sys_Error ("gamedir should be a single directory name, not a path\n");
		else {
			Con_Printf("gamedir should be a single directory name, not a path\n");
			return;
		}
	}

	if (!q_strcasecmp(fs_gamedir_nopath, dir))
		return;		/* still the same */

/* free up any current game dir info: our top searchpath dir will be hw
 * and any gamedirs set before by this very procedure will be removed.
 * since hexen2 doesn't use this during game execution there will be no
 * changes for it: it has portals or data1 at the top.
 */
	FS_UnwindSearchpaths (fs_base_searchpaths, false);

/* flush all data, so it will be forced to reload */
#if !defined(SERVERONLY)
	Cache_Flush ();
#endif

/* check for reserved gamedirs */
	if (!q_strcasecmp(dir, "hw"))
	{
#if defined(H2W)
	/* that we reached here means the hw server decided to abandon
	 * whatever the previous mod it was running and went back to
	 * pure hw. weird.. do as he wishes anyway and adjust our variables. */
		set_hw_dir ();
#else	/* hexen2 case: */
	/* hw is reserved for hexenworld only. hexen2 shouldn't use it */
		Con_Printf ("WARNING: Gamedir not set to hw :\n"
			    "It is reserved for HexenWorld.\n");
#endif	/* H2W */
		return;
	}

	if (!q_strcasecmp(dir, "portals"))
	{
	/* no hw server is supposed to set gamedir to portals
	 * and hw must be above portals in hierarchy. this is
	 * actually a hypothetical case.
	 * as for hexen2, it cannot reach here.  */
#ifdef H2W
		set_hw_dir ();
#endif
		return;
	}

	if (!q_strcasecmp(dir, "data1"))
	{
	/* another hypothetical case: no hw mod is supposed to
	 * do this and hw must stay above data1 in hierarchy.
	 * as for hexen2, it can only reach here by a silly
	 * command line argument like -game data1, ignore it. */
#ifdef H2W
		set_hw_dir ();
#endif
		return;
	}

/* a new gamedir: let's set it here. */
	FS_AddGameDirectory(dir, false);
#if defined(H2W) && defined(SERVERONLY)
/* change the *gamedir serverinfo properly */
	Info_SetValueForStarKey (svs.info, "*gamedir", dir, MAX_SERVERINFO_STRING);
#endif
}


/*
==============================================================================

FILE I/O within QFS

==============================================================================
*/

long		fs_filesize;	/* size of the last file opened through QFS */
int		file_from_pak;	/* ZOID: global indicating that file came from a pak */

/* OS path of the searchpath entry that satisfied the last lookup; see
 * FS_LastFileSource().  Tracked alongside fs_filesize / file_from_pak. */
static char	fs_lastfile_source[MAX_OSPATH];


/*
===========
FS_CopyFile

Copies the FROMPATH file as TOPATH file, creating any dirs needed.
Used for saving the game. Returns 0 on success, non-zero on error.
===========
*/
/* NOTE: reachable from the background save worker (Host_CopyFiles), so this
 * must stay free of both zone allocation and console output — neither is
 * thread-safe. FS_CreatePath only needs a mutable copy, which the stack
 * provides, and failures are reported by the caller, which names both paths. */
int FS_CopyFile (const char *frompath, const char *topath)
{
	char	tmp[MAX_OSPATH];
	int	err;

	if (!frompath || !topath)
		return 1;
	if (q_strlcpy(tmp, topath, sizeof(tmp)) >= sizeof(tmp))
		return 1;
	/* create directories up to the dest file */
	err = FS_CreatePath(tmp);
	if (err != 0)
		return err;

	return Sys_CopyFile (frompath, topath);
}

#define	COPY_READ_BUFSIZE		8192	/* BUFSIZ */
int FS_WriteFileFromHandle (FILE *fromfile, const char *topath, size_t size)
{
	char	buf[COPY_READ_BUFSIZE];
	FILE	*out;
/*	off_t	remaining, count;*/
	size_t	remaining, count;
	char	tmp[MAX_OSPATH];
	int	err;

	if (!fromfile || !topath)
	{
		Con_Printf ("%s: null input\n", __thisfunc__);
		return 1;
	}
	if (q_strlcpy(tmp, topath, sizeof(tmp)) >= sizeof(tmp))
	{
		Con_Printf ("%s: path too long\n", __thisfunc__);
		return 1;
	}

	/* create directories up to the dest file */
	err = FS_CreatePath(tmp);
	if (err != 0)
	{
		Con_Printf ("%s: unable to create directory\n", __thisfunc__);
		return err;
	}

	out = FS_FOpen (topath, "wb");
	if (!out)
	{
		Con_Printf ("%s: unable to create %s\n", topath, __thisfunc__);
		return 1;
	}

	remaining = size;
	while (remaining)
	{
		if (remaining < sizeof(buf))
			count = remaining;
		else	count = sizeof(buf);

		if (fread(buf, 1, count, fromfile) != count)
			break;
		if (fwrite(buf, 1, count, out) != count)
			break;

		remaining -= count;
	}

	fclose (out);

	return (remaining == 0)? 0 : 1;
}

/*
============
FS_WriteFile

The filename will be prefixed by the current game directory
Returns 0 on success, 1 on error.
============
*/
int FS_WriteFile (const char *filename, const void *data, size_t len)
{
	FILE	*f;
	char	name[MAX_OSPATH];
	size_t	size;
	int	err;

	FS_MakePath_BUF (FS_USERDIR, &err, name, sizeof(name), filename);
	if (err)
	{
		Host_Error("%s: %d: string buffer overflow!", __thisfunc__, __LINE__);
		return 1;
	}

	f = FS_FOpen (name, "wb");
	if (!f)
	{
		Con_Printf ("Error opening %s\n", filename);
		return 1;
	}

	Sys_Printf ("%s: %s\n", __thisfunc__, name);
	size = fwrite (data, 1, len, f);
	fclose (f);
	if (size != len)
	{
		Con_Printf ("Error in writing %s\n", filename);
		return 1;
	}
	return 0;
}


/*
============
FS_CreatePath

Creates directory under user's path, making parent directories
as needed. The path must either be a path to a file, or, if the
full path is meant to be created, it must have the trailing path
seperator. Returns 0 on success, non-zero on error.
============
*/
int FS_CreatePath (char *path)
{
	char	*ofs, c;
	int	err = 0;
	size_t	offset;

	if (!path || !*path)
	{
		Con_Printf ("%s: no path!\n", __thisfunc__);
		return 1;
	}

	if (strstr(path, ".."))
	{
		Con_Printf ("Relative pathnames are not allowed.\n");
		return 1;
	}

	offset = strlen(host_parms->userdir);
	if (offset && strstr(path, host_parms->userdir) != path)
	{
		Sys_Error ("Attempted to create a directory out of user's path");
		return 1;
	}

	ofs = path + offset;
	if (*ofs == '\0')	/* not necessarily an error. */
		return 0;
	/* check for the path separator after the userdir. */
	if (IS_DIR_SEPARATOR(*ofs))
		ofs++;
	else if (offset)
	{
		/* if the userdir itself has no trailing DIRSEP
		 * either, then it is a bad path: */
		if (!IS_DIR_SEPARATOR(host_parms->userdir[offset - 1]))
		{
			Con_Printf ("%s: bad path\n", __thisfunc__);
			return 1;
		}
	}

	for ( ; *ofs; ofs++)
	{
		c = *ofs;
		if (IS_DIR_SEPARATOR(c))
		{	/* create the directory */
			*ofs = 0;
			err = Sys_mkdir (path, false);
			*ofs = c;
			if (err)
				break;
		}
	}

	return err;
}


/*
===========
FS_ResolveCasePath

On case-sensitive filesystems (Linux), resolve a path with case-insensitive
matching. Walks each component of relpath under basedir, scanning directories
for case-insensitive matches. Returns true and writes result to resolved
(size MAX_OSPATH) on success.
===========
*/
#ifndef PLATFORM_WINDOWS
static qboolean FS_ResolveCasePath (const char *basedir, const char *relpath, char *resolved)
{
	char		buf[MAX_OSPATH];
	const char	*p, *end;
	DIR		*dir;
	struct dirent	*ent;

	q_strlcpy (buf, basedir, sizeof(buf));

	p = relpath;
	while (*p)
	{
		/* extract next path component */
		end = p;
		while (*end && *end != '/')
			end++;

		dir = opendir (buf);
		if (!dir)
			return false;

		{
			char component[MAX_QPATH];
			size_t len = end - p;
			qboolean found = false;

			if (len >= sizeof(component))
				len = sizeof(component) - 1;
			memcpy (component, p, len);
			component[len] = '\0';

			while ((ent = readdir(dir)) != NULL)
			{
				if (!q_strcasecmp(ent->d_name, component))
				{
					q_strlcat (buf, "/", sizeof(buf));
					q_strlcat (buf, ent->d_name, sizeof(buf));
					found = true;
					break;
				}
			}
			closedir (dir);
			if (!found)
				return false;
		}

		p = end;
		if (*p == '/')
			p++;
	}

	q_strlcpy (resolved, buf, MAX_OSPATH);
	return true;
}
#endif

/*
===========
FS_OpenFile_Internal

Internal function - finds the file in the search path, returns fs_filesize.
If silent is true, suppresses error messages for missing files.

If paks_only is true the directory entries are skipped and only pak members
can satisfy the lookup.  FS_AddGameDirectory links a gamedir's bare directory
entry ABOVE its pak entries on purpose (see the comment there), so a loose
file permanently hides the pak copy of the same name; this is the only way to
reach the copy underneath without moving the loose file out of the way.  A
flag through the one loop rather than a second copy of it: the two searches
must stay in step on case folding, hashing and the fs_filesize /
file_from_pak / fs_lastfile_source state every caller reads afterwards.
uhexen2-nt96.
===========
*/
static long FS_OpenFile_Internal (const char *filename, FILE **file, unsigned int *path_id, qboolean silent, qboolean paks_only)
{
	searchpath_t	*search;
	pack_t		*pak;
	char	ospath[MAX_OSPATH];
	int	i, key;

	file_from_pak = 0;
	fs_lastzip = NULL;
	fs_lastzipentry = NULL;

	/* search through the path, one element at a time */
	for (search = fs_searchpaths ; search ; search = search->next)
	{
		if (search->pack)	/* look through all the pak file elements */
		{
			pak = search->pack;
			key = Hash_GenerateKeyString (&pak->hash, filename, false);
			for (i = Hash_First(&pak->hash, key); i != -1; i = Hash_Next(&pak->hash, i))
			{
				if (q_strcasecmp(pak->files[i].name, filename) != 0)
					continue;
				/* found it! */
				fs_filesize = pak->files[i].filelen;
				file_from_pak = 1;
				q_strlcpy (fs_lastfile_source, pak->filename, sizeof(fs_lastfile_source));
				if (path_id)
					*path_id = search->path_id;
				if (!file) /* for FS_FileExists() */
					return fs_filesize;
				/* open a new file on the pakfile */
				*file = FS_FOpen (pak->filename, "rb");
				if (!*file)
					Sys_Error ("Couldn't reopen %s", pak->filename);
				fseek (*file, pak->files[i].filepos, SEEK_SET);
				return fs_filesize;
			}
		}
		else if (search->zip)	/* look through a mounted .pk3 */
		{
			zippack_t	*zip = search->zip;

			key = Hash_GenerateKeyString (&zip->hash, filename, false);
			for (i = Hash_First(&zip->hash, key); i != -1; i = Hash_Next(&zip->hash, i))
			{
				if (q_strcasecmp(zip->files[i].name, filename) != 0)
					continue;
				/* found it! */
				fs_filesize = zip->files[i].filelen;
				/* An archive member is "from a pak" for every purpose that
				 * asks: gamedir provenance, progs substitution, the modified
				 * -content check.  The question those callers are really
				 * asking is "did this come from packaged content rather than
				 * a loose file", and it did. */
				file_from_pak = 1;
				q_strlcpy (fs_lastfile_source, zip->filename, sizeof(fs_lastfile_source));
				if (path_id)
					*path_id = search->path_id;
				if (!file) /* for FS_FileExists() */
					return fs_filesize;

				if (zip->files[i].filepos == -1)
				{	/* STORED, offset not resolved yet */
					mz_zip_archive_file_stat	st;
					int	ofs = -2;

					if (mz_zip_reader_file_stat (&zip->archive, zip->files[i].index, &st))
						ofs = FS_ZipDataOffset (zip, st.m_local_header_ofs);
					/* A local header we cannot parse demotes the entry to the
					 * inflate path rather than failing the lookup: miniz can
					 * still decode it, we just cannot shortcut it. */
					zip->files[i].filepos = (ofs < 0) ? -2 : ofs;
				}

				if (zip->files[i].filepos >= 0)
				{	/* STORED: contiguous plain bytes, serve it like a pak member */
					*file = FS_FOpen (zip->filename, "rb");
					if (!*file)
						Sys_Error ("Couldn't reopen %s", zip->filename);
					fseek (*file, zip->files[i].filepos, SEEK_SET);
					return fs_filesize;
				}

				/* DEFLATED: no FILE * can represent this. */
				*file = NULL;
				fs_lastzip = zip;
				fs_lastzipentry = &zip->files[i];
				return fs_filesize;
			}
		}
		else if (!paks_only)	/* check a file in the directory tree */
		{
			q_snprintf (ospath, sizeof(ospath), "%s/%s",search->filename, filename);
			fs_filesize = Sys_filesize (ospath);
#ifndef PLATFORM_WINDOWS
			if (fs_filesize < 0)
			{	/* case-insensitive fallback for loose files */
				if (FS_ResolveCasePath (search->filename, filename, ospath))
					fs_filesize = Sys_filesize (ospath);
			}
#endif
			if (fs_filesize < 0)
				continue;
			q_strlcpy (fs_lastfile_source, ospath, sizeof(fs_lastfile_source));
			if (path_id)
				*path_id = search->path_id;
			if (!file) /* for FS_FileExists() */
				return fs_filesize;
			*file = FS_FOpen (ospath, "rb");
			if (!*file)
				Sys_Error ("Couldn't reopen %s", ospath);
			return fs_filesize;
		}
	}

	// Only print "can't find" messages when developer >= 1 and not in silent mode
	// (suppresses noise from optional external textures and missing assets)
	if (!silent && developer.integer >= 1)
		Sys_Printf ("%s: can't find %s\n", __thisfunc__, filename);

	if (file) *file = NULL;
	fs_filesize = -1;
	fs_lastfile_source[0] = '\0';
	return fs_filesize;
}

/*
===========
FS_LastFileSource

OS path of the searchpath entry that satisfied the most recent lookup:
the pak file the entry lives in, or the loose file itself.  Empty after a
failed lookup.  Same "state left behind by the last call" contract as
fs_filesize, so read it immediately after the FS_OpenFile / FS_Load*File
whose provenance you want.
===========
*/
const char *FS_LastFileSource (void)
{
	return fs_lastfile_source;
}

/*
===========
FS_OpenFile

Finds the file in the search path, returns fs_filesize.
===========
*/
long FS_OpenFile (const char *filename, FILE **file, unsigned int *path_id)
{
	return FS_OpenFile_Internal (filename, file, path_id, false, false);
}

/*
===========
FS_OpenFileInPak

Same, but only pak members are considered.  Silent, because both callers
report the miss themselves in terms the player can act on.
===========
*/
static long FS_OpenFileInPak (const char *filename, FILE **file, unsigned int *path_id)
{
	return FS_OpenFile_Internal (filename, file, path_id, true, true);
}

/*
===========
FS_OpenFile_Silent

Finds the file in the search path, returns fs_filesize.
Does not print error messages for missing files.
Use for optional file checks (e.g., external textures).
===========
*/
long FS_OpenFile_Silent (const char *filename, FILE **file, unsigned int *path_id)
{
	return FS_OpenFile_Internal (filename, file, path_id, true, false);
}


/*
===========
FS_OpenFileHandle

FS_OpenFile() for callers that want to read incrementally rather than slurp the
whole file.  Fills in the fshandle_t they would otherwise have had to populate
by hand, and -- the reason it exists -- transparently covers DEFLATED .pk3
entries, which FS_OpenFile() cannot express because there is no FILE * position
holding the decompressed bytes.

A loose file, a pak member and a STORED archive entry all come back FILE-backed
and behave exactly as before.  A deflated entry is inflated once here into a
malloc'd buffer that FS_fclose() frees.
===========
*/
static long FS_OpenFileHandle_Internal (const char *filename, fshandle_t *fh,
					unsigned int *path_id, qboolean silent)
{
	FILE	*f;
	long	len;

	if (!fh)
		return -1;

	memset (fh, 0, sizeof(*fh));

	len = silent ? FS_OpenFile_Silent (filename, &f, path_id)
		     : FS_OpenFile (filename, &f, path_id);
	if (len < 0)
		return -1;

	if (f)
	{
		fh->file = f;
		fh->pak = (file_from_pak != 0);
		fh->start = ftell (f);
		fh->length = len;
		fh->pos = 0;
		return len;
	}

	/* deflated archive entry */
	fh->data = (byte *) malloc ((size_t)len + 1);
	if (!fh->data)
	{
		Sys_Printf ("%s: out of memory for %s\n", __thisfunc__, filename);
		return -1;
	}
	fh->data[len] = 0;

	if (!FS_ZipReadEntry (fs_lastzip, fs_lastzipentry, fh->data))
	{
		Sys_Printf ("%s: corrupt deflate stream for %s in %s\n",
			    __thisfunc__, filename, fs_lastzip->filename);
		free (fh->data);
		fh->data = NULL;
		return -1;
	}

	fh->pak = true;		/* it came out of packaged content */
	fh->start = 0;
	fh->length = len;
	fh->pos = 0;
	return len;
}

long FS_OpenFileHandle (const char *filename, fshandle_t *fh, unsigned int *path_id)
{
	return FS_OpenFileHandle_Internal (filename, fh, path_id, false);
}

/* ...and the quiet form, for optional content whose absence is normal:
 * external texture replacements, DDS/KTX sidecars. */
long FS_OpenFileHandle_Silent (const char *filename, fshandle_t *fh, unsigned int *path_id)
{
	return FS_OpenFileHandle_Internal (filename, fh, path_id, true);
}

/*
===========
FS_FileExists

Returns whether the file is found in the hexen2 filesystem.
===========
*/
qboolean FS_FileExists (const char *filename, unsigned int *path_id)
{
	long ret = FS_OpenFile_Silent(filename, NULL, path_id);
	return (ret < 0) ? false : true;
}

/*
===========
FS_FileExistsInPak

Is there a copy of the file inside some pak on the search path, whether or
not a loose file is currently hiding it?  Touches no directories, so it costs
a few hash lookups and no syscalls -- ask it first when both questions are
being asked.
===========
*/
qboolean FS_FileExistsInPak (const char *filename, unsigned int *path_id)
{
	long ret = FS_OpenFileInPak(filename, NULL, path_id);
	return (ret < 0) ? false : true;
}

/*
============
FS_FileInGamedir

Reports the existance of a file with read permissions in
fs_gamedir or fs_userdir. *NOT* for files in pakfiles!
============
*/
qboolean FS_FileInGamedir (const char *filename)
{
	int	ret;

	ret = Sys_FileType(FS_MakePath(FS_USERDIR, NULL, filename));
	if (ret & FS_ENT_FILE)
		return true;
	ret = Sys_FileType(FS_MakePath(FS_GAMEDIR, NULL, filename));
	if (ret & FS_ENT_FILE)
		return true;

	return false;
}

/*
============
FS_UserdirHasFile

Is there a loose copy of `filename' in the user directory's `gamedir'?

Not the same question as FS_FileInGamedir(): that one asks about the gamedir
currently set and answers yes for the install directory's copy as well.  This
one is specifically "did the player put their own file in ~/.hexen2?", and it
takes the gamedir by name because the caller need not be asking about the
current one -- fs_userdir tracks the gamedir last *set*.

Resolved the way FS_OpenFile_Internal resolves a loose file, case-fold and
all, so that a hand-copied PROGS.DAT is seen exactly when the searchpath
would have seen it.

False where there is no user directory (Windows, OS/2), and false where the
user directory and the install directory are the same place: that is the test
FS_AddGameDirectory uses to decide whether a userdir searchpath exists at all,
and where it doesn't, the "user's own file" would just be the install's.
============
*/
qboolean FS_UserdirHasFile (const char *gamedir, const char *filename)
{
#if DO_USERDIRS
	char	userpath[MAX_OSPATH];
	char	basepath[MAX_OSPATH];
	char	ospath[MAX_OSPATH];

	FS_MakePath_BUF (FS_USERBASE, NULL, userpath, sizeof(userpath), gamedir);
	FS_MakePath_BUF (FS_BASEDIR, NULL, basepath, sizeof(basepath), gamedir);
	if (!strcmp(userpath, basepath))
		return false;

	q_snprintf (ospath, sizeof(ospath), "%s/%s", userpath, filename);
	if (Sys_FileType(ospath) & FS_ENT_FILE)
		return true;
#ifndef PLATFORM_WINDOWS
	if (FS_ResolveCasePath (userpath, filename, ospath))
		return (Sys_FileType(ospath) & FS_ENT_FILE) ? true : false;
#endif
	return false;
#else
	(void) gamedir;
	(void) filename;
	return false;
#endif	/* DO_USERDIRS */
}

/*
============
FS_LoadFile

Filename are reletive to the quake directory.
Allways appends a 0 byte to the loaded data.
============
*/
#define	LOADFILE_ZONE		0
#define	LOADFILE_HUNK		1
#define	LOADFILE_TEMPHUNK	2
#define	LOADFILE_CACHE		3
#define	LOADFILE_STACK		4
#define	LOADFILE_MALLOC		5

static byte	*loadbuf;
#if !defined(SERVERONLY)
static cache_user_t *loadcache;
#endif
static long		loadsize;
static int		zone_num;

#if defined (SERVERONLY)
#define Draw_BeginDisc()
#define Draw_EndDisc()
#endif

/* Allocate-and-read tail, shared by the searchpath loader below and by
 * FS_LoadHunkFileFromOSPath().  Consumes `h': it is closed either way. */
/* Allocation half of FS_ReadIntoBuffer(), split out so the archive path can
 * obtain a destination buffer by exactly the same rules before inflating into
 * it -- same hunk tags, same LOADFILE_STACK reuse, same Sys_Error on failure.
 * uhexen2-pzha. */
static byte *FS_AllocLoadBuffer (const char *path, int usehunk, long len)
{
	byte	*buf;
	char	base[32];

/* extract the file's base name for hunk tag */
	COM_FileBase (path, base, sizeof(base));
	buf = NULL;	/* quiet compiler warning */

	switch (usehunk)
	{
	case LOADFILE_HUNK:
		buf = (byte *) Hunk_AllocName (len+1, base);
		break;
	case LOADFILE_TEMPHUNK:
		buf = (byte *) Hunk_TempAlloc (len+1);
		break;
	case LOADFILE_ZONE:
		buf = (byte *) Z_Malloc (len+1, zone_num);
		break;
#if !defined(SERVERONLY)
	case LOADFILE_CACHE:
		buf = (byte *) Cache_Alloc (loadcache, len+1, base);
		break;
#endif
	case LOADFILE_STACK:
		if (len < loadsize)
			buf = loadbuf;
		else
			buf = (byte *) Hunk_TempAlloc (len+1);
		break;
	case LOADFILE_MALLOC:
		buf = (byte *) malloc (len+1);
		break;
	default:
		Sys_Error ("%s: bad usehunk", __thisfunc__);
	}

	if (!buf)
		Sys_Error ("%s: not enough space for %s", __thisfunc__, path);

	((byte *)buf)[len] = 0;

	return buf;
}

static byte *FS_ReadIntoBuffer (FILE *h, const char *path, int usehunk, long len)
{
	byte	*buf;
	size_t	nread;

	buf = FS_AllocLoadBuffer (path, usehunk, len);

	Draw_BeginDisc ();
	nread = fread (buf, 1, (size_t)len, h);
	fclose (h);
	Draw_EndDisc ();

	/* A short read means the length the filesystem promised and the bytes
	 * that actually arrived disagree -- a truncated pak member, an I/O
	 * error, a file rewritten under us.  Every caller sizes its parsing from
	 * fs_filesize rather than from what was read, so the gap has to be
	 * filled rather than left: Hunk_AllocName() hands back zeroed memory,
	 * but malloc(), Cache_Alloc() and the LOADFILE_STACK buffer do not, and
	 * on those paths the tail of a short read is whatever the allocator was
	 * holding, which the caller then parses as file content.  Zeroing keeps
	 * the hunk paths behaving exactly as before and extends that behaviour
	 * to the rest; the warning is what turns a genuine short read from
	 * silent into diagnosable.  Deliberately not an error: the loaders'
	 * contract is unchanged, which is what kept 9f52d0a6a from touching
	 * this.  uhexen2-huys. */
	if (nread < (size_t)len)
	{
		memset (buf + nread, 0, (size_t)len - nread);
		Sys_Printf ("WARNING: %s: short read on %s (%lu of %ld bytes)\n",
			    __thisfunc__, path, (unsigned long)nread, len);
	}

	return buf;
}


/*
===========
FS_ReadZipIntoBuffer

Inflate the DEFLATED archive entry that the preceding FS_OpenFile() landed on.

Called instead of FS_ReadIntoBuffer() when that lookup returned a length with a
NULL FILE *; see the fs_lastzip commentary above.  Allocates by the same rules,
so from the caller's side the only difference is where the bytes came from.
===========
*/
static byte *FS_ReadZipIntoBuffer (const char *path, int usehunk, long len)
{
	byte	*buf;

	buf = FS_AllocLoadBuffer (path, usehunk, len);

	Draw_BeginDisc ();
	if (!FS_ZipReadEntry (fs_lastzip, fs_lastzipentry, buf))
	{
		/* Matches FS_ReadIntoBuffer's short-read contract rather than
		 * erroring: callers size their parsing from fs_filesize, so the
		 * buffer has to be fully defined either way. */
		memset (buf, 0, (size_t)len);
		Sys_Printf ("WARNING: %s: corrupt deflate stream for %s in %s\n",
			    __thisfunc__, path, fs_lastzip->filename);
	}
	Draw_EndDisc ();

	return buf;
}

static byte *FS_LoadFile (const char *path, int usehunk, unsigned int *path_id)
{
	FILE	*h;
	long	len;

/* look for it in the filesystem or pack files */
	len = FS_OpenFile (path, &h, path_id);
	if (len < 0)
		return NULL;

	if (!h)		/* deflated archive entry -- inflate rather than read */
		return FS_ReadZipIntoBuffer (path, usehunk, len);

	return FS_ReadIntoBuffer (h, path, usehunk, len);
}

byte *FS_LoadHunkFileFromOSPath (const char *ospath)
{
	FILE	*h;
	long	len;

	len = Sys_filesize (ospath);
	if (len < 0)
		return NULL;
	h = FS_FOpen (ospath, "rb");
	if (!h)
		return NULL;

	/* Callers read fs_filesize, file_from_pak and FS_LastFileSource() as
	 * state left behind by the load.  PR_LoadProgs() uses fs_filesize three
	 * times -- the size print, the CRC over the whole file, and the strings
	 * bounds check -- so a stale one there corrupts the CRC and the bounds
	 * check silently rather than failing. */
	fs_filesize = len;
	file_from_pak = 0;
	q_strlcpy (fs_lastfile_source, ospath, sizeof(fs_lastfile_source));

	return FS_ReadIntoBuffer (h, ospath, LOADFILE_HUNK, len);
}

byte *FS_LoadHunkFile (const char *path, unsigned int *path_id)
{
	return FS_LoadFile (path, LOADFILE_HUNK, path_id);
}

byte *FS_LoadHunkFileFromPak (const char *path, unsigned int *path_id)
{
	FILE	*h;
	long	len;

	len = FS_OpenFileInPak (path, &h, path_id);
	if (len < 0)
		return NULL;

	if (!h)		/* deflated archive entry -- inflate rather than read */
		return FS_ReadZipIntoBuffer (path, LOADFILE_HUNK, len);

	/* FS_OpenFileInPak has already left fs_filesize, file_from_pak and
	 * fs_lastfile_source describing the pak member, exactly as FS_OpenFile
	 * does for FS_LoadFile above -- PR_LoadProgs' provenance line reads all
	 * three and must name the pak, not the loose file it stepped over. */
	return FS_ReadIntoBuffer (h, path, LOADFILE_HUNK, len);
}

byte *FS_LoadZoneFile (const char *path, int zone_id, unsigned int *path_id)
{
	zone_num = zone_id;
	return FS_LoadFile (path, LOADFILE_ZONE, path_id);
}

byte *FS_LoadTempFile (const char *path, unsigned int *path_id)
{
	return FS_LoadFile (path, LOADFILE_TEMPHUNK, path_id);
}

#if !defined(SERVERONLY)
void FS_LoadCacheFile (const char *path, struct cache_user_s *cu, unsigned int *path_id)
{
	loadcache = cu;
	FS_LoadFile (path, LOADFILE_CACHE, path_id);
}
#endif

/* uses temp hunk if larger than bufsize */
byte *FS_LoadStackFile (const char *path, void *buffer, long bufsize, unsigned int *path_id)
{
	byte	*buf;

	loadbuf = (byte *)buffer;
	loadsize = bufsize;
	buf = FS_LoadFile (path, LOADFILE_STACK, path_id);

	return buf;
}

/* returns malloc'd memory */
byte *FS_LoadMallocFile (const char *path, unsigned int *path_id)
{
	return FS_LoadFile (path, LOADFILE_MALLOC, path_id);
}


/*
==============================================================================

MISC CONSOLE COMMANDS

==============================================================================
*/

/*
============
FS_ListSearchSubdirs

Enumerates subdirectories of <relpath> across all loose (non-pak)
searchpath gamedirs.  Higher-priority gamedirs are scanned first;
duplicate names (case-insensitive) are skipped.  Pak entries are ignored.
============
*/
int FS_ListSearchSubdirs (const char *relpath, char dirs[][64], int maxdirs)
{
	searchpath_t	*search;
	char		path[MAX_OSPATH];
	char		buf[64][64];
	int		count = 0;
	int		i, n, j;

	if (!relpath || !dirs || maxdirs <= 0)
		return 0;

	for (search = fs_searchpaths; search && count < maxdirs; search = search->next)
	{
		if (search->pack)
			continue;
		q_snprintf (path, sizeof(path), "%s/%s", search->filename, relpath);
		n = Sys_ListDirectories (path, buf, 64);
		for (i = 0; i < n && count < maxdirs; i++)
		{
			qboolean dup = false;
			for (j = 0; j < count; j++)
			{
				if (!q_strcasecmp(dirs[j], buf[i]))
				{
					dup = true;
					break;
				}
			}
			if (!dup)
			{
				q_strlcpy (dirs[count], buf[i], 64);
				count++;
			}
		}
	}

	return count;
}


/*
============
FS_Path_f
Prints the search path to the console
============
*/
static void FS_Path_f (void)
{
	searchpath_t	*s;

	Con_Printf ("Current search path:\n");
	for (s = fs_searchpaths ; s ; s = s->next)
	{
		if (s == fs_base_searchpaths)
			Con_Printf ("----------\n");
		if (s->pack)
			Con_Printf ("%s (%i files)\n", s->pack->filename, s->pack->numfiles);
		else
			Con_Printf ("%s\n", s->filename);
	}
}

/*
==============================================================================

NAME LISTING (console listers and tab-completion)

The listers below all share one name list. Only one list can be live at a
time, which is fine because every caller is synchronous: it reads the names
and calls FS_FreeNameList() before returning. Each scan drops whatever the
previous caller left behind, so a caller that bails out early cannot leak.

==============================================================================
*/
#if !defined(SERVERONLY)	/* dedicated servers dont need these commands */

#define MAX_LISTNAMES	256	/* max number of names to list */
#define MAX_GAMEDIRS	128	/* max subdirectories probed for "game" */
static int	listname_count = 0;
static char	*listnames[MAX_LISTNAMES];

/*
===========
FS_FreeNameList
Releases the names handed out by the last scan.
===========
*/
void FS_FreeNameList (void)
{
	while (listname_count)
		Z_Free (listnames[--listname_count]);
}

/*
===========
addListName
Adds one already-trimmed name to the list, skipping names that
are already there (case-insensitively, so the same file reached
through two searchpaths is only offered once).
Returns 0 if the name is skipped, the new count if it is added,
or -1 when the list is full.
===========
*/
static int addListName (const char *name)
{
	int	j;

	if (listname_count >= MAX_LISTNAMES)
	{
		Con_Printf ("WARNING: reached maximum number of names to list\n");
		return -1;
	}

	for (j = 0; j < listname_count; j++)
	{
		if (! q_strcasecmp(listnames[j], name))
			return 0;	/* duplicated name. skip. */
	}

	listnames[listname_count] = Z_Strdup (name);
	return (++listname_count);
}

/*
===========
addFileName
Prefix-filters a filename, strips its extension and adds it.
The prefix is matched against the name with the extension still
on, which is what the maplist command has always done.
===========
*/
static int addFileName (const char *filename, const char *partial, size_t len_partial, const char *ext)
{
	char	cur_name[MAX_QPATH];
	size_t	extlen = strlen(ext);
	size_t	len;

	if (len_partial)
	{
		if (q_strncasecmp(partial, filename, len_partial) != 0)
			return 0;	/* doesn't match the prefix. skip. */
	}

	len = q_strlcpy (cur_name, filename, sizeof(cur_name));
	if (len >= sizeof(cur_name))
		return 0;	/* truncated: not a name we could hand back */
	if (len <= extlen)
		return 0;

	len -= extlen;
	if (q_strcasecmp(&cur_name[len], ext) != 0)
		return 0;

	cur_name[len] = 0;

	return addListName (cur_name);
}

/*
===========
FS_ScanFiles
Walks every searchpath collecting files with the given extension.
subdir names a directory below the gamedir ("maps"), or is NULL to
scan the gamedir root, in which case pak entries in subdirectories
are ignored. Fills the shared name list.
===========
*/
static void FS_ScanFilesEx (const char *subdir, const char *ext, const char *prefix, size_t preLen, qboolean reset)
{
	searchpath_t	*search;
	fsfind_t	find;
	const char	*findname;
	char		pakdir[MAX_QPATH];
	char		pattern[MAX_QPATH];
	size_t		dirlen;

	if (reset)
		FS_FreeNameList ();

	pakdir[0] = 0;
	if (subdir)
		q_snprintf (pakdir, sizeof(pakdir), "%s/", subdir);
	dirlen = strlen (pakdir);
	q_snprintf (pattern, sizeof(pattern), "*%s", ext);

	for (search = fs_searchpaths; search; search = search->next)
	{
		if (search->pack)
		{
			int i = 0;
			for (; i < search->pack->numfiles; ++i)
			{
				const char *name = search->pack->files[i].name;

				if (dirlen)
				{
					if (strncmp(pakdir, name, dirlen) != 0)
						continue;
					name += dirlen;
				}
				else if (strchr(name, '/'))
					continue;	/* gamedir root only */

				if (addFileName(name, prefix, preLen, ext) < 0)
					return;
			}
		}
		else
		{
			findname = Sys_FindFirstFile(&find,
					subdir ? va("%s/%s", search->filename, subdir) : search->filename,
					pattern);
			while (findname)
			{
				if (addFileName(findname, prefix, preLen, ext) < 0)
				{
					Sys_FindClose (&find);
					return;
				}
				findname = Sys_FindNextFile (&find);
			}
			Sys_FindClose (&find);
		}
	}
}

/* The common case: one extension, starting a fresh list. */
static void FS_ScanFiles (const char *subdir, const char *ext, const char *prefix, size_t preLen)
{
	FS_ScanFilesEx (subdir, ext, prefix, preLen, true);
}

/*
===========
fillMatches
Hands the scanned names to a tab-completion caller in the
ListCommands() shape: fills buf[pos+i], returns the count added.
===========
*/
static int fillMatches (const char **buf, int pos)
{
	int	i;

	if (!buf)
		return 0;

	for (i = 0; i < listname_count; i++)
	{
		if (pos + i >= MAX_MATCHES)
			break;
		buf[pos + i] = listnames[i];
	}

	return i;
}

/*
===========
ListMaps
Console tab-completion lister for map names.
===========
*/
int ListMaps (const char *prefix, const char **buf, int pos)
{
	FS_ScanFiles ("maps", ".bsp", prefix, (prefix == NULL) ? 0 : strlen(prefix));
	return fillMatches (buf, pos);
}

/*
===========
ListDemos
Console tab-completion lister for demo names. Demos live in the
gamedir root, and CL_Record_f writes them through FS_USERDIR, so
on platforms with a user directory the freshly recorded ones are
picked up from that searchpath entry rather than the basedir one.
===========
*/
int ListDemos (const char *prefix, const char **buf, int pos)
{
	FS_ScanFiles (NULL, ".dem", prefix, (prefix == NULL) ? 0 : strlen(prefix));
	return fillMatches (buf, pos);
}

/*
===========
ListCfgs
Console tab-completion lister for "exec" targets: .cfg basenames in
the gamedir root across every searchpath.  That includes the user
directory, so a config the player wrote themselves completes the same
as the shipped ones.
===========
*/
int ListCfgs (const char *prefix, const char **buf, int pos)
{
	FS_ScanFiles (NULL, ".cfg", prefix, (prefix == NULL) ? 0 : strlen(prefix));
	return fillMatches (buf, pos);
}

/*
===========
ListSkies
Console tab-completion lister for "sky" targets.

Sky_LoadSkyBox builds gfx/env/<name>_<suf>.<ext> for suf in rt/bk/lf/
ft/up/dn, trying png, then tga, then pcx.  So the set of valid names is
exactly the set of files matching gfx/env/*_rt.<ext> with the suffix
trimmed -- the right face is enough to offer a name, and demanding all
six would reject a half-installed skybox that the loader itself accepts
face by face.

Names ending in '_' are not a separate case: Sky_LoadSkyBox only skips
the separator it would otherwise double, so "moon" and "moon_" resolve
to the same files and "moon" is the form to offer.
===========
*/
int ListSkies (const char *prefix, const char **buf, int pos)
{
	size_t	preLen = (prefix == NULL) ? 0 : strlen(prefix);

	FS_ScanFilesEx ("gfx/env", "_rt.png", prefix, preLen, true);
	FS_ScanFilesEx ("gfx/env", "_rt.tga", prefix, preLen, false);
	FS_ScanFilesEx ("gfx/env", "_rt.pcx", prefix, preLen, false);
	return fillMatches (buf, pos);
}

/*
===========
ListSaves
Console tab-completion lister for "save" and "load" targets.

The console commands take an arbitrary name, unlike the menu's fixed
s0..sN / ms0..msN slots, so this enumerates whatever is actually there:
directories directly under fs_userdir holding an info.dat.  That file is
what Host_Loadgame_f reads first, so its presence is the same test the
loader applies -- a directory without one would complete and then fail.

Offered for "save" as well as "load" so overwriting an existing save is
as easy to type as loading it.  New names simply have nothing to
complete against, which is correct.
===========
*/
int ListSaves (const char *prefix, const char **buf, int pos)
{
	char	alldirs[MAX_GAMEDIRS][MAX_QPATH];
	char	path[MAX_OSPATH];
	size_t	preLen = (prefix == NULL) ? 0 : strlen(prefix);
	int	numdirs, i;

	FS_FreeNameList ();

	numdirs = Sys_ListDirectories (fs_userdir, alldirs, MAX_GAMEDIRS);

	for (i = 0; i < numdirs; i++)
	{
		if (preLen && q_strncasecmp(prefix, alldirs[i], preLen) != 0)
			continue;
		q_snprintf (path, sizeof(path), "%s/%s/info.dat", fs_userdir, alldirs[i]);
		if (Sys_FileType(path) != FS_ENT_FILE)
			continue;
		if (addListName(alldirs[i]) < 0)
			break;
	}

	return fillMatches (buf, pos);
}

/*
===========
FS_IsGamedir
Is basedir/dir a game data directory? This is the one definition of
"mod directory" -- the mods menu and "game" tab-completion both use it.

It answers "would a player call this a mod", not "can the engine mount
it": Host_Game_f mounts any directory that exists and never consults
this, so a directory rejected here is not unusable, only undiscoverable.
That distinction is why the test is deliberately generous -- a mod that
fails it is one the player installed and then cannot find.
===========
*/
qboolean FS_IsGamedir (const char *basedir, const char *dir)
{
	char	path[MAX_OSPATH];
	fsfind_t	find;
	const char	*findname;
	qboolean	found;
	int	i;

	q_snprintf (path, sizeof(path), "%s/%s/progs.dat", basedir, dir);
	if (Sys_FileType(path) == FS_ENT_FILE)
		return true;

	/* ...or hwprogs.dat.  A HexenWorld mod carries gamecode just as much as a
	 * Hexen II one does.  Six of the thirty-nine mods in the reference corpus
	 * (docs/MODS_CORPUS.md) ship a loose hwprogs.dat and nothing this function
	 * used to recognise -- db, hexarena, hwctf, hwcycle, rk and siege -- so
	 * every one of them was invisible to the mods menu and to "game"
	 * tab-completion while "game <dir>" accepted it without complaint.
	 * The Hexen II client still cannot run them; the mods menu tags them and
	 * refuses to select them.  Being listed and marked beats not existing.
	 * uhexen2-3m0h. */
	q_snprintf (path, sizeof(path), "%s/%s/hwprogs.dat", basedir, dir);
	if (Sys_FileType(path) == FS_ENT_FILE)
		return true;

	for (i = 0; i < 10; i++)
	{
		q_snprintf (path, sizeof(path), "%s/%s/pak%d.pak", basedir, dir, i);
		if (Sys_FileType(path) == FS_ENT_FILE)
			return true;
	}

	/* ...or any .pk3.  A mod shipped as a single archive is still a mod, and
	 * without this it would mount correctly but never appear in the mods menu
	 * or in "game" tab-completion.  Unlike the pak probe above this cannot be
	 * a fixed set of names, since pk3s carry arbitrary ones.  uhexen2-pzha. */
	q_snprintf (path, sizeof(path), "%s/%s", basedir, dir);
	findname = Sys_FindFirstFile (&find, path, "*.pk3");
	found = (findname != NULL);
	Sys_FindClose (&find);
	if (found)
		return true;

	/* ...or a maps/ directory holding at least one .bsp.  A pure map pack
	 * ships no gamecode and no archive -- it runs on the base game's
	 * progs.dat -- but it is still a mod the player dropped in and expects to
	 * see listed.  fo4d (a seventeen-level mission) and tt in the corpus are
	 * exactly this shape, and both were invisible.  The .bsp probe matters:
	 * a bare maps/ directory, or one holding only .lit or .ent sidecars, is
	 * not a map pack.  uhexen2-3m0h. */
	q_snprintf (path, sizeof(path), "%s/%s/maps", basedir, dir);
	if (Sys_FileType(path) != FS_ENT_DIRECTORY)
		return false;

	findname = Sys_FindFirstFile (&find, path, "*.bsp");
	found = (findname != NULL);
	Sys_FindClose (&find);

	return found;
}

/*
===========
ListGames
Console tab-completion lister for "game" targets. Unlike the mods
menu, which renders data1 and portals as fixed rows and so leaves
them out of its scan, this offers everything Host_Game_f accepts:
data1 unconditionally (it is accepted without a filesystem probe),
portals when it is installed, and every other game data directory.
===========
*/
int ListGames (const char *prefix, const char **buf, int pos)
{
	char	alldirs[MAX_GAMEDIRS][MAX_QPATH];
	char	path[MAX_OSPATH];
	size_t	preLen = (prefix == NULL) ? 0 : strlen(prefix);
	int	numdirs, i;

	FS_FreeNameList ();

	/* fs_basedir, not host_parms->basedir: Host_Game_f resolves its
	 * argument against the former, which -basedir moves.  uhexen2-5mhd. */
	if (!preLen || !q_strncasecmp(prefix, "data1", preLen))
		addListName ("data1");

	q_snprintf (path, sizeof(path), "%s/portals", fs_basedir);
	if (Sys_FileType(path) == FS_ENT_DIRECTORY)
	{
		if (!preLen || !q_strncasecmp(prefix, "portals", preLen))
			addListName ("portals");
	}

	numdirs = Sys_ListDirectories (fs_basedir, alldirs, MAX_GAMEDIRS);

	for (i = 0; i < numdirs; i++)
	{
		/* hw holds HexenWorld's data, which only the hwcl/hwsv binaries
		 * can use; the single player client has nothing to do with it */
		if (!q_strcasecmp(alldirs[i], "hw"))
			continue;
		if (preLen && q_strncasecmp(prefix, alldirs[i], preLen) != 0)
			continue;
		if (!FS_IsGamedir(fs_basedir, alldirs[i]))
			continue;
		if (addListName(alldirs[i]) < 0)
			break;
	}

	return fillMatches (buf, pos);
}

/*
===========
FS_Maplist_f
Prints map filenames to the console
===========
*/
static void FS_Maplist_f (void)
{
	const char	*prefix;
	size_t		preLen;

	if (Cmd_Argc() > 1)
	{
		prefix = Cmd_Argv(1);
		preLen = strlen(prefix);
	}
	else
	{
		preLen = 0;
		prefix = NULL;
	}

	FS_ScanFiles ("maps", ".bsp", prefix, preLen);

	if (!listname_count)
	{
		Con_Printf ("No maps found.\n\n");
		return;
	}

	Con_Printf ("Found %d maps:\n\n", listname_count);
	/* sort the list */
	if (listname_count > 1)
		qsort (listnames, listname_count, sizeof(char *), COM_StrCompare);
	Con_ShowList (listname_count, (const char**)listnames);
	Con_Printf ("\n");

	FS_FreeNameList ();
}
#endif	/* SERVERONLY */


/*
==============================================================================

INIT

==============================================================================
*/

/*
================
CheckRegistered

Looks for the pop.txt file and verifies it.
Sets the registered flag.
================
*/
static int CheckRegistered (void)
{
	fshandle_t	fh;
	unsigned short	check[128];
	int	i;

	/* Through the handle, not a raw FILE *: an install repacked as a .pk3 --
	 * which is the whole point of uhexen2-pzha -- keeps pop.lmp in a deflated
	 * entry, and FS_OpenFile() hands back a NULL FILE * for those.  Reading it
	 * the old way silently demoted a registered install to shareware. */
	if (FS_OpenFileHandle ("gfx/pop.lmp", &fh, NULL) < 0)
		return -1;

	if (FS_fread (check, 1, sizeof(check), &fh) != sizeof(check))
	{
		FS_fclose (&fh);
		return -1;
	}
	FS_fclose (&fh);

	for (i = 0; i < 128; i++)
	{
		if (pop[i] != (unsigned short)BigShort(check[i]))
		{
			Sys_Printf ("Corrupted data file\n");
			return -1;
		}
	}

	return 0;
}

/*
================
Host_Game_f

Runtime mod switching: "game <dirname>" to switch, "game" to print current.
================
*/
#if !defined(SERVERONLY)
static void Host_Game_f (void)
{
	const char	*dir;
	char		path[MAX_OSPATH];
	qboolean	use_portals = false;

	if (Cmd_Argc() < 2)
	{
		Con_Printf ("Current game directory: %s\n", fs_gamedir_nopath);
		return;
	}

	dir = Cmd_Argv(1);

	/* optional second arg: 1 = include portals data */
	if (Cmd_Argc() >= 3)
		use_portals = (atoi(Cmd_Argv(2)) != 0);

	/* validate */
	if (!*dir || !strcmp(dir, ".") || strstr(dir, "..") || strstr(dir, "/") || strstr(dir, "\\"))
	{
		Con_Printf ("gamedir should be a single directory name, not a path\n");
		return;
	}

	/* switching back to base game */
	if (!q_strcasecmp(dir, "data1"))
		dir = "data1";

	/* already the current game? */
	if (!q_strcasecmp(fs_gamedir_nopath, dir))
	{
		Con_Printf ("Already running: %s\n", dir);
		return;
	}

	/* validate that the directory exists (skip for data1) */
	if (q_strcasecmp(dir, "data1"))
	{
		/* fs_basedir, not host_parms->basedir: -basedir moves the former and
		 * never touches the latter, so probing host_parms->basedir looks for
		 * mods under the working directory the process happened to start in.
		 * uhexen2-5mhd. */
		q_snprintf (path, sizeof(path), "%s/%s", fs_basedir, dir);
		if (Sys_FileType(path) != FS_ENT_DIRECTORY)
		{
			Con_Printf ("Game directory \"%s\" not found\n", dir);
			return;
		}
	}

	/* === FULL ENGINE RESET === */

	/* save config to old mod's directory before switching */
	Host_WriteConfiguration ("config.cfg");

	/* stop everything */
	CL_Disconnect ();
	Host_ShutdownServer (true);

	/* flush all memory (models, sounds, hunk) */
	Host_ClearMemory ();

	/* clear stale client static state — close demo file if open */
	if (cls.demofile)
	{
		fclose (cls.demofile);
		cls.demofile = NULL;
	}
	memset (cls.demos, 0, sizeof(cls.demos));
	cls.demonum = 0;
	cls.demorecording = false;
	cls.demoplayback = false;
	cls.timedemo = false;
	cls.signon = 0;

	/* discard any pending commands from the old mod */
	Cbuf_Clear ();

	/* Reset the filesystem back to the base game before adding new game
	 * paths.  Unwind to fs_base_nomp_searchpaths, NOT fs_base_searchpaths:
	 * the latter includes portals whenever the session was launched with
	 * -game, because FS_Init step 2 folds the mission pack into the base for
	 * mods to read.  Stopping there left pak3 on the path across every later
	 * switch, so "game data1" put the player on base-game maps running
	 * mission-pack content -- maps, models, sounds and progs.dat alike.
	 * Portals is re-added below when the destination actually wants it.
	 * uhexen2-5vb6. */
	FS_UnwindSearchpaths (fs_base_nomp_searchpaths, false);
	fs_base_searchpaths = fs_base_nomp_searchpaths;
	Cache_Flush ();

	/* optionally add portals as base for custom mods */
	if (use_portals && q_strcasecmp(dir, "data1") && q_strcasecmp(dir, "portals"))
	{
		q_snprintf (path, sizeof(path), "%s/portals", fs_basedir);	/* uhexen2-5mhd */
		if (Sys_FileType(path) == FS_ENT_DIRECTORY)
		{
			FS_AddGameDirectory ("portals", true);
			fs_base_searchpaths = fs_searchpaths;
		}
	}

	/* add the new game directory (skip for data1 — already in base) */
	if (q_strcasecmp(dir, "data1"))
		FS_AddGameDirectory (dir, !q_strcasecmp(dir, "portals"));
	else
	{
		/* reset gamedir tracking to data1 */
		qerr_strlcpy(__thisfunc__, __LINE__, fs_gamedir_nopath, "data1",
						sizeof(fs_gamedir_nopath));
		FS_MakePath_BUF (FS_BASEDIR, NULL, fs_gamedir, sizeof(fs_gamedir), "data1");
		FS_MakePath_BUF (FS_USERBASE, NULL, fs_userdir, sizeof(fs_userdir), "data1");
	}

	/* flush non-persistent GL textures and reload menu/HUD graphics */
#ifdef GLQUAKE
	{ extern void TexMgr_NewGame (void); TexMgr_NewGame (); }
#endif
	Draw_ReInit ();
	BGM_Stop ();	/* stop music from previous game */

	/* re-read bindlist.lst so the previous mod's Key Setup rows don't linger */
	M_BuildBindList ();

	/* clean slate: reload binds, aliases, and configs from new mod */
	Cbuf_AddText ("unbindall\nunaliasall\n");
	Cbuf_AddText (Cmd_StartupScript ());
	Con_Printf ("\ngame changed to \"%s\"\n", dir);
}
#endif	/* !SERVERONLY */


/*
================
FS_Init
================
*/
void FS_Init (void)
{
	qboolean check_portals = false;
	int	i;

	Cvar_RegisterVariable (&oem);
	Cvar_RegisterVariable (&registered);

	Cmd_AddCommand ("path", FS_Path_f);
#if !defined(SERVERONLY)
	Cmd_AddCommand ("maplist", FS_Maplist_f);
	Cmd_AddCommand ("game", Host_Game_f);
#endif

/* -basedir <path> overrides the system supplied base directory */
	i = COM_CheckParm ("-basedir");
	if (i && i < com_argc-1)
	{
		fs_basedir = com_argv[i+1];
		if (!*fs_basedir) Sys_Error("Bad argument to -basedir");
#if !DO_USERDIRS
		host_parms->userdir = com_argv[i+1];
#endif
		Sys_Printf ("%s: basedir changed to: %s\n", __thisfunc__, fs_basedir);
	}
	else
	{
		fs_basedir = host_parms->basedir;
	}

/* step 1: start up with data1 by default */
	FS_AddGameDirectory ("data1", true);

	if (gameflags & GAME_REGISTERED0 && gameflags & GAME_REGISTERED1)
		gameflags |= GAME_REGISTERED;
	if (gameflags & GAME_OEM0 && gameflags & GAME_OEM2)
		gameflags |= GAME_OEM;
	if (gameflags & GAME_OLD_CDROM0 && gameflags & GAME_OLD_CDROM1)
		gameflags |= GAME_REGISTERED_OLD;
	if (gameflags & GAME_OLD_OEM0 && gameflags & GAME_OLD_OEM2)
		gameflags |= GAME_OLD_OEM;
	/* check for bad installations (mix'n'match data): */
	if ((gameflags & GAME_REGISTERED0 && gameflags & GAME_OLD_CDROM1) ||
	    (gameflags & GAME_REGISTERED1 && gameflags & GAME_OLD_CDROM0) ||
	    (gameflags & (GAME_OEM2|GAME_OLD_OEM2) && gameflags & (GAME_REGISTERED|GAME_REGISTERED_OLD|GAME_DEMO|GAME_OLD_DEMO)) ||
	    (gameflags & (GAME_REGISTERED1|GAME_OLD_CDROM1) &&
					 gameflags & (GAME_DEMO|GAME_OLD_DEMO|GAME_OEM0|GAME_OLD_OEM0|GAME_OEM2|GAME_OLD_OEM2)))
	{
		Sys_Error ("Bad Hexen II installation: mixed data from incompatible versions");
	}
#if !ENABLE_OLD_DEMO
	if (gameflags & GAME_OLD_DEMO)
		Sys_Error ("Old version of Hexen II demo isn't supported");
#endif	/* OLD_DEMO */
#if !ENABLE_OLD_RETAIL
	/* check if we have 1.11 versions of pak0.pak and pak1.pak */
	if (gameflags & (GAME_OLD_CDROM0|GAME_OLD_CDROM1|GAME_OLD_OEM0|GAME_OLD_OEM2))
		Sys_Error ("You must patch your installation with Raven's 1.11 update");
#endif	/* OLD_RETAIL */

	/* finish the base filesystem setup */
	if (gameflags & (GAME_REGISTERED|GAME_REGISTERED_OLD))
	{
		Cvar_SetROM ("registered", "1");
		Sys_Printf ("Playing the registered version.\n");
	}
	else if (gameflags & GAME_OEM)
	{
		Cvar_SetROM ("oem", "1");
		Sys_Printf ("Playing the oem (Matrox m3D bundle) version \"Continent of Blackmarsh\"\n");
	}
	else if (gameflags & (GAME_DEMO|GAME_OLD_DEMO))
	{
		Sys_Printf ("Playing the demo version.\n");
	}
	else
	{
	/* No proper Raven data.  Still fatal, but this is the first thing a new
	 * user meets and the bare one-line version told them nothing: not what
	 * file was wanted, not where it was looked for, not how to obtain any.
	 * Hexenwail ships no game content and cannot -- see assets/demo/README.md
	 * and uhexen2-3vmk -- so pointing at the fetch helper is the whole of the
	 * answer we can give.  uhexen2-49ep. */
		Sys_Error ("Unable to find a proper Hexen II installation.\n"
			   "\n"
			   "Wanted a \"data1\" directory containing pak0.pak, under:\n"
			   "    %s\n"
			   "\n"
			   "If you own Hexen II (Steam, GOG or the CD), copy that\n"
			   "installation's data1 directory to the path above.\n"
			   "\n"
			   "If you don't, the free three-level 1997 demo works. A\n"
			   "helper that downloads and verifies it ships next to this\n"
			   "executable:\n"
#ifdef PLATFORM_WINDOWS
			   "    get_demo.cmd\n"
			   "\n"
			   "(double-click it, or run it from cmd). In a source\n"
			   "checkout it is scripts\\get_demo.cmd instead.",
#else
			   "    ./get_demo.sh\n"
			   "\n"
			   "In a source checkout it is scripts/get_demo.sh instead, or\n"
			   "run: nix run .#get-demo -- \"%s\"",
#endif
			   fs_basedir
#ifndef PLATFORM_WINDOWS
			   , fs_basedir
#endif
			   );
	}
	if (gameflags & (GAME_OLD_DEMO|GAME_REGISTERED_OLD|GAME_OLD_OEM))
		Sys_Printf ("Using old/unsupported, pre-1.11 version pak files.\n");
	if (gameflags & (GAME_REGISTERED|GAME_REGISTERED_OLD))
	{
		if (CheckRegistered() != 0)
			Sys_Error ("Unable to verify retail version data.");
	}
#if !(defined(H2W) && defined(SERVERONLY))
	if (gameflags & GAME_MODIFIED && !(gameflags & (GAME_REGISTERED|GAME_REGISTERED_OLD)))
		Sys_Error ("You must have the full version of Hexen II to play modified games");
#endif

/* Mark the end of step 1.  Everything at or below this point is the base
 * game and nothing else; Host_Game_f unwinds here to return to data1.
 * uhexen2-5vb6. */
	fs_base_nomp_searchpaths = fs_searchpaths;

/* step 2: portals directory (mission pack) */
#if defined(H2MP)
	if (! COM_CheckParm ("-noportals"))
		check_portals = true;
	if (check_portals && !(gameflags & (GAME_REGISTERED|GAME_REGISTERED_OLD)))
		Sys_Error ("Portal of Praevus requires registered version of Hexen II");
#elif defined(H2W)
	if (! COM_CheckParm ("-noportals") && gameflags & (GAME_REGISTERED|GAME_REGISTERED_OLD))
		check_portals = true;
#else
	/* Mission pack support.
	 *
	 * Default behavior: when -game/-mod selects a custom directory, fold
	 * portals/ into the searchpath automatically.  Custom mods commonly
	 * depend on shared mission pack assets (sucwp2p.mdl etc.), and any
	 * progs.dat that uses the v1.12 / Mission Pack features assumes the
	 * portals data is present.  Pass -noportals to opt out.
	 *
	 * If portals/ is missing, the rollback path below prints a one-line
	 * warning and continues with data1+gamedir, same as H2MP builds. */
	if (COM_CheckParm ("-noportals"))
	{
		check_portals = false;
	}
	else
	{
		check_portals = (COM_CheckParm ("-portals")) ||
				(COM_CheckParm ("-missionpack")) ||
				(COM_CheckParm ("-h2mp"));
		i = COM_CheckParm ("-game");
		if (i && i < com_argc-1)
			check_portals = true;
		i = COM_CheckParm ("-mod");
		if (i && i < com_argc-1)
			check_portals = true;
	}
	if (check_portals && !(gameflags & (GAME_REGISTERED|GAME_REGISTERED_OLD)))
		Sys_Error ("Portal of Praevus requires registered version of Hexen II");
#endif
#if !defined(H2W)
	if (sv_protocol == PROTOCOL_RAVEN_111 && check_portals)
		Sys_Error ("Old protocol request not compatible with the Mission Pack");
#endif

	if (check_portals)
	{
		searchpath_t	*mark = fs_searchpaths;
		FS_AddGameDirectory ("portals", true);
		if (! (gameflags & GAME_PORTALS))
		{
			/* back out searchpaths from invalid mission pack
			 * installations because the portals directory is
			 * reserved for the mission pack */
			Sys_Printf ("Missing or invalid mission pack installation\n");
			Con_Printf("Missing or invalid mission pack installation\n");

			FS_UnwindSearchpaths (mark, true);
			/* back to data1.  All three, the way Host_Game_f's own
			 * reset-to-data1 does it: leaving fs_gamedir_nopath on
			 * "portals" while the path holds only data1 makes the
			 * engine misreport which game it is running, and any
			 * gate keyed on that name inherits the lie.
			 * uhexen2-1bmj. */
			qerr_strlcpy(__thisfunc__, __LINE__, fs_gamedir_nopath, "data1",
							sizeof(fs_gamedir_nopath));
			FS_MakePath_BUF (FS_BASEDIR, NULL, fs_gamedir, sizeof(fs_gamedir), "data1");
			FS_MakePath_BUF (FS_USERBASE,NULL, fs_userdir, sizeof(fs_userdir), "data1");
		}
		/* nothing to do on success: the portals entry stays where
		 * FS_AddGameDirectory put it, below fs_base_searchpaths. */
	}

/* step 3: hw directory (hexenworld) */
#if defined(H2W)
	FS_AddGameDirectory ("hw", true);
	/* error out if GAME_HEXENWORLD isn't set */
	if (!(gameflags & GAME_HEXENWORLD))
		Sys_Error ("You must have the HexenWorld data installed");
	/* hw is added ABOVE portals, so a single pointer cannot mark a base that
	 * keeps hw and drops the mission pack.  HexenWorld never rolls back to
	 * data1 at runtime -- FS_Gamedir refuses both "data1" and "portals" -- so
	 * collapse the two marks rather than invent a partial unwind for a path
	 * nothing takes. */
	fs_base_nomp_searchpaths = fs_searchpaths;
#endif	/* H2W */

/* this is the end of our base searchpath:
 * any set gamedirs, such as those from -game commandline
 * arguments, from exec'ed configs or the ones dictated by
 * the server, will be freed up to here upon a new gamedir
 * command */
	fs_base_searchpaths = fs_searchpaths;

	i = COM_CheckParm ("-game");
	if (i == 0)
		i = COM_CheckParm ("-mod");
	if (i != 0)
	{
		/* only registered versions can do -game/-mod */
		if (! (gameflags & (GAME_REGISTERED|GAME_REGISTERED_OLD)))
			Sys_Error ("You must have the full version of Hexen II to play modified games");
		/* add basedir/gamedir as an override game.
		 * Mods depend on shared mission pack models (sucwp2p.mdl etc.),
		 * so portals must sit below the mod in the search path.  Step 2
		 * normally already put it there, *below* fs_base_searchpaths,
		 * where FS_Gamedir will not strip it -- adding it again here
		 * would only burn a second path_id and fd (uhexen2-6h8x).
		 *
		 * And it is not re-added when step 2 rolled it back out.  That
		 * rollback happens for exactly one reason -- the mission pack
		 * failed validation -- because check_portals is what gates step
		 * 2 in the first place, so "step 2 ran but portals is not on
		 * the path" can only mean it was rejected.  Putting the
		 * directory back here would undo a rollback whose whole purpose
		 * is that "the portals directory is reserved for the mission
		 * pack": a broken or partial install would serve files to the
		 * mod anyway, re-scanning the same rejected pak and printing
		 * its warning a second time.  A mod that wanted mission pack
		 * assets does without them, which is what an H2MP build already
		 * does after the same rollback.  uhexen2-ofgb. */
		if (i < com_argc - 1)
			FS_Gamedir (com_argv[i+1]);
	}
}

#define	FS_NUM_BUFFS	4
#define	FS_BUFFERLEN	1024

static char *get_fs_buffer(void)
{
	static char fs_buffers[FS_NUM_BUFFS][FS_BUFFERLEN];
	static int buffer_idx = 0;
	buffer_idx = (buffer_idx + 1) & (FS_NUM_BUFFS - 1);
	return fs_buffers[buffer_idx];
}

static int init_MakePath (int base, char *buf, size_t siz)
{
	int	len;

	switch (base)
	{
	case FS_USERDIR:
		len = q_strlcpy(buf, fs_userdir, siz);
		break;
	case FS_GAMEDIR:
		len = q_strlcpy(buf, fs_gamedir, siz);
		break;
	case FS_USERBASE:
		len = q_strlcpy(buf, host_parms->userdir, siz);
		break;
	case FS_BASEDIR:
		len = q_strlcpy(buf, fs_basedir, siz);
		break;
	default:
		Sys_Error ("%s: Bad FS_BASE", __thisfunc__);
		return -1;
	}

	if (len >= (int)siz - 1)
		return -1;
	if (len && !IS_DIR_SEPARATOR(buf[len - 1]))
		buf[len++] = DIR_SEPARATOR_CHAR;
	buf[len] = '\0';
	return len;
}

static char *do_MakePath (int base, int *error, char *buf, size_t siz, const char *path)
{
	int	len;

	len = init_MakePath(base, buf, siz);
	if (len < 0) goto _bad;

	len = q_strlcat(buf, path, siz);
	if (len < (int)siz) {
		if (error) *error = 0;
	} else {
	_bad:
		if (error) *error = 1;
		Con_DPrintf("%s: overflow (string truncated)\n", __thisfunc__);
	}

	return buf;
}

static char *do_MakePath_VA (int base, int *error, char *buf, size_t siz,
					const char *format, va_list args)
{
	int	len, ret;

	len = init_MakePath(base, buf, siz);
	if (len < 0) goto _bad;

	ret = q_vsnprintf(&buf[len], siz - len, format, args);
	if (ret < (int)siz - len) {
		if (error) *error = 0;
	} else {
	_bad:
		if (error) *error = 1;
		Con_DPrintf("%s: overflow (string truncated)\n", __thisfunc__);
	}

	return buf;
}

static char *FSERR_MakePath_BUF (const char *caller, int linenum, int base,
				 char *buf, size_t siz, const char *path)
{
	int	err;
	char	*p;

	p = do_MakePath(base, &err, buf, siz, path);

	if (err) Sys_Error("%s: %d: string buffer overflow!", caller, linenum);
	return p;
}

static char *FSERR_MakePath_VABUF (const char *caller, int linenum, int base,
				char *buf, size_t siz, const char *format, ...)
{
	va_list	argptr;
	int	err;
	char	*p;

	va_start (argptr, format);
	p = do_MakePath_VA(base, &err, buf, siz, format, argptr);
	va_end (argptr);

	if (err) Sys_Error("%s: %d: string buffer overflow!", caller, linenum);
	return p;
}

char *FS_MakePath (int base, int *error, const char *path)
{
	return do_MakePath(base, error, get_fs_buffer(), FS_BUFFERLEN, path);
}

char *FS_MakePath_BUF (int base, int *error, char *buf, size_t siz, const char *path)
{
	return do_MakePath(base, error, buf, siz, path);
}

char *FS_MakePath_VA (int base, int *error, const char *format, ...)
{
	va_list	argptr;
	char	*p;

	va_start (argptr, format);
	p = do_MakePath_VA(base, error, get_fs_buffer(), FS_BUFFERLEN, format, argptr);
	va_end (argptr);

	return p;
}

char *FS_MakePath_VABUF (int base, int *error, char *buf, size_t siz, const char *format, ...)
{
	va_list	argptr;
	char	*p;

	va_start (argptr, format);
	p = do_MakePath_VA(base, error, buf, siz, format, argptr);
	va_end (argptr);

	return p;
}

const char *FS_GetBasedir (void)
{
	return fs_basedir;
}

const char *FS_GetUserbase (void)
{
	return host_parms->userdir;
}

const char *FS_GetGamedir (void)
{
	return fs_gamedir;
}

const char *FS_GetUserdir (void)
{
	return fs_userdir;
}

unsigned int FS_GetPortalsPathID (void)
{
	return fs_portals_path_id;
}

/* The following FS_*() stdio replacements are necessary if one is
 * to perform non-sequential reads on files reopened on pak files
 * because we need the bookkeeping about file start/end positions.
 * Allocating and filling in the fshandle_t structure is the users'
 * responsibility when the file is initially opened. */

size_t FS_fread(void *ptr, size_t size, size_t nmemb, fshandle_t *fh)
{
	long byte_size;
	long bytes_read;
	size_t nmemb_read;

	if (!fh) {
		errno = EBADF;
		return 0;
	}
	if (!ptr) {
		errno = EFAULT;
		return 0;
	}
	if (!size || !nmemb) {	/* no error, just zero bytes wanted */
		errno = 0;
		return 0;
	}

	byte_size = nmemb * size;
	if (byte_size > fh->length - fh->pos)	/* just read to end */
		byte_size = fh->length - fh->pos;
	if (fh->data) {		/* inflated archive entry, already in memory */
		memcpy(ptr, fh->data + fh->pos, (size_t)byte_size);
		bytes_read = byte_size;
	} else {
		bytes_read = fread(ptr, 1, byte_size, fh->file);
	}
	fh->pos += bytes_read;

	/* fread() must return the number of elements read,
	 * not the total number of bytes. */
	nmemb_read = bytes_read / size;
	/* even if the last member is only read partially
	 * it is counted as a whole in the return value. */
	if (bytes_read % size)
		nmemb_read++;

	return nmemb_read;
}

int FS_fseek(fshandle_t *fh, long offset, int whence)
{
/* I don't care about 64 bit off_t or fseeko() here.
 * the quake/hexen2 file system is 32 bits, anyway. */
	int ret;

	if (!fh) {
		errno = EBADF;
		return -1;
	}

	/* the relative file position shouldn't be smaller
	 * than zero or bigger than the filesize. */
	switch (whence)
	{
	case SEEK_SET:
		break;
	case SEEK_CUR:
		offset += fh->pos;
		break;
	case SEEK_END:
		offset = fh->length + offset;
		break;
	default:
		errno = EINVAL;
		return -1;
	}

	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}

	if (offset > fh->length)	/* just seek to end */
		offset = fh->length;

	if (!fh->data) {
		ret = fseek(fh->file, fh->start + offset, SEEK_SET);
		if (ret < 0)
			return ret;
	}

	fh->pos = offset;
	return 0;
}

int FS_fclose(fshandle_t *fh)
{
	if (!fh) {
		errno = EBADF;
		return -1;
	}
	if (fh->data) {
		free(fh->data);
		fh->data = NULL;
		return 0;
	}
	return fclose(fh->file);
}

long FS_ftell(fshandle_t *fh)
{
	if (!fh) {
		errno = EBADF;
		return -1;
	}
	return fh->pos;
}

void FS_rewind(fshandle_t *fh)
{
	if (!fh) return;
	if (!fh->data) {
		clearerr(fh->file);
		fseek(fh->file, fh->start, SEEK_SET);
	}
	fh->pos = 0;
}

int FS_feof(fshandle_t *fh)
{
	if (!fh) {
		errno = EBADF;
		return -1;
	}
	if (fh->pos >= fh->length)
		return -1;
	return 0;
}

int FS_ferror(fshandle_t *fh)
{
	if (!fh) {
		errno = EBADF;
		return -1;
	}
	if (fh->data)	/* a memory-backed handle has no stream to fault */
		return 0;
	return ferror(fh->file);
}

int FS_fgetc(fshandle_t *fh)
{
	if (!fh) {
		errno = EBADF;
		return EOF;
	}
	if (fh->pos >= fh->length)
		return EOF;
	if (fh->data)
		return (int) fh->data[fh->pos++];
	fh->pos += 1;
	return fgetc(fh->file);
}

char *FS_fgets(char *s, int size, fshandle_t *fh)
{
	char *ret;

	if (FS_feof(fh))
		return NULL;

	if (size > (fh->length - fh->pos) + 1)
		size = (fh->length - fh->pos) + 1;

	if (fh->data) {
		/* same contract as fgets(): copy up to and including the first
		 * newline, stop at size-1 bytes, always NUL-terminate. */
		int i = 0;

		if (size < 2)
			return NULL;
		while (i < size - 1 && fh->pos < fh->length) {
			s[i] = (char) fh->data[fh->pos++];
			if (s[i++] == '\n')
				break;
		}
		s[i] = '\0';
		return i ? s : NULL;
	}

	ret = fgets(s, size, fh->file);
	fh->pos = ftell(fh->file) - fh->start;

	return ret;
}

long FS_filelength (fshandle_t *fh)
{
	if (!fh) {
		errno = EBADF;
		return -1;
	}
	return fh->length;
}

