/*
 * menu.c
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
#include "q_ctype.h"
#include "bgmusic.h"
#include "cdaudio.h"
#if defined(GLQUAKE)
#include "gl_postprocess.h"
#include "gl_vbo.h"
#include "gl_shader.h"
#include "gl_pipeline.h"
#endif
#include "sbar.h"
#include "sdl_inc.h"

void (*vid_menudrawfn)(void);

void (*vid_menukeyfn)(int key);

enum m_state_e	m_state;

/* -------------------------------------------------------------------------
 * ui_* menu interaction cvars (Ironwail parity, uhexen2-a5nn.14)
 *
 * Names, defaults and archive flags are Ironwail's (Quake/menu.c:31-34)
 * verbatim, so a config.cfg written by either engine means the same thing in
 * both.  What each one governs here is documented at its use site.
 * ------------------------------------------------------------------------- */
/* Mouse-driven menus, on by default because that is what this engine has always
 * done.  0 turns the pointer off in menus entirely -- no hover, no click -- for
 * anyone playing on a pad or a couch, or whose trackpad keeps moving the
 * selection out from under them.  in_sdl.c reads it at both entry points, which
 * is where the menu's mouse state comes from.  uhexen2-a5nn.36 */
cvar_t		ui_mouse          = {"ui_mouse",          "1",   CVAR_ARCHIVE};
static cvar_t	ui_live_preview   = {"ui_live_preview",   "1",   CVAR_ARCHIVE};
static cvar_t	ui_mouse_sound    = {"ui_mouse_sound",    "0",   CVAR_ARCHIVE};
static cvar_t	ui_sound_throttle = {"ui_sound_throttle", "0.1", CVAR_ARCHIVE};
static cvar_t	ui_search_timeout = {"ui_search_timeout", "1",   CVAR_ARCHIVE};

void M_Menu_Main_f (void);
static void M_Menu_SinglePlayer_f (void);
static void M_Menu_Load_f (void);
static void M_Menu_Save_f (void);
static void M_Menu_MultiPlayer_f (void);
static void M_Menu_Setup_f (void);
static void M_Menu_Net_f (void);
void M_Menu_Options_f (void);
static void M_Menu_Keys_f (void);
static void M_Menu_Video_f (void);
static void M_Menu_Help_f (void);
void M_Menu_Quit_f (void);
static void M_Menu_LanConfig_f (void);
static void M_Menu_GameOptions_f (void);
static void M_Menu_Mods_f (void);
static void M_Menu_Maps_f (void);
static void M_Menu_Search_f (void);
static void M_Menu_ServerList_f (void);

static void M_Main_Draw (void);
static void M_SinglePlayer_Draw (void);
static void M_Load_Draw (void);
static void M_Save_Draw (void);
static void M_MultiPlayer_Draw (void);
static void M_Setup_Draw (void);
static void M_Net_Draw (void);
static void M_Options_Draw (void);
static void M_Keys_Draw (void);
static void M_Video_Draw (void);
static void M_Mods_Draw (void);
static void M_Maps_Draw (void);
static void M_Help_Draw (void);
static void M_Quit_Draw (void);
static void M_LanConfig_Draw (void);
static void M_GameOptions_Draw (void);
static void M_Search_Draw (void);
static void M_ServerList_Draw (void);

static void M_Main_Key (int key);
static void M_SinglePlayer_Key (int key);
static void M_Load_Key (int key);
static void M_Save_Key (int key);
static void M_MultiPlayer_Key (int key);
static void M_Setup_Key (int key);
static void M_Net_Key (int key);
static void M_Options_Key (int key);
static void M_Keys_Key (int key);
static void M_Video_Key (int key);
static void M_Mods_Key (int key);
static void M_Maps_Key (int key);
static void M_Help_Key (int key);
static void M_Quit_Key (int key);
static void M_LanConfig_Key (int key);
static void M_GameOptions_Key (int key);
static void M_Search_Key (int key);
static void M_ServerList_Key (int key);


static qboolean	m_entersound;		// play after drawing a frame, so caching
					// won't disrupt the sound
static qboolean	m_recursiveDraw;

enum m_state_e	m_return_state;
qboolean	m_return_onerror;
char		m_return_reason [32];

qboolean	menu_disabled_mouse = false;

static float	TitlePercent = 0;
static float	TitleTargetPercent = 1;
static float	LogoPercent = 0;
static float	LogoTargetPercent = 1;

static int	setup_class;

static const char	*msave_message, *msave_message2;
static double	message_time;


static void M_ConfigureNetSubsystem(void);

#define StartingGame	(m_multiplayer_cursor == 1)
#define JoiningGame		(m_multiplayer_cursor == 0)

#define	_item_net_tcp		0	/* order of TCP menu entry */

#define	TCPIPConfig		(m_net_cursor == _item_net_tcp)


static void M_Menu_Class_f (void);

const char *ClassNames[MAX_PLAYER_CLASS] =
{
	"Paladin",
	"Crusader",
	"Necromancer",
	"Assassin",
	"Demoness"
};

static const char *ClassNamesU[MAX_PLAYER_CLASS] =
{
	"PALADIN",
	"CRUSADER",
	"NECROMANCER",
	"ASSASSIN",
	"DEMONESS"
};

#define	NUM_DIFFLEVELS		4

static const char *DiffNames[MAX_PLAYER_CLASS][NUM_DIFFLEVELS] =
{
	{	// Paladin
		"APPRENTICE",
		"SQUIRE",
		"ADEPT",
		"LORD"
	},

	{	// Crusader
		"GALLANT",
		"HOLY AVENGER",
		"DIVINE HERO",
		"LEGEND"
	},

	{	// Necromancer
		"SORCERER",
		"DARK SERVANT",
		"WARLOCK",
		"LICH KING"
	},

	{	// Assassin
		"ROGUE",
		"CUTTHROAT",
		"EXECUTIONER",
		"WIDOW MAKER"
	},
	{	// Demoness
		"LARVA",
		"SPAWN",
		"FIEND",
		"SHE BITCH"
	}
};


//=============================================================================
/* Mouse cursor support for menus */

extern int	menu_mouse_x, menu_mouse_y;
extern qboolean	menu_mouse_moved;

/* Convert a screen pixel Y to the menu canvas (0..200 logical) Y when
 * CANVAS_MENU is active, accounting for centering offset and scale. */
static int M_ScreenYToCanvasY (int screen_y);

/* ui_sound_throttle -- rate-limit a *repeated* menu sound.  Keyed on the
 * sample name, as upstream is (Ironwail Quake/menu.c:226): two different
 * sounds back to back are both wanted (a move followed by a confirm), but the
 * same one fired frame after frame is a pointer being dragged down a list,
 * and that machine-guns the channel.  0 disables the throttle. */
static char	m_lastsound[MAX_QPATH];
static double	m_lastsoundtime;

static void M_ThrottledSound (const char *sample)
{
	if (m_lastsoundtime > realtime)
		m_lastsoundtime = 0.0;	/* realtime restarted; re-arm */

	if (!strcmp (m_lastsound, sample) &&
	    realtime - m_lastsoundtime < ui_sound_throttle.value)
		return;

	q_strlcpy (m_lastsound, sample, sizeof(m_lastsound));
	m_lastsoundtime = realtime;
	S_LocalSound (sample);
}

/* ui_mouse_sound -- audible feedback as the pointer crosses rows.  Off by
 * default, which is both upstream's default and the right one for a menu
 * that is driven with the mouse and the arrow keys interchangeably. */
static void M_MouseSound (const char *sample)
{
	if (!ui_mouse_sound.integer)
		return;
	M_ThrottledSound (sample);
}

/* Sound once per row the pointer *enters*, not once per frame it rests there.
 * Keyed on (m_state, row) so that the same row index in a different menu still
 * counts as a new row, and so that leaving the rows entirely (item < 0) re-arms
 * without sounding.  Every caller is already gated on menu_mouse_moved, so a
 * stationary pointer cannot retrigger this. */
static void M_HoverSound (int item)
{
	static int	last_state = -1;
	static int	last_item = -1;

	if (item == last_item && (int)m_state == last_state)
		return;
	last_item = item;
	last_state = (int)m_state;

	if (item >= 0)
		M_MouseSound ("raven/menu1.wav");
}

/* Convert screen mouse position to menu-local coordinates.
 * Menu items are drawn at (76 + offset, 92 + cursor*8) in a viewport that is
 * 320 logical units wide and centered on screen.  It is NOT 200 tall: see
 * CANVAS_MENU in gl_draw.c, whose ortho height is glheight/scale, so the
 * visible Y range is at least 480 at every automatic scale.  Submenus longer
 * than 200 units rely on that: Rendering is the longest and its search prompt
 * currently sits at y=284, which is well inside 480.  (Written as "19 rows,
 * y=252" when the note went in; the list has grown twice since, which is why
 * the bound and not the count is what matters here.) */
static int M_MouseToMenuItem (int screen_y, int first_y, int item_height, int num_items)
{
	int vy, idx;

	/* Only answer when the pointer actually moved (or was clicked) this
	 * frame.  Every caller does `h = M_MouseToMenuItem(...); if (h >= 0)
	 * cursor = h;` unconditionally, so answering on a stationary mouse
	 * re-pins the cursor to whatever row the pointer happens to rest on,
	 * every frame -- the arrow keys then appear to do nothing at all.
	 * Gating here rather than at the fifteen call sites keeps the two
	 * input devices from fighting in one place.  uhexen2-u4iz. */
	if (!menu_mouse_moved)
		return -1;

	vy = M_ScreenYToCanvasY (screen_y);
	idx = (vy - first_y) / item_height;
	if (idx < 0) idx = -1;
	if (idx >= num_items) idx = -1;

	M_HoverSound (idx);
	return idx;
}

//=============================================================================
/* Support Routines */

/* When CANVAS_MENU is active in M_Draw, the viewport is a 320x200 logical
 * canvas already centered on the screen. The legacy `(vid.width - 320)>>1`
 * centering offset must be skipped or the menu draws off the canvas. */
static qboolean m_canvas_active;
static int M_CenterOfs (void)
{
	return m_canvas_active ? 0 : ((vid.width - 320) >> 1);
}

/* Mouse comes in as raw screen pixels (top-left origin). Convert to the
 * 320x200 menu canvas Y. Canvas is top-anchored, horizontally centered,
 * so canvas Y = screen Y / scale. */
static int M_ScreenYToCanvasY (int screen_y)
{
	float s;
	if (!m_canvas_active)
		return screen_y;
	s = SCR_CalcUIScale (&scr_menuscale);
	if (s > (float)glwidth / 320.0f) s = (float)glwidth / 320.0f;
	if (s > (float)glheight / 200.0f) s = (float)glheight / 200.0f;
	if (s < 0.0001f) s = 1.0f;
	return (int)(screen_y / s);
}

/*
================
M_DrawCharacter

Draws one solid graphics character, centered, on line
================
*/
void M_DrawCharacter (int cx, int line, int num)
{
	Draw_Character (cx + M_CenterOfs(), line, num);
}

void M_Print (int cx, int cy, const char *str)
{
	int charset_offset = 256; 
	
	while (*str)
	{
		if (str[0] == '\\' && str[1] == '1')
		{
			charset_offset = 0;
			str += 2;
		}
		else if (str[0] == '\\' && str[1] == '2')
		{
			charset_offset = 128;
			str += 2;
		}
		else if (str[0] == '\\' && str[1] == '3')
		{
			charset_offset = 256;
			str += 2;
		}
		else if (str[0] == '\\' && str[1] == '4')
		{
			charset_offset = 384;
			str += 2;
		}
		else
		{
			M_DrawCharacter(cx, cy, ((unsigned char)(*str)) + charset_offset);
			str++;
			cx += 8;
		}
	}
}

void M_PrintWhite (int cx, int cy, const char *str)
{
	while (*str)
	{
		M_DrawCharacter (cx, cy, (unsigned char)*str);
		str++;
		cx += 8;
	}
}

void M_DrawTransPic (int x, int y, qpic_t *pic)
{
	Draw_TransPic (x + M_CenterOfs(), y, pic);
}

void M_DrawPic (int x, int y, qpic_t *pic)
{
	Draw_Pic (x + M_CenterOfs(), y, pic);
}

static void M_DrawTransPicCropped (int x, int y, qpic_t *pic)
{
	Draw_TransPicCropped (x + M_CenterOfs(), y, pic);
}

static byte identityTable[256];
static byte translationTable[256];

static void M_BuildTranslationTable(int top, int bottom)
{
	int		j;
	byte	*dest, *source, *sourceA, *sourceB, *colorA, *colorB;

	for (j = 0; j < 256; j++)
		identityTable[j] = j;
	dest = translationTable;
	source = identityTable;
	memcpy (dest, source, 256);

	if (top > 10)
		top = 0;
	if (bottom > 10)
		bottom = 0;

	top -= 1;
	bottom -= 1;

	colorA = playerTranslation + 256 + color_offsets[(int)setup_class-1];
	colorB = colorA + 256;
	sourceA = colorB + 256 + (top * 256);
	sourceB = colorB + 256 + (bottom * 256);
	for (j = 0; j < 256; j++, colorA++, colorB++, sourceA++, sourceB++)
	{
		if (top >= 0 && (*colorA != 255))
			dest[j] = source[*sourceA];
		if (bottom >= 0 && (*colorB != 255))
			dest[j] = source[*sourceB];
	}
}


void M_DrawTextBox (int x, int y, int width, int lines)
{
	qpic_t	*p, *tm, *bm;
	int		cx, cy;
	int		n;

	// draw left side
	cx = x;
	cy = y;
	p = Draw_CachePic ("gfx/box_tl.lmp");
	M_DrawTransPic (cx, cy, p);
	p = Draw_CachePic ("gfx/box_ml.lmp");
	for (n = 0; n < lines; n++)
	{
		cy += 8;
		M_DrawTransPic (cx, cy, p);
	}
	p = Draw_CachePic ("gfx/box_bl.lmp");
	M_DrawTransPic (cx, cy+8, p);

	// draw middle
	cx += 8;
	tm = Draw_CachePic ("gfx/box_tm.lmp");
	bm = Draw_CachePic ("gfx/box_bm.lmp");
	while (width > 0)
	{
		cy = y;
		M_DrawTransPic (cx, cy, tm);
		p = Draw_CachePic ("gfx/box_mm.lmp");
		for (n = 0; n < lines; n++)
		{
			cy += 8;
			if (n == 1)
				p = Draw_CachePic ("gfx/box_mm2.lmp");
			M_DrawTransPic (cx, cy, p);
		}
		M_DrawTransPic (cx, cy+8, bm);
		width -= 2;
		cx += 16;
	}

	// draw right side
	cy = y;
	p = Draw_CachePic ("gfx/box_tr.lmp");
	M_DrawTransPic (cx, cy, p);
	p = Draw_CachePic ("gfx/box_mr.lmp");
	for (n = 0; n < lines; n++)
	{
		cy += 8;
		M_DrawTransPic (cx, cy, p);
	}
	p = Draw_CachePic ("gfx/box_br.lmp");
	M_DrawTransPic (cx, cy+8, p);
}

//=============================================================================

static int m_save_demonum;

/*
================
M_ToggleMenu_f
================
*/
void M_ToggleMenu_f (void)
{
	keydest_t dest = Key_GetDest();

	m_entersound = true;

	if (dest & key_menu)
	{
		if (m_state != m_main)
		{
			LogoTargetPercent = TitleTargetPercent = 1;
			LogoPercent = TitlePercent = 0;
			M_Menu_Main_f ();
			return;
		}
		Key_SetDest (key_game);
		m_state = m_none;
		Sbar_Changed ();
		return;
	}
	if (dest == key_console)
	{
		Con_ToggleConsole_f ();
	}
	else
	{
		LogoTargetPercent = TitleTargetPercent = 1;
		/* Only reset scroll if menu wasn't already showing —
		 * avoids a one-frame flash when title is already visible
		 * (e.g., pressing escape during boot demo). */
		if (m_state == m_none)
		{
			LogoPercent = TitlePercent = 0;
		}
		M_Menu_Main_f ();
	}
}

// Note: old version of demo has bigfont.lmp, not bigfont2.lmp
#define	BIGCHAR_FONT_FILE0	"gfx/menu/bigfont.lmp"
#define	BIGCHAR_FONT_FILE	"gfx/menu/bigfont2.lmp"
#define	BIGCHAR_WIDTH_FILE	"gfx/menu/fontsize.lmp"

static char	BigCharWidth[27][27];

static void M_BuildBigCharWidth (void)
{
	qpic_t		*p;
	byte		*source;
	int	ypos, xpos;
	int	numA, numB;
	int	biggestX, adjustment;
	char	After[20], Before[20];

	p = (qpic_t *)FS_LoadTempFile (BIGCHAR_FONT_FILE, NULL);
	if (!p) p = (qpic_t *)FS_LoadTempFile (BIGCHAR_FONT_FILE0, NULL);
	if (!p)
		Sys_Error ("Failed to load %s", BIGCHAR_FONT_FILE);
	SwapPic(p);

	for (numA = 0; numA < 27; numA++)
	{
		memset (After, 20, sizeof(After));
		source = p->data + ((numA % 8) * 20) + (numA / 8 * p->width * 20);
		biggestX = 0;

		for (ypos = 0; ypos < 19; ypos++)
		{
			for (xpos = 0; xpos < 19; xpos++, source++)
			{
				if (*source)
				{
					After[ypos] = xpos;
					if (xpos > biggestX)
						biggestX = xpos;
				}
			}
			source += (p->width - 19);
		}
		biggestX++;

		for (numB = 0; numB < 27; numB++)
		{
			memset (Before, 0, sizeof(Before));
			source = p->data + ((numB % 8) * 20) + (numB / 8 * p->width * 20);
			adjustment = 0;

			for (ypos = 0; ypos < 19; ypos++)
			{
				for (xpos = 0; xpos < 19; xpos++, source++)
				{
					if (!(*source))
					{
						Before[ypos]++;
					}
					else
						break;
				}
				source += (p->width - xpos);
			}

			while (1)
			{
				for (ypos = 0; ypos < 19; ypos++)
				{
					if (After[ypos] - Before[ypos] >= 15)
						break;
					Before[ypos]--;
				}
				if (ypos < 19)
					break;
				adjustment--;
			}
			BigCharWidth[numA][numB] = adjustment + biggestX;
		}
	}

	FS_CreatePath(FS_MakePath(FS_USERDIR, NULL, BIGCHAR_WIDTH_FILE));
	FS_WriteFile (BIGCHAR_WIDTH_FILE, BigCharWidth, sizeof(BigCharWidth));
}

static int M_DrawBigCharacter (int x, int y, int num, int numNext)
{
	int		add;

	if (num == ' ')
		return 32;

	if (num == '/')
		num = 26;
	else
		num -= 65;

	if (num < 0 || num >= 27)	// only a-z and /
		return 0;

	if (numNext == '/')
		numNext = 26;
	else
		numNext -= 65;

	Draw_BigCharacter (x, y, num);

	if (numNext < 0 || numNext >= 27)
		return 0;

	add = 0;
	if (num == (int)'C'-65 && numNext == (int)'P'-65)
		add = 3;

	return BigCharWidth[num][numNext] + add;
}

static void M_DrawBigString(int x, int y, const char *string)
{
	x += M_CenterOfs();

	while (*string)
	{
		x += M_DrawBigCharacter(x, y, string[0], string[1]);
		++string;
	}
}


void ScrollTitle (const char *name)
{
	qpic_t			*p;
	float		delta;
	int		finaly;
	static const char	*LastName = "";
	static qboolean		CanSwitch = true;

	if (TitlePercent < TitleTargetPercent)
	{
		delta = ((TitleTargetPercent-TitlePercent)/0.5)*host_frametime;
		if (delta < 0.004)
			delta = 0.004;
		TitlePercent += delta;
		if (TitlePercent > TitleTargetPercent)
		{
			TitlePercent = TitleTargetPercent;
		}
	}
	else if (TitlePercent > TitleTargetPercent)
	{
		delta = ((TitlePercent-TitleTargetPercent)/0.15)*host_frametime;
		if (delta < 0.02)
			delta = 0.02;
		TitlePercent -= delta;
		if (TitlePercent <= TitleTargetPercent)
		{
			TitlePercent = TitleTargetPercent;
			CanSwitch = true;
		}
	}

	if (LogoPercent < LogoTargetPercent)
	{
		/*
		delta = ((LogoTargetPercent-LogoPercent)/1.1)*host_frametime;
		if (delta < 0.0015)
			delta = 0.0015;
		*/
		delta = ((LogoTargetPercent-LogoPercent)/.15)*host_frametime;
		if (delta < 0.02)
			delta = 0.02;
		LogoPercent += delta;
		if (LogoPercent > LogoTargetPercent)
			LogoPercent = LogoTargetPercent;
	}

	if (q_strcasecmp(LastName,name) != 0 && TitleTargetPercent != 0)
		TitleTargetPercent = 0;

	if (CanSwitch)
	{
		LastName = name;
		CanSwitch = false;
		TitleTargetPercent = 1;
	}

	p = Draw_CachePic(LastName);
	finaly = ((float)p->height * TitlePercent) - p->height;
	M_DrawTransPicCropped( (320-p->width)/2, finaly , p);

	if (m_state != m_keys)
	{
		p = Draw_CachePic("gfx/menu/hplaque.lmp");
		finaly = ((float)p->height * LogoPercent) - p->height;
		M_DrawTransPicCropped(10, finaly, p);
	}
}


//=============================================================================
/* MAIN MENU */

static int	m_main_cursor;
#define	MAIN_ITEMS	7

static void BGM_RestartMusic(void);
static char	old_bgmtype[20];	// S.A
/* Snapshotted alongside old_bgmtype: the MIDI ONLY <-> ALL CODECS menu item
 * only flips bgm_extmusic and leaves bgmtype at "midi", so watching the
 * bgmtype string alone never noticed that change and the music was not
 * refreshed until the next level load.  -1 = nothing snapshotted, matching
 * old_bgmtype[0] == 0.  uhexen2-3j53. */
static int	old_extmusic = -1;


void M_Menu_Main_f (void)
{
	// Deactivate the mouse when the menus are drawn
	menu_disabled_mouse = true;
	IN_DeactivateMouse ();

	if (!(Key_GetDest() & key_menu))
	{
		m_save_demonum = cls.demonum;
		cls.demonum = -1;
	}
	Key_SetDest (key_menu);
	m_state = m_main;
	m_entersound = true;
}


/*
================
M_RendererName

The renderer this build actually runs, for the main-menu footer.  Keyed off the
same macros quakeinc.h keys its header set off, because gl_renderer_caps only
exists on the GLQUAKE side of that split.
================
*/
static const char *M_RendererName (void)
{
#if defined(WEBSOFT)
	return "Software 8bpp";
#elif defined(GLQUAKE)
	return gl_renderer_caps.profile_name ? gl_renderer_caps.profile_name
					     : "OpenGL";
#else
	return "Unknown renderer";
#endif
}


static void M_Main_Draw (void)
{
	int		f;

	ScrollTitle("gfx/menu/title0.lmp");
	M_DrawBigString (72, 60 + (0 * 20), "SINGLE PLAYER");
	M_DrawBigString (72, 60 + (1 * 20), "MULTIPLAYER");
	M_DrawBigString (72, 60 + (2 * 20), "OPTIONS");
	M_DrawBigString (72, 60 + (3 * 20), "MODS");
	M_DrawBigString (72, 60 + (4 * 20), "HELP");
	M_DrawBigString (72, 60 + (5 * 20), "INTRO");
	M_DrawBigString (72, 60 + (6 * 20), "QUIT");

	/* Mouse hover — update cursor position from mouse */
	{
		int hover = M_MouseToMenuItem(menu_mouse_y, 60, 20, MAIN_ITEMS);
		if (hover >= 0)
			m_main_cursor = hover;
	}

	f = (int)(realtime * 10)%8;
	M_DrawTransPicCropped (43, 54 + m_main_cursor * 20, Draw_CachePic( va("gfx/menu/menudot%i.lmp", f+1 ) ) );

	/* Which renderer this binary is.  The three client configurations are
	 * indistinguishable on screen until something is wrong, and a bug report
	 * that says "the water looks flat" is a different bug on each of them.
	 *
	 * Mirrors the version watermark Draw_ConsoleVersionInfo pins to the
	 * bottom-right (gl_draw.c), so the two boot-time identifiers sit at
	 * opposite ends of the same baseline.  That means the console canvas,
	 * not CANVAS_MENU: the menu canvas is only 320 wide and centred, so
	 * drawing here would land the label mid-screen on anything widescreen.
	 * Same 11px inset and 14px lift off the bottom as the watermark, and
	 * the same |0x100 charset. */
	{
		const char *r = M_RendererName();
		int i, y = vid.height - 14;	/* CANVAS_DEFAULT extent (a5nn.37) */

		GL_SetCanvas (CANVAS_DEFAULT);
		for (i = 0; r[i]; i++)
			Draw_Character (11 + i*8, y, r[i] | 0x100);
		GL_SetCanvas (CANVAS_MENU);
	}
}


static void M_Main_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		// leaving the main menu, reactivate mouse - S.A.
		menu_disabled_mouse = false;
		IN_ActivateMouse ();
		// and check we haven't changed the music type
		if (old_bgmtype[0] != 0 &&
		    (strcmp(old_bgmtype,bgmtype.string) != 0 ||
		     old_extmusic != bgm_extmusic.integer))
			BGM_RestartMusic ();
		old_bgmtype[0] = 0;
		old_extmusic = -1;
		Key_SetDest (key_game);
		m_state = m_none;
		Sbar_Changed ();
		cls.demonum = m_save_demonum;
		if (cls.demonum != -1 && !cls.demoplayback && cls.state != ca_connected)
			CL_NextDemo ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		if (++m_main_cursor >= MAIN_ITEMS)
			m_main_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		if (--m_main_cursor < 0)
			m_main_cursor = MAIN_ITEMS - 1;
		break;

	case K_ENTER:
		m_entersound = true;

		switch (m_main_cursor)
		{
		case 0:
			M_Menu_SinglePlayer_f ();
			break;

		case 1:
			M_Menu_MultiPlayer_f ();
			break;

		case 2:
			M_Menu_Options_f ();
			break;

		case 3:
			M_Menu_Mods_f ();
			break;

		case 4:
			M_Menu_Help_f ();
			break;

		case 5:
			SDL_MinimizeWindow (VID_GetWindow());
			SDL_OpenURL ("https://www.youtube.com/watch?v=3bJcIeH5r38&t=141s");
			break;

		case 6:
			M_Menu_Quit_f ();
			break;
		}
	}
}


//=============================================================================
/* DIFFICULTY MENU */

static void M_Menu_Difficulty_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_difficulty;
}

static int	m_diff_cursor;
static int	m_enter_portals = 0;
#define	DIFF_ITEMS	NUM_DIFFLEVELS

static void M_Difficulty_Draw (void)
{
	int	f, i;

	ScrollTitle("gfx/menu/title5.lmp");

	setup_class = cl_playerclass.integer;

	if (setup_class < 1 || setup_class > MAX_PLAYER_CLASS)
		setup_class = MAX_PLAYER_CLASS;
	if (setup_class > MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES && !(gameflags & GAME_PORTALS))
		setup_class = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
	setup_class--;

	for (i = 0; i < NUM_DIFFLEVELS; ++i)
		M_DrawBigString (72, 60 + (i * 20), DiffNames[setup_class][i]);

	{ int h = M_MouseToMenuItem(menu_mouse_y, 60, 20, DIFF_ITEMS); if (h >= 0) m_diff_cursor = h; }
	f = (int)(realtime * 10)%8;
	M_DrawTransPic (43, 54 + m_diff_cursor * 20, Draw_CachePic(va("gfx/menu/menudot%i.lmp", f+1)) );
}

static void M_NewMissionPackGame (void)
{
/* running a new single player mission pack game through
 * the menu system starts intermission screen #12, first.
 * when the user hits a key, Key_Event () gets us out of
 * the intermission by running the keep1 map.
 */
	Key_SetDest (key_game);
	cls.demonum = m_save_demonum;
	CL_SetupIntermission (12);
/* make sure the mouse is active, so that pressing a mouse
 * button can be captured by Key_Event (see above.) */
	menu_disabled_mouse = false;
	IN_ActivateMouse ();
}

static void M_Difficulty_Key (int key)
{
	switch (key)
	{
	case K_LEFTARROW:
	case K_RIGHTARROW:
		break;
	case K_ESCAPE:
		M_Menu_Class_f ();
		break;
	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		if (++m_diff_cursor >= DIFF_ITEMS)
			m_diff_cursor = 0;
		break;
	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		if (--m_diff_cursor < 0)
			m_diff_cursor = DIFF_ITEMS - 1;
		break;
	case K_ENTER:
		Cvar_SetValue ("skill", m_diff_cursor);
		m_entersound = true;
		m_state = m_none;
		if (m_enter_portals)
		{
			M_NewMissionPackGame ();
			return;
		}
		Cbuf_AddText ("wait\n"); /* make m_none to really work */

		//Launch the old mission (on a custom map if so specificied in the command line)
		int i = COM_CheckParm("-startold");
		if (i && i < com_argc - 1)
		{
			Cbuf_AddText("map ");
			Cbuf_AddText(com_argv[i + 1]);
			Cbuf_AddText("\n");
		}
		else
		{
			/* mods can set "startmap" in autoexec.cfg */
			if (startmap.string[0])
				Cbuf_AddText(va("map %s\n", startmap.string));
			else
				Cbuf_AddText("map demo1\n");
		}

		break;
	default:
		Key_SetDest (key_game);
		m_state = m_none;
		break;
	}
}


//=============================================================================
/* CLASS CHOICE MENU */

static int	class_flag;

static void M_Menu_Class_f (void)
{
	class_flag = 0;
	Key_SetDest (key_menu);
	m_state = m_class;
}

static void M_Menu_Class2_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_class;
	class_flag = 1;
}

static int	m_class_cursor;
#define	CLASS_ITEMS	MAX_PLAYER_CLASS

static void M_Class_Draw (void)
{
	int	i, f = MAX_PLAYER_CLASS;

	if (! (gameflags & GAME_PORTALS))
		f = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
#if DISALLOW_DEMONESS_IN_OLD_GAME
	else if (!m_enter_portals)
		f = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
#endif

	if (m_class_cursor >= f)
		m_class_cursor = 0;

	ScrollTitle("gfx/menu/title2.lmp");
	for (i = 0; i < f; ++i)
		M_DrawBigString (72, 60 + (i * 20), ClassNamesU[i]);

	{ int h = M_MouseToMenuItem(menu_mouse_y, 60, 20, f); if (h >= 0) m_class_cursor = h; }
	f = (int)(realtime * 10)%8;
	M_DrawTransPic (43, 54 + m_class_cursor * 20, Draw_CachePic(va("gfx/menu/menudot%i.lmp", f+1)) );

	M_DrawPic (251, 54 + 21, Draw_CachePicNoTrans (va("gfx/cport%d.lmp", m_class_cursor + 1)));
	M_DrawTransPic (242, 54, Draw_CachePic ("gfx/menu/frame.lmp"));
}

static void M_Class_Key (int key)
{
	int		f = MAX_PLAYER_CLASS;

	if (! (gameflags & GAME_PORTALS))
		f = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
#if DISALLOW_DEMONESS_IN_OLD_GAME
	else if (!m_enter_portals)
		f = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
#endif

	switch (key)
	{
	case K_LEFTARROW:
	case K_RIGHTARROW:
		break;
	case K_ESCAPE:
		M_Menu_SinglePlayer_f ();
		break;
	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
#if ENABLE_OLD_DEMO
		if (gameflags & GAME_OLD_DEMO)
			m_class_cursor = (m_class_cursor == CLASS_PALADIN-1) ? CLASS_THEIF-1 : CLASS_PALADIN-1;
		else
#endif	/* OLD_DEMO */
		if (++m_class_cursor >= f)
			m_class_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
#if ENABLE_OLD_DEMO
		if (gameflags & GAME_OLD_DEMO)
			m_class_cursor = (m_class_cursor == CLASS_PALADIN-1) ? CLASS_THEIF-1 : CLASS_PALADIN-1;
		else
#endif	/* OLD_DEMO */
		if (--m_class_cursor < 0)
			m_class_cursor = f - 1;
		break;

	case K_ENTER:
		Cbuf_AddText ( va ("playerclass %d\n", m_class_cursor+1) );
		m_entersound = true;
		if (!class_flag)
		{
			M_Menu_Difficulty_f();
		}
		else
		{
			Key_SetDest (key_game);
			m_state = m_none;
		}
		break;
	default:
		Key_SetDest (key_game);
		m_state = m_none;
		break;
	}
}


//=============================================================================
/* SINGLE PLAYER MENU */

#define SINGLEPLAYER_ITEMS	3
#define	SP_PORTALS_ITEMS	2

static int	m_singleplayer_cursor;

static void M_Menu_SinglePlayer_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_singleplayer;
	m_entersound = true;
	Cvar_Set ("timelimit", "0");		//put this here to help play single after dm
}

/* A custom mod rides on Portals data (GAME_PORTALS) but lives in its own
 * gamedir -- it is neither the real Portals mission pack nor the base game.
 * Both NEW and OLD MISSION launch the mod's "startmap" cvar; NEW additionally
 * plays intermission #12 first. For mods we relabel these Portals-specific
 * items for clarity (the big menu font only renders A-Z, space and '/').
 */
static qboolean M_RunningMod (void)
{
	return (gameflags & GAME_PORTALS) &&
		q_strcasecmp(fs_gamedir_nopath, "portals") != 0 &&
		q_strcasecmp(fs_gamedir_nopath, "data1") != 0;
}

static void M_SinglePlayer_Draw (void)
{
	int	f;
	qboolean mod = M_RunningMod();

	ScrollTitle("gfx/menu/title1.lmp");

	if (mod)
		M_DrawBigString (72, 60 + (0 * 20), "START W/ INTRO");
	else if (gameflags & GAME_PORTALS)
		M_DrawBigString (72, 60 + (0 * 20), "NEW MISSION");
	else
		M_DrawBigString (72, 60 + (0 * 20), "NEW GAME");

	M_DrawBigString (72, 60 + (1 * 20), "LOAD");
	M_DrawBigString (72, 60 + (2 * 20), "SAVE");

	if (gameflags & GAME_PORTALS)
	{
		M_DrawBigString (72, 60 + (3 * 20), mod ? "START" : "OLD MISSION");
		M_DrawBigString (72, 60 + (4 * 20), mod ? "MOD INTRO" : "PORTALS INTRO");
	}

	/* Mouse hover */
	{
		int items = (gameflags & GAME_PORTALS) ? SINGLEPLAYER_ITEMS + SP_PORTALS_ITEMS : SINGLEPLAYER_ITEMS;
		int hover = M_MouseToMenuItem(menu_mouse_y, 60, 20, items);
		if (hover >= 0)
			m_singleplayer_cursor = hover;
	}

	f = (int)(realtime * 10)%8;
	M_DrawTransPic (43, 54 + m_singleplayer_cursor * 20, Draw_CachePic(va("gfx/menu/menudot%i.lmp", f+1)) );
}


static void M_SinglePlayer_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;
	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		m_singleplayer_cursor++;
		if (gameflags & GAME_PORTALS)
		{
			if (m_singleplayer_cursor >= SINGLEPLAYER_ITEMS + SP_PORTALS_ITEMS)
				m_singleplayer_cursor = 0;
		}
		else
		{
			if (m_singleplayer_cursor >= SINGLEPLAYER_ITEMS)
				m_singleplayer_cursor = 0;
		}
		break;
	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		if (--m_singleplayer_cursor < 0)
		{
			if (gameflags & GAME_PORTALS)
				m_singleplayer_cursor = SINGLEPLAYER_ITEMS + SP_PORTALS_ITEMS - 1;
			else
				m_singleplayer_cursor = SINGLEPLAYER_ITEMS - 1;
		}
		break;
	case K_ENTER:
		m_entersound = true;
		m_enter_portals = 0;
		switch (m_singleplayer_cursor)
		{
		case 0:
			if (gameflags & GAME_PORTALS)
				m_enter_portals = 1;
		case 3:
			if (sv.active)
				if (!SCR_ModalMessage("Are you sure you want to\nstart a new game?\n", 0.0f))
					break;
			Key_SetDest (key_game);
			if (sv.active)
				Cbuf_AddText ("disconnect\n");
			Host_RemoveGIPFiles(NULL);
			Cbuf_AddText ("maxplayers 1\n");
			Cbuf_AddText ("coop 0\n");
			Cbuf_AddText ("deathmatch 0\n");
			M_Menu_Class_f ();
			break;

		case 1:
			M_Menu_Load_f ();
			break;

		case 2:
			M_Menu_Save_f ();
			break;
		case 4:
			if (gameflags & GAME_PORTALS)
			{
				Key_SetDest (key_game);
				Cbuf_AddText("playdemo t9\n");
			}
			break;
		}
	}
}

//=============================================================================
/* LOAD/SAVE MENU */

static int		load_cursor;		// 0 < load_cursor < MAX_SAVEGAMES

static char	m_filenames[MAX_SAVEGAMES][SAVEGAME_COMMENT_LENGTH+1];
static char	savefile[MAX_OSPATH];
static int		loadable[MAX_SAVEGAMES];

