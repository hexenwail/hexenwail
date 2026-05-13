/* console.c -- in-game console and chat message buffer handling
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
#include "debuglog.h"
#include "sdl_inc.h"


console_t	*con;

qboolean	con_initialized;

int	con_linewidth;		// characters across screen
static int	con_vislines;
int		con_notifylines;	// scan lines to clear for notify lines
int		con_totallines;		// total lines in console scrollback
static float	con_cursorspeed = 4;
qboolean 	con_forcedup;		// because no entities to refresh
int		con_ormask;

static conselection_t	con_selection;
static conmousestate_t	con_mouse_state;
static int		con_mouse_x, con_mouse_y;
static conofs_t		con_press_ofs;
qboolean		con_mouse_button_down;

typedef struct {
	conofs_t begin;
	conofs_t end;
	char url[256];
} con_url_t;

#define MAX_CON_URLS 64
static con_url_t con_urls[MAX_CON_URLS];
static int con_url_count = 0;
static int con_hover_url = -1;

static	cvar_t	con_notifytime = {"con_notifytime", "3", CVAR_NONE};	//seconds
static	cvar_t	con_notifycenter = {"con_notifycenter", "0", CVAR_ARCHIVE};	/* center notify text horizontally */
static	cvar_t	con_notifyfade = {"con_notifyfade", "1", CVAR_ARCHIVE};	/* fade notify lines over the last second instead of hard-cutting (Ironwail parity) */
/* Optional cap on console line width.  0 = no cap (use full screen).
 * At 4K the natural width is ~238 cols, which is unreadable; this lets
 * the user pin a saner column count (e.g. 80, 100, 120) regardless of
 * display resolution.  Externally visible so the menu can surface it. */
cvar_t	con_maxcols = {"con_maxcols", "0", CVAR_ARCHIVE};

#define	NUM_CON_TIMES 4
static float	con_times[NUM_CON_TIMES];	// realtime time the line was generated
						// for transparent notify lines
static qboolean	con_suppress_notify;		// when true, Con_Print skips notify timestamps

extern qboolean		menu_disabled_mouse;


static void Key_ClearTyping (void)
{
	key_lines[edit_line][1] = 0;	// clear any typing
	key_linepos = 1;
}

/*
================
Con_ToggleConsole_f
================
*/
void Con_ToggleConsole_f (void)
{
	keydest_t dest = Key_GetDest();

	// activate mouse when in console in
	// case it is disabled somewhere else
	menu_disabled_mouse = false;
	IN_ActivateMouse ();

	Key_ClearTyping ();

	if (dest == key_console || (dest == key_game && con_forcedup))
	{
		Con_ClearSelection ();
		if (cls.state == ca_active)
			Key_SetDest (key_game);
		else
			M_Menu_Main_f ();
	}
	else
	{
		Key_SetDest (key_console);
	}

	SCR_EndLoadingPlaque ();
	Con_ClearNotify ();
}

/*
================
Con_Clear_f
================
*/
static void Con_Clear_f (void)
{
	int	i;
	for (i = 0; i < CON_TEXTSIZE; i++)
		con->text[i] = ' ';
}


/*
================
Con_ClearNotify
================
*/
void Con_ClearNotify (void)
{
	int		i;

	for (i = 0; i < NUM_CON_TIMES; i++)
		con_times[i] = 0;
}


/*
================
Con_MessageMode_f
================
*/
static void Con_MessageMode_f (void)
{
	if (cls.state != ca_active || cls.demoplayback)
		return;
	chat_team = false;
	Key_SetDest (key_message);
}

/*
================
Con_MessageMode2_f
================
*/
static void Con_MessageMode2_f (void)
{
	if (cls.state != ca_active || cls.demoplayback)
		return;
	chat_team = true;
	Key_SetDest (key_message);
}


