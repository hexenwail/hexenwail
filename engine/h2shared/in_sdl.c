/* in_sdl.c -- SDL3 game input code.
 *
 * Copyright (C) 2001  contributors of the Anvil of Thyrion project
 * Copyright (C) 2005-2012  Steven Atkinson, O.Sezer, Sander van Dijk
 * Copyright (C) 2025  uHexen2 contributors
 *
 * Gamepad support modeled after Ironwail (SDL3 Gamepad API, circular
 * deadzone, power-curve easing).
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

#include "sdl_inc.h"
#include "quakedef.h"

#include <math.h>

static qboolean	prev_gamekey;

/* ================================================================
   Mouse
   ================================================================ */

static cvar_t	m_filter = {"m_filter", "0", CVAR_NONE};

static int	mouse_x, mouse_y, old_mouse_x, old_mouse_y;

static qboolean	mouseactive = false;
static qboolean	mouseinitialized = false;
static qboolean	mouseactivatetoggle = false;
static qboolean	mouseshowtoggle = true;

static int buttonremap[] =
{
	K_MOUSE1,
	K_MOUSE3,	/* right button		*/
	K_MOUSE2,	/* middle button	*/
	K_MOUSE4,
	K_MOUSE5
};


/* ================================================================
   Gamepad (SDL3 Gamepad API)
   ================================================================ */

static SDL_Gamepad	*gp_active = NULL;
static SDL_JoystickID	gp_active_id = 0;

/* cvars (non-static so menu.c can read them) */
cvar_t	in_gamepad = {"gamepad", "1", CVAR_ARCHIVE};
cvar_t	joy_deadzone_look = {"joy_deadzone_look", "0.175", CVAR_ARCHIVE};
cvar_t	joy_deadzone_move = {"joy_deadzone_move", "0.175", CVAR_ARCHIVE};
cvar_t	joy_deadzone_trigger = {"joy_deadzone_trigger", "0.2", CVAR_ARCHIVE};
cvar_t	joy_sensitivity_yaw = {"joy_sensitivity_yaw", "240", CVAR_ARCHIVE};
cvar_t	joy_sensitivity_pitch = {"joy_sensitivity_pitch", "130", CVAR_ARCHIVE};
cvar_t	joy_exponent = {"joy_exponent", "2", CVAR_ARCHIVE};
cvar_t	joy_exponent_move = {"joy_exponent_move", "2", CVAR_ARCHIVE};
cvar_t	joy_invert = {"joy_invert", "0", CVAR_ARCHIVE};
cvar_t	joy_swapmovelook = {"joy_swapmovelook", "0", CVAR_ARCHIVE};
cvar_t	joy_rumble = {"joy_rumble", "0.5", CVAR_ARCHIVE};

/* axis state for edge-detected trigger/stick key events */
typedef struct {
	float	x, y;
} stickpair_t;

static stickpair_t	gp_old_move, gp_old_look;
static qboolean		gp_old_ltrigger, gp_old_rtrigger;

/* map SDL3 gamepad buttons to engine keycodes */
static int IN_GPButtonToKey (SDL_GamepadButton btn)
{
	switch (btn)
	{
	case SDL_GAMEPAD_BUTTON_SOUTH:		return K_GP_A;
	case SDL_GAMEPAD_BUTTON_EAST:		return K_GP_B;
	case SDL_GAMEPAD_BUTTON_WEST:		return K_GP_X;
	case SDL_GAMEPAD_BUTTON_NORTH:		return K_GP_Y;
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:	return K_GP_LSHOULDER;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:	return K_GP_RSHOULDER;
	case SDL_GAMEPAD_BUTTON_LEFT_STICK:	return K_GP_LTHUMB;
	case SDL_GAMEPAD_BUTTON_RIGHT_STICK:	return K_GP_RTHUMB;
	case SDL_GAMEPAD_BUTTON_BACK:		return K_GP_BACK;
	case SDL_GAMEPAD_BUTTON_START:		return K_GP_START;
	case SDL_GAMEPAD_BUTTON_DPAD_UP:	return K_UPARROW;
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN:	return K_DOWNARROW;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT:	return K_LEFTARROW;
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:	return K_RIGHTARROW;
	default:				return 0;
	}
}

