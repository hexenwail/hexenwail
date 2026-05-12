/* gl_main.c
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
#include "gl_sky.h"
#include "gl_shader.h"
#include "gl_vbo.h"
#include "gl_matrix.h"
#include "gl_postprocess.h"

/* ES 3.0 compatibility */
#ifdef EMSCRIPTEN
#ifndef GLdouble
#define GLdouble double
#endif
#ifndef GL_QUADS
#define GL_QUADS 0
#endif
#ifndef GL_POLYGON
#define GL_POLYGON 0
#endif
#endif

/* gl_fog.c */
void Fog_SetupFrame (void);
void Fog_EnableGFog (void);
float Fog_GetDensity (void);
float *Fog_GetColor (void);
void Fog_DisableGFog (void);

entity_t	r_worldentity;
vec3_t		modelorg, r_entorigin;

int			r_visframecount;	// bumped when going to a new PVS
int			r_framecount;		// used for dlight push checking

mplane_t	frustum[4];

int			c_brush_polys, c_alias_polys;

qboolean	r_cache_thrash;			// compatability

GLuint			currenttexture = GL_UNUSED_TEXTURE;	// to avoid unnecessary texture sets

GLuint			particletexture;	// little dot for particles
GLuint			playertextures[MAX_CLIENTS];	// up to MAX_CLIENTS color translated skins
GLuint			gl_extra_textures[MAX_EXTRA_TEXTURES];   // generic textures for models

int			mirrortexturenum;	// quake texturenum, not gltexturenum
qboolean	mirror;
mplane_t	*mirror_plane;

static float	model_constant_alpha;
static qboolean	model_fullbright_pass;	// true during fullbright overlay pass

static float	r_time1;
static float	r_lasttime1 = 0;

extern qmodel_t	*player_models[MAX_PLAYER_CLASS];

//
// view origin
//
vec3_t		vup, vpn, vright, r_origin;

float		r_world_matrix[16];

//
// screen size info
//
refdef_t	r_refdef;
mleaf_t		*r_viewleaf, *r_oldviewleaf;

texture_t	*r_notexture_mip;

int		d_lightstylevalue[256];	// 8.8 fraction of base light value

int		gl_coloredstatic;	// used to store what type of static light
					// we loaded in Mod_LoadLighting()

static	qboolean AlwaysDrawModel;

cvar_t	r_norefresh = {"r_norefresh", "0", CVAR_NONE};
cvar_t	r_drawentities = {"r_drawentities", "1", CVAR_NONE};
cvar_t	r_drawviewmodel = {"r_drawviewmodel", "1", CVAR_NONE};
cvar_t	r_speeds = {"r_speeds", "0", CVAR_NONE};
cvar_t	r_speeds_gpufinish = {"r_speeds_gpufinish", "0", CVAR_NONE};	/* diagnostic: glFinish() at start of chains to attribute GPU sync */
cvar_t	r_waterwarp = {"r_waterwarp", "0", CVAR_ARCHIVE};
cvar_t	r_motionblur = {"r_motionblur", "0", CVAR_ARCHIVE};
cvar_t	r_alphatocoverage = {"r_alphatocoverage", "1", CVAR_ARCHIVE};
cvar_t	r_fullbright = {"r_fullbright", "0", CVAR_NONE};
cvar_t	r_lightmap = {"r_lightmap", "0", CVAR_NONE};
cvar_t	r_shadows = {"r_shadows", "0", CVAR_ARCHIVE};
cvar_t	r_mirroralpha = {"r_mirroralpha", "1", CVAR_NONE};
cvar_t	r_wateralpha = {"r_wateralpha", "1", CVAR_ARCHIVE};
cvar_t	r_skyalpha = {"r_skyalpha", "0.67", CVAR_ARCHIVE};
cvar_t	r_dynamic = {"r_dynamic", "1", CVAR_ARCHIVE};
cvar_t	r_farclip = {"r_farclip", "4096", CVAR_ARCHIVE};
cvar_t	r_entdist = {"r_entdist", "0", CVAR_ARCHIVE};	/* entity draw distance (0=unlimited) */
cvar_t	r_viewmodel_fov = {"r_viewmodel_fov", "0", CVAR_ARCHIVE};
cvar_t	cl_gun_fovscale = {"cl_gun_fovscale", "1", CVAR_ARCHIVE};
cvar_t	r_lavaalpha = {"r_lavaalpha", "0", CVAR_ARCHIVE};
cvar_t	r_slimealpha = {"r_slimealpha", "0", CVAR_ARCHIVE};
cvar_t	r_telealpha = {"r_telealpha", "0", CVAR_ARCHIVE};
/* Catch-all turb alpha for custom mod liquid names not matched by the
 * water/ice/glass/lava/slime/tele branches in R_LiquidAlpha.  Default 1
 * (opaque) preserves the uhexen2-6697 fix for Arena.bsp double-layer
 * turbs.  Modders with translucent custom liquids that don't match the
 * substring heuristics can set this to 0.7 in autoexec. */
cvar_t	r_turbalpha = {"r_turbalpha", "1", CVAR_ARCHIVE};
/* Heal T-junctions between adjacent turb (water/lava/slime) surfaces at
 * map load.  Mappers commonly build a lava pool out of many small brushes;
 * each brush is subdivided independently, so adjacent surfaces don't share
 * a common vertex on every shared edge.  The per-vertex sin warp +
 * Z-ripple amplifies the gap at every T-junction into a visible crack.
 * Fix: walk all turb polys after subdivision, find vertices that lie
 * strictly inside another turb poly's edge, and insert them as Steiner
 * points so the warp math produces a continuous output across the seam.
 * uhexen2-9o7u. */
cvar_t	r_turbtjunc = {"r_turbtjunc", "1", CVAR_ARCHIVE};
cvar_t	r_novis = {"r_novis", "0", CVAR_NONE};
cvar_t	r_wholeframe = {"r_wholeframe", "1", CVAR_ARCHIVE};
cvar_t	r_lerpmodels = {"r_lerpmodels", "1", CVAR_ARCHIVE};	/* smooth model animation interpolation */
cvar_t	r_animsmoothing = {"r_animsmoothing", "0", CVAR_ARCHIVE};	/* smooth long server-timed animations by lerping over the observed inter-frame interval instead of a fixed 0.1s (Ironwail LERP_FINISH approximation, uhexen2-wax3) */
cvar_t	r_alphasort = {"r_alphasort", "1", CVAR_ARCHIVE};
cvar_t	r_showbboxes = {"r_showbboxes", "0", CVAR_NONE};
cvar_t	r_showbboxes_think = {"r_showbboxes_think", "0", CVAR_NONE};	/* >0 = thinkers only, <0 = non-thinkers only (Ironwail parity) */
cvar_t	r_showbboxes_health = {"r_showbboxes_health", "0", CVAR_NONE};	/* >0 = health>0 only, <0 = health<=0 only (Ironwail parity) */
cvar_t	r_showbboxes_targets = {"r_showbboxes_targets", "0", CVAR_NONE};	/* 1 = highlight target/targetname matches of the focused entity (Ironwail parity) */
cvar_t	r_clearcolor = {"r_clearcolor", "0", CVAR_ARCHIVE};
cvar_t	r_texture_external = {"r_texture_external", "0", CVAR_ARCHIVE};
cvar_t	r_texture_external_hud = {"r_texture_external_hud", "0", CVAR_ARCHIVE};

cvar_t	gl_clear = {"gl_clear", "1", CVAR_NONE};
cvar_t	gl_cull = {"gl_cull", "1", CVAR_NONE};
cvar_t	gl_zfix = {"gl_zfix", "1", CVAR_ARCHIVE};
cvar_t	gl_smoothmodels = {"gl_smoothmodels", "1", CVAR_NONE};
cvar_t	gl_polyblend = {"gl_polyblend", "1", CVAR_NONE};
cvar_t	gl_cshiftpercent = {"gl_cshiftpercent", "100", CVAR_ARCHIVE};	/* global polyblend intensity 0-100 */
cvar_t	gl_flashblend = {"gl_flashblend", "0", CVAR_ARCHIVE};
cvar_t	gl_playermip = {"gl_playermip", "0", CVAR_NONE};
cvar_t	gl_nocolors = {"gl_nocolors", "0", CVAR_NONE};
cvar_t	gl_keeptjunctions = {"gl_keeptjunctions", "1", CVAR_ARCHIVE};
cvar_t	gl_reporttjunctions = {"gl_reporttjunctions", "0", CVAR_NONE};
cvar_t	gl_waterripple = {"gl_waterripple", "2", CVAR_ARCHIVE};
cvar_t	gl_particles = {"gl_particles", "1", CVAR_ARCHIVE};	// 0=square, 1=round (default)
cvar_t	gl_fullbrights = {"gl_fullbrights", "1", CVAR_ARCHIVE};	// fullbright pixel overlay on models
cvar_t	gl_overbright_models = {"gl_overbright_models", "1", CVAR_ARCHIVE};	// clamp alias model lighting
cvar_t	gl_fxaa = {"gl_fxaa", "0", CVAR_ARCHIVE};		// FXAA post-process anti-aliasing
cvar_t	gl_lmatlas = {"gl_lmatlas", "1", CVAR_ARCHIVE};	// lightmap atlas (0 to disable)
cvar_t	gl_glows = {"gl_glows", "1", CVAR_ARCHIVE};
cvar_t	gl_other_glows = {"gl_other_glows", "1", CVAR_ARCHIVE};
cvar_t	gl_missile_glows = {"gl_missile_glows", "1", CVAR_ARCHIVE};
cvar_t	gl_torch_dlight = {"gl_torch_dlight", "1", CVAR_ARCHIVE};
cvar_t	gl_glow_intensity = {"gl_glow_intensity", "1", CVAR_ARCHIVE};
cvar_t	gl_flashintensity = {"gl_flashintensity", "1", CVAR_ARCHIVE};

cvar_t	gl_coloredlight = {"gl_coloredlight", "1", CVAR_ARCHIVE};
cvar_t	gl_extra_dynamic_lights = {"gl_extra_dynamic_lights", "1", CVAR_NONE};

/*
=================
R_LerpEntity

Computes interpolated origin/angles for render-time smoothing.
Called before drawing each entity. Detects when the entity's
physics origin changes (server tick) and blends between the
previous and current positions using cl.lerpfrac.
=================
*/
void R_LerpEntity (entity_t *e, vec3_t out_origin, vec3_t out_angles)
{
	// entities are now interpolated by CL_RelinkEntities via CL_LerpPoint
	// (which runs every render frame). Just pass through the current values.
	VectorCopy (e->origin, out_origin);
	VectorCopy (e->angles, out_angles);
}

//=============================================================================


/*
=================
R_CullBox

Returns true if the box is completely outside the frustom
=================
*/
qboolean R_CullBox (vec3_t mins, vec3_t maxs)
{
	int		i;

	for (i = 0; i < 4; i++)
	{
		if (BoxOnPlaneSide (mins, maxs, &frustum[i]) == 2)
			return true;
	}
	return false;
}


/*
=================
R_RotateForEntity
=================
*/
void R_RotateForEntity (entity_t *e)
{
	GL_Translatef (e->origin[0], e->origin[1], e->origin[2]);
	GL_Rotatef (e->angles[1], 0, 0, 1);
	GL_Rotatef (-e->angles[0], 0, 1, 0);
	GL_Rotatef (-e->angles[2], 1, 0, 0);
}

/*
=================
R_RotateForEntity2

Same as R_RotateForEntity(), but checks for
EF_ROTATE and modifies yaw appropriately.
=================
*/
static void R_RotateForEntity2 (entity_t *e)
{
	float	forward, yaw, pitch;
	vec3_t			angles;

	GL_Translatef(e->origin[0], e->origin[1], e->origin[2]);

	if (e->model->flags & EF_FACE_VIEW)
	{
		VectorSubtract(e->origin,r_origin,angles);
		VectorSubtract(r_origin,e->origin,angles);
		VectorNormalize(angles);

		if (angles[1] == 0 && angles[0] == 0)
		{
			yaw = 0;
			if (angles[2] > 0)
				pitch = 90;
			else
				pitch = 270;
		}
		else
		{
			yaw = (int) (atan2(angles[1], angles[0]) * 180 / M_PI);
			if (yaw < 0)
				yaw += 360;

			forward = sqrt (angles[0]*angles[0] + angles[1]*angles[1]);
			pitch = (int) (atan2(angles[2], forward) * 180 / M_PI);
			if (pitch < 0)
				pitch += 360;
		}

		angles[0] = pitch;
		angles[1] = yaw;
		angles[2] = 0;

		GL_Rotatef (-angles[0], 0, 1, 0);
		GL_Rotatef (angles[1], 0, 0, 1);
		GL_Rotatef (-e->angles[2], 1, 0, 0);
	}
	else
	{
		if ((e->model->flags & EF_ROTATE) ||
		    (R_GetPimpFlags(e, NULL) & EF_SPIN))
		{
			GL_Rotatef (anglemod((e->origin[0] + e->origin[1])*0.8
								+ (108*cl.time)),
						    0, 0, 1);
		}
		else
		{
			GL_Rotatef (e->angles[1], 0, 0, 1);
		}

		GL_Rotatef (-e->angles[0], 0, 1, 0);
		GL_Rotatef (-e->angles[2], 1, 0, 0);
	}
}

/*
=============================================================

SPRITE MODELS

=============================================================
*/

/*
================
R_GetSpriteFrame
================
*/
static mspriteframe_t *R_GetSpriteFrame (entity_t *e)
{
	msprite_t	*psprite;
	mspritegroup_t	*pspritegroup;
	mspriteframe_t	*pspriteframe;
	int			i, numframes, frame;
	float		*pintervals, fullinterval, targettime, time;

	psprite = (msprite_t *) e->model->cache.data;
	frame = e->frame;

	if ((frame >= psprite->numframes) || (frame < 0))
	{
		Con_DPrintf ("%s: no such frame %d for %s\n", __thisfunc__, frame, e->model->name);
		frame = 0;
	}

	if (psprite->frames[frame].type == SPR_SINGLE)
	{
		pspriteframe = psprite->frames[frame].frameptr;
	}
	else
	{
		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		pintervals = pspritegroup->intervals;
		numframes = pspritegroup->numframes;
		fullinterval = pintervals[numframes-1];

		time = cl.time + e->syncbase;

	// when loading in Mod_LoadSpriteGroup, we guaranteed all interval values
	// are positive, so we don't have to worry about division by 0
		targettime = time - ((int)(time / fullinterval)) * fullinterval;

		for (i = 0; i < (numframes-1); i++)
		{
			if (pintervals[i] > targettime)
				break;
		}

		pspriteframe = pspritegroup->frames[i];
	}

	return pspriteframe;
}


/*
=================
R_DrawSpriteModel

=================
*/
typedef struct
{
	vec3_t		vup, vright, vpn;	// in worldspace
} spritedesc_t;

static void R_DrawSpriteModel (entity_t *e)
{
	vec3_t		point;
	mspriteframe_t	*frame;
	msprite_t	*psprite;
	vec3_t		tvec;
	float		dot, angle, sr, cr;
	spritedesc_t	r_spritedesc;
	int			i;

	frame = R_GetSpriteFrame (e);
	psprite = (msprite_t *) e->model->cache.data;

	if (psprite->type == SPR_FACING_UPRIGHT)
	{
	// generate the sprite's axes, with vup straight up in worldspace, and
	// r_spritedesc.vright perpendicular to modelorg.
	// This will not work if the view direction is very close to straight up or
	// down, because the cross product will be between two nearly parallel
	// vectors and starts to approach an undefined state, so we don't draw if
	// the two vectors are less than 1 degree apart
		tvec[0] = -modelorg[0];
		tvec[1] = -modelorg[1];
		tvec[2] = -modelorg[2];
		VectorNormalize (tvec);
		dot = tvec[2];	// same as DotProduct (tvec, r_spritedesc.vup)
				// because r_spritedesc.vup is 0, 0, 1

		if ((dot > 0.999848) || (dot < -0.999848))	// cos(1 degree) = 0.999848
			return;

		r_spritedesc.vup[0] = 0;
		r_spritedesc.vup[1] = 0;
		r_spritedesc.vup[2] = 1;
		r_spritedesc.vright[0] = tvec[1];
								// CrossProduct (r_spritedesc.vup, -modelorg,
		r_spritedesc.vright[1] = -tvec[0];
								//		 r_spritedesc.vright)
		r_spritedesc.vright[2] = 0;
		VectorNormalize (r_spritedesc.vright);
		r_spritedesc.vpn[0] = -r_spritedesc.vright[1];
		r_spritedesc.vpn[1] = r_spritedesc.vright[0];
		r_spritedesc.vpn[2] = 0;
					// CrossProduct (r_spritedesc.vright, r_spritedesc.vup,
					//		 r_spritedesc.vpn)
	}
	else if (psprite->type == SPR_VP_PARALLEL)
	{
	// generate the sprite's axes, completely parallel to the viewplane. There
	// are no problem situations, because the sprite is always in the same
	// position relative to the viewer
		for (i = 0; i < 3; i++)
		{
			r_spritedesc.vup[i] = vup[i];
			r_spritedesc.vright[i] = vright[i];
			r_spritedesc.vpn[i] = vpn[i];
		}
	}
	else if (psprite->type == SPR_VP_PARALLEL_UPRIGHT)
	{
	// generate the sprite's axes, with vup straight up in worldspace, and
	// r_spritedesc.vright parallel to the viewplane.
	// This will not work if the view direction is very close to straight up or
	// down, because the cross product will be between two nearly parallel
	// vectors and starts to approach an undefined state, so we don't draw if
	// the two vectors are less than 1 degree apart
		dot = vpn[2];	// same as DotProduct (vpn, r_spritedesc.vup)
				// because r_spritedesc.vup is 0, 0, 1

		if ((dot > 0.999848) || (dot < -0.999848))	// cos(1 degree) = 0.999848
			return;

		r_spritedesc.vup[0] = 0;
		r_spritedesc.vup[1] = 0;
		r_spritedesc.vup[2] = 1;
		r_spritedesc.vright[0] = vpn[1];
							// CrossProduct (r_spritedesc.vup, vpn,
		r_spritedesc.vright[1] = -vpn[0];	//		 r_spritedesc.vright)
		r_spritedesc.vright[2] = 0;
		VectorNormalize (r_spritedesc.vright);
		r_spritedesc.vpn[0] = -r_spritedesc.vright[1];
		r_spritedesc.vpn[1] = r_spritedesc.vright[0];
		r_spritedesc.vpn[2] = 0;
					// CrossProduct (r_spritedesc.vright, r_spritedesc.vup,
					//		 r_spritedesc.vpn)
	}
	else if (psprite->type == SPR_ORIENTED)
	{
	// generate the sprite's axes, according to the sprite's world orientation
		AngleVectors (e->angles, r_spritedesc.vpn, r_spritedesc.vright, r_spritedesc.vup);
	}
	else if (psprite->type == SPR_VP_PARALLEL_ORIENTED)
	{
	// generate the sprite's axes, parallel to the viewplane, but rotated in
	// that plane around the center according to the sprite entity's roll
	// angle. So vpn stays the same, but vright and vup rotate
		angle = e->angles[ROLL] * (M_PI*2 / 360);
		sr = sin(angle);
		cr = cos(angle);

		for (i = 0; i < 3; i++)
		{
			r_spritedesc.vpn[i] = vpn[i];
			r_spritedesc.vright[i] = vright[i] * cr + vup[i] * sr;
			r_spritedesc.vup[i] = vright[i] * -sr + vup[i] * cr;
		}
	}
	else
	{
		Sys_Error ("%s: Bad sprite type %d", __thisfunc__, psprite->type);
	}

	/* translucency handling.  Inside an OIT pass the FBO has two
	 * blend-enabled MRT attachments configured by OIT_BeginTranslucency
	 * (accum + revealage).  Disabling GL_BLEND there would clobber both
	 * attachments with REPLACE writes — so opaque sprites must still
	 * keep blend on and just contribute α=1.0 through the OIT formula. */
	if ((e->drawflags & DRF_TRANSLUCENT) || (e->model->flags & EF_TRANSPARENT) ||
	    (e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha)))
	{
		glEnable_fp (GL_BLEND);
		if (!OIT_InPass())
			glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else if (!OIT_InPass())
	{
		glDisable_fp (GL_BLEND);
	}

	GL_Bind(frame->gl_texturenum);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin ();

	{
		float sprite_alpha;
		if (e->alpha != ENTALPHA_DEFAULT)
			sprite_alpha = ENTALPHA_DECODE(e->alpha);
		else if ((e->drawflags & DRF_TRANSLUCENT) || (e->model->flags & EF_TRANSPARENT))
			sprite_alpha = r_wateralpha.value;
		else
			sprite_alpha = 1.0f;
		GL_ImmColor4f (1.0f, 1.0f, 1.0f, sprite_alpha);
	}

	GL_ImmTexCoord2f (0, 1);
	VectorMA (e->origin, frame->down, r_spritedesc.vup, point);
	VectorMA (point, frame->left, r_spritedesc.vright, point);
	GL_ImmVertex3f (point[0], point[1], point[2]);

	GL_ImmTexCoord2f (0, 0);
	VectorMA (e->origin, frame->up, r_spritedesc.vup, point);
	VectorMA (point, frame->left, r_spritedesc.vright, point);
	GL_ImmVertex3f (point[0], point[1], point[2]);

	GL_ImmTexCoord2f (1, 0);
	VectorMA (e->origin, frame->up, r_spritedesc.vup, point);
	VectorMA (point, frame->right, r_spritedesc.vright, point);
	GL_ImmVertex3f (point[0], point[1], point[2]);

	GL_ImmTexCoord2f (1, 1);
	VectorMA (e->origin, frame->down, r_spritedesc.vup, point);
	VectorMA (point, frame->right, r_spritedesc.vright, point);
	GL_ImmVertex3f (point[0], point[1], point[2]);

	GL_ImmEnd (GL_QUADS, OIT_InPass() ? &gl_shader_alias_oit : &gl_shader_alias);

// restore tex parms
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	GL_SetAlphaThreshold(0.01f);
	/* During an OIT pass leave GL_BLEND on and let OIT_EndTranslucency
	 * restore state — disabling it here would break a later draw in
	 * the same OIT pass. */
	if (!OIT_InPass())
		glDisable_fp (GL_BLEND);
}


