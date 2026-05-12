/* console.h -- the game console
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

#ifndef __CONSOLE_H
#define __CONSOLE_H

#define CON_TEXTSIZE	16384
typedef struct
{
	short	text[CON_TEXTSIZE];
	int		current;		// line where next message will be printed
	int		x;			// offset in current line for next print
	int		display;		// bottom of console displays this line
} console_t;

typedef struct { int line; int col; } conofs_t;
typedef struct { conofs_t begin; conofs_t end; qboolean active; } conselection_t;
typedef enum { CMS_NOTPRESSED=0, CMS_PRESSED, CMS_DRAGGING } conmousestate_t;

extern	console_t	*con;
extern	int con_linewidth;

extern	int		con_ormask;

extern	int con_totallines;
extern	qboolean con_forcedup;	// because no entities to refresh
extern	qboolean con_initialized;
extern	byte *con_chars;
extern	int con_notifylines;	// scan lines to clear for notify lines

extern	cvar_t con_maxcols;	// 0 = no cap; else clamp console line width

void Con_DrawCharacter (int cx, int line, int num);

void Con_CheckResize (void);
void Con_Init (void);
void Con_DrawConsole (int lines);

void Con_ShowList (int , const char **);
void Con_DrawNotify (void);
void Con_ClearNotify (void);
void Con_ToggleConsole_f (void);

void Con_NotifyBox (const char *text);	// during startup for sound / cd warnings

void Con_MouseMove (int x, int y);
void Con_UpdateMouseState (void);
void Con_ClearSelection (void);
void Con_SelectAll (void);
qboolean Con_CopySelectionToClipboard (void);

extern qboolean con_mouse_button_down;

#endif	/* __CONSOLE_H */
