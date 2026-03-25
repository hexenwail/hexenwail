/* pr_csqc.c -- CSQC builtin functions (Ironwail-compatible subset)
 *
 * These builtins are available only to the CSQC VM (csprogs.dat).
 * They provide 2D drawing primitives and stat/player info access
 * so mods can replace the HUD with QuakeC-drawn overlays.
 *
 * Builtin numbering matches Ironwail/DarkPlaces for QC compatibility.
 *
 * Copyright (C) 2025  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"

#if !defined(SERVERONLY)

/*
==================
Helper: clamp float to 0..1 range
==================
*/
static float clampf01 (float f)
{
	if (f < 0) return 0;
	if (f > 1) return 1;
	return f;
}


/* ---- Drawing builtins ---- */

/*
==================
PF_csqc_drawpic -- #322
void drawpic(vector position, string pic, vector size, vector rgb, float alpha, float flag)
==================
*/
static void PF_csqc_drawpic (void)
{
	float	*pos = G_VECTOR(OFS_PARM0);
	const char *picname = G_STRING(OFS_PARM1);
	float	*size = G_VECTOR(OFS_PARM2);
	/*float	*rgb = G_VECTOR(OFS_PARM3);*/
	float	alpha = G_FLOAT(OFS_PARM4);
	qpic_t	*pic;

	(void)size; /* TODO: scaling support */

	pic = Draw_CachePic (picname);
	if (!pic)
		return;

	if (alpha >= 1.0f)
		Draw_Pic ((int)pos[0], (int)pos[1], pic);
	else
		Draw_AlphaPic ((int)pos[0], (int)pos[1], pic, clampf01(alpha));
}


/*
==================
PF_csqc_drawstring -- #326
float drawstring(vector position, string text, vector scale, vector rgb, float alpha, float flag)
==================
*/
static void PF_csqc_drawstring (void)
{
	float	*pos = G_VECTOR(OFS_PARM0);
	const char *text = G_STRING(OFS_PARM1);
	/*float	*scale = G_VECTOR(OFS_PARM2);*/
	/*float	*rgb = G_VECTOR(OFS_PARM3);*/
	/*float	alpha = G_FLOAT(OFS_PARM4);*/

	Draw_String ((int)pos[0], (int)pos[1], text);
	G_FLOAT(OFS_RETURN) = 1;
}


/*
==================
PF_csqc_drawrawstring -- #321
float drawrawstring(vector position, string text, vector scale, vector rgb, float alpha, float flag)
==================
*/
static void PF_csqc_drawrawstring (void)
{
	/* identical to drawstring for now — charset markup not implemented */
	PF_csqc_drawstring ();
}


/*
==================
PF_csqc_drawcharacter -- #320
float drawcharacter(vector position, float character, vector scale, vector rgb, float alpha, float flag)
==================
*/
static void PF_csqc_drawcharacter (void)
{
	float	*pos = G_VECTOR(OFS_PARM0);
	int	chr = (int)G_FLOAT(OFS_PARM1);

	Draw_Character ((int)pos[0], (int)pos[1], chr);
	G_FLOAT(OFS_RETURN) = 1;
}


/*
==================
PF_csqc_drawfill -- #323
void drawfill(vector position, vector size, vector rgb, float alpha, float flag)
==================
*/
static void PF_csqc_drawfill (void)
{
	float	*pos = G_VECTOR(OFS_PARM0);
	float	*size = G_VECTOR(OFS_PARM1);
	float	*rgb = G_VECTOR(OFS_PARM2);
	/*float	alpha = G_FLOAT(OFS_PARM3);*/

	/* Draw_Fill takes a palette index — approximate from RGB */
	int color = 0;
	if (rgb[0] > 0.5f)	color = 251;	/* red-ish */
	else if (rgb[1] > 0.5f)	color = 184;	/* green-ish */
	else if (rgb[2] > 0.5f)	color = 208;	/* blue-ish */
	else			color = 0;	/* black */

	Draw_Fill ((int)pos[0], (int)pos[1], (int)size[0], (int)size[1], color);
}


