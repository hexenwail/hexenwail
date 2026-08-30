/* userdir.h -- arch specific user directory definitions
 *
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

#ifndef __USERDIR_H
#define __USERDIR_H

#if defined(DEMOBUILD)
/* use a different user directory for the demo version,
 * so that the demo and retail versions can co-exist on
 * the same machine peacefully */
#define	SYS_USERDIR_OSX		"Library/Application Support/Hexen2 Demo"
#define	SYS_USERDIR_UNIX	".hexen2demo"
#define	SYS_USERDIR_XDG		"hexen2demo"
#define	SYS_USERDIR_HAIKU	"config/settings/hexen2demo"

#else	/* for retail version: */

#define	SYS_USERDIR_OSX		"Library/Application Support/Hexen2"
#define	SYS_USERDIR_UNIX	".hexen2"
#define	SYS_USERDIR_XDG		"hexen2"
#define	SYS_USERDIR_HAIKU	"config/settings/hexen2"

#endif


#if defined(PLATFORM_OSX)
#define	AOT_USERDIR		SYS_USERDIR_OSX
#elif defined(PLATFORM_HAIKU)
#define	AOT_USERDIR		SYS_USERDIR_HAIKU
#else	/* unix: */
#define	AOT_USERDIR		SYS_USERDIR_UNIX
/* Plain unix follows the XDG Base Directory spec: a fresh install lands in
 * $XDG_DATA_HOME/AOT_USERDIR_XDG instead of cluttering $HOME.  OS X and Haiku
 * have their own conventions and are deliberately left out -- this header is
 * the only place the platform split happens, sys_unix.c carries no platform
 * ifdefs of its own.  An already existing $HOME/AOT_USERDIR still wins at
 * runtime; see Sys_GetUserdir(). */
#define	USE_XDG_USERDIR		1
#define	AOT_USERDIR_XDG		SYS_USERDIR_XDG
#endif

#endif	/* __USERDIR_H */