/*
=============================================================

ALIAS MODELS

=============================================================
*/

static float	shadelight, ambientlight;

// precalculated dot products for quantized angles
#define SHADEDOT_QUANT		16
float	r_avertexnormal_dots[SHADEDOT_QUANT][256] =
{
#include "anorm_dots.h"
};

static float	*shadedots = r_avertexnormal_dots[0];
static int	shadedot_row_index;	/* row index for GPU path (0-15) */
static vec3_t	shadevector;

static int	lastposenum;

/*
=============
GL_DrawAliasFrame
=============
*/
static void GL_DrawAliasFrame (entity_t *e, aliashdr_t *paliashdr, int posenum, int prevposenum, float lerpfrac)
{
	float		l;
	trivertx_t	*verts, *verts_prev;
	int		*order;
	int		count;
	float		r, g, b;
	byte		ColorShade;
	qboolean	do_lerp;

	lastposenum = posenum;

	verts = (trivertx_t *)((byte *)paliashdr + paliashdr->posedata);
	verts += posenum * paliashdr->poseverts;

	do_lerp = (lerpfrac > 0.0f && lerpfrac < 1.0f && prevposenum != posenum);
	if (do_lerp)
	{
		verts_prev = (trivertx_t *)((byte *)paliashdr + paliashdr->posedata);
		verts_prev += prevposenum * paliashdr->poseverts;
	}
	else
		verts_prev = NULL;

	order = (int *)((byte *)paliashdr + paliashdr->commands);

	ColorShade = e->colorshade;

	if (ColorShade)
	{
		r = RTint[ColorShade];
		g = GTint[ColorShade];
		b = BTint[ColorShade];
	}
	else
		r = g = b = 1;

	/* Batch all strips/fans as triangles in one draw call.
	 * Converts strips and fans to explicit triangles to avoid
	 * per-primitive GL_ImmBegin/End overhead (5-15x fewer draws). */
	{
	/* Temp arrays for current strip/fan vertices */
	static float tmp_tc[128][2];
	static float tmp_color[128][4];
	static float tmp_pos[128][3];
	int vi;

	GL_ImmBegin ();

	while (1)
	{
		qboolean is_fan;

		count = *order++;
		if (!count)
			break;
		if (count < 0)
		{
			count = -count;
			is_fan = true;
		}
		else
			is_fan = false;

		if (count > 128) count = 128;

		/* Collect strip/fan vertices into temp arrays */
		for (vi = 0; vi < count; vi++)
		{
			tmp_tc[vi][0] = ((float *)order)[0];
			tmp_tc[vi][1] = ((float *)order)[1];
			order += 2;

			if (model_fullbright_pass)
			{
				tmp_color[vi][0] = tmp_color[vi][1] = tmp_color[vi][2] = tmp_color[vi][3] = 1;
			}
			else if (gl_lightmap_format == GL_RGBA)
			{
				l = shadedots[verts->lightnormalindex];
				tmp_color[vi][0] = l * lightcolor[0];
				tmp_color[vi][1] = l * lightcolor[1];
				tmp_color[vi][2] = l * lightcolor[2];
				tmp_color[vi][3] = model_constant_alpha;
			}
			else
			{
				l = shadedots[verts->lightnormalindex] * shadelight;
				tmp_color[vi][0] = r*l;
				tmp_color[vi][1] = g*l;
				tmp_color[vi][2] = b*l;
				tmp_color[vi][3] = model_constant_alpha;
			}

			if (do_lerp)
			{
				tmp_pos[vi][0] = verts_prev->v[0] + (verts->v[0] - verts_prev->v[0]) * lerpfrac;
				tmp_pos[vi][1] = verts_prev->v[1] + (verts->v[1] - verts_prev->v[1]) * lerpfrac;
				tmp_pos[vi][2] = verts_prev->v[2] + (verts->v[2] - verts_prev->v[2]) * lerpfrac;
				verts_prev++;
			}
			else
			{
				tmp_pos[vi][0] = verts->v[0];
				tmp_pos[vi][1] = verts->v[1];
				tmp_pos[vi][2] = verts->v[2];
			}
			verts++;
		}

		/* Check buffer space */
		if (GL_ImmCount() + (count - 2) * 3 >= GL_IMM_MAX_VERTS - 6)
		{
			GL_ImmEnd (GL_TRIANGLES, OIT_InPass() ? &gl_shader_alias_oit : &gl_shader_alias);
			GL_ImmBegin ();
		}

		/* Convert to triangles and emit */
		if (is_fan)
		{
			for (vi = 2; vi < count; vi++)
			{
#define EMIT_VERT(idx) do { \
	GL_ImmTexCoord2f(tmp_tc[idx][0], tmp_tc[idx][1]); \
	GL_ImmColor4f(tmp_color[idx][0], tmp_color[idx][1], tmp_color[idx][2], tmp_color[idx][3]); \
	GL_ImmVertex3f(tmp_pos[idx][0], tmp_pos[idx][1], tmp_pos[idx][2]); \
} while(0)
				EMIT_VERT(0);
				EMIT_VERT(vi - 1);
				EMIT_VERT(vi);
			}
		}
		else
		{
			/* triangle strip → triangles */
			for (vi = 2; vi < count; vi++)
			{
				if (vi & 1)
				{
					EMIT_VERT(vi);
					EMIT_VERT(vi - 1);
					EMIT_VERT(vi - 2);
				}
				else
				{
					EMIT_VERT(vi - 2);
					EMIT_VERT(vi - 1);
					EMIT_VERT(vi);
				}
			}
#undef EMIT_VERT
		}
	}

	GL_ImmEnd (GL_TRIANGLES, OIT_InPass() ? &gl_shader_alias_oit : &gl_shader_alias);
	}
}


/* r_alias_gpu: 0 = CPU streaming, 1 = SSBO instanced batching */
#ifdef __EMSCRIPTEN__
cvar_t	r_alias_gpu = {"r_alias_gpu", "0", CVAR_NONE};	/* no SSBOs in WebGL2 */
#else
cvar_t	r_alias_gpu = {"r_alias_gpu", "1", CVAR_ARCHIVE};
#endif

/* r_brush_inst: 0 = legacy per-entity R_DrawBrushModel path,
 * 1 = unified-shader collected dispatch via R_DrawBrushInstanced.
 * The 1 path runs through gl_shader_world (same compiled program as
 * world surfaces), so within-shader gl_Position invariance covers
 * coplanar joins between brush ents and the world.  Default on. */
cvar_t	r_brush_inst = {"r_brush_inst", "1", CVAR_ARCHIVE};

/* r_brush_inst_offset: tunable polygon-offset magnitude for the brush-ent
 * dispatch.  Default 0 — with the unified shader (uhexen2-mf45) brush
 * ents and world surfaces share gl_shader_world, so within-shader
 * invariant gl_Position covers coplanar joins and no offset is needed.
 * Kept as a safety net; sign auto-flipped for reversed-Z. */
cvar_t	r_brush_inst_offset = {"r_brush_inst_offset", "0.0", 0};

/*
=============
GL_DrawAliasShadow -- sezero projected mesh shadow
=============
*/
static void GL_DrawAliasShadow (entity_t *e, aliashdr_t *paliashdr, int posenum)
{
	trivertx_t	*verts;
	int		*order;
	vec3_t		point;
	float		height, lheight;
	int		count;

	lheight = e->origin[2] - lightspot[2];

	height = -lheight + 1.0;
	verts = (trivertx_t *)((byte *)paliashdr + paliashdr->posedata);
	verts += posenum * paliashdr->poseverts;
	order = (int *)((byte *)paliashdr + paliashdr->commands);

	if (have_stencil)
	{
		glEnable_fp(GL_STENCIL_TEST);
		glStencilFunc_fp(GL_EQUAL, 1, 2);
		glStencilOp_fp(GL_KEEP, GL_KEEP, GL_INCR);
	}

	while (1)
	{
		GLenum mode;

		count = *order++;
		if (!count)
			break;
		if (count < 0)
		{
			count = -count;
			mode = GL_TRIANGLE_FAN;
		}
		else
			mode = GL_TRIANGLE_STRIP;

		GL_ImmBegin();
		GL_ImmColor4f(0, 0, 0, 0.5f);
		do
		{
			order += 2;	/* skip texture coordinates */

			point[0] = verts->v[0] * paliashdr->scale[0] + paliashdr->scale_origin[0];
			point[1] = verts->v[1] * paliashdr->scale[1] + paliashdr->scale_origin[1];
			point[2] = verts->v[2] * paliashdr->scale[2] + paliashdr->scale_origin[2];

			point[0] -= shadevector[0] * (point[2] + lheight);
			point[1] -= shadevector[1] * (point[2] + lheight);
			point[2] = height;

			GL_ImmVertex3f(point[0], point[1], point[2]);

			verts++;
		} while (--count);
		GL_ImmEnd(mode, &gl_shader_flat);
	}

	if (have_stencil)
		glDisable_fp(GL_STENCIL_TEST);
}


/*
=================
R_SetupAliasFrame

=================
*/
static void R_SetupAliasFrame (entity_t *e, aliashdr_t *paliashdr)
{
	int	pose, prevpose, numposes, frame, prevframe;
	float	interval, lerpfrac;

	frame = e->frame;
	if ((frame >= paliashdr->numframes) || (frame < 0))
	{
		Con_DPrintf ("%s: no such frame %d for %s\n", __thisfunc__, frame, e->model->name);
		frame = 0;
	}

	pose = paliashdr->frames[frame].firstpose;
	numposes = paliashdr->frames[frame].numposes;

	if (numposes > 1)
	{
		interval = paliashdr->frames[frame].interval;
		pose += (int)(cl.time / interval) % numposes;
	}

	/* Calculate animation interpolation */
	prevframe = e->previouspose;
	lerpfrac = 0.0f;

	if ((e->lerpflags & LERP_RESETANIM) || model_fullbright_pass)
	{
		/* Skip animation lerp after teleport or during fullbright pass */
		if (e->lerpflags & LERP_RESETANIM)
		{
			e->lerpflags &= ~LERP_RESETANIM;
			e->lerptime = 0;
		}
	}
	else if (r_lerpmodels.integer && e->lerptime > 0 && prevframe != frame &&
	    prevframe >= 0 && prevframe < paliashdr->numframes)
	{
		float elapsed = cl.time - e->lerpstart;
		lerpfrac = elapsed / e->lerptime;
		if (lerpfrac >= 1.0f)
			lerpfrac = 0.0f;	/* lerp complete, use current frame */
		else
			lerpfrac = 1.0f - lerpfrac; /* invert: 1=prev, 0=current */
	}

	if (lerpfrac > 0.0f)
	{
		prevpose = paliashdr->frames[prevframe].firstpose;
		numposes = paliashdr->frames[prevframe].numposes;
		if (numposes > 1)
		{
			interval = paliashdr->frames[prevframe].interval;
			prevpose += (int)(e->lerpstart / interval) % numposes;
		}
		GL_DrawAliasFrame(e, paliashdr, pose, prevpose, 1.0f - lerpfrac);
	}
	else
	{
		GL_DrawAliasFrame(e, paliashdr, pose, pose, 0.0f);
	}
}


/*
=================
R_DrawAliasModel

=================
*/
static void AliasModelGetLightInfo (entity_t *e)
{
	vec3_t		adjust_origin;

	VectorCopy(e->origin, adjust_origin);
	adjust_origin[2] += (e->model->mins[2] + e->model->maxs[2]) / 2;
	if (gl_lightmap_format == GL_RGBA)
		ambientlight = shadelight = R_LightPointColor (adjust_origin);
	else
		ambientlight = shadelight = R_LightPoint (adjust_origin);
}

