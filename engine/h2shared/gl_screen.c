/*
 * screen.c -- master for refresh, status bar, console, chat, notify, etc
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

/*=============================================================================

	background clear
	rendering
	turtle/net/ram icons
	sbar
	centerprint / slow centerprint
	notify lines
	intermission / finale overlay
	loading plaque
	console
	menu

	required background clears
	required update regions

	syncronous draw mode or async
	One off screen buffer, with updates either copied or xblited
	Need to double buffer?

	async draw will require the refresh area to be cleared, because
	it will be xblited, but sync draw can just ignore it.

	sync
	draw

	CenterPrint ()
	SlowPrint ()
	Screen_Update ();
	Con_Printf ();

	net
	turn off messages option

	the refresh is always rendered, unless the console is full screen

	console is:
		notify lines
		half
		full

=============================================================================*/

#include "quakedef.h"
#include "gl_postprocess.h"
#include "gl_shader.h"
#include "gl_pipeline.h"
#include "gl_vbo.h"
#include "draw.h"
#include "image.h"
#if !defined(SERVERONLY) && !defined(H2W)
#include "cl_csqc.h"
#endif
#include <time.h>
#ifdef PLATFORM_WINDOWS
#include "winquake.h"
#endif

static qboolean	scr_initialized;	// ready to draw

vrect_t		scr_vrect;
int		glx, gly, glwidth, glheight;

/* these are only functional in the software renderer */
int		scr_copytop;		// only the refresh window will be updated
int		scr_copyeverything;	// unless these variables are flagged
int		scr_topupdate;
int		scr_fullupdate;

static int	clearconsole;
int		clearnotify;

float		scr_con_current;
float		scr_conlines;		// lines of console to display

int		trans_level = 0;

/* scr_demobar_timeout -- Ironwail (Quake/gl_screen.c, same slot in this block).
 * Negative hides the demo playback bar entirely, 0 pins it up for the whole
 * demo, positive is how many seconds it stays up after the last playback
 * event before it fades out.  See SCR_DrawDemoBar. */
cvar_t		scr_demobar_timeout = {"scr_demobar_timeout", "1", CVAR_ARCHIVE};
cvar_t		scr_viewsize = {"viewsize", "110", CVAR_ARCHIVE};
cvar_t		scr_fov = {"fov", "90", CVAR_NONE};	// 10 - 170
cvar_t		scr_fov_adapt = {"fov_adapt", "1", CVAR_ARCHIVE};	// "Hor+" scaling

/* Read-only mirrors of the field of view the engine actually renders with,
 * published for gamecode: cvar("fov_effective") / cvar("fov_effective_y").
 *
 * QC could previously only read "fov", which is the unadapted 4:3 value.  The
 * renderer runs it through AdaptFovx for the display aspect and blends it
 * toward scr_zoomfov while zooming, and neither result was visible to a mod.
 * Anything placed at a fixed distance to cover a fixed slice of the screen was
 * therefore tuned for one aspect ratio and wrong everywhere else -- Shadows of
 * Chaos puts its pop-up menus at eye + v_forward * (25.5 - 0.15 * fov), which
 * fills the width exactly on 16:9 and overflows by 18% on 4:3.  uhexen2-vsxi
 *
 * Deliberately not named fov_adapted: one character from the fov_adapt toggle,
 * and a mod that reached for the wrong one would silently get 1 instead of a
 * field of view. */
cvar_t		scr_fov_effective = {"fov_effective", "90", CVAR_ROM};
cvar_t		scr_fov_effective_y = {"fov_effective_y", "73.74", CVAR_ROM};
static	cvar_t	scr_zoomfov = {"zoom_fov", "30", CVAR_ARCHIVE};
static	cvar_t	scr_zoomspeed = {"zoom_speed", "8", CVAR_ARCHIVE};
cvar_t		scr_contrans = {"contrans", "0", CVAR_ARCHIVE};
static	cvar_t	scr_conspeed = {"scr_conspeed", "300", CVAR_NONE};
static	cvar_t	scr_centertime = {"scr_centertime", "4", CVAR_NONE};
/* scr_centerprintbg: 0 = off, 1 = full-width thin dim strip, 2 = text-width dim box (Ironwail parity).
 * Default 2 matches Ironwail df5219c — Hexen II uses centerprint heavily for narrative text. */
cvar_t		scr_centerprintbg = {"scr_centerprintbg", "2", CVAR_ARCHIVE};
static	cvar_t	con_logcenterprint = {"con_logcenterprint", "1", CVAR_ARCHIVE};
static	cvar_t	cl_showcrouchmsg = {"cl_showcrouchmsg", "1", CVAR_ARCHIVE};
static	cvar_t	scr_showram = {"showram", "1", CVAR_NONE};
static	cvar_t	scr_showturtle = {"showturtle", "0", CVAR_NONE};
static	cvar_t	scr_showpause = {"showpause", "1", CVAR_NONE};
static	cvar_t	scr_showfps = {"showfps", "0", CVAR_ARCHIVE};
static	cvar_t	scr_showspeed = {"scr_showspeed", "0", CVAR_ARCHIVE};
/* scr_menubgstyle: 0 = no dim, 1 = simple dim (Draw_FadeScreen), 2 = dim + translucent menu-area box (Ironwail parity).
 * Default 1 preserves the previous scr_menufade=1 default. */
cvar_t		scr_menubgstyle = {"scr_menubgstyle", "1", CVAR_ARCHIVE};
static	cvar_t	scr_showclock = {"showclock", "0", CVAR_ARCHIVE};
//static	cvar_t	gl_triplebuffer = {"gl_triplebuffer", "0", CVAR_ARCHIVE};

#if !defined(H2W)
static qboolean	scr_drawloading;
static float	scr_disabled_time;
int		total_loading_size, current_loading_size, loading_stage;
#endif	/* H2W */
qboolean	scr_disabled_for_loading;
qboolean	scr_skipupdate;
qboolean	block_drawing;

static qpic_t	*scr_ram;
static qpic_t	*scr_net;
static qpic_t	*scr_turtle;

static void SCR_ScreenShot_f (void);
static void SCR_ScreenHash_f (void);

static const char	*plaquemessage = "";	// pointer to current plaque message

static void Plaque_Draw (const char *message, qboolean AlwaysDraw);
#if !defined(H2W)
/* procedures for the mission pack intro messages and objectives */
static void Info_Plaque_Draw (const char *message);
static void Bottom_Plaque_Draw (const char *message);
#endif	/* H2W */


/*
===============================================================================

CENTER PRINTING

===============================================================================
*/

static char	scr_centerstring[1024];
float		scr_centertime_off;
static int	scr_center_lines;
static int	scr_erase_lines;

#define	MAXLINES	27
static int	lines;
static int	StartC[MAXLINES], EndC[MAXLINES], CorrectionC[MAXLINES];

#if !defined(H2W)
/* mission pack objectives: */
#define	MAX_INFO	1024
static char	infomessage[MAX_INFO];

static void UpdateInfoMessage (void)
{
	unsigned int i, check;
	const char *newmessage;

	q_strlcpy(infomessage, "Objectives:", sizeof(infomessage));

	if (!info_string_count)
		return;

	for (i = 0; i < 32; i++)
	{
		check = (1 << i);

		if (cl.info_mask & check)
		{
			newmessage = CL_GetInfoString(i);
			q_strlcat(infomessage, "@@", sizeof(infomessage));
			q_strlcat(infomessage, newmessage, sizeof(infomessage));
		}
	}

	for (i = 0; i < 32; i++)
	{
		check = (1 << i);

		if (cl.info_mask2 & check)
		{
			newmessage = CL_GetInfoString(i + 32);
			q_strlcat(infomessage, "@@", sizeof(infomessage));
			q_strlcat(infomessage, newmessage, sizeof(infomessage));
		}
	}
}
#endif	/* H2W */

static void FindTextBreaks (const char *message, int Width)
{
	int	pos, start, lastspace, oldlast;

	lines = pos = start = 0;
	lastspace = -1;
	CorrectionC[lines] = 0;

	while (1)
	{
		if (message[pos] == '\\' && (message[pos + 1] == '1' || message[pos + 1] == '2' || message[pos + 1] == '3' || message[pos + 1] == '4')) CorrectionC[lines] -= 2; 
		
		if (pos - start + CorrectionC[lines] >= Width || message[pos] == '@' || message[pos] == 0)
		{
			oldlast = lastspace;
			if (message[pos] == '@' || lastspace == -1 || message[pos] == 0)
				lastspace = pos;

			StartC[lines] = start;
			EndC[lines] = lastspace;
			lines++;
			CorrectionC[lines] = 0;
			if (lines == MAXLINES)
				return;
			if (message[pos] == '@')
				start = pos + 1;
			else if (oldlast == -1)
				start = lastspace;
			else
				start = lastspace + 1;

			lastspace = -1;
		}

		if (message[pos] == 32)
			lastspace = pos;
		else if (message[pos] == 0)
			break;

		pos++;
	}
}