static void M_ScanSaves (void)
{
	int	i, j, version;
	char	name[MAX_OSPATH];
	FILE	*f;

	for (i = 0; i < MAX_SAVEGAMES; i++)
	{
		q_strlcpy (m_filenames[i], "--- UNUSED SLOT ---", SAVEGAME_COMMENT_LENGTH+1);
		loadable[i] = false;
		FS_MakePath_VABUF (FS_USERDIR, NULL, name, sizeof(name), "s%i/info.dat", i);
		f = fopen (name, "r");
		if (!f)
			continue;
		(void) fscanf (f, "%i\n", &version);
		if (version != SAVEGAME_VERSION)
		{
			fclose (f);
			continue;
		}
		(void) fscanf (f, "%79s\n", name);
		q_strlcpy (m_filenames[i], name, SAVEGAME_COMMENT_LENGTH+1);

	// change _ back to space
		for (j = 0; j < SAVEGAME_COMMENT_LENGTH; j++)
		{
			if (m_filenames[i][j] == '_')
				m_filenames[i][j] = ' ';
		}
		loadable[i] = true;
		fclose (f);
	}
}

static void M_Menu_Load_f (void)
{
	m_entersound = true;
	m_state = m_load;
	Key_SetDest (key_menu);
	M_ScanSaves ();
}


static void M_Menu_Save_f (void)
{
	if (!sv.active)
		return;
	if (cl.intermission)
		return;
	if (svs.maxclients != 1)
		return;
	m_entersound = true;
	m_state = m_save;
	Key_SetDest (key_menu);
	M_ScanSaves ();
}


static void M_Load_Draw (void)
{
	int		i;

	ScrollTitle("gfx/menu/load.lmp");

	for (i = 0; i < MAX_SAVEGAMES; i++)
		M_Print (16, 60 + 8*i, m_filenames[i]);

	{ int h = M_MouseToMenuItem(menu_mouse_y, 60, 8, MAX_SAVEGAMES); if (h >= 0) load_cursor = h; }
// line cursor
	M_DrawCharacter (8, 60 + load_cursor*8, 12+((int)(realtime*4)&1));
}


static void M_Save_Draw (void)
{
	int		i;

	ScrollTitle("gfx/menu/save.lmp");

	for (i = 0; i < MAX_SAVEGAMES; i++)
		M_Print (16, 60 + 8*i, m_filenames[i]);

	{ int h = M_MouseToMenuItem(menu_mouse_y, 60, 8, MAX_SAVEGAMES); if (h >= 0) load_cursor = h; }
// line cursor
	M_DrawCharacter (8, 60 + load_cursor*8, 12+((int)(realtime*4)&1));
}


static void M_Load_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_SinglePlayer_f ();
		break;

	case K_DEL:
		S_LocalSound ("raven/menu2.wav");
		if (!loadable[load_cursor])
			return;
		if (!SCR_ModalMessage("Are you sure you want to\ndelete this saved game?\n", 0.0f))
			return;
		FS_MakePath_VABUF (FS_USERDIR, NULL, savefile, sizeof(savefile), "s%i", load_cursor);
		Host_DeleteSave (savefile);
		M_ScanSaves ();
		break;

	case K_ENTER:
		S_LocalSound ("raven/menu2.wav");
		if (!loadable[load_cursor])
			return;
		m_state = m_none;
		Sbar_Changed ();
		Key_SetDest (key_game);

	// Host_Loadgame_f can't bring up the loading plaque because too much
	// stack space has been used, so do it now
		SCR_BeginLoadingPlaque ();

	// issue the load command
		Cbuf_AddText (va ("load s%i\n", load_cursor) );
		return;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("raven/menu1.wav");
		load_cursor--;
		if (load_cursor < 0)
			load_cursor = MAX_SAVEGAMES-1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("raven/menu1.wav");
		load_cursor++;
		if (load_cursor >= MAX_SAVEGAMES)
			load_cursor = 0;
		break;
	}
}


static void M_Save_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_SinglePlayer_f ();
		break;

	case K_DEL:
		S_LocalSound ("raven/menu2.wav");
		if (!loadable[load_cursor])
			return;
		if (!SCR_ModalMessage("Are you sure you want to\ndelete this saved game?\n", 0.0f))
			return;
		FS_MakePath_VABUF (FS_USERDIR, NULL, savefile, sizeof(savefile), "s%i", load_cursor);
		Host_DeleteSave (savefile);
		M_ScanSaves ();
		break;

	case K_ENTER:
		m_state = m_none;
		Sbar_Changed ();
		Key_SetDest (key_game);
		Cbuf_AddText (va("save s%i\n", load_cursor));
		menu_disabled_mouse = false;
		IN_ActivateMouse ();
		return;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("raven/menu1.wav");
		load_cursor--;
		if (load_cursor < 0)
			load_cursor = MAX_SAVEGAMES-1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("raven/menu1.wav");
		load_cursor++;
		if (load_cursor >= MAX_SAVEGAMES)
			load_cursor = 0;
		break;
	}
}


//=============================================================================
/* MULTIPLAYER LOAD/SAVE MENU */

static void M_ScanMSaves (void)
{
	int	i, j, version;
	char	name[MAX_OSPATH];
	FILE	*f;

	for (i = 0; i < MAX_SAVEGAMES; i++)
	{
		q_strlcpy (m_filenames[i], "--- UNUSED SLOT ---", SAVEGAME_COMMENT_LENGTH+1);
		loadable[i] = false;
		FS_MakePath_VABUF (FS_USERDIR, NULL, name, sizeof(name), "ms%i/info.dat", i);
		f = fopen (name, "r");
		if (!f)
			continue;
		(void) fscanf (f, "%i\n", &version);
		if (version != SAVEGAME_VERSION)
		{
			fclose (f);
			continue;
		}
		(void) fscanf (f, "%79s\n", name);
		q_strlcpy (m_filenames[i], name, SAVEGAME_COMMENT_LENGTH+1);

	// change _ back to space
		for (j = 0; j < SAVEGAME_COMMENT_LENGTH; j++)
		{
			if (m_filenames[i][j] == '_')
				m_filenames[i][j] = ' ';
		}
		loadable[i] = true;
		fclose (f);
	}
}

static void M_Menu_MLoad_f (void)
{
	m_entersound = true;
	m_state = m_mload;
	Key_SetDest (key_menu);
	M_ScanMSaves ();
}


static void M_Menu_MSave_f (void)
{
	if (!sv.active || cl.intermission || svs.maxclients == 1)
	{
		msave_message = "Only a network server";
		msave_message2 = "can save a multiplayer game";
		message_time = realtime;
		return;
	}
	m_entersound = true;
	m_state = m_msave;
	Key_SetDest (key_menu);
	M_ScanMSaves ();
}


static void M_MLoad_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_MultiPlayer_f ();
		break;

	case K_DEL:
		S_LocalSound ("raven/menu2.wav");
		if (!loadable[load_cursor])
			return;
		if (!SCR_ModalMessage("Are you sure you want to\ndelete this saved game?\n", 0.0f))
			return;
		FS_MakePath_VABUF (FS_USERDIR, NULL, savefile, sizeof(savefile), "ms%i", load_cursor);
		Host_DeleteSave (savefile);
		M_ScanMSaves ();
		break;

	case K_ENTER:
		S_LocalSound ("raven/menu2.wav");
		if (!loadable[load_cursor])
			return;
		m_state = m_none;
		Sbar_Changed ();
		Key_SetDest (key_game);

		if (sv.active)
			Cbuf_AddText ("disconnect\n");
		Cbuf_AddText ("listen 1\n");	// so host_netport will be re-examined

	// Host_Loadgame_f can't bring up the loading plaque because too much
	// stack space has been used, so do it now
		SCR_BeginLoadingPlaque ();

	// issue the load command
		Cbuf_AddText (va ("load ms%i\n", load_cursor) );
		return;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("raven/menu1.wav");
		load_cursor--;
		if (load_cursor < 0)
			load_cursor = MAX_SAVEGAMES-1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("raven/menu1.wav");
		load_cursor++;
		if (load_cursor >= MAX_SAVEGAMES)
			load_cursor = 0;
		break;
	}
}


static void M_MSave_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_MultiPlayer_f ();
		break;

	case K_DEL:
		S_LocalSound ("raven/menu2.wav");
		if (!loadable[load_cursor])
			return;
		if (!SCR_ModalMessage("Are you sure you want to\ndelete this saved game?\n", 0.0f))
			return;
		FS_MakePath_VABUF (FS_USERDIR, NULL, savefile, sizeof(savefile), "ms%i", load_cursor);
		Host_DeleteSave (savefile);
		M_ScanMSaves ();
		break;

	case K_ENTER:
		m_state = m_none;
		Sbar_Changed ();
		Key_SetDest (key_game);
		Cbuf_AddText (va("save ms%i\n", load_cursor));
		menu_disabled_mouse = false;
		IN_ActivateMouse ();
		return;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("raven/menu1.wav");
		load_cursor--;
		if (load_cursor < 0)
			load_cursor = MAX_SAVEGAMES-1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("raven/menu1.wav");
		load_cursor++;
		if (load_cursor >= MAX_SAVEGAMES)
			load_cursor = 0;
		break;
	}
}

//=============================================================================
/* MULTIPLAYER MENU */

static int	m_multiplayer_cursor;
#define	MULTIPLAYER_ITEMS	5

static void M_Menu_MultiPlayer_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_multiplayer;
	m_entersound = true;

	msave_message = NULL;
}


static void M_MultiPlayer_Draw (void)
{
	int	f;

	ScrollTitle("gfx/menu/title4.lmp");
//	M_DrawTransPic (72, 32, Draw_CachePic ("gfx/mp_menu.lmp") );

	M_DrawBigString (72, 60 + (0 * 20), "JOIN A GAME");
	M_DrawBigString (72, 60 + (1 * 20), "NEW GAME");
	M_DrawBigString (72, 60 + (2 * 20), "SETUP");
	M_DrawBigString (72, 60 + (3 * 20), "LOAD");
	M_DrawBigString (72, 60 + (4 * 20), "SAVE");

	{ int h = M_MouseToMenuItem(menu_mouse_y, 60, 20, MULTIPLAYER_ITEMS); if (h >= 0) m_multiplayer_cursor = h; }
	f = (int)(realtime * 10)%8;
	M_DrawTransPic (43, 54 + m_multiplayer_cursor * 20,Draw_CachePic( va("gfx/menu/menudot%i.lmp", f+1 ) ) );

	if (msave_message)
	{
		M_PrintWhite ((320/2) - ((27*8)/2), 168, msave_message);
		M_PrintWhite ((320/2) - ((27*8)/2), 176, msave_message2);
		if (realtime - 5 > message_time)
			msave_message = NULL;
	}

	if (tcpipAvailable)
		return;
	M_PrintWhite ((320/2) - ((27*8)/2), 160, "No Communications Available");
}


static void M_MultiPlayer_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		if (++m_multiplayer_cursor >= MULTIPLAYER_ITEMS)
			m_multiplayer_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		if (--m_multiplayer_cursor < 0)
			m_multiplayer_cursor = MULTIPLAYER_ITEMS - 1;
		break;

	case K_ENTER:
		m_entersound = true;
		switch (m_multiplayer_cursor)
		{
		case 0:
			if (tcpipAvailable)
				M_Menu_Net_f ();
			break;

		case 1:
			if (tcpipAvailable)
				M_Menu_Net_f ();
			break;

		case 2:
			M_Menu_Setup_f ();
			break;

		case 3:
			M_Menu_MLoad_f ();
			break;

		case 4:
			M_Menu_MSave_f ();
			break;
		}
	}
}

//=============================================================================
/* SETUP MENU */

static int		setup_cursor = 5;
static const int	setup_cursor_table[] = {40, 56, 80, 104, 128, 156};

static char	setup_hostname[16];
static char	setup_myname[16];
static int		setup_oldtop;
static int		setup_oldbottom;
static int		setup_top;
static int		setup_bottom;

#define	NUM_SETUP_CMDS	6

static void M_Menu_Setup_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_setup;
	m_entersound = true;
	q_strlcpy(setup_myname, cl_name.string, sizeof(setup_myname));
	q_strlcpy(setup_hostname, hostname.string, sizeof(setup_hostname));
	setup_top = setup_oldtop = (cl_color.integer >> 4) & 15;
	setup_bottom = setup_oldbottom = cl_color.integer & 15;
	setup_class = cl_playerclass.integer;
	if (setup_class < 1 || setup_class > MAX_PLAYER_CLASS)
		setup_class = MAX_PLAYER_CLASS;
#if ENABLE_OLD_DEMO
	if (gameflags & GAME_OLD_DEMO)
	{
		if (setup_class != CLASS_PALADIN && setup_class != CLASS_THEIF)
			setup_class = CLASS_PALADIN;
	}
	else
#endif	/* OLD_DEMO */
	if (!(gameflags & GAME_PORTALS))
	{
		if (setup_class > MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES)
			setup_class = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
	}
}


static void M_DrawTransPicTranslate (int x, int y, qpic_t *pic, int p_class)
{
	Draw_TransPicTranslate (x + M_CenterOfs(), y, pic, translationTable, p_class);
}

static void M_Setup_Draw (void)
{
	qpic_t	*p;

	ScrollTitle("gfx/menu/title4.lmp");

	M_Print (64, 40, "Hostname");
	M_DrawTextBox (160, 32, 16, 1);
	M_Print (168, 40, setup_hostname);

	M_Print (64, 56, "Your name");
	M_DrawTextBox (160, 48, 16, 1);
	M_Print (168, 56, setup_myname);

	M_Print (64, 80, "Current Class: ");
	M_Print (88, 88, ClassNames[setup_class-1]);

	M_Print (64, 104, "First color patch");
	M_Print (64, 128, "Second color patch");

	M_DrawTextBox (64, 156-8, 14, 1);
	M_Print (72, 156, "Accept Changes");

	p = Draw_CachePic (va("gfx/menu/netp%i.lmp",setup_class));
	M_BuildTranslationTable(setup_top, setup_bottom);

	/* garymct */
	M_DrawTransPicTranslate (220, 72, p, setup_class);

	M_DrawCharacter (56, setup_cursor_table [setup_cursor], 12+((int)(realtime*4)&1));

	if (setup_cursor == 0)
		M_DrawCharacter (168 + 8*strlen(setup_hostname), setup_cursor_table [setup_cursor], 10+((int)(realtime*4)&1));

	if (setup_cursor == 1)
		M_DrawCharacter (168 + 8*strlen(setup_myname), setup_cursor_table [setup_cursor], 10+((int)(realtime*4)&1));
}


static void M_Setup_Key (int k)
{
	int		l;

	switch (k)
	{
	case K_ESCAPE:
		M_Menu_MultiPlayer_f ();
		break;

	case K_ENTER:
		if (setup_cursor == 0 || setup_cursor == 1)
			return;

		if (setup_cursor == 2 || setup_cursor == 3 || setup_cursor == 4)
			goto forward;

		if (strcmp(cl_name.string, setup_myname) != 0)
			Cbuf_AddText ( va ("name \"%s\"\n", setup_myname) );
		if (strcmp(hostname.string, setup_hostname) != 0)
			Cvar_Set("hostname", setup_hostname);
		if (setup_top != setup_oldtop || setup_bottom != setup_oldbottom)
			Cbuf_AddText( va ("color %i %i\n", setup_top, setup_bottom) );
		Cbuf_AddText ( va ("playerclass %d\n", setup_class) );
		m_entersound = true;
		M_Menu_MultiPlayer_f ();
		break;

	case K_BACKSPACE:
		if (setup_cursor == 0)
		{
			if (strlen(setup_hostname))
				setup_hostname[strlen(setup_hostname)-1] = 0;
		}

		if (setup_cursor == 1)
		{
			if (strlen(setup_myname))
				setup_myname[strlen(setup_myname)-1] = 0;
		}
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		setup_cursor--;
		if (setup_cursor < 0)
			setup_cursor = NUM_SETUP_CMDS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		setup_cursor++;
		if (setup_cursor >= NUM_SETUP_CMDS)
			setup_cursor = 0;
		break;

	case K_LEFTARROW:
		if (setup_cursor < 2)
			return;
		S_LocalSound ("raven/menu3.wav");
		if (setup_cursor == 2)
		{
#if ENABLE_OLD_DEMO
			if (gameflags & GAME_OLD_DEMO)
			{
				setup_class = (setup_class == CLASS_PALADIN) ? CLASS_THEIF : CLASS_PALADIN;
				break;
			}
#endif	/* OLD_DEMO */
			setup_class--;
			if (setup_class < 1)
				setup_class = MAX_PLAYER_CLASS;
			if (setup_class > MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES && !(gameflags & GAME_PORTALS))
				setup_class = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
		}
		else if (setup_cursor == 3)
			setup_top = setup_top - 1;
		else if (setup_cursor == 4)
			setup_bottom = setup_bottom - 1;
		break;
	case K_RIGHTARROW:
		if (setup_cursor < 2)
			return;
forward:
		S_LocalSound ("raven/menu3.wav");
		if (setup_cursor == 2)
		{
#if ENABLE_OLD_DEMO
			if (gameflags & GAME_OLD_DEMO)
			{
				setup_class = (setup_class == CLASS_PALADIN) ? CLASS_THEIF : CLASS_PALADIN;
				break;
			}
#endif	/* OLD_DEMO */
			setup_class++;
			if (setup_class > MAX_PLAYER_CLASS)
				setup_class = 1;
			if (setup_class > MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES && !(gameflags & GAME_PORTALS))
				setup_class = 1;
		}
		else if (setup_cursor == 3)
			setup_top = setup_top + 1;
		else if (setup_cursor == 4)
			setup_bottom = setup_bottom + 1;
		break;

	default:
		if (k < 32 || k > 127)
			break;
		if (setup_cursor == 0)
		{
			l = strlen(setup_hostname);
			if (l < 15)
			{
				setup_hostname[l+1] = 0;
				setup_hostname[l] = k;
			}
		}
		if (setup_cursor == 1)
		{
			l = strlen(setup_myname);
			if (l < 15)
			{
				setup_myname[l+1] = 0;
				setup_myname[l] = k;
			}
		}
	}

	if (setup_top > 10)
		setup_top = 0;
	else if (setup_top < 0)
		setup_top = 10;
	if (setup_bottom > 10)
		setup_bottom = 0;
	else if (setup_bottom < 0)
		setup_bottom = 10;
}

//=============================================================================
/* NET MENU */

#define NET_ITEMS	1

static int	m_net_cursor = 0;

static const char *net_helpMessage[] =
{
/* .........1.........2.... */
  " Commonly used to play  ",
  " over the Internet, but ",
  " also used on a Local   ",
  " Area Network.          "
};

static void M_Menu_Net_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_net;
	m_entersound = true;

	if (m_net_cursor >= NET_ITEMS)
		m_net_cursor = 0;
	m_net_cursor--;
	M_Net_Key (K_DOWNARROW);
}


static void M_Net_Draw (void)
{
	int	f;

	ScrollTitle("gfx/menu/title4.lmp");

	M_DrawBigString (72, 72 + (_item_net_tcp * 20), "TCP/IP");

	f = (320 - 26*8) / 2;
	M_DrawTextBox (f, 142, 24, 4);
	f += 8;
	M_Print (f, (142 + 1*8), net_helpMessage[m_net_cursor*4 + 0]);
	M_Print (f, (142 + 2*8), net_helpMessage[m_net_cursor*4 + 1]);
	M_Print (f, (142 + 3*8), net_helpMessage[m_net_cursor*4 + 2]);
	M_Print (f, (142 + 4*8), net_helpMessage[m_net_cursor*4 + 3]);

	f = (int)(realtime * 10)%8;
	M_DrawTransPic (43, 64 + m_net_cursor * 20, Draw_CachePic(va("gfx/menu/menudot%i.lmp", f+1)) );
}

static void M_Net_Key (int k)
{
again:
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_MultiPlayer_f ();
		break;

	case K_DOWNARROW:
// Tries to re-draw the menu here, and m_net_cursor could be set to -1
//		S_LocalSound ("raven/menu1.wav");
		if (++m_net_cursor >= NET_ITEMS)
			m_net_cursor = 0;
		break;

	case K_UPARROW:
//		S_LocalSound ("raven/menu1.wav");
		if (--m_net_cursor < 0)
			m_net_cursor = NET_ITEMS - 1;
		break;

	case K_ENTER:
		m_entersound = true;
		switch (m_net_cursor)
		{
		case _item_net_tcp:
			M_Menu_LanConfig_f ();
			break;
		default:
		// multiprotocol
			break;
		}
		break;
	}

	if (TCPIPConfig && !tcpipAvailable)
		goto again;

	switch (k)
	{
		case K_DOWNARROW:
		case K_UPARROW:
			S_LocalSound ("raven/menu1.wav");
			break;
	}
}


//=============================================================================
/* OPTIONS MENU */

#define	SLIDER_RANGE	10

enum
{
	OPT_CUSTOMIZE = 0,
	OPT_DISPLAY,
	OPT_SOUND,
	OPT_GAME,
	OPT_GAMEPAD,
	OPT_CONSOLE,
	OPT_DEFAULTS,
	OPTIONS_ITEMS
};

// prototypes for submenus
static void M_Menu_Display_f (void);
static void M_Display_Draw (void);
static void M_Display_Key (int k);
static qboolean M_Display_IsSkip (int cursor);
static void M_Menu_Sound_f (void);
static void M_Sound_Draw (void);
static void M_Sound_Key (int k);
static void M_Menu_Game_f (void);
static void M_Game_Draw (void);
static void M_Game_Key (int k);

// prototypes for the rendering and graphics submenus
static void M_Menu_Rendering_f (void);
static void M_Rendering_Draw (void);
static void M_Rendering_Key (int k);
static void M_Menu_Graphics_f (void);
static void M_Graphics_Draw (void);
static void M_Graphics_Key (int k);

// prototypes for the gamepad menu
static void M_Menu_Gamepad_f (void);
static void M_Gamepad_Draw (void);
static void M_Gamepad_Key (int k);

/* -------------------------------------------------------------------------
 * Menu search/filter (uhexen2-rawq)
 *
 * Shared substring filter for long option submenus.  Each integrating menu
 * provides a `const char *labels[ITEMS]` table; while m_search_len > 0
 * the menu's IsSkip helper hides rows whose label doesn't contain the
 * (case-insensitive) search buffer.  Initial integration: Rendering
 * submenu (REND_ITEMS, the biggest list).  Pattern propagates to other
 * submenus by adding a labels table + IsSkip + search hooks in *_Key.
 * ------------------------------------------------------------------------- */
#define M_SEARCH_BUFLEN	24
static char m_search_buf[M_SEARCH_BUFLEN];
static int  m_search_len = 0;
static double m_search_time = 0.0;	/* realtime of the last edit; see M_Filter_Think */

static qboolean M_Filter_Active (void)
{
	return m_search_len > 0;
}

static void M_Filter_Clear (void)
{
	m_search_buf[0] = 0;
	m_search_len = 0;
	m_search_time = 0.0;
}

/* Case-insensitive substring match.  Always-true when search inactive
 * so callers can use a single gating predicate. */
static qboolean M_Filter_Matches (const char *label)
{
	const char *p;
	int n = m_search_len;
	int i;

	if (n <= 0 || !label)
		return true;

	for (p = label; *p; ++p)
	{
		for (i = 0; i < n; ++i)
		{
			char a = p[i];
			char b = m_search_buf[i];
			if (!a)
				return false;
			if (q_tolower((unsigned char)a) != q_tolower((unsigned char)b))
				break;
		}
		if (i == n)
			return true;
	}
	return false;
}

/* Returns true if the key was consumed by the search input.  Callers
 * should treat this as "buffer changed — re-snap cursor to first match". */
static qboolean M_Filter_HandleKey (int k)
{
	if (k == K_BACKSPACE)
	{
		if (m_search_len <= 0)
			return false;	/* let ESC/etc. propagate when buffer empty */
		m_search_buf[--m_search_len] = 0;
		m_search_time = realtime;
		S_LocalSound ("raven/menu2.wav");
		return true;
	}
	if (k >= 32 && k < K_BACKSPACE)
	{
		if (m_search_len < M_SEARCH_BUFLEN - 1)
		{
			m_search_buf[m_search_len++] = (char)k;
			m_search_buf[m_search_len] = 0;
			m_search_time = realtime;
			S_LocalSound ("raven/menu2.wav");
		}
		return true;
	}
	return false;
}

/* Render "Search: foo_" prompt — call at bottom of host menu draw. */
static void M_Filter_Draw (int x, int y)
{
	if (!M_Filter_Active())
		return;
	M_Print (x, y, "Search:");
	M_PrintWhite (x + 8*8, y, m_search_buf);
	if (((int)(realtime * 4) & 1) == 0)
		M_DrawCharacter (x + 8*8 + 8 * m_search_len, y, 11);
}

static int	options_cursor;

void M_Menu_Options_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_options;
	m_entersound = true;
}


static void M_DrawSlider (int x, int y, float range)
{
	int	i;

	if (range < 0)
		range = 0;
	else if (range > 1)
		range = 1;
	M_DrawCharacter (x-8, y, 256);
	for (i = 0; i < SLIDER_RANGE; i++)
		M_DrawCharacter (x + i*8, y, 257);
	M_DrawCharacter (x + i*8, y, 258);
	M_DrawCharacter (x + (SLIDER_RANGE-1)*8 * range, y, 259);
}

static void M_DrawSliderValue (int x, int y, float range, const char *fmt, float value)
{
	char	buf[32];

	M_DrawSlider (x, y, range);
	snprintf(buf, sizeof(buf), fmt, value);
	M_PrintWhite (x + 100, y, buf);
}

void M_DrawCheckbox (int x, int y, int on)
{
	if (on)
		M_Print (x, y, "on");
	else
		M_Print (x, y, "off");
}

static void M_Options_Draw (void)
{
	menu_disabled_mouse = false;
	IN_ActivateMouse ();

	ScrollTitle("gfx/menu/title3.lmp");

	M_Print (76, 92 + 8*OPT_CUSTOMIZE,	"Key Setup");
	M_Print (76, 92 + 8*OPT_DISPLAY,	"Display");
	M_Print (76, 92 + 8*OPT_SOUND,		"Sound");
	M_Print (76, 92 + 8*OPT_GAME,		"Game");
	M_Print (76, 92 + 8*OPT_GAMEPAD,	"Controller");
	M_Print (76, 92 + 8*OPT_CONSOLE,	"Go to Console");
	M_Print (76, 92 + 8*OPT_DEFAULTS,	"Reset to Defaults");

	/* Mouse hover */
	{
		int hover = M_MouseToMenuItem(menu_mouse_y, 92, 8, OPTIONS_ITEMS);
		if (hover >= 0)
			options_cursor = hover;
	}

	// cursor
	M_DrawCharacter (64, 92 + 8*options_cursor, 12 + ((int)(realtime*4) & 1));
}


static void M_Options_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;

	case K_ENTER:
		m_entersound = true;
		switch (options_cursor)
		{
		case OPT_CUSTOMIZE:
			M_Menu_Keys_f ();
			break;
		case OPT_DISPLAY:
			M_Menu_Display_f ();
			break;
		case OPT_SOUND:
			M_Menu_Sound_f ();
			break;
		case OPT_GAME:
			M_Menu_Game_f ();
			break;
		case OPT_GAMEPAD:
			M_Menu_Gamepad_f ();
			break;
		case OPT_CONSOLE:
			m_state = m_none;
			Con_ToggleConsole_f ();
			break;
		case OPT_DEFAULTS:
			/* Re-bind keys from default.cfg, then enable mouse-look —
			 * default.cfg only sets bindings, not the mlook state, so
			 * a fresh install hitting Reset Defaults still gets
			 * classic mouse-turn-only without the +mlook here. */
			Cbuf_AddText ("unbindall\nexec default.cfg\n+mlook\n");
			vid.recalc_refdef = 1;
			break;
		}
		return;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		options_cursor--;
		if (options_cursor < 0)
			options_cursor = OPTIONS_ITEMS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		options_cursor++;
		if (options_cursor >= OPTIONS_ITEMS)
			options_cursor = 0;
		break;
	}
}


//=============================================================================
/* DISPLAY MENU */

enum
{
	DISP_PRESET = 0,
	DISP_GAMMA,
	DISP_CONTRAST,
#ifdef GLQUAKE
	DISP_CONSCALE,
	DISP_PIXELASPECT,
#endif
	DISP_SCRSIZE,
	DISP_RENDERING,	/* enters rendering submenu */
	DISP_GRAPHICS,		/* enters graphics/misc submenu */
	DISP_SEP,		/* separator between rendering and video */
	DISP_FULLSCREEN,
	DISP_RESOLUTION,
	DISP_MSAA,
	DISP_VSYNC,
	DISP_MAXFPS,
	DISP_SHOWFPS,
	DISP_MENUFADE,
	DISP_MENUFADEALPHA,
	DISP_SEP2,		/* separator before apply */
	DISP_APPLY,
	DISP_ITEMS
};

static int	display_cursor;

/* Label table for substring search (uhexen2-rawq).  Mirrors DISP_* enum.
 * Separators / dynamic-skip rows get NULL; they're already filtered out
 * by the existing M_Display_IsSkip logic. */
static const char *disp_labels[DISP_ITEMS] = {
	"Preset        :",	/* DISP_PRESET */
	"Brightness    :",	/* DISP_GAMMA */
	"Contrast      :",	/* DISP_CONTRAST */
#ifdef GLQUAKE
	"Console Scale :",	/* DISP_CONSCALE */
	"2D Aspect     :",	/* DISP_PIXELASPECT */
#endif
	"HUD Layout    :",	/* DISP_SCRSIZE */
	"Rendering",		/* DISP_RENDERING */
	"Misc",			/* DISP_GRAPHICS */
	NULL,			/* DISP_SEP */
	"Window Mode   :",	/* DISP_FULLSCREEN */
	"Resolution    :",	/* DISP_RESOLUTION */
	"Antialiasing  :",	/* DISP_MSAA */
	"VSync         :",	/* DISP_VSYNC */
	"FPS Limit     :",	/* DISP_MAXFPS */
	"Show FPS      :",	/* DISP_SHOWFPS */
	"Menu Backdrop :",	/* DISP_MENUFADE */
	"Backdrop Dim  :",	/* DISP_MENUFADEALPHA */
	NULL,			/* DISP_SEP2 */
	"APPLY CHANGES",	/* DISP_APPLY */
};

static void M_Menu_Display_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_display;
	m_entersound = true;
	M_Filter_Clear ();
#ifdef GLQUAKE
	VID_MenuInit ();
#endif
}

/* Detect the active display preset by matching key cvars.
 * Returns 0=User, 1=Crunchy, 2=Retro, 3=Authentic, 4=Faithful, 5=Clean,
 * 6=Modern, 7=Ultra. */
static int M_Display_DetectPreset (void)
{
	int se = (int)r_softemu.value;
	float sc = r_scale.value;
	int glow = (int)Cvar_VariableValue("gl_glows");
	float mb = Cvar_VariableValue("r_motionblur");
	int lmb = r_lightmap_bicubic.integer;

	if (sc <= 0.25f && se == 1 && !lmb)
		return 1;	/* Crunchy */
	if (sc <= 0.5f && se == 2 && !lmb)
		return 2;	/* Retro */
	if (sc <= 0.5f && se == 3 && !lmb)
		return 3;	/* Authentic */
	if (sc >= 1.0f && se == 0 && gl_filter_idx <= 1 && glow && !lmb)
		return 4;	/* Faithful */
	if (sc >= 1.0f && se == 0 && gl_filter_idx == 2 && glow && !lmb)
		return 5;	/* Clean */
	if (sc >= 1.0f && se == 0 && gl_filter_idx >= 3 && mb <= 0 && lmb)
		return 6;	/* Modern */
	if (sc >= 1.0f && se == 0 && gl_filter_idx >= 3 && mb > 0 && lmb)
		return 7;	/* Ultra */
	return 0;	/* User — custom settings, no preset matches */
}