static void R_DrawAliasModel (entity_t *e)
{
	int		i;
	int		lnum;
	vec3_t		dist;
	float		add;
	qmodel_t	*clmodel;
	vec3_t		mins, maxs;
	aliashdr_t	*paliashdr;
	static float	tmatrix[3][4];
	float		an, entScale;
	float		xyfact = 1.0, zfact = 1.0; // avoid compiler warning
	int		skinnum;
	int		mls;

	clmodel = e->model;

	VectorAdd (e->origin, clmodel->mins, mins);
	VectorAdd (e->origin, clmodel->maxs, maxs);

	if (!AlwaysDrawModel && R_CullBox (mins, maxs))
		return;

	VectorCopy (e->origin, r_entorigin);
	VectorSubtract (r_origin, r_entorigin, modelorg);

	// if shadows are enabled, get lighting information here regardless
	// of special cases below, because R_LightPoint[Color]() calculates
	// lightspot for us which is used by GL_DrawAliasShadow()
	if (r_shadows.integer && e != &cl.viewent)
		AliasModelGetLightInfo (e);

	mls = e->drawflags & MLS_MASKIN;
	if ((e->model->flags & EF_ROTATE) ||
	    (R_GetPimpFlags(e, NULL) & (EF_SPIN | EF_FLOAT)))
	{
		ambientlight = shadelight =
		lightcolor[0] =
		lightcolor[1] =
		lightcolor[2] =
				60 + 34 + sin(e->origin[0] + e->origin[1] + (cl.time*3.8)) * 34;
	}
	else if (mls == MLS_ABSLIGHT)
	{
		lightcolor[0] =
		lightcolor[1] =
		lightcolor[2] =
		ambientlight =
		shadelight =
				e->abslight;
	}
	else if (mls != MLS_NONE)
	{
		// Use a model light style (25-30)
		lightcolor[0] =
		lightcolor[1] =
		lightcolor[2] =
		ambientlight =
		shadelight =
				d_lightstylevalue[24+mls]/2;
	}
	else if (e != &cl.viewent)	// R_DrawViewModel() already does viewmodel lighting.
	{
		if (!r_shadows.integer)
			AliasModelGetLightInfo (e);

		for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
		{
			if (cl_dlights[lnum].die >= cl.time)
			{
				VectorSubtract (e->origin, cl_dlights[lnum].origin, dist);
				add = cl_dlights[lnum].radius - VectorLengthFast(dist);
				if (add > 0)
				{
					if (cl_dlights[lnum].dark)
					{
						ambientlight -= add;
						if (ambientlight < 0)
							ambientlight = 0;
						lightcolor[0] -= (cl_dlights[lnum].color[0] * add);
						if (lightcolor[0] < 0)
							lightcolor[0] = 0;
						lightcolor[1] -= (cl_dlights[lnum].color[1] * add);
						if (lightcolor[1] < 0)
							lightcolor[1] = 0;
						lightcolor[2] -= (cl_dlights[lnum].color[2] * add);
						if (lightcolor[2] < 0)
							lightcolor[2] = 0;
					}
					else
					{
						ambientlight += add;
						lightcolor[0] += (cl_dlights[lnum].color[0] * add);
						lightcolor[1] += (cl_dlights[lnum].color[1] * add);
						lightcolor[2] += (cl_dlights[lnum].color[2] * add);
					}
				}
			}
		}

		// clamp lighting so it doesn't overbright as much
		if (gl_overbright_models.integer)
		{
			if (ambientlight > 128)
				ambientlight = 128;
			if (ambientlight + shadelight > 192)
				shadelight = 192 - ambientlight;

			// clamp lightcolor channels to match shade clamping
			for (i = 0; i < 3; i++)
			{
				if (lightcolor[i] > 192)
					lightcolor[i] = 192;
			}
		}
	}

	shadedot_row_index = ((int)(e->angles[1] * (SHADEDOT_QUANT / 360.0))) & (SHADEDOT_QUANT - 1);
	shadedots = r_avertexnormal_dots[shadedot_row_index];
	shadelight = shadelight / 200.0;
	VectorScale(lightcolor, 1.0f / 200.0f, lightcolor);

	an = e->angles[1] / 180 * M_PI;
	shadevector[0] = cos(-an);
	shadevector[1] = sin(-an);
	shadevector[2] = 1;
	VectorNormalize (shadevector);

	//
	// locate the proper data
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);

	c_alias_polys += paliashdr->numtris;

	//
	// draw all the triangles
	//
	GL_PushMatrix();
	R_RotateForEntity2(e);

	if (e->scale != 0 && e->scale != 100)
	{
		entScale = (float)e->scale / 100.0f;
		switch (e->drawflags & SCALE_TYPE_MASKIN)
		{
		case SCALE_TYPE_UNIFORM:
			tmatrix[0][0] = paliashdr->scale[0]*entScale;
			tmatrix[1][1] = paliashdr->scale[1]*entScale;
			tmatrix[2][2] = paliashdr->scale[2]*entScale;
			xyfact = zfact = (entScale-1.0)*127.95;
			break;
		case SCALE_TYPE_XYONLY:
			tmatrix[0][0] = paliashdr->scale[0]*entScale;
			tmatrix[1][1] = paliashdr->scale[1]*entScale;
			tmatrix[2][2] = paliashdr->scale[2];
			xyfact = (entScale-1.0)*127.95;
			zfact = 1.0;
			break;
		case SCALE_TYPE_ZONLY:
			tmatrix[0][0] = paliashdr->scale[0];
			tmatrix[1][1] = paliashdr->scale[1];
			tmatrix[2][2] = paliashdr->scale[2]*entScale;
			xyfact = 1.0;
			zfact = (entScale-1.0)*127.95;
			break;
		}

		switch (e->drawflags & SCALE_ORIGIN_MASKIN)
		{
		case SCALE_ORIGIN_CENTER:
			tmatrix[0][3] = paliashdr->scale_origin[0]-paliashdr->scale[0]*xyfact;
			tmatrix[1][3] = paliashdr->scale_origin[1]-paliashdr->scale[1]*xyfact;
			tmatrix[2][3] = paliashdr->scale_origin[2]-paliashdr->scale[2]*zfact;
			break;
		case SCALE_ORIGIN_BOTTOM:
			tmatrix[0][3] = paliashdr->scale_origin[0]-paliashdr->scale[0]*xyfact;
			tmatrix[1][3] = paliashdr->scale_origin[1]-paliashdr->scale[1]*xyfact;
			tmatrix[2][3] = paliashdr->scale_origin[2];
			break;
		case SCALE_ORIGIN_TOP:
			tmatrix[0][3] = paliashdr->scale_origin[0]-paliashdr->scale[0]*xyfact;
			tmatrix[1][3] = paliashdr->scale_origin[1]-paliashdr->scale[1]*xyfact;
			tmatrix[2][3] = paliashdr->scale_origin[2]-paliashdr->scale[2]*zfact*2.0;
			break;
		}
	}
	else
	{
		tmatrix[0][0] = paliashdr->scale[0];
		tmatrix[1][1] = paliashdr->scale[1];
		tmatrix[2][2] = paliashdr->scale[2];
		tmatrix[0][3] = paliashdr->scale_origin[0];
		tmatrix[1][3] = paliashdr->scale_origin[1];
		tmatrix[2][3] = paliashdr->scale_origin[2];
	}

	if ((clmodel->flags & EF_ROTATE) ||
	    (R_GetPimpFlags(e, NULL) & EF_FLOAT))
	{
		// Floating motion
		tmatrix[2][3] += sin(e->origin[0] + e->origin[1] + (cl.time*3)) * 5.5;
	}

	if (e == &cl.viewent && scr_fov.value > 90.f && cl_gun_fovscale.value)
	{
		/* Ironwail-style viewmodel distortion correction: blend factor
		 * 0 = no correction (gun stretches with FOV), 1 = full correction
		 * (gun stays same apparent size as at 90 FOV). */
		float fovscale = tan(scr_fov.value * (0.5f * M_PI / 180.f));
		fovscale = 1.f + (fovscale - 1.f) * cl_gun_fovscale.value;
		GL_Translatef (tmatrix[0][3], tmatrix[1][3] * fovscale, tmatrix[2][3] * fovscale);
		GL_Scalef (tmatrix[0][0], tmatrix[1][1] * fovscale, tmatrix[2][2] * fovscale);
	}
	else
	{
		GL_Translatef (tmatrix[0][3], tmatrix[1][3], tmatrix[2][3]);
		GL_Scalef (tmatrix[0][0], tmatrix[1][1], tmatrix[2][2]);
	}

	if (e->model->flags & EF_SPECIAL_TRANS)
	{
		glEnable_fp (GL_BLEND);
		glBlendFunc_fp (GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
		model_constant_alpha = 1.0f;
		glDisable_fp (GL_CULL_FACE);
	}
	else if (e->drawflags & DRF_TRANSLUCENT)
	{
		glEnable_fp (GL_BLEND);
		glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		if (e->alpha != ENTALPHA_DEFAULT)
			model_constant_alpha = ENTALPHA_DECODE(e->alpha);
		else
			model_constant_alpha = r_wateralpha.value;
	}
	else if (e->model->flags & EF_TRANSPARENT)
	{
		glEnable_fp (GL_BLEND);
		glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		model_constant_alpha = 1.0f;
	}
	else if (e->model->flags & EF_HOLEY)
	{
		glDisable_fp (GL_BLEND);
		GL_SetAlphaThreshold(0.666f);	/* alpha test for see-through cutouts */
		if (r_alphatocoverage.integer)
			glEnable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
		model_constant_alpha = 1.0f;
	}
	else if (e->alpha != ENTALPHA_DEFAULT)
	{
		glEnable_fp (GL_BLEND);
		glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		model_constant_alpha = ENTALPHA_DECODE(e->alpha);
	}
	else
	{
		model_constant_alpha = 1.0f;
	}

	skinnum = e->skinnum;
	if (skinnum >= 100)
	{
		if (skinnum > 255)
			Sys_Error ("skinnum > 255");

		if (gl_extra_textures[skinnum - 100] == GL_UNUSED_TEXTURE) // Need to load it in
		{
			qpic_t		*stonepic;
			glpic_t		*gl;
			char		temp[80];

			q_snprintf (temp, sizeof(temp), "gfx/skin%d.lmp", skinnum);
			stonepic = Draw_CachePic(temp);
			gl = (glpic_t *)stonepic->data;
			gl_extra_textures[skinnum - 100] = gl->texnum;
		}

		GL_Bind(gl_extra_textures[skinnum - 100]);
	}
	else
	{
		int	anim = (int)(cl.time*10) & 3;

		if ((skinnum >= paliashdr->numskins) || (skinnum < 0))
		{
			Con_DPrintf ("%s: no such skin # %d on %s (has %d)\n",
				     __thisfunc__, skinnum,
				     e->model ? e->model->name : "<null>",
				     paliashdr->numskins);
			skinnum = 0;
		}
		GL_Bind(paliashdr->gl_texturenum[skinnum][anim]);

		// we can't dynamically colormap textures, so they are cached
		// seperately for the players.  Heads are just uncolored.
		if (e->colormap != vid.colormap && !gl_nocolors.integer)
		{
			if (e->model == player_models[0] ||
			    e->model == player_models[1] ||
			    e->model == player_models[2] ||
			    e->model == player_models[4] ||
			    e->model == player_models[3])
			{
				i = e - cl_entities - 1;
				if (i >= 0 && i < cl.maxclients)
				{
					GL_Bind(playertextures[i]);
				}
			}
		}
	}

	R_SetupAliasFrame (e, paliashdr);

	// Fullbright pass: render fullbright pixels with additive blending
	// Skip for translucent models — additive blend over transparency looks wrong
	if (gl_fullbrights.integer && skinnum < 100 &&
	    !((e->drawflags & DRF_TRANSLUCENT) ||
	      (e->model->flags & (EF_TRANSPARENT | EF_SPECIAL_TRANS)) ||
	      (e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha))))
	{
		int anim_fb = (int)(cl.time*10) & 3;
		GLuint fb_tex;

		if (skinnum >= paliashdr->numskins || skinnum < 0)
			skinnum = 0;
		fb_tex = paliashdr->gl_fb_texturenum[skinnum][anim_fb];

		if (fb_tex && !r_fullbright.integer)
		{
			/* Push fullbright pass toward the camera in NDC so its
			 * depth always wins against the base pass even when the
			 * GLSL compiler reorders gl_Position math (Mesa ignores
			 * the invariant qualifier). Sign flips with reversed-Z:
			 * forward-Z wants smaller Z, reversed-Z wants larger Z.
			 * uhexen2-iir3. */
			float fb_offset = gl_clipcontrol_able ? 1.0f : -1.0f;
			GL_Bind(fb_tex);
			glEnable_fp (GL_BLEND);
			glBlendFunc_fp (GL_ONE, GL_ONE);	// additive
			glDepthMask_fp (0);
			glEnable_fp (GL_POLYGON_OFFSET_FILL);
			glPolygonOffset_fp (fb_offset, fb_offset);
			GL_SetAlphaThreshold(0.01f);

			model_fullbright_pass = true;
			R_SetupAliasFrame (e, paliashdr);
			model_fullbright_pass = false;

			glDisable_fp (GL_POLYGON_OFFSET_FILL);
			glPolygonOffset_fp (0.0f, 0.0f);
			glDepthMask_fp (1);
			glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDisable_fp (GL_BLEND);
			GL_SetAlphaThreshold(0.01f);	/* restore default */
		}
	}

// restore params

	model_constant_alpha = 1.0f;

	if ((e->drawflags & DRF_TRANSLUCENT) ||
	    (e->model->flags & EF_SPECIAL_TRANS) ||
	    (e->model->flags & EF_TRANSPARENT) ||
	    (e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha)))
	{
		glDisable_fp (GL_BLEND);
	}

	if (e->model->flags & EF_SPECIAL_TRANS)
	{
		glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnable_fp (GL_CULL_FACE);
	}

	if (e->model->flags & EF_HOLEY)
	{
		if (r_alphatocoverage.integer)
			glDisable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	}

	GL_SetAlphaThreshold(0.01f);	/* restore default */

	GL_PopMatrix();

	if (r_shadows.integer && e != &cl.viewent)
	{
		GL_PushMatrix();
		R_RotateForEntity2(e);
		glEnable_fp (GL_BLEND);
		glDepthMask_fp (0);
		GL_DrawAliasShadow (e, paliashdr, lastposenum);
		glDepthMask_fp (1);
		glDisable_fp (GL_BLEND);
		GL_PopMatrix();
	}
}

//=============================================================================

typedef struct sortedent_s {
	entity_t	*ent;
	vec_t		len;
} sortedent_t;

static sortedent_t	cl_transvisedicts[MAX_VISEDICTS];
static sortedent_t	cl_transwateredicts[MAX_VISEDICTS];

static int			cl_numtransvisedicts;
static int			cl_numtranswateredicts;

/*
=============
Instanced alias model rendering (ES 3.0 compatible)
=============
*/

static alias_instance_t	alias_instances[MAX_ALIAS_INSTANCES];
static int		num_alias_instances;
/* alias_inst_view_proj — staging for the per-frame view-projection
 * uniform.  Was an SSBO header in the inst_ssbo prefix until uhexen2-8pc2
 * moved it out to a plain uniform mat4 (cleaner GL_Upload migration). */
static float		alias_inst_view_proj[16];
static alias_batch_t	alias_batches[MAX_ALIAS_BATCHES];
static int		num_alias_batches;
/* inst_ssbo removed in uhexen2-8pc2 — the streaming ring (gl_buffer.c)
 * provides per-frame storage with fence-guarded reuse, eliminating the
 * driver-implicit-sync hazard of the old static SSBO + glBufferSubData
 * pattern. */
static qboolean		inst_collected[MAX_VISEDICTS]; /* true if visedict was instanced */

/* Per-entity bookkeeping for shadow/batch passes after instanced draw */
typedef struct {
	entity_t	*ent;
	aliashdr_t	*hdr;
	int		pose;
	GLuint		skin_tex;	/* resolved skin texture */
	GLuint		fb_tex;		/* fullbright texture (0 if none) */
} inst_entity_t;
static inst_entity_t	inst_entities[MAX_ALIAS_INSTANCES];
static int		num_inst_entities;

/* Sort key for batch grouping */
typedef struct {
	int	instance_idx;
	size_t	model_key;	/* aliashdr_t pointer as integer */
	GLuint	skin_tex;
	GLuint	fb_tex;
} inst_sort_t;

/*
=============
Brush-submodel batched rendering (uhexen2-mf45)

R_CollectBrushInstances walks visible brush entities, frustum-culls,
captures a per-entity (mvp, mv) matrix snapshot, and emits one
world_surf_key_t per visible surface tagged with (texture, instance,
firstIndex, count).  The collector writes opaque and SURF_DRAWFENCE
surfaces into separate key arrays — fence draws go through the same
shader with a tighter alpha threshold + optional A2C.

R_DrawBrushInstanced sorts the keys by (instance, texture, first), then
dispatches them through gl_shader_world (the same compiled program as
world surfaces) with per-entity glUniformMatrix4fv + per-(instance,
texture) run-coalesced glDrawElements.  Using the same shader as world
surfaces means within-shader invariant gl_Position covers coplanar joins
between brush ents and the world (drawbridge/ground, portcullis/wall
pocket) — no cross-shader 1-ULP depth drift, no polygon-offset backstop.

Special surfaces (sky / turb / underwater-with-warp) on a brush entity
fall back to the legacy R_DrawBrushModelSpecialOnly path.  Translucent
ents (DRF_TRANSLUCENT, ENTALPHA, EF_TRANSPARENT, MLS_ABSLIGHT) are
skipped here and rendered via R_DrawBrushModel from R_DrawEntitiesOnList
or R_DrawTransEntitiesOnList.
=============
*/

/* Cap matches MAX_VISEDICTS so dense maps don't silently truncate the
 * brush-entity instance list.  uhexen2-l0ac. */
#define MAX_WORLD_INSTANCES	MAX_VISEDICTS
/* Soft caps on per-frame surf-key arrays.  Sized large vs. observed
 * counts (a few hundred opaque, ~1k fence in dense maps); a hit on
 * either is silently dropped. */
#define MAX_WORLD_SURF_KEYS	32768
#define MAX_WORLD_SURF_KEYS_FENCE	8192

typedef struct {
	float	mvp[16];	/* projection * view * entity_transform —
				 * captured per-frame in R_CollectBrushInstances,
				 * applied per-instance via glUniformMatrix4fv. */
	float	mv[16];		/* view * entity_transform — used for u_modelview
				 * (eye-space fog distance). */
} world_instance_t;

static world_instance_t	world_instances[MAX_WORLD_INSTANCES];
static int		num_world_instances;

typedef struct {
	GLuint		tex;		/* gl_texturenum */
	GLuint		fb_tex;		/* gl_fb_texturenum or gl_null_fb_texture — uhexen2-61bb */
	GLuint		instance;	/* world_instances[] index */
	GLuint		first;		/* surf->vbo_firstindex */
	GLuint		count;		/* surf->vbo_numtris * 3 */
} world_surf_key_t;
static world_surf_key_t	world_surf_keys[MAX_WORLD_SURF_KEYS];
static int		num_world_surf_keys;
static world_surf_key_t	world_surf_keys_fence[MAX_WORLD_SURF_KEYS_FENCE];
static int		num_world_surf_keys_fence;

/* r_speeds counters: surface counts dispatched by the brush-ent passes.
 * Reset in R_DrawEntitiesOnList; written in R_DrawBrushInstanced;
 * read by the format printer. */
int			rprof_brush_inst_opaque;
int			rprof_brush_inst_fence;
/* Track which brush entities still need legacy R_DrawBrushModel for
 * special (sky/turb/underwater-warp) surfaces. */
static qboolean		world_inst_collected[MAX_VISEDICTS];
static qboolean		world_inst_needs_legacy[MAX_VISEDICTS];

extern qboolean		R_CullBox (vec3_t mins, vec3_t maxs);
extern void		R_MarkLights (dlight_t *light, int bit, mnode_t *node);
extern int		c_brush_polys;
extern GLuint		world_vao;
extern GLuint		world_ibo;
extern int		world_num_indices;
extern GLuint		lm_atlas_texture;
extern qboolean		lm_atlas_enabled;
extern int		lightmap_bytes;
#define LM_BLOCK_WIDTH	128
#define LM_BLOCK_HEIGHT	128
extern unsigned char	*lightmaps;
extern int		d_lightstylevalue[256];
extern int		r_framecount;
extern void		R_BuildLightMap_Public (msurface_t *surf, byte *dest, int stride);
extern void		LM_ExpandDirtyRect (int lmnum, int x, int y, int w, int h);
/* The above two have static linkage in gl_rsurf.c; we'll wire them
 * via a small "do lightmap rebuild" helper added there. */
extern void		R_LightmapRebuildIfDirty (msurface_t *s);
extern texture_t	*R_TextureAnimation (entity_t *e, texture_t *base);

static int world_surf_key_cmp (const void *a, const void *b)
{
	const world_surf_key_t *sa = (const world_surf_key_t *)a;
	const world_surf_key_t *sb = (const world_surf_key_t *)b;
	/* Sort by (instance, texture, firstIndex).  Iterating per-instance
	 * lets us upload mvp/modelview uniforms once per entity, then walk
	 * its surfaces grouped by texture.  Critical: brush ents now run
	 * through gl_shader_world (not _inst) so within-shader gl_Position
	 * invariance covers BOTH world surfaces and brush-ent surfaces at
	 * coplanar joins (uhexen2-mf45). */
	if (sa->instance != sb->instance)
		return (sa->instance < sb->instance) ? -1 : 1;
	if (sa->tex != sb->tex)
		return (sa->tex < sb->tex) ? -1 : 1;
	if (sa->first != sb->first)
		return (sa->first < sb->first) ? -1 : 1;
	return 0;
}

extern cvar_t r_brush_inst;

qboolean R_BrushInst_Available (void)
{
	return r_brush_inst.integer != 0 &&
	       gl_shader_world.program != 0 &&
	       world_vao && world_ibo && lm_atlas_enabled && lm_atlas_texture;
}