/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
void Con_CheckResize (void)
{
	int	i, j, width, oldwidth, oldtotallines, numlines, numchars;
	short	tbuf[CON_TEXTSIZE];

	width = (vid.width >> 3) - 2;

	/* User-imposed column cap (con_maxcols).  Clamp to >= 38 to keep
	 * the same minimum the uninitialized-video fallback uses below. */
	if (con_maxcols.integer > 0 && width > con_maxcols.integer)
		width = con_maxcols.integer;
	if (width < 38)
		width = 38;

	if (width == con_linewidth)
		return;

	if (width < 1)			// video hasn't been initialized yet
	{
		width = 38;
		con_linewidth = width;
		con_totallines = CON_TEXTSIZE / con_linewidth;
		Con_Clear_f();
	}
	else
	{
		oldwidth = con_linewidth;
		con_linewidth = width;
		oldtotallines = con_totallines;
		con_totallines = CON_TEXTSIZE / con_linewidth;
		numlines = oldtotallines;

		if (con_totallines < numlines)
			numlines = con_totallines;

		numchars = oldwidth;

		if (con_linewidth < numchars)
			numchars = con_linewidth;

		memcpy (tbuf, con->text, CON_TEXTSIZE*sizeof(short));
		Con_Clear_f();

		for (i = 0; i < numlines; i++)
		{
			for (j = 0; j < numchars; j++)
			{
				con->text[(con_totallines - 1 - i) * con_linewidth + j] =
						tbuf[((con->current - i + oldtotallines) % oldtotallines) * oldwidth + j];
			}
		}

		Con_ClearNotify ();
	}

	con->current = con_totallines - 1;
	con->display = con->current;
}


/*
================
Con_Init
================
*/
void Con_Init (void)
{
	con = (console_t *) Hunk_AllocName (sizeof(console_t), "con_main");
	con_linewidth = -1;
	Con_CheckResize ();

	Con_Printf ("Console initialized.\n");

	Cvar_RegisterVariable (&con_notifytime);
	Cvar_RegisterVariable (&con_notifycenter);
	Cvar_RegisterVariable (&con_notifyfade);
	Cvar_RegisterVariable (&con_maxcols);

	Cmd_AddCommand ("toggleconsole", Con_ToggleConsole_f);
	Cmd_AddCommand ("messagemode", Con_MessageMode_f);
	Cmd_AddCommand ("messagemode2", Con_MessageMode2_f);
	Cmd_AddCommand ("clear", Con_Clear_f);

	con_initialized = true;
}


/*
===============
Con_Linefeed
===============
*/
static void Con_Linefeed (void)
{
	int	i, j;

	con->x = 0;
	if (con->display == con->current)
		con->display++;
	con->current++;
	j = (con->current%con_totallines) * con_linewidth;
	for (i = 0; i < con_linewidth; i++)
		con->text[i+j] = ' ';
}

/*
================
Con_Print

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be logged to disk
If no console is visible, the notify window will pop up.
================
*/
static void Con_Print (const char *txt)
{
	int		y;
	int		c, l;
	static int	cr;
	int		mask;
	qboolean	boundary;

	if (txt[0] == 1)
	{
		mask = 256;		// go to colored text
		S_LocalSound ("misc/comm.wav");	// play talk wav
		txt++;
	}
	else if (txt[0] == 2)
	{
		mask = 256;		// go to colored text
		txt++;
	}
	else
		mask = 0;

	boundary = true;

	while ( (c = (byte)*txt) )
	{
		if (c <= ' ')
		{
			boundary = true;
		}
		else if (boundary)
		{
			// count word length
			for (l = 0; l < con_linewidth; l++)
				if (txt[l] <= ' ')
					break;

			// word wrap
			if (l != con_linewidth && (con->x + l > con_linewidth))
				con->x = 0;

			boundary = false;
		}

		txt++;

		if (cr)
		{
			con->current--;
			cr = false;
		}

		if (!con->x)
		{
			Con_Linefeed ();
		// mark time for transparent overlay
			if (con->current >= 0 && !con_suppress_notify)
				con_times[con->current % NUM_CON_TIMES] = realtime;
		}

		switch (c)
		{
		case '\n':
			con->x = 0;
			break;

		case '\r':
			con->x = 0;
			cr = 1;
			break;

		default:	// display character and advance
			y = con->current % con_totallines;
			con->text[y*con_linewidth+con->x] = c | mask | con_ormask;
			con->x++;
			if (con->x >= con_linewidth)
				con->x = 0;
			break;
		}
	}
}