/*
==============
SCR_CenterPrintToConsole

Echo a centerprint to the console in the form the PLAQUE actually shows.

con_logcenterprint arrived from Ironwail, where a centerprint is plain text
whose line breaks are newlines.  Hexen II encodes the same message differently
and the port did not account for it, so the raw string reached the console with
its encoding still in it.  Two things are in there:

  '@'          Raven's line break.  FindTextBreaks above treats EVERY '@' as
               one, so a literal '@' can never reach the plaque -- which is
               what makes translating all of them correct here rather than a
               guess about intent.
  "\1".."\4"   charset selects, consumed by M_Print (menu.c:271-290) and never
               drawn.

Shadows of Chaos' welcome message is the case that showed this up: the plaque
reads as eight tidy lines while qconsole.log got one 250-character run --
"Welcome to Hexen II: Shadows of Chaos@@Damage & abilities improve as you
level@@@Default bindings (autoexec.cfg):@Altfire: right mouse@..."

Done here rather than at the svc_centerprint parse site on purpose: the plaque
path indexes into the original string through StartC/EndC, so that payload has
to stay byte-exact.  This takes a private copy instead.

The result is never longer than the input -- each '@' becomes one newline and
each escape pair is dropped -- so the centerprint buffer's size bounds it.
==============
*/
static void SCR_CenterPrintToConsole (const char *str)
{
	char	buf[sizeof(scr_centerstring)];
	size_t	n = 0;

	while (*str && n < sizeof(buf) - 1)
	{
		if (str[0] == '\\' && str[1] >= '1' && str[1] <= '4')
		{
			str += 2;
			continue;
		}

		buf[n++] = (*str == '@') ? '\n' : *str;
		str++;
	}

	/* A message ending in '@' -- or in a run of them, which is how mods
	 * space out sections -- would otherwise stack blank lines on top of the
	 * newline CON_Printf adds below. */
	while (n > 0 && buf[n - 1] == '\n')
		n--;
	buf[n] = '\0';

	if (n)
		CON_Printf (_PRINT_NONOTIFY, "%s\n", buf);
}

/*
==============
SCR_CenterPrint

Called for important messages that should stay in the center of the screen
for a few moments
==============
*/
void SCR_CenterPrint (const char *str)
{
	if (!cl_showcrouchmsg.integer && strstr(str, "No room to stand up here"))
		return;

	strncpy (scr_centerstring, str, sizeof(scr_centerstring)-1);
	scr_centertime_off = scr_centertime.value;

	/* skip leading _ for line count (bottom plaque prefix) */
	FindTextBreaks(scr_centerstring[0] == '_' ?
		       scr_centerstring + 1 : scr_centerstring, 38);
	scr_center_lines = lines;

	if (con_logcenterprint.integer && str[0] != '_')
		SCR_CenterPrintToConsole (str);
}

static void SCR_DrawCenterString (void)
{
	int	i, cnt;
	int	bx, by;
	char	temp[80];

	FindTextBreaks(scr_centerstring, 38);

	by = (25-lines) * 8 / 2 + ((vid.height - 200)>>1);

	/* draw semi-transparent background behind centerprint text */
	if (scr_centerprintbg.integer && lines > 0)
	{
		int pad = 8;
		int bg_y = by - pad;
		int bg_h = lines * 8 + pad * 2;
		int bg_w, bg_x;
		float alpha;

		if (scr_centerprintbg.integer == 1)
		{
			/* Mode 1 (Simple): full-width thin dim strip — lighter alpha so
			 * the world stays visible through the band. */
			bg_w = vid.width;
			bg_x = 0;
			alpha = 0.30f;
		}
		else
		{
			/* Mode 2 (Menu Box): text-width dim box — darker for legibility. */
			bg_w = 38 * 8 + pad * 2;
			bg_x = (vid.width - bg_w) / 2;
			alpha = 0.50f;
		}

		Draw_FlushCharBatch();	/* GL_ImmBegin reuses the imm buffer; flush queued glyphs first */
		R_SetBlend (true);
		GL_ImmBegin();
		GL_ImmColor4f(0.0f, 0.0f, 0.0f, alpha);
		GL_ImmVertex2f(bg_x, bg_y);
		GL_ImmVertex2f(bg_x + bg_w, bg_y);
		GL_ImmVertex2f(bg_x + bg_w, bg_y + bg_h);
		GL_ImmVertex2f(bg_x, bg_y + bg_h);
		GL_ImmEnd(GL_QUADS, &gl_shader_flat);
		R_SetBlend (false);
	}

	for (i = 0; i < lines; i++, by += 8)
	{
		cnt = EndC[i] - StartC[i];
		strncpy (temp, &scr_centerstring[StartC[i]], cnt);
		temp[cnt] = 0;
		bx = (40-strlen(temp) - CorrectionC[i]) * 8 / 2;
		M_Print (bx, by, temp);
	}
}

static void SCR_CheckDrawCenterString (void)
{
	scr_copytop = 1;
	if (scr_center_lines > scr_erase_lines)
		scr_erase_lines = scr_center_lines;

	scr_centertime_off -= host_frametime;

	if (scr_centertime_off <= 0 && !cl.intermission)
		return;
	if (Key_GetDest() != key_game)
		return;
#if !defined(H2W)
	if (intro_playing || scr_centerstring[0] == '_')
	{
		Bottom_Plaque_Draw(scr_centerstring[0] == '_' ?
				   scr_centerstring + 1 : scr_centerstring);
		return;
	}
#endif	/* H2W */
	SCR_DrawCenterString ();
}

//=============================================================================


/*
====================
AdaptFovx
Adapt a 4:3 horizontal FOV to the current screen size using the "Hor+" scaling:
2.0 * atan(width / height * 3.0 / 4.0 * tan(fov43 / 2.0))
====================
*/
static float AdaptFovx (float fov_x, float width, float height)
{
	float	a, x;

	if (fov_x < 1 || fov_x > 179)
		Sys_Error ("Bad fov: %f", fov_x);

	if (!scr_fov_adapt.integer)
		return fov_x;
	/* Hor+ is a statement about the shape of the PICTURE, so it has to run
	 * on the displayed width, not the pixel count.  They differ whenever
	 * pixels are not square -- a 320x200 mode shown as 4:3 is the classic
	 * case, and using 1.6 there instead of 1.3333 overstates the field of
	 * view by about ten degrees.  uhexen2-c01c */
	width *= VID_PixelAspect ();
	if ((x = height / width) == 0.75)
		return fov_x;
	a = atan(0.75 / x * tan(fov_x / 360 * M_PI));
	a = a * 360 / M_PI;
	return a;
}

/*
====================
CalcFovy
====================
*/
static float CalcFovy (float fov_x, float width, float height)
{
	float	a, x;

	if (fov_x < 1 || fov_x > 179)
		Sys_Error ("Bad fov: %f", fov_x);

	/* Same correction as AdaptFovx: fov_y is the vertical angle of the
	 * displayed picture, so the horizontal term is the displayed width.
	 * Without this a non-square-pixel mode gets a vertical FOV that does
	 * not match its own horizontal one, and the view is subtly stretched
	 * rather than merely too wide.  uhexen2-c01c */
	width *= VID_PixelAspect ();
	x = width / tan(fov_x / 360 * M_PI);
	a = atan(height / x);
	a = a * 360 / M_PI;
	return a;
}

/*
=================
SCR_CalcRefdef

Must be called whenever vid changes
Internal use only
=================
*/
/*
====================
SCR_PublishEffectiveFov

Mirror the rendered field of view into the read-only cvars gamecode can see.
Guarded on change: the values only move when fov, the window aspect or the
zoom does, so the usual frame does a float compare and nothing else.
uhexen2-vsxi
====================
*/
static void SCR_PublishEffectiveFov (void)
{
	if (scr_fov_effective.value != r_refdef.fov_x)
		Cvar_SetValueROM ("fov_effective", r_refdef.fov_x);
	if (scr_fov_effective_y.value != r_refdef.fov_y)
		Cvar_SetValueROM ("fov_effective_y", r_refdef.fov_y);
}

