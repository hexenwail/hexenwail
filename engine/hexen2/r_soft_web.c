/*
 * r_soft_web.c -- renderer glue for the web software-renderer build.
 *
 * hexenwail's shared client code (cl_main.c, cl_effect.c, cl_parse.c,
 * menu.c, pr_csqc.c) is written against a handful of renderer-policy
 * symbols that the WebGL2 renderer publishes from r_webgl2.c. Some of
 * those knobs -- dynamic-light policy in particular -- are meaningful in
 * the software rasteriser too; others describe GPU features that have no
 * software counterpart.
 *
 * This file is the software counterpart of the r_webgl2.c cvar block:
 *   - light-policy cvars are real and honoured by the client;
 *   - GPU-only cvars exist so shared code links, and the menu rows that
 *     drive them are hidden under the software renderer (menu.c).
 *
 * See docs/web/SOFTWARE_RENDERER.md.
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

#include "quakedef.h"

/* --- light policy: honoured by cl_main.c / cl_effect.c ------------- */
cvar_t	gl_missile_glows = {"gl_missile_glows", "1", CVAR_ARCHIVE};
cvar_t	gl_torch_dlight = {"gl_torch_dlight", "1", CVAR_ARCHIVE};
cvar_t	gl_flashintensity = {"gl_flashintensity", "1", CVAR_ARCHIVE};
cvar_t	gl_extra_dynamic_lights = {"gl_extra_dynamic_lights", "1", CVAR_ARCHIVE};

/* --- GPU-only knobs: present for linkage, hidden in the menu ------- */
cvar_t	r_scale = {"r_scale", "1", CVAR_ARCHIVE};
cvar_t	r_softemu = {"r_softemu", "0", CVAR_ARCHIVE};
cvar_t	r_dither = {"r_dither", "0", CVAR_ARCHIVE};
/* The r_softemu sub-cvars (uhexen2-a5nn.3).  Only the one with a menu row
 * has to be here for linkage -- menu.c reads it to draw that row -- but all
 * four are registered so a config written by the GL build round-trips through
 * this one unchanged, which is the same reason r_softemu itself is here. */
cvar_t	r_softemu_mdl_warp = {"r_softemu_mdl_warp", "-1", CVAR_ARCHIVE};
cvar_t	r_softemu_dither_screen = {"r_softemu_dither_screen", "1.0", CVAR_ARCHIVE};
cvar_t	r_softemu_dither_texture = {"r_softemu_dither_texture", "1.0", CVAR_ARCHIVE};
cvar_t	r_softemu_lightmap_banding = {"r_softemu_lightmap_banding", "-1", CVAR_ARCHIVE};
cvar_t	r_lightmap_bicubic = {"r_lightmap_bicubic", "0", CVAR_ARCHIVE};
cvar_t	gl_particles = {"gl_particles", "1", CVAR_ARCHIVE};
/* The particle off switch (uhexen2-a5nn.11).  Real here, not just for
 * linkage: the software R_DrawParticles honours it too. */
cvar_t	r_drawparticles = {"r_drawparticles", "1", CVAR_ARCHIVE};
cvar_t	gl_fullbrights = {"gl_fullbrights", "1", CVAR_ARCHIVE};
cvar_t	r_wateralpha = {"r_wateralpha", "1", CVAR_ARCHIVE};
cvar_t	gl_glows = {"gl_glows", "1", CVAR_ARCHIVE};
cvar_t	gl_glow_intensity = {"gl_glow_intensity", "1", CVAR_ARCHIVE};
cvar_t	gl_fxaa = {"gl_fxaa", "0", CVAR_ARCHIVE};
cvar_t	r_hdr = {"r_hdr", "0", CVAR_ARCHIVE};
cvar_t	r_hdr_exposure = {"r_hdr_exposure", "1", CVAR_ARCHIVE};
cvar_t	gl_overbright_models = {"gl_overbright_models", "1", CVAR_ARCHIVE};
cvar_t	gl_coloredlight = {"gl_coloredlight", "1", CVAR_ARCHIVE};
cvar_t	r_softparticles = {"r_softparticles", "0", CVAR_ARCHIVE};
cvar_t	gl_waterwarp_speed = {"gl_waterwarp_speed", "0.5", CVAR_ARCHIVE};
cvar_t	gl_waterwarp_amount = {"gl_waterwarp_amount", "0.5", CVAR_ARCHIVE};
/* The demo playback bar (uhexen2-itie).  Drawn by gl_screen.c, which the
 * software build swaps out for screen.c, so the overlay does not exist here --
 * but menu.c reads the cvar unconditionally and the link fails without it.
 * Default matches gl_screen.c exactly, or a config round-tripped through the
 * software build would come back to the GL one with the bar turned off. */
cvar_t	scr_demobar_timeout = {"scr_demobar_timeout", "1", CVAR_ARCHIVE};

int	gl_filter_idx = 0;
float	gl_max_anisotropy = 1.0f;

