/* cl_csqc.c -- Client-Side QuakeC: loading, entry points, VM switching
 *
 * Ironwail-style HUD-only CSQC. The CSQC VM runs csprogs.dat and
 * provides draw builtins so mods can replace the status bar with
 * QuakeC-drawn overlays.
 *
 * The server VM and CSQC VM never execute concurrently. We use a
 * save/restore mechanism: before calling CSQC, save the server progs
 * state, load the CSQC state, run, then restore.
 *
 * Copyright (C) 2025  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "cl_csqc.h"

qboolean	csqc_active;
cvar_t		cl_nocsqc = {"cl_nocsqc", "0", CVAR_ARCHIVE};

/* CSQC progs state — loaded separately from the server VM */
static pr_vmstate_t	csqc_state;
static pr_vmstate_t	sv_saved;
static qboolean		in_csqc;

/* CSQC entry point function indices */
static func_t		csqc_fn_init;
static func_t		csqc_fn_shutdown;
static func_t		csqc_fn_drawhud;
static func_t		csqc_fn_drawscores;


/*
==================
CSQC_SwitchToCSQC / CSQC_SwitchToServer
==================
*/
static void CSQC_SwitchToCSQC (void)
{
	if (in_csqc)
		return;

	/* save server VM state */
	PR_SaveVMState (&sv_saved);

	/* load CSQC VM state */
	PR_RestoreVMState (&csqc_state);

	/* clear sv_globals — CSQC doesn't use server global mapping */
	memset (&sv_globals, 0, sizeof(sv_globals));

	/* reset execution context */
	pr_xfunction = NULL;
	pr_xstatement = 0;
	pr_argc = 0;
	pr_trace = false;

	in_csqc = true;
}

static void CSQC_SwitchToServer (void)
{
	if (!in_csqc)
		return;

	/* save CSQC state back (string table may have been modified) */
	PR_SaveVMState (&csqc_state);

	/* restore server VM state */
	PR_RestoreVMState (&sv_saved);

	in_csqc = false;
}


/*
==================
CL_LoadCSProgs

Load csprogs.dat after signon is complete.
==================
*/
void CL_LoadCSProgs (void)
{
	byte		*buf;
	dprograms_t	*p;
	dfunction_t	*fn;
	int		i;

	CL_ShutdownCSProgs ();

	if (cl_nocsqc.value)
	{
		Con_DPrintf ("CSQC disabled by cl_nocsqc\n");
		return;
	}

	buf = (byte *)FS_LoadHunkFile ("csprogs.dat", NULL);
	if (!buf)
	{
		Con_DPrintf ("CSQC: csprogs.dat not found\n");
		return;
	}

	p = (dprograms_t *)buf;

	/* byte swap header */
	for (i = 0; i < (int)sizeof(*p) / 4; i++)
		((int *)p)[i] = LittleLong (((int *)p)[i]);

	if (p->version != PROG_VERSION_V6 && p->version != PROG_VERSION_V7)
	{
		Con_Printf ("CSQC: csprogs.dat has unsupported version %d\n", p->version);
		return;
	}

	/* set up the CSQC VM state */
	memset (&csqc_state, 0, sizeof(csqc_state));
	csqc_state.progs = p;
	csqc_state.is_v6 = (p->version == PROG_VERSION_V6);
	csqc_state.crc = CRC_Block (buf, fs_filesize);

	csqc_state.functions = (dfunction_t *)((byte *)p + p->ofs_functions);
	csqc_state.strings = (char *)p + p->ofs_strings;
	csqc_state.stringssize = p->numstrings;
	csqc_state.globals = (float *)((byte *)p + p->ofs_globals);

	csqc_state.edict_size = p->entityfields * 4 + sizeof(edict_t) - sizeof(entvars_t);
	csqc_state.edict_size += sizeof(void *) - 1;
	csqc_state.edict_size &= ~(sizeof(void *) - 1);

	if (csqc_state.is_v6)
	{
		/* V6 progs need conversion — use the existing converter via
		 * a temporary switch to the CSQC state.  For simplicity, we
		 * only support V7 csprogs.dat for now. */
		Con_Printf ("CSQC: csprogs.dat is v6 format (unsupported, use v7)\n");
		memset (&csqc_state, 0, sizeof(csqc_state));
		return;
	}

	csqc_state.globaldefs = (ddef_t *)((byte *)p + p->ofs_globaldefs);
	csqc_state.fielddefs = (ddef_t *)((byte *)p + p->ofs_fielddefs);
	csqc_state.statements = (dstatement_t *)((byte *)p + p->ofs_statements);

	/* byte swap lumps */
	for (i = 0; i < p->numfunctions; i++)
	{
		csqc_state.functions[i].first_statement = LittleLong (csqc_state.functions[i].first_statement);
		csqc_state.functions[i].parm_start = LittleLong (csqc_state.functions[i].parm_start);
		csqc_state.functions[i].s_name = LittleLong (csqc_state.functions[i].s_name);
		csqc_state.functions[i].s_file = LittleLong (csqc_state.functions[i].s_file);
		csqc_state.functions[i].numparms = LittleLong (csqc_state.functions[i].numparms);
		csqc_state.functions[i].locals = LittleLong (csqc_state.functions[i].locals);
	}
	for (i = 0; i < p->numstatements; i++)
	{
		csqc_state.statements[i].op = LittleShort (csqc_state.statements[i].op);
		csqc_state.statements[i].a = LittleLong (csqc_state.statements[i].a);
		csqc_state.statements[i].b = LittleLong (csqc_state.statements[i].b);
		csqc_state.statements[i].c = LittleLong (csqc_state.statements[i].c);
	}
	for (i = 0; i < p->numglobaldefs; i++)
	{
		csqc_state.globaldefs[i].type = LittleShort (csqc_state.globaldefs[i].type);
		csqc_state.globaldefs[i].ofs = LittleLong (csqc_state.globaldefs[i].ofs);
		csqc_state.globaldefs[i].s_name = LittleLong (csqc_state.globaldefs[i].s_name);
	}
	for (i = 0; i < p->numfielddefs; i++)
	{
		csqc_state.fielddefs[i].type = LittleShort (csqc_state.fielddefs[i].type);
		csqc_state.fielddefs[i].ofs = LittleLong (csqc_state.fielddefs[i].ofs);
		csqc_state.fielddefs[i].s_name = LittleLong (csqc_state.fielddefs[i].s_name);
	}
	for (i = 0; i < p->numglobals; i++)
		((int *)csqc_state.globals)[i] = LittleLong (((int *)csqc_state.globals)[i]);

	/* install CSQC builtins */
	csqc_state.builtins = csqc_builtins;
	csqc_state.numbuiltins = csqc_numbuiltins;

	/* find CSQC entry points by scanning function names */
	csqc_fn_init = csqc_fn_shutdown = csqc_fn_drawhud = csqc_fn_drawscores = 0;

	for (i = 1; i < p->numfunctions; i++)
	{
		fn = &csqc_state.functions[i];
		const char *name = csqc_state.strings + fn->s_name;
		if (!strcmp(name, "CSQC_Init"))
			csqc_fn_init = (func_t)i;
		else if (!strcmp(name, "CSQC_Shutdown"))
			csqc_fn_shutdown = (func_t)i;
		else if (!strcmp(name, "CSQC_DrawHud"))
			csqc_fn_drawhud = (func_t)i;
		else if (!strcmp(name, "CSQC_DrawScores"))
			csqc_fn_drawscores = (func_t)i;
	}

	if (!csqc_fn_drawhud)
	{
		Con_Printf ("CSQC: csprogs.dat has no CSQC_DrawHud, ignoring\n");
		memset (&csqc_state, 0, sizeof(csqc_state));
		return;
	}

	csqc_active = true;
	Con_Printf ("CSQC: loaded csprogs.dat (%d functions)\n", p->numfunctions);

	/* call CSQC_Init if present */
	if (csqc_fn_init)
	{
		CSQC_SwitchToCSQC ();
		G_FLOAT(OFS_PARM0) = 0;	/* apilevel */
		G_INT(OFS_PARM1) = PR_SetEngineString (ENGINE_NAME);
		G_FLOAT(OFS_PARM2) = ENGINE_VERSION;
		pr_argc = 3;
		PR_ExecuteProgram (csqc_fn_init, "CSQC_Init");
		CSQC_SwitchToServer ();
	}
}