/*
================
CON_Printf
Prepare the message to be printed and
send it to the proper handlers.
================
*/
void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAX_PRINTMSG];
	static qboolean	inupdate;

	if (flags & _PRINT_DEVEL && !developer.integer)
	{
		if (con_debuglog & LOG_DEVEL)	/* full logging */
		{
			va_start (argptr, fmt);
			q_vsnprintf (msg, sizeof(msg), fmt, argptr);
			va_end (argptr);
			LOG_Print (msg);
		}
		return;
	}

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Sys_PrintTerm (msg);	// echo to the terminal
	if (con_debuglog)
		LOG_Print (msg);

	if (flags & _PRINT_TERMONLY || !con_initialized)
		return;

	if (cls.state == ca_dedicated)
		return;		// no graphics mode

// write it to the scrollable buffer
	if (flags & _PRINT_NONOTIFY)
		con_suppress_notify = true;
	Con_Print (msg);
	con_suppress_notify = false;

	if (flags & _PRINT_SAFE)
		return;	// safe: doesn't update the screen

// update the screen immediately if the console is displayed
	if (cls.signon != SIGNONS && !scr_disabled_for_loading )
	{
	// protect against infinite loop if SCR_UpdateScreen
	// itself calls Con_Printf
		if (!inupdate)
		{
			inupdate = true;
			SCR_UpdateScreen ();
			inupdate = false;
		}
	}
}


/*
==================
Con_ShowList

Tyrann's ShowList ported by S.A.:
Prints a given list to the console with columnized formatting
==================
*/
void Con_ShowList (int cnt, const char **list)
{
	const char	*s;
	char		*line;
	int	i, j, max_len, len, cols, rows;

	// Lay them out in columns
	max_len = 0;
	for (i = 0; i < cnt; ++i)
	{
		len = (int) strlen(list[i]);
		if (len > max_len)
			max_len = len;
	}

	line = (char *) Z_Malloc(con_linewidth + 1, Z_MAINZONE);
	cols = con_linewidth / (max_len + 2);
	rows = cnt / cols + 1;

	// Looks better if we have a few rows before spreading out
	if (rows < 5)
	{
		cols = cnt / 5 + 1;
		rows = cnt / cols + 1;
	}

	for (i = 0; i < rows; ++i)
	{
		line[0] = '\0';
		for (j = 0; j < cols; ++j)
		{
			if (j * rows + i >= cnt)
				break;
			s = list[j * rows + i];
			len = (int) strlen(s);

			q_strlcat(line, s, con_linewidth+1);
			if (j < cols - 1)
			{
				while (len < max_len)
				{
					q_strlcat(line, " ", con_linewidth+1);
					len++;
				}
				q_strlcat(line, "  ", con_linewidth+1);
			}
		}

		if (line[0] != '\0')
			Con_Printf("%s\n", line);
	}

	Z_Free(line);
}


/*
==============================================================================

DRAWING

==============================================================================
*/


/*
================
Con_DrawInput

The input line scrolls horizontally if typing goes beyond the right edge
================
*/
static void Con_DrawInput (void)
{
	int		i, y;
	size_t		pos;
	char	editlinecopy[MAXCMDLINE], *text;

	if (Key_GetDest() != key_console && !con_forcedup)
		return;		// don't draw anything

	pos = q_strlcpy(editlinecopy, key_lines[edit_line], sizeof(editlinecopy));
	text = editlinecopy;

// fill out remainder with spaces
	for ( ; pos < MAXCMDLINE; ++pos)
		text[pos] = ' ';

// add the cursor frame
	if ((int)(realtime * con_cursorspeed) & 1)	// cursor is visible
		text[key_linepos] = (key_insert) ? 11 : 95; // underscore for overwrite mode, square for insert

//	prestep if horizontally scrolling
	if (key_linepos >= con_linewidth)
		text += 1 + key_linepos - con_linewidth;

// draw it
	y = con_vislines - 22;
	for (i = 0; i < con_linewidth; i++)
		Draw_Character ((i + 1)<<3, y, text[i]);
}