void R_CollectBrushInstances (void)
{
	int		i, k, s_idx;
	entity_t	*e;
	qmodel_t	*clmodel;
	vec3_t		mins, maxs;
	float		modelorg_local[3];
	qboolean	rotated;

	num_world_instances = 0;
	num_world_surf_keys = 0;
	num_world_surf_keys_fence = 0;
	memset(world_inst_collected, 0, sizeof(world_inst_collected));
	memset(world_inst_needs_legacy, 0, sizeof(world_inst_needs_legacy));

	for (i = 0; i < cl_numvisedicts; i++)
	{
		float fwd[3], rt[3], up[3];
		entity_t *eview;
		qboolean translucent;

		e = cl_visedicts[i];
		if (!e->model || e->model->type != mod_brush)
			continue;

		/* Skip ents the instanced path can't handle: translucent
		 * (drawn via R_DrawTransEntitiesOnList), abslight (uniform
		 * intensity instead of lightmap × tex), and EF_TRANSPARENT
		 * (whole-entity blend).  These leave world_inst_collected[i]
		 * false, and the post-instance walk in R_DrawEntitiesOnList
		 * routes the latter two to the full legacy R_DrawBrushModel
		 * since they're opaque (the trans-edicts pass only sees
		 * actually-translucent ents). */
		translucent = ((e->drawflags & DRF_TRANSLUCENT) ||
			(e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha))) != 0;
		if (translucent)
			continue;
		if ((e->drawflags & MLS_MASKIN) != MLS_NONE)
			continue;	/* MLS_ABSLIGHT/FULLBRIGHT/POWERMODE/TORCH/TOTALDARK
					 * all need the per-surface intensity treatment that
					 * R_RenderBrushPoly provides — fast path would multiply
					 * the lightmap atlas instead.  uhexen2-j7rp. */
		if (e->model->flags & EF_TRANSPARENT)
			continue;

		clmodel = e->model;
		if (e->angles[0] || e->angles[1] || e->angles[2])
		{
			rotated = true;
			for (k = 0; k < 3; k++)
			{
				mins[k] = e->origin[k] - clmodel->radius;
				maxs[k] = e->origin[k] + clmodel->radius;
			}
		}
		else
		{
			rotated = false;
			VectorAdd (e->origin, clmodel->mins, mins);
			VectorAdd (e->origin, clmodel->maxs, maxs);
		}
		if (R_CullBox (mins, maxs))
			continue;

		/* Compute model-space view origin for backface culling */
		VectorSubtract (r_refdef.vieworg, e->origin, modelorg_local);
		if (rotated)
		{
			vec3_t tmp;
			VectorCopy (modelorg_local, tmp);
			AngleVectors (e->angles, fwd, rt, up);
			modelorg_local[0] =  DotProduct (tmp, fwd);
			modelorg_local[1] = -DotProduct (tmp, rt);
			modelorg_local[2] =  DotProduct (tmp, up);
		}

		/* Mark dlights against this submodel's BSP — only when
		 * a dlight is actually live this frame. */
		if (clmodel->firstmodelsurface != 0)
		{
			for (k = 0; k < MAX_DLIGHTS; k++)
			{
				dlight_t *dl = &cl_dlights[k];
				if (dl->die < cl.time || !dl->radius)
					continue;
				if (rotated)
				{
					dlight_t tdl = *dl;
					vec3_t tmp;
					VectorSubtract (dl->origin, e->origin, tmp);
					tdl.origin[0] =  DotProduct (tmp, fwd);
					tdl.origin[1] = -DotProduct (tmp, rt);
					tdl.origin[2] =  DotProduct (tmp, up);
					R_MarkLights (&tdl, 1<<k,
					    clmodel->nodes + clmodel->hulls[0].firstclipnode);
				}
				else
				{
					R_MarkLights (dl, 1<<k,
					    clmodel->nodes + clmodel->hulls[0].firstclipnode);
				}
			}
		}

		/* Compute the world matrix.  R_RotateForEntity does:
		 *   T(origin) * Rz(yaw) * Ry(-pitch) * Rx(-roll)
		 * Bake that into a 3x4 (transposed mat3x4) row-major. */
		if (num_world_instances >= MAX_WORLD_INSTANCES)
			break;
		eview = e;
		{
			float saved_pitch = e->angles[0];
			float saved_roll  = e->angles[2];
			float mv_save[16];
			world_instance_t *winst = &world_instances[num_world_instances];

			GL_GetModelview (mv_save);
			eview->angles[0] = -saved_pitch;
			eview->angles[2] = -saved_roll;
			/* Match the legacy R_DrawBrushModel z-fight workaround:
			 * shift origin by DIST_EPSILON before applying the
			 * entity transform so brush-entity surfaces don't co-
			 * plane with the world.  Restore origin after. */
			if (gl_zfix.integer)
			{
				eview->origin[0] -= DIST_EPSILON;
				eview->origin[1] -= DIST_EPSILON;
				eview->origin[2] -= DIST_EPSILON;
			}
			/* Post-multiply the entity transform onto the current
			 * view matrix so MV becomes view * entity_transform =
			 * entity-modelview, matching what the legacy
			 * R_DrawBrushModel passes as u_modelview. */
			R_RotateForEntity (eview);
			if (gl_zfix.integer)
			{
				eview->origin[0] += DIST_EPSILON;
				eview->origin[1] += DIST_EPSILON;
				eview->origin[2] += DIST_EPSILON;
			}
			eview->angles[0] = saved_pitch;
			eview->angles[2] = saved_roll;

			/* Capture mv = view * entity_transform and
			 * mvp = projection * mv.  Pre-multiplying on the CPU
			 * gives the shader a single mat4×vec4 multiply for
			 * gl_Position — bit-identical to the legacy world
			 * shader, so depth doesn't flicker on moving ents. */
			GL_GetModelview (winst->mv);
			GL_GetMVP       (winst->mvp);
			GL_LoadMatrixf  (mv_save);
		}
		int instance_idx = num_world_instances++;
		world_inst_collected[i] = true;

		/* Walk surfaces: accept opaque + fence, mark remaining
		 * special flags (sky/turb/underwater) so the legacy pass
		 * renders them.  No CPU backface test — for rotating brush
		 * ents (drawbridges, pendulums) the dot-vs-BACKFACE_EPSILON
		 * test flips frame-to-frame on near-tangent surfaces, which
		 * read as texture popping when those surfaces wink in/out
		 * of the MDI dispatch.  Backface triangles waste rasterizer
		 * work but the cost is negligible compared to the popping. */
		{
			msurface_t *psurf = &clmodel->surfaces[clmodel->firstmodelsurface];
			for (s_idx = 0; s_idx < clmodel->nummodelsurfaces; s_idx++, psurf++)
			{
				int spec = psurf->flags &
					(SURF_DRAWSKY | SURF_DRAWTURB |
					 SURF_DRAWFENCE | SURF_UNDERWATER);
				qboolean is_fence;

				if (spec & (SURF_DRAWSKY | SURF_DRAWTURB |
					    SURF_UNDERWATER))
				{
					world_inst_needs_legacy[i] = true;
					continue;
				}
				is_fence = (spec & SURF_DRAWFENCE) != 0;

				if (psurf->vbo_numtris <= 0)
					continue;

				R_LightmapRebuildIfDirty (psurf);

				if (is_fence)
				{
					if (num_world_surf_keys_fence >= MAX_WORLD_SURF_KEYS_FENCE)
						continue;
					{
						texture_t *t = R_TextureAnimation (e, psurf->texinfo->texture);
						world_surf_key_t *wsk = &world_surf_keys_fence[num_world_surf_keys_fence++];
						wsk->tex      = t->gl_texturenum;
						wsk->fb_tex   = t->gl_fb_texturenum ? t->gl_fb_texturenum : gl_null_fb_texture;
						wsk->instance = instance_idx;
						wsk->first    = psurf->vbo_firstindex;
						wsk->count    = psurf->vbo_numtris * 3;
					}
				}
				else
				{
					if (num_world_surf_keys >= MAX_WORLD_SURF_KEYS)
						break;
					{
						texture_t *t = R_TextureAnimation (e, psurf->texinfo->texture);
						world_surf_key_t *wsk = &world_surf_keys[num_world_surf_keys++];
						wsk->tex      = t->gl_texturenum;
						wsk->fb_tex   = t->gl_fb_texturenum ? t->gl_fb_texturenum : gl_null_fb_texture;
						wsk->instance = instance_idx;
						wsk->first    = psurf->vbo_firstindex;
						wsk->count    = psurf->vbo_numtris * 3;
					}
				}
			}
		}
	}

}

/* Walk a sorted-by-(instance, texture, first) key array, uploading the
 * per-instance mvp/mv uniforms once per entity, binding each texture
 * once per (instance, texture) group, and run-coalescing contiguous
 * (firstIndex, count) ranges into single glDrawElements calls.  Used
 * twice — once for the opaque pass, once for the fence pass.  Reuses
 * gl_shader_world (the same shader as world surfaces) so within-shader
 * invariant gl_Position covers both world and brush-ent draws — no
 * cross-shader z-fight, no polygon-offset backstop needed. */
static void R_DispatchBrushInstancedPass (
	const world_surf_key_t *keys, int num_keys, glprogram_t *prog)
{
	int i = 0;
	while (i < num_keys)
	{
		GLuint inst = keys[i].instance;
		if (prog->u_mvp >= 0)
			glUniformMatrix4fv_fp(prog->u_mvp, 1, GL_FALSE,
					      world_instances[inst].mvp);
		if (prog->u_modelview >= 0)
			glUniformMatrix4fv_fp(prog->u_modelview, 1, GL_FALSE,
					      world_instances[inst].mv);
		while (i < num_keys && keys[i].instance == inst)
		{
			GLuint cur_tex = keys[i].tex;
			GLuint cur_fb  = keys[i].fb_tex;
			/* Bind the fullbright mask at TU2 first so the matching
			 * sworld_frag sample picks it up for this (instance, tex)
			 * group.  Leave TU0 sticky for the diffuse bind below.
			 * uhexen2-61bb. */
			glActiveTexture_fp(GL_TEXTURE2);
			glBindTexture_fp(GL_TEXTURE_2D, cur_fb);
			glActiveTexture_fp(GL_TEXTURE0);
			glBindTexture_fp(GL_TEXTURE_2D, cur_tex);
			while (i < num_keys && keys[i].instance == inst &&
			       keys[i].tex == cur_tex)
			{
				GLuint run_first = keys[i].first;
				GLuint run_count = keys[i].count;
				i++;
				while (i < num_keys && keys[i].instance == inst &&
				       keys[i].tex == cur_tex &&
				       keys[i].first == run_first + run_count)
				{
					run_count += keys[i].count;
					i++;
				}
				glDrawElements_fp(GL_TRIANGLES, run_count, GL_UNSIGNED_INT,
				    (void *)((size_t)run_first * sizeof(unsigned int)));
				c_brush_polys++;
			}
		}
	}
}