static void SCR_CalcRefdef (void)
{
	float	size;
	int	h;

	scr_fullupdate = 0;		// force a background redraw

// bound viewsize.  Below 100 the 3D viewport is inset and SCR_TileClear
// fills the border; 110 and above progressively hides the Hexen status bar
// (see sbar.c).  The sub-100 range was clamped away once as a Pentium-era
// perf knob, which missed that it also sets the frame a mod's world-space
// menus are sized against.  uhexen2-461l
	if (scr_viewsize.integer < 30)
		Cvar_SetQuick (&scr_viewsize, "30");
	else if (scr_viewsize.integer > 140)
		Cvar_SetQuick (&scr_viewsize, "140");

// bound field of view
	if (scr_fov.integer < 10)
		Cvar_SetQuick (&scr_fov, "10");
	else if (scr_fov.integer > 170)
		Cvar_SetQuick (&scr_fov, "170");
	if (scr_zoomfov.integer < 10)
		Cvar_SetQuick (&scr_zoomfov, "10");
	else if (scr_zoomfov.integer > 170)
		Cvar_SetQuick (&scr_zoomfov, "170");

	vid.recalc_refdef = 0;

// force the status bar to redraw
	SB_ViewSizeChanged ();
	Sbar_Changed();

	if (scr_viewsize.integer >= 110)
		sb_lines = 0;		// no status bar
	else
		sb_lines = 36;	// FIXME: why not 46, i.e. BAR_TOP_HEIGHT?

#if !defined(SERVERONLY) && !defined(H2W)
	if (csqc_active)
		sb_lines = 0;		// CSQC draws its own HUD
#endif

	if (cl.intermission)
		sb_lines = 0;		// intermission is always full screen

	/* Below 100, viewsize insets the 3D viewport and SCR_TileClear fills
	 * the border -- the vanilla behaviour, restored after being clamped
	 * away as "a Pentium-era perf knob".  It is not only a perf knob: a mod
	 * that draws menus as world geometry in front of the player sizes them
	 * against the viewport, so removing the inset made Shadows of Chaos's
	 * pop-ups go edge to edge instead of sitting in a frame.  100 and above
	 * is unchanged, so nobody's current setup moves.  uhexen2-461l */
	size = scr_viewsize.integer > 100 ? 100.0f : (float) scr_viewsize.integer;
	if (cl.intermission)
		size = 100.0f;		// intermission is always full screen
	size /= 100.0f;

	h = vid.height - sb_lines;

	r_refdef.vrect.width = (int)(vid.width * size);
	if (r_refdef.vrect.width < 96)
	{
		size = 96.0f / vid.width;
		r_refdef.vrect.width = 96;	// min for icons
	}

	r_refdef.vrect.height = (int)(vid.height * size);
	if (r_refdef.vrect.height > h)
		r_refdef.vrect.height = h;

	r_refdef.vrect.x = (vid.width - r_refdef.vrect.width) / 2;
	r_refdef.vrect.y = (h - r_refdef.vrect.height) / 2;

	{
		float fov, zoom;
		zoom = cl.zoom;
		zoom = zoom * zoom * (3.f - 2.f * zoom);	/* smoothstep */
		fov = scr_fov.value + (scr_zoomfov.value - scr_fov.value) * zoom;
		r_refdef.fov_x = AdaptFovx (fov, r_refdef.vrect.width, r_refdef.vrect.height);
		r_refdef.fov_y = CalcFovy (r_refdef.fov_x, r_refdef.vrect.width, r_refdef.vrect.height);
	}

	SCR_PublishEffectiveFov ();

	scr_vrect = r_refdef.vrect;
}

//=============================================================================


/*
=================
SCR_SizeUp_f

Keybinding command
=================
*/
static void SCR_SizeUp_f (void)
{
	int v = scr_viewsize.integer + 10;
	if (v > 140) v = 140;
	Cvar_SetValueQuick (&scr_viewsize, v);
}

/*
=================
SCR_SizeDown_f

Keybinding command
=================
*/
static void SCR_SizeDown_f (void)
{
	int v = scr_viewsize.integer - 10;
	if (v < 100) v = 100;
	Cvar_SetValueQuick (&scr_viewsize, v);
}

static void SCR_Callback_refdef (cvar_t *var)
{
	vid.recalc_refdef = 1;
}

static void SCR_ToggleZoom_f (void)
{
	if (cl.zoomdir)
		cl.zoomdir = -cl.zoomdir;
	else
		cl.zoomdir = cl.zoom > 0.5f ? -1.f : 1.f;
}

static void SCR_ZoomDown_f (void)
{
	cl.zoomdir = 1.f;
}

static void SCR_ZoomUp_f (void)
{
	cl.zoomdir = -1.f;
}

static void SCR_UpdateZoom (void)
{
	float speed, delta;

	if (!cl.zoomdir)
		return;

	speed = scr_zoomspeed.value > 0.f ? scr_zoomspeed.value : 1e6f;
	delta = cl.zoomdir * speed * (float)(cl.time - cl.oldtime);
	cl.zoom += delta;
	if (cl.zoom >= 1.f)
	{
		cl.zoom = 1.f;
		cl.zoomdir = 0.f;
	}
	else if (cl.zoom <= 0.f)
	{
		cl.zoom = 0.f;
		cl.zoomdir = 0.f;
	}
	vid.recalc_refdef = 1;
}

//=============================================================================


/*
==================
SCR_Init
==================
*/
/* Ironwail's cl_screenshotname (uhexen2-a5nn.16): a filename TEMPLATE with
 * %map%, %date% and %time% tokens, expanded at capture time.
 *
 * This one earns its place here more than it does upstream.  Field reports
 * arrive as a folder of screenshots whose map has to be supplied from memory,
 * and hexen0007.png does not say which one it was.
 *
 * The default diverges from upstream's "screenshots/%map%_%date%_%time%" only
 * in the directory, which is shots/ in this tree (and where the menu, the
 * README and every existing user's folder already look).  A template with no
 * time-varying token in it -- "shots/hexen" -- gives back a stable name and
 * the numbered series that follows it: shots/hexen.png, then hexen_0000.png,
 * hexen_0001.png and so on. */
#if !defined(H2W)
static cvar_t cl_screenshotname = {"cl_screenshotname", "shots/%map%_%date%_%time%", CVAR_ARCHIVE};
#else
static cvar_t cl_screenshotname = {"cl_screenshotname", "shots/hw_%map%_%date%_%time%", CVAR_ARCHIVE};
#endif


void SCR_Init (void)
{
	scr_ram = Draw_PicFromWad ("ram");
	scr_net = Draw_PicFromWad ("net");
	scr_turtle = Draw_PicFromWad ("turtle");

	if (draw_reinit)
		return;

	Cvar_SetCallback (&scr_fov, SCR_Callback_refdef);
	Cvar_SetCallback (&scr_fov_adapt, SCR_Callback_refdef);
	Cvar_SetCallback (&scr_viewsize, SCR_Callback_refdef);
	Cvar_RegisterVariable (&scr_fov);
	Cvar_RegisterVariable (&scr_fov_effective);
	Cvar_RegisterVariable (&scr_fov_effective_y);
	Cvar_RegisterVariable (&scr_fov_adapt);
	Cvar_RegisterVariable (&scr_viewsize);
	Cvar_RegisterVariable (&scr_demobar_timeout);
	Cvar_RegisterVariable (&scr_zoomfov);
	Cvar_RegisterVariable (&scr_zoomspeed);
	Cvar_RegisterVariable (&con_logcenterprint);
	Cvar_RegisterVariable (&scr_centerprintbg);
	Cvar_RegisterVariable (&cl_showcrouchmsg);
	Cvar_RegisterVariable (&scr_contrans);
	Cvar_RegisterVariable (&scr_conspeed);
	Cvar_RegisterVariable (&scr_showram);
	Cvar_RegisterVariable (&scr_showturtle);
	Cvar_RegisterVariable (&scr_showpause);
	Cvar_RegisterVariable (&scr_showfps);
	Cvar_RegisterVariable (&scr_showspeed);
	Cvar_RegisterVariable (&scr_menubgstyle);
	Cvar_RegisterVariable (&scr_showclock);
	Cvar_RegisterVariable (&scr_centertime);
	Cvar_RegisterVariable (&cl_screenshotname);
//	Cvar_RegisterVariable (&gl_triplebuffer);

	Cmd_AddCommand ("screenshot",SCR_ScreenShot_f);
	Cmd_AddCommand ("screenhash",SCR_ScreenHash_f);
	Cmd_AddCommand ("sizeup",SCR_SizeUp_f);
	Cmd_AddCommand ("sizedown",SCR_SizeDown_f);
	Cmd_AddCommand ("+zoom", SCR_ZoomDown_f);
	Cmd_AddCommand ("-zoom", SCR_ZoomUp_f);
	Cmd_AddCommand ("togglezoom", SCR_ToggleZoom_f);

	scr_initialized = true;
	con_forcedup = true;	// we're just initialized and not connected yet
}

//=============================================================================


/*
==============
SCR_DrawRam
==============
*/
static void SCR_DrawRam (void)
{
	if (!scr_showram.integer)
		return;

	if (!r_cache_thrash)
		return;

	Draw_Pic (scr_vrect.x+32, scr_vrect.y, scr_ram);
}

/*
==============
SCR_DrawTurtle
==============
*/
static void SCR_DrawTurtle (void)
{
	static int	count;

	if (!scr_showturtle.integer)
		return;

	if (host_frametime < 0.1)
	{
		count = 0;
		return;
	}

	count++;
	if (count < 3)
		return;

	Draw_Pic (scr_vrect.x, scr_vrect.y, scr_turtle);
}