static void M_Display_AdjustSliders (int dir)
{
	float	f;

	S_LocalSound ("raven/menu3.wav");

	switch (display_cursor)
	{
	case DISP_PRESET:
	{
		/* Cycle through presets (skip "User" — that's auto-detected).
		 * 1=Crunchy, 2=Retro, 3=Authentic, 4=Faithful, 5=Clean,
		 * 6=Modern, 7=Ultra.
		 * Snap to the actual cvar state before stepping so the cycle
		 * starts from where we really are, not a stale static.  When
		 * detection returns 0 (User) keep the last known preset. */

		static int preset = 6;
		int detected = M_Display_DetectPreset();
		if (detected != 0)
			preset = detected;
		preset += dir;
		if (preset < 1) preset = 7;
		if (preset > 7) preset = 1;

#define PRESET_COMMON \
	Cvar_SetValue ("gl_flashblend", 0); \
	Cvar_SetValue ("r_dynamic", 1); \
	Cvar_SetValue ("gl_torch_dlight", 1); \
	Cvar_SetValue ("scr_menubgstyle", 0);

		if (preset == 1)	/* Crunchy — 25% scale, maximum lo-fi */
		{
			Cvar_SetValue ("r_scale", 0.25f);
			Cvar_SetValue ("r_softemu", 1);
			Cvar_SetValue ("r_dither", 1.0f);
			Cvar_Set ("gl_texturemode", "GL_NEAREST_MIPMAP_NEAREST");
			Cvar_SetValue ("gl_texture_anisotropy", 1);
			Cvar_SetValue ("gl_fullbrights", 1);
			Cvar_SetValue ("gl_fxaa", 0);
			Cvar_SetValue ("r_lightmap_bicubic", 0);
			Cvar_SetValue ("r_watercolor", 0);
			Cvar_SetValue ("r_waterwarp", 0);
			Cvar_SetValue ("r_motionblur", 0);
			Cvar_SetValue ("gl_glows", 0);
			Cvar_SetValue ("gl_missile_glows", 0);
			Cvar_SetValue ("gl_other_glows", 0);
			Cvar_SetValue ("gl_glow_intensity", 0.4f);
			Cvar_SetValue ("gl_torch_dlight", 1);
			Cvar_SetValue ("r_lerpmodels", 0);	/* snappy lo-fi animation */
			PRESET_COMMON
		}
		else if (preset == 2)	/* Retro — half res, banded, old-school */
		{
			Cvar_SetValue ("r_scale", 0.5f);
			Cvar_SetValue ("r_softemu", 2);
			Cvar_SetValue ("r_dither", 0.5f);
			Cvar_Set ("gl_texturemode", "GL_NEAREST_MIPMAP_NEAREST");
			Cvar_SetValue ("gl_texture_anisotropy", 1);
			Cvar_SetValue ("gl_fullbrights", 1);
			Cvar_SetValue ("gl_fxaa", 0);
			Cvar_SetValue ("r_lightmap_bicubic", 0);
			Cvar_SetValue ("r_watercolor", 0);
			Cvar_SetValue ("r_waterwarp", 1);
			Cvar_SetValue ("r_motionblur", 0);
			Cvar_SetValue ("gl_glows", 0);
			Cvar_SetValue ("gl_missile_glows", 0);
			Cvar_SetValue ("gl_other_glows", 0);
			Cvar_SetValue ("gl_glow_intensity", 0.4f);
			Cvar_SetValue ("gl_torch_dlight", 1);
			Cvar_SetValue ("r_lerpmodels", 0);	/* snappy lo-fi animation */
			PRESET_COMMON
		}
		else if (preset == 3)	/* Authentic — Retro, but quantized through Raven's own colormap */
		{
			Cvar_SetValue ("r_scale", 0.5f);
			Cvar_SetValue ("r_softemu", 3);
			Cvar_SetValue ("r_dither", 0.5f);
			Cvar_Set ("gl_texturemode", "GL_NEAREST_MIPMAP_NEAREST");
			Cvar_SetValue ("gl_texture_anisotropy", 1);
			Cvar_SetValue ("gl_fullbrights", 1);
			Cvar_SetValue ("gl_fxaa", 0);
			Cvar_SetValue ("r_lightmap_bicubic", 0);
			Cvar_SetValue ("r_watercolor", 0);
			Cvar_SetValue ("r_waterwarp", 1);
			Cvar_SetValue ("r_motionblur", 0);
			Cvar_SetValue ("gl_glows", 0);
			Cvar_SetValue ("gl_missile_glows", 0);
			Cvar_SetValue ("gl_other_glows", 0);
			Cvar_SetValue ("gl_glow_intensity", 0.4f);
			Cvar_SetValue ("gl_torch_dlight", 1);
			Cvar_SetValue ("r_lerpmodels", 0);	/* snappy lo-fi animation */
			PRESET_COMMON
		}
		else if (preset == 4)	/* Faithful — native res, OG textures, static water */
		{
			Cvar_SetValue ("r_scale", 1.0f);
			Cvar_SetValue ("r_softemu", 0);
			Cvar_SetValue ("r_dither", 0);
			Cvar_Set ("gl_texturemode", "GL_NEAREST_MIPMAP_NEAREST");
			Cvar_SetValue ("gl_texture_anisotropy", 1);
			Cvar_SetValue ("gl_fullbrights", 1);
			Cvar_SetValue ("gl_fxaa", 0);
			Cvar_SetValue ("r_lightmap_bicubic", 0);
			Cvar_SetValue ("r_watercolor", 0);
			Cvar_SetValue ("r_waterwarp", 0);
			Cvar_SetValue ("r_motionblur", 0);
			Cvar_SetValue ("gl_glows", 1);
			Cvar_SetValue ("gl_missile_glows", 1);
			Cvar_SetValue ("gl_other_glows", 1);
			Cvar_SetValue ("gl_glow_intensity", 0.2f);
			PRESET_COMMON
		}
		else if (preset == 5)	/* Clean — sharp native, mild effects */
		{
			Cvar_SetValue ("r_scale", 1.0f);
			Cvar_SetValue ("r_softemu", 0);
			Cvar_SetValue ("r_dither", 0);
			Cvar_Set ("gl_texturemode", "GL_NEAREST_MIPMAP_LINEAR");
			Cvar_SetValue ("gl_texture_anisotropy", 1);
			Cvar_SetValue ("gl_fullbrights", 1);
			Cvar_SetValue ("gl_fxaa", 0);
			Cvar_SetValue ("r_lightmap_bicubic", 0);
			Cvar_SetValue ("r_watercolor", 0);
			Cvar_SetValue ("r_waterwarp", 1);
			Cvar_SetValue ("r_motionblur", 0);
			Cvar_SetValue ("gl_glows", 1);
			Cvar_SetValue ("gl_missile_glows", 1);
			Cvar_SetValue ("gl_other_glows", 1);
			Cvar_SetValue ("gl_glow_intensity", 0.4f);
			Cvar_SetValue ("gl_torch_dlight", 1);
			PRESET_COMMON
		}
		else if (preset == 6)	/* Modern — smooth, full effects */
		{
			Cvar_SetValue ("r_scale", 1.0f);
			Cvar_SetValue ("r_softemu", 0);
			Cvar_SetValue ("r_dither", 0);
			Cvar_Set ("gl_texturemode", "GL_LINEAR_MIPMAP_NEAREST");
			Cvar_SetValue ("gl_texture_anisotropy", gl_max_anisotropy);
			Cvar_SetValue ("gl_fullbrights", 1);
			Cvar_SetValue ("gl_fxaa", 1);
			Cvar_SetValue ("r_lightmap_bicubic", 1);
			Cvar_SetValue ("r_watercolor", 1);
			Cvar_SetValue ("r_waterwarp", 1);
			Cvar_SetValue ("r_motionblur", 0);
			Cvar_SetValue ("gl_glows", 1);
			Cvar_SetValue ("gl_missile_glows", 1);
			Cvar_SetValue ("gl_other_glows", 1);
			Cvar_SetValue ("gl_glow_intensity", 1.0f);
			Cvar_SetValue ("gl_torch_dlight", 1);
			PRESET_COMMON
		}
		else if (preset == 7)	/* Ultra — everything maxed */
		{
			Cvar_SetValue ("r_scale", 1.0f);
			Cvar_SetValue ("r_softemu", 0);
			Cvar_SetValue ("r_dither", 0);
			Cvar_Set ("gl_texturemode", "GL_LINEAR_MIPMAP_LINEAR");
			Cvar_SetValue ("gl_texture_anisotropy", gl_max_anisotropy);
			Cvar_SetValue ("gl_fullbrights", 1);
			Cvar_SetValue ("gl_fxaa", 1);
			Cvar_SetValue ("r_lightmap_bicubic", 1);
			Cvar_SetValue ("r_watercolor", 1);
			Cvar_SetValue ("r_waterwarp", 1);
			Cvar_SetValue ("r_motionblur", 1.0f);
			Cvar_SetValue ("gl_glows", 1);
			Cvar_SetValue ("gl_missile_glows", 1);
			Cvar_SetValue ("gl_other_glows", 1);
			Cvar_SetValue ("gl_glow_intensity", 1.0f);
			Cvar_SetValue ("gl_torch_dlight", 1);
			PRESET_COMMON
		}
#undef PRESET_COMMON
		Con_Printf ("Preset applied. Reload map for full effect.\n");
		break;
	}
	case DISP_GAMMA:
		f = v_gamma.value - dir * 0.05;
		if (f < 0.3)	f = 0.3;
		else if (f > 1)	f = 1;
		Cvar_SetValue ("gamma", f);
		break;
	case DISP_CONTRAST:
		f = v_contrast.value + dir * 0.05;
		if (f < 0.5)	f = 0.5;
		else if (f > 2)	f = 2;
		Cvar_SetValue ("contrast", f);
		break;
#ifdef GLQUAKE
	case DISP_CONSCALE:
		VID_ChangeConsize(dir);
		break;
	case DISP_PIXELASPECT:
		/* Upstream's two values and its toggle, not a slider: the point of
		 * the setting is the 320x200-over-4:3 look, and the values between
		 * are a way to get neither.  uhexen2-a5nn.37 */
		Cvar_Set ("scr_pixelaspect", vid.guipixelaspect == 1.0f ? "5:6" : "1");
		break;
#endif
	case DISP_SCRSIZE:
	{
		/* cycle between Full(110), Mini(120), Off(130), Clean(140) */
		static const int hud_sizes[] = { 110, 120, 130, 140 };
		int cur = 0, j;
		for (j = 0; j < 4; j++)
			if (scr_viewsize.integer >= hud_sizes[j])
				cur = j;
		cur += dir;
		if (cur < 0) cur = 3;
		if (cur > 3) cur = 0;
		Cvar_SetValue ("viewsize", hud_sizes[cur]);
		break;
	}
#ifdef GLQUAKE
	case DISP_FULLSCREEN:
		VID_MenuAdjustWindowMode (dir);
		break;
	case DISP_RESOLUTION:
		VID_MenuAdjustResolution (dir);
		break;
	case DISP_MSAA:
		/* "Antialiasing" now toggles FXAA (post-process) rather than
		 * window MSAA.  Window MSAA was removed because it screen-doored
		 * translucent surfaces (uhexen2-zroc); FXAA is instant (no video
		 * restart) and needs no multisampled framebuffer. */
		Cvar_SetValue ("gl_fxaa", Cvar_VariableValue("gl_fxaa") > 0 ? 0 : 1);
		break;
	case DISP_VSYNC:
		VID_MenuAdjustVSync (dir);
		break;
	case DISP_MAXFPS:
	{
		static const int fps_steps[] = { 24, 30, 60, 72, 90, 120, 144, 0 };
		int i, cur = (int)host_maxfps.value;
		int num_steps = sizeof(fps_steps) / sizeof(fps_steps[0]);

		/* find current position */
		for (i = 0; i < num_steps; i++)
		{
			if (fps_steps[i] >= cur || fps_steps[i] == 0)
				break;
		}
		i += dir;
		if (i < 0) i = 0;
		if (i >= num_steps) i = num_steps - 1;
		Cvar_SetValue ("host_maxfps", fps_steps[i]);
		break;
	}
	case DISP_SHOWFPS:
		Cvar_Set ("showfps", Cvar_VariableValue("showfps") ? "0" : "1");
		break;
	case DISP_MENUFADE:
	{
		int val = scr_menubgstyle.integer + dir;
		if (val > 2) val = 0;
		if (val < 0) val = 2;
		Cvar_SetValue ("scr_menubgstyle", val);
		break;
	}
	case DISP_MENUFADEALPHA:
	{
		float val = scr_menubgalpha.value + dir * 0.1f;
		if (val < 0.0f) val = 0.0f;
		if (val > 1.0f) val = 1.0f;
		Cvar_SetValue ("scr_menubgalpha", val);
		break;
	}
#endif
	}
}

static void M_Display_Draw (void)
{
	float	r;

	ScrollTitle("gfx/menu/title3.lmp");
	M_PrintWhite (96, 72, "Display Options");

	if (!M_Display_IsSkip(DISP_PRESET))
	{
		static const char *preset_names[] = {
			"User", "Crunchy", "Retro", "Authentic", "Faithful", "Clean", "Modern", "Ultra"
		};
		M_Print (76, 92 + 8*DISP_PRESET, disp_labels[DISP_PRESET]);
		M_PrintWhite (220, 92 + 8*DISP_PRESET, preset_names[M_Display_DetectPreset()]);
	}

	if (!M_Display_IsSkip(DISP_GAMMA))
	{
		M_Print (76, 92 + 8*DISP_GAMMA, disp_labels[DISP_GAMMA]);
		r = (1.0 - v_gamma.value) / 0.7;
		M_DrawSliderValue (220, 92 + 8*DISP_GAMMA, r, "%.2f", v_gamma.value);
	}

	if (!M_Display_IsSkip(DISP_CONTRAST))
	{
		M_Print (76, 92 + 8*DISP_CONTRAST, disp_labels[DISP_CONTRAST]);
		r = (v_contrast.value - 0.5) / 1.5;
		M_DrawSliderValue (220, 92 + 8*DISP_CONTRAST, r, "%.2f", v_contrast.value);
	}

#ifdef GLQUAKE
	if (!M_Display_IsSkip(DISP_CONSCALE))
	{
		M_Print (76, 92 + 8*DISP_CONSCALE, disp_labels[DISP_CONSCALE]);
		r = VID_ReportConsize();
		M_DrawSliderValue (220, 92 + 8*DISP_CONSCALE, (r-1)/2, "%.2fx", r);
	}

	if (!M_Display_IsSkip(DISP_PIXELASPECT))
	{
		M_Print (76, 92 + 8*DISP_PIXELASPECT, disp_labels[DISP_PIXELASPECT]);
		M_Print (220, 92 + 8*DISP_PIXELASPECT,
			 vid.guipixelaspect == 1.0f ? "Square" : "Stretched");
	}
#endif

	if (!M_Display_IsSkip(DISP_SCRSIZE))
	{
		M_Print (76, 92 + 8*DISP_SCRSIZE, disp_labels[DISP_SCRSIZE]);
		if (scr_viewsize.integer >= 140)
			M_PrintWhite (220, 92 + 8*DISP_SCRSIZE, "Clean");
		else if (scr_viewsize.integer >= 130)
			M_PrintWhite (220, 92 + 8*DISP_SCRSIZE, "Off");
		else if (scr_viewsize.integer >= 120)
			M_PrintWhite (220, 92 + 8*DISP_SCRSIZE, "Mini");
		else
			M_PrintWhite (220, 92 + 8*DISP_SCRSIZE, "Full");
	}

	if (!M_Display_IsSkip(DISP_RENDERING))
		M_Print (76, 92 + 8*DISP_RENDERING, "Rendering...");
	if (!M_Display_IsSkip(DISP_GRAPHICS))
		M_Print (76, 92 + 8*DISP_GRAPHICS, "Misc...");

	/* separator */

#ifdef GLQUAKE
	{
		qboolean is_current, available;
		const char *s;
		int ms, vsync;

		if (!M_Display_IsSkip(DISP_FULLSCREEN))
		{
			int wmode = VID_MenuGetWindowMode ();
			M_Print (76, 92 + 8*DISP_FULLSCREEN, disp_labels[DISP_FULLSCREEN]);
			M_PrintWhite (220, 92 + 8*DISP_FULLSCREEN,
				wmode == 2 ? "Fullscreen" : wmode == 1 ? "Borderless" : "Windowed");
		}

		if (!M_Display_IsSkip(DISP_RESOLUTION))
		{
			M_Print (76, 92 + 8*DISP_RESOLUTION, disp_labels[DISP_RESOLUTION]);
			s = VID_MenuGetResolution (&is_current);
			if (is_current)
				M_PrintWhite (220, 92 + 8*DISP_RESOLUTION, s);
			else
				M_Print (220, 92 + 8*DISP_RESOLUTION, s);
		}

		if (!M_Display_IsSkip(DISP_MSAA))
		{
			/* "Antialiasing" drives FXAA now (see M_Display_AdjustSliders);
			 * window MSAA was removed for uhexen2-zroc. */
			M_Print (76, 92 + 8*DISP_MSAA, disp_labels[DISP_MSAA]);
			M_PrintWhite (220, 92 + 8*DISP_MSAA,
				      Cvar_VariableValue("gl_fxaa") > 0 ? "FXAA" : "Off");
			(void)is_current; (void)available; (void)ms;
		}

		if (!M_Display_IsSkip(DISP_VSYNC))
		{
			M_Print (76, 92 + 8*DISP_VSYNC, disp_labels[DISP_VSYNC]);
			vsync = VID_MenuGetVSync ();
			M_PrintWhite (220, 92 + 8*DISP_VSYNC,
				vsync == -1 ? "Adaptive" : vsync ? "On" : "Off");
		}

		if (!M_Display_IsSkip(DISP_MAXFPS))
		{
			M_Print (76, 92 + 8*DISP_MAXFPS, disp_labels[DISP_MAXFPS]);
			if ((int)host_maxfps.value <= 0)
				M_PrintWhite (220, 92 + 8*DISP_MAXFPS, "Unlimited");
			else
				M_PrintWhite (220, 92 + 8*DISP_MAXFPS, va("%d", (int)host_maxfps.value));
		}

		if (!M_Display_IsSkip(DISP_SHOWFPS))
		{
			M_Print (76, 92 + 8*DISP_SHOWFPS, disp_labels[DISP_SHOWFPS]);
			M_DrawCheckbox (220, 92 + 8*DISP_SHOWFPS, (int)Cvar_VariableValue("showfps"));
		}

		if (!M_Display_IsSkip(DISP_MENUFADE))
		{
			M_Print (76, 92 + 8*DISP_MENUFADE, disp_labels[DISP_MENUFADE]);
			M_PrintWhite (220, 92 + 8*DISP_MENUFADE,
				scr_menubgstyle.integer == 2 ? "Menu Box" :
				scr_menubgstyle.integer == 1 ? "Simple" : "Off");
		}
		if (!M_Display_IsSkip(DISP_MENUFADEALPHA))
		{
			M_Print (76, 92 + 8*DISP_MENUFADEALPHA, disp_labels[DISP_MENUFADEALPHA]);
			M_DrawSliderValue (220, 92 + 8*DISP_MENUFADEALPHA,
				scr_menubgalpha.value, "%.1f", scr_menubgalpha.value);
		}

		if (!M_Display_IsSkip(DISP_APPLY) && VID_MenuNeedApply ())
			M_Print (76, 92 + 8*DISP_APPLY, disp_labels[DISP_APPLY]);
	}
#endif

	/* Mouse hover */
	{
		int hover = M_MouseToMenuItem(menu_mouse_y, 92, 8, DISP_ITEMS);
		if (hover >= 0 && !M_Display_IsSkip(hover))
			display_cursor = hover;
	}

	if (!M_Display_IsSkip(display_cursor))
		M_DrawCharacter (64, 92 + display_cursor*8, 12+((int)(realtime*4)&1));

	M_Filter_Draw (76, 92 + 8*(DISP_ITEMS + 1));
}

static qboolean M_Display_IsSkip (int cursor)
{
	if (cursor < 0 || cursor >= DISP_ITEMS)
		return true;
	if (cursor == DISP_SEP || cursor == DISP_SEP2)
		return true;
#ifdef GLQUAKE
	if (cursor == DISP_APPLY && !VID_MenuNeedApply ())
		return true;
#endif
	/* Nothing to dim when the backdrop is off, and a slider that visibly
	 * does nothing is worse than an absent one. */
	if (cursor == DISP_MENUFADEALPHA && scr_menubgstyle.integer < 1)
		return true;
	if (M_Filter_Active() && !M_Filter_Matches(disp_labels[cursor]))
		return true;
	return false;
}

static void M_Display_Key (int k)
{
	if (k != K_ESCAPE && k != K_ENTER &&
	    k != K_UPARROW && k != K_DOWNARROW &&
	    k != K_LEFTARROW && k != K_RIGHTARROW)
	{
		if (M_Filter_HandleKey (k))
		{
			int i;
			if (M_Display_IsSkip (display_cursor))
			{
				for (i = 0; i < DISP_ITEMS; i++)
				{
					if (!M_Display_IsSkip (i))
					{
						display_cursor = i;
						break;
					}
				}
			}
			return;
		}
	}

	switch (k)
	{
	case K_ESCAPE:
		if (M_Filter_Active ())
		{
			M_Filter_Clear ();
			return;
		}
		M_Menu_Options_f ();
		break;
	case K_ENTER:
		m_entersound = true;
		if (display_cursor == DISP_RENDERING)
		{
			M_Menu_Rendering_f ();
			return;
		}
		if (display_cursor == DISP_GRAPHICS)
		{
			M_Menu_Graphics_f ();
			return;
		}
#ifdef GLQUAKE
		if (display_cursor == DISP_APPLY && VID_MenuNeedApply ())
		{
			VID_MenuApply ();
			/* Reset cursor to first valid item after apply, since some items may now be skipped */
			display_cursor = 0;
			while (display_cursor < DISP_ITEMS && M_Display_IsSkip(display_cursor))
				display_cursor++;
			if (display_cursor >= DISP_ITEMS)
				display_cursor = 0;
			return;
		}
#endif
		M_Display_AdjustSliders (1);
		return;
	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		{
			int guard = DISP_ITEMS;
			do {
				display_cursor--;
				if (display_cursor < 0)
					display_cursor = DISP_ITEMS-1;
			} while (M_Display_IsSkip (display_cursor) && --guard > 0);
		}
		break;
	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		{
			int guard = DISP_ITEMS;
			do {
				display_cursor++;
				if (display_cursor >= DISP_ITEMS)
					display_cursor = 0;
			} while (M_Display_IsSkip (display_cursor) && --guard > 0);
		}
		break;
	case K_LEFTARROW:
		M_Display_AdjustSliders (-1);
		break;
	case K_RIGHTARROW:
		M_Display_AdjustSliders (1);
		break;
	}
}


//=============================================================================
/* RENDERING SUBMENU */

enum
{
	REND_RENDERSCALE = 0,
	REND_SOFTEMU,
	REND_MDLWARP,
	REND_DITHER,
	REND_TEXFILTER,
	REND_ANISOTROPY,
	REND_EXTTEXTURES,
	REND_LEGACYALPHA,
	REND_LMBICUBIC,
	REND_PARTICLES,
	REND_SOFTPARTICLES,
	REND_FULLBRIGHTS,
	REND_DYNLIGHT,
	REND_WATERCOLOR,
	REND_WATERALPHA,
	REND_WATERWARP,
	REND_LIQUIDWARP,
	REND_GLOWS,
	REND_FLASHINTENSITY,
	REND_FXAA,
	REND_MOTIONBLUR,
	REND_HDR,
	REND_HDR_EXPOSURE,
	REND_ITEMS
};

static int	rendering_cursor;

/* Label table — single source of truth for both M_Print and search match.
 * Order must mirror the REND_* enum.  Search is case-insensitive substring. */
static const char *rend_labels[REND_ITEMS] = {
	"Render Scale  :",	/* REND_RENDERSCALE */
	"Retro Mode    :",	/* REND_SOFTEMU */
	"Model Warp    :",	/* REND_MDLWARP */
	"Dither Amount :",	/* REND_DITHER */
	"Textures      :",	/* REND_TEXFILTER */
	"Anisotropy    :",	/* REND_ANISOTROPY */
	"HD Textures   :",	/* REND_EXTTEXTURES */
	"Old Skin Alpha:",	/* REND_LEGACYALPHA */
	"Smooth Lmaps  :",	/* REND_LMBICUBIC */
	"Particles     :",	/* REND_PARTICLES */
	"Soft Sprites  :",	/* REND_SOFTPARTICLES */
	"Fullbrights   :",	/* REND_FULLBRIGHTS */
	"Dynamic Light :",	/* REND_DYNLIGHT */
	"Water Tint    :",	/* REND_WATERCOLOR */
	"Water Alpha   :",	/* REND_WATERALPHA */
	"Water Warp    :",	/* REND_WATERWARP */
	"Liquid Warp   :",	/* REND_LIQUIDWARP */
	"Glows         :",	/* REND_GLOWS */
	"Dyn Light Str :",	/* REND_FLASHINTENSITY */
	"FXAA          :",	/* REND_FXAA */
	"Motion Blur   :",	/* REND_MOTIONBLUR */
	"HDR Tonemap   :",	/* REND_HDR */
	"HDR Exposure  :",	/* REND_HDR_EXPOSURE */
};

static qboolean M_Rendering_IsSkip (int i)
{
	if (i < 0 || i >= REND_ITEMS)
		return true;
#if defined(WEBSOFT)
	/* GPU-only rows have no counterpart in the software rasterizer.  Their
	 * cvars still exist (r_soft_web.c registers them) so configs written by
	 * the GL build round-trip, but a slider that cannot move anything does
	 * not belong in the menu. */
	switch (i)
	{
	case REND_RENDERSCALE:
	case REND_SOFTEMU:
	case REND_MDLWARP:
	case REND_DITHER:
	case REND_TEXFILTER:
	case REND_ANISOTROPY:
	case REND_EXTTEXTURES:
	case REND_LEGACYALPHA:
	case REND_LMBICUBIC:
	case REND_SOFTPARTICLES:
	case REND_FULLBRIGHTS:
	case REND_WATERCOLOR:
	case REND_WATERALPHA:
	case REND_LIQUIDWARP:
	case REND_FXAA:
	case REND_MOTIONBLUR:
	case REND_HDR:
	case REND_HDR_EXPOSURE:
		return true;
	default:
		break;
	}
	/* The complement, spelled out so tools/menu_soft_parity.py can hold
	 * every row to a decision.  These do reach something real in the
	 * software build; the script checks that claim against the cvars each
	 * row writes and the sources CMake compiles into it.
	 *
	 * soft-ok: REND_PARTICLES, REND_DYNLIGHT, REND_WATERWARP, REND_GLOWS,
	 *          REND_FLASHINTENSITY */
#else
	/* Hide controls whose backing GL feature this context does not have,
	 * rather than offering a slider that silently does nothing.  On the ES
	 * tier both of these are extensions that a browser may not expose.
	 * d2c46f078. */
	if (i == REND_ANISOTROPY && !gl_renderer_caps.anisotropy)
		return true;
	if ((i == REND_HDR || i == REND_HDR_EXPOSURE) && !gl_renderer_caps.float_color_buffer)
		return true;
#endif
	return M_Filter_Active() && !M_Filter_Matches(rend_labels[i]);
}


static void M_Menu_Rendering_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_rendering;
	m_entersound = true;
	M_Filter_Clear ();
}

static void M_Rendering_AdjustSliders (int dir)
{
	S_LocalSound ("raven/menu3.wav");

	switch (rendering_cursor)
	{
	case REND_RENDERSCALE:
	{
		static const float scale_steps[] = { 0.25f, 0.33f, 0.50f, 0.67f, 0.75f, 1.0f };
		int i, num = sizeof(scale_steps) / sizeof(scale_steps[0]);
		float cur = r_scale.value;
		for (i = 0; i < num; i++)
		{
			if (scale_steps[i] >= cur - 0.01f)
				break;
		}
		i += dir;
		if (i < 0) i = 0;
		if (i >= num) i = num - 1;
		Cvar_SetValue ("r_scale", scale_steps[i]);
		break;
	}
	case REND_SOFTEMU:
	{
		int v = (int)r_softemu.value + dir;
		if (v < 0) v = 3;
		if (v > 3) v = 0;
		Cvar_SetValue ("r_softemu", v);
		break;
	}
	/* The affine texture mapping the software rasteriser had on models.
	 * Tri-state because the shipped -1 is not a boolean: it means "follow
	 * Retro Mode", which turns it on at Colormap and leaves it off below.
	 * The one softemu sub-cvar Ironwail exposes too.  uhexen2-ktjv. */
	case REND_MDLWARP:
	{
		float	v = r_softemu_mdl_warp.value;
		int	state = (v < 0.0f) ? 0 : (v == 0.0f) ? 1 : 2;

		state = (state + dir) % 3;
		if (state < 0)
			state += 3;
		Cvar_Set ("r_softemu_mdl_warp",
			  state == 0 ? "-1" : state == 1 ? "0" : "1");
		break;
	}
	case REND_DITHER:
	{
		float f = r_dither.value + dir * 0.25;
		if (f < 0) f = 0;
		if (f > 2) f = 2;
		Cvar_SetValue ("r_dither", f);
		break;
	}
	case REND_TEXFILTER:
		VID_MenuAdjustTexFilter ();
		break;
	case REND_ANISOTROPY:
		VID_MenuAdjustAnisotropy (dir);
		break;
	case REND_LMBICUBIC:
		Cvar_SetValue ("r_lightmap_bicubic", !r_lightmap_bicubic.integer);
		break;
	case REND_PARTICLES:
		Cvar_SetValue ("gl_particles", !gl_particles.integer);
		break;
	case REND_SOFTPARTICLES:
		Cvar_SetValue ("r_softparticles", !r_softparticles.integer);
		break;
	case REND_FULLBRIGHTS:
		Cvar_SetValue ("gl_fullbrights", !gl_fullbrights.integer);
		break;
	case REND_EXTTEXTURES:
		Cvar_SetValue ("r_external_textures",
			       Cvar_VariableValue("r_external_textures") ? 0 : 1);
		break;
	case REND_LEGACYALPHA:
		Cvar_SetValue ("r_legacy_special_trans_alpha",
			       Cvar_VariableValue("r_legacy_special_trans_alpha") ? 0 : 1);
		break;
	case REND_DYNLIGHT:
		Cvar_SetValue ("r_dynamic", r_dynamic.integer ? 0 : 1);
		break;
	case REND_WATERCOLOR:
	{
		int v = (int)Cvar_VariableValue("r_watercolor") + dir;
		if (v < 0) v = 3;
		if (v > 3) v = 0;
		Cvar_SetValue ("r_watercolor", v);
		break;
	}
	case REND_WATERALPHA:
	{
		float f = r_wateralpha.value + dir * 0.05f;
		if (f < 0.7f) f = 0.7f;
		if (f > 1.0f) f = 1.0f;
		Cvar_SetValue ("r_wateralpha", f);
		break;
	}
	case REND_WATERWARP:
		Cvar_SetValue ("r_waterwarp", !r_waterwarp.integer);
		GL_PostProcess_RequestWaterwarpPreview(0.5f);	/* 500ms preview */
		break;
	case REND_LIQUIDWARP:
	{
		/* Strength of the warp on the liquid surface itself, as a
		 * percentage of vanilla.  Stops at 200%: the engine clamps
		 * gl_waterwarp_amount at 4, but past double the stock throw the
		 * turb stops reading as a moving surface and starts reading as a
		 * broken texture, which is not something to offer in a menu.
		 * gl_waterwarp_speed stays a console knob -- this row is the one
		 * people mean when they say the water is too much. */
		float f = gl_waterwarp_amount.value + dir * 0.1f;
		if (f < 0.0f) f = 0.0f;
		if (f > 2.0f) f = 2.0f;
		Cvar_SetValue ("gl_waterwarp_amount", f);
		break;
	}
	case REND_GLOWS:
	{
		/* cycle: Off(0) -> Torch Only(1) -> Reduced(2) -> All(3) */
		int cur;
		if (!gl_glows.integer)
			cur = 0;
		else if (!gl_missile_glows.integer)
			cur = 1;
		else if (gl_glow_intensity.value < 0.9f)
			cur = 2;
		else
			cur = 3;
		if (dir > 0) { if (++cur > 3) cur = 0; }
		else         { if (--cur < 0) cur = 3; }
		Cvar_SetValue ("gl_glows", cur >= 1 ? 1 : 0);
		Cvar_SetValue ("gl_missile_glows", cur >= 2 ? 1 : 0);
		Cvar_SetValue ("gl_other_glows", cur >= 2 ? 1 : 0);
		Cvar_SetValue ("gl_glow_intensity", cur >= 3 ? 1.0f : 0.4f);
		break;
	}
	case REND_FLASHINTENSITY:
	{
		float f = gl_flashintensity.value + dir * 0.25f;
		if (f < 0) f = 0;
		if (f > 2) f = 2;
		Cvar_SetValue ("gl_flashintensity", f);
		break;
	}
	case REND_FXAA:
		Cvar_SetValue ("gl_fxaa", !gl_fxaa.integer);
		break;
	case REND_MOTIONBLUR:
	{
		float f = Cvar_VariableValue("r_motionblur") + dir * 0.25f;
		if (f < 0) f = 0;
		if (f > 1) f = 1;
		Cvar_SetValue ("r_motionblur", f);
		break;
	}
	case REND_HDR:
		Cvar_SetValue ("r_hdr", r_hdr.integer ? 0 : 1);
		break;
	case REND_HDR_EXPOSURE:
	{
		float f = r_hdr_exposure.value + dir * 0.1f;
		if (f < 0.1f) f = 0.1f;
		if (f > 4.0f) f = 4.0f;
		Cvar_SetValue ("r_hdr_exposure", f);
		break;
	}
	}
}

static void M_Rendering_Draw (void)
{
	float	r;
	qboolean available;
	int	aniso;

	ScrollTitle("gfx/menu/title3.lmp");
	M_PrintWhite (96, 72, "Rendering");

	if (!M_Rendering_IsSkip(REND_RENDERSCALE))
	{
		M_Print (76, 92 + 8*REND_RENDERSCALE, rend_labels[REND_RENDERSCALE]);
		if (r_scale.value >= 1.0f)
			M_PrintWhite (220, 92 + 8*REND_RENDERSCALE, "Native");
		else
			M_PrintWhite (220, 92 + 8*REND_RENDERSCALE, va("%d%%", (int)(r_scale.value * 100)));
	}

	if (!M_Rendering_IsSkip(REND_SOFTEMU))
	{
		M_Print (76, 92 + 8*REND_SOFTEMU, rend_labels[REND_SOFTEMU]);
		if ((int)r_softemu.value == 1)
			M_PrintWhite (220, 92 + 8*REND_SOFTEMU, "Dithered");
		else if ((int)r_softemu.value == 2)
			M_PrintWhite (220, 92 + 8*REND_SOFTEMU, "Banded");
		else if ((int)r_softemu.value == 3)
			M_PrintWhite (220, 92 + 8*REND_SOFTEMU, "Colormap");
		else
			M_PrintWhite (220, 92 + 8*REND_SOFTEMU, "Off");
	}

	if (!M_Rendering_IsSkip(REND_MDLWARP))
	{
		float v = r_softemu_mdl_warp.value;
		M_Print (76, 92 + 8*REND_MDLWARP, rend_labels[REND_MDLWARP]);
		M_PrintWhite (220, 92 + 8*REND_MDLWARP,
			(v < 0.0f) ? "Auto" : (v == 0.0f) ? "Off" : "On");
	}

	if (!M_Rendering_IsSkip(REND_DITHER))
	{
		M_Print (76, 92 + 8*REND_DITHER, rend_labels[REND_DITHER]);
		r = r_dither.value / 2.0;
		M_DrawSliderValue (220, 92 + 8*REND_DITHER, r, "%.0f%%", r_dither.value * 50);
	}

	if (!M_Rendering_IsSkip(REND_TEXFILTER))
	{
		M_Print (76, 92 + 8*REND_TEXFILTER, rend_labels[REND_TEXFILTER]);
		M_PrintWhite (220, 92 + 8*REND_TEXFILTER,
			VID_MenuGetTexFilter () ? "Smooth" : "Classic");
	}

	if (!M_Rendering_IsSkip(REND_ANISOTROPY))
	{
		M_Print (76, 92 + 8*REND_ANISOTROPY, rend_labels[REND_ANISOTROPY]);
		aniso = VID_MenuGetAnisotropy (&available);
		if (available)
			M_PrintWhite (220, 92 + 8*REND_ANISOTROPY, va("%dx", aniso));
		else
			M_PrintWhite (220, 92 + 8*REND_ANISOTROPY, "N/A");
	}

	if (!M_Rendering_IsSkip(REND_LMBICUBIC))
	{
		M_Print (76, 92 + 8*REND_LMBICUBIC, rend_labels[REND_LMBICUBIC]);
		M_DrawCheckbox (220, 92 + 8*REND_LMBICUBIC, r_lightmap_bicubic.integer);
	}

	if (!M_Rendering_IsSkip(REND_PARTICLES))
	{
		M_Print (76, 92 + 8*REND_PARTICLES, rend_labels[REND_PARTICLES]);
		M_PrintWhite (220, 92 + 8*REND_PARTICLES, gl_particles.integer ? "Round" : "Square");
	}

	if (!M_Rendering_IsSkip(REND_SOFTPARTICLES))
	{
		M_Print (76, 92 + 8*REND_SOFTPARTICLES, rend_labels[REND_SOFTPARTICLES]);
		M_DrawCheckbox (220, 92 + 8*REND_SOFTPARTICLES, r_softparticles.integer);
	}

	if (!M_Rendering_IsSkip(REND_EXTTEXTURES))
	{
		M_Print (76, 92 + 8*REND_EXTTEXTURES, rend_labels[REND_EXTTEXTURES]);
		M_DrawCheckbox (220, 92 + 8*REND_EXTTEXTURES,
				(int)Cvar_VariableValue("r_external_textures"));
	}

	if (!M_Rendering_IsSkip(REND_LEGACYALPHA))
	{
		M_Print (76, 92 + 8*REND_LEGACYALPHA, rend_labels[REND_LEGACYALPHA]);
		M_DrawCheckbox (220, 92 + 8*REND_LEGACYALPHA,
				(int)Cvar_VariableValue("r_legacy_special_trans_alpha"));
	}

	if (!M_Rendering_IsSkip(REND_FULLBRIGHTS))
	{
		M_Print (76, 92 + 8*REND_FULLBRIGHTS, rend_labels[REND_FULLBRIGHTS]);
		M_DrawCheckbox (220, 92 + 8*REND_FULLBRIGHTS, gl_fullbrights.integer);
	}

	if (!M_Rendering_IsSkip(REND_DYNLIGHT))
	{
		M_Print (76, 92 + 8*REND_DYNLIGHT, rend_labels[REND_DYNLIGHT]);
		M_DrawCheckbox (220, 92 + 8*REND_DYNLIGHT, r_dynamic.integer);
	}

	if (!M_Rendering_IsSkip(REND_WATERCOLOR))
	{
		M_Print (76, 92 + 8*REND_WATERCOLOR, rend_labels[REND_WATERCOLOR]);
		switch ((int)Cvar_VariableValue("r_watercolor"))
		{
		case 1:  M_PrintWhite (220, 92 + 8*REND_WATERCOLOR, "Blue"); break;
		case 2:  M_PrintWhite (220, 92 + 8*REND_WATERCOLOR, "Green"); break;
		case 3:  M_PrintWhite (220, 92 + 8*REND_WATERCOLOR, "Clear"); break;
		default: M_PrintWhite (220, 92 + 8*REND_WATERCOLOR, "Classic"); break;
		}
	}

	if (!M_Rendering_IsSkip(REND_WATERALPHA))
	{
		M_Print (76, 92 + 8*REND_WATERALPHA, rend_labels[REND_WATERALPHA]);
		r = r_wateralpha.value;
		M_DrawSliderValue (220, 92 + 8*REND_WATERALPHA, r, "%.0f%%", r * 100);
	}

	if (!M_Rendering_IsSkip(REND_WATERWARP))
	{
		M_Print (76, 92 + 8*REND_WATERWARP, rend_labels[REND_WATERWARP]);
		M_DrawCheckbox (220, 92 + 8*REND_WATERWARP, r_waterwarp.integer);
	}

	if (!M_Rendering_IsSkip(REND_LIQUIDWARP))
	{
		M_Print (76, 92 + 8*REND_LIQUIDWARP, rend_labels[REND_LIQUIDWARP]);
		r = gl_waterwarp_amount.value;
		if (r <= 0.0f)
			M_PrintWhite (220, 92 + 8*REND_LIQUIDWARP, "Off");
		else
			M_DrawSliderValue (220, 92 + 8*REND_LIQUIDWARP, r * 0.5f,
					   "%.0f%%", r * 100);
	}

	if (!M_Rendering_IsSkip(REND_GLOWS))
	{
		M_Print (76, 92 + 8*REND_GLOWS, rend_labels[REND_GLOWS]);
		if (!gl_glows.integer)
			M_PrintWhite (220, 92 + 8*REND_GLOWS, "Off");
		else if (!gl_missile_glows.integer)
			M_PrintWhite (220, 92 + 8*REND_GLOWS, "Torch Only");
		else if (gl_glow_intensity.value < 0.9f)
			M_PrintWhite (220, 92 + 8*REND_GLOWS, "Reduced");
		else
			M_PrintWhite (220, 92 + 8*REND_GLOWS, "All");
	}

	if (!M_Rendering_IsSkip(REND_FLASHINTENSITY))
	{
		M_Print (76, 92 + 8*REND_FLASHINTENSITY, rend_labels[REND_FLASHINTENSITY]);
		if (gl_flashintensity.value <= 0)
			M_PrintWhite (220, 92 + 8*REND_FLASHINTENSITY, "Off");
		else
			M_DrawSliderValue (220, 92 + 8*REND_FLASHINTENSITY, gl_flashintensity.value / 2.0f, "%.2f", gl_flashintensity.value);
	}

	if (!M_Rendering_IsSkip(REND_FXAA))
	{
		M_Print (76, 92 + 8*REND_FXAA, rend_labels[REND_FXAA]);
		M_DrawCheckbox (220, 92 + 8*REND_FXAA, gl_fxaa.integer);
	}

	if (!M_Rendering_IsSkip(REND_MOTIONBLUR))
	{
		float mb = Cvar_VariableValue("r_motionblur");
		M_Print (76, 92 + 8*REND_MOTIONBLUR, rend_labels[REND_MOTIONBLUR]);
		if (mb <= 0)
			M_PrintWhite (220, 92 + 8*REND_MOTIONBLUR, "Off");
		else
			M_DrawSliderValue (220, 92 + 8*REND_MOTIONBLUR, mb, "%.0f%%", mb * 100);
	}

	if (!M_Rendering_IsSkip(REND_HDR))
	{
		M_Print (76, 92 + 8*REND_HDR, rend_labels[REND_HDR]);
		M_DrawCheckbox (220, 92 + 8*REND_HDR, r_hdr.integer);
	}

	if (!M_Rendering_IsSkip(REND_HDR_EXPOSURE))
	{
		M_Print (76, 92 + 8*REND_HDR_EXPOSURE, rend_labels[REND_HDR_EXPOSURE]);
		if (!r_hdr.integer)
			M_PrintWhite (220, 92 + 8*REND_HDR_EXPOSURE, "(HDR off)");
		else
			M_DrawSliderValue (220, 92 + 8*REND_HDR_EXPOSURE,
				(r_hdr_exposure.value - 0.1f) / 3.9f,
				"%.2f", r_hdr_exposure.value);
	}

	{
		/* mouse hover only lands on visible rows */
		int h = M_MouseToMenuItem(menu_mouse_y, 92, 8, REND_ITEMS);
		if (h >= 0 && !M_Rendering_IsSkip(h))
			rendering_cursor = h;
	}
	/* Raw index, not a compacted visual row: every label above draws at
	 * `92 + 8*REND_x` and M_MouseToMenuItem hit-tests the same raw stride, so
	 * a hidden row leaves a gap rather than closing one up.  Compacting only
	 * the cursor put the glyph one row above the item it controls for every
	 * row below a hidden one -- previously only visible while a search filter
	 * was typed, but permanent once a capability check can hide a row. */
	if (!M_Rendering_IsSkip(rendering_cursor))
		M_DrawCharacter (64, 92 + 8*rendering_cursor, 12+((int)(realtime*4)&1));

	/* Both replacement-skin rows are read by Mod_LoadAllSkins, so toggling
	 * one changes nothing until models are reloaded.  Without this the row
	 * reads as broken -- the checkbox moves and the world does not.  Drawn
	 * only while the cursor is on one of them; row REND_ITEMS is free,
	 * REND_ITEMS+1 belongs to the search prompt.  uhexen2-b0kv */
	if (rendering_cursor == REND_EXTTEXTURES || rendering_cursor == REND_LEGACYALPHA)
		M_Print (76, 92 + 8*REND_ITEMS, "applies on next map load");

	/* r_wateralpha is split-brained and the row cannot say just one thing.
	 * World water reads it per frame, so the slider is immediate there.  But
	 * GL_Upload8 bakes it into an EF_TRANSPARENT alias skin's alpha at upload
	 * (gl_draw.c, the `p & 1` arm) and Mod_RestoreIndexAlpha does the same for
	 * a rebuilt replacement, so those models keep whatever was current when
	 * they loaded.  That is 12 of the 13 EF_TRANSPARENT models in retail
	 * content, 22% of their skin texels on average and nearly all of
	 * stclrbm.mdl and boss/bone3.mdl -- visible enough to be worth saying.
	 * uhexen2-xaon, which also records why deferring the factor to draw time
	 * is not cheap: alpha already encodes three states, so there is no spare
	 * room for a class marker that survives filtering. */
	if (rendering_cursor == REND_WATERALPHA)
		M_Print (76, 92 + 8*REND_ITEMS, "water now; models on map load");

	/* search prompt below the menu (no row uses Y == REND_ITEMS+1) */
	M_Filter_Draw (76, 92 + 8*(REND_ITEMS + 1));
}