void R_DrawBrushInstanced (void)
{
#ifndef __EMSCRIPTEN__
	glprogram_t *prog = &gl_shader_world;
	extern float r_fog_density;
	extern float r_fog_color[3];

	if (!prog->program || num_world_instances == 0 ||
	    (num_world_surf_keys == 0 && num_world_surf_keys_fence == 0))
		return;

	/* Sort by (instance, texture, firstIndex) — see world_surf_key_cmp. */
	if (num_world_surf_keys > 0)
		qsort (world_surf_keys, num_world_surf_keys,
		       sizeof(world_surf_keys[0]), world_surf_key_cmp);
	if (num_world_surf_keys_fence > 0)
		qsort (world_surf_keys_fence, num_world_surf_keys_fence,
		       sizeof(world_surf_keys_fence[0]), world_surf_key_cmp);

	rprof_brush_inst_opaque = num_world_surf_keys;
	rprof_brush_inst_fence  = num_world_surf_keys_fence;

	/* Flush lightmap dirty rects produced by R_CollectBrushInstances —
	 * R_LightmapRebuildIfDirty marks them but R_UpdateLightmaps already
	 * ran before the world phase.  Without this second upload, brush
	 * ents sample last frame's atlas. */
	{
		extern void R_UpdateLightmaps (qboolean Translucent);
		R_UpdateLightmaps (false);
	}

	/* Bind world VAO + WORLD shader (not _inst!) + lightmap atlas +
	 * fog uniforms.  Using gl_shader_world here is the entire point
	 * of uhexen2-mf45: same compiled program for world and brush ents
	 * means the GLSL compiler emits identical instructions for both,
	 * and within-shader invariant gl_Position covers coplanar joins
	 * (drawbridge plank vs. ground, portcullis vs. wall pocket). */
	glBindVertexArray_fp(world_vao);
	glUseProgram_fp(prog->program);
	glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
	if (prog->u_fog_density >= 0)
		glUniform1f_fp(prog->u_fog_density, r_fog_density);
	if (prog->u_fog_color >= 0)
		glUniform3f_fp(prog->u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
	glActiveTexture_fp(GL_TEXTURE1);
	glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
	/* Seed TU2 with the null fullbright sentinel.  R_DispatchBrushInstancedPass
	 * rebinds per (instance, texture) group using the per-key fb_tex stored
	 * by R_CollectBrushInstances, so any brush ent that wraps a miptex with
	 * fullbright pixels (e.g. a torch tex on a func_train) gets the same
	 * additive contribution as the matching world surface.  uhexen2-61bb. */
	glActiveTexture_fp(GL_TEXTURE2);
	glBindTexture_fp(GL_TEXTURE_2D, gl_null_fb_texture);
	glActiveTexture_fp(GL_TEXTURE0);
	GL_ImmInvalidateState();

	/* Optional polygon offset — kept as a tunable safety net (default 0).
	 * With the shader unified, no offset should be needed; bumping the
	 * cvar lets us A/B verify if any residual z-fight remains. */
	if (r_brush_inst_offset.value != 0.0f)
	{
		float mag = r_brush_inst_offset.value;
		float fb_offset = gl_clipcontrol_able ? mag : -mag;
		glEnable_fp(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset_fp(fb_offset, fb_offset);
	}

	/* Opaque pass: alpha threshold near zero, no A2C. */
	if (num_world_surf_keys > 0)
	{
		if (prog->u_alpha_threshold >= 0)
			glUniform1f_fp(prog->u_alpha_threshold, 0.01f);
		R_DispatchBrushInstancedPass(world_surf_keys, num_world_surf_keys, prog);
	}

	/* Fence pass: alpha cutout @0.666, optional A2C. */
	if (num_world_surf_keys_fence > 0)
	{
		if (prog->u_alpha_threshold >= 0)
			glUniform1f_fp(prog->u_alpha_threshold, 0.666f);
		if (r_alphatocoverage.integer)
			glEnable_fp(GL_SAMPLE_ALPHA_TO_COVERAGE);
		R_DispatchBrushInstancedPass(world_surf_keys_fence, num_world_surf_keys_fence, prog);
		if (r_alphatocoverage.integer)
			glDisable_fp(GL_SAMPLE_ALPHA_TO_COVERAGE);
	}

	if (r_brush_inst_offset.value != 0.0f)
	{
		glDisable_fp(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset_fp(0.0f, 0.0f);
	}

	glBindVertexArray_fp(0);
	glUseProgram_fp(0);
#endif /* !__EMSCRIPTEN__ */
}

qboolean R_BrushInst_WasCollected (int visedict_idx)
{
	if (visedict_idx < 0 || visedict_idx >= MAX_VISEDICTS)
		return false;
	return world_inst_collected[visedict_idx];
}

qboolean R_BrushInst_NeedsLegacy (int visedict_idx)
{
	if (visedict_idx < 0 || visedict_idx >= MAX_VISEDICTS)
		return false;
	return world_inst_needs_legacy[visedict_idx];
}

static int inst_sort_cmp (const void *a, const void *b)
{
	const inst_sort_t *sa = (const inst_sort_t *)a;
	const inst_sort_t *sb = (const inst_sort_t *)b;
	if (sa->model_key != sb->model_key)
		return (sa->model_key < sb->model_key) ? -1 : 1;
	if (sa->skin_tex != sb->skin_tex)
		return (sa->skin_tex < sb->skin_tex) ? -1 : 1;
	return 0;
}

/*
=============
R_CollectAliasInstance

Compute per-entity data and add to the instance list.
Returns true if the entity was collected, false if it should
be drawn the traditional way (translucent, special, etc.)
=============
*/
static qboolean R_CollectAliasInstance (entity_t *e)
{
	alias_instance_t *inst;
	alias_gpu_mesh_t *gm;
	aliashdr_t	*paliashdr;
	qmodel_t	*clmodel;
	vec3_t		mins, maxs;
	int		mls, lnum, skinnum, anim, frame, prevframe;
	int		pose, prevpose, numposes;
	float		interval, lerpfrac, add, entScale;
	float		xyfact = 1.0, zfact = 1.0;
	/* Switch fall-throughs below leave these uninitialized when
	 * drawflags has unrecognized bits — gcc -Wmaybe-uninitialized
	 * warns under aggressive inlining. */
	float		tscale[3] = {0,0,0}, torigin[3] = {0,0,0};
	float		world[16], saved_mv[16];
	vec3_t		dist;
	byte		cs;

	if (num_alias_instances >= MAX_ALIAS_INSTANCES)
		return false;

	clmodel = e->model;
	if (!clmodel)
		return false;

	/* Skip translucent / special blend entities — drawn separately.
	 * Must match the item_trans check in R_DrawEntitiesOnList. */
	if ((e->drawflags & DRF_TRANSLUCENT) ||
	    (clmodel->flags & (EF_TRANSPARENT | EF_HOLEY | EF_SPECIAL_TRANS)) ||
	    (e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha)))
		return false;

	/* Skip viewmodel — drawn separately with FOV compensation */
	if (e == &cl.viewent)
		return false;

	/* Skip entities with special skin handling (extra textures,
	 * player colormaps) — drawn via R_DrawAliasModel instead. */
	if (e->skinnum >= 100 ||
	    (e->colormap != vid.colormap && !gl_nocolors.integer))
		return false;

	/* Frustum cull */
	VectorAdd(e->origin, clmodel->mins, mins);
	VectorAdd(e->origin, clmodel->maxs, maxs);
	if (R_CullBox(mins, maxs))
		return true;	/* culled, but don't fall back to non-instanced */

	/* Legacy LOD cull (radius * radius * 40000) removed — see the
	 * matching note in the phase-2 alias loop.  Ironwail doesn't
	 * have one and the threshold was too aggressive on dense maps.
	 * uhexen2-l0ac. */

	paliashdr = (aliashdr_t *)Mod_Extradata(clmodel);
	gm = GL_GetAliasGPUMesh(paliashdr);
	if (!gm || !gm->ssbo_pose)
		return false;

	/* --- Lighting (same as R_DrawAliasModel) --- */
	VectorCopy(e->origin, r_entorigin);
	VectorSubtract(r_origin, r_entorigin, modelorg);

	if (r_shadows.integer)
		AliasModelGetLightInfo(e);

	mls = e->drawflags & MLS_MASKIN;
	if ((clmodel->flags & EF_ROTATE) ||
	    (R_GetPimpFlags(e, NULL) & (EF_SPIN | EF_FLOAT)))
	{
		ambientlight = shadelight =
		lightcolor[0] = lightcolor[1] = lightcolor[2] =
			60 + 34 + sin(e->origin[0] + e->origin[1] + (cl.time*3.8)) * 34;
	}
	else if (mls == MLS_ABSLIGHT)
	{
		lightcolor[0] = lightcolor[1] = lightcolor[2] =
		ambientlight = shadelight = e->abslight;
	}
	else if (mls != MLS_NONE)
	{
		lightcolor[0] = lightcolor[1] = lightcolor[2] =
		ambientlight = shadelight = d_lightstylevalue[24+mls]/2;
	}
	else
	{
		if (!r_shadows.integer)
			AliasModelGetLightInfo(e);

		for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
		{
			if (cl_dlights[lnum].die >= cl.time)
			{
				VectorSubtract(e->origin, cl_dlights[lnum].origin, dist);
				add = cl_dlights[lnum].radius - VectorLengthFast(dist);
				if (add > 0)
				{
					if (cl_dlights[lnum].dark)
					{
						ambientlight -= add;
						if (ambientlight < 0) ambientlight = 0;
						lightcolor[0] -= cl_dlights[lnum].color[0] * add;
						if (lightcolor[0] < 0) lightcolor[0] = 0;
						lightcolor[1] -= cl_dlights[lnum].color[1] * add;
						if (lightcolor[1] < 0) lightcolor[1] = 0;
						lightcolor[2] -= cl_dlights[lnum].color[2] * add;
						if (lightcolor[2] < 0) lightcolor[2] = 0;
					}
					else
					{
						ambientlight += add;
						lightcolor[0] += cl_dlights[lnum].color[0] * add;
						lightcolor[1] += cl_dlights[lnum].color[1] * add;
						lightcolor[2] += cl_dlights[lnum].color[2] * add;
					}
				}
			}
		}

		if (gl_overbright_models.integer)
		{
			int i;
			if (ambientlight > 128) ambientlight = 128;
			if (ambientlight + shadelight > 192)
				shadelight = 192 - ambientlight;
			for (i = 0; i < 3; i++)
				if (lightcolor[i] > 192) lightcolor[i] = 192;
		}
	}

	shadelight = shadelight / 200.0;
	VectorScale(lightcolor, 1.0f / 200.0f, lightcolor);

	/* --- Build world matrix with scale/origin baked in --- */
	/* Save current modelview (which is the VIEW matrix), load identity,
	 * apply entity rotation, bake in scale_origin + scale, read result. */
	GL_GetModelview(saved_mv);
	GL_LoadIdentity();
	R_RotateForEntity2(e);

	/* Compute per-entity scale/origin (handles Hexen II entity scaling) */
	if (e->scale != 0 && e->scale != 100)
	{
		entScale = (float)e->scale / 100.0f;
		switch (e->drawflags & SCALE_TYPE_MASKIN)
		{
		case SCALE_TYPE_UNIFORM:
			tscale[0] = paliashdr->scale[0]*entScale;
			tscale[1] = paliashdr->scale[1]*entScale;
			tscale[2] = paliashdr->scale[2]*entScale;
			xyfact = zfact = (entScale-1.0)*127.95;
			break;
		case SCALE_TYPE_XYONLY:
			tscale[0] = paliashdr->scale[0]*entScale;
			tscale[1] = paliashdr->scale[1]*entScale;
			tscale[2] = paliashdr->scale[2];
			xyfact = (entScale-1.0)*127.95;
			zfact = 1.0;
			break;
		case SCALE_TYPE_ZONLY:
			tscale[0] = paliashdr->scale[0];
			tscale[1] = paliashdr->scale[1];
			tscale[2] = paliashdr->scale[2]*entScale;
			xyfact = 1.0;
			zfact = (entScale-1.0)*127.95;
			break;
		}
		switch (e->drawflags & SCALE_ORIGIN_MASKIN)
		{
		case SCALE_ORIGIN_CENTER:
			torigin[0] = paliashdr->scale_origin[0]-paliashdr->scale[0]*xyfact;
			torigin[1] = paliashdr->scale_origin[1]-paliashdr->scale[1]*xyfact;
			torigin[2] = paliashdr->scale_origin[2]-paliashdr->scale[2]*zfact;
			break;
		case SCALE_ORIGIN_BOTTOM:
			torigin[0] = paliashdr->scale_origin[0]-paliashdr->scale[0]*xyfact;
			torigin[1] = paliashdr->scale_origin[1]-paliashdr->scale[1]*xyfact;
			torigin[2] = paliashdr->scale_origin[2];
			break;
		case SCALE_ORIGIN_TOP:
			torigin[0] = paliashdr->scale_origin[0]-paliashdr->scale[0]*xyfact;
			torigin[1] = paliashdr->scale_origin[1]-paliashdr->scale[1]*xyfact;
			torigin[2] = paliashdr->scale_origin[2]-paliashdr->scale[2]*zfact*2.0;
			break;
		}
	}
	else
	{
		VectorCopy(paliashdr->scale, tscale);
		VectorCopy(paliashdr->scale_origin, torigin);
	}

	/* Floating motion */
	if ((clmodel->flags & EF_ROTATE) ||
	    (R_GetPimpFlags(e, NULL) & EF_FLOAT))
		torigin[2] += sin(e->origin[0] + e->origin[1] + (cl.time*3)) * 5.5;

	/* Bake scale_origin and scale into the world matrix:
	 * world = EntityRot * Translate(torigin) * Scale(tscale) */
	GL_Translatef(torigin[0], torigin[1], torigin[2]);
	GL_Scalef(tscale[0], tscale[1], tscale[2]);
	GL_GetModelview(world);

	/* Restore the VIEW matrix */
	GL_LoadMatrixf(saved_mv);

	/* --- Resolve pose and lerp --- */
	frame = e->frame;
	if (frame >= paliashdr->numframes || frame < 0)
		frame = 0;
	pose = paliashdr->frames[frame].firstpose;
	numposes = paliashdr->frames[frame].numposes;
	if (numposes > 1)
	{
		interval = paliashdr->frames[frame].interval;
		pose += (int)(cl.time / interval) % numposes;
	}

	prevframe = e->previouspose;
	lerpfrac = 0.0f;
	if (r_lerpmodels.integer && e->lerptime > 0 && prevframe != frame &&
	    prevframe >= 0 && prevframe < paliashdr->numframes &&
	    !(e->lerpflags & LERP_RESETANIM))
	{
		float elapsed = cl.time - e->lerpstart;
		lerpfrac = elapsed / e->lerptime;
		if (lerpfrac >= 1.0f)
			lerpfrac = 0.0f;
		else
			lerpfrac = 1.0f - lerpfrac;
	}

	if (lerpfrac > 0.0f)
	{
		prevpose = paliashdr->frames[prevframe].firstpose;
		numposes = paliashdr->frames[prevframe].numposes;
		if (numposes > 1)
		{
			interval = paliashdr->frames[prevframe].interval;
			prevpose += (int)(e->lerpstart / interval) % numposes;
		}
	}
	else
	{
		prevpose = pose;
	}

	/* --- Resolve skin texture --- */
	skinnum = e->skinnum;
	if (skinnum >= paliashdr->numskins || skinnum < 0)
		skinnum = 0;
	anim = (int)(cl.time * 10) & 3;

	/* --- Fill instance data (80 bytes) --- */
	inst = &alias_instances[num_alias_instances];
	Mat4_Transpose4x3(world, inst->worldmatrix);

	/* Store light color.  In RGBA lightmap mode, lightcolor already has
	 * full intensity from R_LightPointColor — don't multiply by shadelight
	 * (the CPU path also skips shadelight in RGBA mode). */
	cs = e->colorshade;
	if (gl_lightmap_format == GL_RGBA)
	{
		if (cs)
		{
			inst->light_color[0] = lightcolor[0] * RTint[cs];
			inst->light_color[1] = lightcolor[1] * GTint[cs];
			inst->light_color[2] = lightcolor[2] * BTint[cs];
		}
		else
		{
			VectorCopy(lightcolor, inst->light_color);
		}
	}
	else
	{
		if (cs)
		{
			inst->light_color[0] = lightcolor[0] * RTint[cs] * shadelight;
			inst->light_color[1] = lightcolor[1] * GTint[cs] * shadelight;
			inst->light_color[2] = lightcolor[2] * BTint[cs] * shadelight;
		}
		else
		{
			inst->light_color[0] = lightcolor[0] * shadelight;
			inst->light_color[1] = lightcolor[1] * shadelight;
			inst->light_color[2] = lightcolor[2] * shadelight;
		}
	}

	inst->alpha = 1.0f;
	{
		/* Clamp blend to [0,1] — GLSL mix() extrapolates outside this
		 * range, so any bogus lerpfrac (e.g. e->lerpstart from a stale
		 * time reference after map change / demo seek / save-load) would
		 * fling vertices outward from the entity origin.  Legacy CPU path
		 * has the same risk but is guarded by 'do_lerp = lerpfrac in (0,1)'
		 * inside GL_DrawAliasFrame; the SSBO path needs the clamp here.
		 * uhexen2-iir3. */
		float blend = 1.0f - lerpfrac;
		if (blend < 0.0f) blend = 0.0f;
		else if (blend > 1.0f) blend = 1.0f;
		inst->blend = blend;	/* shader: mix(v1, v0, blend), 1.0 = use v0 */
	}
	inst->pose0 = pose * gm->poseverts;	/* pre-multiply for direct SSBO indexing */
	inst->pose1 = prevpose * gm->poseverts;
	inst->shadedot_row = ((int)(e->angles[1] * (SHADEDOT_QUANT / 360.0))) & (SHADEDOT_QUANT - 1);

	/* Save entity info + resolved skin for shadow/batch passes.
	 * Resolve skin HERE while paliashdr is valid — a second
	 * Mod_Extradata call later might return a different pointer
	 * if cache pressure caused the model to be reloaded. */
	{
		GLuint stex = paliashdr->gl_texturenum[skinnum][anim];
		GLuint ftex = paliashdr->gl_fb_texturenum[skinnum][anim];
		if (!stex) stex = paliashdr->gl_texturenum[skinnum][0];
		if (!ftex) ftex = paliashdr->gl_fb_texturenum[skinnum][0];
		inst_entities[num_alias_instances].skin_tex = stex;
		inst_entities[num_alias_instances].fb_tex = ftex;
	}
	inst_entities[num_alias_instances].ent = e;
	inst_entities[num_alias_instances].hdr = paliashdr;
	inst_entities[num_alias_instances].pose = pose;

	c_alias_polys += paliashdr->numtris;
	num_alias_instances++;
	return true;
}

/*
=============
R_BuildAliasBatches

Sort collected instances by (model, skin) and build batch list.
=============
*/
static inst_sort_t inst_sort_keys[MAX_ALIAS_INSTANCES];

static void R_CollectAndBatchAliasInstances (void)
{
	int	i, j;
	entity_t *e;

	num_alias_instances = 0;
	num_alias_batches = 0;
	num_inst_entities = 0;
	memset(inst_collected, 0, sizeof(inst_collected));

	if (!r_drawentities.integer)
		return;
	if (!gl_shader_alias_inst.program)
		return;

	/* Collect all eligible opaque alias entities */
	for (i = 0; i < cl_numvisedicts; i++)
	{
		e = cl_visedicts[i];
		if (!e->model || e->model->type != mod_alias)
			continue;

		/* chase-cam pitch adj. by FrikaC */
		if (e == &cl_entities[cl.viewentity])
			e->angles[0] *= 0.3;

		int old_count = num_alias_instances;
		if (R_CollectAliasInstance(e))
		{
			/* Record sort key only if instance was actually collected */
			if (num_alias_instances > old_count)
			{
				int idx = num_alias_instances - 1;
				inst_collected[i] = true;
				/* Use skin/hdr already resolved in R_CollectAliasInstance
				 * — avoids a second Mod_Extradata that could return a
				 * different pointer after cache eviction. */
				inst_sort_keys[idx].instance_idx = idx;
				inst_sort_keys[idx].model_key = (size_t)inst_entities[idx].hdr;
				inst_sort_keys[idx].skin_tex = inst_entities[idx].skin_tex;
				inst_sort_keys[idx].fb_tex = inst_entities[idx].fb_tex;
			}
		}
	}

	if (num_alias_instances == 0)
		return;

	/* Save entity count for shadow pass (before sort reorders instances) */
	num_inst_entities = num_alias_instances;

	/* Sort by (model, skin) for batching */
	qsort(inst_sort_keys, num_alias_instances, sizeof(inst_sort_t), inst_sort_cmp);

	/* Reorder instance data to match sort order */
	{
		static alias_instance_t sorted[MAX_ALIAS_INSTANCES];
		for (i = 0; i < num_alias_instances; i++)
			sorted[i] = alias_instances[inst_sort_keys[i].instance_idx];
		memcpy(alias_instances, sorted, num_alias_instances * sizeof(alias_instance_t));
	}

	/* Build batch list */
	{
		alias_batch_t *batch = &alias_batches[0];
		aliashdr_t *hdr = (aliashdr_t *)(size_t)inst_sort_keys[0].model_key;

		batch->hdr = hdr;
		batch->skin_tex = inst_sort_keys[0].skin_tex;
		batch->fb_tex = inst_sort_keys[0].fb_tex;
		batch->first = 0;
		batch->count = 1;
		num_alias_batches = 1;

		for (i = 1; i < num_alias_instances; i++)
		{
			if (inst_sort_keys[i].model_key == inst_sort_keys[i-1].model_key &&
			    inst_sort_keys[i].skin_tex == inst_sort_keys[i-1].skin_tex)
			{
				batch->count++;
			}
			else
			{
				if (num_alias_batches >= MAX_ALIAS_BATCHES)
					break;
				batch = &alias_batches[num_alias_batches++];
				batch->hdr = (aliashdr_t *)(size_t)inst_sort_keys[i].model_key;
				batch->skin_tex = inst_sort_keys[i].skin_tex;
				batch->fb_tex = inst_sort_keys[i].fb_tex;
				batch->first = i;
				batch->count = 1;
			}
		}
	}
}

/*
=============
R_DrawAliasInstanced

Draw all collected alias instances in batches.
=============
*/
static void R_DrawAliasInstanced (void)
{
	int	b, i;
	gl_alias_inst_prog_t *prog = &gl_shader_alias_inst;
	extern float r_fog_density;
	extern float r_fog_color[3];
	GLuint		inst_buf;
	GLintptr	inst_ofs;
	size_t		inst_bytes;

	if (num_alias_instances == 0 || !prog->program)
		return;

	/* View-projection: written into a uniform mat4 (uhexen2-8pc2).
	 * The matrix stack currently has just VIEW, so GetMVP = Proj * View. */
	GL_GetMVP(alias_inst_view_proj);

	/* Stream the instance array into this frame's host ring slot.
	 * Returns (buf, offset) suitable for GL_BindBufferRange at binding 0.
	 * uhexen2-8pc2: replaces glBufferSubData on a long-lived SSBO. */
	inst_bytes = (size_t) num_alias_instances * sizeof(alias_instance_t);
	GL_Upload(GL_SHADER_STORAGE_BUFFER, alias_instances, inst_bytes,
		  &inst_buf, &inst_ofs);
	GL_BindBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
			   inst_buf, inst_ofs, inst_bytes);

	/* Activate instanced shader */
	glUseProgram_fp(prog->program);

	/* Set uniforms (view-proj, fog, alpha threshold, eye position) */
	if (prog->u_viewproj >= 0)
		glUniformMatrix4fv_fp(prog->u_viewproj, 1, GL_FALSE,
				      alias_inst_view_proj);
	/* Use r_fog_density (pre-scaled by Fog_SetupFrame) not raw Fog_GetDensity() */
	if (prog->u_fog_density >= 0)
		glUniform1f_fp(prog->u_fog_density, r_fog_density);
	if (prog->u_eyepos >= 0)
		glUniform3f_fp(prog->u_eyepos, r_origin[0], r_origin[1], r_origin[2]);
	if (prog->u_fog_color >= 0)
		glUniform3f_fp(prog->u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
	/* R_CollectAliasInstance filters out EF_HOLEY/EF_TRANSPARENT/translucent
	 * entities, so every batch here is opaque. Use the inert 0.01 threshold
	 * (matches Ironwail's ALPHATEST=0 shader variant for non-holey alias) —
	 * a global 0.666 would discard skin pixels whose alpha dips below 2/3 due
	 * to bilinear filtering of palette index 255 or PNG alpha edges, even on
	 * models that were never tagged as cutouts. uhexen2-6eab. */
	if (prog->u_alpha_threshold >= 0)
		glUniform1f_fp(prog->u_alpha_threshold, 0.01f);

	/* Bind shadedots SSBO at binding 2 (matches non-instanced GPU alias path) */
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 2, prog->ubo_shadedots);

	GL_SetAlphaThreshold(0.01f);

	/* Draw each batch */
	for (b = 0; b < num_alias_batches; b++)
	{
		alias_batch_t	*batch = &alias_batches[b];
		alias_gpu_mesh_t *gm = GL_GetAliasGPUMesh(batch->hdr);

		if (!gm || !gm->valid || !gm->ssbo_pose)
			continue;

		/* Bind skin texture to unit 0 */
		glActiveTexture_fp(GL_TEXTURE0);
		GL_Bind(batch->skin_tex);

		/* Bind pose SSBO at binding 1 */
		glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 1, gm->ssbo_pose);

		/* Set base instance offset for this batch */
		if (prog->u_inst_base >= 0)
			glUniform1i_fp(prog->u_inst_base, batch->first);

		/* Use model's VAO (texcoords + IBO already set up) */
		glBindVertexArray_fp(gm->vao);

		/* Draw! */
		glDrawElementsInstanced_fp(GL_TRIANGLES, gm->num_indices,
					   GL_UNSIGNED_SHORT, NULL, batch->count);
	}

	/* Restore state */
	glBindVertexArray_fp(0);
	glUseProgram_fp(0);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 0, 0);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 1, 0);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 2, 0);
	GL_SetAlphaThreshold(0.01f);

	/* Shadow pass — drawn individually per entity */
	if (r_shadows.integer)
	{
		for (i = 0; i < num_inst_entities; i++)
		{
			entity_t *se = inst_entities[i].ent;
			aliashdr_t *shdr = inst_entities[i].hdr;
			int spose = inst_entities[i].pose;

			GL_PushMatrix();
			R_RotateForEntity2(se);
			glEnable_fp(GL_BLEND);
			glDepthMask_fp(0);
			GL_DrawAliasShadow(se, shdr, spose);
			glDepthMask_fp(1);
			glDisable_fp(GL_BLEND);
			GL_PopMatrix();
		}
	}

	/* Fullbright pass — re-draw instances with fullbright textures,
	 * additive blending, uniform brightness */
	if (gl_fullbrights.integer && !r_fullbright.integer)
	{
		qboolean has_fb = false;
		for (b = 0; b < num_alias_batches && !has_fb; b++)
			if (alias_batches[b].fb_tex)
				has_fb = true;

		if (has_fb)
		{
			/* Override light_color to uniform brightness for fullbright */
			for (i = 0; i < num_alias_instances; i++)
			{
				alias_instances[i].light_color[0] = 1.0f;
				alias_instances[i].light_color[1] = 1.0f;
				alias_instances[i].light_color[2] = 1.0f;
			}

			/* Re-stream the modified instance data into a fresh
			 * region of the ring; old region is still bound on
			 * the main-pass draw commands which haven't executed
			 * yet — independent allocations keep them safe.
			 * uhexen2-8pc2. */
			{
				size_t fb_bytes = (size_t) num_alias_instances
					* sizeof(alias_instance_t);
				GLuint   fb_buf;
				GLintptr fb_ofs;
				GL_Upload(GL_SHADER_STORAGE_BUFFER,
					  alias_instances, fb_bytes,
					  &fb_buf, &fb_ofs);
				GL_BindBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
						   fb_buf, fb_ofs, fb_bytes);
			}

			glUseProgram_fp(prog->program);
			if (prog->u_viewproj >= 0)
				glUniformMatrix4fv_fp(prog->u_viewproj, 1, GL_FALSE,
						      alias_inst_view_proj);
			if (prog->u_fog_density >= 0)
				glUniform1f_fp(prog->u_fog_density, Fog_GetDensity());
			if (prog->u_fog_color >= 0)
			{
				const float *fc = Fog_GetColor();
				glUniform3f_fp(prog->u_fog_color, fc[0], fc[1], fc[2]);
			}
			if (prog->u_alpha_threshold >= 0)
				glUniform1f_fp(prog->u_alpha_threshold, 0.01f);

			glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 2, prog->ubo_shadedots);
			glEnable_fp(GL_BLEND);
			glBlendFunc_fp(GL_ONE, GL_ONE);	/* additive */
			glDepthMask_fp(0);
			/* Polygon-offset backstop for the gl_Position non-invariance
			 * z-fight. See R_DrawAliasModel fullbright pass. uhexen2-iir3. */
			{
				float fb_offset = gl_clipcontrol_able ? 1.0f : -1.0f;
				glEnable_fp(GL_POLYGON_OFFSET_FILL);
				glPolygonOffset_fp(fb_offset, fb_offset);
			}
			GL_SetAlphaThreshold(0.01f);

			for (b = 0; b < num_alias_batches; b++)
			{
				alias_batch_t *batch = &alias_batches[b];
				alias_gpu_mesh_t *gm;

				if (!batch->fb_tex)
					continue;

				gm = GL_GetAliasGPUMesh(batch->hdr);
				if (!gm || !gm->valid || !gm->ssbo_pose)
					continue;

				glActiveTexture_fp(GL_TEXTURE0);
				GL_Bind(batch->fb_tex);
				glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 1, gm->ssbo_pose);
				if (prog->u_inst_base >= 0)
					glUniform1i_fp(prog->u_inst_base, batch->first);
				glBindVertexArray_fp(gm->vao);

				glDrawElementsInstanced_fp(GL_TRIANGLES, gm->num_indices,
							   GL_UNSIGNED_SHORT, NULL, batch->count);
			}

			glDisable_fp(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset_fp(0.0f, 0.0f);
			glDepthMask_fp(1);
			glBlendFunc_fp(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDisable_fp(GL_BLEND);

			glBindVertexArray_fp(0);
			glUseProgram_fp(0);
			glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 0, 0);
			glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 1, 0);
			glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 2, 0);
		}
	}
}

/* Sub-timers for R_DrawEntitiesOnList -- declared here so they are
 * visible before the function body; the rprof block below holds the
 * rest of the profiling variables. */
static double	rprof_cpu_ents_collect;
static double	rprof_cpu_ents_inst;
static double	rprof_cpu_ents_loop;
static int	rprof_ents_n_alias_loop;
static int	rprof_ents_n_brush_loop;
/* Finer-grained breakdowns of `loop` so we can tell whether the
 * cost is the legacy alias path, the brush-special legacy path, or
 * loop overhead.  bL counts entities that actually hit
 * R_DrawBrushModelSpecialOnly (subset of b which counts every visible
 * non-translucent brush ent).  inst_fence/inst_opaque (declared
 * earlier near the brush instancer) count surfaces dispatched via
 * the instanced MDI passes. */