/*
==============
SCR_DrawNet
==============
*/
static void SCR_DrawNet (void)
{
#if !defined(H2W)
	if (realtime - cl.last_received_message < 0.3)
		return;
#else
	if (cls.netchan.outgoing_sequence -
			cls.netchan.incoming_acknowledged < UPDATE_BACKUP-1)
		return;
#endif
	if (cls.demoplayback)
		return;

	Draw_Pic (scr_vrect.x+64, scr_vrect.y, scr_net);
}

#if !defined(H2W)
/*
==============
SCR_DrawPointFileLabel

Label the origin of the leak path -- the point qbsp says the inside of the map
connects to the void -- so a mapper can find it without following the arrow
chain by eye.  Only drawn for the `pointfile leak` auto-load, where the world
genuinely has no visdata; a hand-typed `pointfile` gets arrows and nothing
else, since it may well be a stale .pts on a map that now seals.

R_ShowPointFile does the projection while the 3D matrices are still live and
hands back CANVAS_DEFAULT coordinates, which is the canvas already active
here.
==============
*/
static void SCR_DrawPointFileLabel (void)
{
	static const char	label[] = "Leak";
	float	x, y;

	if (!r_pointfile_isleak || !R_GetPointFileLabelPos (&x, &y))
		return;

	/* R_GetPointFileLabelPos answers in CANVAS_DEFAULT coordinates, which is
	 * what the have_world block runs in -- Draw_Crosshair switches to
	 * CANVAS_CROSSHAIR but restores on the way out, so the canvas is not
	 * inherited from it (uhexen2-ffdy).  Name it anyway: GL_SetCanvas is a
	 * no-op when the canvas already matches, and this way the function does
	 * not depend on where in the block it is called from. */
	GL_SetCanvas (CANVAS_DEFAULT);

	/* Centre the word on the point and lift it clear, so the arrowhead
	 * underneath stays readable. */
	Draw_String ((int)(x - (sizeof(label) - 1) * 4), (int)(y - 12), label);
}
#endif	/* !H2W */

/*
==============
SCR_InfoCorner

The bottom-right corner the debug readouts stack up from, in CANVAS_INFO's own
coordinates.  They used to measure from vid.width/vid.height, which is only the
right answer while the canvas is 1:1; with scr_infoscale it no longer is, and
the status bar they sit above is measured in vid space either way, so its height
has to come across with them.  uhexen2-r9qj.
==============
*/
static void SCR_InfoCorner (int *right, int *bottom)
{
	int	w, h;

	SCR_InfoCanvasSize (&w, &h);
	*right = w;
	/* round the bar up, so a half-canvas-pixel of it never overlaps the text */
	*bottom = h - (sb_lines * h + vid.height - 1) / vid.height;
}

static void SCR_DrawFPS (void)
{
	static double	oldtime = 0;
	static double	lastfps = 0;
	static int	oldframecount = 0;
	double	elapsed_time;
	int	frames;
	char	st[16];
	int	x, y;

	if (!scr_showfps.integer)
		return;

	elapsed_time = realtime - oldtime;
	frames = r_framecount - oldframecount;

	if (elapsed_time < 0 || frames < 0)
	{
		oldtime = realtime;
		oldframecount = r_framecount;
		return;
	}
	// update value every 3/4 second
	if (elapsed_time > 0.75)
	{
		lastfps = frames / elapsed_time;
		oldtime = realtime;
		oldframecount = r_framecount;
	}

	sprintf(st, "%4.0f FPS", lastfps);
	SCR_InfoCorner (&x, &y);
	x -= strlen(st) * 8 + 8;
	y -= 8;
//	Draw_TileClear(x, y, strlen(st) * 8, 8);
	Draw_String(x, y, st);
}

static void SCR_DrawClock (void)
{
	char	st[16];
	int	x, y, right;
	int	minutes, seconds;

	if (!scr_showclock.integer)
		return;

	if (scr_showclock.integer >= 2)
	{
		/* Wall clock (real time) */
		time_t	rawtime;
		struct tm *ti;
		time(&rawtime);
		ti = localtime(&rawtime);
		if (!ti) return;
		if (scr_showclock.integer == 3)
			sprintf(st, "%02d:%02d:%02d", ti->tm_hour, ti->tm_min, ti->tm_sec);
		else
			sprintf(st, "%02d:%02d", ti->tm_hour, ti->tm_min);
	}
	else
	{
		/* Game time (level elapsed) */
		seconds = (int)cl.time;
		minutes = seconds / 60;
		seconds %= 60;
		if (minutes >= 60)
			sprintf(st, "%d:%02d:%02d", minutes / 60, minutes % 60, seconds);
		else
			sprintf(st, "%d:%02d", minutes, seconds);
	}

	SCR_InfoCorner (&right, &y);
	x = right - strlen(st) * 8 - 8;
	y -= 16 + (scr_showfps.integer ? 8 : 0);
	Draw_String(x, y, st);

	/* Also show host uptime below the clock */
	if (scr_showclock.integer >= 2)
	{
		seconds = (int)realtime;
		minutes = seconds / 60;
		seconds %= 60;
		if (minutes >= 60)
			sprintf(st, "%d:%02d:%02d", minutes / 60, minutes % 60, seconds);
		else
			sprintf(st, "%d:%02d", minutes, seconds);
		x = right - strlen(st) * 8 - 8;
		y -= 8;
		Draw_String(x, y, st);
	}
}

/*
==============
SCR_DrawSpeed -- Ironwail
==============
*/
static void SCR_DrawSpeed (void)
{
	static float	display_speed = -1;
	static float	maxspeed = 0;
	static double	lasttime = 0;
	vec3_t		vel;
	float		speed;
	char		st[12];
	int		x, y;

	if (!scr_showspeed.integer)
		return;
	if (cls.state != ca_active)
		return;

	/* horizontal speed only (no vertical component) */
	vel[0] = cl.velocity[0];
	vel[1] = cl.velocity[1];
	vel[2] = 0;
	speed = VectorLength (vel);
	if (speed > maxspeed)
		maxspeed = speed;

	/* update display value every 0.1 seconds */
	if (realtime - lasttime >= 0.1)
	{
		display_speed = maxspeed;
		maxspeed = 0;
		lasttime = realtime;
	}
	if (display_speed < 0)
		return;

	sprintf (st, "%d", (int)display_speed);
	SCR_InfoCorner (&x, &y);
	x -= strlen(st) * 8 + 8;
	y -= 8;
	if (scr_showfps.integer)
		y -= 8;
	if (scr_showclock.integer)
		y -= 8;
	Draw_String (x, y, st);
}


/*
==============
SCR_DrawDemoBar -- Ironwail

Playback position readout during demo playback: a seek rail with a cursor,
the demo name, the play/pause state, and the elapsed map time above the
cursor.  Ported from Ironwail's SCR_DrawDemoControls (Quake/gl_screen.c);
the half of that function that drives demo speed and scrubbing has nothing
to drive here yet -- uHexen2 has no demo speed control -- so this is the
readout without the controls.

The position is approximated from the demo file offset rather than from a
time index: a .dem carries no duration in its header, and walking it to
find one would mean reading the whole file up front.  fshandle_t.pos is
already relative to the data start, so a demo inside a pak or a .pk3 needs
no extra bookkeeping (Ironwail carries cls.demofilestart for exactly that).

Drawn in CANVAS_SBAR so it scales with the status bar and sits just above
the Hexen II bar's top bumps, or in CANVAS_MENU during intermission, where
there is no status bar to clear.
==============
*/
#define DEMOBAR_CHARS	38	/* Ironwail's TIMEBAR_CHARS */
#define DEMOBAR_FADE	0.5f	/* seconds of fade-out at the end of the timeout */