static void M_Rendering_Key (int k)
{
	/* Reserved keys must not feed the search buffer */
	if (k != K_ESCAPE && k != K_ENTER &&
	    k != K_UPARROW && k != K_DOWNARROW &&
	    k != K_LEFTARROW && k != K_RIGHTARROW)
	{
		if (M_Filter_HandleKey (k))
		{
			/* snap cursor to first visible row after filter change */
			int i;
			if (M_Rendering_IsSkip (rendering_cursor))
			{
				for (i = 0; i < REND_ITEMS; i++)
				{
					if (!M_Rendering_IsSkip (i))
					{
						rendering_cursor = i;
						break;
					}
				}
			}
			return;
		}
	}

	switch (k)
	{
	case K_ESCAPE:
		if (M_Filter_Active ())
		{
			/* first ESC clears the filter, second exits the menu */
			M_Filter_Clear ();
			return;
		}
		M_Menu_Display_f ();
		break;
	case K_ENTER:
		m_entersound = true;
		M_Rendering_AdjustSliders (1);
		return;
	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		{
			int guard = REND_ITEMS;
			do {
				rendering_cursor--;
				if (rendering_cursor < 0)
					rendering_cursor = REND_ITEMS-1;
			} while (M_Rendering_IsSkip (rendering_cursor) && --guard > 0);
		}
		break;
	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		{
			int guard = REND_ITEMS;
			do {
				rendering_cursor++;
				if (rendering_cursor >= REND_ITEMS)
					rendering_cursor = 0;
			} while (M_Rendering_IsSkip (rendering_cursor) && --guard > 0);
		}
		break;
	case K_LEFTARROW:
		M_Rendering_AdjustSliders (-1);
		break;
	case K_RIGHTARROW:
		M_Rendering_AdjustSliders (1);
		break;
	}
}


//=============================================================================
/* GRAPHICS MENU */

enum
{
	GFX_CENTERPRINTBG = 0,
	GFX_HUDSCALE,
	GFX_HUDTRANS,
	GFX_MENUSCALE,
	GFX_CROSSHAIRSCALE,
	GFX_CONALPHA,
	GFX_CONBRIGHT,
	GFX_CONMAXCOLS,
	GFX_OVERBRIGHT,
	GFX_COLORED_LM,
	GFX_TORCH_DLIGHT,
	GFX_GLOW_INTENSITY,
	GFX_SHOWSPEED,
	GFX_SHOWCLOCK,
	GFX_DEMOBAR,
	GFX_ITEMS
};

static int	graphics_cursor;

static const char *gfx_labels[GFX_ITEMS] = {
	"Message Backdrop:",	/* GFX_CENTERPRINTBG */
	"HUD Scale       :",	/* GFX_HUDSCALE */
	"HUD Transparency:",	/* GFX_HUDTRANS */
	"Menu Scale      :",	/* GFX_MENUSCALE */
	"Crosshair Scale :",	/* GFX_CROSSHAIRSCALE */
	"Console Alpha   :",	/* GFX_CONALPHA */
	"Console Bright  :",	/* GFX_CONBRIGHT */
	"Console Max Cols:",	/* GFX_CONMAXCOLS */
	"Overbright Mdls :",	/* GFX_OVERBRIGHT */
	"Colored Lighting:",	/* GFX_COLORED_LM */
	"Wall Torch DLs  :",	/* GFX_TORCH_DLIGHT */
	"Glow Intensity  :",	/* GFX_GLOW_INTENSITY */
	"Show Speed      :",	/* GFX_SHOWSPEED */
	"Show Clock      :",	/* GFX_SHOWCLOCK */
	"Demo Bar        :",	/* GFX_DEMOBAR */
};

static qboolean M_Graphics_IsSkip (int i)
{
	if (i < 0 || i >= GFX_ITEMS) return true;
#if defined(WEBSOFT)
	/* Rows whose cvar is only ever read by a gl_*.c file.  This build swaps
	 * gl_draw.c/gl_screen.c/gl_rmain.c out for draw_soft_web.c/screen.c and
	 * the 8bpp rasterizer, so nothing on this side consumes them: the demo
	 * bar has no bar, the three UI scales cannot apply (draw_soft_web.c's
	 * SCR_CalcUIScale returns 1 unconditionally and says why), and the rest
	 * reach no code at all.  Their cvars stay registered in r_soft_web.c so
	 * a config written by the GL build round-trips unharmed; what does not
	 * belong here is the row offering to move something it cannot.
	 *
	 * Found by tools/menu_soft_parity.py, which now holds this list to that
	 * standard on every push.  uhexen2-ibnq.2.
	 */
	switch (i)
	{
	case GFX_CENTERPRINTBG:
	case GFX_HUDSCALE:
	case GFX_MENUSCALE:
	case GFX_CROSSHAIRSCALE:
	case GFX_CONALPHA:
	case GFX_CONBRIGHT:
	case GFX_OVERBRIGHT:
	case GFX_COLORED_LM:
	case GFX_GLOW_INTENSITY:
	case GFX_SHOWSPEED:
	case GFX_SHOWCLOCK:
	case GFX_DEMOBAR:
		return true;
	default:
		break;
	}
	/* soft-ok: GFX_HUDTRANS, GFX_CONMAXCOLS, GFX_TORCH_DLIGHT */
#endif
	return M_Filter_Active() && !M_Filter_Matches(gfx_labels[i]);
}

static void M_Menu_Graphics_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_graphics;
	m_entersound = true;
	M_Filter_Clear ();
}

static void M_Graphics_AdjustSliders (int dir)
{
	S_LocalSound ("raven/menu3.wav");

	switch (graphics_cursor)
	{
	case GFX_CENTERPRINTBG:
	{
		int val = scr_centerprintbg.integer + dir;
		if (val > 2) val = 0;
		if (val < 0) val = 2;
		Cvar_SetValue ("scr_centerprintbg", val);
		break;
	}
	case GFX_HUDSCALE:
	{
		int val = (int)scr_sbarscale.value + dir;
		if (val < 0) val = 4;
		if (val > 4) val = 0;
		Cvar_SetValue ("scr_sbarscale", val);
		break;
	}
	case GFX_MENUSCALE:
	{
		int val = (int)scr_menuscale.value + dir;
		if (val < 0) val = 4;
		if (val > 4) val = 0;
		Cvar_SetValue ("scr_menuscale", val);
		break;
	}
	case GFX_CROSSHAIRSCALE:
	{
		int val = (int)scr_crosshairscale.value + dir;
		if (val < 0) val = 4;
		if (val > 4) val = 0;
		Cvar_SetValue ("scr_crosshairscale", val);
		break;
	}
	case GFX_CONALPHA:
	{
		float v = scr_conalpha.value + dir * 0.1f;
		if (v < 0.0f) v = 0.0f;
		if (v > 1.0f) v = 1.0f;
		Cvar_SetValue ("scr_conalpha", v);
		break;
	}
	case GFX_CONBRIGHT:
	{
		float v = scr_conbrightness.value + dir * 0.1f;
		if (v < 0.0f) v = 0.0f;
		if (v > 2.0f) v = 2.0f;
		Cvar_SetValue ("scr_conbrightness", v);
		break;
	}
	case GFX_CONMAXCOLS:
	{
		/* Discrete steps: Off, 80, 100, 120, 160, 200. */
		static const int steps[] = { 0, 80, 100, 120, 160, 200 };
		const int n = (int)(sizeof(steps) / sizeof(steps[0]));
		int cur = con_maxcols.integer;
		int i, idx = 0;
		for (i = 0; i < n; i++) if (steps[i] == cur) { idx = i; break; }
		idx += dir;
		if (idx < 0) idx = n - 1;
		if (idx >= n) idx = 0;
		Cvar_SetValue ("con_maxcols", steps[idx]);
		break;
	}
	case GFX_HUDTRANS:
	{
		int v = sbtrans.integer + dir;
		if (v < 0) v = 2;
		if (v > 2) v = 0;
		Cvar_SetValue ("sbtrans", v);
		break;
	}
	case GFX_OVERBRIGHT:
	{
		/* Three states since uhexen2-enbw, so this cycles rather than
		 * toggling — a checkbox here would have silently reset a
		 * console-set 2 to 1 the first time anyone touched it. */
		int v = gl_overbright_models.integer + dir;
		if (v < 0) v = 2;
		if (v > 2) v = 0;
		Cvar_SetValue ("gl_overbright_models", v);
		break;
	}
	case GFX_COLORED_LM:
		Cvar_SetValue ("gl_coloredlight", !gl_coloredlight.integer);
		break;
	case GFX_TORCH_DLIGHT:
		Cvar_SetValue ("gl_torch_dlight", !gl_torch_dlight.integer);
		break;
	case GFX_GLOW_INTENSITY:
	{
		float v = gl_glow_intensity.value + dir * 0.1f;
		if (v < 0.0f) v = 0.0f;
		if (v > 1.0f) v = 1.0f;
		Cvar_SetValue ("gl_glow_intensity", v);
		break;
	}
	case GFX_SHOWSPEED:
		Cvar_SetValue ("scr_showspeed",
			Cvar_VariableValue("scr_showspeed") ? 0 : 1);
		break;
	case GFX_SHOWCLOCK:
	{
		int v = (int)Cvar_VariableValue("showclock") + dir;
		if (v < 0) v = 3;
		if (v > 3) v = 0;
		Cvar_SetValue ("showclock", v);
		break;
	}
	/* Three states over a cvar that is really a float: below zero the bar
	 * never draws, zero draws it for as long as the demo runs, and anything
	 * positive is how many seconds of no interaction it survives.  The row
	 * rotates between the two flags and the shipped 1 second, and READS any
	 * other positive value truthfully -- but rotating away from a
	 * hand-tuned 2.5 and back lands on 1, because the row has nowhere to
	 * keep the old number.  Same trade the sibling rotations here make.
	 * uhexen2-itie. */
	case GFX_DEMOBAR:
	{
		float	v = scr_demobar_timeout.value;
		int	state = (v < 0.0f) ? 0 : (v == 0.0f) ? 1 : 2;

		state = (state + dir) % 3;
		if (state < 0)
			state += 3;
		Cvar_Set ("scr_demobar_timeout",
			  state == 0 ? "-1" : state == 1 ? "0" : "1");
		break;
	}
	}
}

static void M_Graphics_Draw (void)
{
	ScrollTitle("gfx/menu/title3.lmp");
	M_PrintWhite (96, 72, "Misc / HUD");

	if (!M_Graphics_IsSkip(GFX_CENTERPRINTBG))
	{
		M_Print (76, 92 + 8*GFX_CENTERPRINTBG, gfx_labels[GFX_CENTERPRINTBG]);
		M_PrintWhite (220, 92 + 8*GFX_CENTERPRINTBG,
			scr_centerprintbg.integer == 2 ? "Menu Box" :
			scr_centerprintbg.integer == 1 ? "Simple" : "Off");
	}

	if (!M_Graphics_IsSkip(GFX_HUDSCALE))
	{
		M_Print (76, 92 + 8*GFX_HUDSCALE, gfx_labels[GFX_HUDSCALE]);
		if ((int)scr_sbarscale.value == 0)
			M_PrintWhite (220, 92 + 8*GFX_HUDSCALE, "Auto");
		else
		{
			char buf[8];
			snprintf(buf, sizeof(buf), "%dx", (int)scr_sbarscale.value);
			M_PrintWhite (220, 92 + 8*GFX_HUDSCALE, buf);
		}
	}

	if (!M_Graphics_IsSkip(GFX_MENUSCALE))
	{
		M_Print (76, 92 + 8*GFX_MENUSCALE, gfx_labels[GFX_MENUSCALE]);
		if ((int)scr_menuscale.value == 0)
			M_PrintWhite (220, 92 + 8*GFX_MENUSCALE, "Auto");
		else
		{
			char buf[8];
			snprintf(buf, sizeof(buf), "%dx", (int)scr_menuscale.value);
			M_PrintWhite (220, 92 + 8*GFX_MENUSCALE, buf);
		}
	}

	if (!M_Graphics_IsSkip(GFX_CROSSHAIRSCALE))
	{
		M_Print (76, 92 + 8*GFX_CROSSHAIRSCALE, gfx_labels[GFX_CROSSHAIRSCALE]);
		if ((int)scr_crosshairscale.value == 0)
			M_PrintWhite (220, 92 + 8*GFX_CROSSHAIRSCALE, "Auto");
		else
		{
			char buf[8];
			snprintf(buf, sizeof(buf), "%dx", (int)scr_crosshairscale.value);
			M_PrintWhite (220, 92 + 8*GFX_CROSSHAIRSCALE, buf);
		}
	}

	if (!M_Graphics_IsSkip(GFX_CONALPHA))
	{
		M_Print (76, 92 + 8*GFX_CONALPHA, gfx_labels[GFX_CONALPHA]);
		M_DrawSliderValue (220, 92 + 8*GFX_CONALPHA,
			scr_conalpha.value, "%.2f", scr_conalpha.value);
	}

	if (!M_Graphics_IsSkip(GFX_CONBRIGHT))
	{
		M_Print (76, 92 + 8*GFX_CONBRIGHT, gfx_labels[GFX_CONBRIGHT]);
		M_DrawSliderValue (220, 92 + 8*GFX_CONBRIGHT,
			scr_conbrightness.value * 0.5f, "%.2f", scr_conbrightness.value);
	}

	if (!M_Graphics_IsSkip(GFX_CONMAXCOLS))
	{
		M_Print (76, 92 + 8*GFX_CONMAXCOLS, gfx_labels[GFX_CONMAXCOLS]);
		if (con_maxcols.integer <= 0)
			M_PrintWhite (220, 92 + 8*GFX_CONMAXCOLS, "Off");
		else
		{
			char buf[8];
			snprintf(buf, sizeof(buf), "%d", con_maxcols.integer);
			M_PrintWhite (220, 92 + 8*GFX_CONMAXCOLS, buf);
		}
	}

	if (!M_Graphics_IsSkip(GFX_HUDTRANS))
	{
		M_Print (76, 92 + 8*GFX_HUDTRANS, gfx_labels[GFX_HUDTRANS]);
		M_PrintWhite (220, 92 + 8*GFX_HUDTRANS,
			sbtrans.integer == 2 ? "Heavy" :
			sbtrans.integer == 1 ? "Light" : "Off");
	}

	if (!M_Graphics_IsSkip(GFX_OVERBRIGHT))
	{
		M_Print (76, 92 + 8*GFX_OVERBRIGHT, gfx_labels[GFX_OVERBRIGHT]);
		M_PrintWhite (220, 92 + 8*GFX_OVERBRIGHT,
			gl_overbright_models.integer >= 2 ? "Full" :
			gl_overbright_models.integer == 1 ? "On" : "Off");
	}

	if (!M_Graphics_IsSkip(GFX_COLORED_LM))
	{
		M_Print (76, 92 + 8*GFX_COLORED_LM, gfx_labels[GFX_COLORED_LM]);
		M_DrawCheckbox (220, 92 + 8*GFX_COLORED_LM, gl_coloredlight.integer);
	}

	if (!M_Graphics_IsSkip(GFX_TORCH_DLIGHT))
	{
		M_Print (76, 92 + 8*GFX_TORCH_DLIGHT, gfx_labels[GFX_TORCH_DLIGHT]);
		M_DrawCheckbox (220, 92 + 8*GFX_TORCH_DLIGHT, gl_torch_dlight.integer);
	}

	if (!M_Graphics_IsSkip(GFX_GLOW_INTENSITY))
	{
		M_Print (76, 92 + 8*GFX_GLOW_INTENSITY, gfx_labels[GFX_GLOW_INTENSITY]);
		M_DrawSliderValue (220, 92 + 8*GFX_GLOW_INTENSITY,
			gl_glow_intensity.value, "%.2f", gl_glow_intensity.value);
	}

	if (!M_Graphics_IsSkip(GFX_SHOWSPEED))
	{
		M_Print (76, 92 + 8*GFX_SHOWSPEED, gfx_labels[GFX_SHOWSPEED]);
		M_DrawCheckbox (220, 92 + 8*GFX_SHOWSPEED,
			(int)Cvar_VariableValue("scr_showspeed"));
	}

	if (!M_Graphics_IsSkip(GFX_SHOWCLOCK))
	{
		int v = (int)Cvar_VariableValue("showclock");
		M_Print (76, 92 + 8*GFX_SHOWCLOCK, gfx_labels[GFX_SHOWCLOCK]);
		M_PrintWhite (220, 92 + 8*GFX_SHOWCLOCK,
			v == 3 ? "Wall HH:MM:SS" :
			v == 2 ? "Wall HH:MM" :
			v == 1 ? "Game Time" : "Off");
	}

	if (!M_Graphics_IsSkip(GFX_DEMOBAR))
	{
		float	v = scr_demobar_timeout.value;
		char	buf[16];

		M_Print (76, 92 + 8*GFX_DEMOBAR, gfx_labels[GFX_DEMOBAR]);
		if (v < 0.0f)
			q_strlcpy (buf, "Off", sizeof(buf));
		else if (v == 0.0f)
			q_strlcpy (buf, "Always", sizeof(buf));
		else
			q_snprintf (buf, sizeof(buf), "%g sec", v);
		M_PrintWhite (220, 92 + 8*GFX_DEMOBAR, buf);
	}

	{
		int h = M_MouseToMenuItem(menu_mouse_y, 92, 8, GFX_ITEMS);
		if (h >= 0 && !M_Graphics_IsSkip(h))
			graphics_cursor = h;
	}
	/* Raw index, like every label above and like the hit-test just above that.
	 * The cursor used to walk a COMPACTED row count instead, which put the
	 * glyph one row too high for every item below a hidden one -- rows are not
	 * closed up when they are hidden, so there was no compacted layout for it
	 * to point into.  Same defect the Rendering submenu carries a note about.
	 * uhexen2-8uc1. */
	if (!M_Graphics_IsSkip(graphics_cursor))
		M_DrawCharacter (64, 92 + 8*graphics_cursor, 12+((int)(realtime*4)&1));

	M_Filter_Draw (76, 92 + 8*(GFX_ITEMS + 1));
}

static void M_Graphics_Key (int k)
{
	if (k != K_ESCAPE && k != K_ENTER &&
	    k != K_UPARROW && k != K_DOWNARROW &&
	    k != K_LEFTARROW && k != K_RIGHTARROW)
	{
		if (M_Filter_HandleKey (k))
		{
			int i;
			if (M_Graphics_IsSkip (graphics_cursor))
			{
				for (i = 0; i < GFX_ITEMS; i++)
				{
					if (!M_Graphics_IsSkip (i))
					{
						graphics_cursor = i;
						break;
					}
				}
			}
			return;
		}
	}

	switch (k)
	{
	case K_ESCAPE:
		if (M_Filter_Active ())
		{
			M_Filter_Clear ();
			return;
		}
		M_Menu_Display_f ();
		break;
	case K_ENTER:
		m_entersound = true;
		M_Graphics_AdjustSliders (1);
		return;
	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		{
			int guard = GFX_ITEMS;
			do {
				graphics_cursor--;
				if (graphics_cursor < 0)
					graphics_cursor = GFX_ITEMS-1;
			} while (M_Graphics_IsSkip (graphics_cursor) && --guard > 0);
		}
		break;
	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		{
			int guard = GFX_ITEMS;
			do {
				graphics_cursor++;
				if (graphics_cursor >= GFX_ITEMS)
					graphics_cursor = 0;
			} while (M_Graphics_IsSkip (graphics_cursor) && --guard > 0);
		}
		break;
	case K_LEFTARROW:
		M_Graphics_AdjustSliders (-1);
		break;
	case K_RIGHTARROW:
		M_Graphics_AdjustSliders (1);
		break;
	}
}


//=============================================================================
/* SOUND MENU */

enum
{
	SND_MUSICTYPE = 0,
	SND_MUSICVOL,
	SND_SFXVOL,
	SND_WATERFX,
	SND_ITEMS
};

static int	sound_cursor;

static void M_Menu_Sound_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_sound;
	m_entersound = true;

	if (old_bgmtype[0] == 0)
	{
		q_strlcpy(old_bgmtype, bgmtype.string, sizeof(old_bgmtype));
		old_extmusic = bgm_extmusic.integer;
	}
}

static void M_Sound_AdjustSliders (int dir)
{
	float	f;

	S_LocalSound ("raven/menu3.wav");

	switch (sound_cursor)
	{
	case SND_MUSICTYPE:
		if (q_strcasecmp(bgmtype.string,"midi") == 0)
		{
			if (bgm_extmusic.integer)
			{
			    if (dir < 0)
				Cvar_Set("bgm_extmusic","0");
			    else
				Cvar_Set("bgmtype","cd");
			}
			else
			{
			    if (dir < 0)
				Cvar_Set("bgmtype","none");
			    else
				Cvar_Set("bgm_extmusic","1");
			}
		}
		else if (q_strcasecmp(bgmtype.string,"cd") == 0)
		{
			if (dir < 0)
			{
				Cvar_Set("bgmtype","midi");
				Cvar_Set("bgm_extmusic","1");
			}
			else
			{
				Cvar_Set("bgmtype","none");
			}
		}
		else
		{
			if (dir < 0)
			{
				Cvar_Set("bgmtype","cd");
			}
			else
			{
				Cvar_Set("bgm_extmusic","0");
				Cvar_Set("bgmtype","midi");
			}
		}
		break;
	case SND_MUSICVOL:
		f = bgmvolume.value + dir * 0.1;
		if (f < 0)	f = 0;
		else if (f > 1)	f = 1;
		Cvar_SetValue ("bgmvolume", f);
		break;
	case SND_SFXVOL:
		f = sfxvolume.value + dir * 0.1;
		if (f < 0)	f = 0;
		else if (f > 1)	f = 1;
		Cvar_SetValue ("volume", f);
		break;
	case SND_WATERFX:
		f = snd_waterfx.value + dir * 0.1;
		if (f < 0)	f = 0;
		else if (f > 2)	f = 2;
		Cvar_SetValue ("snd_waterfx", f);
		break;
	}
}

static void M_Sound_Draw (void)
{
	float	r;

	ScrollTitle("gfx/menu/title3.lmp");
	M_PrintWhite (96, 72, "Sound Options");

	M_Print (76, 92 + 8*SND_MUSICTYPE,	"Music Type    :");
	if (q_strcasecmp(bgmtype.string, "none") == 0)
		M_PrintWhite (76+16*8, 92 + 8*SND_MUSICTYPE, "None");
	else if (q_strcasecmp(bgmtype.string, "cd") == 0)
		M_PrintWhite (76+16*8, 92 + 8*SND_MUSICTYPE, "CD ONLY");
	else if (q_strcasecmp(bgmtype.string, "midi") == 0)
	{
		if (bgm_extmusic.integer)
			M_PrintWhite (76+16*8, 92 + 8*SND_MUSICTYPE, "ALL CODECS");
		else
			M_PrintWhite (76+16*8, 92 + 8*SND_MUSICTYPE, "MIDI ONLY");
	}
	else
		M_PrintWhite (76+16*8, 92 + 8*SND_MUSICTYPE, "MIDI ONLY");

	M_Print (76, 92 + 8*SND_MUSICVOL,	"Music Volume  :");
	r = bgmvolume.value;
	M_DrawSliderValue (220, 92 + 8*SND_MUSICVOL, r, "%.0f%%", r * 100);

	M_Print (76, 92 + 8*SND_SFXVOL,	"Sound Volume  :");
	r = sfxvolume.value;
	M_DrawSliderValue (220, 92 + 8*SND_SFXVOL, r, "%.0f%%", r * 100);

	M_Print (76, 92 + 8*SND_WATERFX,	"Water Muffle  :");
	r = snd_waterfx.value;
	M_DrawSliderValue (220, 92 + 8*SND_WATERFX, r / 2, "%.0f%%", r * 50);

	{ int h = M_MouseToMenuItem(menu_mouse_y, 92, 8, SND_ITEMS); if (h >= 0) sound_cursor = h; }
	M_DrawCharacter (64, 92 + sound_cursor*8, 12+((int)(realtime*4)&1));
}

static void M_Sound_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Options_f ();
		break;
	case K_ENTER:
		m_entersound = true;
		M_Sound_AdjustSliders (1);
		return;
	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		sound_cursor--;
		if (sound_cursor < 0)
			sound_cursor = SND_ITEMS-1;
		break;
	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		sound_cursor++;
		if (sound_cursor >= SND_ITEMS)
			sound_cursor = 0;
		break;
	case K_LEFTARROW:
		M_Sound_AdjustSliders (-1);
		break;
	case K_RIGHTARROW:
		M_Sound_AdjustSliders (1);
		break;
	}
}


//=============================================================================
/* GAME MENU */

enum
{
	GAME_FOV = 0,
	GAME_GUN_FOVSCALE,
	GAME_ALWAYRUN,
	GAME_MOUSESPEED,
	GAME_INVMOUSE,
	GAME_MLOOK,
	GAME_USEMOUSE,
	GAME_RAWINPUT,
	GAME_MFILTER,
	GAME_UIMOUSESND,
	GAME_CROSSHAIR,
	GAME_CHASE,
	GAME_VIEWBOB,
	GAME_VIEWROLL,
	GAME_ANIMSMOOTH,
	GAME_LERPVIEWMDL,
	GAME_UIPREVIEW,
	GAME_CONTRANS,
	GAME_GAMECODE,
	GAME_ITEMS
};

static int	game_cursor;

static const char *game_labels[GAME_ITEMS] = {
	"Field of View :",	/* GAME_FOV */
	"Gun FOV Scale :",	/* GAME_GUN_FOVSCALE */
	"Always Run    :",	/* GAME_ALWAYRUN */
	"Mouse Speed   :",	/* GAME_MOUSESPEED */
	"Invert Mouse  :",	/* GAME_INVMOUSE */
	"Mouse Look    :",	/* GAME_MLOOK */
	"Use Mouse     :",	/* GAME_USEMOUSE */
	"Raw Input     :",	/* GAME_RAWINPUT */
	"Mouse Filter  :",	/* GAME_MFILTER */
	"Mouse Sounds  :",	/* GAME_UIMOUSESND */
	"Crosshair     :",	/* GAME_CROSSHAIR */
	"Chase Mode    :",	/* GAME_CHASE */
	"View Bob      :",	/* GAME_VIEWBOB */
	"View Roll     :",	/* GAME_VIEWROLL */
	"Anim Smoothing:",	/* GAME_ANIMSMOOTH */
	"Smooth Weapon :",	/* GAME_LERPVIEWMDL */
	"Live Preview  :",	/* GAME_UIPREVIEW */
	"Console Alpha :",	/* GAME_CONTRANS */
	"Gamecode from :",	/* GAME_GAMECODE */
};

static qboolean M_Game_IsSkip (int i)
{
	if (i < 0 || i >= GAME_ITEMS) return true;
	/* Hidden only when there is genuinely nothing to choose between.  A
	 * bundle is one way to have a second state, not the only one: on a
	 * bundle-less build a loose progs.dat hiding the pak copy makes "Loose"
	 * and "Pak" distinct loads, and gating on the bundle alone hid the row
	 * from exactly the player who needed it -- one carrying a stale
	 * hand-copied gamecode, whose only other remedy was to delete the file
	 * by hand.  -vanillaprogs still hides it outright, since both predicates
	 * answer false under it.  uhexen2-vbnx, uhexen2-uw6y. */
	if (i == GAME_GAMECODE &&
	    !PR_GamecodeAvailable() && !PR_GamecodePakOnlyAvailable()) return true;
	return M_Filter_Active() && !M_Filter_Matches(game_labels[i]);
}

static void M_Menu_Game_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_game;
	m_entersound = true;
	M_Filter_Clear ();
}

static void M_Game_AdjustSliders (int dir)
{
	float	f;

	S_LocalSound ("raven/menu3.wav");

	switch (game_cursor)
	{
	case GAME_FOV:
		f = scr_fov.value + dir * 2;
		if (f < 60)	f = 60;
		else if (f > 130) f = 130;
		Cvar_SetValue ("fov", f);
		break;
	case GAME_GUN_FOVSCALE:
	{
		float f = Cvar_VariableValue("cl_gun_fovscale") + dir * 0.1f;
		if (f < 0)	f = 0;
		else if (f > 1)	f = 1;
		Cvar_SetValue ("cl_gun_fovscale", f);
		break;
	}
	case GAME_ALWAYRUN:
		Cvar_SetValue ("cl_alwaysrun", !cl_alwaysrun.integer);
		Cvar_Set ("cl_forwardspeed", "200");
		Cvar_Set ("cl_backspeed", "200");
		break;
	case GAME_MOUSESPEED:
		f = sensitivity.value + dir * 0.5;
		if (f > 11)	f = 11;
		else if (f < 1)	f = 1;
		Cvar_SetValue ("sensitivity", f);
		break;
	case GAME_INVMOUSE:
		Cvar_SetValue ("m_pitch", -m_pitch.value);
		break;
	case GAME_MLOOK:
		/* Drives the cvar, as upstream's row does.  Toggling the BUTTON
		 * could never untick this box for a player whose mouselook came
		 * from a config rather than from a held key -- there is no key to
		 * release.  The bare -mlook still goes out when the button really
		 * is latched, both to clear it and because that form now clears
		 * the cvar too.  uhexen2-a5nn.25 */
		if (CL_MouseLookActive())
		{
			Cvar_SetValue ("freelook", 0);
			if (in_mlook.state & 1)
				Cbuf_AddText ("-mlook\n");
		}
		else
		{
			Cvar_SetValue ("freelook", 1);
		}
		break;
	case GAME_USEMOUSE:
		Cvar_Set ("_enable_mouse", _enable_mouse.integer ? "0" : "1");
		break;
	case GAME_RAWINPUT:
		Cvar_SetValue ("m_rawinput", !Cvar_VariableValue("m_rawinput"));
		break;
	case GAME_MFILTER:
		Cvar_SetValue ("m_filter", !m_filter.integer);
		break;
	/* Ironwail folds this into a tri-state "UI mouse" row -- Off / Quiet /
	 * Noisy (M_Options_GetUIMouse, Quake/menu.c:3659) -- where Off means its
	 * ui_mouse cvar has disabled menu pointer support entirely.  We have no
	 * such cvar: menu mouse handling here is unconditional (in_sdl.c feeds
	 * menu_mouse_x/y whenever key_menu is the destination), so the Off state
	 * has nothing to switch off and the row degenerates to the two states
	 * that remain.  uhexen2-n592. */
	case GAME_UIMOUSESND:
		Cvar_SetValue ("ui_mouse_sound", !ui_mouse_sound.integer);
		break;
	case GAME_CROSSHAIR:
		Cvar_Set ("crosshair", crosshair.integer ? "0" : "1");
		break;
	case GAME_CHASE:
		Cvar_Set ("chase_active", chase_active.integer ? "0" : "1");
		break;
	case GAME_VIEWBOB:
		f = Cvar_VariableValue("cl_bob") + dir * 0.005;
		if (f < 0)	f = 0;
		else if (f > 0.05)	f = 0.05;
		Cvar_SetValue ("cl_bob", f);
		break;
	case GAME_VIEWROLL:
		f = Cvar_VariableValue("cl_rollangle") + dir * 0.5;
		if (f < 0)	f = 0;
		else if (f > 5)	f = 5;
		Cvar_SetValue ("cl_rollangle", f);
		break;
	case GAME_ANIMSMOOTH:
		Cvar_SetValue ("r_lerpmodels", !Cvar_VariableValue("r_lerpmodels"));
		break;
	case GAME_LERPVIEWMDL:
		Cvar_SetValue ("r_lerp_viewmodel", !Cvar_VariableValue("r_lerp_viewmodel"));
		break;
	/* Governs the *other* submenus, not this one: with it on, Display /
	 * Video / Rendering / Graphics skip the amber fade so a setting is
	 * judged against a clean game view while it is being adjusted (M_Draw).
	 * Ironwail carries the same switch as a plain checkbox in its Options
	 * menu (OPT_PREVIEW, Quake/menu.c:4346). */
	case GAME_UIPREVIEW:
		Cvar_SetValue ("ui_live_preview", !ui_live_preview.integer);
		break;
	case GAME_CONTRANS:
		f = Cvar_VariableValue("contrans") + dir * 1;
		if (f < 0)	f = 0;
		else if (f > 2)	f = 2;
		Cvar_SetValue ("contrans", f);
		break;

	case GAME_GAMECODE:
		/* Takes effect on the next new game, never mid-campaign -- see
		 * PR_LatchGamecode.  uhexen2-vbnx.
		 *
		 * A rotation, not a flip, and how far it rotates depends on the
		 * install: state 2 (the pak copy specifically) is only a distinct
		 * outcome where a loose file is hiding that copy.  Everywhere else
		 * it resolves the identical file state 0 does, and stepping through
		 * a setting that changes nothing is the inert control M_Game_IsSkip
		 * hides the whole row to avoid.  uhexen2-nt96. */
		{
			int	avail[3];
			int	n = 0, cur = 0, i;
			int	v = (int)Cvar_VariableValue("sv_gamecode");

			/* Build the reachable states rather than assuming a count.  State
			 * 1 and state 2 are independently available -- a bundle-less
			 * install can offer 0 and 2, which the old fixed "2 states, and a
			 * 3rd if pak-only" shape could not express and would have rotated
			 * into the unreachable state 1. */
			avail[n++] = 0;				/* the install's own file */
			if (PR_GamecodeAvailable())
				avail[n++] = 1;			/* the bundle beside the engine */
			if (PR_GamecodePakOnlyAvailable())
				avail[n++] = 2;			/* the pak copy specifically */

			/* Mirror PR_GamecodeWanted before looking v up: any nonzero
			 * value that is not state 2 means state 1, which is what every
			 * nonzero value meant while this was a boolean.  Without this a
			 * config carrying such a value would sit at cur 0 and spend its
			 * first press moving to a state the row already names. */
			if (v != 0 && v != 2)
				v = 1;

			/* CVAR_ARCHIVE: the config may have come from an install with
			 * different answers above, or from a build that predates a state.
			 * Anything unreachable folds onto 0 so the row cannot get stuck. */
			for (i = 0; i < n; i++)
			{
				if (avail[i] == v)
				{
					cur = i;
					break;
				}
			}

			cur = (cur + dir) % n;
			if (cur < 0)
				cur += n;
			Cvar_SetValue ("sv_gamecode", avail[cur]);
		}
		break;
	}
}

