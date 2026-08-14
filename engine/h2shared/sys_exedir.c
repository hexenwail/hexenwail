/* sys_exedir.c -- locate the directory the running executable lives in
 *
 * basedir is getcwd()/GetCurrentDirectory(), i.e. the directory the game was
 * launched *from*, which for a desktop launcher, a Steam shortcut or a file
 * manager double-click is frequently not the install directory.  Anything
 * that ships beside the binary has to be found without going through basedir.
 *
 * macOS caveat for whoever packages this: inside a .app bundle the executable
 * lives in Foo.app/Contents/MacOS/, so that is what this returns, and anything
 * layered on it (docs/BUNDLED_GAMECODE.md F5) resolves inside the bundle
 * rather than beside the .app.  For a plain CLI build -- Homebrew, MacPorts,
 * nix, a local ./configure -- that is exactly right.  For a real bundle the
 * payload has to be placed under Contents/, not next to the .app; this
 * function is not the place to special-case it.
 *
 * Copyright (C) 2026  uHexen2 contributors
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

#if defined(PLATFORM_WINDOWS)
#include <windows.h>	/* GetModuleFileName: h2ded gets nothing from glheader.h */
#define EXEDIR_SUPPORTED	1
#elif defined(__APPLE__)
#include <mach-o/dyld.h>	/* _NSGetExecutablePath */
#include <stdlib.h>		/* realpath */
#define EXEDIR_SUPPORTED	1
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <unistd.h>	/* readlink */
#define EXEDIR_SUPPORTED	1
#endif

const char *Sys_GetExeDir (void)
{
#ifdef EXEDIR_SUPPORTED
	static char	exedir[MAX_OSPATH];
	static qboolean	resolved = false;
	static qboolean	valid = false;
	char		*slash;

	if (resolved)
		return valid ? exedir : NULL;
	resolved = true;

#if defined(PLATFORM_WINDOWS)
	{
		/* ANSI form, matching Sys_GetBasedir's GetCurrentDirectory: the
		 * two must agree on encoding or paths built from them won't. */
		DWORD len = GetModuleFileName(NULL, exedir, sizeof(exedir));
		/* On truncation this returns nSize (pre-Vista) or nSize with
		 * ERROR_INSUFFICIENT_BUFFER (Vista+); both fail the same way. */
		if (len == 0 || len >= sizeof(exedir))
			return NULL;
	}
	slash = strrchr(exedir, '\\');
	if (!slash)
		slash = strrchr(exedir, '/');
#elif defined(__APPLE__)
	{
		char		raw[MAX_OSPATH];
		uint32_t	rawsize = (uint32_t) sizeof(raw);

		/* Returns -1 and overwrites rawsize with the size needed when the
		 * buffer is too small.  A path that does not fit MAX_OSPATH is one
		 * nothing else in the engine could hold either, so a short buffer
		 * is a hard no rather than something to retry larger. */
		if (_NSGetExecutablePath(raw, &rawsize) != 0)
			return NULL;

		/* What comes back is whatever argv[0] resolution produced: it may be
		 * relative, may contain . or .., and may run through symlinks --
		 * Homebrew and MacPorts both install the binary as one.  Apple's own
		 * documentation says to pass it through realpath, and the truncation
		 * below needs a real directory to cut.  exedir is MAX_OSPATH, which
		 * is PATH_MAX on this platform, which is realpath's contract. */
		if (!realpath(raw, exedir))
			return NULL;
	}
	slash = strrchr(exedir, '/');
#else
	{
		ssize_t len = readlink("/proc/self/exe", exedir, sizeof(exedir) - 1);
		if (len <= 0 || len >= (ssize_t)sizeof(exedir) - 1)
			return NULL;
		exedir[len] = '\0';
	}
	slash = strrchr(exedir, '/');
#endif

	if (!slash)
		return NULL;
	*slash = '\0';
	valid = true;
	return exedir;
#else
	return NULL;
#endif	/* EXEDIR_SUPPORTED */
}