static void SCR_DrawDemoBar (void)
{
	static float	showtime = 0.0f;
	static double	lastframe = 0.0;
	static double	lastactivity = 0.0;
	static qboolean	wasplaying = false;
	static qboolean	waspaused = false;
	int		i, x, y, cursor, mins, secs;
	float		frac, alpha, s;
	const char	*str, *colon;
	double		dt;

	if (!cls.demoplayback || scr_demobar_timeout.value < 0.0f)
	{
		showtime = 0.0f;
		wasplaying = false;
		lastframe = realtime;
		lastactivity = cls.demoactivity;
		return;
	}

	dt = realtime - lastframe;
	lastframe = realtime;
	if (dt < 0.0 || dt > 1.0)
		dt = 0.0;	/* loading hitch or clock reset: don't eat the timeout */

	/* Re-arm the timeout on anything the viewer can cause.  Ironwail keys off
	 * demo speed changes, which we have no control for; what we have is the
	 * demo starting, the pause state flipping, and input that reaches the demo
	 * rather than the menu (keys.c stamps cls.demoactivity for key_dest ==
	 * key_game only -- counting menu keys left the bar blinking underneath the
	 * menu that a keypress during playback opens, uhexen2-tkn5). */
	if (!wasplaying || cl.paused != waspaused ||
	    cls.demoactivity != lastactivity || scr_demobar_timeout.value == 0.0f)
	{
		wasplaying = true;
		waspaused = cl.paused;
		lastactivity = cls.demoactivity;
		showtime = (scr_demobar_timeout.value > 0.0f) ? scr_demobar_timeout.value : 1.0f;
	}
	else
	{
		showtime -= (float) dt;
		if (showtime <= 0.0f)
		{
			showtime = 0.0f;
			return;
		}
	}

	alpha = (showtime < DEMOBAR_FADE) ? showtime / DEMOBAR_FADE : 1.0f;

	frac = (cls.demofh.length > 0)
		? (float)(cls.demofh.pos / (double) cls.demofh.length) : 0.0f;
	if (frac < 0.0f) frac = 0.0f;
	else if (frac > 1.0f) frac = 1.0f;

	if (cl.intermission)
	{
		GL_SetCanvas (CANVAS_MENU);
		/* CANVAS_MENU is 320 wide and as tall as the screen divided by the
		 * menu scale; mirror GL_SetCanvas's own math to find the bottom.
		 * Ironwail sits the bar 1/8 of the way up from there. */
		s = SCR_CalcUIScale (&scr_menuscale);
		if (s > (float)glwidth / (float)UI_CANVAS_WIDTH)
			s = (float)glwidth / (float)UI_CANVAS_WIDTH;
		if (s < 0.0001f)
			s = 1.0f;
		y = (int)((float)glheight / s) * 7 / 8;
	}
	else
	{
		GL_SetCanvas (CANVAS_SBAR);
		/* Canvas y grows downward to the screen bottom at UI_SBAR_CANVAS_HEIGHT.
		 * The status bar's top bumps start 69 units above that (BAR_TOP_HEIGHT +
		 * BAR_BUMP_HEIGHT), so 89 leaves the name row clear of them. */
		y = UI_SBAR_CANVAS_HEIGHT - 89;
	}
	x = (UI_CANVAS_WIDTH - DEMOBAR_CHARS * 8) / 2;

	/* Backdrop for the whole readout: time label at y-19, rail at y-8, name
	 * row at y, each 8 tall -- padded 4 all round. */
	Draw_FillAlpha (x - 8, y - 23, DEMOBAR_CHARS * 8 + 16, 35,
			0.0f, 0.0f, 0.0f, 0.5f * alpha);

	Draw_SetCharacterAlpha (alpha);

	/* Playback state on the left, demo name centered. */
	Draw_String (x, y, cl.paused ? "II" : ">");
	if (cls.demofilename[0])
		Draw_String ((UI_CANVAS_WIDTH - (int)strlen(cls.demofilename) * 8) / 2,
			     y, cls.demofilename);

	/* Seek rail and cursor.  256..259 are Hexen II's slider glyphs -- the
	 * same ones M_DrawSlider uses, not Quake's 128..131. */
	cursor = x + (int)((DEMOBAR_CHARS - 1) * 8 * frac);
	y -= 8;
	Draw_Character (x - 8, y, 256);
	for (i = 0; i < DEMOBAR_CHARS; i++)
		Draw_Character (x + i * 8, y, 257);
	Draw_Character (x + i * 8, y, 258);
	Draw_Character (cursor, y, 259);

	/* Elapsed map time above the cursor, ':' aligned with it. */
	y -= 11;
	secs = (int) cl.time;
	mins = secs / 60;
	secs %= 60;
	str = va ("%i:%02i", mins, secs);
	colon = strchr (str, ':');
	x = cursor - (int)(colon - str) * 8;
	if (x < 0)
		x = 0;
	else if (x + (int)strlen(str) * 8 > UI_CANVAS_WIDTH)
		x = UI_CANVAS_WIDTH - (int)strlen(str) * 8;
	Draw_String (x, y, str);

	Draw_SetCharacterAlpha (1.0f);
}

/*
==============
DrawPause
==============
*/
static void SCR_DrawPause (void)
{
	static qboolean	newdraw = false;
	static float	LogoPercent, LogoTargetPercent;
	qpic_t	*pic;
	int	finaly;
	float	delta;

	if (!scr_showpause.integer)	// turn off for screenshots
		return;

	if (!cl.paused)
	{
		newdraw = false;
		return;
	}

	if (!newdraw)
	{
		newdraw = true;
		LogoTargetPercent = 1;
		LogoPercent = 0;
	}

	pic = Draw_CachePic ("gfx/menu/paused.lmp");
//	Draw_Pic ( (vid.width - pic->width)/2, (vid.height - 48 - pic->height)/2, pic);

	if (LogoPercent < LogoTargetPercent)
	{
		delta = ((LogoTargetPercent - LogoPercent) / .5) * host_frametime;
		if (delta < 0.004)
			delta = 0.004;
		LogoPercent += delta;
		if (LogoPercent > LogoTargetPercent)
			LogoPercent = LogoTargetPercent;
	}

	finaly = ((float)pic->height * LogoPercent) - pic->height;
	Draw_TransPicCropped ( (vid.width - pic->width)/2, finaly, pic);
}

#if !defined(H2W)
/*
==============
SCR_DrawLoading
==============
*/
#if !defined(DRAW_PROGRESSBARS)
void SCR_DrawLoading (void)
{
	int	offset;
	qpic_t	*pic;

	if (!scr_drawloading && loading_stage == 0)
		return;

	pic = Draw_CacheLoadingPic ();
	offset = (vid.width - pic->width) / 2;
	Draw_TransPic (offset, 0, pic);
}
#else
void SCR_DrawLoading (void)
{
	int	size, count, offset;
	qpic_t	*pic;

	if (!scr_drawloading && loading_stage == 0)
		return;

	pic = Draw_CachePic ("gfx/menu/loading.lmp");
	offset = (vid.width - pic->width) / 2;
	Draw_TransPic (offset, 0, pic);

	if (loading_stage == 0)
		return;

	size = (total_loading_size) ?
		(current_loading_size * 106 / total_loading_size) : 0;
	offset += 42;

	count = (loading_stage == 1) ? size : 106;
	if (count)
	{
		Draw_Fill (offset, 87+0, count, 1, 136);
		Draw_Fill (offset, 87+1, count, 4, 138);
		Draw_Fill (offset, 87+5, count, 1, 136);
	}

	count = (loading_stage == 2) ? size : 0;
	if (count)
	{
		Draw_Fill (offset, 97+0, count, 1, 168);
		Draw_Fill (offset, 97+1, count, 4, 170);
		Draw_Fill (offset, 97+5, count, 1, 168);
	}
}
#endif	/* !DRAW_PROGRESSBARS */

/*
===============
SCR_BeginLoadingPlaque

================
*/
void SCR_BeginLoadingPlaque (void)
{
	S_StopAllSounds (true);

	/* Vanilla Quake/uHexen2 bailed here when (cls.state != ca_connected
	 * || cls.signon != SIGNONS). That made the plaque a no-op for the
	 * common case — Host_Map_f calls CL_Disconnect (which clears state
	 * and signon) immediately before this — so scr_disabled_for_loading
	 * was never set and the screen kept ticking through signon, giving
	 * the user a flash of conback instead of a held "Loading" frame.
	 * Callers know when they want a plaque; trust them. */

// redraw with no console and the loading plaque
	Con_ClearNotify ();
	scr_centertime_off = 0;
	scr_con_current = 0;

	scr_drawloading = true;
	scr_fullupdate = 0;
	Sbar_Changed();
	SCR_UpdateScreen ();
	scr_drawloading = false;

	scr_disabled_for_loading = true;
	scr_disabled_time = realtime;
	scr_fullupdate = 0;
}

/*
===============
SCR_EndLoadingPlaque

================
*/
void SCR_EndLoadingPlaque (void)
{
	scr_disabled_for_loading = false;
	scr_fullupdate = 0;
	Con_ClearNotify ();
}
#endif	/* H2W */

//=============================================================================


/*
==================
SCR_SetUpToDrawConsole
==================
*/
static void SCR_SetUpToDrawConsole (void)
{
	Con_CheckResize ();

#if !defined(H2W)
	if (scr_drawloading)
		return;		// never a console with loading plaque

	con_forcedup = !cl.worldmodel || cls.signon != SIGNONS;
#else
	con_forcedup = cls.state != ca_active;
#endif	/* H2W */

// decide on the height of the console
	if (con_forcedup)
	{
		scr_conlines = vid.height;	// full screen
		scr_con_current = scr_conlines;
	}
	else if (Key_GetDest() == key_console)
		scr_conlines = vid.height / 2;	// half screen
	else
		scr_conlines = 0;		// none visible

	if (scr_conlines < scr_con_current)
	{
		scr_con_current -= scr_conspeed.value * host_frametime;
		if (scr_conlines > scr_con_current)
			scr_con_current = scr_conlines;
	}
	else if (scr_conlines > scr_con_current)
	{
		scr_con_current += scr_conspeed.value * host_frametime;
		if (scr_conlines < scr_con_current)
			scr_con_current = scr_conlines;
	}

	if (clearconsole++ < vid.numpages)
	{
		Sbar_Changed();
	}
	else if (clearnotify++ < vid.numpages)
	{
	}
	else
		con_notifylines = 0;
}

