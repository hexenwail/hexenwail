/*
 * HexenWorld signon file downloads.  Kept in its own translation unit so the
 * wire-facing half can be driven by scripts/hw_download_test.c, which supplies
 * its own stubs for the engine symbols below -- every path here parses data a
 * remote server chose, and a cooperative server never exercises the rejections.
 *
 * Textually included by cl_hw.c (the bgmnull.c / cd_sdl.c pattern) rather than
 * linked separately: the precache name tables and HWCL_LoadModels it calls are
 * file-static there, and publishing them to keep one subsystem testable would
 * widen more than it narrows.
 */

/*
 * Signon file downloads, ported from the original HexenWorld client's
 * CL_CheckOrDownloadFile / Sound_NextDownload / Model_NextDownload.
 *
 * Each precache list is walked before it is loaded; the first missing file
 * is requested with "download", hwsv answers with svc_download blocks that we
 * pull one at a time with "nextdl", and the walk resumes after the rename.
 * Files land in the current gamedir's userdir, which FS_AddGameDirectory put
 * on the searchpath as a loose directory, so the renamed file is found by the
 * very next lookup.
 */
/* hwsv serves one 1024-byte block per request and never states a total, so
 * without a ceiling a hostile server could fill the disk. */
#define HWCL_DOWNLOAD_MAX_BYTES	(64L * 1024L * 1024L)

typedef enum
{
	hwcl_dl_none,
	hwcl_dl_sound,
	hwcl_dl_model
} hwcl_dltype_t;

static hwcl_dltype_t hwcl_download_type;
static int hwcl_download_number;
static FILE *hwcl_download;
static char hwcl_download_name[MAX_QPATH];
static char hwcl_download_tempname[MAX_QPATH + 4];
static long hwcl_download_bytes;

static void HWCL_RequestNextDownload (void);

/* The name comes off the wire and becomes a path under the userdir. */
static qboolean HWCL_DownloadNameIsSafe (const char *name)
{
	static const char *const devices[] = { "con", "prn", "aux", "nul" };
	const unsigned char *p;
	const char *comp, *end;
	size_t len, baselen;
	int i;

	if (!name[0] || strlen (name) >= MAX_QPATH)
		return false;
	if (strstr (name, "..") || name[0] == '/' || name[0] == '.')
		return false;
	/* '\\' is refused anywhere, not just leading: it is a separator on
	 * Windows.  The rest are invalid in Windows file names. */
	for (p = (const unsigned char *)name; *p; p++)
	{
		if (*p < 0x20 || *p == 0x7f || strchr ("\\:<>\"|?*", *p))
			return false;
	}
	/* hwsv demands a subdirectory too; a bare name would land in the
	 * gamedir root beside config.cfg and the paks. */
	if (!strchr (name, '/'))
		return false;

	for (comp = name; ; comp = end + 1)
	{
		end = strchr (comp, '/');
		len = end ? (size_t)(end - comp) : strlen (comp);
		/* Windows silently strips a trailing '.' or ' ', so the name we
		 * checked would not be the file that gets opened. */
		if (len == 0 || comp[len - 1] == '.' || comp[len - 1] == ' ')
			return false;
		/* Device names open the device whatever the extension ("nul.bsp"),
		 * and Windows ignores spaces before that extension. */
		for (baselen = 0; baselen < len && comp[baselen] != '.'; baselen++)
			;
		while (baselen > 0 && comp[baselen - 1] == ' ')
			baselen--;
		if (baselen == 3)
		{
			for (i = 0; i < (int)(sizeof(devices) / sizeof(devices[0])); i++)
			{
				if (!q_strncasecmp (comp, devices[i], 3))
					return false;
			}
		}
		else if (baselen == 4 && comp[3] >= '1' && comp[3] <= '9' &&
				(!q_strncasecmp (comp, "com", 3) ||
				 !q_strncasecmp (comp, "lpt", 3)))
			return false;
		if (!end)
			break;
	}
	return true;
}

static qboolean HWCL_DownloadPath (char *buf, size_t size, const char *name)
{
	int err = 0;

	FS_MakePath_BUF (FS_USERDIR, &err, buf, size, name);
	return !err;
}