/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
void Con_DrawNotify (void)
{
	int	i, x, v;
	const short	*text;
	float	time;

	v = 0;
	for (i = con->current-NUM_CON_TIMES+1; i <= con->current; i++)
	{
		float	alpha;

		if (i < 0)
			continue;
		time = con_times[i % NUM_CON_TIMES];
		if (time == 0)
			continue;
		time = realtime - time;
		if (time > con_notifytime.value)
			continue;
		text = con->text + (i % con_totallines)*con_linewidth;

		if (scr_viewsize.integer < 100)
			clearnotify = 0;
		scr_copytop = 1;

		/* fade out over the last second; cvar 0 disables (Ironwail parity) */
		alpha = 1.0f;
		if (con_notifyfade.integer)
		{
			float	remaining = con_notifytime.value - time;
			if (remaining < 1.0f)
				alpha = remaining < 0.0f ? 0.0f : remaining;
		}
		Draw_SetCharacterAlpha (alpha);

		if (con_notifycenter.integer)
		{
			/* find actual text length (trim trailing spaces) */
			int len = con_linewidth;
			while (len > 0 && (text[len-1] & 0xFF) == ' ')
				--len;
			for (x = 0; x < len; x++)
				Draw_Character ((vid.width - len*8)/2 + x*8, v, text[x]);
		}
		else
		{
			for (x = 0; x < con_linewidth; x++)
				Draw_Character ((x+1)<<3, v, text[x]);
		}

		v += 8;
	}
	Draw_SetCharacterAlpha (1.0f);

	if (Key_GetDest() == key_message)
	{
		const char	*s;

		if (scr_viewsize.integer < 100)
			clearnotify = 0;
		scr_copytop = 1;

		if (chat_team)
		{
			Draw_String (8, v, "say_team:");
			x = 11;
		}
		else
		{
			Draw_String (8, v, "say:");
			x = 6;
		}

		s = Key_GetChatBuffer();
		i = Key_GetChatMsgLen();
		if (i > (vid.width>>3) - x - 1)
			s += i - (vid.width>>3) + x + 1;

		while (*s)
		{
			Draw_Character (x<<3, v, *s);
			s++;
			x++;
		}

		Draw_Character (x<<3, v, 10 + ((int)(realtime*con_cursorspeed)&1));
		v += 8;
	}

	if (v > con_notifylines)
		con_notifylines = v;
}

void Con_MouseMove (int x, int y)
{
	con_mouse_x = x;
	con_mouse_y = y;
}

void Con_ClearSelection (void)
{
	con_selection.active = false;
	con_selection.begin.line = 0;
	con_selection.begin.col = 0;
	con_selection.end.line = 0;
	con_selection.end.col = 0;
}

void Con_SelectAll (void)
{
	con_selection.begin.line = 0;
	con_selection.begin.col = 0;
	con_selection.end.line = con->current;
	con_selection.end.col = con_linewidth - 1;
	con_selection.active = true;
}

static qboolean Con_IsURLChar (char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
	       c == '~' || c == ':' || c == '/' || c == '?' || c == '#' ||
	       c == '[' || c == ']' || c == '@' || c == '!' || c == '$' ||
	       c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' ||
	       c == '+' || c == ',' || c == ';' || c == '=' || c == '%';
}

static int Con_ExtractURL (int start_line, int start_col,
                           char *url_out, int url_len, conofs_t *end_ofs)
{
	int pos = 0, line = start_line, col = start_col;
	url_out[0] = '\0';

	while (pos < url_len - 1 && line <= con->current)
	{
		if (con->current - line > con_totallines)
			break;

		short *text_row = con->text + (line % con_totallines) * con_linewidth;

		while (col < con_linewidth && pos < url_len - 1)
		{
			char c = text_row[col] & 0xFF;
			if (!Con_IsURLChar(c))
				break;
			url_out[pos++] = c;
			col++;
		}

		if (col < con_linewidth)
			break;

		line++;
		col = 0;
	}

	url_out[pos] = '\0';
	if (end_ofs)
	{
		end_ofs->line = line;
		end_ofs->col = col;
	}
	return pos;
}

