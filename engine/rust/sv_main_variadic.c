/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * sv_main_variadic.c -- the part of engine/hexen2/sv_main.c's map_checks
 * readout that cannot be written in stable Rust, and nothing else.
 *
 * SV_PrintMapCheck is C-variadic, and Rust cannot *define* a C-variadic
 * function on stable (`c_variadic' is unstable), so it stays C, as
 * sv_debugmovestep's helpers did in #463 and FS_MakePath_VA did in
 * quakefs_variadic.c.  Its only caller, SV_PrintMapChecklist, is
 * engine/rust/src/sv_main_spawn.rs (E2 / slice E of #469), which calls it as
 * a C variadic.  The body is sv_main.c's, moved verbatim; mapcheck_t moved to
 * sv_main_target.h with it.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2026 Hexenwail contributors.
 *
 * Not in sv_main_target.c, which holds facts and accessors only; compiled
 * into the same targets, the ones that link sv_main_sound.
 */

#include "quakedef.h"
#include "sv_main_target.h"

/*
================
SV_PrintMapCheck
================
*/
void SV_PrintMapCheck (mapcheck_t status, const char *format, ...)
{
	char		str[1024];
	va_list		argptr;
	char		mark;

	va_start (argptr, format);
	q_vsnprintf (str, sizeof(str), format, argptr);
	va_end (argptr);

	switch (status)
	{
	case MAPCHECK_OK:	mark = 'x'; break;
	case MAPCHECK_NA:	mark = '-'; break;
	case MAPCHECK_PARTIAL:	mark = '~'; break;
	default:		mark = ' '; break;
	}

	Con_SafePrintf ("[%c] %s\n", mark, str);

	if (status == MAPCHECK_FAILED || status == MAPCHECK_PARTIAL)
		sv.mapchecks.numwarnings++;
}