/* Close and delete a partial download.  Safe to call with nothing active. */
static void HWCL_CancelDownload (void)
{
	char path[MAX_OSPATH];

	if (hwcl_download)
	{
		fclose (hwcl_download);
		hwcl_download = NULL;
		if (HWCL_DownloadPath (path, sizeof(path), hwcl_download_tempname))
			Sys_unlink (path);
	}
	hwcl_download_type = hwcl_dl_none;
	hwcl_download_number = 0;
	hwcl_download_bytes = 0;
	hwcl_download_name[0] = 0;
	hwcl_download_tempname[0] = 0;
}

/* Drop the current file (partial or refused) and keep the list walk alive. */
static void HWCL_AbandonCurrentDownload (void)
{
	char path[MAX_OSPATH];

	if (hwcl_download)
	{
		fclose (hwcl_download);
		hwcl_download = NULL;
		if (HWCL_DownloadPath (path, sizeof(path), hwcl_download_tempname))
			Sys_unlink (path);
	}
	hwcl_download_bytes = 0;
	hwcl_download_name[0] = 0;
	hwcl_download_tempname[0] = 0;
}

/* True when the file is present (or must not be fetched); false once a
 * download has been requested, in which case the caller waits for it. */
static qboolean HWCL_CheckOrDownloadFile (const char *filename)
{
	if (!HWCL_DownloadNameIsSafe (filename))
	{
		Con_Printf ("Refusing to download unsafe path %s\n", filename);
		return true;
	}
	if (FS_FileExists (filename, NULL))
		return true;

	q_strlcpy (hwcl_download_name, filename, sizeof(hwcl_download_name));
	q_snprintf (hwcl_download_tempname, sizeof(hwcl_download_tempname),
			"%s.tmp", filename);
	hwcl_download_bytes = 0;
	Con_Printf ("Downloading %s...\n", hwcl_download_name);
	/* A plaque held over a transfer of unknown length only runs into the
	 * 25 second "load timeout"; show the console progress instead. */
	SCR_EndLoadingPlaque ();
	HWCL_StringCmd (va ("download %s", hwcl_download_name));
	return false;
}

static void HWCL_SoundNextDownload (void)
{
	if (hwcl_download_number == 0)
		hwcl_download_number = 1;
	hwcl_download_type = hwcl_dl_sound;
	for ( ; hwcl_download_number < hwcl_sound_count; hwcl_download_number++)
	{
		if (!hwcl_sound_names[hwcl_download_number][0])
			continue;
		if (!HWCL_CheckOrDownloadFile (va ("sound/%s",
				hwcl_sound_names[hwcl_download_number])))
		{
			hwcl_download_number++;	/* resume after this one */
			return;
		}
	}

	HWCL_CancelDownload ();
	HWCL_LoadSounds ();
	HWCL_StringCmd (va ("modellist %d 0", hwcl_servercount));
}

static void HWCL_ModelNextDownload (void)
{
	const char *name;

	if (hwcl_download_number == 0)
		hwcl_download_number = 1;
	hwcl_download_type = hwcl_dl_model;
	for ( ; hwcl_download_number < hwcl_model_count; hwcl_download_number++)
	{
		name = hwcl_model_names[hwcl_download_number];
		if (!name[0] || name[0] == '*')
			continue;	/* empty slot or inline brush model */
		if (!HWCL_CheckOrDownloadFile (name))
		{
			hwcl_download_number++;
			return;
		}
	}

	HWCL_CancelDownload ();
	if (!HWCL_LoadModels ())
	{
		/* Everything else is optional (a portals-less install is legal), but
		 * signon with no world leaves nothing to draw or collide against. */
		Con_Printf ("\nHexenWorld: the map %s could not be found or "
				"downloaded (gamedir %s).  Disconnecting.\n"
				"Install hw/pak4.pak and the server's mod assets under %s (or %s).\n\n",
				hwcl_model_names[1][0] ? hwcl_model_names[1] : "(none)",
				fs_gamedir_nopath, FS_GetBasedir (), FS_GetUserbase ());
		HWCL_Disconnect ();
		return;
	}
	Con_Printf ("HexenWorld precache lists received; requesting signon.\n");
	HWCL_StringCmd (va ("prespawn %d 0", hwcl_servercount));
}

static void HWCL_RequestNextDownload (void)
{
	switch (hwcl_download_type)
	{
	case hwcl_dl_sound:
		HWCL_SoundNextDownload ();
		break;
	case hwcl_dl_model:
		HWCL_ModelNextDownload ();
		break;
	case hwcl_dl_none:
	default:
		break;
	}
}