static void Con_FindURLs (void)
{
	int line, col;
	con_url_count = 0;

	for (line = 0; line <= con->current && con_url_count < MAX_CON_URLS; line++)
	{
		if (con->current - line > con_totallines)
			continue;

		short *text_row = con->text + (line % con_totallines) * con_linewidth;

		for (col = 0; col < con_linewidth - 7 && con_url_count < MAX_CON_URLS; col++)
		{
			char c0 = text_row[col] & 0xFF;
			char c1 = text_row[col+1] & 0xFF;
			char c2 = text_row[col+2] & 0xFF;
			char c3 = text_row[col+3] & 0xFF;

			qboolean is_http = (c0 == 'h' && c1 == 't' && c2 == 't' && c3 == 'p');
			qboolean is_ftp = (c0 == 'f' && c1 == 't' && c2 == 'p');

			int scheme_len = 0;
			if (is_http)
			{
				if (col + 7 < con_linewidth)
				{
					char c4 = text_row[col+4] & 0xFF;
					char c5 = text_row[col+5] & 0xFF;
					char c6 = text_row[col+6] & 0xFF;

					if (c4 == ':' && c5 == '/' && c6 == '/')
						scheme_len = 7;
					else if (col + 8 < con_linewidth)
					{
						char c7 = text_row[col+7] & 0xFF;
						if (c4 == 's' && c5 == ':' && c6 == '/' && c7 == '/')
							scheme_len = 8;
					}
				}
			}
			else if (is_ftp && col + 6 < con_linewidth)
			{
				char c3 = text_row[col+3] & 0xFF;
				char c4 = text_row[col+4] & 0xFF;
				char c5 = text_row[col+5] & 0xFF;

				if (c3 == ':' && c4 == '/' && c5 == '/')
					scheme_len = 6;
			}

			if (scheme_len > 0)
			{
				con_url_t *url = &con_urls[con_url_count];
				url->begin.line = line;
				url->begin.col = col;

				Con_ExtractURL(line, col, url->url, sizeof(url->url), &url->end);

				if (url->url[0] != '\0')
				{
					con_url_count++;
					col += strlen(url->url) - 1;
				}
			}
		}
	}
}

static int Con_FindURLAtOffset (conofs_t ofs)
{
	for (int i = 0; i < con_url_count; i++)
	{
		con_url_t *url = &con_urls[i];

		if (ofs.line == url->begin.line && ofs.col >= url->begin.col && ofs.col <= url->end.col)
			return i;
		if (ofs.line > url->begin.line && ofs.line < url->end.line)
			return i;
		if (ofs.line == url->end.line && ofs.col <= url->end.col && ofs.line > url->begin.line)
			return i;
	}
	return -1;
}

static qboolean Con_ScreenToOffset (int sx, int sy, conofs_t *ofs);

static qboolean Con_ScreenToOffset (int sx, int sy, conofs_t *ofs)
{
	int rows, y_top, y_bot, row_from_bottom;

	// Calculate console display area dimensions
	rows = (con_vislines - 22) >> 3;  // Exclude input line (~22 pixels)
	y_bot = con_vislines - 30;        // Bottom of text area
	y_top = y_bot - (rows - 1) * 8;   // Top of text area

	// Reject if outside vertical bounds
	if (sy < y_top || sy > y_bot + 8)
		return false;

	// Column from screen x (each char is 8 pixels wide)
	ofs->col = (sx >> 3) - 1;
	if (ofs->col < 0) ofs->col = 0;
	if (ofs->col >= con_linewidth) ofs->col = con_linewidth - 1;

	// Row from screen y (measured from bottom up)
	row_from_bottom = (y_bot - sy) >> 3;
	if (row_from_bottom < 0) row_from_bottom = 0;
	if (row_from_bottom >= rows) row_from_bottom = rows - 1;

	// Convert to console line number
	ofs->line = con->display - row_from_bottom;

	// Validate line is within ring buffer
	if (con->current - ofs->line >= con_totallines)
		return false;

	return true;
}

