/* sys_win.c -- Windows system interface code
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
#include "winquake.h"
#include "sys_sdl.h"
#include "debuglog.h"


#define MIN_MEM_ALLOC	0x4000000	/* 64 mb */
#define STD_MEM_ALLOC	0x20000000	/* 512 mb */

#define CONSOLE_ERROR_TIMEOUT	60.0

cvar_t		sys_nostdout = {"sys_nostdout", "0", CVAR_NONE};
cvar_t		sys_throttle = {"sys_throttle", "0.02", CVAR_ARCHIVE};

qboolean		isDedicated;

static qboolean		sc_return_on_enter = false;
static HANDLE		hinput, houtput;


/*
===============================================================================

FILE IO

===============================================================================
*/

/* Convert UTF-8 path to UTF-16 for Windows API calls */
static wchar_t *Sys_UTF8ToWide (const char *utf8path)
{
	static wchar_t widepath[MAX_PATH];
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8path, -1, widepath, MAX_PATH);
	if (len == 0)
		return NULL;
	return widepath;
}

/* Convert UTF-16 filename to UTF-8 for return to engine */
static const char *Sys_WideToUTF8 (const wchar_t *widename)
{
	static char utf8name[MAX_OSPATH];
	int len = WideCharToMultiByte(CP_UTF8, 0, widename, -1, utf8name, sizeof(utf8name), NULL, NULL);
	if (len == 0)
		return NULL;
	return utf8name;
}

int Sys_mkdir (const char *path, qboolean crash)
{
	wchar_t *widepath = Sys_UTF8ToWide(path);
	if (!widepath)
	{
		if (crash)
			Sys_Error("Unable to convert path to UTF-16: %s", path);
		return -1;
	}
	if (CreateDirectoryW(widepath, NULL) != 0)
		return 0;
	if (GetLastError() == ERROR_ALREADY_EXISTS)
		return 0;
	if (crash)
		Sys_Error("Unable to create directory %s", path);
	return -1;
}

int Sys_rmdir (const char *path)
{
	wchar_t *widepath = Sys_UTF8ToWide(path);
	if (!widepath)
		return -1;
	if (RemoveDirectoryW(widepath) != 0)
		return 0;
	return -1;
}

int Sys_unlink (const char *path)
{
	wchar_t *widepath = Sys_UTF8ToWide(path);
	if (!widepath)
		return -1;
	if (DeleteFileW(widepath) != 0)
		return 0;
	return -1;
}

int Sys_rename (const char *oldp, const char *newp)
{
	wchar_t *wideoldp = Sys_UTF8ToWide(oldp);
	wchar_t *widenewp = Sys_UTF8ToWide(newp);
	if (!wideoldp || !widenewp)
		return -1;
	if (MoveFileW(wideoldp, widenewp) != 0)
		return 0;
	return -1;
}

long Sys_filesize (const char *path)
{
	HANDLE fh;
	WIN32_FIND_DATAW data;
	long size;
	wchar_t *widepath = Sys_UTF8ToWide(path);

	if (!widepath)
		return -1;
	fh = FindFirstFileW(widepath, &data);
	if (fh == INVALID_HANDLE_VALUE)
		return -1;
	FindClose(fh);
	if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return -1;
	size = (long) data.nFileSizeLow;
	return size;
}

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES	((DWORD)-1)
#endif
int Sys_FileType (const char *path)
{
	wchar_t *widepath = Sys_UTF8ToWide(path);
	DWORD result;

	if (!widepath)
		return FS_ENT_NONE;
	result = GetFileAttributesW(widepath);

	if (result == INVALID_FILE_ATTRIBUTES)
		return FS_ENT_NONE;
	if (result & FILE_ATTRIBUTE_DIRECTORY)
		return FS_ENT_DIRECTORY;

	return FS_ENT_FILE;
}

int Sys_CopyFile (const char *frompath, const char *topath)
{
	wchar_t *widefrom = Sys_UTF8ToWide(frompath);
	wchar_t *wideto = Sys_UTF8ToWide(topath);

	if (!widefrom || !wideto)
		return -1;
	if (CopyFileW(widefrom, wideto, FALSE) != 0)
		return 0;
	return -1;
}

/*
=================================================
simplified findfirst/findnext implementation
=================================================
*/
static HANDLE findhandle = INVALID_HANDLE_VALUE;
static WIN32_FIND_DATAW finddata;
static wchar_t	findstr[MAX_OSPATH];