/* circular deadzone: returns rescaled vector with magnitude [0,1] */
static stickpair_t IN_ApplyDeadzone (float x, float y, float deadzone)
{
	stickpair_t	result = {0, 0};
	float		mag = sqrtf(x*x + y*y);
	float		outer = 0.02f;

	if (mag <= deadzone)
		return result;

	float scale = fminf(1.0f, (mag - deadzone) / (1.0f - deadzone - outer)) / mag;
	result.x = x * scale;
	result.y = y * scale;
	return result;
}

/* power curve easing: preserves direction, applies pow to magnitude */
static stickpair_t IN_ApplyEasing (stickpair_t in, float exponent)
{
	stickpair_t	result = {0, 0};
	float		mag = sqrtf(in.x*in.x + in.y*in.y);

	if (mag < 0.001f)
		return result;

	float eased = powf(fminf(mag, 1.0f), exponent);
	float scale = eased / mag;
	result.x = in.x * scale;
	result.y = in.y * scale;
	return result;
}

static float IN_GetAxis (SDL_GamepadAxis axis)
{
	if (!gp_active)
		return 0;
	return SDL_GetGamepadAxis(gp_active, axis) / 32768.0f;
}

static void IN_GPKeyEvent (qboolean old_down, qboolean new_down, int key)
{
	if (new_down != old_down)
		Key_Event(key, new_down);
}

/* stick-as-arrows for menu navigation */
static void IN_GPMenuMove (stickpair_t old_s, stickpair_t new_s)
{
	const float threshold = 0.7f;
	IN_GPKeyEvent(old_s.x < -threshold, new_s.x < -threshold, K_LEFTARROW);
	IN_GPKeyEvent(old_s.x >  threshold, new_s.x >  threshold, K_RIGHTARROW);
	IN_GPKeyEvent(old_s.y < -threshold, new_s.y < -threshold, K_UPARROW);
	IN_GPKeyEvent(old_s.y >  threshold, new_s.y >  threshold, K_DOWNARROW);
}

static void IN_StartupGamepad (void)
{
	int	count;
	SDL_JoystickID	*pads;

	if (safemode || COM_CheckParm("-nojoy") || COM_CheckParm("-nogamepad"))
		return;

	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
	{
		Con_Printf("Couldn't init SDL gamepad: %s\n", SDL_GetError());
		return;
	}

	pads = SDL_GetGamepads(&count);
	if (!pads || count == 0)
	{
		Con_Printf("No gamepad devices found\n");
		if (pads) SDL_free(pads);
		return;
	}

	Con_Printf("SDL_Gamepad: %d device(s) found:\n", count);
	for (int i = 0; i < count; i++)
		Con_Printf("  #%d: \"%s\"\n", i, SDL_GetGamepadNameForID(pads[i]));

	if (in_gamepad.integer)
	{
		gp_active = SDL_OpenGamepad(pads[0]);
		if (gp_active)
		{
			gp_active_id = pads[0];
			Con_Printf("Gamepad opened: \"%s\"\n", SDL_GetGamepadName(gp_active));
		}
		else
			Con_Printf("Gamepad open failed: %s\n", SDL_GetError());
	}

	SDL_free(pads);
}

qboolean IN_HasGamepad (void)
{
	return (gp_active != NULL);
}