static double	rprof_cpu_alias_legacy;
static double	rprof_cpu_brush_legacy;
static int	rprof_ents_n_brush_legacy;
/* Phase-level breakdown of the entity loop.  bC = brush-instance
 * collect, bD = brush-instance dispatch, p1 = post-instance fall-
 * through walk, p2 = alias-fallback + sprite scan. */
static double	rprof_cpu_brush_collect;
static double	rprof_cpu_brush_dispatch;
static double	rprof_cpu_phase1_loop;
static double	rprof_cpu_phase2_loop;

/*
=============
R_DrawEntitiesOnList
=============
*/
static void R_DrawEntitiesOnList (void)
{
	int			i;
	qboolean	item_trans = false;
	qboolean	use_instancing;
	mleaf_t		*pLeaf;
	entity_t	*e;

	cl_numtransvisedicts = 0;
	cl_numtranswateredicts = 0;

	if (r_speeds.integer >= 2)
	{
		rprof_cpu_ents_collect = 0;
		rprof_cpu_ents_inst = 0;
		rprof_cpu_ents_loop = 0;
		rprof_ents_n_alias_loop = 0;
		rprof_ents_n_brush_loop = 0;
		rprof_cpu_alias_legacy = 0;
		rprof_cpu_brush_legacy = 0;
		rprof_ents_n_brush_legacy = 0;
		rprof_brush_inst_opaque = 0;
		rprof_brush_inst_fence = 0;
		rprof_cpu_brush_collect = 0;
		rprof_cpu_brush_dispatch = 0;
		rprof_cpu_phase1_loop = 0;
		rprof_cpu_phase2_loop = 0;
	}

	if (!r_drawentities.integer)
		return;

	/* Instanced alias rendering: collect and batch-draw opaque alias
	 * models via SSBO before the per-entity loop. */
	use_instancing = (r_alias_gpu.integer >= 1 && gl_shader_alias_inst.program);
	if (use_instancing)
	{
		double _t0c = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
		R_CollectAndBatchAliasInstances();
		if (r_speeds.integer >= 2)
			rprof_cpu_ents_collect = Sys_DoubleTime() - _t0c;

		double _t0d = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
		R_DrawAliasInstanced();
		if (r_speeds.integer >= 2)
			rprof_cpu_ents_inst = Sys_DoubleTime() - _t0d;
	}

	double _t0loop = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;

	/* Two-phase split: brush models first, then alias-fallback +
	 * sprite handling.  This keeps the world-VBO/shader/atlas state
	 * stable across the entire brush phase so R_DrawBrushModel can
	 * skip per-entity rebinds altogether.  The chase-cam pitch
	 * adjustment for cl.viewentity fires exactly once per entity
	 * since each entity belongs to one phase only. */

	/* Phase 1: brush models.  Use the unified-shader brush-ent path
	 * (R_CollectBrushInstances + R_DrawBrushInstanced through
	 * gl_shader_world) when r_brush_inst is set; fall back to the
	 * per-entity legacy R_DrawBrushModel walk otherwise. */
	{
		extern qboolean R_BrushInst_Available(void);
		extern void	R_CollectBrushInstances(void);
		extern void	R_DrawBrushInstanced(void);
		extern qboolean	R_BrushInst_NeedsLegacy(int);
		extern qboolean	R_BrushInst_WasCollected(int);
		extern void	R_DrawBrushModelSpecialOnly(entity_t *e);
		extern void	R_BeginBrushBatch(void);
		extern void	R_EndBrushBatch(void);

		/* Apply chase-cam pitch adjustment for cl.viewentity if it's
		 * a brush model (rare — viewentity is normally alias). */
		if (cl_entities[cl.viewentity].model &&
		    cl_entities[cl.viewentity].model->type == mod_brush)
			cl_entities[cl.viewentity].angles[0] *= 0.3;

		if (R_BrushInst_Available())
		{
			double _t0bc = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
			/* Single instanced dispatch covers every opaque
			 * brush entity's regular surfaces. */
			R_CollectBrushInstances();
			if (r_speeds.integer >= 2)
				rprof_cpu_brush_collect = Sys_DoubleTime() - _t0bc;

			double _t0bd = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
			R_DrawBrushInstanced();
			if (r_speeds.integer >= 2)
				rprof_cpu_brush_dispatch = Sys_DoubleTime() - _t0bd;

			double _t0p1 = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
			/* Legacy fall-through for special surfaces (sky /
			 * turb / fence / underwater) on collected entities,
			 * plus translucent deferral for non-collected. */
			for (i = 0; i < cl_numvisedicts; i++)
			{
				e = cl_visedicts[i];
				if (!e->model || e->model->type != mod_brush)
					continue;

				item_trans = ((e->drawflags & DRF_TRANSLUCENT) ||
					(e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha))) != 0;
				if (item_trans)
				{
					pLeaf = Mod_PointInLeaf (e->origin, cl.worldmodel);
					if (pLeaf->contents != CONTENTS_WATER)
						cl_transvisedicts[cl_numtransvisedicts++].ent = e;
					else
						cl_transwateredicts[cl_numtranswateredicts++].ent = e;
					continue;
				}
				if (r_speeds.integer >= 2) rprof_ents_n_brush_loop++;
				if (!R_BrushInst_WasCollected(i))
				{
					/* Ent skipped by collector (MLS_ABSLIGHT,
					 * EF_TRANSPARENT, or frustum-culled).
					 * Render with the full legacy path since
					 * the instanced pass drew nothing for it.
					 * R_DrawBrushModel re-runs the cull cheaply
					 * for the rejected case. */
					double _t0bl = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
					R_DrawBrushModel (e, false);
					if (r_speeds.integer >= 2)
					{
						rprof_cpu_brush_legacy += Sys_DoubleTime() - _t0bl;
						rprof_ents_n_brush_legacy++;
					}
				}
				else if (R_BrushInst_NeedsLegacy(i))
				{
					double _t0bl = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
					R_DrawBrushModelSpecialOnly(e);
					if (r_speeds.integer >= 2)
					{
						rprof_cpu_brush_legacy += Sys_DoubleTime() - _t0bl;
						rprof_ents_n_brush_legacy++;
					}
				}
			}
			if (r_speeds.integer >= 2)
				rprof_cpu_phase1_loop = Sys_DoubleTime() - _t0p1;
		}
		else
		{
			/* Fallback: legacy per-entity path inside a brush
			 * batch session for shared GL state.  Reuses p1 timer
			 * so r_speeds shows the legacy walk's cost when
			 * r_brush_inst=0. */
			double _t0leg = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
			R_BeginBrushBatch();
			for (i = 0; i < cl_numvisedicts; i++)
			{
				e = cl_visedicts[i];
				if (!e->model || e->model->type != mod_brush)
					continue;
				item_trans = ((e->drawflags & DRF_TRANSLUCENT) ||
					(e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha))) != 0;
				if (!item_trans)
				{
					if (r_speeds.integer >= 2) rprof_ents_n_brush_loop++;
					R_DrawBrushModel (e, false);
				}
				else
				{
					pLeaf = Mod_PointInLeaf (e->origin, cl.worldmodel);
					if (pLeaf->contents != CONTENTS_WATER)
						cl_transvisedicts[cl_numtransvisedicts++].ent = e;
					else
						cl_transwateredicts[cl_numtranswateredicts++].ent = e;
				}
			}
			R_EndBrushBatch();
			if (r_speeds.integer >= 2)
				rprof_cpu_phase1_loop = Sys_DoubleTime() - _t0leg;
		}
	}

	double _t0p2 = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
	/* Phase 2: alias-fallback + sprite (latter deferred to trans pass) */
	for (i = 0; i < cl_numvisedicts; i++)
	{
		e = cl_visedicts[i];
		if (!e->model || e->model->type == mod_brush)
			continue;
		if (e == &cl_entities[cl.viewentity])
			e->angles[0] *= 0.3;	// chase-cam pitch adj. by FrikaC

		switch (e->model->type)
		{
		case mod_alias:
		{
			/* Optional hard entity draw distance (r_entdist).
			 *
			 * The legacy "< 4 px screen size" LOD cull
			 * (radius * radius * 40000) was removed — Ironwail
			 * doesn't have one, and the threshold was too
			 * aggressive for small-radius models (popped in/out as
			 * the player moved on dense maps).  uhexen2-l0ac. */
			if (r_entdist.value > 0)
			{
				float dx = e->origin[0] - r_origin[0];
				float dy = e->origin[1] - r_origin[1];
				float dz = e->origin[2] - r_origin[2];
				float dist_sq = dx*dx + dy*dy + dz*dz;
				if (dist_sq > r_entdist.value * r_entdist.value &&
				    e != &cl_entities[cl.viewentity])
					break;
			}

			item_trans = ((e->drawflags & DRF_TRANSLUCENT) ||
					(e->model->flags & (EF_TRANSPARENT|EF_HOLEY|EF_SPECIAL_TRANS)) ||
					(e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha))) != 0;
			if (!item_trans)
			{
				if (!use_instancing || !inst_collected[i])
				{
					double _t0al = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
					R_DrawAliasModel (e);
					if (r_speeds.integer >= 2)
					{
						rprof_cpu_alias_legacy += Sys_DoubleTime() - _t0al;
						rprof_ents_n_alias_loop++;
					}
				}
			}
			break;
		}

		case mod_sprite:
			item_trans = true;
			break;

		default:
			item_trans = false;
			break;
		}

		if (item_trans)
		{
			pLeaf = Mod_PointInLeaf (e->origin, cl.worldmodel);
			if (pLeaf->contents != CONTENTS_WATER)
				cl_transvisedicts[cl_numtransvisedicts++].ent = e;
			else
				cl_transwateredicts[cl_numtranswateredicts++].ent = e;
		}
	}

	if (r_speeds.integer >= 2)
	{
		rprof_cpu_phase2_loop = Sys_DoubleTime() - _t0p2;
		rprof_cpu_ents_loop = Sys_DoubleTime() - _t0loop;
	}

	/* Clean up GPU alias state left bound across entities */
	glBindVertexArray_fp(0);
	glUseProgram_fp(0);
}


/*
================
R_DrawTransEntitiesOnList
Implemented by: jack
================
*/

/*
 * R_AlphaSortRadix
 *
 * LSD radix sort over the 4 bytes of the IEEE 754 bit pattern of `len`.
 * `len` is a squared distance (always >= 0), so the unsigned bit pattern
 * compares in the same order as the float value.  Inverting the bits
 * makes the natural ascending sort yield descending output (back-to-
 * front) without an extra reverse pass.  Stable, O(n), matches Ironwail.
 */
static void R_AlphaSortRadix (sortedent_t *ents, int n)
{
	static sortedent_t	tmp[MAX_VISEDICTS];
	uint32_t		hist[4][256];
	int			i, b;
	sortedent_t		*src, *dst, *swap;

	if (n < 2)
		return;

	memset (hist, 0, sizeof (hist));
	for (i = 0; i < n; i++)
	{
		uint32_t bits;
		memcpy (&bits, &ents[i].len, 4);
		bits = ~bits;
		hist[0][(bits      ) & 0xFF]++;
		hist[1][(bits >>  8) & 0xFF]++;
		hist[2][(bits >> 16) & 0xFF]++;
		hist[3][(bits >> 24) & 0xFF]++;
	}

	for (b = 0; b < 4; b++)
	{
		uint32_t s = 0;
		for (i = 0; i < 256; i++)
		{
			uint32_t c = hist[b][i];
			hist[b][i] = s;
			s += c;
		}
	}

	src = ents;
	dst = tmp;
	for (b = 0; b < 4; b++)
	{
		for (i = 0; i < n; i++)
		{
			uint32_t bits;
			memcpy (&bits, &src[i].len, 4);
			bits = ~bits;
			dst[hist[b][(bits >> (b * 8)) & 0xFF]++] = src[i];
		}
		swap = src; src = dst; dst = swap;
	}
	/* 4 swaps -> src is back at `ents`, which holds the final result. */
}

static void R_DrawTransEntitiesOnList (qboolean inwater)
{
	int		i;
	int		numents;
	sortedent_t	*theents;
	entity_t	*e;
	int	depthMaskWrite = 0;
	vec3_t	result;

	theents = (inwater) ? cl_transwateredicts : cl_transvisedicts;
	numents = (inwater) ? cl_numtranswateredicts : cl_numtransvisedicts;

	for (i = 0; i < numents; i++)
	{
		VectorSubtract(theents[i].ent->origin, r_origin, result);
	//	theents[i].len = VectorLength(result);
		theents[i].len = (result[0] * result[0]) + (result[1] * result[1]) + (result[2] * result[2]);
	}

	if (r_alphasort.integer)
		R_AlphaSortRadix (theents, numents);

	glDepthMask_fp(0);
	for (i = 0; i < numents; i++)
	{
		e = theents[i].ent;

		if (!e->model)
			continue;

		switch (e->model->type)
		{
		case mod_alias:
			if (!depthMaskWrite)
			{
				depthMaskWrite = 1;
				glDepthMask_fp(1);
			}
			R_DrawAliasModel (e);
			break;
		case mod_brush:
			if (!depthMaskWrite)
			{
				depthMaskWrite = 1;
				glDepthMask_fp(1);
			}
			R_DrawBrushModel (e, true);
			break;
		case mod_sprite:
			if (depthMaskWrite)
			{
				depthMaskWrite = 0;
				glDepthMask_fp(0);
			}
			R_DrawSpriteModel (e);
			break;
		}
	}

	if (!depthMaskWrite)
		glDepthMask_fp(1);

	/* Clean up GPU alias state left bound across entities */
	glBindVertexArray_fp(0);
	glUseProgram_fp(0);
}

//=============================================================================


// Glow styles. These rely on unchanged game code!
#define	TORCH_STYLE	1	/* Flicker	*/
#define	MISSILE_STYLE	6	/* Flicker	*/
#define	PULSE_STYLE	11	/* Slow pulse	*/

// Per-entity PimpModel overrides, keyed by entity number
static pimp_override_t	pimp_overrides[MAX_EDICTS];

void R_ClearPimpOverrides (void)
{
	memset(pimp_overrides, 0, sizeof(pimp_overrides));
}

pimp_override_t *R_GetPimpOverride (int entnum)
{
	if (entnum < 0 || entnum >= MAX_EDICTS)
		return NULL;
	return &pimp_overrides[entnum];
}

// Returns model flags for an entity, with per-entity trail overrides
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

// Returns combined ex_flags for an entity (pimp override | model defaults)
// and sets *gsettings_out to the active glow settings
static float null_glow_settings[GLOW_SETTINGS_COUNT];
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

static void R_DrawGlow (entity_t *e)
{
	qmodel_t	*clmodel;

	clmodel = e->model;

	float *gsettings;
	int glow_flags = R_GetPimpFlags(e, &gsettings);

	// If the orb is firing because of an engine-set XF_*GLOW flag on the
	// model and the per-entity override didn't explicitly request the orb
	// (no EF_GLOW in the override's own ex_flags), use the model's
	// canonical glow color rather than the override's. The override's
	// color was set by the QC for a different effect (e.g. EF_ILLUMINATE
	// dlight color on demo1's misc_modelpimp artifacts) and should not
	// recolor an engine-known orb (red mana, orange torch, etc.).
	if (clmodel)
	{
		int entnum = (int)(e - cl_entities);
		int pimp_ex = (entnum >= 0 && entnum < MAX_EDICTS &&
		               R_GetPimpOverride(entnum)->active)
		              ? R_GetPimpOverride(entnum)->ex_flags : 0;
		if (!(pimp_ex & EF_GLOW) &&
		    (clmodel->ex_flags & (XF_TORCH_GLOW | XF_GLOW | XF_MISSILE_GLOW)))
		{
			gsettings = clmodel->glow_settings;
		}
	}

	// Torches & Flames
	if ((gl_glows.integer && (glow_flags & XF_TORCH_GLOW)) ||
	    (gl_missile_glows.integer && (glow_flags & XF_MISSILE_GLOW)) ||
	    (gl_other_glows.integer && (glow_flags & (XF_GLOW | EF_GLOW))) )
	{
		// NOTE: It would be better if we batched these up.
		//	 All those state changes are not nice. KH
		vec3_t	lightorigin;		// Origin of torch.
		vec3_t	glow_vect;		// Vector to torch.
		float	radius;			// Radius of torch flare.
		float	distance;		// Vector distance to torch.
		float	intensity;		// Intensity of torch flare.
		int	i, j;
		vec3_t	vp2;

		// NOTE: I don't think this is centered on the model.
		VectorCopy(e->origin, lightorigin);

		radius = 20.0f;

		// for mana, make it bit bigger
		if ( !q_strncasecmp(clmodel->name, "models/i_btmana", 15))
			radius += 5.0f;

		// Use custom radius if set
		if (gsettings[ORB_RADIUS] > 1)
			radius = gsettings[ORB_RADIUS];

		VectorSubtract(lightorigin, r_origin, vp2);

		// See if view is outside the light.
		distance = VectorLengthFast(vp2);

		if (distance > radius)
		{
			VectorNormalizeFast(vp2);
			GL_PushMatrix();

			// Translate the glow to coincide with the flame. KH
			if (glow_flags & XF_TORCH_GLOW)
			{
				if (glow_flags & XF_TORCH_GLOW_EGYPT)	// egypt torch fix
					GL_Translatef (cos(e->angles[1]/180*M_PI)*8.0f + gsettings[ORB_OFFSET_X], sin(e->angles[1]/180*M_PI)*8.0f + gsettings[ORB_OFFSET_Y], 16.0f + gsettings[ORB_OFFSET_Z]);
				else	GL_Translatef (gsettings[ORB_OFFSET_X], gsettings[ORB_OFFSET_Y], 8.0f + gsettings[ORB_OFFSET_Z]);
			}

			// 'floating' movement
			if (clmodel->flags & EF_ROTATE || glow_flags & EF_FLOAT)
				GL_Translatef (gsettings[ORB_OFFSET_X], gsettings[ORB_OFFSET_Y], sin(e->origin[0] + e->origin[1] + (cl.time*3))*5.5 + gsettings[ORB_OFFSET_Z]);

			// Diminish torch flare inversely with distance.
			intensity = (1024.0f - distance) / 1024.0f;

			// Invert (fades as you approach).
			intensity = (1.0f - intensity);

			// Clamp, but don't let the flare disappear.
			if (intensity > 1.0f)
				intensity = 1.0f;
			else if (intensity < 0.0f)
				intensity = 0.0f;

			// Now modulate with flicker using LIGHT_STYLE from glow_settings.
			// Match Shanjaq: only gate on cl_lightstyle[].length (style 0 plays
			// its ramp normally if present).
			j = 0;	// avoid compiler warning
			{
				int style = (int)gsettings[LIGHT_STYLE];
				if (style < 0 || style >= MAX_LIGHTSTYLES)
					style = 0;
				i = (int)(cl.time*10);
				if (!cl_lightstyle[style].length) {
					j = 256;
				}
				else
				{
					j = i % cl_lightstyle[style].length;
					j = cl_lightstyle[style].map[j] - 'a';
					j = j * 22;
				}
			}

			intensity *= ((float)j / 255.0f);

			// Scale non-torch glows by gl_glow_intensity
			if (!(glow_flags & XF_TORCH_GLOW) && gl_glow_intensity.value < 1.0f)
				intensity *= gl_glow_intensity.value;

			// Attenuate glow by fog so it doesn't shine through.
			// EXP2 form (matches the shader path in gl_shader.c).
			{
				float fogdensity = Fog_GetDensity() / 64.0f;
				if (fogdensity > 0)
				{
					float fogfac = fogdensity * distance;
					float fogfactor = exp(-fogfac * fogfac);
					if (fogfactor < 0) fogfactor = 0;
					if (fogfactor > 1) fogfactor = 1;
					intensity *= fogfactor;
				}
			}

			GL_ImmBegin ();

			GL_ImmColor4f (gsettings[COLOR_R]*intensity,
					gsettings[COLOR_G]*intensity,
					gsettings[COLOR_B]*intensity,
					gsettings[COLOR_A]);

			for (i = 0; i < 3; i++)
				glow_vect[i] = lightorigin[i] - vp2[i]*radius;

			GL_ImmVertex3f (glow_vect[0], glow_vect[1], glow_vect[2]);

			GL_ImmColor4f (0.0f, 0.0f, 0.0f, 1.0f);

			for (i = 16; i >= 0; i--)
			{
				float a = i / 16.0f * M_PI * 2;

				for (j = 0; j < 3; j++)
					glow_vect[j] = lightorigin[j] + vright[j]*cos(a)*radius + vup[j]*sin(a)*radius;

				GL_ImmVertex3f (glow_vect[0], glow_vect[1], glow_vect[2]);
			}

			GL_ImmEnd (GL_TRIANGLE_FAN, &gl_shader_flat);
			// Restore previous matrix
			GL_PopMatrix();
		}
	}
}