static void M_Game_Draw (void)
{
	float	r;

	ScrollTitle("gfx/menu/title3.lmp");
	M_PrintWhite (96, 72, "Game Options");

	if (!M_Game_IsSkip(GAME_FOV))
	{
		M_Print (76, 92 + 8*GAME_FOV, game_labels[GAME_FOV]);
		r = (scr_fov.value - 60) / (130 - 60);
		M_DrawSliderValue (220, 92 + 8*GAME_FOV, r, "%.0f°", scr_fov.value);
	}

	if (!M_Game_IsSkip(GAME_GUN_FOVSCALE))
	{
		float s = Cvar_VariableValue("cl_gun_fovscale");
		M_Print (76, 92 + 8*GAME_GUN_FOVSCALE, game_labels[GAME_GUN_FOVSCALE]);
		if (s <= 0)
			M_PrintWhite (220, 92 + 8*GAME_GUN_FOVSCALE, "Off");
		else
			M_DrawSliderValue (220, 92 + 8*GAME_GUN_FOVSCALE, s, "%.0f%%", s * 100);
	}

	if (!M_Game_IsSkip(GAME_ALWAYRUN))
	{
		M_Print (76, 92 + 8*GAME_ALWAYRUN, game_labels[GAME_ALWAYRUN]);
		M_DrawCheckbox (220, 92 + 8*GAME_ALWAYRUN, cl_alwaysrun.integer);
	}

	if (!M_Game_IsSkip(GAME_MOUSESPEED))
	{
		M_Print (76, 92 + 8*GAME_MOUSESPEED, game_labels[GAME_MOUSESPEED]);
		r = (sensitivity.value - 1) / 10;
		M_DrawSliderValue (220, 92 + 8*GAME_MOUSESPEED, r, "%.2f", sensitivity.value);
	}

	if (!M_Game_IsSkip(GAME_INVMOUSE))
	{
		M_Print (76, 92 + 8*GAME_INVMOUSE, game_labels[GAME_INVMOUSE]);
		M_DrawCheckbox (220, 92 + 8*GAME_INVMOUSE, m_pitch.value < 0);
	}

	if (!M_Game_IsSkip(GAME_MLOOK))
	{
		M_Print (76, 92 + 8*GAME_MLOOK, game_labels[GAME_MLOOK]);
		M_DrawCheckbox (220, 92 + 8*GAME_MLOOK, CL_MouseLookActive());
	}

	if (!M_Game_IsSkip(GAME_USEMOUSE))
	{
		M_Print (76, 92 + 8*GAME_USEMOUSE, game_labels[GAME_USEMOUSE]);
		M_DrawCheckbox (220, 92 + 8*GAME_USEMOUSE, _enable_mouse.integer);
	}

	if (!M_Game_IsSkip(GAME_RAWINPUT))
	{
		M_Print (76, 92 + 8*GAME_RAWINPUT, game_labels[GAME_RAWINPUT]);
		M_DrawCheckbox (220, 92 + 8*GAME_RAWINPUT, (int)Cvar_VariableValue("m_rawinput"));
	}

	if (!M_Game_IsSkip(GAME_MFILTER))
	{
		M_Print (76, 92 + 8*GAME_MFILTER, game_labels[GAME_MFILTER]);
		M_DrawCheckbox (220, 92 + 8*GAME_MFILTER, m_filter.integer);
	}

	if (!M_Game_IsSkip(GAME_UIMOUSESND))
	{
		M_Print (76, 92 + 8*GAME_UIMOUSESND, game_labels[GAME_UIMOUSESND]);
		M_DrawCheckbox (220, 92 + 8*GAME_UIMOUSESND, ui_mouse_sound.integer);
	}

	if (!M_Game_IsSkip(GAME_CROSSHAIR))
	{
		M_Print (76, 92 + 8*GAME_CROSSHAIR, game_labels[GAME_CROSSHAIR]);
		M_DrawCheckbox (220, 92 + 8*GAME_CROSSHAIR, crosshair.integer);
	}

	if (!M_Game_IsSkip(GAME_CHASE))
	{
		M_Print (76, 92 + 8*GAME_CHASE, game_labels[GAME_CHASE]);
		M_DrawCheckbox (220, 92 + 8*GAME_CHASE, chase_active.integer);
	}

	if (!M_Game_IsSkip(GAME_VIEWBOB))
	{
		M_Print (76, 92 + 8*GAME_VIEWBOB, game_labels[GAME_VIEWBOB]);
		r = Cvar_VariableValue("cl_bob") / 0.05;
		M_DrawSliderValue (220, 92 + 8*GAME_VIEWBOB, r, "%.0f%%", Cvar_VariableValue("cl_bob") * 100);
	}

	if (!M_Game_IsSkip(GAME_VIEWROLL))
	{
		M_Print (76, 92 + 8*GAME_VIEWROLL, game_labels[GAME_VIEWROLL]);
		r = Cvar_VariableValue("cl_rollangle") / 5.0;
		M_DrawSliderValue (220, 92 + 8*GAME_VIEWROLL, r, "%.0f%%", Cvar_VariableValue("cl_rollangle") * 20);
	}

	if (!M_Game_IsSkip(GAME_ANIMSMOOTH))
	{
		M_Print (76, 92 + 8*GAME_ANIMSMOOTH, game_labels[GAME_ANIMSMOOTH]);
		M_DrawCheckbox (220, 92 + 8*GAME_ANIMSMOOTH, (int)Cvar_VariableValue("r_lerpmodels"));
	}

	if (!M_Game_IsSkip(GAME_LERPVIEWMDL))
	{
		M_Print (76, 92 + 8*GAME_LERPVIEWMDL, game_labels[GAME_LERPVIEWMDL]);
		M_DrawCheckbox (220, 92 + 8*GAME_LERPVIEWMDL, (int)Cvar_VariableValue("r_lerp_viewmodel"));
	}

	if (!M_Game_IsSkip(GAME_UIPREVIEW))
	{
		M_Print (76, 92 + 8*GAME_UIPREVIEW, game_labels[GAME_UIPREVIEW]);
		M_DrawCheckbox (220, 92 + 8*GAME_UIPREVIEW, ui_live_preview.integer);
	}

	if (!M_Game_IsSkip(GAME_CONTRANS))
	{
		int ct = (int)Cvar_VariableValue("contrans");
		M_Print (76, 92 + 8*GAME_CONTRANS, game_labels[GAME_CONTRANS]);
		M_PrintWhite (220, 92 + 8*GAME_CONTRANS,
			ct == 0 ? "Opaque" : ct == 1 ? "Light" : "Clear");
	}

	/* Names WHERE the gamecode comes from, never whose it is.  The first cut
	 * read "Classic" / "Updated", which is a claim about authorship, and it is
	 * wrong on any install carrying a loose copy of OUR progs.dat in data1/ --
	 * put there by the pre-bundle install instructions, and loaded by
	 * "Classic".  A source is something this row can state truthfully every
	 * time.  The value column starts at x=220 on a 320-unit canvas, so 12
	 * characters; the longest of these is 7.  uhexen2-nt96. */
	if (!M_Game_IsSkip(GAME_GAMECODE))
	{
		int		gc = (int)Cvar_VariableValue("sv_gamecode");
		const char	*src;

		/* Fold the archived value onto what this install can actually reach
		 * before naming it.  A config carrying state 1 from a bundled build,
		 * read on a build that ships no bundle, would otherwise read
		 * "Bundled" for a load that cannot happen: PR_BundledProgsPath gates
		 * on the same three conditions PR_GamecodeAvailable tests, so with it
		 * false the install's own file is what loads.  Rotating the row is
		 * not what makes this true, so the fold cannot live in the rotation
		 * -- the row has to read correctly before it is ever touched. */
		if (gc != 0 && gc != 2 && !PR_GamecodeAvailable())
			gc = 0;
		if (gc == 2 && !PR_GamecodePakOnlyAvailable())
			gc = 0;

		if (gc == 2)
			src = "Pak";
		else if (gc != 0)
			src = "Bundled";
		else	/* state 0, and the states that fold onto it */
			src = PR_GamecodeLooseShadowsPak() ? "Loose" : "Pak";

		M_Print (76, 92 + 8*GAME_GAMECODE, game_labels[GAME_GAMECODE]);
		M_PrintWhite (220, 92 + 8*GAME_GAMECODE, src);
	}

	/* Status, not a control: no enum slot, no cursor stop, nothing to press.
	 * The row above names a SOURCE; this one names the author of what came out
	 * of it, and the pair is only informative because the two can disagree --
	 * a progs.dat hand-copied into the install's data1/ is loaded by "Loose"
	 * and is ours.  Drawn whether or not the row above is (that one hides
	 * itself when no bundle shipped, and a player in exactly that state still
	 * hand-copies).  NULL until a map has loaded -- including on a client
	 * attached to a remote server, where the answer is genuinely unknown --
	 * and then the row is simply absent rather than showing a placeholder.
	 * uhexen2-8r3e. */
	/* One full-width line rather than the label/value pair every row above
	 * uses.  "hexenwail-2026-08-15" is 20 characters and the value column
	 * starts at x=220 on a canvas 320 units wide, which leaves room for 12 --
	 * so in two columns the stamp would run off the right edge.  Left-aligned
	 * at x=16 the whole string ends at 312 and fits.  The other long form,
	 * "hexenwail (undated)", is 19 and ends at 304 (uhexen2-nt96). */
	{
		const char	*ident = PR_GamecodeIdent();

		if (ident && (!M_Filter_Active() || M_Filter_Matches("Gamecode")))
		{
			M_Print (16, 92 + 8*GAME_ITEMS, "Gamecode loaded:");
			M_PrintWhite (16 + 8*17, 92 + 8*GAME_ITEMS, ident);
		}
	}

	{
		int h = M_MouseToMenuItem(menu_mouse_y, 92, 8, GAME_ITEMS);
		if (h >= 0 && !M_Game_IsSkip(h))
			game_cursor = h;
	}
	if (!M_Game_IsSkip(game_cursor))
		M_DrawCharacter (64, 92 + game_cursor*8, 12+((int)(realtime*4)&1));

	M_Filter_Draw (76, 92 + 8*(GAME_ITEMS + 1));
}

static void M_Game_Key (int k)
{
	if (k != K_ESCAPE && k != K_ENTER &&
	    k != K_UPARROW && k != K_DOWNARROW &&
	    k != K_LEFTARROW && k != K_RIGHTARROW)
	{
		if (M_Filter_HandleKey (k))
		{
			int i;
			if (M_Game_IsSkip (game_cursor))
			{
				for (i = 0; i < GAME_ITEMS; i++)
				{
					if (!M_Game_IsSkip (i))
					{
						game_cursor = i;
						break;
					}
				}
			}
			return;
		}
	}

	switch (k)
	{
	case K_ESCAPE:
		if (M_Filter_Active ())
		{
			M_Filter_Clear ();
			return;
		}
		M_Menu_Options_f ();
		break;
	case K_ENTER:
		m_entersound = true;
		M_Game_AdjustSliders (1);
		return;
	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		{
			int guard = GAME_ITEMS;
			do {
				game_cursor--;
				if (game_cursor < 0)
					game_cursor = GAME_ITEMS-1;
			} while (M_Game_IsSkip (game_cursor) && --guard > 0);
		}
		break;
	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		{
			int guard = GAME_ITEMS;
			do {
				game_cursor++;
				if (game_cursor >= GAME_ITEMS)
					game_cursor = 0;
			} while (M_Game_IsSkip (game_cursor) && --guard > 0);
		}
		break;
	case K_LEFTARROW:
		M_Game_AdjustSliders (-1);
		break;
	case K_RIGHTARROW:
		M_Game_AdjustSliders (1);
		break;
	}
}



//=============================================================================
/* GAMEPAD MENU */

enum
{
	GPAD_ENABLE = 0,
	GPAD_SENSX,
	GPAD_SENSY,
	GPAD_INVERT,
	GPAD_SWAPSTICKS,
	GPAD_ACCEL_LOOK,
	GPAD_ACCEL_MOVE,
	GPAD_DZ_LOOK,
	GPAD_DZ_MOVE,
	GPAD_DZ_TRIGGER,
	GPAD_RUMBLE,
	GPAD_ITEMS
};

static int	gamepad_cursor;

static void M_Menu_Gamepad_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_gamepad;
	m_entersound = true;
}

static void M_Gamepad_Draw (void)
{
	float	r;
	int	y;

	ScrollTitle("gfx/menu/title3.lmp");

	if (IN_HasGamepad())
		M_PrintWhite (80, 72, "Controller Options");
	else
		M_PrintWhite (64, 72, "Controller (not connected)");

	y = 90;

	M_Print (32, y + 8*GPAD_ENABLE, "Gamepad Enabled");
	M_DrawCheckbox (220, y + 8*GPAD_ENABLE, in_gamepad.integer);

	M_Print (32, y + 8*GPAD_SENSX, "Yaw Speed");
	r = (joy_sensitivity_yaw.value - 60) / (720 - 60);
	M_DrawSlider (220, y + 8*GPAD_SENSX, r);

	M_Print (32, y + 8*GPAD_SENSY, "Pitch Speed");
	r = (joy_sensitivity_pitch.value - 60) / (720 - 60);
	M_DrawSlider (220, y + 8*GPAD_SENSY, r);

	M_Print (32, y + 8*GPAD_INVERT, "Invert Pitch");
	M_DrawCheckbox (220, y + 8*GPAD_INVERT, joy_invert.integer);

	M_Print (32, y + 8*GPAD_SWAPSTICKS, "Swap Sticks");
	M_DrawCheckbox (220, y + 8*GPAD_SWAPSTICKS, joy_swapmovelook.integer);

	M_Print (32, y + 8*GPAD_ACCEL_LOOK, "Look Accel");
	r = (joy_exponent.value - 1.0) / (5.0 - 1.0);
	M_DrawSlider (220, y + 8*GPAD_ACCEL_LOOK, r);

	M_Print (32, y + 8*GPAD_ACCEL_MOVE, "Move Accel");
	r = (joy_exponent_move.value - 1.0) / (5.0 - 1.0);
	M_DrawSlider (220, y + 8*GPAD_ACCEL_MOVE, r);

	M_Print (32, y + 8*GPAD_DZ_LOOK, "Look Deadzone");
	r = joy_deadzone_look.value / 0.5;
	M_DrawSlider (220, y + 8*GPAD_DZ_LOOK, r);

	M_Print (32, y + 8*GPAD_DZ_MOVE, "Move Deadzone");
	r = joy_deadzone_move.value / 0.5;
	M_DrawSlider (220, y + 8*GPAD_DZ_MOVE, r);

	M_Print (32, y + 8*GPAD_DZ_TRIGGER, "Trigger Thresh");
	r = joy_deadzone_trigger.value / 0.75;
	M_DrawSlider (220, y + 8*GPAD_DZ_TRIGGER, r);

	M_Print (32, y + 8*GPAD_RUMBLE, "Vibration");
	r = joy_rumble.value;
	M_DrawSlider (220, y + 8*GPAD_RUMBLE, r);

	{ int h = M_MouseToMenuItem(menu_mouse_y, 90, 8, GPAD_ITEMS); if (h >= 0) gamepad_cursor = h; }
	M_DrawCharacter (208, y + gamepad_cursor*8, 12 + ((int)(realtime*4) & 1));
}

static void M_Gamepad_AdjustSliders (int dir)
{
	float	f;

	S_LocalSound ("raven/menu3.wav");

	switch (gamepad_cursor)
	{
	case GPAD_ENABLE:
		Cvar_Set ("gamepad", in_gamepad.integer ? "0" : "1");
		break;
	case GPAD_SENSX:
		f = joy_sensitivity_yaw.value + dir * 30;
		if (f < 60)  f = 60;
		if (f > 720) f = 720;
		Cvar_SetValue ("joy_sensitivity_yaw", f);
		break;
	case GPAD_SENSY:
		f = joy_sensitivity_pitch.value + dir * 30;
		if (f < 60)  f = 60;
		if (f > 720) f = 720;
		Cvar_SetValue ("joy_sensitivity_pitch", f);
		break;
	case GPAD_INVERT:
		Cvar_Set ("joy_invert", joy_invert.integer ? "0" : "1");
		break;
	case GPAD_SWAPSTICKS:
		Cvar_Set ("joy_swapmovelook", joy_swapmovelook.integer ? "0" : "1");
		break;
	case GPAD_ACCEL_LOOK:
		f = joy_exponent.value + dir * 0.25;
		if (f < 1.0) f = 1.0;
		if (f > 5.0) f = 5.0;
		Cvar_SetValue ("joy_exponent", f);
		break;
	case GPAD_ACCEL_MOVE:
		f = joy_exponent_move.value + dir * 0.25;
		if (f < 1.0) f = 1.0;
		if (f > 5.0) f = 5.0;
		Cvar_SetValue ("joy_exponent_move", f);
		break;
	case GPAD_DZ_LOOK:
		f = joy_deadzone_look.value + dir * 0.025;
		if (f < 0)   f = 0;
		if (f > 0.5) f = 0.5;
		Cvar_SetValue ("joy_deadzone_look", f);
		break;
	case GPAD_DZ_MOVE:
		f = joy_deadzone_move.value + dir * 0.025;
		if (f < 0)   f = 0;
		if (f > 0.5) f = 0.5;
		Cvar_SetValue ("joy_deadzone_move", f);
		break;
	case GPAD_DZ_TRIGGER:
		f = joy_deadzone_trigger.value + dir * 0.05;
		if (f < 0)    f = 0;
		if (f > 0.75) f = 0.75;
		Cvar_SetValue ("joy_deadzone_trigger", f);
		break;
	case GPAD_RUMBLE:
		f = joy_rumble.value + dir * 0.1;
		if (f < 0) f = 0;
		if (f > 1) f = 1;
		Cvar_SetValue ("joy_rumble", f);
		break;
	}
}

static void M_Gamepad_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
	case K_GP_B:
		M_Menu_Options_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		gamepad_cursor--;
		if (gamepad_cursor < 0)
			gamepad_cursor = GPAD_ITEMS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		gamepad_cursor++;
		if (gamepad_cursor >= GPAD_ITEMS)
			gamepad_cursor = 0;
		break;

	case K_LEFTARROW:
		M_Gamepad_AdjustSliders (-1);
		break;

	case K_RIGHTARROW:
	case K_ENTER:
	case K_GP_A:
		M_Gamepad_AdjustSliders (1);
		break;
	}
}


//=============================================================================
/* KEYS MENU */

/* Engine defaults. These seed the runtime row list built by M_BuildBindList();
 * a mod's bindlist.lst is appended after them, never in place of them. */
static const char *bindnames[][2] =
{
	{"+attack",		"attack"},
	{"impulse 10",		"next weapon"},
	{"impulse 12",		"prev.weapon"},
	{"impulse 1",		"weapon 1"},
	{"impulse 2",		"weapon 2"},
	{"impulse 3",		"weapon 3"},
	{"impulse 4",		"weapon 4"},
	{"+jump",		"jump / swim up"},
	{"+forward",		"walk forward"},
	{"+back",		"backpedal"},
	{"+left",		"turn left"},
	{"+right",		"turn right"},
	{"+speed",		"run"},
	{"+moveleft",		"step left"},
	{"+moveright",		"step right"},
	{"+strafe",		"sidestep"},
	{"+crouch",		"crouch"},
	{"+lookup",		"look up"},
	{"+lookdown",		"look down"},
	{"centerview",		"center view"},
	{"togglechase",		"3rd person"},
	{"+moveup",		"swim up"},
	{"+movedown",		"swim down"},
	{"impulse 13",		"lift object"},
	{"invuse",		"use inv item"},
	{"impulse 44",		"drop inv item"},
	{"+showinfo",		"full inventory"},
	{"+showdm",		"info / frags"},
	{"toggle_dm",		"toggle frags"},
	{"+infoplaque",		"objectives"},	/* command to display the mission pack's objectives */
	{"invleft",		"inv move left"},
	{"invright",		"inv move right"},
	{"impulse 100",		"inv:torch"},
	{"impulse 101",		"inv:qrtz flask"},
	{"impulse 102",		"inv:mystic urn"},
	{"impulse 103",		"inv:krater"},
	{"impulse 104",		"inv:chaos devc"},
	{"impulse 105",		"inv:tome power"},
	{"impulse 106",		"inv:summon stn"},
	{"impulse 107",		"inv:invisiblty"},
	{"impulse 108",		"inv:glyph"},
	{"impulse 109",		"inv:boots"},
	{"impulse 110",		"inv:repulsion"},
	{"impulse 111",		"inv:bo peep"},
	{"impulse 112",		"inv:flight"},
	{"impulse 113",		"inv:force cube"},
	{"impulse 114",		"inv:icon defn"}
};

#define	NUM_ENGINE_BINDS	(sizeof(bindnames)/sizeof(bindnames[0]))

/* Mod-supplied rows, parsed from the gamedir's bindlist.lst. Caps are hard:
 * a malformed or hostile file must degrade to "engine binds only", not crash.
 * MAX_BIND_COMMAND is sized so the longest command still fits M_Keybind()'s
 * "bind2 \"KEY\" \"command\"" buffer without truncation. */
#define	MAX_MOD_BINDS		64
#define	MAX_BIND_COMMAND	48
#define	MAX_BIND_LABEL		32

typedef struct
{
	char	command[MAX_BIND_COMMAND];
	char	label[MAX_BIND_LABEL];
} modbind_t;

static modbind_t	modbinds[MAX_MOD_BINDS];

/* The list the menu actually renders: engine rows, then mod rows. Entries
 * point either at the bindnames literals or into modbinds, so nothing here
 * is dynamically allocated and nothing can leak across rebuilds. */
static const char	*keys_bindlist[NUM_ENGINE_BINDS + MAX_MOD_BINDS][2];
static int		keys_numcommands;

#define KEYS_SIZE 14

/* Bind-conflict report (uhexen2-yae).  Binding a key that another action
 * already owns silently takes it away -- the old owner's row just reverts to
 * "???", and if it is on another page of this scrolling list, or bound to
 * something with no row at all (a console command, a default from
 * default.cfg), nothing on screen says the key moved.  Players discover it
 * mid-game.  Record what was displaced and say so under the list. */
static char		keys_conflict_msg[64];
static double		keys_conflict_time;
#define KEYS_CONFLICT_SECONDS	4.0

static int		keys_cursor;
static int		keys_top = 0;
static qboolean		keys_tap = false;	// TAB toggles tap binding mode

/*
================
M_BindCommandIsUsable

Would this bindlist.lst row actually DO anything in this engine?

A .lst is written for whatever engine the modder had, and the format is shared
with QSS and Keep -- so rows routinely name commands we do not have.  Ironwail
has filtered these since it gained the feature (Quake/menu.c, the +voip case in
Mjolnir's bindlist.lst) and we did not port that half with uhexen2-7aok, so
until now such a row appeared in Key Setup as an ordinary, bindable line that
silently bound a key to nothing.  uhexen2-a5nn.22.

Only the FIRST token is tested, which is what makes "impulse 23" work: the
command is `impulse` and the rest is its argument.

CVARS COUNT, where upstream tests only commands and aliases.  Typing a cvar
name at the console is a legitimate thing to bind -- it prints or sets it --
so treating `viewsize` as unusable would drop a row that works.  Erring toward
keeping a row is the right direction here: a kept row that does nothing is a
cosmetic wart, a dropped row is mod content the player cannot rebind.
================
*/
static qboolean M_BindCommandIsUsable (const char *cmd)
{
	const char	*data;

	data = COM_Parse (cmd);
	(void) data;	/* COM_Parse reports emptiness through com_token, not its return */
	if (!com_token[0])
		return false;

	/* NO DEPRECATED-NAME LIST, though upstream has one.  Its entries are
	 * `+klook` and `+mlook`, deprecated because IRONWAIL REMOVED THEM; this
	 * engine still registers both (cl_input.c) and menu.c issues `+mlook`
	 * itself, so copying that list would drop two rows that work.  The
	 * existence test below is the whole filter, and it gets those right by
	 * construction -- which is the argument for testing what the engine has
	 * rather than transcribing what upstream lacks. */
	return Cmd_Exists (com_token) || Cmd_AliasExists (com_token) ||
	       Cvar_FindVar (com_token) != NULL;
}

/*
================
M_BuildBindList

Rebuilds the Key Setup rows: the engine defaults, then any extra rows from
the current gamedir's bindlist.lst -- a list of "command" "label" pairs, as
used by QSS and Keep.

Ported from Ironwail 096c3d952, which deliberately differs from QSS here:
the mod's rows are MERGED after the engine's rather than replacing them, so
a mod that lists three of its own commands doesn't lose the movement rows.

Rows naming something this engine cannot run are dropped -- see
M_BindCommandIsUsable, the half of upstream's handling uhexen2-7aok missed.

Also called on a gamedir switch, so a previous mod's rows can't survive into
the next one even if the Key Setup menu is on screen at the time.
================
*/
void M_BuildBindList (void)
{
	const char	*data;
	size_t		i;
	int		n;

	for (i = 0; i < NUM_ENGINE_BINDS; i++)
	{
		keys_bindlist[i][0] = bindnames[i][0];
		keys_bindlist[i][1] = bindnames[i][1];
	}
	keys_numcommands = (int) NUM_ENGINE_BINDS;

	memset (modbinds, 0, sizeof(modbinds));

	/* temp hunk: resolves out of the gamedir and out of PAKs, and is
	 * reclaimed on its own, so repeated rebuilds cost nothing */
	data = (const char *) FS_LoadTempFile ("bindlist.lst", NULL);

	for (n = 0; data && n < MAX_MOD_BINDS; )
	{
		data = COM_Parse (data);
		if (!data)
			break;			/* end of file */
		q_strlcpy (modbinds[n].command, com_token, sizeof(modbinds[n].command));

		data = COM_Parse (data);
		if (!data)
			break;			/* odd token count: drop the dangling command */
		q_strlcpy (modbinds[n].label, com_token, sizeof(modbinds[n].label));

		if (!modbinds[n].command[0] || !modbinds[n].label[0])
			continue;		/* empty quoted token: skip the pair, reuse the slot */

		if (!M_BindCommandIsUsable (modbinds[n].command))
		{
			/* Named so the modder can find it: this is the only signal
			 * that a row they wrote is not showing up.  Upstream prints
			 * the same thing at the same verbosity. */
			Con_DPrintf ("Skipping unsupported key binding: \"%s\" \"%s\"\n",
				     modbinds[n].command, modbinds[n].label);
			continue;		/* nothing here would run it; reuse the slot */
		}

		keys_bindlist[keys_numcommands][0] = modbinds[n].command;
		keys_bindlist[keys_numcommands][1] = modbinds[n].label;
		keys_numcommands++;
		n++;
	}

	if (keys_cursor >= keys_numcommands)
		keys_cursor = keys_numcommands - 1;
	if (keys_cursor < 0)
		keys_cursor = 0;
	if (keys_top > keys_numcommands - KEYS_SIZE)
		keys_top = keys_numcommands - KEYS_SIZE;
	if (keys_top < 0)
		keys_top = 0;
}

static void M_Menu_Keys_f (void)
{
	M_BuildBindList ();
	keys_tap = false;
	Key_SetDest (key_menu);
	m_state = m_keys;
	m_entersound = true;
}


static void M_FindKeysForCommand (const char *command, int *twokeys)
{
	int		count;
	int		j;
	int		l,l2;
	char	*b;

	twokeys[0] = twokeys[1] = -1;
	l = strlen(command);
	count = 0;

	for (j = 0; j < MAX_KEYS; j++)
	{
		b = keybindings[j];
		if (!b)
			continue;
		if (!strncmp (b, command, l))
		{
			l2 = strlen(b);
			if (l == l2)
			{
				twokeys[count] = j;
				count++;
				if (count == 2)
					break;
			}
		}
	}
}

static void M_UnbindCommand (const char *command)
{
	int		j;
	int		l;
	char	*b;

	l = strlen(command);

	for (j = 0; j < MAX_KEYS; j++)
	{
		b = keybindings[j];
		if (!b)
			continue;
		if (!strncmp (b, command, l) )
			Key_SetBinding (j, NULL);
	}
}

static void M_FindDoubleKeysForCommand (const char *command, int *twokeys)
{
	int		count;
	int		j;
	int		l, l2;
	char	*b;

	twokeys[0] = twokeys[1] = -1;
	l = strlen(command);
	count = 0;

	for (j = 0; j < MAX_KEYS; j++)
	{
		b = doublebindings[j];
		if (!b)
			continue;
		if (!strncmp (b, command, l))
		{
			l2 = strlen(b);
			if (l == l2)
			{
				twokeys[count] = j;
				count++;
				if (count == 2)
					break;
			}
		}
	}
}

static void M_UnbindDoubleCommand (const char *command)
{
	int		j;
	int		l;
	char	*b;
	char	cmd[80];

	l = strlen(command);

	for (j = 0; j < MAX_KEYS; j++)
	{
		b = doublebindings[j];
		if (!b)
			continue;
		if (!strncmp (b, command, l))
		{
			q_snprintf (cmd, sizeof(cmd), "unbind2 \"%s\"\n", Key_KeynumToString(j));
			Cbuf_InsertText (cmd);
		}
	}
}


/*
================
M_Keys_FitLabel

The label column runs from x=16 to the cursor at x=130, which is fourteen 8px
characters.  Every engine label was written to fit; a mod's bindlist.lst label
need not be, and the storage cap is 32 on purpose (uhexen2-7aok kept the modder's
whole string rather than silently shortening it at parse time).  So the shortening
happens here, at the draw, where it costs nothing but the pixels it saves: a long
label ends in "..." instead of running through the key names.  uhexen2-tgr7.
================
*/
#define	KEYS_LABEL_COLS		14

static const char *M_Keys_FitLabel (const char *label)
{
	static char	fitted[KEYS_LABEL_COLS + 1];

	if (strlen(label) <= KEYS_LABEL_COLS)
		return label;

	q_strlcpy (fitted, label, KEYS_LABEL_COLS - 3 + 1);
	q_strlcat (fitted, "...", sizeof(fitted));
	return fitted;
}

static void M_Keys_Draw (void)
{
	int		i, x, y;
	int		keys[2];
	const char	*name;

	ScrollTitle("gfx/menu/title6.lmp");

	/* Mode indicator */
	if (keys_tap)
		M_Print (96, 56, "TAP  (TAB: normal)");
	else
		M_Print (92, 56, "NORMAL  (TAB: tap)");

	if (keys_top)
		M_DrawCharacter (6, 80, 128);
	if (keys_top + KEYS_SIZE < keys_numcommands)
		M_DrawCharacter (6, 80 + ((KEYS_SIZE-1)*8), 129);

	/* Proportional scrollbar on right edge */
	if (keys_numcommands > KEYS_SIZE)
	{
		int track_y = 80;
		int track_h = KEYS_SIZE * 8;
		int thumb_h = (KEYS_SIZE * track_h) / keys_numcommands;
		int thumb_y = track_y + (keys_top * track_h) / keys_numcommands;
		int j;
		if (thumb_h < 8) thumb_h = 8;
		for (j = 0; j < track_h; j += 8)
		{
			int cy = track_y + j;
			if (cy >= thumb_y && cy < thumb_y + thumb_h)
				M_DrawCharacter (308, cy, 11);	/* solid block */
			else
				M_DrawCharacter (308, cy, '-');	/* track dash */
		}
	}

// search for known bindings
	for (i = 0; i < KEYS_SIZE; i++)
	{
		if (i + keys_top >= keys_numcommands)
			break;		/* count is now runtime, so it may fall short of a full page */

		y = 80 + 8*i;

		M_Print (16, y, M_Keys_FitLabel (keys_bindlist[i+keys_top][1]));

		if (keys_tap)
			M_FindDoubleKeysForCommand (keys_bindlist[i+keys_top][0], keys);
		else
			M_FindKeysForCommand (keys_bindlist[i+keys_top][0], keys);

		if (keys[0] == -1)
		{
			/* While grabbing a key for this row, preview that holding the
			 * gamepad alt-modifier will bind the second (ALT) layer. */
			if ((Key_GetDest() & key_bindbit) && (i + keys_top) == keys_cursor &&
			    Key_GetGamepadAltModifierState())
				M_Print (140, y, "Alt-???");
			else
				M_Print (140, y, "???");
		}
		else
		{
			name = Key_KeynumToDisplayString (keys[0]);
			M_Print (140, y, name);
			x = strlen(name) * 8;
			if (keys[1] != -1)
			{
				M_Print (140 + x + 8, y, "or");
				M_Print (140 + x + 32, y, Key_KeynumToDisplayString (keys[1]));
			}
		}
	}

	/* Mouse hover — map visible row to absolute key index */
	if (!(Key_GetDest() & key_bindbit))
	{
		int h = M_MouseToMenuItem(menu_mouse_y, 80, 8, KEYS_SIZE);
		if (h >= 0)
			keys_cursor = keys_top + h;
	}

	if (Key_GetDest() & key_bindbit)
	{
		if (Key_GetGamepadAltModifierState())
			M_Print (12, 64, "Press a gamepad button for the ALT layer");
		else if (keys_tap)
			M_Print (12, 64, "Press a key or button for TAP action");
		else
			M_Print (12, 64, "Press a key or button for this action");
		M_DrawCharacter (130, 80 + (keys_cursor-keys_top)*8, '=');
	}
	else
	{
		/* Conflict notice takes over the instruction line for a few seconds
		 * after a bind that displaced another action.  On the instruction
		 * line because that is where this menu already talks to the user,
		 * and white rather than the menu's gold so it reads as a
		 * notification and not another row.
		 * Timed out rather than sticky: it describes an edit that already
		 * happened, and would go stale at the next rebind.  uhexen2-yae. */
		if (keys_conflict_msg[0] &&
		    realtime - keys_conflict_time < KEYS_CONFLICT_SECONDS)
		{
			int len = (int) strlen (keys_conflict_msg);
			M_PrintWhite ((320 - len*8) / 2, 64, keys_conflict_msg);
		}
		else
			M_Print (18, 64, "Enter to change, backspace to clear");

		M_DrawCharacter (130, 80 + (keys_cursor-keys_top)*8, 12+((int)(realtime*4)&1));
	}
}


static void M_Keys_Key (int k)
{
	int		keys[2];

	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Options_f ();
		break;

	case K_LEFTARROW:
	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		keys_cursor--;
		if (keys_cursor < 0)
			keys_cursor = keys_numcommands-1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("raven/menu1.wav");
		keys_cursor++;
		if (keys_cursor >= keys_numcommands)
			keys_cursor = 0;
		break;

	case K_TAB:		// toggle normal / double-tap mode
		keys_tap = !keys_tap;
		S_LocalSound ("raven/menu1.wav");
		break;

	case K_ENTER:		// go into bind mode
		if (keys_tap)
			M_FindDoubleKeysForCommand (keys_bindlist[keys_cursor][0], keys);
		else
			M_FindKeysForCommand (keys_bindlist[keys_cursor][0], keys);
		S_LocalSound ("raven/menu2.wav");
		if (keys[1] != -1)
		{
			if (keys_tap)
				M_UnbindDoubleCommand (keys_bindlist[keys_cursor][0]);
			else
				M_UnbindCommand (keys_bindlist[keys_cursor][0]);
		}
		Key_SetDest (key_menubind);
		break;

	case K_BACKSPACE:	// delete bindings
	case K_DEL:		// delete bindings
		S_LocalSound ("raven/menu2.wav");
		if (keys_tap)
			M_UnbindDoubleCommand (keys_bindlist[keys_cursor][0]);
		else
			M_UnbindCommand (keys_bindlist[keys_cursor][0]);
		break;

	default:
		/* Type-to-search: jump to next binding matching typed char */
		if (k >= 32 && k < 128 && !(Key_GetDest() & key_bindbit))
		{
			int j, start = keys_cursor + 1;
			char ch = (k >= 'A' && k <= 'Z') ? k + 32 : k;
			for (j = 0; j < keys_numcommands; j++)
			{
				int idx = (start + j) % keys_numcommands;
				const char *name = keys_bindlist[idx][1];
				int n;
				for (n = 0; name[n]; n++)
				{
					char c = name[n];
					if (c >= 'A' && c <= 'Z') c += 32;
					if (c == ch) { keys_cursor = idx; goto found; }
				}
			}
		found: ;
		}
		break;
	}

	if (keys_cursor < keys_top)
		keys_top = keys_cursor;
	else if (keys_cursor >= keys_top+KEYS_SIZE)
		keys_top = keys_cursor - KEYS_SIZE + 1;
}

//=============================================================================
/* VIDEO MENU */

static void M_Menu_Video_f (void)
{
	/* redirect to combined Display menu */
	M_Menu_Display_f ();
}


static void M_Video_Draw (void)
{
	(*vid_menudrawfn) ();
}


static void M_Video_Key (int key)
{
	(*vid_menukeyfn) (key);
}

//=============================================================================
/* MODS MENU */

#define	MODS_MAX		256
#define	MODS_LIST_TOP		60
#define	MODS_LIST_X		64
#define	MODS_CURSOR_X		56
#define	MODS_SCROLLBAR_X	252
#define	MODS_FOOTER_RESERVE	32	/* px reserved below the list: portals toggle + search line */