void Con_UpdateMouseState (void)
{
	conofs_t cur_ofs;
	qboolean on_text;
	int url_index;

	// Reset if not in console
	if (Key_GetDest() != key_console)
	{
		con_mouse_state = CMS_NOTPRESSED;
		con_mouse_button_down = false;
		con_hover_url = -1;
		VID_SetMouseCursor(MCURSOR_DEFAULT);
		return;
	}

	// Update URL cache
	Con_FindURLs();

	// Try to get current mouse position in console coordinates
	on_text = Con_ScreenToOffset(con_mouse_x, con_mouse_y, &cur_ofs);

	// Check if hovering over a URL
	url_index = -1;
	if (on_text)
		url_index = Con_FindURLAtOffset(cur_ofs);

	con_hover_url = url_index;

	// Set cursor shape based on position
	if (url_index >= 0)
		VID_SetMouseCursor(MCURSOR_HAND);
	else if (on_text)
		VID_SetMouseCursor(MCURSOR_IBEAM);
	else
		VID_SetMouseCursor(MCURSOR_DEFAULT);

	// State machine
	switch (con_mouse_state)
	{
	case CMS_NOTPRESSED:
		if (con_mouse_button_down && on_text)
		{
			// Check if clicking on a URL
			if (url_index >= 0)
			{
				SDL_OpenURL(con_urls[url_index].url);
				con_mouse_state = CMS_PRESSED;
			}
			else
			{
				// Button went down on text: transition to PRESSED
				con_press_ofs = cur_ofs;
				con_selection.active = false;
				con_mouse_state = CMS_PRESSED;
			}
		}
		break;

	case CMS_PRESSED:
		if (!con_mouse_button_down)
		{
			// Button released: go back to NOTPRESSED, keep selection
			con_mouse_state = CMS_NOTPRESSED;
		}
		else if (on_text &&
		    (cur_ofs.col != con_press_ofs.col || cur_ofs.line != con_press_ofs.line))
		{
			// Button held and moved: start drag
			con_selection.begin = con_press_ofs;
			con_selection.end = cur_ofs;
			con_selection.active = true;
			con_mouse_state = CMS_DRAGGING;
		}
		break;

	case CMS_DRAGGING:
		if (!con_mouse_button_down)
		{
			// Button released: go back to NOTPRESSED, keep selection
			con_mouse_state = CMS_NOTPRESSED;
		}
		else if (on_text)
		{
			// Button held: update selection end
			con_selection.end = cur_ofs;
			// Normalize begin/end if needed
			if (con_selection.begin.line > con_selection.end.line ||
			    (con_selection.begin.line == con_selection.end.line &&
			     con_selection.begin.col > con_selection.end.col))
			{
				conofs_t temp = con_selection.begin;
				con_selection.begin = con_selection.end;
				con_selection.end = temp;
			}
		}
		break;
	}
}

static void Con_DrawSelection (int lines)
{
	int y_bot, row_height, x1, x2, y, row_from_bottom;
	int line_offset, col_start, col_end;
	conofs_t b, e;

	if (!con_selection.active)
		return;

	// Setup dimensions
	y_bot = lines - 30;
	row_height = 8;

	// Get normalized begin/end
	b = con_selection.begin;
	e = con_selection.end;
	if (b.line > e.line || (b.line == e.line && b.col > e.col))
	{
		conofs_t temp = b;
		b = e;
		e = temp;
	}

	// Draw highlight for each visible row in selection
	for (int row = b.line; row <= e.line; row++)
	{
		// Check if row is visible
		row_from_bottom = con->display - row;
		if (row_from_bottom < 0 || row_from_bottom >= ((lines - 22) >> 3))
			continue;

		y = y_bot - (row_from_bottom << 3);
		if (y < 0 || y > lines - 30)
			continue;

		// Determine column range for this row
		if (row == b.line && row == e.line)
		{
			// Single-row selection
			col_start = b.col;
			col_end = e.col;
		}
		else if (row == b.line)
		{
			// First row: from begin.col to end of line
			col_start = b.col;
			col_end = con_linewidth - 1;
		}
		else if (row == e.line)
		{
			// Last row: from start of line to end.col
			col_start = 0;
			col_end = e.col;
		}
		else
		{
			// Middle rows: entire line
			col_start = 0;
			col_end = con_linewidth - 1;
		}

		// Draw highlight quad
		x1 = (col_start + 1) << 3;
		x2 = (col_end + 2) << 3;
		Draw_FillAlpha(x1, y, x2 - x1, row_height, 0.2f, 0.4f, 1.0f, 0.35f);
	}
}

qboolean Con_CopySelectionToClipboard (void)
{
	char buf[8192];
	int buf_pos = 0;
	int line, col, line_start, line_end;
	conofs_t b, e;

	if (!con_selection.active)
		return false;

	// Get normalized begin/end
	b = con_selection.begin;
	e = con_selection.end;
	if (b.line > e.line || (b.line == e.line && b.col > e.col))
	{
		conofs_t temp = b;
		b = e;
		e = temp;
	}

	// Build text from selection
	for (line = b.line; line <= e.line && buf_pos < (int)sizeof(buf) - 2; line++)
	{
		// Check if line is valid in ring buffer
		if (con->current - line >= con_totallines)
			continue;

		// Determine column range for this line
		if (line == b.line && line == e.line)
		{
			line_start = b.col;
			line_end = e.col;
		}
		else if (line == b.line)
		{
			line_start = b.col;
			line_end = con_linewidth - 1;
		}
		else if (line == e.line)
		{
			line_start = 0;
			line_end = e.col;
		}
		else
		{
			line_start = 0;
			line_end = con_linewidth - 1;
		}

		// Copy characters from this line
		short *text_row = con->text + (line % con_totallines) * con_linewidth;
		for (col = line_start; col <= line_end && buf_pos < (int)sizeof(buf) - 2; col++)
		{
			char c = text_row[col] & 0xFF;
			// Replace non-printable with space
			if (c < 32 && c != '\n')
				c = ' ';
			buf[buf_pos++] = c;
		}

		// Add newline between lines (but not after last line)
		if (line < e.line && buf_pos < (int)sizeof(buf) - 2)
			buf[buf_pos++] = '\n';
	}

	buf[buf_pos] = '\0';

	// Copy to clipboard
	SDL_SetClipboardText(buf);

	return true;
}