/*
==================
CL_ShutdownCSProgs
==================
*/
void CL_ShutdownCSProgs (void)
{
	if (!csqc_active)
		return;

	if (csqc_fn_shutdown)
	{
		CSQC_SwitchToCSQC ();
		pr_argc = 0;
		PR_ExecuteProgram (csqc_fn_shutdown, "CSQC_Shutdown");
		CSQC_SwitchToServer ();
	}

	if (csqc_state.knownstrings)
		Z_Free ((void *)csqc_state.knownstrings);

	memset (&csqc_state, 0, sizeof(csqc_state));
	csqc_fn_init = csqc_fn_shutdown = csqc_fn_drawhud = csqc_fn_drawscores = 0;
	csqc_active = false;
}


/*
==================
CSQC_RegisterCvars
==================
*/
void CSQC_RegisterCvars (void)
{
	Cvar_RegisterVariable (&cl_nocsqc);
}


/*
==================
CSQC_DrawHud

Called instead of Sbar_Draw when CSQC is active.
Returns true if CSQC handled the HUD.
==================
*/
qboolean CSQC_DrawHud (void)
{
	if (!csqc_active || !csqc_fn_drawhud)
		return false;

	CSQC_SwitchToCSQC ();

	/* CSQC_DrawHud(vector virtsize, float showscores) */
	G_FLOAT(OFS_PARM0) = vid.width;
	G_FLOAT(OFS_PARM0 + 1) = vid.height;
	G_FLOAT(OFS_PARM0 + 2) = 0;
	G_FLOAT(OFS_PARM1) = 0;	/* showscores */
	pr_argc = 2;

	PR_ExecuteProgram (csqc_fn_drawhud, "CSQC_DrawHud");

	CSQC_SwitchToServer ();
	return true;
}


/*
==================
CSQC_DrawScores

Called when showscores is active or during intermission.
Returns true if CSQC handled the scores overlay.
==================
*/
qboolean CSQC_DrawScores (void)
{
	if (!csqc_active || !csqc_fn_drawscores)
		return false;

	CSQC_SwitchToCSQC ();

	G_FLOAT(OFS_PARM0) = vid.width;
	G_FLOAT(OFS_PARM0 + 1) = vid.height;
	G_FLOAT(OFS_PARM0 + 2) = 0;
	G_FLOAT(OFS_PARM1) = 1;	/* showscores */
	pr_argc = 2;

	PR_ExecuteProgram (csqc_fn_drawscores, "CSQC_DrawScores");

	CSQC_SwitchToServer ();
	return true;
}