const char *Sys_FindFirstFile (const char *path, const char *pattern)
{
	wchar_t *widepath = Sys_UTF8ToWide(path);
	if (findhandle != INVALID_HANDLE_VALUE)
		Sys_Error ("Sys_FindFirst without FindClose");
	if (!widepath)
		return NULL;
	q_swprintf (findstr, sizeof(findstr)/sizeof(wchar_t), L"%s/%hs", widepath, pattern);
	findhandle = FindFirstFileW(findstr, &finddata);
	if (findhandle == INVALID_HANDLE_VALUE)
		return NULL;
	if (finddata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return Sys_FindNextFile();
	return Sys_WideToUTF8(finddata.cFileName);
}

const char *Sys_FindNextFile (void)
{
	if (findhandle == INVALID_HANDLE_VALUE)
		return NULL;
	while (FindNextFileW(findhandle, &finddata) != 0)
	{
		if (finddata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		return Sys_WideToUTF8(finddata.cFileName);
	}
	return NULL;
}

void Sys_FindClose (void)
{
	if (findhandle != INVALID_HANDLE_VALUE)
	{
		FindClose(findhandle);
		findhandle = INVALID_HANDLE_VALUE;
	}
}


int Sys_ListDirectories (const char *path, char dirs[][64], int maxdirs)
{
	WIN32_FIND_DATAW	fdata;
	HANDLE		fh;
	wchar_t		searchstr[MAX_OSPATH];
	wchar_t		*widepath;
	const char	*utf8name;
	int		count = 0;

	widepath = Sys_UTF8ToWide(path);
	if (!widepath)
		return 0;
	q_swprintf(searchstr, sizeof(searchstr)/sizeof(wchar_t), L"%s/*", widepath);
	fh = FindFirstFileW(searchstr, &fdata);
	if (fh == INVALID_HANDLE_VALUE)
		return 0;

	do
	{
		if (!(fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		if (fdata.cFileName[0] == L'.')
			continue;	/* skip ".", ".." and hidden dirs */
		utf8name = Sys_WideToUTF8(fdata.cFileName);
		if (utf8name)
			q_strlcpy(dirs[count], utf8name, 64);
		count++;
	} while (FindNextFileW(fh, &fdata) != 0 && count < maxdirs);

	FindClose(fh);
	return count;
}


/*
===============================================================================

SYSTEM IO

===============================================================================
*/

#if id386 && !defined(GLQUAKE)
void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length)
{
	DWORD	flOldProtect;
	if (!VirtualProtect((LPVOID)startaddr, length, PAGE_EXECUTE_READWRITE, &flOldProtect))
		Sys_Error("Protection change failed\n");
}
#endif	/* id386, !GLQUAKE */


#define ERROR_PREFIX	"\nFATAL ERROR: "
void Sys_Error (const char *error, ...)
{
	va_list		argptr;
	char		text[MAX_PRINTMSG];
	const char	text2[] = ERROR_PREFIX;
	const char	text3[] = "\n";
	const char	text4[] = "\nPress Enter to exit\n";
	DWORD		dummy;
	double		err_begin;

	host_parms->errstate++;

	va_start (argptr, error);
	q_vsnprintf (text, sizeof (text), error, argptr);
	va_end (argptr);

	if (con_debuglog)
	{
		LOG_Print (ERROR_PREFIX);
		LOG_Print (text);
		LOG_Print ("\n\n");
	}

	Host_Shutdown ();

	if (isDedicated)
	{
		WriteFile (houtput, text2, strlen(text2), &dummy, NULL);
		WriteFile (houtput, text,  strlen(text),  &dummy, NULL);
		WriteFile (houtput, text3, strlen(text3), &dummy, NULL);
		WriteFile (houtput, text4, strlen(text4), &dummy, NULL);

		err_begin = Sys_DoubleTime ();
		sc_return_on_enter = true;
		while (!Sys_ConsoleInput () &&
			((Sys_DoubleTime () - err_begin) < CONSOLE_ERROR_TIMEOUT))
		{
		}
	}
	else
	{
		MessageBox(NULL, text, ENGINE_NAME " Error", MB_OK | MB_SETFOREGROUND | MB_ICONSTOP);
	}

	exit (1);
}

void Sys_PrintTerm (const char *msgtxt)
{
	DWORD		dummy;

	if (isDedicated)
	{
		if (sys_nostdout.integer)
			return;

		WriteFile(houtput, msgtxt, strlen(msgtxt), &dummy, NULL);
	}
}

void Sys_Quit (void)
{
	Host_Shutdown();

	if (isDedicated)
		FreeConsole ();

	exit (0);
}


const char *Sys_ConsoleInput (void)
{
	static char	con_text[256];
	static int	textlen;
	INPUT_RECORD	recs[1024];
	int		ch;
	DWORD		dummy, numread, numevents;

	for ( ;; )
	{
		if (GetNumberOfConsoleInputEvents(hinput, &numevents) == 0)
			Sys_Error ("Error getting # of console events");

		if (! numevents)
			break;

		if (ReadConsoleInput(hinput, recs, 1, &numread) == 0)
			Sys_Error ("Error reading console input");

		if (numread != 1)
			Sys_Error ("Couldn't read console input");

		if (recs[0].EventType == KEY_EVENT)
		{
		    if (recs[0].Event.KeyEvent.bKeyDown == FALSE)
		    {
			ch = recs[0].Event.KeyEvent.uChar.AsciiChar;

			switch (ch)
			{
			case '\r':
				WriteFile(houtput, "\r\n", 2, &dummy, NULL);
				if (textlen != 0)
				{
					con_text[textlen] = 0;
					textlen = 0;
					return con_text;
				}
				else if (sc_return_on_enter)
				{
					con_text[0] = '\r';
					textlen = 0;
					return con_text;
				}

				break;

			case '\b':
				WriteFile(houtput, "\b \b", 3, &dummy, NULL);
				if (textlen != 0)
					textlen--;

				break;

			default:
				if (ch >= ' ')
				{
					WriteFile(houtput, &ch, 1, &dummy, NULL);
					con_text[textlen] = ch;
					textlen = (textlen + 1) & 0xff;
				}

				break;
			}
		    }
		}
	}

	return NULL;
}


static int Sys_GetBasedir (char *argv0, char *dst, size_t dstsize)
{
	char *tmp;
	size_t rc;

	rc = GetCurrentDirectory(dstsize, dst);
	if (rc == 0 || rc > dstsize)
		return -1;

	tmp = dst;
	while (*tmp != 0)
		tmp++;
	while (*tmp == 0 && tmp != dst)
	{
		--tmp;
		if (tmp != dst && (*tmp == '/' || *tmp == '\\'))
			*tmp = 0;
	}

	return 0;
}

/*
==============================================================================

 WINDOWS CRAP

==============================================================================
*/

static void PrintVersion (void)
{
	Sys_Printf ("Hexenwail %s (%s)\n", HW_VERSION, PLATFORM_STRING);
	Sys_Printf ("based on Hexen II engine %4.2f / Hammer of Thyrion 1.5.10\n", ENGINE_VERSION);
}

/*
==================
WinMain
==================
*/
static char	*argv[MAX_NUM_ARGVS];
static char	cwd[1024];
static char	prog[MAX_PATH];
static quakeparms_t	parms;

int WINAPI WinMain (HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	int		i;

	/* previous instances do not exist in Win32 */
	if (hPrevInstance)
		return 0;

	memset (&parms, 0, sizeof(parms));
	parms.basedir = cwd;
	parms.userdir = cwd;	/* no userdir on win32 */
	parms.errstate = 0;
	host_parms = &parms;

	memset (cwd, 0, sizeof(cwd));
	if (Sys_GetBasedir(NULL, cwd, sizeof(cwd)) != 0)
		Sys_Error ("Couldn't determine current directory");

	parms.argc = 1;
	argv[0] = prog;
	if (GetModuleFileName(NULL, prog, sizeof(prog)) == 0)
		prog[0] = '\0';
	else	prog[MAX_PATH - 1] = '\0';

	while (*lpCmdLine && (parms.argc < MAX_NUM_ARGVS))
	{
		while (*lpCmdLine && ((*lpCmdLine <= 32) || (*lpCmdLine > 126)))
			lpCmdLine++;

		if (*lpCmdLine)
		{
			argv[parms.argc] = lpCmdLine;
			parms.argc++;

			while (*lpCmdLine && ((*lpCmdLine > 32) && (*lpCmdLine <= 126)))
				lpCmdLine++;

			if (*lpCmdLine)
			{
				*lpCmdLine = 0;
				lpCmdLine++;
			}
		}
	}

	parms.argv = argv;

	isDedicated = (COM_CheckParm ("-dedicated") != 0);
	if (isDedicated)
	{
		if (!AllocConsole ())
		{
			isDedicated = false;
			Sys_Error ("Couldn't create dedicated server console");
		}
		hinput = GetStdHandle (STD_INPUT_HANDLE);
		houtput = GetStdHandle (STD_OUTPUT_HANDLE);
		if (hinput  == INVALID_HANDLE_VALUE ||
		    houtput == INVALID_HANDLE_VALUE ||
		    hinput  == NULL || houtput == NULL)
		{
			isDedicated = false;
			Sys_Error ("Couldn't retrieve server console handles");
		}

		PrintVersion();
	}

	LOG_Init (&parms);

	Sys_Printf("basedir is: %s\n", parms.basedir);
	Sys_Printf("userdir is: %s\n", parms.userdir);

	COM_ValidateByteorder ();

	Sys_CheckSDL ();

	parms.memsize = (isDedicated)? MIN_MEM_ALLOC : STD_MEM_ALLOC;
	i = COM_CheckParm ("-heapsize");
	if (i && i < com_argc-1)
		parms.memsize = atoi (com_argv[i+1]) * 1024;

	parms.membase = malloc (parms.memsize);
	if (!parms.membase)
		Sys_Error ("Insufficient memory.");

	Sys_SDLInit ();
	atexit (Sys_SDLShutdown);

	Host_Init();

	Sys_MainLoop ();

	return 0;
}