/*
==================
PF_csqc_drawsetcliparea -- #324
float drawsetcliparea(float x, float y, float width, float height)
==================
*/
static void PF_csqc_drawsetcliparea (void)
{
	float x = G_FLOAT(OFS_PARM0);
	float y = G_FLOAT(OFS_PARM1);
	float w = G_FLOAT(OFS_PARM2);
	float h = G_FLOAT(OFS_PARM3);

	glEnable (GL_SCISSOR_TEST);
	glScissor ((int)x, (int)(vid.height - y - h), (int)w, (int)h);
	G_FLOAT(OFS_RETURN) = 0;
}


/*
==================
PF_csqc_drawresetcliparea -- #325
void drawresetcliparea(void)
==================
*/
static void PF_csqc_drawresetcliparea (void)
{
	glDisable (GL_SCISSOR_TEST);
}


/*
==================
PF_csqc_drawgetimagesize -- #318
vector drawgetimagesize(string pic)
==================
*/
static void PF_csqc_drawgetimagesize (void)
{
	const char *picname = G_STRING(OFS_PARM0);
	qpic_t *pic;

	pic = Draw_CachePic (picname);
	if (pic)
	{
		G_FLOAT(OFS_RETURN + 0) = pic->width;
		G_FLOAT(OFS_RETURN + 1) = pic->height;
		G_FLOAT(OFS_RETURN + 2) = 0;
	}
	else
	{
		G_FLOAT(OFS_RETURN + 0) = 0;
		G_FLOAT(OFS_RETURN + 1) = 0;
		G_FLOAT(OFS_RETURN + 2) = 0;
	}
}


/*
==================
PF_csqc_stringwidth -- #327
float stringwidth(string text, float usecolors, vector scale)
==================
*/
static void PF_csqc_stringwidth (void)
{
	const char *text = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = strlen(text) * 8;	/* 8 pixels per char */
}


/*
==================
PF_csqc_precache_pic -- #317
string precache_pic(string pic)
==================
*/
static void PF_csqc_precache_pic (void)
{
	const char *picname = G_STRING(OFS_PARM0);
	Draw_CachePic (picname);
	G_INT(OFS_RETURN) = G_INT(OFS_PARM0);	/* return same string */
}


/*
==================
PF_csqc_iscachedpic -- #316
float iscachedpic(string pic)
==================
*/
static void PF_csqc_iscachedpic (void)
{
	const char *picname = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = (Draw_CachePic(picname) != NULL) ? 1 : 0;
}


/*
==================
PF_csqc_drawsubpic -- #328
void drawsubpic(vector pos, vector size, string pic, vector srcpos, vector srcsize, vector rgb, float alpha, float flag)
==================
*/
static void PF_csqc_drawsubpic (void)
{
	float	*pos = G_VECTOR(OFS_PARM0);
	/*float	*size = G_VECTOR(OFS_PARM1);*/
	const char *picname = G_STRING(OFS_PARM2);
	float	*srcpos = G_VECTOR(OFS_PARM3);
	float	*srcsize = G_VECTOR(OFS_PARM4);
	qpic_t	*pic;

	pic = Draw_CachePic (picname);
	if (!pic)
		return;

	Draw_SubPic ((int)pos[0], (int)pos[1], pic,
		     (int)(srcpos[0] * pic->width),
		     (int)(srcpos[1] * pic->height),
		     (int)(srcsize[0] * pic->width),
		     (int)(srcsize[1] * pic->height));
}


/* ---- Stat access builtins ---- */

/*
==================
PF_csqc_getstati -- #330
float getstati(float index)
==================
*/
static void PF_csqc_getstati (void)
{
	int idx = (int)G_FLOAT(OFS_PARM0);
	if (idx < 0 || idx >= MAX_CL_STATS)
	{
		G_FLOAT(OFS_RETURN) = 0;
		return;
	}
	G_FLOAT(OFS_RETURN) = cl.stats[idx];
}