static void R_DrawAllGlows (void)
{
	int		i;
	entity_t	*e;

	if (!gl_glows.integer && !gl_missile_glows.integer && !gl_other_glows.integer)
		return;

	if (!r_drawentities.integer)
		return;

	glDepthMask_fp (0);
	glEnable_fp (GL_BLEND);
	glBlendFunc_fp (GL_ONE, GL_ONE);

	for (i = 0; i < cl_numvisedicts; i++)
	{
		e = cl_visedicts[i];

		if (e->model && e->model->type == mod_alias)
			R_DrawGlow (e);
	}

	glDisable_fp (GL_BLEND);
	glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask_fp (1);
}

//=============================================================================


/*
=============
R_DrawViewModel
=============
*/
static void R_DrawViewModel (void)
{
	int			lnum;
	vec3_t		dist;
	float		add;
	dlight_t	*dl;
	entity_t	*e;

	e = &cl.viewent;

	if (!e->model)
		return;

	if (gl_lightmap_format == GL_RGBA)
	{
		ambientlight = R_LightPointColor (e->origin);
		if (lightcolor[0] < 24)
			lightcolor[0] = 24;
		if (lightcolor[1] < 24)
			lightcolor[1] = 24;
		if (lightcolor[2] < 24)
			lightcolor[2] = 24;
		if (ambientlight < 24)
			ambientlight = 24;		// always give some light on gun
	}
	else
	{
		ambientlight = shadelight = R_LightPoint (e->origin);
		if (ambientlight < 24)
			ambientlight = shadelight = 24;	// always give some light on gun
	}

// add dynamic lights
	if (!r_dynamic.integer)
		goto skip_dlights;
	for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
	{
		dl = &cl_dlights[lnum];
		if (!dl->radius)
			continue;
		if (dl->die < cl.time)
			continue;

		VectorSubtract (e->origin, dl->origin, dist);
		add = dl->radius - VectorLengthFast(dist);
		if (add > 0)
		{
			if (gl_lightmap_format == GL_RGBA)
			{
				lightcolor[0] += (float) (dl->color[0] * add);
				lightcolor[1] += (float) (dl->color[1] * add);
				lightcolor[2] += (float) (dl->color[2] * add);
			}
			else
			{
				shadelight += (float) add;
			}

			ambientlight += add;
		}
	}
skip_dlights:

	cl.light_level = ambientlight;

	if ((cl.v.health <= 0) ||
	    (chase_active.integer) ||
//rjr	    (cl.items & IT_INVISIBILITY) ||
	    (!r_drawviewmodel.integer) ||
	    (!r_drawentities.integer) ||
	    (scr_viewsize.integer >= 140))
	{
		return;
	}

	// hack the depth range to prevent view model from poking into walls.
	// Reversed-Z: near is gldepthmax (1.0), so the viewmodel slice sits
	// at the upper 30% of the range instead of the lower 30%.
	if (gl_clipcontrol_able)
		glDepthRange_fp (gldepthmax - 0.3*(gldepthmax-gldepthmin), gldepthmax);
	else
		glDepthRange_fp (gldepthmin, gldepthmin + 0.3*(gldepthmax-gldepthmin));

	/* override projection for weapon FOV if set */
	if (r_viewmodel_fov.value > 0)
	{
		float fov = r_viewmodel_fov.value;
		float aspect = (float)r_refdef.vrect.width / (float)r_refdef.vrect.height;
		GLdouble xmax, ymax;
		if (fov < 30) fov = 30;
		if (fov > 170) fov = 170;
		ymax = 4.0 * tan(fov * M_PI / 360.0);
		xmax = ymax * aspect;
		GL_MatrixMode(GL_MAT_PROJECTION);
		GL_PushMatrix();
		GL_LoadIdentity();
		GL_Frustum(-xmax, xmax, -ymax, ymax, 4.0, 16384);
		GL_MatrixMode(GL_MAT_MODELVIEW);
	}

	AlwaysDrawModel = true;
	R_DrawAliasModel (e);
	AlwaysDrawModel = false;

	if (r_viewmodel_fov.value > 0)
	{
		GL_MatrixMode(GL_MAT_PROJECTION);
		GL_PopMatrix();
		GL_MatrixMode(GL_MAT_MODELVIEW);
	}

	glDepthRange_fp (gldepthmin, gldepthmax);
}

//=============================================================================


/*
===============
R_MarkLeaves
===============
*/
static void R_MarkLeaves (void)
{
	byte	*vis;
	mnode_t	*node;
	int		i;
	byte	solid[4096];

	if (r_oldviewleaf == r_viewleaf && !r_novis.integer)
		return;

	if (mirror)
		return;

	r_visframecount++;
	r_oldviewleaf = r_viewleaf;

	if (r_novis.integer)
	{
		vis = solid;
		memset (solid, 0xff, (cl.worldmodel->numleafs+7)>>3);
	}
	else
		vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);

	for (i = 0; i < cl.worldmodel->numleafs; i++)
	{
		if ( vis[i>>3] & (1<<(i&7)) )
		{
			node = (mnode_t *)&cl.worldmodel->leafs[i+1];
			do
			{
				if (node->visframe == r_visframecount)
					break;
				node->visframe = r_visframecount;
				node = node->parent;
			} while (node);
		}
	}
}

//=============================================================================


/*
=================
GL_DrawBlendPoly

Renders a polygon covering the whole screen. For
fullscreen color blending and approximated gamma
correction. To be called from R_PolyBlend().
=================
*/
static void GL_DrawBlendPoly (void)
{
	GL_ImmBegin ();
	GL_ImmVertex3f (10, 100, 100);
	GL_ImmVertex3f (10, -100, 100);
	GL_ImmVertex3f (10, -100, -100);
	GL_ImmVertex3f (10, 100, -100);
	GL_ImmEnd (GL_QUADS, &gl_shader_flat);
}

/*
=================
GL_DoGamma

Uses GL_DrawBlendPoly() for gamma correction.
Idea originally from LordHavoc.
This trick is useful if normal ways of gamma
adjustment fail: In case of 3dfx Voodoo1/2/Rush,
we can't use 3dfx specific extensions in unix,
so this can be our friend at a cost of 4-5 fps.
To be called from R_PolyBlend().
=================
*/
#if 0
static void GL_DoGamma (void)
{
	if (v_gamma.value >= 1)
		return;

	glBlendFunc_fp (GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);

	GL_ImmBegin ();
	GL_ImmColor4f (1, 1, 1, v_gamma.value);
	GL_ImmVertex3f (10, 100, 100);
	GL_ImmVertex3f (10, -100, 100);
	GL_ImmVertex3f (10, -100, -100);
	GL_ImmVertex3f (10, 100, -100);
	GL_ImmEnd (GL_QUADS, &gl_shader_flat);

	glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
#endif

/*
============
R_PolyBlend
============
*/
static void R_PolyBlend (void)
{
	if (!gl_polyblend.integer || gl_cshiftpercent.value <= 0)
		return;

	if (!v_blend[3])
		return;

	glEnable_fp (GL_BLEND);
	glDisable_fp (GL_DEPTH_TEST);
	GL_SetAlphaThreshold(0.0f);

	GL_LoadIdentity();
	GL_Rotatef(-90, 1, 0, 0);
	GL_Rotatef(90, 0, 0, 1);

	{
		float alpha = v_blend[3] * (gl_cshiftpercent.value / 100.0f);
		if (alpha > 1) alpha = 1;
		GL_ImmBegin ();
		GL_ImmColor4f (v_blend[0], v_blend[1], v_blend[2], alpha);
		GL_ImmVertex3f (10, 100, 100);
		GL_ImmVertex3f (10, -100, 100);
		GL_ImmVertex3f (10, -100, -100);
		GL_ImmVertex3f (10, 100, -100);
		GL_ImmEnd (GL_QUADS, &gl_shader_flat);
	}

	/*GL_DoGamma ();*/

	glDisable_fp (GL_BLEND);
}

//=============================================================================


static int SignbitsForPlane (mplane_t *out)
{
	int	bits, j;

	// for fast box on planeside test

	bits = 0;
	for (j = 0; j < 3; j++)
	{
		if (out->normal[j] < 0)
			bits |= 1<<j;
	}
	return bits;
}

/*
===============
TurnVector -- johnfitz

turn forward towards side on the plane defined by forward and side
if angle = 90, the result will be equal to side
assumes side and forward are perpendicular, and normalized
to turn away from side, use a negative angle
===============
*/
static void TurnVector (vec3_t out, const vec3_t forward, const vec3_t side, float angle)
{
	float	scale_forward, scale_side;

	scale_forward = cos(angle * M_PI / 180.0);
	scale_side = sin(angle * M_PI / 180.0);

	out[0] = scale_forward*forward[0] + scale_side*side[0];
	out[1] = scale_forward*forward[1] + scale_side*side[1];
	out[2] = scale_forward*forward[2] + scale_side*side[2];
}

static void R_SetFrustum (void)
{
	int		i;

	if (r_refdef.fov_x == 90)
	{
		// front side is visible
		VectorAdd (vpn, vright, frustum[0].normal);
		VectorSubtract (vpn, vright, frustum[1].normal);
		VectorAdd (vpn, vup, frustum[2].normal);
		VectorSubtract (vpn, vup, frustum[3].normal);
	}
	else
	{
		TurnVector(frustum[0].normal, vpn, vright, r_refdef.fov_x/2 - 90); // left plane
		TurnVector(frustum[1].normal, vpn, vright, 90 - r_refdef.fov_x/2); // right plane
		TurnVector(frustum[2].normal, vpn, vup,    90 - r_refdef.fov_y/2); // bottom plane
		TurnVector(frustum[3].normal, vpn, vup,    r_refdef.fov_y/2 - 90); // top plane
	}

	for (i = 0; i < 4; i++)
	{
		frustum[i].type = PLANE_ANYZ;
		frustum[i].dist = DotProduct (r_origin, frustum[i].normal);
		frustum[i].signbits = SignbitsForPlane (&frustum[i]);
	}
}


/*
===============
R_SetupFrame
===============
*/
static void R_SetupFrame (void)
{
// don't allow cheats in multiplayer
	if (cl.maxclients > 1)
		Cvar_SetQuick (&r_fullbright, "0");

	R_AnimateLight ();

	r_framecount++;

// build the transformation matrix for the given view angles
	VectorCopy (r_refdef.vieworg, r_origin);

	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

// current viewleaf
	r_oldviewleaf = r_viewleaf;
	r_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);

	V_SetContentsColor (r_viewleaf->contents);
	V_CalcBlend ();

	r_cache_thrash = false;

	c_brush_polys = 0;
	c_alias_polys = 0;
}


#define NEARCLIP	4

/*
=============
R_SetupGL
=============
*/
static void R_SetupGL (void)
{
	int	x, x2, y2, y, w, h;

	/* default: discard fully transparent pixels (alpha=0) but keep
	 * semi-transparent ones. Fence/holey textures set 0.666 where needed. */
	GL_SetAlphaThreshold(0.01f);

	//
	// set up viewpoint
	//
	x  =  r_refdef.vrect.x * glwidth/vid.width;
	x2 = (r_refdef.vrect.x + r_refdef.vrect.width) * glwidth/vid.width;
	y  = (vid.height - r_refdef.vrect.y) * glheight/vid.height;
	y2 = (vid.height - (r_refdef.vrect.y + r_refdef.vrect.height)) * glheight/vid.height;

	// fudge around because of frac screen scale
	if (x > 0)
		x--;
	if (x2 < glwidth)
		x2++;
	if (y2 < 0)
		y2--;
	if (y < glheight)
		y++;

	w = x2 - x;
	h = y - y2;

	glViewport_fp (glx + x, gly + y2, w, h);

	{
		GLdouble xmax = NEARCLIP * tan(r_refdef.fov_x * M_PI / 360.0);
		GLdouble ymax = NEARCLIP * tan(r_refdef.fov_y * M_PI / 360.0);
		GL_MatrixMode(GL_MAT_PROJECTION);
		GL_LoadIdentity();
		GL_Frustum(-xmax, xmax, -ymax, ymax, NEARCLIP, 16384);
	}

	if (mirror)
	{
		if (mirror_plane->normal[2])
			GL_Scalef(1, -1, 1);
		else
			GL_Scalef(-1, 1, 1);
		glCullFace_fp(GL_BACK);
	}
	else
		glCullFace_fp(GL_FRONT);

	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_LoadIdentity();
	GL_Rotatef(-90, 1, 0, 0);
	GL_Rotatef(90, 0, 0, 1);
	GL_Rotatef(-r_refdef.viewangles[2], 1, 0, 0);
	GL_Rotatef(-r_refdef.viewangles[0], 0, 1, 0);
	GL_Rotatef(-r_refdef.viewangles[1], 0, 0, 1);
	GL_Translatef(-r_refdef.vieworg[0], -r_refdef.vieworg[1], -r_refdef.vieworg[2]);

	GL_GetModelview(r_world_matrix);

	//
	// set drawing parms
	//
	if (gl_cull.integer)
		glEnable_fp(GL_CULL_FACE);
	else
		glDisable_fp(GL_CULL_FACE);

	glDisable_fp(GL_BLEND);
	glEnable_fp(GL_DEPTH_TEST);
}

/*
================
R_RenderScene

r_refdef must be set before the first call
================
*/
/* CPU sub-pass timers for r_speeds >= 2 — used by R_RenderScene below
 * and reported in R_ProfileReport above. */
static double	rprof_cpu_marklv;
static double	rprof_cpu_drawworld;
static double	rprof_cpu_sky;
static double	rprof_cpu_ents;
static double	rprof_cpu_glows;
static double	rprof_cpu_dlights;
/* Defined in gl_rsurf.c — finer breakdown of R_DrawWorld */
extern double	rprof_cpu_bsp;
extern double	rprof_cpu_lmupload;
extern double	rprof_cpu_gpucull;
extern double	rprof_cpu_chains;
extern double	rprof_cpu_chains_skystencil;
extern double	rprof_cpu_chains_skyproc;
extern double	rprof_cpu_chains_loop;
extern double	rprof_cpu_chains_deferred;
extern int	rprof_chains_n_surfwalk;
extern int	rprof_chains_n_lmrebuilt;
extern double	rprof_cpu_chains_lmbuild;
extern double	rprof_cpu_chains_surfwalk;
extern double	rprof_cpu_chains_gpufinish;
extern int	rprof_chains_n_fast;
extern int	rprof_chains_n_imm;
extern int	rprof_chains_n_slow;
extern int	rprof_chains_n_skypoly;

static void R_RenderScene (void)
{
	double t0;
#define RPROF_CPU_BEGIN()	(t0 = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0)
#define RPROF_CPU_END(slot)	do { if (r_speeds.integer >= 2) (slot) = Sys_DoubleTime() - t0; } while (0)

	R_SetupFrame ();

	R_SetFrustum ();

	R_SetupGL ();

	GL_ImmResetState ();	/* reset cached GL state for new frame */

	Fog_SetupFrame ();

	RPROF_CPU_BEGIN();
	R_MarkLeaves ();	// done here so we know if we're in water
	RPROF_CPU_END(rprof_cpu_marklv);

	Fog_EnableGFog ();

	RPROF_CPU_BEGIN();
	R_DrawWorld ();		// adds static entities to the list
	RPROF_CPU_END(rprof_cpu_drawworld);

	RPROF_CPU_BEGIN();
	Sky_DrawSky ();		// render skybox
	RPROF_CPU_END(rprof_cpu_sky);

	S_ExtraUpdate ();	// don't let sound get messed up if going slow

	RPROF_CPU_BEGIN();
	R_DrawEntitiesOnList ();
	RPROF_CPU_END(rprof_cpu_ents);

	/* R_DrawAllGlows moved to R_RenderView, AFTER OIT_EndTranslucency:
	 * glows blend additively (GL_ONE, GL_ONE) but the OIT resolve uses
	 * src-alpha blending which multiplies dst by (1 - revealage), so a
	 * glow drawn before OIT_End gets dimmed wherever a translucent
	 * fragment sits over it.  Drawing after OIT_End lets the additive
	 * contribution land on the resolved scene unimpeded.  uhexen2-a0hp. */

	Fog_DisableGFog ();

	RPROF_CPU_BEGIN();
	R_RenderDlights ();
	RPROF_CPU_END(rprof_cpu_dlights);

#undef RPROF_CPU_BEGIN
#undef RPROF_CPU_END
}