/*
==================
SCR_DrawConsole
==================
*/
static void SCR_DrawConsole (void)
{
	if (scr_con_current)
	{
		scr_copyeverything = 1;
		Con_DrawConsole (scr_con_current);
		clearconsole = 0;
	}
	else
	{
		keydest_t dest = Key_GetDest();
		if (dest == key_game || dest == key_message)
			Con_DrawNotify ();	// only draw notify in game
	}
}


/*
==============================================================================

SCREEN SHOTS

==============================================================================
*/

typedef struct _TargaHeader {
	unsigned char	id_length, colormap_type, image_type;
	unsigned short	colormap_index, colormap_length;
	unsigned char	colormap_size;
	unsigned short	x_origin, y_origin, width, height;
	unsigned char	pixel_size, attributes;
} TargaHeader;


/*
==================
SCR_ScreenShot_f
==================
*/
#if !defined(H2W)
static const char scr_shotprefix[] = "shots/hexen";
#else
static const char scr_shotprefix[] = "shots/hw";
#endif

/*
==================
SCR_ExpandShotName

Expand cl_screenshotname's tokens, then make the result safe to hand to the
filesystem.  The sanitising pass is not paranoia about the cvar: %map% carries
whatever the map is called, and mod maps do contain spaces and punctuation.
'/' survives it so the template can name a subdirectory, which is the whole
reason upstream's default starts with one.
==================
*/
static void SCR_ExpandShotName (const char *tmpl, char *out, size_t outsize)
{
	char		stamp[24], datebuf[16], timebuf[16], mapbuf[MAX_QPATH];
	const char	*sub;
	size_t		n = 0;
	int		i;

	/* Sys_DateTimeString gives "MM/DD/YYYY HH:MM:SS" -- slashes and colons,
	 * neither of which belongs in a filename -- so split it into two
	 * filename-safe halves rather than sanitising it after substitution,
	 * where '/' has to stay legal for the directory part. */
	Sys_DateTimeString (stamp);
	memcpy (datebuf, stamp, 10);	datebuf[10] = '\0';
	memcpy (timebuf, stamp + 11, 8); timebuf[8] = '\0';
	for (i = 0; datebuf[i]; i++) if (datebuf[i] == '/') datebuf[i] = '-';
	for (i = 0; timebuf[i]; i++) if (timebuf[i] == ':') timebuf[i] = '-';

	if (cl.worldmodel && cl.worldmodel->name[0])
		COM_FileBase (cl.worldmodel->name, mapbuf, sizeof(mapbuf));
	else
		q_strlcpy (mapbuf, "nomap", sizeof(mapbuf));

	while (*tmpl && n + 1 < outsize)
	{
		if (*tmpl == '%')
		{
			sub = NULL;
			if (!q_strncasecmp (tmpl, "%map%", 5))       { sub = mapbuf;  tmpl += 5; }
			else if (!q_strncasecmp (tmpl, "%date%", 6)) { sub = datebuf; tmpl += 6; }
			else if (!q_strncasecmp (tmpl, "%time%", 6)) { sub = timebuf; tmpl += 6; }
			if (sub)
			{
				while (*sub && n + 1 < outsize)
					out[n++] = *sub++;
				continue;
			}
			/* an unrecognised %something% is copied through verbatim
			 * rather than eaten, so a typo shows up in the filename
			 * instead of silently vanishing */
		}
		out[n++] = *tmpl++;
	}
	out[n] = '\0';

	for (i = 0; out[i]; i++)
	{
		char c = out[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' ||
		      c == '.' || c == '/'))
			out[i] = '_';
	}

	if (!out[0])
		q_strlcpy (out, scr_shotprefix, outsize);
}