static void Con_DrawURLs (int lines)
{
	int y_bot, x1, x2, y, row_from_bottom;

	for (int i = 0; i < con_url_count; i++)
	{
		con_url_t *url = &con_urls[i];
		y_bot = lines - 30;

		// Draw underline for each row of the URL
		for (int row = url->begin.line; row <= url->end.line; row++)
		{
			row_from_bottom = con->display - row;
			if (row_from_bottom < 0 || row_from_bottom >= ((lines - 22) >> 3))
				continue;

			y = y_bot - (row_from_bottom << 3);
			if (y < 0 || y > lines - 30)
				continue;

			int col_start, col_end;
			if (row == url->begin.line && row == url->end.line)
			{
				col_start = url->begin.col;
				col_end = url->end.col;
			}
			else if (row == url->begin.line)
			{
				col_start = url->begin.col;
				col_end = con_linewidth - 1;
			}
			else if (row == url->end.line)
			{
				col_start = 0;
				col_end = url->end.col;
			}
			else
			{
				col_start = 0;
				col_end = con_linewidth - 1;
			}

			x1 = (col_start + 1) << 3;
			x2 = (col_end + 2) << 3;
			float alpha = (i == con_hover_url) ? 0.8f : 0.4f;
			Draw_FillAlpha(x1, y + 6, x2 - x1, 1, 0.2f, 0.6f, 1.0f, alpha);
		}
	}
}

/*
================
Con_DrawConsole

Draws the console with the solid background
================
*/
void Con_DrawConsole (int lines)
{
	int		i, x, y;
	int		row, rows;
	const short	*text;

	if (lines <= 0)
		return;

// draw the background
	Draw_ConsoleBackground (lines);

// draw the text
	con_vislines = lines;
	Con_UpdateMouseState();

// changed to line things up better
	rows = (lines-22)>>3;		// rows of text to draw

	y = lines - 30;

// draw from the bottom up
	if (con->display != con->current)
	{
	// draw arrows to show the buffer is backscrolled
		for (x = 0; x < con_linewidth; x += 4)
			Draw_Character ( (x+1)<<3, y, '^');

		y -= 8;
		rows--;
	}

	row = con->display;
	for (i = 0; i < rows; i++, y -= 8, row--)
	{
		if (row < 0)
			break;
		if (con->current - row >= con_totallines)
			break;		// past scrollback wrap point

		text = con->text + (row % con_totallines)*con_linewidth;

		for (x = 0; x < con_linewidth; x++)
			Draw_Character ( (x+1)<<3, y, text[x]);
	}

	Con_DrawURLs(lines);
	Con_DrawSelection(lines);

// draw the input prompt, user text, and cursor if desired
	Con_DrawInput ();
}


/*
==================
Con_NotifyBox
==================
*/
void Con_NotifyBox (const char *text)
{
	double		t1, t2;

// during startup for sound / cd warnings
	Con_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n");

	Con_Printf ("%s", text);

	Con_Printf ("Press a key.\n");
	Con_Printf("\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n");

	key_count = -2;		// wait for a key down and up
	Key_SetDest (key_console);

	do
	{
		t1 = Sys_DoubleTime ();
		SCR_UpdateScreen ();
		Sys_SendKeyEvents ();
		t2 = Sys_DoubleTime ();
		realtime += t2-t1;	// make the cursor blink
	} while (key_count < 0);

	Con_Printf ("\n");
	Key_SetDest (key_game);
	realtime = 0;		// put the cursor back to invisible
}

