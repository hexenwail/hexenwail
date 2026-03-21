/*
 * sdl_inc.h -- common SDL header.
 *
 * Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
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

#ifndef __HX2_SDL_INC
#define __HX2_SDL_INC

#if !defined(SDLQUAKE)
#error "SDLQUAKE must be defined in order to use sdl_inc.h"
#endif	/* SDLQUAKE */

#include <SDL3/SDL.h>

/* =================================================================
Minimum required SDL versions:
=================================================================== */

#define SDL_MIN_X	3
#define SDL_MIN_Y	0
#define SDL_MIN_Z	0

#if !SDL_VERSION_ATLEAST(SDL_MIN_X,SDL_MIN_Y,SDL_MIN_Z)
#error SDL version found is too old
#endif

#endif	/* __HX2_SDL_INC */