/* Status tags ("<-", "[HW]") are right-aligned to end here, which keeps them
 * clear of the scrollbar at MODS_SCROLLBAR_X.  Labels get everything to the
 * left of the widest tag, so the two can never collide. */
#define	MODS_TAG_RIGHT		248
#define	MODS_LABEL_MAX		19	/* characters, from MODS_LIST_X to 216 */

typedef enum
{
	MOD_BASE = 0,	/* data1 */
	MOD_PORTALS,	/* portals */
	MOD_CUSTOM
} modkind_t;

typedef struct
{
	char		dir[MAX_QPATH];		/* what `game' takes */
	char		label[MAX_QPATH];	/* display name; falls back to dir */
	modkind_t	kind;
	qboolean	installed;		/* false only for a missing portals row */
	qboolean	hexenworld;		/* hwprogs.dat gamecode: not playable here */
} modentry_t;

static modentry_t	mods_list[MODS_MAX];
static int	mods_count;

/* Indices into mods_list that pass the search filter.  The cursor and the
 * scroll offset index THIS, never mods_list.  That distinction is the whole
 * safety story for filtering: the old code compared mods_cursor against
 * MODS_FIXED_PORTALS and friends as raw list positions, and every one of those
 * comparisons goes wrong the moment a filter hides a row above it.  Entry kind
 * is carried per-entry instead, so nothing here depends on a row number.
 * Same bug shape as uhexen2-8uc1 (Graphics submenu drew the cursor at the
 * compacted row while hit-testing the raw enum index). */
static int	mods_view[MODS_MAX];
static int	mods_view_count;
static int	mods_cursor;		/* index into mods_view */
static int	mods_top;		/* first visible view row — scroll offset */

static qboolean	mods_portals_toggle;	/* "with Portals" for custom mods */
static qboolean	mods_have_portals;
static qboolean	mods_truncated;		/* scan ran out of MODS_MAX slots */

/* Display names for the hexenworld.org add-on corpus, transcribed from that
 * archive's readme -- see docs/MODS_CORPUS.md, which is the authority for this
 * table.  The corpus is ~40 mods whose directory names ("rvnlrd", "sprgnth",
 * "fo4d", "tt") tell a player nothing, which is the whole reason this exists.
 * A mod that is not listed here still works: the lookup falls back to the
 * directory name, and a modinfo.txt in the mod overrides both. */
typedef struct
{
	const char	*dir;
	const char	*title;
} modname_t;

static const modname_t mods_known[] =
{
	{ "acorn",	"Magic Acorn" },
	{ "ahumado",	"Ahumado's Skull" },
	{ "apocbot",	"Apocalypse Bots" },
	{ "bigspdrs",	"Spiders!" },
	{ "black",	"Black Plague" },
	{ "bodies",	"Bodies" },
	{ "db",		"DungeonBreak" },
	{ "eyeofra",	"The Eye of Ra" },
	{ "ffmus",	"Final Fantasy Music" },
	{ "fo4d",	"Fortress of Four Doors" },
	{ "h2ctf",	"HexDev Hexen II CTF" },
	{ "hcbots",	"CronosBot" },
	{ "hexarena",	"HexArena" },
	{ "hwctf",	"HexenWorld CTF" },
	{ "hwcycle",	"Map cycling for HW" },
	{ "leech",	"Weapon Leech" },
	{ "lightng",	"Lightning Sorcery" },
	{ "mpbyrino",	"Mission Pack by Rino" },
	{ "mpbyrino2",	"Mission Pack by Rino 2" },
	{ "ndm",	"DM pak for Hexen II" },
	{ "orbmeek",	"The Orb of the Meek" },
	{ "ovinoimp",	"Ovinomancer Imp" },
	{ "peanut",	"Project Peanut" },
	{ "q2sounds",	"Quake 2 Sound Pack" },
	{ "quake",	"Quake Sounds & Weapons" },
	{ "qweapons",	"Quake Weapons" },
	{ "raven",	"Ravenhurst" },
	{ "redmed",	"Red Medusa" },
	{ "rk",		"Rival Kingdoms" },
	{ "rvnlrd",	"RavenLord" },
	{ "siege",	"Siege" },
	{ "speeed",	"Speeeed's Imagination" },
	{ "spiders",	"Arachnophobia" },
	{ "spike",	"Deadly Sheep" },
	{ "sprgnth",	"Super Gauntlets" },
	{ "SuperNecro",	"SuperNecro" },
	{ "thebarongastonehousebyrino",	"The Baron Gastone House" },
	{ "tt",		"The Tyrant's Tome" },
	{ "xability",	"Extra Abilities" }
};

static const char *M_Mods_KnownTitle (const char *dir)
{
	size_t	i;

	for (i = 0; i < sizeof(mods_known) / sizeof(mods_known[0]); i++)
	{
		if (!q_strcasecmp(dir, mods_known[i].dir))
			return mods_known[i].title;
	}
	return NULL;
}

/*
Read a display name out of <basedir>/<dir>/modinfo.txt: the first line that is
not blank and not a # or ; comment.  Deliberately plain stdio -- at scan time
the mod is not on the searchpath, and mounting every candidate just to read a
title would be far more work than the menu is worth.  Returns false and leaves
`out' untouched when there is nothing usable to read.
*/
static qboolean M_Mods_ReadModinfo (const char *basedir, const char *dir,
				    char *out, size_t outsize)
{
	char	path[MAX_OSPATH];
	char	line[256];
	char	*p, *end;
	FILE	*f;

	q_snprintf (path, sizeof(path), "%s/%s/modinfo.txt", basedir, dir);
	f = fopen (path, "rt");
	if (!f)
		return false;

	while (fgets(line, sizeof(line), f))
	{
		p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == 0 || *p == '\n' || *p == '\r' || *p == '#' || *p == ';')
			continue;

		end = p + strlen(p);
		while (end > p && (end[-1] == '\n' || end[-1] == '\r' ||
				   end[-1] == ' '  || end[-1] == '\t'))
			*--end = 0;
		if (end == p)
			continue;

		q_strlcpy (out, p, outsize);
		fclose (f);
		return true;
	}

	fclose (f);
	return false;
}

/*
A mod carrying hwprogs.dat and no progs.dat is HexenWorld gamecode.  It still
satisfies FS_IsGamedir (it has paks, or a pk3), so it lists here -- but the
Hexen II client cannot run it, and offering it would just hand the player a
broken switch.  The corpus has at least three (hwctf, hwcycle, siege).

Loose files only: catching an hwprogs.dat packed inside a .pak would mean
opening every candidate archive during a menu scan.  A mod that hides its
gamecode that way is listed as playable and fails at launch, which is exactly
what happens today.
*/
static qboolean M_Mods_IsHexenWorld (const char *basedir, const char *dir)
{
	char	path[MAX_OSPATH];

	q_snprintf (path, sizeof(path), "%s/%s/progs.dat", basedir, dir);
	if (Sys_FileType(path) == FS_ENT_FILE)
		return false;

	q_snprintf (path, sizeof(path), "%s/%s/hwprogs.dat", basedir, dir);
	return (Sys_FileType(path) == FS_ENT_FILE);
}

/* Visible rows fit between MODS_LIST_TOP and (200 - MODS_FOOTER_RESERVE) on the
 * 320x200 menu canvas. When the separator is drawn (between fixed and custom
 * entries) it eats one extra row, so reserve one slot. */
static int M_Mods_VisibleRows (void)
{
	int avail = 200 - MODS_LIST_TOP - MODS_FOOTER_RESERVE;
	int rows = avail / 8 - 1;	/* minus 1 for potential separator */
	if (rows < 4) rows = 4;
	return rows;
}

static int mods_sortcmp (const void *a, const void *b)
{
	const modentry_t *ea = (const modentry_t *)a;
	const modentry_t *eb = (const modentry_t *)b;

	return q_strcasecmp (ea->dir, eb->dir);
}

/* Fixed entries at top of mods list */
#define MODS_FIXED_COUNT	2	/* base game + portals */

static void M_Mods_SetEntry (modentry_t *e, const char *basedir,
			     const char *dir, modkind_t kind,
			     const char *forced_label)
{
	const char	*title;
	char		info[MAX_QPATH];

	memset (e, 0, sizeof(*e));
	q_strlcpy (e->dir, dir, sizeof(e->dir));
	e->kind = kind;
	e->installed = true;

	if (forced_label)
	{
		q_strlcpy (e->label, forced_label, sizeof(e->label));
		return;
	}

	title = M_Mods_KnownTitle (dir);
	q_strlcpy (e->label, title ? title : dir, sizeof(e->label));

	/* a mod that describes itself outranks our shipped table */
	if (M_Mods_ReadModinfo (basedir, dir, info, sizeof(info)))
		q_strlcpy (e->label, info, sizeof(e->label));
}

/*
Scan one filesystem root for mod directories and merge what it finds into
mods_list.  Called once for the basedir and again for the userdir, because
FS_AddGameDirectory mounts <basedir>/<dir> AND <userdir>/<dir> for every
gamedir it is handed -- so a mod dropped into ~/.hexen2/<mod> has always been
fully playable and was simply never listed anywhere the player could find it.
uhexen2-3m0h.

Fails loudly on truncation: a silently shortened list reads as "that mod is
not installed", which sends the player looking in the wrong place.
*/
static void M_Mods_ScanRoot (const char *root)
{
	static char	alldirs[MODS_MAX][MAX_QPATH];
	char	path[MAX_OSPATH];
	char	info[MAX_QPATH];
	int	numdirs, i, j;

	numdirs = Sys_ListDirectories (root, alldirs, MODS_MAX);
	if (numdirs >= MODS_MAX)
	{
		mods_truncated = true;
		Con_Printf ("Mods: more than %d game directories in %s, list truncated\n",
			    MODS_MAX, root);
	}

	for (i = 0; i < numdirs; i++)
	{
		if (!q_strcasecmp(alldirs[i], "data1"))
			continue;
		if (!q_strcasecmp(alldirs[i], "portals"))
			continue;
		if (!q_strcasecmp(alldirs[i], "hw"))
			continue;
		if (!FS_IsGamedir(root, alldirs[i]))
			continue;

		for (j = MODS_FIXED_COUNT; j < mods_count; j++)
		{
			if (!q_strcasecmp(mods_list[j].dir, alldirs[i]))
				break;
		}
		if (j < mods_count)
		{
			/* Installed under both roots: one mod, one row.  Both copies
			 * mount, and the userdir's is pushed onto the searchpath after
			 * the basedir's, so the userdir is the copy whose content the
			 * player actually gets -- its modinfo.txt wins the label.  A
			 * loose progs.dat in EITHER root likewise makes the mod
			 * playable by this client, so it clears an [HW] tag the other
			 * root earned; the reverse does not hold, which is why this
			 * probes progs.dat rather than re-running
			 * M_Mods_IsHexenWorld (that would also answer false for a
			 * root holding neither progs.dat nor hwprogs.dat). */
			if (M_Mods_ReadModinfo (root, alldirs[i], info, sizeof(info)))
				q_strlcpy (mods_list[j].label, info, sizeof(mods_list[j].label));
			q_snprintf (path, sizeof(path), "%s/%s/progs.dat", root, alldirs[i]);
			if (Sys_FileType(path) == FS_ENT_FILE)
				mods_list[j].hexenworld = false;
			continue;
		}

		if (mods_count >= MODS_MAX)
		{
			mods_truncated = true;
			Con_Printf ("Mods: more than %d mods installed, list truncated\n",
				    MODS_MAX);
			break;
		}

		M_Mods_SetEntry (&mods_list[mods_count], root, alldirs[i], MOD_CUSTOM, NULL);
		mods_list[mods_count].hexenworld = M_Mods_IsHexenWorld (root, alldirs[i]);
		mods_count++;
	}
}

static void M_ScanMods (void)
{
	char	path[MAX_OSPATH];
	const char	*basedir, *userbase;

	/* FS_GetBasedir(), not host_parms->basedir: -basedir moves fs_basedir and
	 * never touches host_parms->basedir, so scanning the latter would list a
	 * different set of mods than the ones Host_Game_f (quakefs.c) and the
	 * `game' tab completer will actually accept.  uhexen2-jk53. */
	basedir = FS_GetBasedir ();
	userbase = FS_GetUserbase ();
	mods_truncated = false;

	/* check if portals directory exists */
	q_snprintf (path, sizeof(path), "%s/portals", basedir);
	mods_have_portals = (Sys_FileType(path) == FS_ENT_DIRECTORY);

	/* fixed entries — labelled by hand, they are not "mods" the player installed */
	mods_count = 0;
	M_Mods_SetEntry (&mods_list[mods_count], basedir, "data1", MOD_BASE, "Hexen II");
	mods_count++;
	M_Mods_SetEntry (&mods_list[mods_count], basedir, "portals", MOD_PORTALS, "Portal of Praevus");
	mods_list[mods_count].installed = mods_have_portals;
	mods_count++;

	/* Scan for custom mods.  Basedir first, because the merge in
	 * M_Mods_ScanRoot lets the second root override the first's label and that
	 * is the precedence the searchpath itself uses.  The userdir pass is
	 * skipped where the two roots are the same directory, which is every
	 * !DO_USERDIRS platform (Windows, OS/2, Emscripten): there FS_Init points
	 * the userdir at the basedir, and -basedir moves both together. */
	M_Mods_ScanRoot (basedir);
	if (strcmp(basedir, userbase))
		M_Mods_ScanRoot (userbase);

	/* sort only the custom mods (after the fixed entries) */
	if (mods_count - MODS_FIXED_COUNT > 1)
		qsort (&mods_list[MODS_FIXED_COUNT], mods_count - MODS_FIXED_COUNT,
		       sizeof(mods_list[0]), mods_sortcmp);
}

static qboolean M_Mods_IsActive (const modentry_t *e)
{
	return !q_strcasecmp(fs_gamedir_nopath, e->dir);
}

/* Rebuild the filtered view.  Matches the search buffer against both the
 * display name and the directory name, because a player may know a mod as
 * "sprgnth" or as "Super Gauntlets" and both should find it. */
static void M_Mods_BuildView (void)
{
	int	i;

	mods_view_count = 0;
	for (i = 0; i < mods_count; i++)
	{
		if (M_Filter_Active() &&
		    !M_Filter_Matches(mods_list[i].label) &&
		    !M_Filter_Matches(mods_list[i].dir))
			continue;
		mods_view[mods_view_count++] = i;
	}

	if (mods_cursor >= mods_view_count)
		mods_cursor = mods_view_count - 1;
	if (mods_cursor < 0)
		mods_cursor = 0;
}

/* Can this view row be activated?  A missing Portals row and HexenWorld-only
 * mods are drawn (hiding them reads as a bug) but cannot be chosen. */
static qboolean M_Mods_Selectable (int view_idx)
{
	const modentry_t	*e;

	if (view_idx < 0 || view_idx >= mods_view_count)
		return false;
	e = &mods_list[mods_view[view_idx]];
	return e->installed && !e->hexenworld;
}

/* Step the cursor by one row in `dir', skipping what cannot be activated.
 * Bounded by the view size: a filter that matches only unselectable rows must
 * terminate, not spin. */
static void M_Mods_MoveCursor (int dir)
{
	int	i;

	if (mods_view_count <= 0)
		return;

	for (i = 0; i < mods_view_count; i++)
	{
		mods_cursor += dir;
		if (mods_cursor >= mods_view_count)
			mods_cursor = 0;
		else if (mods_cursor < 0)
			mods_cursor = mods_view_count - 1;
		if (M_Mods_Selectable(mods_cursor))
			return;
	}
	/* nothing selectable anywhere: the walk returns the cursor where it began */
}

/* After a jump (paging, Home/End, a filter change) pull the cursor onto the
 * nearest selectable row, trying `prefer' first and then the other way. */
static void M_Mods_SnapSelectable (int prefer)
{
	int	i;

	if (mods_view_count <= 0)
		return;
	if (mods_cursor < 0)
		mods_cursor = 0;
	if (mods_cursor >= mods_view_count)
		mods_cursor = mods_view_count - 1;
	if (M_Mods_Selectable(mods_cursor))
		return;

	for (i = mods_cursor; i >= 0 && i < mods_view_count; i += prefer)
	{
		if (M_Mods_Selectable(i))
		{
			mods_cursor = i;
			return;
		}
	}
	for (i = mods_cursor; i >= 0 && i < mods_view_count; i -= prefer)
	{
		if (M_Mods_Selectable(i))
		{
			mods_cursor = i;
			return;
		}
	}
}

/* Keep mods_cursor within the visible window by adjusting mods_top. */
static void M_Mods_EnsureVisible (void)
{
	int visible = M_Mods_VisibleRows ();
	if (mods_view_count <= visible)
	{
		mods_top = 0;
		return;
	}
	if (mods_cursor < mods_top)
		mods_top = mods_cursor;
	else if (mods_cursor >= mods_top + visible)
		mods_top = mods_cursor - visible + 1;
	if (mods_top < 0)
		mods_top = 0;
	if (mods_top > mods_view_count - visible)
		mods_top = mods_view_count - visible;
}

/* Re-seat the cursor and scroll after the filter changed. */
static void M_Mods_FilterChanged (void)
{
	M_Mods_BuildView ();
	mods_cursor = 0;
	mods_top = 0;
	M_Mods_SnapSelectable (1);
	M_Mods_EnsureVisible ();
}

/* Fit a label into the label column, ellipsising what does not go.  The corpus
 * carries a 27-character directory name (thebarongastonehousebyrino), so this
 * is a real case and not defensive padding. */
static const char *M_Mods_FitLabel (const char *label)
{
	static char	buf[MODS_LABEL_MAX + 1];

	if (strlen(label) <= MODS_LABEL_MAX)
		return label;

	q_strlcpy (buf, label, MODS_LABEL_MAX - 3 + 1);
	q_strlcat (buf, "...", sizeof(buf));
	return buf;
}

/* Right-align a status tag so it ends at MODS_TAG_RIGHT, clear of both the
 * label column and the scrollbar. */
static void M_Mods_DrawTag (int y, const char *tag, qboolean white)
{
	int	x = MODS_TAG_RIGHT - (int)strlen(tag) * 8;

	if (white)
		M_PrintWhite (x, y, tag);
	else
		M_Print (x, y, tag);
}

static void M_Menu_Mods_f (void)
{
	int	i;

	Key_SetDest (key_menu);
	m_state = m_mods;
	m_entersound = true;

	/* the search buffer is shared with the other filtered submenus */
	M_Filter_Clear ();

	M_ScanMods ();
	M_Mods_BuildView ();

	/* start on the running mod */
	mods_cursor = 0;
	for (i = 0; i < mods_view_count; i++)
	{
		if (M_Mods_IsActive(&mods_list[mods_view[i]]))
		{
			mods_cursor = i;
			break;
		}
	}
	mods_top = 0;
	M_Mods_SnapSelectable (1);
	M_Mods_EnsureVisible ();

	mods_portals_toggle = mods_have_portals;
}

/* Does a separator follow view row `i'?  One rule, read by both the layout
 * walk and the draw loop, because a list whose rows are drawn somewhere other
 * than where they are hit-tested is precisely the uhexen2-8uc1 failure. */
static qboolean M_Mods_SeparatorAfter (int i, int last_visible)
{
	return (mods_list[mods_view[i]].kind != MOD_CUSTOM &&
		i + 1 < last_visible &&
		mods_list[mods_view[i + 1]].kind == MOD_CUSTOM);
}

/* Canvas Y of every row in the visible window, in view order from mods_top.
 * Returns how many were laid out.  The separator costs a row's worth of space
 * (4 above the dashes and 4 below), which is why a plain MODS_LIST_TOP + n*8
 * hit test would drift by 8 pixels for every row below the divide. */
static int M_Mods_LayoutRows (int last_visible, int *ys)
{
	int	i, y = MODS_LIST_TOP, n = 0;

	for (i = mods_top; i < last_visible; i++)
	{
		ys[n++] = y;
		y += 8;
		if (M_Mods_SeparatorAfter (i, last_visible))
			y += 8;
	}
	return n;
}

/* Mouse hover: move the cursor to the row under the pointer.
 *
 * Gated on menu_mouse_moved for the same reason M_MouseToMenuItem is -- a
 * stationary pointer that re-pins the cursor every frame makes the arrow keys
 * look dead (uhexen2-u4iz).  Unselectable rows are ignored rather than
 * hovered, matching what M_Mods_MoveCursor does with the arrows: an [HW] mod
 * or an uninstalled Portals row is drawn, but is not a place the cursor rests.
 *
 * Wheel and click need nothing here: M_Keydown already maps K_MWHEELUP/DOWN to
 * the arrows and K_MOUSE1 to Enter for every menu, so pointing at a row and
 * clicking activates that row.  Dragging the scrollbar thumb is NOT
 * implemented -- the menu is handed a position and a moved flag and no button
 * state at all (in_sdl.c:64), so a drag cannot be told from a hover. */
static void M_Mods_MouseHover (int n, const int *ys)
{
	int	vy, k;

	if (!menu_mouse_moved)
		return;

	vy = M_ScreenYToCanvasY (menu_mouse_y);
	for (k = 0; k < n; k++)
	{
		if (vy >= ys[k] && vy < ys[k] + 8)
		{
			if (M_Mods_Selectable (mods_top + k))
			{
				mods_cursor = mods_top + k;
				M_HoverSound (mods_cursor);
			}
			else
				M_HoverSound (-1);	/* drawn but not a stop */
			return;
		}
	}
	M_HoverSound (-1);
}

static void M_Mods_Draw (void)
{
	int		i, k, y, cursor_y, yf;
	int		visible, last_visible, nrows, list_bottom;
	int		row_y[MODS_MAX];
	const modentry_t	*e;

	ScrollTitle("gfx/menu/title0.lmp");

	visible = M_Mods_VisibleRows ();
	M_Mods_EnsureVisible ();

	last_visible = mods_top + visible;
	if (last_visible > mods_view_count)
		last_visible = mods_view_count;

	/* Lay the window out, then let the pointer pick a row, then draw -- so the
	 * highlight and the blinking cursor agree with the hover within one frame
	 * rather than trailing it by one. */
	nrows = M_Mods_LayoutRows (last_visible, row_y);
	M_Mods_MouseHover (nrows, row_y);

	cursor_y = -1;
	for (k = 0; k < nrows; k++)
	{
		i = mods_top + k;
		y = row_y[k];
		e = &mods_list[mods_view[i]];

		if (i == mods_cursor)
		{
			cursor_y = y;
			M_PrintWhite (MODS_LIST_X, y, M_Mods_FitLabel(e->label));
		}
		else
			M_Print (MODS_LIST_X, y, M_Mods_FitLabel(e->label));

		if (!e->installed)
			M_Mods_DrawTag (y, "(none)", false);
		else if (e->hexenworld)
			M_Mods_DrawTag (y, "[HW]", false);
		else if (M_Mods_IsActive(e))
			M_Mods_DrawTag (y, "<-", true);

		/* separator between the fixed rows and the scanned ones, drawn only
		 * when both sides of the divide are on screen.  Keyed off entry kind
		 * rather than a row number, because the filter renumbers rows. */
		if (M_Mods_SeparatorAfter (i, last_visible))
		{
			M_DrawCharacter (MODS_LIST_X, y + 12, '-' + 128);
			M_DrawCharacter (MODS_LIST_X + 8, y + 12, '-' + 128);
			M_DrawCharacter (MODS_LIST_X + 16, y + 12, '-' + 128);
		}
	}

	if (mods_view_count == 0)
		M_Print (MODS_LIST_X, MODS_LIST_TOP, "No mods match");

	/* Bottom of the drawn list, from the layout rather than from
	 * MODS_LIST_TOP + visible * 8.  Those two are not the same number: the
	 * separator adds a row's worth of height, so the naive formula leaves the
	 * scrollbar, the down arrow and the footer 8 pixels short whenever the
	 * divide is on screen -- visible as a scrollbar that stops one row above
	 * the last entry.  Third place that geometry has to come from one source;
	 * see M_Mods_LayoutRows. */
	list_bottom = nrows ? row_y[nrows - 1] + 8 : MODS_LIST_TOP;

	/* up/down scroll indicators */
	if (mods_top > 0)
		M_DrawCharacter (MODS_LIST_X - 16, MODS_LIST_TOP, 128);
	if (last_visible < mods_view_count && nrows)
		M_DrawCharacter (MODS_LIST_X - 16, row_y[nrows - 1], 129);

	/* proportional scrollbar on right edge */
	if (mods_view_count > visible)
	{
		int track_y = MODS_LIST_TOP;
		int track_h = list_bottom - MODS_LIST_TOP;
		int thumb_h = (visible * track_h) / mods_view_count;
		int thumb_y = track_y + (mods_top * track_h) / mods_view_count;
		int j;
		if (thumb_h < 8) thumb_h = 8;
		for (j = 0; j < track_h; j += 8)
		{
			int cy = track_y + j;
			if (cy >= thumb_y && cy < thumb_y + thumb_h)
				M_DrawCharacter (MODS_SCROLLBAR_X, cy, 11);
			else
				M_DrawCharacter (MODS_SCROLLBAR_X, cy, '-');
		}
	}

	yf = list_bottom + 8;

	/* portals toggle — only for custom mods, and only when portals is installed */
	if (mods_have_portals && mods_view_count > 0 &&
	    mods_list[mods_view[mods_cursor]].kind == MOD_CUSTOM)
	{
		M_Print (MODS_LIST_X, yf, "Portals data:");
		if (mods_portals_toggle)
			M_PrintWhite (176, yf, "ON");
		else
			M_Print (176, yf, "off");
	}

	M_Filter_Draw (MODS_LIST_X, yf + 10);

	/* blinking cursor on the active row */
	if (cursor_y >= 0)
		M_DrawCharacter (MODS_CURSOR_X, cursor_y, 12 + ((int)(realtime * 4) & 1));
}

static void M_Mods_Key (int key)
{
	int	visible = M_Mods_VisibleRows ();
	const modentry_t	*e;

	/* Search input first.  M_Filter_HandleKey only claims printable ASCII and
	 * backspace, so escape (27), enter (13), the arrows (128+) and the gamepad
	 * codes (243+) all fall through to navigation below. */
	if (M_Filter_HandleKey (key))
	{
		M_Mods_FilterChanged ();
		return;
	}

	switch (key)
	{
	case K_ESCAPE:
	case K_GP_B:
		/* first press drops the filter, second leaves the menu */
		if (M_Filter_Active ())
		{
			S_LocalSound ("raven/menu2.wav");
			M_Filter_Clear ();
			M_Mods_FilterChanged ();
			break;
		}
		M_Menu_Main_f ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		M_Mods_MoveCursor (1);
		M_Mods_EnsureVisible ();
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		M_Mods_MoveCursor (-1);
		M_Mods_EnsureVisible ();
		break;

	case K_PGUP:
		S_LocalSound ("raven/menu1.wav");
		mods_cursor -= visible;
		M_Mods_SnapSelectable (-1);
		M_Mods_EnsureVisible ();
		break;

	case K_PGDN:
		S_LocalSound ("raven/menu1.wav");
		mods_cursor += visible;
		M_Mods_SnapSelectable (1);
		M_Mods_EnsureVisible ();
		break;

	case K_HOME:
		S_LocalSound ("raven/menu1.wav");
		mods_cursor = 0;
		M_Mods_SnapSelectable (1);
		M_Mods_EnsureVisible ();
		break;

	case K_END:
		S_LocalSound ("raven/menu1.wav");
		mods_cursor = mods_view_count - 1;
		M_Mods_SnapSelectable (-1);
		M_Mods_EnsureVisible ();
		break;

	case K_LEFTARROW:
	case K_RIGHTARROW:
		/* toggle portals for custom mods only */
		if (mods_have_portals && mods_view_count > 0 &&
		    mods_list[mods_view[mods_cursor]].kind == MOD_CUSTOM)
		{
			S_LocalSound ("raven/menu1.wav");
			mods_portals_toggle = !mods_portals_toggle;
		}
		break;

	case K_ENTER:
	case K_GP_A:
		if (!M_Mods_Selectable(mods_cursor))
			break;
		e = &mods_list[mods_view[mods_cursor]];
		if (M_Mods_IsActive(e))
			break;	/* already running */
		m_entersound = true;
		Key_SetDest (key_game);
		m_state = m_none;
		M_Filter_Clear ();
		Cbuf_AddText (va("game \"%s\" %d\n", e->dir,
				mods_portals_toggle ? 1 : 0));
		break;
	}
}


//=============================================================================
/* HELP MENU */

static int		help_page;

#define	NUM_HELP_PAGES		5

static void M_Menu_Help_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_help;
	m_entersound = true;
	help_page = 0;
}


#if FULLSCREEN_INTERMISSIONS
#	if defined(GLQUAKE) || defined(WEBQUAKE)
#		define	Load_HelpPic_FN(X,Y,Z)	Draw_CachePicNoTrans((X))
#		define	Draw_HelpPic_FN(X,Y,Z)	Draw_IntermissionPic((Z))
#	else
#		define	Load_HelpPic_FN(X,Y,Z)	Draw_CachePicResize((X),(Y),(Z))
#		define	Draw_HelpPic_FN(X,Y,Z)	Draw_Pic(0,0,(Z))
#	endif
#else
#	ifdef GLQUAKE
#		define	Load_HelpPic_FN(X,Y,Z)	Draw_CachePic((X))
#		define	Draw_HelpPic_FN(X,Y,Z)	Draw_Pic((X),(Y),(Z))
#	else
#		define	Load_HelpPic_FN(X,Y,Z)	Draw_CachePic((X))
#		define	Draw_HelpPic_FN(X,Y,Z)	Draw_Pic((X),(Y),(Z))
#	endif
#endif

static void M_Help_Draw (void)
{
		Draw_HelpPic_FN (M_CenterOfs(), 0, Load_HelpPic_FN(va("gfx/menu/help%02i.lmp", help_page+1), vid.width, vid.height));
}


static void M_Help_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;

	case K_UPARROW:
	case K_RIGHTARROW:
		m_entersound = true;
		if (++help_page >= NUM_HELP_PAGES)
			help_page = 0;
		break;

	case K_DOWNARROW:
	case K_LEFTARROW:
		m_entersound = true;
		if (--help_page < 0)
			help_page = NUM_HELP_PAGES-1;
		break;
	}
}

//=============================================================================
/* QUIT MENU */

//static int		msgNumber;
static enum m_state_e	m_quit_prevstate;
static qboolean		wasInMenus;

#if 0
static const char *quitMessage [] = 
{
/* .........1.........2.... */
  "   Look! Behind you!    ",
  "  There's a big nasty   ",
  "   thing - shoot it!    ",
  "                        ",

  "  You can't go now, I   ",
  "   was just getting     ",
  "    warmed up.          ",
  "                        ",

  "    One more game.      ",
  "      C'mon...          ",
  "   Who's gonna know?    ",
  "                        ",

  "   What's the matter?   ",
  "   Palms too sweaty to  ",
  "     keep playing?      ",
  "                        ",

  "  Watch your local store",
  "      for Hexen 2       ",
  "    plush toys and      ",
  "    greeting cards!     ",

  "  Hexen 2...            ",
  "                        ",
  "    Too much is never   ",
  "        enough.         ",

  "  Sure go ahead and     ",
  "  leave.  But I know    ",
  "  you'll be back.       ",
  "                        ",

  "                        ",
  "  Insert cute phrase    ",
  "        here            ",
  "                        "
};
#endif

static float LinePos;
static int LineTimes;
static int MaxLines;
static const char **LineText;
static qboolean LineTxt2;
static qboolean SoundPlayed;


#define	MAX_LINES	145

static const char *CreditText[MAX_LINES] =
{
   "Project Director: Brian Raffel",
   "",
   "Lead Programmer: Rick Johnson",
   "",
   "Programming:",
   "   Ben Gokey",
   "   Bob Love",
   "   Mike Gummelt",
   "",
   "Additional Programming:",
   "   Josh Weier",
   "",
   "Lead Design: Eric Biessman",
   "",
   "Design:",
   "   Brian Raffel",
   "   Brian Frank",
   "   Tom Odell",
   "",
   "Art Director: Brian Pelletier",
   "",
   "Art:",
   "   Shane Gurno",
   "   Jim Sumwalt",
   "   Mark Morgan",
   "   Kim Lathrop",
   "   Ted Halsted",
   "   Rebecca Rettenmund",
   "   Les Dorscheid",
   "",
   "Animation:",
   "   Chaos (Mike Werckle)",
   "   Brian Shubat",
   "",
   "Cinematics:",
   "   Jeff Dewitt",
   "   Jeffrey P. Lampo",
   "",
   "Music:",
   "   Kevin Schilder",
   "",
   "Sound:",
   "   Kevin Schilder",
   "   Chia Chin Lee",
   "",
   "",
   "Activision",
   "",
   "Producer:",
   "   Steve Stringer",
   "",
   "Localization Producer:",
   "   Sandi Isaacs",
   "",
   "Marketing Product Manager:",
   "   Henk Hartong",
   "",
   "European Marketing",
   "Product Director:",
   "   Janine Johnson",
   "",
   "Marketing Associate:",
   "   Kevin Kraff",
   "",
   "Senior Quality",
   "Assurance Lead:",
   "   Tim Vanlaw",
   "",
   "Quality Assurance Lead:",
   "   John Tam",
   "",
   "Quality Assurance Team:",
   "   Steve Rosenthal, Mike Spann,",
   "   Steve Elwell, Kelly Wand,",
   "   Kip Stolberg, Igor Krinitskiy,",
   "   Ian Stevens, Marilena Wahmann,",
   "   David Baker, Winnie Lee",
   "",
   "Documentation:",
   "   Mike Rivera, Sylvia Orzel,",
   "   Belinda Vansickle",
   "",
   "Chronicle of Deeds written by:",
   "   Joe Grant Bell",
   "",
   "Localization:",
   "   Nathalie Dove, Lucy Morgan,",
   "   Alex Wylde, Nicky Kerth",
   "",
   "Installer by:",
   "   Steve Stringer, Adam Goldberg,",
   "   Tanya Martino, Eric Schmidt,",
   "   Ronnie Lane",
   "",
   "Art Assistance by:",
   "   Carey Chico and Franz Boehm",
   "",
   "BizDev Babe:",
   "   Jamie Bafus",
   "",
   "And...",
   "",
   "Deal Guru:",
   "   Mitch Lasky",
   "",
   "",
   "Thanks to Id software:",
   "   John Carmack",
   "   Adrian Carmack",
   "   Kevin Cloud",
   "   Barrett 'Bear'  Alexander",
   "   American McGee",
   "",
   "",
   "Published by Id Software, Inc.",
   "Distributed by Activision, Inc.",
   "",
   "The Id Software Technology used",
   "under license in Hexen II (tm)",
   "(c) 1996, 1997 Id Software, Inc.",
   "All Rights Reserved.",
   "",
   "Hexen(r) is a registered trademark",
   "of Raven Software Corp.",
   "Hexen II (tm) and the Raven logo",
   "are trademarks of Raven Software",
   "Corp.  The Id Software name and",
   "id logo are trademarks of",
   "Id Software, Inc.  Activision(r)",
   "is a registered trademark of",
   "Activision, Inc. All other",
   "trademarks are the property of",
   "their respective owners.",
   "",
   "",
   "",
   "Send bug descriptions to:",
   "   h2bugs@mail.ravensoft.com",
   "",
   "Special thanks to Gary McTaggart",
   "at 3dfx for his help with",
   "the gl version!",
   "",
   "No snails were harmed in the",
#ifdef DEMOBUILD
   "making of this demo!"
#else
   "making of this game!"
#endif
};

#define	MAX_LINES2	158