/*
==================
PF_csqc_getstatf -- #331
float getstatf(float index)
==================
*/
static void PF_csqc_getstatf (void)
{
	int idx = (int)G_FLOAT(OFS_PARM0);
	if (idx < 0 || idx >= MAX_CL_STATS)
	{
		G_FLOAT(OFS_RETURN) = 0;
		return;
	}
	/* reinterpret int bits as float */
	union { int i; float f; } u;
	u.i = cl.stats[idx];
	G_FLOAT(OFS_RETURN) = u.f;
}


/*
==================
PF_csqc_getstats -- #332
string getstats(float index)
==================
*/
static void PF_csqc_getstats (void)
{
	/* Hexen II doesn't use string stats — return empty */
	G_INT(OFS_RETURN) = PR_SetEngineString ("");
}


/* ---- Player info builtins ---- */

/*
==================
PF_csqc_getplayerkeyvalue -- #348
string getplayerkeyvalue(float playernum, string key)
==================
*/
static void PF_csqc_getplayerkeyvalue (void)
{
	int		pnum = (int)G_FLOAT(OFS_PARM0);
	const char	*key = G_STRING(OFS_PARM1);

	if (pnum < 0 || pnum >= cl.maxclients)
	{
		G_INT(OFS_RETURN) = PR_SetEngineString ("");
		return;
	}

	if (!strcmp(key, "name"))
		G_INT(OFS_RETURN) = PR_SetEngineString (cl.scores[pnum].name);
	else if (!strcmp(key, "frags"))
	{
		static char buf[16];
		q_snprintf (buf, sizeof(buf), "%d", cl.scores[pnum].frags);
		G_INT(OFS_RETURN) = PR_SetEngineString (buf);
	}
	else if (!strcmp(key, "colors"))
	{
		static char buf[16];
		q_snprintf (buf, sizeof(buf), "%d", cl.scores[pnum].colors);
		G_INT(OFS_RETURN) = PR_SetEngineString (buf);
	}
	else if (!strcmp(key, "playerclass"))
	{
		static char buf[16];
		q_snprintf (buf, sizeof(buf), "%g", cl.scores[pnum].playerclass);
		G_INT(OFS_RETURN) = PR_SetEngineString (buf);
	}
	else
		G_INT(OFS_RETURN) = PR_SetEngineString ("");
}


/* ---- Misc builtins ---- */

/*
==================
PF_csqc_print -- #1
void print(string ...)
==================
*/
static void PF_csqc_print (void)
{
	int i;
	for (i = 0; i < pr_argc; i++)
		Con_Printf ("%s", G_STRING(OFS_PARM0 + i * 3));
}


/*
==================
PF_csqc_ftos -- #26
string ftos(float f)
==================
*/
static void PF_csqc_ftos (void)
{
	static char buf[64];
	float v = G_FLOAT(OFS_PARM0);
	if (v == (int)v)
		q_snprintf (buf, sizeof(buf), "%d", (int)v);
	else
		q_snprintf (buf, sizeof(buf), "%g", v);
	G_INT(OFS_RETURN) = PR_SetEngineString (buf);
}


/*
==================
PF_csqc_vtos -- #27
string vtos(vector v)
==================
*/
static void PF_csqc_vtos (void)
{
	static char buf[128];
	float *v = G_VECTOR(OFS_PARM0);
	q_snprintf (buf, sizeof(buf), "'%5.1f %5.1f %5.1f'", v[0], v[1], v[2]);
	G_INT(OFS_RETURN) = PR_SetEngineString (buf);
}


/*
==================
PF_csqc_cvar -- #45
float cvar(string name)
==================
*/
static void PF_csqc_cvar (void)
{
	const char *name = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = Cvar_VariableValue (name);
}


/*
==================
PF_csqc_localcmd -- #46
void localcmd(string cmd)
==================
*/
static void PF_csqc_localcmd (void)
{
	const char *cmd = G_STRING(OFS_PARM0);
	Cbuf_AddText (cmd);
}


/*
==================
PF_csqc_strlen_bi -- #114 (distinct from the C stdlib strlen)
float strlen(string s)
==================
*/
static void PF_csqc_strlen_bi (void)
{
	const char *s = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = (float)strlen(s);
}


