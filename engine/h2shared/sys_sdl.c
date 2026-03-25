/* sys_sdl.c -- Shared system functions using SDL3
 *
 * Platform-independent implementations of system functions.
 * Platform-specific code (file I/O, console input, entry points)
 * remains in sys_unix.c and sys_win.c.
 *
 * Copyright (C) 2008-2012  O.Sezer <sezero@users.sourceforge.net>
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "sdl_inc.h"

#include <time.h>

#define PAUSE_SLEEP		50	/* sleep time on pause or minimization		*/
#define NOT_FOCUS_SLEEP		20	/* sleep time when not focus			*/

/*
================
Sys_DoubleTime
================
*/
double Sys_DoubleTime (void)
{
	return (double)SDL_GetTicksNS() / 1e9;
}

/*
================
Sys_Sleep
================
*/
void Sys_Sleep (unsigned long msecs)
{
	SDL_Delay ((Uint32)msecs);
}

/*
================
Sys_SendKeyEvents
================
*/
void Sys_SendKeyEvents (void)
{
	IN_SendKeyEvents();
}

/*
================
Sys_GetClipboardData
================
*/
#define MAX_CLIPBOARDTXT	MAXCMDLINE	/* 256 */
char *Sys_GetClipboardData (void)
{
	char *data = NULL;
	char *cliptext;

	cliptext = SDL_GetClipboardText();
	if (cliptext && cliptext[0])
	{
		size_t size = strlen(cliptext) + 1;
		size = q_min(MAX_CLIPBOARDTXT, size);
		data = (char *) Z_Malloc(size, Z_MAINZONE);
		q_strlcpy (data, cliptext, size);
	}
	SDL_free(cliptext);
	return data;
}

/*
================
Sys_DateTimeString
================
*/
char *Sys_DateTimeString (char *buf)
{
	static char strbuf[24];
	time_t t;
	struct tm *l;

	if (!buf) buf = strbuf;

	t = time(NULL);
	l = localtime(&t);
	strftime(buf, 20, "%m/%d/%Y %H:%M:%S", l);

	return buf;
}

/*
================
Sys_CheckSDL
================
*/
void Sys_CheckSDL (void)
{
	int sdl_ver = SDL_GetVersion();
	Sys_Printf("Found SDL version %i.%i.%i\n",
		SDL_VERSIONNUM_MAJOR(sdl_ver),
		SDL_VERSIONNUM_MINOR(sdl_ver),
		SDL_VERSIONNUM_MICRO(sdl_ver));
}

/*
================
Sys_SDLInit — call early from platform main()
================
*/
void Sys_SDLInit (void)
{
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS))
		Sys_Error("SDL failed to initialize: %s", SDL_GetError());
}

/*
================
Sys_SDLShutdown — registered via atexit()
================
*/
void Sys_SDLShutdown (void)
{
	SDL_Quit();
}

/*
================
Sys_MainLoop — shared client/server frame loop
================
*/
static double	mainloop_oldtime;

static void Sys_MainFrame (void)
{
	double	time, newtime;

	if (isDedicated)
	{
		newtime = Sys_DoubleTime ();
		time = newtime - mainloop_oldtime;

#ifndef __EMSCRIPTEN__
		while (time < sys_ticrate.value)
		{
			SDL_Delay (1);
			newtime = Sys_DoubleTime ();
			time = newtime - mainloop_oldtime;
		}
#endif

		Host_Frame (time);
		mainloop_oldtime = newtime;
	}
	else
	{
		qboolean focused, minimized;

		/* pump SDL events first so window flags are current */
		Sys_SendKeyEvents();

		focused = VID_HasMouseOrInputFocus();
		minimized = VID_IsMinimized();

		/* yield the CPU when minimized or blocked — skip drawing */
		if (minimized || block_drawing)
		{
#ifndef __EMSCRIPTEN__
			SDL_Delay (PAUSE_SLEEP);
#endif
			scr_skipupdate = 1;
		}
#ifndef __EMSCRIPTEN__
		else if (!focused)
		{
			/* unfocused but visible — keep drawing, just throttle */
			SDL_Delay (NOT_FOCUS_SLEEP);
		}
#endif

		newtime = Sys_DoubleTime ();
		time = newtime - mainloop_oldtime;

#ifndef __EMSCRIPTEN__
		/* cap framerate to host_maxfps (skip when VSync is on — VSync
		 * already rate-limits the frame; the SDL_Delay polling loop
		 * can overshoot the vblank window and cause double-frame stutters) */
		if (host_maxfps.value > 0 && VID_MenuGetVSync() == 0)
		{
			double mintime = 1.0 / host_maxfps.value;
			while (time < mintime)
			{
				SDL_Delay (1);
				newtime = Sys_DoubleTime ();
				time = newtime - mainloop_oldtime;
			}
		}
#endif

		Host_Frame (time);

		mainloop_oldtime = newtime;
	}
}

void Sys_MainLoop (void)
{
	mainloop_oldtime = Sys_DoubleTime ();

#ifdef __EMSCRIPTEN__
	/* Emscripten: use requestAnimationFrame (fps=0), simulate infinite loop */
	emscripten_set_main_loop(Sys_MainFrame, 0, 1);
#else
	while (1)
		Sys_MainFrame();
#endif
}