static const char *Credit2Text[MAX_LINES2] =
{
   "Map Master: ",
   "   'Caffeine Buzz' Raffel",
   "",
   "Code Warrior:",
   "   Rick 'Superfly' Johnson",
   "",
   "Grunt Boys:",
   "   'Judah' Ben Gokey",
   "   Bob 'Back In Action' Love",
   "   Mike 'Jethro' Gummelt",
   "",
   "Additional Grunting:",
   "   Josh 'Intern' Weier",
   "",
   "Whippin' Boy:",
   "   Eric 'Baby' Biessman",
   "",
   "Crazy Levelers:",
   "   'Big Daddy' Brian Raffel",
   "   Brian 'Red' Frank",
   "   Tom 'Texture Alignment' Odell",
   "",
   "Art Lord:",
   "   Brian 'Mr. Calm' Pelletier",
   "",
   "Pixel Pushers:",
   "   Shane 'Duh' Gurno",
   "   'Slim' Jim Sumwalt",
   "   Mark 'Dad Gummit' Morgan",
   "   Kim 'Toy Master' Lathrop",
   "   'Drop Dead' Ted Halsted",
   "   Rebecca 'Zombie' Rettenmund",
   "   Les 'Be Friends' Dorscheid",
   "",
   "Salad Shooters:",
   "   Mike 'Chaos' Werckle",
   "   Brian 'Mutton Chops' Shubat",
   "",
   "Last Minute Addition:",
   "   Jeff 'Spanky' Dewitt",
   "   Jeffrey 'Misspalld' Lampo",
   "",
   "Random Notes:",
   "   Kevin 'I Already Paid' Schilder",
   "",
   "Grunts, Groans, and Moans:",
   "   Kevin 'I Already Paid' Schilder",
   "   Chia 'Pet' Chin Lee",
   "",
   "",
   "Activision",
   "",
   "Producer:",
   "   Steve 'Ferris' Stringer",
   "",
   "Localization Producer:",
   "   Sandi 'Boneduster' Isaacs",
   "",
   "Marketing Product Manager:",
   "   Henk 'A-10' Hartong",
   "",
   "European Marketing",
   "Product Director:",
   "   Janine Johnson",
   "",
   "Marketing Associate:",
   "   Kevin 'Savage' Kraff",
   "",
   "Senior Quality",
   "Assurance Lead:",
   "   Tim 'Outlaw' Vanlaw",
   "",
   "Quality Assurance Lead:",
   "   John 'Armadillo' Tam",
   "",
   "Quality Assurance Team:",
   "   Steve 'Rhinochoadicus'",
   "      Rosenthal,",
   "   Mike 'Dragonhawk' Spann,",
   "   Steve 'Zendog' Elwell,",
   "   Kelly 'Li'l Bastard' Wand,",
   "   Kip 'Angus' Stolberg,",
   "   Igor 'Russo' Krinitskiy,",
   "   Ian 'Cracker' Stevens,",
   "   Marilena 'Raveness-X' Wahmann,",
   "   David 'Spicegirl' Baker,",
   "   Winnie 'Mew' Lee",
   "",
   "Documentation:",
   "   Mike Rivera, Sylvia Orzel,",
   "   Belinda Vansickle",
   "",
   "Chronicle of Deeds written by:",
   "   Joe Grant Bell",
   "",
   "Localization:",
   "   Nathalie Dove, Lucy Morgan,",
   "   Alex Wylde, Nicky Kerth",
   "",
   "Installer by:",
   "   Steve 'Bahh' Stringer,",
   "   Adam Goldberg, Tanya Martino,",
   "   Eric Schmidt, Ronnie Lane",
   "",
   "Art Assistance by:",
   "   Carey 'Damien' Chico and",
   "   Franz Boehm",
   "",
   "BizDev Babe:",
   "   Jamie Bafus",
   "",
   "And...",
   "",
   "Deal Guru:",
   "   Mitch Lasky",
   "",
   "",
   "Thanks to Id software:",
   "   John Carmack",
   "   Adrian Carmack",
   "   Kevin Cloud",
   "   Barrett 'Bear'  Alexander",
   "   American McGee",
   "",
   "",
   "Published by Id Software, Inc.",
   "Distributed by Activision, Inc.",
   "",
   "The Id Software Technology used",
   "under license in Hexen II (tm)",
   "(c) 1996, 1997 Id Software, Inc.",
   "All Rights Reserved.",
   "",
   "Hexen(r) is a registered trademark",
   "of Raven Software Corp.",
   "Hexen II (tm) and the Raven logo",
   "are trademarks of Raven Software",
   "Corp.  The Id Software name and",
   "id logo are trademarks of",
   "Id Software, Inc.  Activision(r)",
   "is a registered trademark of",
   "Activision, Inc. All other",
   "trademarks are the property of",
   "their respective owners.",
   "",
   "",
   "",
   "Send bug descriptions to:",
   "   h2bugs@mail.ravensoft.com",
   "",
   "Special thanks to Bob for",
   "remembering 'P' is for Polymorph",
   "",
   "",
   "See the next movie in the long",
   "awaited sequel, starring",
   "Bobby Love in,",
   "   Out of Traction, Back in Action!"
};

#define	MAX_LINES_MP	138

static const char *CreditTextMP[MAX_LINES_MP] =
{
   "Project Director: James Monroe",
   "Creative Director: Brian Raffel",
   "Project Coordinator: Kevin Schilder",
   "",
   "Lead Programmer: James Monroe",
   "",
   "Programming:",
   "   Mike Gummelt",
   "   Josh Weier",
   "",
   "Additional Programming:",
   "   Josh Heitzman",
   "   Nathan Albury",
   "   Rick Johnson",
   "",
   "Assembly Consultant:",
   "   Mr. John Scott",
   "",
   "Lead Design: Jon Zuk",
   "",
   "Design:",
   "   Tom Odell",
   "   Jeremy Statz",
   "   Mike Renner",
   "   Eric Biessman",
   "   Kenn Hoekstra",
   "   Matt Pinkston",
   "   Bobby Duncanson",
   "   Brian Raffel",
   "",
   "Art Director: Les Dorscheid",
   "",
   "Art:",
   "   Kim Lathrop",
   "   Gina Garren",
   "   Joe Koberstein",
   "   Kevin Long",
   "   Jeff Butler",
   "   Scott Rice",
   "   John Payne",
   "   Steve Raffel",
   "",
   "Animation:",
   "   Eric Turman",
   "   Chaos (Mike Werckle)",
   "",
   "Music:",
   "   Kevin Schilder",
   "",
   "Sound:",
   "   Chia Chin Lee",
   "",
   "Activision",
   "",
   "Producer:",
   "   Steve Stringer",
   "",
   "Marketing Product Manager:",
   "   Henk Hartong",
   "",
   "Marketing Associate:",
   "   Kevin Kraff",
   "",
   "Senior Quality",
   "Assurance Lead:",
   "   Tim Vanlaw",
   "",
   "Quality Assurance Lead:",
   "   Doug Jacobs",
   "",
   "Quality Assurance Team:",
   "   Steve Rosenthal, Steve Elwell,",
   "   Chad Bordwell, David Baker,",
   "   Aaron Casillas, Damien Fischer,",
   "   Winnie Lee, Igor Krinitskiy,",
   "   Samantha Lee, John Park",
   "   Ian Stevens, Chris Toft",
   "",
   "Production Testers:",
   "   Steve Rosenthal and",
   "   Chad Bordwell",
   "",
   "Additional QA and Support:",
   "    Tony Villalobos",
   "    Jason Sullivan",
   "",
   "Installer by:",
   "   Steve Stringer, Adam Goldberg,",
   "   Tanya Martino, Eric Schmidt,",
   "   Ronnie Lane",
   "",
   "Art Assistance by:",
   "   Carey Chico and Franz Boehm",
   "",
   "BizDev Babe:",
   "   Jamie Bafus",
   "",
   "And...",
   "",
   "Our Big Toe:",
   "   Mitch Lasky",
   "",
   "",
   "Special Thanks to:",
   "  Id software",
   "  The original Hexen2 crew",
   "   We couldn't have done it",
   "   without you guys!",
   "",
   "",
   "Published by Id Software, Inc.",
   "Distributed by Activision, Inc.",
   "",
   "The Id Software Technology used",
   "under license in Hexen II (tm)",
   "(c) 1996, 1997 Id Software, Inc.",
   "All Rights Reserved.",
   "",
   "Hexen(r) is a registered trademark",
   "of Raven Software Corp.",
   "Hexen II (tm) and the Raven logo",
   "are trademarks of Raven Software",
   "Corp.  The Id Software name and",
   "id logo are trademarks of",
   "Id Software, Inc.  Activision(r)",
   "is a registered trademark of",
   "Activision, Inc. All other",
   "trademarks are the property of",
   "their respective owners.",
   "",
   "",
   "",
   "Send bug descriptions to:",
   "   h2bugs@mail.ravensoft.com",
   "",
   "",
   "No yaks were harmed in the",
   "making of this game!"
};

#define	MAX_LINES2_MP	151

static const char *Credit2TextMP[MAX_LINES2_MP] =
{
   "PowerTrip: James (emorog) Monroe",
   "Cartoons: Brian Raffel",
   "         (use more puzzles)",
   "Doc Keeper: Kevin Schilder",
   "",
   "Whip cracker: James Monroe",
   "",
   "Whipees:",
   "   Mike (i didn't break it) Gummelt",
   "   Josh (extern) Weier",
   "",
   "We don't deserve whipping:",
   "   Josh (I'm not on this project)",
   "         Heitzman",
   "   Nathan (deer hunter) Albury",
   "   Rick (model crusher) Johnson",
   "",
   "Bit Packer:",
   "   Mr. John (Slaine) Scott",
   "",
   "Lead Slacker: Jon (devil boy) Zuk",
   "",
   "Other Slackers:",
   "   Tom (can i have an office) Odell",
   "   Jeremy (nt crashed again) Statz",
   "   Mike (i should be doing my ",
   "         homework) Renner",
   "   Eric (the nose) Biessman",
   "   Kenn (.plan) Hoekstra",
   "   Matt (big elbow) Pinkston",
   "   Bobby (needs haircut) Duncanson",
   "   Brian (they're in my town) Raffel",
   "",
   "Use the mouse: Les Dorscheid",
   "",
   "What's a mouse?:",
   "   Kim (where's my desk) Lathrop",
   "   Gina (i can do your laundry)",
   "        Garren",
   "   Joe (broken axle) Koberstein",
   "   Kevin (titanic) Long",
   "   Jeff (norbert) Butler",
   "   Scott (what's the DEL key for?)",
   "          Rice",
   "   John (Shpluuurt!) Payne",
   "   Steve (crash) Raffel",
   "",
   "Boners:",
   "   Eric (terminator) Turman",
   "   Chaos Device",
   "",
   "Drum beater:",
   "   Kevin Schilder",
   "",
   "Whistle blower:",
   "   Chia Chin (bruce) Lee",
   "",
   "",
   "Activision",
   "",
   "Producer:",
   "   Steve 'Ferris' Stringer",
   "",
   "Marketing Product Manager:",
   "   Henk 'GODMODE' Hartong",
   "",
   "Marketing Associate:",
   "   Kevin 'Kraffinator' Kraff",
   "",
   "Senior Quality",
   "Assurance Lead:",
   "   Tim 'Outlaw' Vanlaw",
   "",
   "Quality Assurance Lead:",
   "   Doug Jacobs",
   "",
   "Shadow Finders:",
   "   Steve Rosenthal, Steve Elwell,",
   "   Chad Bordwell,",
   "   David 'Spice Girl' Baker,",
   "   Error Casillas, Damien Fischer,",
   "   Winnie Lee,",
   "   Ygor Krynytyskyy,",
   "   Samantha (Crusher) Lee, John Park",
   "   Ian Stevens, Chris Toft",
   "",
   "Production Testers:",
   "   Steve 'Damn It's Cold!'",
   "       Rosenthal and",
   "   Chad 'What Hotel Receipt?'",
   "        Bordwell",
   "",
   "Additional QA and Support:",
   "    Tony Villalobos",
   "    Jason Sullivan",
   "",
   "Installer by:",
   "   Steve 'Bahh' Stringer,",
   "   Adam Goldberg, Tanya Martino,",
   "   Eric Schmidt, Ronnie Lane",
   "",
   "Art Assistance by:",
   "   Carey 'Damien' Chico and",
   "   Franz Boehm",
   "",
   "BizDev Babe:",
   "   Jamie Bafus",
   "",
   "And...",
   "",
   "Our Big Toe:",
   "   Mitch Lasky",
   "",
   "",
   "Special Thanks to:",
   "  Id software",
   "  Anyone who ever worked for Raven,",
   "  (except for Alex)",
   "",
   "",
   "Published by Id Software, Inc.",
   "Distributed by Activision, Inc.",
   "",
   "The Id Software Technology used",
   "under license in Hexen II (tm)",
   "(c) 1996, 1997 Id Software, Inc.",
   "All Rights Reserved.",
   "",
   "Hexen(r) is a registered trademark",
   "of Raven Software Corp.",
   "Hexen II (tm) and the Raven logo",
   "are trademarks of Raven Software",
   "Corp.  The Id Software name and",
   "id logo are trademarks of",
   "Id Software, Inc.  Activision(r)",
   "is a registered trademark of",
   "Activision, Inc. All other",
   "trademarks are the property of",
   "their respective owners.",
   "",
   "",
   "",
   "Send bug descriptions to:",
   "   h2bugs@mail.ravensoft.com",
   "",
   "Special Thanks To:",
   "   E.H.S., The Osmonds,",
   "   B.B.V.D., Daisy The Lovin' Lamb,",
   "  'You Killed' Kenny,",
   "   and Baby Biessman.",
   ""
};

#define QUIT_SIZE 12	/* visible credit-scroll lines */

void M_Menu_Quit_f (void)
{
	if (m_state == m_quit)
		return;
	wasInMenus = !!(Key_GetDest () & key_menu);
	Key_SetDest (key_menu);
	m_quit_prevstate = m_state;
	m_state = m_quit;
	m_entersound = true;
//	msgNumber = rand()&7;

	LinePos = 0;
	LineTimes = 0;
	if (gameflags & GAME_PORTALS)
	{
		LineText = CreditTextMP;
		MaxLines = MAX_LINES_MP;
	}
	else
	{
		LineText = CreditText;
		MaxLines = MAX_LINES;
	}
	LineTxt2 = false;
	SoundPlayed = false;
}


static void M_Quit_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
	case 'n':
	case 'N':
		if (wasInMenus)
		{
			m_state = m_quit_prevstate;
			m_entersound = true;
		}
		else
		{
			Key_SetDest (key_game);
			m_state = m_none;
			Sbar_Changed ();
		}
		break;

	case 'Y':
	case 'y':
	/* K_ENTER is what a gamepad A press arrives as (M_Keydown maps it), so
	 * this is the confirm side of the prompt for a controller-only player.
	 * Without it the Steam Deck could reach this prompt and cancel it with B
	 * but never answer yes, leaving no way to quit without plugging in a
	 * keyboard.  The cancel side already worked: B arrives as K_ESCAPE.
	 * uhexen2-4364. */
	case K_ENTER:
		Key_SetDest (key_console);
		Host_Quit_f ();
		break;

	default:
		break;
	}
}

static void M_Quit_Draw (void)
{
	int i, x, y, place, topy;
	qpic_t	*p;

	if (wasInMenus)
	{
		m_state = m_quit_prevstate;
		m_recursiveDraw = true;
		M_Draw ();
		m_state = m_quit;
	}

	LinePos += host_frametime*1.75;
	if (LinePos > MaxLines + QUIT_SIZE + 2)
	{
		LinePos = 0;
		SoundPlayed = false;
		LineTimes++;
		if (LineTimes >= 2)
		{
			if (gameflags & GAME_PORTALS)
			{
				MaxLines = MAX_LINES2_MP;
				LineText = Credit2TextMP;
				BGM_PlayCDtrack (12, false);
			}
			else
			{
				MaxLines = MAX_LINES2;
				LineText = Credit2Text;
			}
			LineTxt2 = true;
		}
	}

	y = 12;
	M_DrawTextBox (0, 0, 38, 23);

#define QUIT_CENTER(str)	((320 - (int)strlen(str) * 8) / 2)
	{
		const char *l0 = "Hexenwail " HW_VERSION;
		const char *l1 = "Hammer of Thyrion 1.5.10 by sezero";
		const char *l2 = "Hexen II 1.29 by Raven Software";
		const char *l3 = "with Shanjaq & Inky additions,";
		const char *l4 = "& many more contributors";
		const char *l5 = "for Wabbit";
		M_Print      (QUIT_CENTER(l0), y,      l0);
		M_PrintWhite (QUIT_CENTER(l1), y + 12, l1);
		M_PrintWhite (QUIT_CENTER(l2), y + 20, l2);
		M_PrintWhite (QUIT_CENTER(l3), y + 32, l3);
		M_PrintWhite (QUIT_CENTER(l4), y + 40, l4);
		M_PrintWhite (QUIT_CENTER(l5), y + 52, l5);
	}
#undef QUIT_CENTER
	y += 68;

	if (LinePos > 55 && !SoundPlayed && LineTxt2)
	{
		S_LocalSound ("rj/steve.wav");
		SoundPlayed = true;
	}
	topy = y;
	place = floor(LinePos);
	y -= floor((LinePos - place) * 8);
	for (i = 0; i < QUIT_SIZE; i++, y += 8)
	{
		if (i + place - QUIT_SIZE >= MaxLines)
			break;
		if (i + place < QUIT_SIZE)
			continue;

		if (LineText[i + place - QUIT_SIZE][0] == ' ')
			M_PrintWhite(24, y, LineText[i + place - QUIT_SIZE]);
		else
			M_Print(24, y, LineText[i + place - QUIT_SIZE]);
	}

	p = Draw_CachePic ("gfx/box_mm2.lmp");
	x = 24;
	y = topy - 8;
	for (i = 4; i < 38; i++, x += 8)
	{
		M_DrawPic(x, y, p);	// background at top for smooth scroll out
		M_DrawPic(x, y + (QUIT_SIZE*8) + 8, p);	// draw at bottom for smooth scroll in
	}

	y += (QUIT_SIZE * 8) + 8;
	M_PrintWhite ((320 - 15 * 8) / 2, y,  "Press y to exit");
}

//=============================================================================

static int	lanConfig_cursor = -1;
static const int	lanConfig_cursor_table[] = {100, 120, 140, 172};
#define NUM_LANCONFIG_CMDS	4

static int	lanConfig_port;
static char	lanConfig_portname[6];
static char	lanConfig_joinname[30];

static void M_Menu_LanConfig_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_lanconfig;
	m_entersound = true;
	if (lanConfig_cursor == -1)
	{
		if (JoiningGame && TCPIPConfig)
			lanConfig_cursor = 2;
		else
			lanConfig_cursor = 1;
	}
	if (StartingGame && lanConfig_cursor >= 2)
		lanConfig_cursor = 1;
	lanConfig_port = DEFAULTnet_hostport;
	q_snprintf(lanConfig_portname, sizeof(lanConfig_portname), "%d", lanConfig_port);

	m_return_onerror = false;
	m_return_reason[0] = 0;

	setup_class = cl_playerclass.integer;
	if (setup_class < 1 || setup_class > MAX_PLAYER_CLASS)
		setup_class = MAX_PLAYER_CLASS;
#if ENABLE_OLD_DEMO
	if (gameflags & GAME_OLD_DEMO)
	{
		if (setup_class != CLASS_PALADIN && setup_class != CLASS_THEIF)
			setup_class = CLASS_PALADIN;
	}
	else
#endif	/* OLD_DEMO */
	if (!(gameflags & GAME_PORTALS))
	{
		if (setup_class > MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES)
			setup_class = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
	}
	setup_class--;
}


static void M_LanConfig_Draw (void)
{
	int	basex;
	const char	*startJoin;
	const char	*protocol;

	ScrollTitle("gfx/menu/title4.lmp");
	basex = 48;

	if (StartingGame)
		startJoin = "New Game";
	else
		startJoin = "Join Game";
	protocol = "TCP/IP";
	M_Print (basex, 60, va ("%s - %s", startJoin, protocol));
	basex += 8;

	M_Print (basex, 80, "Address:");
	M_Print (basex+9*8, 80, my_tcpip_address);

	M_Print (basex, lanConfig_cursor_table[0], "Port");
	M_DrawTextBox (basex+8*8, lanConfig_cursor_table[0]-8, 6, 1);
	M_Print (basex+9*8, lanConfig_cursor_table[0], lanConfig_portname);

	if (JoiningGame)
	{
		M_Print (basex, lanConfig_cursor_table[1], "Class:");
		M_Print (basex+8*7, lanConfig_cursor_table[1], ClassNames[setup_class]);

		M_Print (basex, lanConfig_cursor_table[2], "Search for local games...");
		M_Print (basex, 156, "Join game at:");
		M_DrawTextBox (basex, lanConfig_cursor_table[3]-8, 30, 1);
		M_Print (basex+8, lanConfig_cursor_table[3], lanConfig_joinname);
	}
	else
	{
		M_DrawTextBox (basex, lanConfig_cursor_table[1]-8, 2, 1);
		M_Print (basex+8, lanConfig_cursor_table[1], "OK");
	}

	M_DrawCharacter (basex-8, lanConfig_cursor_table [lanConfig_cursor], 12+((int)(realtime*4)&1));

	if (lanConfig_cursor == 0)
		M_DrawCharacter (basex+9*8 + 8*strlen(lanConfig_portname), lanConfig_cursor_table [0], 10+((int)(realtime*4)&1));

	if (lanConfig_cursor == 3)
		M_DrawCharacter (basex+8 + 8*strlen(lanConfig_joinname), lanConfig_cursor_table [3], 10+((int)(realtime*4)&1));

	if (*m_return_reason)
		M_PrintWhite (basex, 192, m_return_reason);
}


static void M_LanConfig_Key (int key)
{
	int		l;

	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Net_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		lanConfig_cursor--;

		if (JoiningGame)
		{
			if (lanConfig_cursor < 0)
				lanConfig_cursor = NUM_LANCONFIG_CMDS-1;
		}
		else
		{
			if (lanConfig_cursor < 0)
				lanConfig_cursor = NUM_LANCONFIG_CMDS-2;
		}
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		lanConfig_cursor++;
		if (lanConfig_cursor >= NUM_LANCONFIG_CMDS)
			lanConfig_cursor = 0;
		break;

	case K_ENTER:
		if ((JoiningGame && lanConfig_cursor <= 1) ||
		    (!JoiningGame && lanConfig_cursor == 0))
			break;

		m_entersound = true;
		if (JoiningGame)
			Cbuf_AddText ( va ("playerclass %d\n", setup_class+1) );

		M_ConfigureNetSubsystem ();

		if ((JoiningGame && lanConfig_cursor == 2) ||
		    (!JoiningGame && lanConfig_cursor == 1))
		{
			if (StartingGame)
			{
				M_Menu_GameOptions_f ();
				break;
			}
			M_Menu_Search_f();
			break;
		}

		if (lanConfig_cursor == 3)
		{
			m_return_state = m_state;
			m_return_onerror = true;
			Key_SetDest (key_game);
			m_state = m_none;
			Cbuf_AddText ( va ("connect \"%s\"\n", lanConfig_joinname) );
			break;
		}

		break;

	case K_BACKSPACE:
		if (lanConfig_cursor == 0)
		{
			l = strlen(lanConfig_portname);
			if (l)
				lanConfig_portname[l-1] = 0;
		}
		else if (lanConfig_cursor == 3)
		{
			l = strlen(lanConfig_joinname);
			if (l)
				lanConfig_joinname[l-1] = 0;
		}
		break;

	case K_LEFTARROW:
		if (lanConfig_cursor != 1 || !JoiningGame)
			break;

		S_LocalSound ("raven/menu3.wav");
#if ENABLE_OLD_DEMO
		if (gameflags & GAME_OLD_DEMO)
		{
			setup_class = (setup_class == CLASS_PALADIN-1) ? CLASS_THEIF-1 : CLASS_PALADIN-1;
			break;
		}
#endif	/* OLD_DEMO */
		setup_class--;
		if (setup_class < 0)
			setup_class = MAX_PLAYER_CLASS -1;
		if (setup_class > MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES - 1 && !(gameflags & GAME_PORTALS))
			setup_class = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES -1;
		break;

	case K_RIGHTARROW:
		if (lanConfig_cursor != 1 || !JoiningGame)
			break;

		S_LocalSound ("raven/menu3.wav");
#if ENABLE_OLD_DEMO
		if (gameflags & GAME_OLD_DEMO)
		{
			setup_class = (setup_class == CLASS_PALADIN-1) ? CLASS_THEIF-1 : CLASS_PALADIN-1;
			break;
		}
#endif	/* OLD_DEMO */
		setup_class++;
		if (setup_class > MAX_PLAYER_CLASS - 1)
			setup_class = 0;
		if (setup_class > MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES - 1 && !(gameflags & GAME_PORTALS))
			setup_class = 0;
		break;

	default:
		if (key < 32 || key > 127)
			break;

		if (lanConfig_cursor == 3)
		{
			l = strlen(lanConfig_joinname);
			if (l < 29)
			{
				lanConfig_joinname[l+1] = 0;
				lanConfig_joinname[l] = key;
			}
		}

		if (key < '0' || key > '9')
			break;
		if (lanConfig_cursor == 0)
		{
			l = strlen(lanConfig_portname);
			if (l < 5)
			{
				lanConfig_portname[l+1] = 0;
				lanConfig_portname[l] = key;
			}
		}
	}

	if (StartingGame && lanConfig_cursor == 2)
	{
		if (key == K_UPARROW)
			lanConfig_cursor = 1;
		else
			lanConfig_cursor = 0;
	}
	l =  atoi(lanConfig_portname);
	if (l > 65535)
		l = lanConfig_port;
	else
		lanConfig_port = l;
	q_snprintf(lanConfig_portname, sizeof(lanConfig_portname), "%d", lanConfig_port);
}

//=============================================================================
/* GAME OPTIONS MENU */

static const struct
{
	const char	*name;
	const char	*description;
} levels[] =
{
	{"demo1", "Blackmarsh"},			// 0
	{"demo2", "Barbican"},				// 1

	{"ravdm1", "Deathmatch 1"},			// 2

	{"demo1","Blackmarsh"},				// 3
	{"demo2","Barbican"},				// 4
	{"demo3","The Mill"},				// 5
	{"village1","King's Court"},			// 6
	{"village2","Inner Courtyard"},			// 7
	{"village3","Stables"},				// 8
	{"village4","Palace Entrance"},			// 9
	{"village5","The Forgotten Chapel"},		// 10
	{"rider1a","Famine's Domain"},			// 11

	{"meso2","Plaza of the Sun"},			// 12
	{"meso1","The Palace of Columns"},		// 13
	{"meso3","Square of the Stream"},		// 14
	{"meso4","Tomb of the High Priest"},		// 15
	{"meso5","Obelisk of the Moon"},		// 16
	{"meso6","Court of 1000 Warriors"},		// 17
	{"meso8","Bridge of Stars"},			// 18
	{"meso9","Well of Souls"},			// 19

	{"egypt1","Temple of Horus"},			// 20
	{"egypt2","Ancient Temple of Nefertum"},	// 21
	{"egypt3","Temple of Nefertum"},		// 22
	{"egypt4","Palace of the Pharaoh"},		// 23
	{"egypt5","Pyramid of Anubis"},			// 24
	{"egypt6","Temple of Light"},			// 25
	{"egypt7","Shrine of Naos"},			// 26
	{"rider2c","Pestilence's Lair"},		// 27

	{"romeric1","The Hall of Heroes"},		// 28
	{"romeric2","Gardens of Athena"},		// 29
	{"romeric3","Forum of Zeus"},			// 30
	{"romeric4","Baths of Demetrius"},		// 31
	{"romeric5","Temple of Mars"},			// 32
	{"romeric6","Coliseum of War"},			// 33
	{"romeric7","Reflecting Pool"},			// 34

	{"cath","Cathedral"},				// 35
	{"tower","Tower of the Dark Mage"},		// 36
	{"castle4","The Underhalls"},			// 37
	{"castle5","Eidolon's Ordeal"},			// 38
	{"eidolon","Eidolon's Lair"},			// 39

	{"ravdm1","Atrium of Immolation"},		// 40
	{"ravdm2","Total Carnage"},			// 41
	{"ravdm3","Reckless Abandon"},			// 42
	{"ravdm4","Temple of RA"},			// 43
	{"ravdm5","Tom Foolery"},			// 44

	{"ravdm1", "Deathmatch 1"},			// 45

//OEM
	{"demo1","Blackmarsh"},				// 46
	{"demo2","Barbican"},				// 47
	{"demo3","The Mill"},				// 48
	{"village1","King's Court"},			// 49
	{"village2","Inner Courtyard"},			// 50
	{"village3","Stables"},				// 51
	{"village4","Palace Entrance"},			// 52
	{"village5","The Forgotten Chapel"},		// 53
	{"rider1a","Famine's Domain"},			// 54

//Mission Pack
	{"keep1",	"Eidolon's Lair"},		// 55
	{"keep2",	"Village of Turnabel"},		// 56
	{"keep3",	"Duke's Keep"},			// 57
	{"keep4",	"The Catacombs"},		// 58
	{"keep5",	"Hall of the Dead"},		// 59

	{"tibet1",	"Tulku"},			// 60
	{"tibet2",	"Ice Caverns"},			// 61
	{"tibet3",	"The False Temple"},		// 62
	{"tibet4",	"Courtyards of Tsok"},		// 63
	{"tibet5",	"Temple of Kalachakra"},	// 64
	{"tibet6",	"Temple of Bardo"},		// 65
	{"tibet7",	"Temple of Phurbu"},		// 66
	{"tibet8",	"Palace of Emperor Egg Chen"},	// 67
	{"tibet9",	"Palace Inner Chambers"},	// 68
	{"tibet10",	"The Inner Sanctum of Praevus"},// 69
};

static const struct
{
	const char	*description;
	int		firstLevel;
	int		levels;
} episodes[] =
{
	// Demo
	{"Demo", 0, 2},
	{"Demo Deathmatch", 2, 1},

	// Registered
	{"Village", 3, 9},
	{"Meso", 12, 8},
	{"Egypt", 20, 8},
	{"Romeric", 28, 7},
	{"Cathedral", 35, 5},

	{"MISSION PACK", 55, 15},

	{"Deathmatch", 40, 5},

	// OEM
	{"Village", 46, 9},
	{"Deathmatch", 45, 1},
};

#define OEM_START 9
#define REG_START 2
#define MP_START 7
#define DM_START 8

static int	startepisode;
static int	startlevel;
static int	maxplayers;
//static qboolean m_serverInfoMessage = false;
//static double	m_serverInfoMessageTime;

static const int	gameoptions_cursor_table[] = {40, 56, 64, 72, 80, 88, 96, 104, 112, 128, 136};
#define	NUM_GAMEOPTIONS	11
static int	gameoptions_cursor;

static void M_Menu_GameOptions_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_gameoptions;
	m_entersound = true;
	if (maxplayers == 0)
		maxplayers = svs.maxclients;
	if (maxplayers < 2)
		maxplayers = svs.maxclientslimit;

	setup_class = cl_playerclass.integer;
	if (setup_class < 1 || setup_class > MAX_PLAYER_CLASS)
		setup_class = MAX_PLAYER_CLASS;
#if ENABLE_OLD_DEMO
	if (gameflags & GAME_OLD_DEMO)
	{
		if (setup_class != CLASS_PALADIN && setup_class != CLASS_THEIF)
			setup_class = CLASS_PALADIN;
	}
	else
#endif	/* OLD_DEMO */
	if (!(gameflags & GAME_PORTALS))
	{
		if (setup_class > MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES)
			setup_class = MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
	}
	setup_class--;

	if (oem.integer)
	{
		if (startepisode < OEM_START || startepisode > OEM_START+1)
			startepisode = OEM_START;
		if (coop.integer)
			startepisode = OEM_START;
	}
	else if (registered.integer)
	{
		if (startepisode < REG_START || startepisode >= OEM_START)
			startepisode = REG_START;
		else if (startepisode == MP_START && !(gameflags & GAME_PORTALS))
			startepisode = REG_START;
		if (coop.integer && startepisode == DM_START)
			startepisode = REG_START;
	}
	else	// demo
	{
		if (startepisode < 0 || startepisode > 1)
			startepisode = 0;
		if (coop.integer)
			startepisode = 0;
	}

	if (coop.integer)
	{
		startlevel = 0;
		if (gameoptions_cursor >= NUM_GAMEOPTIONS-1)
			gameoptions_cursor = 0;
	}
}

static void M_GameOptions_Draw (void)
{
	ScrollTitle("gfx/menu/title4.lmp");

	M_DrawTextBox (152+8, 60, 10, 1);
	M_Print (160+8, 68, "begin game");

//	we use 17 character option titles. the second increment
//	to the x offset is: (17 - strlen(option_title)) * 8
	M_Print (0+8 + 6*8, 84, "Max players");
	M_Print (160+8, 84, va("%i", maxplayers) );

	M_Print (0+8 + 8*8, 92, "Game Type");
	if (coop.integer)
		M_Print (160+8, 92, "Cooperative");
	else
		M_Print (160+8, 92, "Deathmatch");

	M_Print (0+8 + 9*8, 100, "Teamplay");
	{
		const char	*msg;

		switch (teamplay.integer)
		{
			case 1:
				msg = "No Friendly Fire";
				break;
			case 2:
				msg = "Friendly Fire";
				break;
			default:
				msg = "Off";
				break;
		}
		M_Print (160+8, 100, msg);
	}

	M_Print (0+8 + 12*8, 108, "Class");
	M_Print (160+8, 108, ClassNames[setup_class]);

	M_Print (0+8 + 7*8, 116, "Difficulty");

	M_Print (160+8, 116, DiffNames[setup_class][skill.integer]);

	M_Print (0+8 + 7*8, 124, "Frag Limit");
	if (fraglimit.integer == 0)
		M_Print (160+8, 124, "none");
	else
		M_Print (160+8, 124, va("%i frags", fraglimit.integer));

	M_Print (0+8 + 7*8, 132, "Time Limit");
	if (timelimit.integer == 0)
		M_Print (160+8, 132, "none");
	else
		M_Print (160+8, 132, va("%i minutes", timelimit.integer));

	M_Print (0+8 + 5*8, 140, "Random Class");
	if (randomclass.integer)
		M_Print (160+8, 140, "on");
	else
		M_Print (160+8, 140, "off");

	M_Print (0+8 + 10*8, 156, "Episode");
	M_Print (160+8, 156, episodes[startepisode].description);

	M_Print (0+8 + 12*8, 164, "Level");
	M_Print (160+8, 164, levels[episodes[startepisode].firstLevel + startlevel].name);
	M_Print (96, 180, levels[episodes[startepisode].firstLevel + startlevel].description);

// line cursor
	M_DrawCharacter (172-16, gameoptions_cursor_table[gameoptions_cursor]+28, 12+((int)(realtime*4)&1));

/*	rjr
	if (m_serverInfoMessage)
	{
		if ((realtime - m_serverInfoMessageTime) < 5.0)
		{
			x = (320-26*8)/2;
			M_DrawTextBox (x, 138, 24, 4);
			x += 8;
			M_Print (x, 146, "  More than 4 players   ");
			M_Print (x, 154, " requires using command ");
			M_Print (x, 162, "line parameters; please ");
			M_Print (x, 170, "   see techinfo.txt.    ");
		}
		else
		{
			m_serverInfoMessage = false;
		}
	}*/
}


static void M_NetStart_Change (int dir)
{
	int	val;
	switch (gameoptions_cursor)
	{
	case 1:
		maxplayers += dir;
		if (maxplayers > svs.maxclientslimit)
		{
			maxplayers = svs.maxclientslimit;
		//	m_serverInfoMessage = true;
		//	m_serverInfoMessageTime = realtime;
		}
		if (maxplayers < 2)
			maxplayers = 2;
		break;

	case 2:
		if (coop.integer)
		{
			Cvar_Set ("coop", "0");
			break;
		}
		Cvar_Set ("coop", "1");
		startlevel = 0;
		if (startepisode == 1)
			startepisode = 0;
		else if (startepisode == DM_START)
			startepisode = REG_START;
		else if (startepisode == OEM_START+1)
			startepisode = OEM_START;
		break;

	case 3:
		val = teamplay.integer + dir;
		if (val > 2)
			val = 0;
		else if (val < 0)
			val = 2;
		Cvar_SetValue ("teamplay", val);
		break;

	case 4:
#if ENABLE_OLD_DEMO
		if (gameflags & GAME_OLD_DEMO)
		{
			setup_class = (setup_class == CLASS_PALADIN-1) ? CLASS_THEIF-1 : CLASS_PALADIN-1;
			break;
		}
#endif	/* OLD_DEMO */
		setup_class += dir;
		if (setup_class < 0)
			setup_class = MAX_PLAYER_CLASS - 1;
		else if(setup_class > MAX_PLAYER_CLASS - 1)
			setup_class = 0;
		if (setup_class > MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES - 1 && !(gameflags & GAME_PORTALS))
			setup_class = (dir > 0)? 0 : MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES - 1;
		break;

	case 5:
		val = skill.integer + dir;
		if (val > 3)
			val = 0;
		else if (val < 0)
			val = 3;
		Cvar_SetValue ("skill", val);
		break;

	case 6:
		val = fraglimit.integer + dir*10;
		if (val > 100)
			val = 0;
		else if (val < 0)
			val = 100;
		Cvar_SetValue ("fraglimit", val);
		break;

	case 7:
		val = timelimit.integer + dir*5;
		if (val > 60)
			val = 0;
		else if (val < 0)
			val = 60;
		Cvar_SetValue ("timelimit", val);
		break;

	case 8:
		Cvar_Set ("randomclass", randomclass.integer ? "0" : "1");
		break;

	case 9:
		if (registered.integer)
		{
			startepisode += dir;
			startlevel = 0;
			if (startepisode > DM_START)
				startepisode = REG_START;
			else
			{
				if (startepisode == MP_START && !(gameflags & GAME_PORTALS))
					startepisode += dir;
				if (coop.integer && startepisode == DM_START)
					startepisode = (dir > 0) ? REG_START : ((gameflags & GAME_PORTALS) ? MP_START : MP_START-1);
				if (startepisode < REG_START)
					startepisode = (coop.integer) ? ((gameflags & GAME_PORTALS) ? MP_START : MP_START-1) : DM_START;
			}
		}
		else if (oem.integer)
		{
			if (!coop.integer)
			{
				startepisode = (startepisode != OEM_START) ? OEM_START : OEM_START+1;
				startlevel = 0;
			}
		}
		else	// demo version
		{
			if (!coop.integer)
			{
				startepisode = (startepisode != 0) ? 0 : 1;
				startlevel = 0;
			}
		}
		break;

	case 10:
		if (coop.integer)
		{
			startlevel = 0;
			break;
		}
		startlevel += dir;

		if (startlevel < 0)
			startlevel = episodes[startepisode].levels - 1;
		else if (startlevel >= episodes[startepisode].levels)
			startlevel = 0;
		break;
	}
}

