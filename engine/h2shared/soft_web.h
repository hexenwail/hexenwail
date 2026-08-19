/*
 * soft_web.h -- shared declarations for the web software-renderer build.
 *
 * The web port can be built with either the classic 8bpp software
 * rasterizer (default) or the experimental WebGL2 renderer. Shared client
 * code (menu.c, sbar.c, console.c, ...) is written against a handful of
 * symbols that the WebGL2 build publishes through r_webgl2.h. This header
 * is the software-renderer counterpart: it is pulled in from quakeinc.h
 * when WEBQUAKE is defined without WEBGL2QUAKE.
 *
 * See docs/web/SOFTWARE_RENDERER.md for the design.
 *
 * Copyright (C) 2025-2026  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __HX2_SOFT_WEB_H
#define __HX2_SOFT_WEB_H

/* Screen extents in framebuffer pixels. The software renderer draws
 * straight into vid.buffer, so the "GL" viewport is simply the whole
 * framebuffer. Kept under these names because shared client code (menu.c)
 * refers to them unconditionally. */
extern int	glwidth, glheight;

/* Player colour-translation table, loaded by R_Init() in r_main.c and
 * consumed by cl_parse.c / menu.c / sbar.c. */
extern byte		*playerTranslation;
extern const int	color_offsets[MAX_PLAYER_CLASS];

/* Renderer-policy cvars shared with the WebGL2 build (r_soft_web.c). */
extern cvar_t	gl_missile_glows;
extern cvar_t	gl_torch_dlight;
extern cvar_t	gl_flashintensity;
extern cvar_t	gl_extra_dynamic_lights;
extern cvar_t	r_scale;
extern cvar_t	r_softemu;
extern cvar_t	r_dither;
extern cvar_t	r_lightmap_bicubic;
extern cvar_t	gl_particles;
extern cvar_t	gl_fullbrights;
extern cvar_t	r_wateralpha;
extern cvar_t	gl_glows;
extern cvar_t	gl_glow_intensity;
extern cvar_t	gl_fxaa;
extern cvar_t	r_hdr;
extern cvar_t	r_hdr_exposure;
extern cvar_t	gl_overbright_models;
extern cvar_t	gl_coloredlight;
extern cvar_t	r_softparticles;
extern cvar_t	gl_waterwarp_speed;
extern cvar_t	gl_waterwarp_amount;

/* Real software-renderer cvars, defined in r_main.c. */
extern cvar_t	r_dynamic;
extern cvar_t	r_waterwarp;
extern int	gl_filter_idx;
extern float	gl_max_anisotropy;

void R_SoftWebInitCvars (void);
void GL_PostProcess_ResetWaterwarpPreview (void);
void GL_PostProcess_RequestWaterwarpPreview (float duration);

/* svc_fog handler. The software renderer has no fog, so this only drains
 * the message payload; r_webgl2.h declares the WebGL2 counterpart. */
void Fog_ParseServerMessage (void);

/* Skin re-upload after a pimpmodel() flag change (pr_cmds.c) and skybox
 * loading (cl_parse.c) are GL concepts; r_soft_web.c answers both. */
void Mod_ReuploadAliasSkins (qmodel_t *mod);
void Sky_LoadSkyBox (const char *name);

/* Implemented by draw_soft_web.c; called once from Draw_Init(). */
void Draw_SoftWebInit (void);

#endif	/* __HX2_SOFT_WEB_H */