/*
=============
R_Clear
=============
*/
static void R_Clear (void)
{
	const GLenum dfunc = gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL;

	if (r_mirroralpha.value != 1.0)
	{
		if (gl_clear.integer)
			glClear_fp (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		else
			glClear_fp (GL_DEPTH_BUFFER_BIT);
		/* Mirror split: primary view occupies the "near" half of the
		 * window-Z range, mirror reflection the "far" half. Reversed-Z
		 * inverts which half is near (1.0 is near), so the splits flip. */
		if (gl_clipcontrol_able)
		{
			gldepthmin = 0.5f;
			gldepthmax = 1.0f;
		}
		else
		{
			gldepthmin = 0.0f;
			gldepthmax = 0.5f;
		}
		glDepthFunc_fp (dfunc);
	}
	else
	{
		if (gl_clear.integer)
			glClear_fp (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		else
			glClear_fp (GL_DEPTH_BUFFER_BIT);
		gldepthmin = 0;
		gldepthmax = 1;
		glDepthFunc_fp (dfunc);
	}

	glDepthRange_fp (gldepthmin, gldepthmax);

	if (have_stencil)
	{
		glClearStencil_fp(0);
		glClear_fp(GL_STENCIL_BUFFER_BIT);
	}
}


/*
=============
R_Mirror
=============
*/
static float	r_base_world_matrix[16];

static void R_Mirror (void)
{
	float		d;
	msurface_t	*s;
	entity_t	*ent;

	if (!mirror)
		return;

	memcpy (r_base_world_matrix, r_world_matrix, sizeof(r_base_world_matrix));

	d = DotProduct (r_refdef.vieworg, mirror_plane->normal) - mirror_plane->dist;
	VectorMA (r_refdef.vieworg, -2*d, mirror_plane->normal, r_refdef.vieworg);

	d = DotProduct (vpn, mirror_plane->normal);
	VectorMA (vpn, -2*d, mirror_plane->normal, vpn);

	r_refdef.viewangles[0] = -asin (vpn[2])/M_PI*180;
	r_refdef.viewangles[1] = atan2 (vpn[1], vpn[0])/M_PI*180;
	r_refdef.viewangles[2] = -r_refdef.viewangles[2];

	ent = &cl_entities[cl.viewentity];
	if (cl_numvisedicts < MAX_VISEDICTS)
	{
		cl_visedicts[cl_numvisedicts] = ent;
		cl_numvisedicts++;
	}

	if (gl_clipcontrol_able)
	{
		gldepthmin = 0.0f;
		gldepthmax = 0.5f;
	}
	else
	{
		gldepthmin = 0.5f;
		gldepthmax = 1.0f;
	}
	glDepthRange_fp (gldepthmin, gldepthmax);
	glDepthFunc_fp (gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL);

	R_RenderScene ();

	glDepthMask_fp(0);

	R_DrawParticles ();

// THIS IS THE F*S*D(KCING MIRROR ROUTINE!  Go down!!!
	R_DrawTransEntitiesOnList (true); // This restores the depth mask

	R_DrawWaterSurfaces (WATER_PHASE_ALL);	/* mirror path runs outside OIT — do both */

	R_DrawTransEntitiesOnList (false);

	if (gl_clipcontrol_able)
	{
		gldepthmin = 0.5f;
		gldepthmax = 1.0f;
	}
	else
	{
		gldepthmin = 0.0f;
		gldepthmax = 0.5f;
	}
	glDepthRange_fp (gldepthmin, gldepthmax);
	glDepthFunc_fp (gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL);

	// blend on top
	glEnable_fp (GL_BLEND);
	GL_MatrixMode(GL_MAT_PROJECTION);
	if (mirror_plane->normal[2])
		GL_Scalef (1,-1,1);
	else
		GL_Scalef (-1,1,1);
	glCullFace_fp(GL_FRONT);
	GL_MatrixMode(GL_MAT_MODELVIEW);

	GL_LoadMatrixf (r_base_world_matrix);

	s = cl.worldmodel->textures[mirrortexturenum]->texturechain;
	for ( ; s ; s = s->texturechain)
		R_RenderBrushPoly (&r_worldentity, s, true);
	cl.worldmodel->textures[mirrortexturenum]->texturechain = NULL;
	glDisable_fp (GL_BLEND);
}


/*
================
R_ShowBoundingBoxes

Draw wireframe axis-aligned bounding boxes around all server entities.
Color-coded by model type: yellow=brush, purple=alias, cyan=sprite, white=other.

r_showbboxes 1 = all entities
r_showbboxes 2 = entities in PVS only (not yet filtered; same as 1 for now)

Filter cvars (Ironwail parity):
r_showbboxes_think  >0 = thinkers only,  <0 = non-thinkers only
r_showbboxes_health >0 = health>0 only,  <0 = health<=0 only
================
*/
static void R_DrawWireBox (vec3_t mins, vec3_t maxs)
{
	/* Bottom face */
	GL_ImmVertex3f (mins[0], mins[1], mins[2]);
	GL_ImmVertex3f (maxs[0], mins[1], mins[2]);

	GL_ImmVertex3f (maxs[0], mins[1], mins[2]);
	GL_ImmVertex3f (maxs[0], maxs[1], mins[2]);

	GL_ImmVertex3f (maxs[0], maxs[1], mins[2]);
	GL_ImmVertex3f (mins[0], maxs[1], mins[2]);

	GL_ImmVertex3f (mins[0], maxs[1], mins[2]);
	GL_ImmVertex3f (mins[0], mins[1], mins[2]);

	/* Top face */
	GL_ImmVertex3f (mins[0], mins[1], maxs[2]);
	GL_ImmVertex3f (maxs[0], mins[1], maxs[2]);

	GL_ImmVertex3f (maxs[0], mins[1], maxs[2]);
	GL_ImmVertex3f (maxs[0], maxs[1], maxs[2]);

	GL_ImmVertex3f (maxs[0], maxs[1], maxs[2]);
	GL_ImmVertex3f (mins[0], maxs[1], maxs[2]);

	GL_ImmVertex3f (mins[0], maxs[1], maxs[2]);
	GL_ImmVertex3f (mins[0], mins[1], maxs[2]);

	/* Vertical edges */
	GL_ImmVertex3f (mins[0], mins[1], mins[2]);
	GL_ImmVertex3f (mins[0], mins[1], maxs[2]);

	GL_ImmVertex3f (maxs[0], mins[1], mins[2]);
	GL_ImmVertex3f (maxs[0], mins[1], maxs[2]);

	GL_ImmVertex3f (maxs[0], maxs[1], mins[2]);
	GL_ImmVertex3f (maxs[0], maxs[1], maxs[2]);

	GL_ImmVertex3f (mins[0], maxs[1], mins[2]);
	GL_ImmVertex3f (mins[0], maxs[1], maxs[2]);
}

/*
================
RayVsAABB

Standard slab test.  Returns true with `*out_t` set to the entry
distance along the ray if the ray (origin + dir*t, t>0) first enters
the box at some t>0.  Returns false if no intersection or if the
camera origin lies inside the box.
================
*/
static qboolean RayVsAABB (const vec3_t origin, const vec3_t dir,
			   const vec3_t mins, const vec3_t maxs, float *out_t)
{
	float	t_near = -1e30f, t_far = 1e30f;
	int	i;

	for (i = 0; i < 3; i++)
	{
		if (fabsf (dir[i]) < 1e-6f)
		{
			if (origin[i] < mins[i] || origin[i] > maxs[i])
				return false;
		}
		else
		{
			float	inv = 1.0f / dir[i];
			float	t1 = (mins[i] - origin[i]) * inv;
			float	t2 = (maxs[i] - origin[i]) * inv;
			if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
			if (t1 > t_near) t_near = t1;
			if (t2 < t_far) t_far = t2;
			if (t_near > t_far)
				return false;
		}
	}
	if (t_near <= 0.0f)
		return false;	/* origin inside or behind box — skip */
	*out_t = t_near;
	return true;
}

/* Pass kind for ShowBBoxes_GetColor — first pass surveys, second draws. */
static qboolean ShowBBoxes_EdictPasses (edict_t *ed, vec3_t mins, vec3_t maxs)
{
	if (!ed || ed->free)
		return false;
	if (r_showbboxes_think.value && (ed->v.nextthink <= 0) == (r_showbboxes_think.value > 0))
		return false;
	if (r_showbboxes_health.value && (ed->v.health <= 0) == (r_showbboxes_health.value > 0))
		return false;

	VectorAdd (ed->v.origin, ed->v.mins, mins);
	VectorAdd (ed->v.origin, ed->v.maxs, maxs);
	if (VectorCompare (mins, maxs))
		return false;	/* zero-size bbox (point ent) */

	return true;
}

static void R_ShowBoundingBoxes (void)
{
	edict_t		*ed, *focused = NULL;
	vec3_t		mins, maxs;
	int		i;
	int		modelindex;
	float		r, g, b, a;
	float		bestdist = 1e30f;
	const char	*focus_target = "", *focus_targetname = "";

	if (!r_showbboxes.integer)
		return;
	if (cls.state != ca_active)
		return;
	if (!sv.active)
		return;

	/* Pass 1: ray-cast through the screen center to pick the focused
	 * entity (closest AABB hit along vpn from r_origin). */
	for (i = 1; i < sv.num_edicts; i++)
	{
		float	dist;

		ed = EDICT_NUM (i);
		if (!ShowBBoxes_EdictPasses (ed, mins, maxs))
			continue;
		if (RayVsAABB (r_origin, vpn, mins, maxs, &dist) && dist < bestdist)
		{
			bestdist = dist;
			focused = ed;
		}
	}

	if (focused && r_showbboxes_targets.integer)
	{
		focus_target     = PR_GetString (focused->v.target);
		focus_targetname = PR_GetString (focused->v.targetname);
		if (!focus_target)     focus_target = "";
		if (!focus_targetname) focus_targetname = "";
	}

	/* Save and disable depth test so boxes show through walls */
	glDisable_fp (GL_DEPTH_TEST);
	glEnable_fp (GL_BLEND);
	glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	/* Pass 2: draw, with color overrides for focused / target-linked
	 * entities and a reddish tint for entities with health. */
	for (i = 1; i < sv.num_edicts; i++)
	{
		qboolean is_focused, is_linked;

		ed = EDICT_NUM (i);
		if (!ShowBBoxes_EdictPasses (ed, mins, maxs))
			continue;

		is_focused = (ed == focused);
		is_linked = false;
		if (focused && ed != focused && r_showbboxes_targets.integer)
		{
			const char *ent_target     = PR_GetString (ed->v.target);
			const char *ent_targetname = PR_GetString (ed->v.targetname);
			if (!ent_target)     ent_target = "";
			if (!ent_targetname) ent_targetname = "";
			if ((*focus_targetname && !strcmp (focus_targetname, ent_target)) ||
			    (*focus_target     && !strcmp (focus_target,     ent_targetname)))
				is_linked = true;
		}

		if (is_focused)
		{
			r = 1.0f; g = 1.0f; b = 1.0f; a = 0.85f;	/* white */
		}
		else if (is_linked)
		{
			r = 0.7f; g = 0.7f; b = 0.7f; a = 0.6f;		/* light grey */
		}
		else
		{
			modelindex = (int)ed->v.modelindex;
			if (modelindex > 0 && modelindex < MAX_MODELS && sv.models[modelindex])
			{
				switch (sv.models[modelindex]->type)
				{
					case mod_brush:		r = 1.0f; g = 1.0f; b = 0.0f; a = 0.5f; break;
					case mod_alias:		r = 0.5f; g = 0.25f; b = 1.0f; a = 0.5f; break;
					case mod_sprite:	r = 0.0f; g = 1.0f; b = 1.0f; a = 0.5f; break;
					default:		r = 1.0f; g = 1.0f; b = 1.0f; a = 0.3f; break;
				}
			}
			else
			{
				r = 1.0f; g = 1.0f; b = 1.0f; a = 0.3f;
			}
			/* Health tint: bias toward red for damageable entities
			 * (matches Ironwail's `if (ed->v.health > 0) color = red`). */
			if (ed->v.health > 0)
			{
				r = 1.0f;
				g *= 0.4f;
				b *= 0.4f;
			}
		}

		GL_ImmBegin ();
		GL_ImmColor4f (r, g, b, a);
		R_DrawWireBox (mins, maxs);
		GL_ImmEnd (GL_LINES, &gl_shader_flat);
	}

	glEnable_fp (GL_DEPTH_TEST);
	glDisable_fp (GL_BLEND);
}


/*
=============
R_PrintTimes
=============
*/
static void R_PrintTimes (void)
{
	float	r_time2;
	float	ms, fps;

	r_lasttime1 = r_time2 = Sys_DoubleTime();

	ms = 1000 * (r_time2 - r_time1);
	fps = 1000 / ms;

	Con_Printf("%3.1f fps %5.0f ms\n%4i wpoly  %4i epoly\n",
			fps, ms, c_brush_polys, c_alias_polys);
}


/*
================
R_RenderView

r_refdef must be set before the first call
================
*/
/* GPU timer query profiler — no glFinish stalls */
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP		0x8E28
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT		0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

#define RPROF_WORLD	0
#define RPROF_PARTICLES	1
#define RPROF_WATER	2
#define RPROF_TRANS	3
#define RPROF_VM	4
#define RPROF_MIRROR	5
#define RPROF_COUNT	6	/* 6 passes = 7 timestamp queries */

static GLuint	rprof_queries[RPROF_COUNT + 1];
static qboolean	rprof_pending;		/* results from previous frame waiting */
static qboolean	rprof_available;	/* queries have been allocated */
static double	rprof_cpu_world;	/* CPU wall-clock for R_RenderScene */
/* sub-pass breakdown — declarations hoisted above R_RenderScene */
static int	rprof_wpoly, rprof_epoly; /* saved from previous frame */

static void R_ProfileInit (void)
{
	if (rprof_available || !glGenQueries_fp)
		return;
	glGenQueries_fp(RPROF_COUNT + 1, rprof_queries);
	rprof_available = true;
	rprof_pending = false;
}

static void R_ProfileTimestamp (int idx)
{
	if (rprof_available)
		glQueryCounter_fp(rprof_queries[idx], GL_TIMESTAMP);
}

static void R_ProfileReport (void)
{
	HWGLuint64 ts[RPROF_COUNT + 1];
	GLint ready = 0;
	int i;
	double ms[RPROF_COUNT], total;

	if (!rprof_available || !rprof_pending)
		return;

	/* Check if last timestamp is ready (implies all earlier ones are too) */
	glGetQueryObjectiv_fp(rprof_queries[RPROF_COUNT], GL_QUERY_RESULT_AVAILABLE, &ready);
	if (!ready)
		return;

	for (i = 0; i <= RPROF_COUNT; i++)
		glGetQueryObjectui64v_fp(rprof_queries[i], GL_QUERY_RESULT, &ts[i]);

	for (i = 0; i < RPROF_COUNT; i++)
		ms[i] = (double)(ts[i + 1] - ts[i]) / 1000000.0;

	total = (double)(ts[RPROF_COUNT] - ts[0]) / 1000000.0;

	Con_Printf("GPU %.1f  CPU %.1f | world %.1f  part %.1f  water %.1f  trans %.1f  vm %.1f  mirr %.1f\n"
		   "  CPU: marklv %.1f  draw %.1f  sky %.1f  ents %.1f (collect %.1f, inst %.1f, loop %.1f [bC=%.1f bD=%.1f p1=%.1f p2=%.1f] a=%d/%.1fms b=%d bL=%d/%.1fms inst[op=%d fc=%d])  glows %.1f  dlt %.1f\n"
		   "  draw: bsp %.1f  lmup %.1f  gcull %.1f  chains %.1f (gpufin %.1f, sky-stencil %.1f, sky-proc %.1f, loop %.1f, defer %.1f)\n"
		   "  chains: fast=%d imm=%d slow=%d skypolys=%d  walk=%d (%.1f ms)  lmrebuild=%d (%.1f ms)\n"
		   "  %4i wpoly  %4i epoly\n",
		   total, rprof_cpu_world * 1000.0,
		   ms[RPROF_WORLD], ms[RPROF_PARTICLES], ms[RPROF_WATER],
		   ms[RPROF_TRANS], ms[RPROF_VM], ms[RPROF_MIRROR],
		   rprof_cpu_marklv * 1000.0,
		   rprof_cpu_drawworld * 1000.0,
		   rprof_cpu_sky * 1000.0,
		   rprof_cpu_ents * 1000.0,
		   rprof_cpu_ents_collect * 1000.0,
		   rprof_cpu_ents_inst * 1000.0,
		   rprof_cpu_ents_loop * 1000.0,
		   rprof_cpu_brush_collect * 1000.0,
		   rprof_cpu_brush_dispatch * 1000.0,
		   rprof_cpu_phase1_loop * 1000.0,
		   rprof_cpu_phase2_loop * 1000.0,
		   rprof_ents_n_alias_loop,
		   rprof_cpu_alias_legacy * 1000.0,
		   rprof_ents_n_brush_loop,
		   rprof_ents_n_brush_legacy,
		   rprof_cpu_brush_legacy * 1000.0,
		   rprof_brush_inst_opaque,
		   rprof_brush_inst_fence,
		   rprof_cpu_glows * 1000.0,
		   rprof_cpu_dlights * 1000.0,
		   rprof_cpu_bsp * 1000.0,
		   rprof_cpu_lmupload * 1000.0,
		   rprof_cpu_gpucull * 1000.0,
		   rprof_cpu_chains * 1000.0,
		   rprof_cpu_chains_gpufinish * 1000.0,
		   rprof_cpu_chains_skystencil * 1000.0,
		   rprof_cpu_chains_skyproc * 1000.0,
		   rprof_cpu_chains_loop * 1000.0,
		   rprof_cpu_chains_deferred * 1000.0,
		   rprof_chains_n_fast,
		   rprof_chains_n_imm,
		   rprof_chains_n_slow,
		   rprof_chains_n_skypoly,
		   rprof_chains_n_surfwalk,
		   rprof_cpu_chains_surfwalk * 1000.0,
		   rprof_chains_n_lmrebuilt,
		   rprof_cpu_chains_lmbuild * 1000.0,
		   rprof_wpoly, rprof_epoly);

	rprof_pending = false;
}

void R_RenderView (void)
{
	if (r_norefresh.integer)
		return;

	/* clamp alpha cvars — match gl_rsurf.c floor of 0.1 to prevent invisible water */
	if (r_wateralpha.value < 0.1f) r_wateralpha.value = 0.1f;
	if (r_wateralpha.value > 1.0f) r_wateralpha.value = 1.0f;

	if (!r_worldentity.model || !cl.worldmodel)
		Sys_Error ("%s: NULL worldmodel", __thisfunc__);

	if (r_speeds.integer)
	{
		if (r_wholeframe.integer)
			r_time1 = r_lasttime1;
		else
			r_time1 = Sys_DoubleTime ();
		/* Save counters before reset — GPU profiler reports 1 frame delayed */
		rprof_wpoly = c_brush_polys;
		rprof_epoly = c_alias_polys;
		c_brush_polys = 0;
		c_alias_polys = 0;
	}

	if (r_speeds.integer >= 2)
	{
		R_ProfileInit();
		/* Report previous frame's results (non-blocking) */
		R_ProfileReport();
	}

	mirror = false;

	R_Clear ();

	// render normal view
	if (r_speeds.integer >= 2) R_ProfileTimestamp(RPROF_WORLD);
	{
		double cpu_start = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
		R_RenderScene ();
		if (r_speeds.integer >= 2) rprof_cpu_world = Sys_DoubleTime() - cpu_start;
	}
	if (r_speeds.integer >= 2) R_ProfileTimestamp(RPROF_PARTICLES);

	/* Opaque liquids (lava, opaque slime/custom turbs) must draw BEFORE
	 * OIT_BeginTranslucency binds the WBOIT FBO — they need plain depth
	 * writes against the scene depth buffer.  Inside the OIT pass they'd
	 * land in the accum buffer with BLEND disabled and be eaten by the
	 * resolve.  Translucent water/ice runs inside OIT below. */
	R_DrawWaterSurfaces (WATER_PHASE_OPAQUE);

	glDepthMask_fp(0);

	R_DrawParticles ();
	if (r_speeds.integer >= 2) R_ProfileTimestamp(RPROF_WATER);

	OIT_BeginTranslucency();

	R_DrawTransEntitiesOnList (r_viewleaf->contents == CONTENTS_EMPTY); // This restores the depth mask

	R_DrawWaterSurfaces (WATER_PHASE_TRANSLUCENT);
	if (r_speeds.integer >= 2) R_ProfileTimestamp(RPROF_TRANS);

	R_DrawTransEntitiesOnList (r_viewleaf->contents != CONTENTS_EMPTY);

	OIT_EndTranslucency(GL_GetSceneFBO());

	/* Additive glow flares: render onto the resolved scene so the
	 * WBOIT src-alpha resolve doesn't dim them under translucent
	 * fragments.  Moved out of R_RenderScene for uhexen2-a0hp. */
	R_DrawAllGlows();

	if (r_speeds.integer >= 2) R_ProfileTimestamp(RPROF_VM);

	R_DrawViewModel();
	if (r_speeds.integer >= 2) R_ProfileTimestamp(RPROF_MIRROR);

	R_ShowBoundingBoxes ();

	// render mirror view
	R_Mirror ();
	if (r_speeds.integer >= 2) { R_ProfileTimestamp(RPROF_COUNT); rprof_pending = true; }

	R_PolyBlend ();

	if (r_speeds.integer == 1)
		R_PrintTimes ();
}