static void M_GameOptions_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Net_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		gameoptions_cursor--;
		if (gameoptions_cursor < 0)
		{
			gameoptions_cursor = NUM_GAMEOPTIONS-1;
			if (coop.integer)
				gameoptions_cursor--;
		}
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		gameoptions_cursor++;
		if (coop.integer)
		{
			if (gameoptions_cursor >= NUM_GAMEOPTIONS-1)
				gameoptions_cursor = 0;
		}
		else
		{
			if (gameoptions_cursor >= NUM_GAMEOPTIONS)
				gameoptions_cursor = 0;
		}
		break;

	case K_LEFTARROW:
		if (gameoptions_cursor == 0)
			break;
		S_LocalSound ("raven/menu3.wav");
		M_NetStart_Change (-1);
		break;

	case K_RIGHTARROW:
		if (gameoptions_cursor == 0)
			break;
		S_LocalSound ("raven/menu3.wav");
		M_NetStart_Change (1);
		break;

	case K_ENTER:
		S_LocalSound ("raven/menu2.wav");
		if (gameoptions_cursor == 0)
		{
			if (sv.active)
				Cbuf_AddText ("disconnect\n");
			Cbuf_AddText ( va ("playerclass %d\n", setup_class+1) );
			Cbuf_AddText ("listen 0\n");	// so host_netport will be re-examined
			Cbuf_AddText ( va ("maxplayers %d\n", maxplayers) );
			SCR_BeginLoadingPlaque ();

			Cbuf_AddText ( va ("map %s\n", levels[episodes[startepisode].firstLevel + startlevel].name) );

			return;
		}

		M_NetStart_Change (1);
		break;
	}
}

//=============================================================================
/* SEARCH MENU */

static qboolean	searchComplete = false;
static double	searchCompleteTime;

static void M_Menu_Search_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_search;
	m_entersound = false;
	slistSilent = true;
	slistLocal = false;
	searchComplete = false;
	NET_Slist_f();
}


/*
=============================================================================

MAPS BROWSER MENU -- Ironwail's M_Menu_Maps_f / ExtraMaps (uhexen2-a5nn.13)

Loading a third-party map used to mean knowing its bsp name and typing
`map <name>` at the console.  This lists what is installed, by title rather
than by filename, which is the difference between a browsable list and a
directory dump.

Titles come from each map's worldspawn "message" via FS_GetMapTitle; a map
without one, or one we cannot read, is listed under its filename rather than
hidden.  The scan itself is the one maplist and randmap already use, so this
adds a view, not a second enumeration.

Structurally this is M_Mods_* with the parts it does not need removed: no
separator row, no unselectable rows, no per-row toggle.  The cursor and the
scroll offset index maps_view, never maps_list -- same rule as the mods menu,
and for the same reason (uhexen2-8uc1).

=============================================================================
*/

#define	MAPS_MAX		1024
#define	MAPS_LIST_TOP		60
#define	MAPS_LIST_X		32
#define	MAPS_CURSOR_X		24
#define	MAPS_SCROLLBAR_X	284
#define	MAPS_FOOTER_RESERVE	24	/* px below the list: search line */
#define	MAPS_TAG_RIGHT		280
#define	MAPS_LABEL_MAX		28	/* characters between MAPS_LIST_X and the tag */

typedef struct
{
	char		name[MAX_QPATH];	/* bsp basename -- what `map' takes */
	char		title[64];		/* worldspawn message, "" if none */
	qboolean	is_start;
} mapentry_t;

static mapentry_t	maps_list[MAPS_MAX];
static int	maps_count;
static int	maps_view[MAPS_MAX];
static int	maps_view_count;
static int	maps_cursor;		/* index into maps_view */
static int	maps_top;		/* first visible view row */
static qboolean	maps_truncated;

/* A start map is where you enter an episode, so it is worth pointing at in a
 * list of eighty.  "start" and "startNN" are the community convention Ironwail
 * encodes (ExtraMaps_IsStart); demo1 is here because it is Hexen II's own
 * campaign entry -- New Game runs exactly `map demo1' (menu.c:887). */
static qboolean M_Maps_IsStart (const char *name)
{
	int	i;

	if (!q_strcasecmp (name, "demo1"))
		return true;
	if (q_strncasecmp (name, "start", 5) != 0)
		return false;
	for (i = 5; name[i]; i++)
	{
		if (name[i] < '0' || name[i] > '9')
			return false;
	}
	return true;
}

/*
Hexen II keeps level titles in strings.txt and puts only the INDEX in
worldspawn's message -- "325", not "The Cathedral".  Quake stores the text,
which is why this step has no counterpart in Ironwail's ExtraMaps and why a
straight port of it lists a column of three-digit numbers.

The engine's own table (Host_LoadStrings / Host_GetString) is NOT usable
here.  It is loaded only when a map spawns -- cl_parse.c:687, sv_main.c:2958,
cl_inlude.c:141, and nowhere in Host_Init -- so at the main menu, which is
exactly where you browse for a map to load, host_string_count is 0.  Calling
Host_LoadStrings from the menu to fix that would be worse: it allocates on
the hunk, which the next map load resets underneath it, and it Host_Errors on
a missing file, which is not an acceptable way for a menu to behave.

So the menu reads the file itself, once per scan, into malloc'd storage it
owns and frees.  It is one file read alongside the N bsp headers the scan
already does.
*/
static char	*maps_strings;		/* strings.txt, newlines -> NUL */
static char	**maps_string_line;
static int	maps_string_count;

static void M_Maps_FreeStrings (void)
{
	if (maps_string_line)
	{
		free (maps_string_line);
		maps_string_line = NULL;
	}
	if (maps_strings)
	{
		free (maps_strings);
		maps_strings = NULL;
	}
	maps_string_count = 0;
}

static void M_Maps_LoadStrings (void)
{
	byte	*data;
	char	*p;
	int	n, i;

	M_Maps_FreeStrings ();

	data = FS_LoadMallocFile ("strings.txt", NULL);
	if (!data)
		return;		/* no titles; rows fall back to filenames */
	maps_strings = (char *)data;

	/* Count lines, then index them.  Both CR and LF terminate a line and a
	 * CRLF pair must not count twice, which is the same rule
	 * Host_LoadStrings applies. */
	n = 1;
	for (p = maps_strings; *p; p++)
	{
		if (*p == '\r' || *p == '\n')
		{
			if (p[0] == '\r' && p[1] == '\n')
				p++;
			n++;
		}
	}

	maps_string_line = (char **) malloc (sizeof(char *) * (size_t)n);
	if (!maps_string_line)
	{
		M_Maps_FreeStrings ();
		return;
	}

	i = 0;
	maps_string_line[i++] = maps_strings;
	for (p = maps_strings; *p && i < n; p++)
	{
		if (*p == '\r' || *p == '\n')
		{
			qboolean crlf = (p[0] == '\r' && p[1] == '\n');
			*p = '\0';
			if (crlf)
				*(++p) = '\0';
			maps_string_line[i++] = p + 1;
		}
	}
	maps_string_count = i;
}

/*
Third-party maps write a literal title as often as an index, so a message
that is not all digits is taken at face value.  An index outside the table
yields no title and the row falls back to its filename.
*/
static void M_Maps_ResolveTitle (const char *raw, char *out, size_t outsize)
{
	int	i, idx;

	out[0] = '\0';
	if (!raw || !*raw)
		return;

	for (i = 0; raw[i]; i++)
	{
		if (raw[i] < '0' || raw[i] > '9')
		{
			q_strlcpy (out, raw, outsize);
			return;
		}
	}

	idx = atoi (raw);
	if (idx < 0 || idx >= maps_string_count || !maps_string_line)
		return;
	q_strlcpy (out, maps_string_line[idx], outsize);
}

static void M_ScanMaps (void)
{
	int	i, n;
	char	raw[64];

	maps_count = 0;
	maps_truncated = false;

	M_Maps_LoadStrings ();

	n = FS_BuildMapList (NULL);
	for (i = 0; i < n; i++)
	{
		const char	*name = FS_MapListName (i);
		mapentry_t	*e;

		if (!name || !*name)
			continue;
		if (maps_count >= MAPS_MAX)
		{
			maps_truncated = true;
			break;
		}
		e = &maps_list[maps_count++];
		q_strlcpy (e->name, name, sizeof(e->name));
		e->is_start = M_Maps_IsStart (e->name);
		/* One bsp header read per map.  Only the entity lump's head is
		 * touched, and this runs once when the menu opens, not per frame. */
		if (FS_GetMapTitle (e->name, raw, sizeof(raw)))
			M_Maps_ResolveTitle (raw, e->title, sizeof(e->title));
		else
			e->title[0] = '\0';
	}

	FS_FreeNameList ();
	M_Maps_FreeStrings ();

	if (maps_truncated)
		Con_Printf ("maps menu: more than %d maps installed, listing the first %d\n",
			    MAPS_MAX, MAPS_MAX);
}

/* What the row shows: the title when the map has one, the filename otherwise.
 * Ironwail lists the message for the same reason -- "Blackmarsh" is findable,
 * "demo1" is not. */
static const char *M_Maps_Label (const mapentry_t *e)
{
	static char	buf[MAPS_LABEL_MAX + 1];
	const char	*src = e->title[0] ? e->title : e->name;

	q_strlcpy (buf, src, sizeof(buf));
	return buf;
}

static int M_Maps_VisibleRows (void)
{
	int	rows = (200 - MAPS_LIST_TOP - MAPS_FOOTER_RESERVE) / 8;

	if (rows < 4)
		rows = 4;
	return rows;
}

static void M_Maps_BuildView (void)
{
	int	i;

	maps_view_count = 0;
	for (i = 0; i < maps_count; i++)
	{
		/* filter on both, so a tester who knows either the title or the
		 * bsp name finds the row */
		if (M_Filter_Active() &&
		    !M_Filter_Matches(maps_list[i].title) &&
		    !M_Filter_Matches(maps_list[i].name))
			continue;
		maps_view[maps_view_count++] = i;
	}

	if (maps_cursor >= maps_view_count)
		maps_cursor = maps_view_count - 1;
	if (maps_cursor < 0)
		maps_cursor = 0;
}

static void M_Maps_EnsureVisible (void)
{
	int	visible = M_Maps_VisibleRows ();

	if (maps_cursor < maps_top)
		maps_top = maps_cursor;
	else if (maps_cursor >= maps_top + visible)
		maps_top = maps_cursor - visible + 1;
	if (maps_top > maps_view_count - visible)
		maps_top = maps_view_count - visible;
	if (maps_top < 0)
		maps_top = 0;
}

static void M_Maps_ClampCursor (void)
{
	if (maps_view_count <= 0)
	{
		maps_cursor = 0;
		maps_top = 0;
		return;
	}
	if (maps_cursor >= maps_view_count)
		maps_cursor = maps_view_count - 1;
	if (maps_cursor < 0)
		maps_cursor = 0;
	M_Maps_EnsureVisible ();
}

static void M_Maps_FilterChanged (void)
{
	M_Maps_BuildView ();
	maps_cursor = 0;
	maps_top = 0;
	M_Maps_ClampCursor ();
}

static void M_Menu_Maps_f (void)
{
	int	i;

	Key_SetDest (key_menu);
	m_state = m_maps;
	m_entersound = true;

	/* the search buffer is shared with the other filtered submenus */
	M_Filter_Clear ();

	M_ScanMaps ();
	M_Maps_BuildView ();

	/* open on the running map when there is one, so the list is a place you
	 * can orient yourself rather than always starting at the top */
	maps_cursor = 0;
	if (cl.worldmodel && cls.state == ca_active)
	{
		char	cur[MAX_QPATH];

		COM_FileBase (cl.worldmodel->name, cur, sizeof(cur));
		for (i = 0; i < maps_view_count; i++)
		{
			if (!q_strcasecmp (maps_list[maps_view[i]].name, cur))
			{
				maps_cursor = i;
				break;
			}
		}
	}
	maps_top = 0;
	M_Maps_ClampCursor ();
}

static void M_Maps_MouseHover (int nrows)
{
	int	vy, k;

	if (!menu_mouse_moved)
		return;

	vy = M_ScreenYToCanvasY (menu_mouse_y);
	for (k = 0; k < nrows; k++)
	{
		int y = MAPS_LIST_TOP + k * 8;
		if (vy >= y && vy < y + 8)
		{
			maps_cursor = maps_top + k;
			M_HoverSound (maps_cursor);
			return;
		}
	}
	M_HoverSound (-1);
}

static void M_Maps_Draw (void)
{
	int	i, k, y, cursor_y, yf;
	int	visible, last_visible, nrows, list_bottom;

	ScrollTitle("gfx/menu/title0.lmp");

	visible = M_Maps_VisibleRows ();
	M_Maps_EnsureVisible ();

	last_visible = maps_top + visible;
	if (last_visible > maps_view_count)
		last_visible = maps_view_count;
	nrows = last_visible - maps_top;
	if (nrows < 0)
		nrows = 0;

	/* hover before draw, so highlight and blinking cursor agree with the
	 * pointer in the same frame rather than trailing it by one */
	M_Maps_MouseHover (nrows);

	cursor_y = -1;
	for (k = 0; k < nrows; k++)
	{
		const mapentry_t *e;

		i = maps_top + k;
		y = MAPS_LIST_TOP + k * 8;
		e = &maps_list[maps_view[i]];

		if (i == maps_cursor)
		{
			cursor_y = y;
			M_PrintWhite (MAPS_LIST_X, y, M_Maps_Label(e));
		}
		else
			M_Print (MAPS_LIST_X, y, M_Maps_Label(e));

		if (e->is_start)
		{
			const char *tag = "start";
			M_Print (MAPS_TAG_RIGHT - (int)strlen(tag) * 8, y, tag);
		}
	}

	if (maps_view_count == 0)
		M_Print (MAPS_LIST_X, MAPS_LIST_TOP,
			 maps_count ? "No maps match" : "No maps found");

	list_bottom = MAPS_LIST_TOP + (nrows ? nrows : 1) * 8;

	/* up/down scroll indicators */
	if (maps_top > 0)
		M_DrawCharacter (MAPS_LIST_X - 16, MAPS_LIST_TOP, 128);
	if (last_visible < maps_view_count && nrows)
		M_DrawCharacter (MAPS_LIST_X - 16, MAPS_LIST_TOP + (nrows - 1) * 8, 129);

	/* proportional scrollbar on the right edge */
	if (maps_view_count > visible)
	{
		int	track_y = MAPS_LIST_TOP;
		int	track_h = list_bottom - MAPS_LIST_TOP;
		int	thumb_h = (visible * track_h) / maps_view_count;
		int	thumb_y = track_y + (maps_top * track_h) / maps_view_count;
		int	j;

		if (thumb_h < 8)
			thumb_h = 8;
		for (j = 0; j < track_h; j += 8)
		{
			int cy = track_y + j;
			if (cy >= thumb_y && cy < thumb_y + thumb_h)
				M_DrawCharacter (MAPS_SCROLLBAR_X, cy, 11);
			else
				M_DrawCharacter (MAPS_SCROLLBAR_X, cy, '-');
		}
	}

	yf = list_bottom + 4;

	/* The bsp name under the list: the title is what you search by, but the
	 * filename is what you would type at the console, and a tester filing a
	 * report needs that one. */
	if (maps_view_count > 0)
	{
		const mapentry_t *e = &maps_list[maps_view[maps_cursor]];
		if (e->title[0])
			M_Print (MAPS_LIST_X, yf, va("(%s)", e->name));
	}

	M_Filter_Draw (MAPS_LIST_X, yf + 10);

	if (cursor_y >= 0)
		M_DrawCharacter (MAPS_CURSOR_X, cursor_y, 12 + ((int)(realtime * 4) & 1));
}

static void M_Maps_Key (int key)
{
	int	visible = M_Maps_VisibleRows ();

	/* Search first.  M_Filter_HandleKey claims only printable ASCII and
	 * backspace, so escape, enter, the arrows and the gamepad codes all fall
	 * through to navigation. */
	if (M_Filter_HandleKey (key))
	{
		M_Maps_FilterChanged ();
		return;
	}

	switch (key)
	{
	case K_ESCAPE:
	case K_GP_B:
		/* first press drops the filter, second leaves the menu */
		if (M_Filter_Active ())
		{
			S_LocalSound ("raven/menu2.wav");
			M_Filter_Clear ();
			M_Maps_FilterChanged ();
			break;
		}
		M_Menu_Main_f ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		if (maps_view_count > 0)
			maps_cursor = (maps_cursor + 1) % maps_view_count;
		M_Maps_EnsureVisible ();
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		if (maps_view_count > 0)
			maps_cursor = (maps_cursor + maps_view_count - 1) % maps_view_count;
		M_Maps_EnsureVisible ();
		break;

	case K_PGUP:
		S_LocalSound ("raven/menu1.wav");
		maps_cursor -= visible;
		M_Maps_ClampCursor ();
		break;

	case K_PGDN:
		S_LocalSound ("raven/menu1.wav");
		maps_cursor += visible;
		M_Maps_ClampCursor ();
		break;

	case K_HOME:
		S_LocalSound ("raven/menu1.wav");
		maps_cursor = 0;
		M_Maps_ClampCursor ();
		break;

	case K_END:
		S_LocalSound ("raven/menu1.wav");
		maps_cursor = maps_view_count - 1;
		M_Maps_ClampCursor ();
		break;

	case K_ENTER:
	case K_GP_A:
		if (maps_view_count <= 0)
			break;
		m_entersound = true;
		Key_SetDest (key_game);
		m_state = m_none;
		M_Filter_Clear ();
		Cbuf_AddText (va("map %s\n", maps_list[maps_view[maps_cursor]].name));
		break;
	}
}

static void M_Search_Draw (void)
{
	int x;

	ScrollTitle("gfx/menu/title4.lmp");

	x = (320/2) - ((12*8)/2) + 4;
	M_DrawTextBox (x-8, 60, 12, 1);
	M_Print (x, 68, "Searching...");

	if (slistInProgress)
	{
		NET_Poll();
		return;
	}

	if (! searchComplete)
	{
		searchComplete = true;
		searchCompleteTime = realtime;
	}

	if (hostCacheCount)
	{
		M_Menu_ServerList_f ();
		return;
	}

	M_PrintWhite ((320/2) - ((22*8)/2), 92, "No Hexen II servers found");
	if ((realtime - searchCompleteTime) < 3.0)
		return;

	M_Menu_LanConfig_f ();
}


static void M_Search_Key (int key)
{
}

/* -------------------------------------------------------------------------
 * ui_search_timeout -- expire the type-ahead filter buffer.
 *
 * Ironwail runs the same countdown in M_List_Update (Quake/menu.c:880): the
 * type-ahead buffer is a jump aid, not a mode, so it lapses a fixed time after
 * the last keystroke rather than holding until dismissed.  0 disables expiry
 * and restores the pre-a5nn.14 behaviour, where the filter held until ESC.
 *
 * Called once per frame from M_Draw ahead of the per-menu draw, so no menu can
 * draw a filtered view whose buffer has already lapsed.
 *
 * The mods and maps menus index a filtered *view* array, so dropping the filter
 * renumbers every row beneath the cursor.  Their ESC path accepts that and
 * resets to the top -- the user asked for it.  A timeout did not, so here the
 * selection is carried across the rebuild by absolute list index and only falls
 * back to the top when the selected entry is gone.
 * ------------------------------------------------------------------------- */
static void M_Filter_Think (void)
{
	int	sel, i;

	if (m_search_len <= 0)
		return;
	if (ui_search_timeout.value <= 0)
		return;
	if (m_search_time > realtime)
		m_search_time = realtime;	/* realtime restarted; re-arm */
	if (realtime - m_search_time < ui_search_timeout.value)
		return;

	switch (m_state)
	{
	case m_maps:
		sel = (maps_cursor >= 0 && maps_cursor < maps_view_count) ?
				maps_view[maps_cursor] : -1;
		M_Filter_Clear ();
		M_Maps_BuildView ();
		maps_cursor = 0;
		for (i = 0; i < maps_view_count; i++)
		{
			if (maps_view[i] == sel)
			{
				maps_cursor = i;
				break;
			}
		}
		M_Maps_ClampCursor ();
		M_Maps_EnsureVisible ();
		break;

	case m_mods:
		sel = (mods_cursor >= 0 && mods_cursor < mods_view_count) ?
				mods_view[mods_cursor] : -1;
		M_Filter_Clear ();
		M_Mods_BuildView ();
		mods_cursor = 0;
		for (i = 0; i < mods_view_count; i++)
		{
			if (mods_view[i] == sel)
			{
				mods_cursor = i;
				break;
			}
		}
		M_Mods_SnapSelectable (1);
		M_Mods_EnsureVisible ();
		break;

	default:
		/* Label-filtered submenus keep absolute row indices whether or
		 * not a row is hidden, so the cursor stays on the same setting
		 * and only the neighbours reappear. */
		M_Filter_Clear ();
		break;
	}
}

//=============================================================================
/* SLIST MENU */

static int		slist_cursor;
static qboolean		slist_sorted;

static void M_Menu_ServerList_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_slist;
	m_entersound = true;
	slist_cursor = 0;
	m_return_onerror = false;
	m_return_reason[0] = 0;
	slist_sorted = false;
}


static void M_ServerList_Draw (void)
{
	int	n;

	if (!slist_sorted)
	{
		slist_sorted = true;
		NET_SlistSort ();
	}

	ScrollTitle("gfx/menu/title4.lmp");
	for (n = 0; n < hostCacheCount; n++)
		M_Print (16, 60 + 8*n, NET_SlistPrintServer (n));

	M_DrawCharacter (0, 60 + slist_cursor*8, 12+((int)(realtime*4)&1));

	if (*m_return_reason)
		M_PrintWhite (16, 176, m_return_reason);
}


static void M_ServerList_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_LanConfig_f ();
		break;

	case K_SPACE:
		M_Menu_Search_f ();
		break;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("raven/menu1.wav");
		slist_cursor--;
		if (slist_cursor < 0)
			slist_cursor = hostCacheCount - 1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("raven/menu1.wav");
		slist_cursor++;
		if (slist_cursor >= hostCacheCount)
			slist_cursor = 0;
		break;

	case K_ENTER:
		S_LocalSound ("raven/menu2.wav");
		m_return_state = m_state;
		m_return_onerror = true;
		slist_sorted = false;
		Key_SetDest (key_game);
		m_state = m_none;
		Cbuf_AddText ( va ("connect \"%s\"\n", NET_SlistPrintServerName(slist_cursor)) );
		break;

	default:
		break;
	}
}


//=============================================================================
/* Menu Subsystem */

void M_Init (void)
{
	char		*ptr;

	ptr = (char *) FS_LoadTempFile (BIGCHAR_WIDTH_FILE, NULL);
	if (ptr == NULL)
		M_BuildBigCharWidth();
	else
	{
		if (fs_filesize == (long) sizeof(BigCharWidth))
			memcpy (BigCharWidth, ptr, sizeof(BigCharWidth));
		else
		{
			Con_Printf ("Unexpected file size (%ld) for %s\n",
					fs_filesize, BIGCHAR_WIDTH_FILE);
			M_BuildBigCharWidth();
		}
	}

	M_BuildBindList ();

	Cvar_RegisterVariable (&ui_mouse);
	Cvar_RegisterVariable (&ui_live_preview);
	Cvar_RegisterVariable (&ui_mouse_sound);
	Cvar_RegisterVariable (&ui_sound_throttle);
	Cvar_RegisterVariable (&ui_search_timeout);

	Cmd_AddCommand ("togglemenu", M_ToggleMenu_f);

	Cmd_AddCommand ("menu_main", M_Menu_Main_f);
	Cmd_AddCommand ("menu_singleplayer", M_Menu_SinglePlayer_f);
	Cmd_AddCommand ("menu_load", M_Menu_Load_f);
	Cmd_AddCommand ("menu_save", M_Menu_Save_f);
	Cmd_AddCommand ("menu_multiplayer", M_Menu_MultiPlayer_f);
	Cmd_AddCommand ("menu_setup", M_Menu_Setup_f);
	Cmd_AddCommand ("menu_options", M_Menu_Options_f);
	Cmd_AddCommand ("menu_keys", M_Menu_Keys_f);
	Cmd_AddCommand ("menu_video", M_Menu_Video_f);
	Cmd_AddCommand ("help", M_Menu_Help_f);
	Cmd_AddCommand ("menu_mods", M_Menu_Mods_f);
	Cmd_AddCommand ("menu_maps", M_Menu_Maps_f);
	Cmd_AddCommand ("menu_quit", M_Menu_Quit_f);
	Cmd_AddCommand ("menu_class", M_Menu_Class2_f);

	memset (old_bgmtype, 0, sizeof(old_bgmtype));
	old_extmusic = -1;
}


void M_Draw (void)
{
	if (m_state == m_none || !(Key_GetDest() & key_menu))
		return;

	M_Filter_Think ();

	if (!m_recursiveDraw)
	{
		scr_copyeverything = 1;

		if (!cl.worldmodel || cls.signon != SIGNONS)
		{
			/* No demo or game running — paint the conback as the
			 * Hexen II splash backdrop. Dedicated backdrop draw
			 * (no version watermark, full opacity, full screen). */
			Draw_MenuBackdrop ();
		}
		else if (scr_menubgstyle.integer >= 1)
		{
			/* Demo/game world is behind — dim it with the amber fade.
			 *
			 * ui_live_preview: the Display submenus skip the fade so
			 * a setting is judged against a clean game view while it
			 * is being adjusted.  This is the same trade Ironwail's
			 * cvar makes (Quake/menu.c:3575) — it fades the *menu*
			 * out over the live scene there, and holds the scene
			 * undimmed here — legibility of the menu against
			 * visibility of the thing being changed.  Turning it off
			 * dims these submenus like every other menu. */
			if (!ui_live_preview.integer
			    || (m_state != m_display && m_state != m_video
			        && m_state != m_rendering && m_state != m_graphics))
				Draw_FadeScreen ();

			/* Mode 2: also draw a translucent backdrop quad over the
			 * typical menu-item area (CANVAS_MENU is 320x200 — most
			 * menus draw items in roughly y=80..168). Ironwail parity. */
			if (scr_menubgstyle.integer >= 2)
			{
				const int bg_x0 = 32, bg_y0 = 80;
				const int bg_x1 = 288, bg_y1 = 168;
				GL_SetCanvas (CANVAS_MENU);
				/* Scaled by the same cvar as the fade behind it,
				 * or the box would stay solid while the world
				 * behind it brightened. */
				Draw_FillAlpha (bg_x0, bg_y0,
					bg_x1 - bg_x0, bg_y1 - bg_y0,
					0.0f, 0.0f, 0.0f,
					0.5f * CLAMP(0.0f, scr_menubgalpha.value, 1.0f));
			}
		}
		if (scr_viewsize.integer < 110)
			scr_fullupdate = 0;
	}
	else
	{
		m_recursiveDraw = false;
	}

	/* All menu draws happen inside the 320x200 CANVAS_MENU. Backgrounds
	 * (fade, console) above this line stay in CANVAS_DEFAULT so they
	 * fill the whole screen. */
	GL_SetCanvas (CANVAS_MENU);
	m_canvas_active = true;

	switch (m_state)
	{
	case m_none:
		break;

	case m_main:
		M_Main_Draw ();
		break;

	case m_singleplayer:
		M_SinglePlayer_Draw ();
		break;

	case m_difficulty:
		M_Difficulty_Draw ();
		break;

	case m_class:
		M_Class_Draw ();
		break;

	case m_load:
	case m_mload:
		M_Load_Draw ();
		break;

	case m_save:
	case m_msave:
		M_Save_Draw ();
		break;

	case m_multiplayer:
		M_MultiPlayer_Draw ();
		break;

	case m_setup:
		M_Setup_Draw ();
		break;

	case m_net:
		M_Net_Draw ();
		break;

	case m_options:
		M_Options_Draw ();
		break;

	case m_display:
		M_Display_Draw ();
		break;
	case m_rendering:
		M_Rendering_Draw ();
		break;
	case m_graphics:
		M_Graphics_Draw ();
		break;
	case m_sound:
		M_Sound_Draw ();
		break;
	case m_game:
		M_Game_Draw ();
		break;

	case m_keys:
		M_Keys_Draw ();
		break;

	case m_video:
		M_Video_Draw ();
		break;
	case m_gamepad:
		M_Gamepad_Draw ();
		break;

	case m_mods:
		M_Mods_Draw ();
		break;

	case m_maps:
		M_Maps_Draw ();
		break;

	case m_help:
		/* Help pics are full-screen sized — render in CANVAS_DEFAULT,
		 * then put the menu canvas back so the post-switch teardown
		 * matches state. */
		m_canvas_active = false;
		GL_SetCanvas (CANVAS_DEFAULT);
		M_Help_Draw ();
		GL_SetCanvas (CANVAS_MENU);
		m_canvas_active = true;
		break;

	case m_quit:
		M_Quit_Draw ();
		break;

	case m_lanconfig:
		M_LanConfig_Draw ();
		break;

	case m_gameoptions:
		M_GameOptions_Draw ();
		break;

	case m_search:
		M_Search_Draw ();
		break;

	case m_slist:
		M_ServerList_Draw ();
		break;
	}

	/* Consume the motion flag now that every hover test for this frame has
	 * run.  Held rather than cleared at the event, because input is
	 * processed before the draw and clearing it there would mean no hover
	 * test ever saw it.  uhexen2-u4iz. */
	menu_mouse_moved = false;

	m_canvas_active = false;
	GL_SetCanvas (CANVAS_DEFAULT);

	/* Restore default character alpha in case live preview reduced it. */
	Draw_SetCharacterAlpha (1.0f);

	if (m_entersound)
	{
		S_LocalSound ("raven/menu2.wav");
		m_entersound = false;
	}

	VID_UnlockBuffer ();
	S_ExtraUpdate ();
	VID_LockBuffer ();
}


/*
================
M_Keys_NoteConflict

Called just before a bind is issued.  If `key` already carries a different
command in the table this bind will write to, remember what is about to lose
it, preferring the bindlist's human label over the raw command string.

Same-command rebinds are not conflicts: Hexen II allows two keys per action,
so pressing a key that already runs this action is a no-op worth staying
quiet about.
================
*/
static void M_Keys_NoteConflict (int key, const char *command)
{
	const char	*prev;
	const char	*label;
	int		i;

	keys_conflict_msg[0] = '\0';

	/* The caller hands us whatever keycode arrived, and the gamepad alt
	 * remap above shifts it; bound before indexing either table. */
	if (key < 0 || key >= MAX_KEYS)
		return;

	prev = keys_tap ? doublebindings[key] : keybindings[key];
	if (!prev || !*prev)
		return;
	if (!strcmp (prev, command))
		return;

	label = prev;
	for (i = 0; i < keys_numcommands; i++)
	{
		if (!strcmp (keys_bindlist[i][0], prev))
		{
			label = keys_bindlist[i][1];
			break;
		}
	}

	q_snprintf (keys_conflict_msg, sizeof(keys_conflict_msg), "%s taken from %s",
		    Key_KeynumToDisplayString (key), label);
	keys_conflict_time = realtime;
}

void M_Keybind (int key)
{
	char	cmd[80];
	const char *command = keys_bindlist[keys_cursor][0];
	S_LocalSound ("raven/menu1.wav");
	if (key != K_ESCAPE && key != '`')
	{
		/* Gamepad alt-modifier second layer (Ironwail 7a2038a):
		 * unless we're binding the modifier command itself, ignore a press
		 * of the modifier key (so the user can hold it without binding it),
		 * and redirect a base gamepad button to its _ALT variant while the
		 * modifier is held. */
		if (strcmp (command, "+altmodifier") != 0)
		{
			if (Key_IsGamepadAltModifier (key))
				return;		/* stay in bind mode, wait for the real button */
			if (Key_GetGamepadAltModifierState () &&
			    key >= K_GP_A && key <= K_GP_START)
				key += K_GP_ALT_OFFSET;
		}
		/* After the alt-modifier remap above, so the reported key is the
		 * one actually being written. */
		M_Keys_NoteConflict (key, command);

		q_snprintf (cmd, sizeof(cmd), "%s \"%s\" \"%s\"\n",
			    keys_tap ? "bind2" : "bind",
			    Key_KeynumToString (key), command);
		Cbuf_InsertText (cmd);
	}

	Key_SetDest (key_menu);
}

void M_Keydown (int key, qboolean repeat)
{
	/* Mouse support: click=Enter, wheel=Up/Down */
	if (key == K_MOUSE1)
		key = K_ENTER;
	else if (key == K_MWHEELUP)
		key = K_UPARROW;
	else if (key == K_MWHEELDOWN)
		key = K_DOWNARROW;
	/* Gamepad menu navigation: A=Enter, B=Escape (Western/Xbox/Steam Deck
	 * convention). Lets a gamepad-only player traverse menus without a
	 * keyboard. uhexen2-x552. */
	else if (key == K_GP_A)
		key = K_ENTER;
	else if (key == K_GP_B)
		key = K_ESCAPE;
	/* D-pad → arrows so menu nav works alongside the new K_GP_DPAD_*
	 * keycodes that exist for in-game bindings. uhexen2-x552. */
	else if (key == K_GP_DPAD_UP || key == K_GP_DPAD_DOWN ||
		 key == K_GP_DPAD_LEFT || key == K_GP_DPAD_RIGHT)
	{
		/* Debounce repeats of the same direction.  A d-pad press should move
		 * the cursor exactly one item, but on a Steam Deck one press was
		 * moving two — the device filter added in in_sdl.c for uhexen2-flj2
		 * covers the duplicate-device case, and this covers any other source
		 * of a doubled press (Steam Input's own d-pad auto-repeat, say) that
		 * we cannot see from here.  The window is deliberately short: it is
		 * far longer than the milliseconds a duplicate takes to arrive, and
		 * far shorter than a deliberate second tap, so navigation stays
		 * responsive.  Only menu navigation is affected — in-game d-pad
		 * bindings still see every K_GP_DPAD_* event. */
		static double	dpad_next_ok;
		static int	dpad_last;
		const double	DPAD_MENU_DEBOUNCE = 0.1;

		if (key == dpad_last && realtime < dpad_next_ok)
			return;
		dpad_last = key;
		dpad_next_ok = realtime + DPAD_MENU_DEBOUNCE;

		if (key == K_GP_DPAD_UP)
			key = K_UPARROW;
		else if (key == K_GP_DPAD_DOWN)
			key = K_DOWNARROW;
		else if (key == K_GP_DPAD_LEFT)
			key = K_LEFTARROW;
		else
			key = K_RIGHTARROW;
	}

	/* Suppress key repeat for everything except navigation/escape */
	if (repeat)
	{
		switch (key)
		{
		case K_UPARROW: case K_DOWNARROW:
		case K_LEFTARROW: case K_RIGHTARROW:
		case K_ESCAPE:
			break;
		default:
			return;
		}
	}

	switch (m_state)
	{
	case m_none:
		return;

	case m_main:
		M_Main_Key (key);
		return;

	case m_singleplayer:
		M_SinglePlayer_Key (key);
		return;

	case m_difficulty:
		M_Difficulty_Key (key);
		return;

	case m_class:
		M_Class_Key (key);
		return;

	case m_load:
		M_Load_Key (key);
		return;

	case m_save:
		M_Save_Key (key);
		return;

	case m_mload:
		M_MLoad_Key (key);
		return;

	case m_msave:
		M_MSave_Key (key);
		return;

	case m_multiplayer:
		M_MultiPlayer_Key (key);
		return;

	case m_setup:
		M_Setup_Key (key);
		return;

	case m_net:
		M_Net_Key (key);
		return;

	case m_options:
		M_Options_Key (key);
		return;

	case m_display:
		M_Display_Key (key);
		return;
	case m_rendering:
		M_Rendering_Key (key);
		return;
	case m_graphics:
		M_Graphics_Key (key);
		return;
	case m_sound:
		M_Sound_Key (key);
		return;
	case m_game:
		M_Game_Key (key);
		return;

	case m_keys:
		M_Keys_Key (key);
		return;

	case m_video:
		M_Video_Key (key);
		return;

	case m_gamepad:
		M_Gamepad_Key (key);
		return;

	case m_mods:
		M_Mods_Key (key);
		return;

	case m_maps:
		M_Maps_Key (key);
		return;

	case m_help:
		M_Help_Key (key);
		return;

	case m_quit:
		M_Quit_Key (key);
		return;

	case m_lanconfig:
		M_LanConfig_Key (key);
		return;

	case m_gameoptions:
		M_GameOptions_Key (key);
		return;

	case m_search:
		M_Search_Key (key);
		break;

	case m_slist:
		M_ServerList_Key (key);
		return;
	}
}


static void M_ConfigureNetSubsystem(void)
{
// enable/disable net systems to match desired config

	Cbuf_AddText ("stopdemo\n");

	if (TCPIPConfig)
		net_hostport = lanConfig_port;
}


static void BGM_RestartMusic (void)
{
	// called after exitting the menus and changing the music type
	// this is pretty crude, but doen't seem to break anything S.A

	if (q_strcasecmp(bgmtype.string,"midi") == 0)
	{
		CDAudio_Stop();
		BGM_PlayMIDIorMusic(cl.midi_name);
	}
	else if (q_strcasecmp(bgmtype.string,"cd") == 0)
	{
		BGM_Stop();
		BGM_PlayCDtrack ((byte)cl.cdtrack, true);
	}
	else
	{
		CDAudio_Stop();
		BGM_Stop();
	}
}