static void HWCL_ParseDownload (void)
{
	char path[MAX_OSPATH];
	char final_path[MAX_OSPATH];
	int size;
	int percent;

	size = MSG_ReadShort ();
	percent = MSG_ReadByte ();
	if (msg_badread)
		return;

	if (size < 0)
	{
		/* -1: hwsv refused or lacks the file (maps inside a pak always
		 * are).  No payload follows. */
		if (hwcl_download_type == hwcl_dl_none || !hwcl_download_name[0])
			return;
		Con_Printf ("Server cannot send %s.\n", hwcl_download_name);
		HWCL_AbandonCurrentDownload ();
		HWCL_RequestNextDownload ();
		return;
	}

	if (size > hw_net_message.cursize - msg_readcount)
	{
		Con_Printf ("HexenWorld download block overruns its packet.\n");
		HWCL_Disconnect ();
		return;
	}

	/* Unsolicited block: consume it and write nothing. */
	if (hwcl_download_type == hwcl_dl_none || !hwcl_download_name[0])
	{
		msg_readcount += size;
		return;
	}

	/* hwsv only sends an empty block for an empty file, and an empty file
	 * is no usable model, sound or map. */
	if (size == 0)
	{
		Con_Printf ("Server sent an empty %s; skipping it.\n",
				hwcl_download_name);
		HWCL_AbandonCurrentDownload ();
		HWCL_RequestNextDownload ();
		return;
	}

	if (!hwcl_download)
	{
		if (!HWCL_DownloadPath (path, sizeof(path), hwcl_download_tempname) ||
				FS_CreatePath (path) != 0 ||
				!(hwcl_download = fopen (path, "wb")))
		{
			msg_readcount += size;
			Con_Printf ("Unable to open %s for writing.\n",
					hwcl_download_tempname);
			HWCL_AbandonCurrentDownload ();
			HWCL_RequestNextDownload ();
			return;
		}
	}

	if (hwcl_download_bytes + size > HWCL_DOWNLOAD_MAX_BYTES)
	{
		msg_readcount += size;
		Con_Printf ("%s exceeds the %ld MB download limit; abandoned.\n",
				hwcl_download_name, HWCL_DOWNLOAD_MAX_BYTES / (1024L * 1024L));
		HWCL_AbandonCurrentDownload ();
		HWCL_RequestNextDownload ();
		return;
	}

	if (fwrite (hw_net_message.data + msg_readcount, 1, size, hwcl_download)
			!= (size_t)size)
	{
		msg_readcount += size;
		Con_Printf ("Write error while downloading %s.\n", hwcl_download_name);
		HWCL_AbandonCurrentDownload ();
		HWCL_RequestNextDownload ();
		return;
	}
	msg_readcount += size;
	hwcl_download_bytes += size;

	/* Exactly 100, as the original client tested.  hwsv computes percent as
	 * int downloadcount*100/size, which overflows past ~21.4 MB and sends
	 * values like 185 mid-file; treating >= 100 as done renamed a truncated
	 * file into place. */
	if (percent != 100)
	{
		Con_DPrintf ("%s: %d%%\n", hwcl_download_name, percent);
		HWCL_StringCmd ("nextdl");
		return;
	}

	fclose (hwcl_download);
	hwcl_download = NULL;
	if (!HWCL_DownloadPath (path, sizeof(path), hwcl_download_tempname) ||
			!HWCL_DownloadPath (final_path, sizeof(final_path),
				hwcl_download_name))
	{
		Con_Printf ("Download path for %s is too long.\n", hwcl_download_name);
	}
	/* rename() replaces on POSIX and Sys_rename replaces on Windows too, so
	 * test first: a file that appeared while we downloaded is never
	 * clobbered. */
	else if (Sys_FileType (final_path) != FS_ENT_NONE)
	{
		Con_Printf ("%s already exists; discarding the download.\n",
				hwcl_download_name);
		Sys_unlink (path);
	}
	else if (Sys_rename (path, final_path) != 0)
	{
		Con_Printf ("Failed to rename %s.\n", hwcl_download_tempname);
		Sys_unlink (path);
	}
	hwcl_download_bytes = 0;
	hwcl_download_name[0] = 0;
	hwcl_download_tempname[0] = 0;
	HWCL_RequestNextDownload ();
}
