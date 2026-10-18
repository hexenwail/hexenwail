/*
 * sv_move.c -- monster movement
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
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

/* The monster movement itself -- SV_CheckBottom, SV_movestep,
 * SV_StepDirection, SV_FixCheckBottom, SV_NewChaseDir, SV_CloseEnough and
 * SV_MoveToGoal -- is engine/rust/src/sv_move.rs (#455); the C original is
 * engine/rust/sv_move/tests/sv_move_h2.c.  What stays here is the
 * sv_debugmovestep diagnostics, which that port calls: they are variadic,
 * and Rust can call a C-variadic function but not define one. */

/* sv_debugmovestep N: log the next N refused monster steps, with the reason.
 * sv_debugmovestep -N: the same N-line budget, and also one line for EVERY
 * SV_CheckBottom call -- pass or fail, which path decided it, the corner box it
 * sampled, the entity's bbox and .hull.  Refusals alone cannot tell "never
 * called" from "always passed" (village3 calls SV_CheckBottom 331 times and
 * refuses nothing), and the corner box is what #278 needed a throwaway build
 * to read.  GitHub #282.
 *
 * Set a negative value from the console or a cfg, not as +sv_debugmovestep
 * -N on the command line: there the "-N" token ends the + command, so the
 * cvar never receives it.
 *
 * Setting the value it already holds does not refill a spent budget (the cvar
 * code skips the callback when the string is unchanged): set 0 first.  Call
 * lines include every SV_CheckBottom caller, not just SV_movestep -- the
 * SV_FixCheckBottom path, sliding corpses in SV_Physics_Step and the QC
 * checkbottom builtin draw on the same budget. */
cvar_t	sv_debugmovestep = {"sv_debugmovestep", "0", CVAR_NONE};
static int sv_debugmovestep_remaining;
static qboolean sv_debugmovestep_calls;

void SV_DebugMoveStep_Changed (cvar_t *var)
{
	/* Parsed from the string, in the integer domain, not from var->value:
	 * the engine builds with -ffast-math, which deletes a float NaN test,
	 * and "nan" then clamped to -1e9 and turned call logging ON.  strtol
	 * reads "nan" (and any junk) as 0, stops at a fraction or exponent, and
	 * the clamp keeps -v defined. */
	long	v = strtol (var->string, NULL, 10);

	if (v > 1000000000L)
		v = 1000000000L;
	else if (v < -1000000000L)
		v = -1000000000L;

	sv_debugmovestep_calls = (v < 0);
	sv_debugmovestep_remaining = (int)((v < 0) ? -v : v);
}

void SV_DebugMoveStep_Log (const edict_t *ent, const trace_t *trace, const char *fmt, ...)
{
	char	buf[256];
	const char *touched = "";
	va_list	argptr;

	if (sv_debugmovestep_remaining <= 0)
		return;
	sv_debugmovestep_remaining--;

	va_start (argptr, fmt);
	q_vsnprintf (buf, sizeof(buf), fmt, argptr);
	va_end (argptr);

	if (trace && trace->ent && trace->ent != sv.edicts)
		touched = PR_GetString (trace->ent->v.classname);

	Con_Printf ("sv_debugmovestep: %s [%s @ %.0f %.0f %.0f]%s%s\n",
		buf,
		PR_GetString (ent->v.classname),
		ent->v.origin[0], ent->v.origin[1], ent->v.origin[2],
		touched[0] ? " touched=" : "",
		touched[0] ? touched : "");
}

#define	DEBUGMOVESTEP_MOVER_RADIUS	256

/*
=============
SV_DebugMoveStep_DumpMovers

Dumps every non-world SOLID_BSP brush entity whose bounding box comes within
DEBUGMOVESTEP_MOVER_RADIUS of the failing foot position. The real absmin/absmax
is the only reliable way to tell a walkable platform from a shaft-filling
elevator car, so a monster refusing to step "onto a train" can be judged
without reading the BSP submodel lump (uhexen2-nnwb).
=============
*/
void SV_DebugMoveStep_DumpMovers (const vec3_t foot)
{
	edict_t	*check;
	int	i, j;

	if (sv_debugmovestep_remaining <= 0)
		return;

	check = NEXT_EDICT(sv.edicts);
	for (i = 1; i < sv.num_edicts; i++, check = NEXT_EDICT(check))
	{
		if (check->free)
			continue;
		if (check->v.solid != SOLID_BSP)
			continue;

		for (j = 0; j < 3; j++)
		{
			if (foot[j] < check->v.absmin[j] - DEBUGMOVESTEP_MOVER_RADIUS ||
			    foot[j] > check->v.absmax[j] + DEBUGMOVESTEP_MOVER_RADIUS)
				break;
		}
		if (j != 3)
			continue;

		if (sv_debugmovestep_remaining <= 0)
			return;
		sv_debugmovestep_remaining--;

		Con_Printf ("sv_debugmovestep:   mover %s org=%.0f %.0f %.0f abs=[%.0f %.0f %.0f]..[%.0f %.0f %.0f] | foot=%.0f %.0f %.0f\n",
			PR_GetString (check->v.classname),
			check->v.origin[0], check->v.origin[1], check->v.origin[2],
			check->v.absmin[0], check->v.absmin[1], check->v.absmin[2],
			check->v.absmax[0], check->v.absmax[1], check->v.absmax[2],
			foot[0], foot[1], foot[2]);
	}
}

/*
=============
SV_DebugMoveStep_Call

One line per SV_CheckBottom call under a negative sv_debugmovestep: the
answer, which test decided it, the absolute corner box the function sampled
(mins/maxs as it computed them), the entity's own bbox and its .hull.  Every
return in SV_CheckBottom goes through here; engine/tests/sv_checkbottom_test.c
fails if one does not.
=============
*/
qboolean SV_DebugMoveStep_Call (const edict_t *ent, const vec3_t mins,
				const vec3_t maxs, qboolean ok,
				const char *why)
{
	if (sv_debugmovestep_calls)
	{
		SV_DebugMoveStep_Log (ent, NULL,
			"CheckBottom call: %s, %s box=[%.1f %.1f %.1f]..[%.1f %.1f %.1f]"
			" bbox=[%.0f %.0f %.0f]..[%.0f %.0f %.0f] hull=%.0f",
			ok ? "pass" : "FAIL", why,
			mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2],
			ent->v.mins[0], ent->v.mins[1], ent->v.mins[2],
			ent->v.maxs[0], ent->v.maxs[1], ent->v.maxs[2],
			ent->v.hull);
	}
	return ok;
}