/*
==================
PF_csqc_strcat -- #115
string strcat(string s1, string s2)
==================
*/
static void PF_csqc_strcat (void)
{
	static char buf[1024];
	const char *s1 = G_STRING(OFS_PARM0);
	const char *s2 = G_STRING(OFS_PARM1);
	q_snprintf (buf, sizeof(buf), "%s%s", s1, s2);
	G_INT(OFS_RETURN) = PR_SetEngineString (buf);
}


/*
==================
PF_csqc_substring -- #116
string substring(string s, float start, float length)
==================
*/
static void PF_csqc_substring (void)
{
	static char buf[1024];
	const char *s = G_STRING(OFS_PARM0);
	int start = (int)G_FLOAT(OFS_PARM1);
	int len = (int)G_FLOAT(OFS_PARM2);
	int slen = (int)strlen(s);

	if (start < 0) start = 0;
	if (start >= slen) { buf[0] = 0; G_INT(OFS_RETURN) = PR_SetEngineString(buf); return; }
	if (len < 0 || start + len > slen) len = slen - start;
	if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;

	memcpy (buf, s + start, len);
	buf[len] = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString (buf);
}


static void PF_Fixme (void)
{
	PR_RunError ("unimplemented CSQC builtin");
}

/*
 * Builtin table for CSQC.  Indices match Ironwail/DarkPlaces numbering.
 * Slots 0..315 are mostly NULL (server builtins), filled with PF_Fixme.
 * We allocate enough slots to cover the highest numbered builtin we need.
 */

#define CSQC_MAX_BUILTINS 400

static builtin_t csqc_builtin_table[CSQC_MAX_BUILTINS];

const builtin_t *csqc_builtins = csqc_builtin_table;
const int csqc_numbuiltins = CSQC_MAX_BUILTINS;

/*
==================
CSQC_InitBuiltins

Fill the CSQC builtin table.  Called once at engine init.
==================
*/
void CSQC_InitBuiltins (void)
{
	int i;

	for (i = 0; i < CSQC_MAX_BUILTINS; i++)
		csqc_builtin_table[i] = PF_Fixme;

	/* basic utilities */
	csqc_builtin_table[1] = PF_csqc_print;		/* print */
	csqc_builtin_table[26] = PF_csqc_ftos;		/* ftos */
	csqc_builtin_table[27] = PF_csqc_vtos;		/* vtos */
	csqc_builtin_table[45] = PF_csqc_cvar;		/* cvar */
	csqc_builtin_table[46] = PF_csqc_localcmd;	/* localcmd */
	csqc_builtin_table[114] = PF_csqc_strlen_bi;	/* strlen */
	csqc_builtin_table[115] = PF_csqc_strcat;	/* strcat */
	csqc_builtin_table[116] = PF_csqc_substring;	/* substring */

	/* drawing builtins (Ironwail/DP numbering) */
	csqc_builtin_table[316] = PF_csqc_iscachedpic;
	csqc_builtin_table[317] = PF_csqc_precache_pic;
	csqc_builtin_table[318] = PF_csqc_drawgetimagesize;
	csqc_builtin_table[320] = PF_csqc_drawcharacter;
	csqc_builtin_table[321] = PF_csqc_drawrawstring;
	csqc_builtin_table[322] = PF_csqc_drawpic;
	csqc_builtin_table[323] = PF_csqc_drawfill;
	csqc_builtin_table[324] = PF_csqc_drawsetcliparea;
	csqc_builtin_table[325] = PF_csqc_drawresetcliparea;
	csqc_builtin_table[326] = PF_csqc_drawstring;
	csqc_builtin_table[327] = PF_csqc_stringwidth;
	csqc_builtin_table[328] = PF_csqc_drawsubpic;

	/* stat access */
	csqc_builtin_table[330] = PF_csqc_getstati;
	csqc_builtin_table[331] = PF_csqc_getstatf;
	csqc_builtin_table[332] = PF_csqc_getstats;

	/* player info */
	csqc_builtin_table[348] = PF_csqc_getplayerkeyvalue;
}

#endif /* !SERVERONLY */