static void SCR_ScreenShot_f (void)
{
	char	ext[8];
	char	shotname[MAX_QPATH];
	char	filename[MAX_QPATH + 16];
	char	fullpath[MAX_OSPATH];
	byte	*rgba;
	int	i, npix, quality;
	qboolean	ok;

	/* Ironwail's contract: "screenshot [png|tga|jpg] [quality]".  The format
	 * is per-invocation rather than a cvar, and png is the default. */
	q_strlcpy (ext, "png", sizeof(ext));
	if (Cmd_Argc() >= 2)
	{
		q_strlcpy (ext, Cmd_Argv(1), sizeof(ext));
		if (q_strcasecmp(ext, "png") && q_strcasecmp(ext, "tga") &&
							q_strcasecmp(ext, "jpg"))
		{
			Con_Printf ("usage: screenshot [png|tga|jpg] [quality]\n");
			return;
		}
	}
	quality = 90;
	if (Cmd_Argc() >= 3)
	{
		quality = atoi (Cmd_Argv(2));
		if (quality < 1 || quality > 100)
			quality = 90;
	}

	/* Expand the template, then find a free name.  The unsuffixed name is
	 * tried FIRST: with the default template a %time% at one-second
	 * resolution already makes collisions rare, and "demo1_08-31-2026_10-42-13.png"
	 * reads better than the same thing with a redundant _0000 on it.  The
	 * numbered fallback covers two shots inside the same second, and carries
	 * the series when the template has no time-varying token in it. */
	SCR_ExpandShotName (cl_screenshotname.string, shotname, sizeof(shotname));

	/* Create the directory the TEMPLATE names, not a hardcoded "shots":
	 * the template is allowed to keep '/' precisely so it can put shots
	 * somewhere else, and a fixed mkdir would leave that case writing into
	 * a directory that does not exist. */
	{
		char	dir[MAX_QPATH];
		char	*slash;

		q_strlcpy (dir, shotname, sizeof(dir));
		slash = strrchr (dir, '/');
		if (slash)
		{
			*slash = '\0';
			FS_MakePath_BUF (FS_USERDIR, NULL, fullpath, sizeof(fullpath), dir);
			Sys_mkdir (fullpath, false);
		}
	}

	q_snprintf (filename, sizeof(filename), "%s.%s", shotname, ext);
	FS_MakePath_BUF (FS_USERDIR, NULL, fullpath, sizeof(fullpath), filename);
	if (Sys_FileType(fullpath) != FS_ENT_NONE)
	{
		for (i = 0; i <= 9999; i++)
		{
			q_snprintf (filename, sizeof(filename), "%s_%04d.%s", shotname, i, ext);
			FS_MakePath_BUF (FS_USERDIR, NULL, fullpath, sizeof(fullpath), filename);
			if (Sys_FileType(fullpath) == FS_ENT_NONE)
				break;
		}
		if (i > 9999) { Con_Printf ("Screenshot: too many files\n"); return; }
	}

	/* GL ES 3.0 guarantees exactly one glReadPixels format/type pair --
	 * GL_RGBA/GL_UNSIGNED_BYTE -- plus one implementation-chosen pair that
	 * has to be queried.  GL_RGB is a desktop-only spelling, and asking for
	 * it on the ES tier wrote a stride-mismatched green smear instead of the
	 * frame.  Desktop GL accepts RGBA just as happily, so read RGBA on both
	 * tiers and pack down here rather than fork the path.  uhexen2-3cke. */
	npix = glwidth * glheight;
	rgba = (byte *) malloc((size_t)npix * 4);
	if (!rgba) { Con_Printf("Screenshot: out of memory\n"); return; }

	glPixelStorei_fp (GL_PACK_ALIGNMENT, 1);
	glReadPixels_fp (glx, gly, glwidth, glheight, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

	if (!q_strcasecmp(ext, "tga"))
	{
		/* Hand-rolled rather than routed through Image_WriteTGA: a bottom-up
		 * origin is native to TGA, so the rows glReadPixels returns go down
		 * verbatim with no flip copy. */
		int	size = npix * 3 + 18;
		byte	*buffer = (byte *) malloc(size);

		if (!buffer)
		{
			free(rgba);
			Con_Printf("Screenshot: out of memory\n");
			return;
		}

		memset (buffer, 0, 18);
		buffer[2] = 2;		/* uncompressed type */
		buffer[12] = glwidth & 255;
		buffer[13] = glwidth >> 8;
		buffer[14] = glheight & 255;
		buffer[15] = glheight >> 8;
		buffer[16] = 24;	/* pixel size */

		for (i = 0; i < npix; i++)
		{
			buffer[18 + i*3 + 0] = rgba[i*4 + 2];	/* B */
			buffer[18 + i*3 + 1] = rgba[i*4 + 1];	/* G */
			buffer[18 + i*3 + 2] = rgba[i*4 + 0];	/* R */
		}

		ok = (FS_WriteFile (filename, buffer, size) == 0);
		free(buffer);
	}
	else
	{
		/* stb wants top-down RGB.  glReadPixels hands back bottom-up rows,
		 * so pack to RGB here and let the writer do the vertical flip. */
		byte	*rgb = (byte *) malloc((size_t)npix * 3);

		if (!rgb)
		{
			free(rgba);
			Con_Printf("Screenshot: out of memory\n");
			return;
		}

		for (i = 0; i < npix; i++)
		{
			rgb[i*3 + 0] = rgba[i*4 + 0];	/* R */
			rgb[i*3 + 1] = rgba[i*4 + 1];	/* G */
			rgb[i*3 + 2] = rgba[i*4 + 2];	/* B */
		}

		if (!q_strcasecmp(ext, "png"))
			ok = Image_WritePNG (fullpath, rgb, glwidth, glheight, 24, false);
		else
			ok = Image_WriteJPG (fullpath, rgb, glwidth, glheight, 24, quality, false);

		free(rgb);
	}

	free(rgba);

	if (ok)
		Con_Printf ("Wrote %s\n", filename);
	else
		Con_Printf ("Couldn't write %s\n", filename);
}

/* uhexen2-8pzr: visual-regression gate for Hi-Z acceptance sweep.  FNV-1a
 * over the post-tonemap framebuffer at the current camera.  Identical hash
 * across `gl_hiz_cull 0` vs 1 proves no surface was wrongly culled at this
 * vantage. */
static void SCR_ScreenHash_f (void)
{
	int		i, size;
	byte		*buffer;
	uint32_t	hash = 2166136261u;	/* FNV-1a offset basis */

	/* RGBA rather than RGB for the reason given in SCR_ScreenShot_f
	 * (uhexen2-3cke).  The alpha byte is dropped before hashing, and the RGB
	 * bytes are fed in the same order as before, so hashes recorded by the
	 * uhexen2-8pzr sweep stay comparable across this change. */
	size = glwidth * glheight * 4;
	buffer = (byte *) malloc(size);
	if (!buffer) { Con_Printf("screenhash: out of memory\n"); return; }

	glPixelStorei_fp (GL_PACK_ALIGNMENT, 1);
	glReadPixels_fp (glx, gly, glwidth, glheight, GL_RGBA, GL_UNSIGNED_BYTE, buffer);

	for (i = 0; i < size; i++)
	{
		if ((i & 3) == 3)
			continue;	/* alpha */
		hash ^= buffer[i];
		hash *= 16777619u;
	}
	free(buffer);

	Con_Printf ("screenhash: %08x  (%dx%d)\n", hash, glwidth, glheight);
}

//=============================================================================


static const char	*scr_notifystring;
static qboolean	scr_drawdialog;

static void SCR_DrawNotifyString (void)
{
	Plaque_Draw(scr_notifystring, true);
}

/*
==================
SCR_ModalMessage

Displays a text string in the center of the screen
and waits for a Y or N keypress.
==================
*/
int SCR_ModalMessage (const char *text)
{
#if !defined(H2W)
	if (cls.state == ca_dedicated)
		return true;
#endif	/* H2W */
	scr_notifystring = text;

// draw a fresh screen
	scr_fullupdate = 0;
	scr_drawdialog = true;
	SCR_UpdateScreen ();
	scr_drawdialog = false;

	S_ClearBuffer ();		// so dma doesn't loop current sound

	do
	{
		key_count = -1;		// wait for a key down and up
		Sys_SendKeyEvents ();
	} while (key_lastpress != 'y' && key_lastpress != 'n' && key_lastpress != K_ESCAPE);

	scr_fullupdate = 0;
	SCR_UpdateScreen ();

	return key_lastpress == 'y';
}

//=============================================================================

/*
===============
SCR_BringDownConsole

Brings the console down and fades the palettes back to normal
================
*/
#if 0	/* all uses are commented out */
void SCR_BringDownConsole (void)
{
	int	i;

	scr_centertime_off = 0;

	for (i = 0; i < 20 && scr_conlines != scr_con_current; i++)
		SCR_UpdateScreen ();

	cl.cshifts[0].percent = 0;	// no area contents palette on next frame
	VID_SetPalette (host_basepal);
}
#endif

//=============================================================================

void SCR_SetPlaqueMessage (const char *msg)
{
	plaquemessage = msg;
}

static void Plaque_Draw (const char *message, qboolean AlwaysDraw)
{
	int	i, cnt;
	int	bx, by;
	char	temp[80];

	/* Any visible console, not just a full-screen one.  The old test only
	 * caught scr_con_current == vid.height, so the ordinary half-height
	 * drop-down let the plaque draw straight over the console text --
	 * BloodShot's "overriding the sprints", with a wall of gold plaque text
	 * across the lines he was trying to read.  Centerprints already behave
	 * this way (SCR_CheckDrawCenterString2 bails on key_dest != key_game);
	 * this makes the plaque agree with them.  AlwaysDraw still wins, which
	 * is what keeps the modal notify plaques visible.  uhexen2-u1n6 */
	if (scr_con_current > 0 && !AlwaysDraw)
		return;

	if (!*message)
		return;

	FindTextBreaks(message, PLAQUE_WIDTH);

	by = (25-lines) * 8 / 2 + ((vid.height - 200)>>1);
	M_DrawTextBox (32, by - 16, PLAQUE_WIDTH + 4, lines + 2);

	for (i = 0; i < lines; i++, by += 8)
	{
		cnt = EndC[i] - StartC[i];
		strncpy (temp, &message[StartC[i]], cnt);
		temp[cnt] = 0;
		bx = (40-strlen(temp) - CorrectionC[i]) * 8 / 2;
		M_Print (bx, by, temp);
	}
}

#if !defined(H2W)
static void Info_Plaque_Draw (const char *message)
{
	int	i, cnt;
	int	bx, by;
	char	temp[80];

	/* Same as Plaque_Draw: suppress for any visible console, not only a
	 * full-screen one.  uhexen2-u1n6 */
	if (scr_con_current > 0)
		return;

	if (!info_string_count || !*message)
		return;

	FindTextBreaks(message, PLAQUE_WIDTH+4);

	if (lines == MAXLINES)
	{
		Con_DPrintf("%s: line overflow error\n", __thisfunc__);
		lines = MAXLINES-1;
	}

	by = (25-lines) * 8 / 2 + ((vid.height - 200)>>1);
	M_DrawTextBox (15, by - 16, PLAQUE_WIDTH + 4 + 4, lines + 2);

	for (i = 0; i < lines; i++, by += 8)
	{
		cnt = EndC[i] - StartC[i];
		strncpy (temp, &message[StartC[i]], cnt);
		temp[cnt] = 0;
		bx = (40-strlen(temp) - CorrectionC[i]) * 8 / 2;
		M_Print (bx, by, temp);
	}
}

static void Bottom_Plaque_Draw (const char *message)
{
	int	i, cnt;
	int	bx, by;
	char	temp[80];

	if (!*message)
		return;

	FindTextBreaks(message, PLAQUE_WIDTH);

	by = (((vid.height) / 8) - lines - 2) * 8;
	M_DrawTextBox (32, by - 16, PLAQUE_WIDTH + 4, lines + 2);

	for (i = 0; i < lines; i++, by += 8)
	{
		cnt = EndC[i] - StartC[i];
		strncpy (temp, &message[StartC[i]], cnt);
		temp[cnt] = 0;
		bx = (40-strlen(temp) - CorrectionC[i]) * 8 / 2;
		M_Print (bx, by, temp);
	}
}

//=============================================================================


static void I_Print (int cx, int cy, const char *str, int flags)
{
	int	num, x, y;
	const char	*s;

	x = cx + ((vid.width - 320)>>1);
	y = cy;
	if (!(flags & (INTERMISSION_PRINT_TOP|INTERMISSION_PRINT_TOPMOST)))
		y += ((vid.height - 200)>>1);
	s = str;

	while (*s)
	{
		num = (unsigned char)(*s);
		if (!(flags & INTERMISSION_PRINT_WHITE))
			num += 256;
		Draw_Character (x, y, num);
		s++;
		x += 8;
	}
}

#if FULLSCREEN_INTERMISSIONS
#	define	Load_IntermissionPic_FN(X,Y,Z)	Draw_CachePicNoTrans((X))
#	define	Draw_IntermissionPic_FN(X,Y,Z)	Draw_IntermissionPic((Z))
#else
#	define	Load_IntermissionPic_FN(X,Y,Z)	Draw_CachePic((X))
#	define	Draw_IntermissionPic_FN(X,Y,Z)	Draw_Pic((X),(Y),(Z))
#endif

/*
===============
SB_IntermissionOverlay
===============
*/
static void SB_IntermissionOverlay (void)
{
	qpic_t	*pic;
	int	elapsed, size, bx, by, i;
	char		temp[80];
	const char	*message;

	scr_copyeverything = 1;
	scr_fullupdate = 0;

#if !defined(H2W)
	if (cl.gametype == GAME_DEATHMATCH)
#else
	if (!cl_siege)
#endif
	{
		Sbar_DeathmatchOverlay ();
		return;
	}

	if (cl.intermission_pic == NULL)
		Host_Error ("%s: NULL intermission picture", __thisfunc__);
	else
	{
		pic = Load_IntermissionPic_FN (cl.intermission_pic, vid.width, vid.height);
		Draw_IntermissionPic_FN (((vid.width - 320)>>1), ((vid.height - 200)>>1), pic);
	}

	if (cl.message_index >= 0 && cl.message_index < host_string_count)
		message = Host_GetString (cl.message_index);
	else if (cl.intermission_flags & INTERMISSION_NO_MESSAGE)
		message = "";
	else
	{
		message = ""; /* silence compilers */
		Host_Error ("%s: Intermission string #%d not available (host_string_count: %d)",
					__thisfunc__, cl.message_index, host_string_count);
	}

	if (cl.intermission_flags & INTERMISSION_NOT_CONNECTED)
		elapsed = (realtime - cl.completed_time) * 20;
	else	elapsed = (cl.time  - cl.completed_time) * 20;
	if (cl.intermission_flags & INTERMISSION_PRINT_DELAY)
	{
		elapsed -= 50;	/* delay about 2.5 seconds */
		if (elapsed < 0)
			elapsed = 0;
	}

	FindTextBreaks(message, 38);

	if (cl.intermission_flags & INTERMISSION_PRINT_TOPMOST)
		by =  16;
	else	by = (25-lines) * 8 / 2;

	for (i = 0; i < lines; i++, by += 8)
	{
		size = EndC[i] - StartC[i];
		strncpy (temp, &message[StartC[i]], size);

		if (size > elapsed)
			size = elapsed;
		temp[size] = 0;

		bx = (40-strlen(temp) - CorrectionC[i]) * 8 / 2;
		I_Print (bx, by, temp, cl.intermission_flags);

		elapsed -= size;
		if (elapsed <= 0)
			break;
	}

	if (i == lines && cl.lasting_time && elapsed >= 20*cl.lasting_time)
		CL_SetupIntermission (cl.intermission_next);
}
#endif	/* H2W */

//=============================================================================


/*
===============
SCR_TileClear

================
*/
static void SCR_TileClear (void)
{
    if (vid.conwidth > 320) {
	if (r_refdef.vrect.x > 0)
	{
		// left
		Draw_TileClear (0, 0, r_refdef.vrect.x, vid.height);
		// right
		Draw_TileClear (r_refdef.vrect.x + r_refdef.vrect.width, 0,
			vid.width - r_refdef.vrect.x + r_refdef.vrect.width, vid.height);
	}
//	if (r_refdef.vrect.y > 0) // if (r_refdef.vrect.height < vid.height - 44)
	{
		// top
		Draw_TileClear (r_refdef.vrect.x, 0,
			r_refdef.vrect.x + r_refdef.vrect.width, r_refdef.vrect.y);
		// bottom
		Draw_TileClear (r_refdef.vrect.x, r_refdef.vrect.y + r_refdef.vrect.height,
			r_refdef.vrect.width, vid.height - (r_refdef.vrect.height + r_refdef.vrect.y));
	}
    } else {
	if (r_refdef.vrect.x > 0)
	{
		// left
		Draw_TileClear (0, 0, r_refdef.vrect.x, vid.height - sb_lines);
		// right
		Draw_TileClear (r_refdef.vrect.x + r_refdef.vrect.width, 0,
			vid.width - r_refdef.vrect.x + r_refdef.vrect.width, vid.height - sb_lines);
	}
	if (r_refdef.vrect.y > 0)
	{
		// top
		Draw_TileClear (r_refdef.vrect.x, 0,
			r_refdef.vrect.x + r_refdef.vrect.width, r_refdef.vrect.y);
		// bottom
		Draw_TileClear (r_refdef.vrect.x, r_refdef.vrect.y + r_refdef.vrect.height,
			r_refdef.vrect.width, vid.height - sb_lines - (r_refdef.vrect.height + r_refdef.vrect.y));
	}
    }
}

//=============================================================================


/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.

WARNING: be very careful calling this from elsewhere, because the refresh
needs almost the entire 256k of stack space!
==================
*/
void SCR_UpdateScreen (void)
{
	if (block_drawing)
		return;

//	vid.numpages = (gl_triplebuffer.integer)? 3 : 2;
	scr_copytop = 0;
	scr_copyeverything = 0;

#if defined(H2W)
	if (scr_disabled_for_loading)
		return;
#else
	if (scr_disabled_for_loading)
	{
		if (realtime - scr_disabled_time > 25) {
		/* this can happen with clients connected to servers
		 * older than uHexen2-1.5.6 who don't issue an error
		 * upon changelevel failures. Or, it could happen if
		 * loading is taking a really long time.
		 */
			scr_disabled_for_loading = false;
			total_loading_size = 0;
			loading_stage = 0;
			Con_Printf ("load timeout.\n");
		}
		else {
			return;
		}
	}

	if (cls.state == ca_dedicated)
		return;		// stdout only
#endif	/* H2W */

	if (!scr_initialized || !con_initialized)
		return;		// not initialized yet

	GL_BeginRendering (&glx, &gly, &glwidth, &glheight);
	GL_PostProcess_BeginFrame ();

	SCR_UpdateZoom ();

//
// check for vid changes
//
	if (vid.recalc_refdef)
	{
		// something changed, so reorder the screen
		SCR_CalcRefdef ();
	}

//
// do 3D refresh drawing, and then update the screen
//
	SCR_SetUpToDrawConsole ();

#if FULLSCREEN_INTERMISSIONS
	// no need to draw view in fullscreen intermission screens
	if (!cl.intermission)
#endif
		V_RenderView ();

	GL_PostProcess_End3D ();
#ifndef USE_GLES
	/* Build Hi-Z pyramid for next frame's cull dispatch.  End3D already
	 * calls this in the pp_active path (where pp_depth_tex is ready).
	 * For the !pp_active path, drive the standalone depth resolve here
	 * so gl_hiz_cull works without an unrelated postprocess effect
	 * being enabled (uhexen2-9912). */
	if (!GL_PostProcess_Active())
		R_BuildHiZForNextFrame ();
#endif
	GL_Set2D ();
	SCR_TileClear ();	// draw any areas not covered by the refresh

#if defined(H2W)
	if (r_netgraph.integer)
		R_NetGraph ();
#endif	/* H2W */

	if (scr_drawdialog)
	{
		Sbar_Draw ();
		GL_SetCanvas (CANVAS_DEFAULT);
		Draw_FadeScreen ();
		SCR_DrawNotifyString ();
		scr_copyeverything = true;
	}
	else if (cl.intermission)
	{
#if !defined(H2W)
		SB_IntermissionOverlay();
		SCR_DrawDemoBar ();
		if (!(cl.intermission_flags & INTERMISSION_NO_MENUS))
		{
			SCR_DrawConsole();
			M_Draw();
		}

		if (scr_drawloading)
			SCR_DrawLoading();
#endif	/* H2W */
	}
#if !defined(H2W)
	else if (scr_drawloading)
	{
		/* No world (initial map load from main menu): paint the
		 * Hexen logo backdrop so the user sees the splash + LOADING
		 * plaque instead of black, since the back buffer is undefined
		 * after the previous SwapBuffers. Mid-game level transitions
		 * keep the vanilla "darken previous frame" feel. */
		if (!cl.worldmodel || cls.signon != SIGNONS)
			Draw_MenuBackdrop ();
		else
			Draw_FadeScreen ();
		SCR_DrawLoading ();
	}
#endif	/* H2W */
	else
	{
		/* In-game HUD overlays (crosshair, net/ram/turtle/pause icons,
		 * status bar) only make sense when a world is actually loaded.
		 * Skipping them when we're sitting at the main menu pre-game
		 * stops the disconnect-net icon from drawing on top of the
		 * menu backdrop. */
		qboolean have_world = (cl.worldmodel && cls.signon == SIGNONS);

		if (have_world)
		{
			if (crosshair.integer && !cls.demoplayback)
				Draw_Crosshair();

			SCR_DrawRam();
			SCR_DrawNet();
			SCR_DrawTurtle();
			SCR_DrawPause();
#if !defined(H2W)
			SCR_DrawPointFileLabel();
#endif
			SCR_CheckDrawCenterString();
#if !defined(SERVERONLY) && !defined(H2W)
			if (!CSQC_DrawHud ())
#endif
				Sbar_Draw();
			SCR_DrawDemoBar ();
		}
		/* Debug readouts on their own canvas so scr_infoscale can size
		 * them without moving the HUD, and vice versa (uhexen2-r9qj). */
		GL_SetCanvas (CANVAS_INFO);
		SCR_DrawFPS();
		SCR_DrawClock();
		SCR_DrawSpeed();

		GL_SetCanvas (CANVAS_DEFAULT);
		Plaque_Draw(plaquemessage, false);
		SCR_DrawConsole();
		M_Draw();

#if !defined(H2W)
		if (info_up)
		{
			UpdateInfoMessage();
			Info_Plaque_Draw(infomessage);
		}
#endif	/* H2W */
	}

	V_UpdatePalette ();

	GL_PostProcess_EndFrame ();

	GL_EndRendering ();
}