/*
================
R_SoftWebInitCvars

Called from VID_Init(), before Draw_Init()/R_Init(), so that config
execution sees the full cvar set regardless of which renderer is built.
================
*/
void R_SoftWebInitCvars (void)
{
	static qboolean registered = false;

	if (registered)
		return;
	registered = true;

	Cvar_RegisterVariable (&gl_missile_glows);
	Cvar_RegisterVariable (&gl_torch_dlight);
	Cvar_RegisterVariable (&gl_flashintensity);
	Cvar_RegisterVariable (&gl_extra_dynamic_lights);

	Cvar_RegisterVariable (&r_scale);
	Cvar_RegisterVariable (&r_softemu);
	Cvar_RegisterVariable (&r_dither);
	Cvar_RegisterVariable (&r_softemu_mdl_warp);
	Cvar_RegisterVariable (&r_softemu_dither_screen);
	Cvar_RegisterVariable (&r_softemu_dither_texture);
	Cvar_RegisterVariable (&r_softemu_lightmap_banding);
	Cvar_RegisterVariable (&r_lightmap_bicubic);
	Cvar_RegisterVariable (&gl_particles);
	Cvar_RegisterVariable (&r_drawparticles);
	Cvar_RegisterVariable (&gl_fullbrights);
	Cvar_RegisterVariable (&r_wateralpha);
	Cvar_RegisterVariable (&gl_glows);
	Cvar_RegisterVariable (&gl_glow_intensity);
	Cvar_RegisterVariable (&gl_fxaa);
	Cvar_RegisterVariable (&r_hdr);
	Cvar_RegisterVariable (&r_hdr_exposure);
	Cvar_RegisterVariable (&gl_overbright_models);
	Cvar_RegisterVariable (&gl_coloredlight);
	Cvar_RegisterVariable (&r_softparticles);
	Cvar_RegisterVariable (&gl_waterwarp_speed);
	Cvar_RegisterVariable (&gl_waterwarp_amount);
	Cvar_RegisterVariable (&scr_demobar_timeout);
}

/*
=============================================================================

	per-entity PimpModel overrides

	model.h declares this table as renderer-owned; r_webgl2.c carries the
	WebGL2 copy. Behaviour must match exactly, because pr_cmds.c /
	pr_edict.c drive it from QuakeC.

=============================================================================
*/

static pimp_override_t	pimp_overrides[MAX_EDICTS];
static float		null_glow_settings[GLOW_SETTINGS_COUNT];

void R_ClearPimpOverrides (void)
{
	memset (pimp_overrides, 0, sizeof(pimp_overrides));
}

pimp_override_t *R_GetPimpOverride (int entnum)
{
	if (entnum < 0 || entnum >= MAX_EDICTS)
		return NULL;
	return &pimp_overrides[entnum];
}

int R_GetEntityModelFlags (entity_t *e)
{
	int entnum = (int)(e - cl_entities);

	if (entnum >= 0 && entnum < MAX_EDICTS && pimp_overrides[entnum].active
		&& pimp_overrides[entnum].trail_override)
	{
		return pimp_overrides[entnum].trail_flags;
	}
	return e->model ? e->model->flags : 0;
}

int R_GetPimpFlags (entity_t *e, float **gsettings_out)
{
	int entnum = (int)(e - cl_entities);

	if (entnum >= 0 && entnum < MAX_EDICTS && pimp_overrides[entnum].active)
	{
		if (gsettings_out)
			*gsettings_out = pimp_overrides[entnum].glow_settings;
		return pimp_overrides[entnum].ex_flags | (e->model ? e->model->ex_flags : 0);
	}
	if (gsettings_out)
		*gsettings_out = e->model ? e->model->glow_settings : null_glow_settings;
	return e->model ? e->model->ex_flags : 0;
}

/*
================
Fog_ParseServerMessage

svc_fog is a renderer-owned message: cl_parse.c dispatches it
unconditionally and each renderer supplies the handler (the WebGL2 build
does so from r_webgl2.c). The software rasteriser has no fog, but the
payload -- [byte] density, [byte] red, [byte] green, [byte] blue,
[short] fade time -- still has to be drained or the rest of the server
message is parsed at the wrong offset.
================
*/
void Fog_ParseServerMessage (void)
{
	MSG_ReadByte();
	MSG_ReadByte();
	MSG_ReadByte();
	MSG_ReadByte();
	MSG_ReadShort();
}

/*
================
GL_PostProcess_ResetWaterwarpPreview

The software renderer's water warp is applied by d_scan.c during
rasterisation, so there is no preview state to reset.
================
*/
void GL_PostProcess_ResetWaterwarpPreview (void)
{
}

void GL_PostProcess_RequestWaterwarpPreview (float duration)
{
	(void) duration;
}

/*
================
Mod_ReuploadAliasSkins

pr_cmds.c calls this when pimpmodel() changes a flag that decides how a skin
was uploaded.  Software skins are never uploaded anywhere -- they stay in the
hunk as palettized texels the rasterizer samples directly -- so there is
nothing to redo.
================
*/
void Mod_ReuploadAliasSkins (qmodel_t *mod)
{
	(void) mod;
}

/*
================
Sky_LoadSkyBox

The software renderer draws the classic two-layer scrolling sky from the
map's sky texture (r_sky.c / d_sky.c).  A six-sided skybox has no rasterizer
behind it, so svc_skybox is accepted and ignored rather than refused.
================
*/
void Sky_LoadSkyBox (const char *name)
{
	(void) name;
}