static void IN_ShutdownGamepad (void)
{
	if (gp_active)
	{
		SDL_CloseGamepad(gp_active);
		gp_active = NULL;
		gp_active_id = 0;
	}
	if (SDL_WasInit(SDL_INIT_GAMEPAD))
		SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/*
===========
IN_GPMove -- gamepad analog stick input
===========
*/
static void IN_GPMove (usercmd_t *cmd)
{
	float		speed;
	stickpair_t	move_raw, look_raw;
	stickpair_t	move, look;
	float		ltrig, rtrig;
	qboolean	lt_down, rt_down;

	if (!gp_active)
		return;

	if (in_speed.state & 1)
		speed = cl_movespeedkey.value;
	else
		speed = 1;

	/* read raw axes */
	if (joy_swapmovelook.integer)
	{
		look_raw.x = IN_GetAxis(SDL_GAMEPAD_AXIS_LEFTX);
		look_raw.y = IN_GetAxis(SDL_GAMEPAD_AXIS_LEFTY);
		move_raw.x = IN_GetAxis(SDL_GAMEPAD_AXIS_RIGHTX);
		move_raw.y = IN_GetAxis(SDL_GAMEPAD_AXIS_RIGHTY);
	}
	else
	{
		move_raw.x = IN_GetAxis(SDL_GAMEPAD_AXIS_LEFTX);
		move_raw.y = IN_GetAxis(SDL_GAMEPAD_AXIS_LEFTY);
		look_raw.x = IN_GetAxis(SDL_GAMEPAD_AXIS_RIGHTX);
		look_raw.y = IN_GetAxis(SDL_GAMEPAD_AXIS_RIGHTY);
	}

	/* apply circular deadzone + easing */
	move = IN_ApplyDeadzone(move_raw.x, move_raw.y, joy_deadzone_move.value);
	move = IN_ApplyEasing(move, joy_exponent_move.value);
	look = IN_ApplyDeadzone(look_raw.x, look_raw.y, joy_deadzone_look.value);
	look = IN_ApplyEasing(look, joy_exponent.value);

	/* triggers */
	ltrig = IN_GetAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
	rtrig = IN_GetAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
	lt_down = (ltrig > joy_deadzone_trigger.value);
	rt_down = (rtrig > joy_deadzone_trigger.value);

	/* in menus: sticks act as arrow keys, triggers as enter */
	if (!Key_IsGameKey())
	{
		IN_GPMenuMove(gp_old_move, move);
		IN_GPKeyEvent(gp_old_ltrigger, lt_down, K_ENTER);
		IN_GPKeyEvent(gp_old_rtrigger, rt_down, K_ENTER);
		gp_old_move = move;
		gp_old_look = look;
		gp_old_ltrigger = lt_down;
		gp_old_rtrigger = rt_down;
		return;
	}

	/* trigger key events */
	IN_GPKeyEvent(gp_old_ltrigger, lt_down, K_GP_LTRIGGER);
	IN_GPKeyEvent(gp_old_rtrigger, rt_down, K_GP_RTRIGGER);
	gp_old_ltrigger = lt_down;
	gp_old_rtrigger = rt_down;
	gp_old_move = move;
	gp_old_look = look;

	/* movement */
	cmd->sidemove += move.x * speed * 225;
	cmd->forwardmove -= move.y * speed * 200;

	/* look */
	cl.viewangles[YAW] -= look.x * joy_sensitivity_yaw.value * host_frametime;
	cl.viewangles[PITCH] += look.y * joy_sensitivity_pitch.value * host_frametime *
				(joy_invert.integer ? -1.0f : 1.0f);
	if (look.x != 0 || look.y != 0)
		V_StopPitchDrift();

	/* bounds check pitch */
	if (cl.viewangles[PITCH] > 80.0f)
		cl.viewangles[PITCH] = 80.0f;
	if (cl.viewangles[PITCH] < -70.0f)
		cl.viewangles[PITCH] = -70.0f;
}

/*
===========
IN_GPRumble -- trigger haptic feedback
===========
*/
void IN_GPRumble (float low_freq, float high_freq, Uint32 duration_ms)
{
	if (!gp_active || joy_rumble.value <= 0)
		return;
	SDL_RumbleGamepad(gp_active,
			  (Uint16)(low_freq * joy_rumble.value * 65535),
			  (Uint16)(high_freq * joy_rumble.value * 65535),
			  duration_ms);
}


/* ================================================================
   Mouse functions
   ================================================================ */

void IN_ShowMouse (void)
{
	if (!mouseshowtoggle)
	{
		SDL_ShowCursor();
		mouseshowtoggle = true;
	}
}

void IN_HideMouse (void)
{
	if (mouseshowtoggle)
	{
		SDL_HideCursor();
		mouseshowtoggle = false;
	}
}

/* ============================================================
   NOTES on enabling-disabling the mouse:
   - In windowed mode, mouse is temporarily disabled in main
     menu, so the un-grabbed pointer can be used for desktop
     This state is stored in menu_disabled_mouse as true
   - In fullscreen mode, we don't disable the mouse in menus,
     if we toggle windowed/fullscreen, the above state variable
     is used to correct this in VID_ToggleFullscreen()
   - In the console mode and in the options menu-group, mouse
     is not disabled, and menu_disabled_mouse is set to false
   - Starting a or connecting to a server activates the mouse
     and sets menu_disabled_mouse to false
   - Pausing the game disables (so un-grabs) the mouse, unpausing
     activates it. We don't play with menu_disabled_mouse in
     such cases
*/

void IN_ActivateMouse (void)
{
	if (mouseinitialized) {
	    if (!mouseactivatetoggle) {
		if (_enable_mouse.integer)
		{
			mouseactivatetoggle = true;
			mouseactive = true;
			SDL_SetWindowRelativeMouseMode(VID_GetWindow(), true);
			/* discard any stale relative motion */
			SDL_GetRelativeMouseState (NULL, NULL);
		}
	    }
	}
}

void IN_DeactivateMouse (void)
{
	if (mouseinitialized) {
	    if (mouseactivatetoggle) {
		mouseactivatetoggle = false;
		mouseactive = false;
		SDL_SetWindowRelativeMouseMode(VID_GetWindow(), false);
	    }
	}
}

static void IN_StartupMouse (void)
{
	if (safemode || COM_CheckParm ("-nomouse"))
	{
		SDL_SetWindowRelativeMouseMode(VID_GetWindow(), false);
		return;
	}

	old_mouse_x = old_mouse_y = 0;
	mouseinitialized = true;
	if (_enable_mouse.integer)
	{
		mouseactivatetoggle = true;
		mouseactive = true;
		SDL_SetWindowRelativeMouseMode(VID_GetWindow(), true);
		SDL_GetRelativeMouseState (NULL, NULL);
	}
}

void IN_ClearStates (void)
{
}

static void Force_CenterView_f (void)
{
	cl.viewangles[PITCH] = 0;
}


/* ================================================================
   Init / Shutdown / Move
   ================================================================ */

void IN_Init (void)
{
	/* mouse */
	Cvar_RegisterVariable (&m_filter);

	/* gamepad */
	Cvar_RegisterVariable (&in_gamepad);
	Cvar_RegisterVariable (&joy_deadzone_look);
	Cvar_RegisterVariable (&joy_deadzone_move);
	Cvar_RegisterVariable (&joy_deadzone_trigger);
	Cvar_RegisterVariable (&joy_sensitivity_yaw);
	Cvar_RegisterVariable (&joy_sensitivity_pitch);
	Cvar_RegisterVariable (&joy_exponent);
	Cvar_RegisterVariable (&joy_exponent_move);
	Cvar_RegisterVariable (&joy_invert);
	Cvar_RegisterVariable (&joy_swapmovelook);
	Cvar_RegisterVariable (&joy_rumble);

	Cmd_AddCommand ("force_centerview", Force_CenterView_f);

	IN_StartupMouse ();
	IN_StartupGamepad ();

	prev_gamekey = Key_IsGameKey();
}

void IN_Shutdown (void)
{
	IN_DeactivateMouse ();
	IN_ShowMouse ();
	mouseinitialized = false;

	IN_ShutdownGamepad ();
}

void IN_ReInit (void)
{
	IN_StartupMouse ();
	prev_gamekey = Key_IsGameKey();
}


static void IN_MouseMove (usercmd_t *cmd, int mx, int my)
{
	if (m_filter.integer)
	{
		mouse_x = (mx + old_mouse_x) * 0.5;
		mouse_y = (my + old_mouse_y) * 0.5;
	}
	else
	{
		mouse_x = mx;
		mouse_y = my;
	}

	old_mouse_x = mx;
	old_mouse_y = my;

	mouse_x *= sensitivity.value;
	mouse_y *= sensitivity.value;

	if ( (in_strafe.state & 1) || (lookstrafe.integer && (in_mlook.state & 1) ))
		cmd->sidemove += m_side.value * mouse_x;
	else
		cl.viewangles[YAW] -= m_yaw.value * mouse_x;

	if (in_mlook.state & 1)
	{
		if (mx || my)
			V_StopPitchDrift ();
	}

	if ( (in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * mouse_y;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	}
	else
	{
		if ((in_strafe.state & 1) && (cl.v.movetype == MOVETYPE_NOCLIP))
			cmd->upmove -= m_forward.value * mouse_y;
		else
			cmd->forwardmove -= m_forward.value * mouse_y;
	}

	if (cl.idealroll == 0)
	{
		if (cl.v.movetype == MOVETYPE_FLY)
		{
			if (mouse_x < 0)
				cl.idealroll = -10;
			else if (mouse_x > 0)
				cl.idealroll = 10;
		}
	}
}

static void IN_DiscardMove (void)
{
	if (mouseinitialized)
	{
		old_mouse_x = old_mouse_y = 0;
		SDL_GetRelativeMouseState (NULL, NULL);
	}
}

void IN_UpdateViewAngles (void)
{
	// no-op: viewangles are updated by IN_Move called from host.c
}

void IN_Move (usercmd_t *cmd)
{
	float	fx, fy;
	int	x, y;
	qboolean app_active;

	if (cl.v.cameramode)
	{
		memset (cmd, 0, sizeof(*cmd));
		IN_DiscardMove ();
		return;
	}

	app_active = !VID_IsMinimized();
	x = 0;
	y = 0;

	if (mouseactive)
	{
		SDL_GetRelativeMouseState(&fx, &fy);
		x = (int)fx;
		y = (int)fy;
	}
	if (x != 0 || y != 0)
		IN_MouseMove (cmd, x, y);

	if (app_active)
		IN_GPMove (cmd);
}

void IN_Commands (void)
{
	/* button events handled by IN_SendKeyEvents() */
}


/* ================================================================
   SDL3 key mapping
   ================================================================ */

/*
===================
IN_SDLScancodeToQuakeKey

Maps physical key positions (scancodes) to Quake key codes.
Used for game bindings so WASD works by physical position
regardless of keyboard layout.
===================
*/
static int IN_SDLScancodeToQuakeKey (SDL_Scancode sc, SDL_Keymod modstate)
{
	/* special keys first */
	switch (sc)
	{
	case SDL_SCANCODE_TAB:		return K_TAB;
	case SDL_SCANCODE_RETURN:	return K_ENTER;
	case SDL_SCANCODE_ESCAPE:	return K_ESCAPE;
	case SDL_SCANCODE_SPACE:	return K_SPACE;
	case SDL_SCANCODE_BACKSPACE:	return K_BACKSPACE;

	case SDL_SCANCODE_UP:		return K_UPARROW;
	case SDL_SCANCODE_DOWN:		return K_DOWNARROW;
	case SDL_SCANCODE_LEFT:		return K_LEFTARROW;
	case SDL_SCANCODE_RIGHT:	return K_RIGHTARROW;

	case SDL_SCANCODE_LALT:
	case SDL_SCANCODE_RALT:		return K_ALT;
	case SDL_SCANCODE_LCTRL:
	case SDL_SCANCODE_RCTRL:	return K_CTRL;
	case SDL_SCANCODE_LSHIFT:
	case SDL_SCANCODE_RSHIFT:	return K_SHIFT;
	case SDL_SCANCODE_LGUI:
	case SDL_SCANCODE_RGUI:		return K_COMMAND;

	case SDL_SCANCODE_DELETE:	return K_DEL;
	case SDL_SCANCODE_INSERT:	return K_INS;
	case SDL_SCANCODE_HOME:		return K_HOME;
	case SDL_SCANCODE_END:		return K_END;
	case SDL_SCANCODE_PAGEUP:	return K_PGUP;
	case SDL_SCANCODE_PAGEDOWN:	return K_PGDN;

	case SDL_SCANCODE_F1:		return K_F1;
	case SDL_SCANCODE_F2:		return K_F2;
	case SDL_SCANCODE_F3:		return K_F3;
	case SDL_SCANCODE_F4:		return K_F4;
	case SDL_SCANCODE_F5:		return K_F5;
	case SDL_SCANCODE_F6:		return K_F6;
	case SDL_SCANCODE_F7:		return K_F7;
	case SDL_SCANCODE_F8:		return K_F8;
	case SDL_SCANCODE_F9:		return K_F9;
	case SDL_SCANCODE_F10:		return K_F10;
	case SDL_SCANCODE_F11:		return K_F11;
	case SDL_SCANCODE_F12:		return K_F12;

	case SDL_SCANCODE_PAUSE:	return K_PAUSE;

	/* numpad */
	case SDL_SCANCODE_NUMLOCKCLEAR:	return K_KP_NUMLOCK;
	case SDL_SCANCODE_KP_0:		return K_KP_INS;
	case SDL_SCANCODE_KP_1:		return K_KP_END;
	case SDL_SCANCODE_KP_2:		return K_KP_DOWNARROW;
	case SDL_SCANCODE_KP_3:		return K_KP_PGDN;
	case SDL_SCANCODE_KP_4:		return K_KP_LEFTARROW;
	case SDL_SCANCODE_KP_5:		return K_KP_5;
	case SDL_SCANCODE_KP_6:		return K_KP_RIGHTARROW;
	case SDL_SCANCODE_KP_7:		return K_KP_HOME;
	case SDL_SCANCODE_KP_8:		return K_KP_UPARROW;
	case SDL_SCANCODE_KP_9:		return K_KP_PGUP;
	case SDL_SCANCODE_KP_PERIOD:	return K_KP_DEL;
	case SDL_SCANCODE_KP_DIVIDE:	return K_KP_SLASH;
	case SDL_SCANCODE_KP_MULTIPLY:	return K_KP_STAR;
	case SDL_SCANCODE_KP_MINUS:	return K_KP_MINUS;
	case SDL_SCANCODE_KP_PLUS:	return K_KP_PLUS;
	case SDL_SCANCODE_KP_ENTER:	return K_KP_ENTER;

	default:
		break;
	}

	/* letter keys: physical US QWERTY positions → lowercase ASCII */
	if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
		return 'a' + (sc - SDL_SCANCODE_A);

	/* number row */
	if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
		return '1' + (sc - SDL_SCANCODE_1);
	if (sc == SDL_SCANCODE_0)
		return '0';

	/* punctuation — US layout equivalents */
	switch (sc)
	{
	case SDL_SCANCODE_GRAVE:	return '`';
	case SDL_SCANCODE_MINUS:	return '-';
	case SDL_SCANCODE_EQUALS:	return '=';
	case SDL_SCANCODE_LEFTBRACKET:	return '[';
	case SDL_SCANCODE_RIGHTBRACKET:	return ']';
	case SDL_SCANCODE_BACKSLASH:	return '\\';
	case SDL_SCANCODE_SEMICOLON:	return ';';
	case SDL_SCANCODE_APOSTROPHE:	return '\'';
	case SDL_SCANCODE_COMMA:	return ',';
	case SDL_SCANCODE_PERIOD:	return '.';
	case SDL_SCANCODE_SLASH:	return '/';
	default:
		break;
	}

	return 0;
}

static int IN_SDLKeyToQuakeKey (SDL_Keycode sym, qboolean gamekey, SDL_Keymod modstate)
{
	switch (sym)
	{
	case SDLK_DELETE:	return K_DEL;
	case SDLK_BACKSPACE:	return K_BACKSPACE;
	case SDLK_F1:		return K_F1;
	case SDLK_F2:		return K_F2;
	case SDLK_F3:		return K_F3;
	case SDLK_F4:		return K_F4;
	case SDLK_F5:		return K_F5;
	case SDLK_F6:		return K_F6;
	case SDLK_F7:		return K_F7;
	case SDLK_F8:		return K_F8;
	case SDLK_F9:		return K_F9;
	case SDLK_F10:		return K_F10;
	case SDLK_F11:		return K_F11;
	case SDLK_F12:		return K_F12;
	case SDLK_PAUSE:	return K_PAUSE;
	case SDLK_UP:		return K_UPARROW;
	case SDLK_DOWN:		return K_DOWNARROW;
	case SDLK_RIGHT:	return K_RIGHTARROW;
	case SDLK_LEFT:		return K_LEFTARROW;
	case SDLK_INSERT:	return K_INS;
	case SDLK_HOME:		return K_HOME;
	case SDLK_END:		return K_END;
	case SDLK_PAGEUP:	return K_PGUP;
	case SDLK_PAGEDOWN:	return K_PGDN;
	case SDLK_RSHIFT:
	case SDLK_LSHIFT:	return K_SHIFT;
	case SDLK_RCTRL:
	case SDLK_LCTRL:	return K_CTRL;
	case SDLK_RALT:
	case SDLK_LALT:	return K_ALT;
	case SDLK_RGUI:
	case SDLK_LGUI:	return K_COMMAND;
	case SDLK_NUMLOCKCLEAR:
		return gamekey ? K_KP_NUMLOCK : 0;
	case SDLK_KP_0:
		return gamekey ? K_KP_INS : ((modstate & SDL_KMOD_NUM) ? '0' : K_INS);
	case SDLK_KP_1:
		return gamekey ? K_KP_END : ((modstate & SDL_KMOD_NUM) ? '1' : K_END);
	case SDLK_KP_2:
		return gamekey ? K_KP_DOWNARROW : ((modstate & SDL_KMOD_NUM) ? '2' : K_DOWNARROW);
	case SDLK_KP_3:
		return gamekey ? K_KP_PGDN : ((modstate & SDL_KMOD_NUM) ? '3' : K_PGDN);
	case SDLK_KP_4:
		return gamekey ? K_KP_LEFTARROW : ((modstate & SDL_KMOD_NUM) ? '4' : K_LEFTARROW);
	case SDLK_KP_5:
		return gamekey ? K_KP_5 : '5';
	case SDLK_KP_6:
		return gamekey ? K_KP_RIGHTARROW : ((modstate & SDL_KMOD_NUM) ? '6' : K_RIGHTARROW);
	case SDLK_KP_7:
		return gamekey ? K_KP_HOME : ((modstate & SDL_KMOD_NUM) ? '7' : K_HOME);
	case SDLK_KP_8:
		return gamekey ? K_KP_UPARROW : ((modstate & SDL_KMOD_NUM) ? '8' : K_UPARROW);
	case SDLK_KP_9:
		return gamekey ? K_KP_PGUP : ((modstate & SDL_KMOD_NUM) ? '9' : K_PGUP);
	case SDLK_KP_PERIOD:
		return gamekey ? K_KP_DEL : ((modstate & SDL_KMOD_NUM) ? '.' : K_DEL);
	case SDLK_KP_DIVIDE:
		return gamekey ? K_KP_SLASH : '/';
	case SDLK_KP_MULTIPLY:
		return gamekey ? K_KP_STAR : '*';
	case SDLK_KP_MINUS:
		return gamekey ? K_KP_MINUS : '-';
	case SDLK_KP_PLUS:
		return gamekey ? K_KP_PLUS : '+';
	case SDLK_KP_ENTER:
		return gamekey ? K_KP_ENTER : '\r';
	case SDLK_KP_EQUALS:
		return gamekey ? 0 : '=';
	case SDLK_TAB:		return K_TAB;
	case SDLK_RETURN:	return K_ENTER;
	case SDLK_ESCAPE:	return K_ESCAPE;
	case SDLK_SPACE:	return K_SPACE;
	default:
		break;
	}

	if (sym >= SDLK_A && sym <= SDLK_Z)
		return 'a' + (sym - SDLK_A);

	if (sym >= SDLK_0 && sym <= SDLK_9)
		return '0' + (sym - SDLK_0);

	switch (sym)
	{
	case SDLK_GRAVE:	return '`';
	case SDLK_MINUS:	return '-';
	case SDLK_EQUALS:	return '=';
	case SDLK_LEFTBRACKET:	return '[';
	case SDLK_RIGHTBRACKET:	return ']';
	case SDLK_BACKSLASH:	return '\\';
	case SDLK_SEMICOLON:	return ';';
	case SDLK_APOSTROPHE:	return '\'';
	case SDLK_COMMA:	return ',';
	case SDLK_PERIOD:	return '.';
	case SDLK_SLASH:	return '/';
	default:
		break;
	}

	if (sym > 0 && sym <= 255)
		return (int)sym;

	return 0;
}


/* ================================================================
   SDL3 event loop
   ================================================================ */

void IN_SendKeyEvents (void)
{
	SDL_Event event;
	int sym, key;
	qboolean state;
	SDL_Keymod modstate;
	qboolean gamekey;

	gamekey = Key_IsGameKey();
	if (gamekey != prev_gamekey)
		prev_gamekey = gamekey;

	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			S_UnblockSound();
			IN_ActivateMouse();
			scr_skipupdate = 0;
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			S_BlockSound();
			IN_DeactivateMouse();
			break;
		case SDL_EVENT_WINDOW_MINIMIZED:
			scr_skipupdate = 1;
			break;
		case SDL_EVENT_WINDOW_RESTORED:
		case SDL_EVENT_WINDOW_EXPOSED:
			scr_skipupdate = 0;
			break;

		case SDL_EVENT_TEXT_INPUT:
			/* feed printable characters to console/chat input */
			Key_CharEvent(event.text.text);
			break;

		case SDL_EVENT_KEY_DOWN:
			if ((event.key.key == SDLK_RETURN) &&
			    (event.key.mod & SDL_KMOD_ALT))
			{
				VID_ToggleFullscreen();
				break;
			}
			if ((event.key.key == SDLK_ESCAPE) &&
			    (event.key.mod & SDL_KMOD_SHIFT))
			{
				Con_ToggleConsole_f();
				break;
			}
			if ((event.key.key == SDLK_G) &&
			    (event.key.mod & SDL_KMOD_CTRL))
			{
				SDL_Window *w = VID_GetWindow();
				SDL_SetWindowRelativeMouseMode(w, !SDL_GetWindowRelativeMouseMode(w));
				break;
			}
		/* fallthrough */
		case SDL_EVENT_KEY_UP:
			modstate = SDL_GetModState();
			state = event.key.down;
			if (gamekey)
				sym = IN_SDLScancodeToQuakeKey(event.key.scancode, modstate);
			else
				sym = IN_SDLKeyToQuakeKey(event.key.key, gamekey, modstate);
			if (sym)
				Key_Event(sym, state);
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (!mouseactive || in_mode_set)
				break;
			if (event.button.button < 1 ||
			    event.button.button > sizeof(buttonremap) / sizeof(buttonremap[0]))
			{
				break;
			}
			Key_Event(buttonremap[event.button.button - 1], event.button.down);
			break;

		case SDL_EVENT_MOUSE_WHEEL:
			if (!mouseactive || in_mode_set)
				break;
			if (event.wheel.y > 0)
			{
				Key_Event(K_MWHEELUP, true);
				Key_Event(K_MWHEELUP, false);
			}
			else if (event.wheel.y < 0)
			{
				Key_Event(K_MWHEELDOWN, true);
				Key_Event(K_MWHEELDOWN, false);
			}
			break;

		/* SDL3 Gamepad button events */
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
			if (in_mode_set)
				break;
			key = IN_GPButtonToKey((SDL_GamepadButton)event.gbutton.button);
			if (key)
				Key_Event(key, event.gbutton.down);
			break;

		/* hot-plug */
		case SDL_EVENT_GAMEPAD_ADDED:
			if (!gp_active && in_gamepad.integer)
			{
				gp_active = SDL_OpenGamepad(event.gdevice.which);
				if (gp_active)
				{
					gp_active_id = event.gdevice.which;
					Con_Printf("Gamepad connected: \"%s\"\n",
						   SDL_GetGamepadName(gp_active));
				}
			}
			break;
		case SDL_EVENT_GAMEPAD_REMOVED:
			if (gp_active && event.gdevice.which == gp_active_id)
			{
				Con_Printf("Gamepad disconnected\n");
				SDL_CloseGamepad(gp_active);
				gp_active = NULL;
				gp_active_id = 0;
			}
			break;

		case SDL_EVENT_MOUSE_MOTION:
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			break;

		case SDL_EVENT_QUIT:
			CL_Disconnect ();
			Sys_Quit ();
			break;

		default:
			break;
		}
	}
}
