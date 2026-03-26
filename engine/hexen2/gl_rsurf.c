/*
 * r_surf.c -- surface-related refresh code
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
#include "gl_sky.h"
#include "gl_shader.h"
#include "gl_vbo.h"
#include "gl_matrix.h"
#include "gl_postprocess.h"

/* fog globals from gl_fog.c */
extern float r_fog_density;
extern float r_fog_color[3];

/* ES 3.0 compatibility: GL_QUADS and GL_POLYGON don't exist */
#ifdef EMSCRIPTEN
#ifndef GL_QUADS
#define GL_QUADS 0
#endif
#ifndef GL_POLYGON
#define GL_POLYGON 0
#endif
#endif

int		gl_lightmap_format = GL_RGBA;
cvar_t		gl_lightmapfmt = {"gl_lightmapfmt", "GL_RGBA", CVAR_NONE};
int		lightmap_bytes = 4;		// 1, 2, or 4. default is 4 for GL_RGBA
static int	lightmap_internalformat = 0x8058;	// GL_RGBA8: sized internal format for glTexImage2D
GLuint		lightmap_textures[MAX_LIGHTMAPS];

static unsigned int	blocklights[18*18];
static unsigned int	blocklightscolor[18*18*3];	// colored light support. *3 for RGB to the definitions at the top

#define	BLOCK_WIDTH	256
#define	BLOCK_HEIGHT	256

static glpoly_t	*lightmap_polys[MAX_LIGHTMAPS];
static qboolean	lightmap_modified[MAX_LIGHTMAPS];

static int	allocated[MAX_LIGHTMAPS][BLOCK_WIDTH];

// the lightmap texture data needs to be kept in
// main memory so texsubimage can update properly
static byte	lightmaps[4*MAX_LIGHTMAPS*BLOCK_WIDTH*BLOCK_HEIGHT];

/* Lightmap atlas — all pages in one 2D texture (16x16 grid of 256x256 pages = 4096x4096) */
#define LM_ATLAS_COLS	16
#define LM_ATLAS_ROWS	16
#define LM_ATLAS_WIDTH	(LM_ATLAS_COLS * BLOCK_WIDTH)	/* 2048 */
#define LM_ATLAS_HEIGHT	(LM_ATLAS_ROWS * BLOCK_HEIGHT)	/* 2048 */
static GLuint	lm_atlas_texture;
static int	lm_atlas_layers;	/* number of pages in the atlas */
static qboolean	lm_atlas_enabled;	/* false = fall back to per-surface binds */


/*
===============
R_AddDynamicLights
===============
*/
static void R_AddDynamicLights (msurface_t *surf)
{
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;
	// vars for lit support
	float		cred, cgreen, cblue, brightness;
	unsigned int	*bl;

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	tex = surf->texinfo;

	for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
	{
		if ( !(surf->dlightbits & (1<<lnum)) )
			continue;		// not lit by this light

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) - surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i = 0; i < 3; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] - surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		// lit support (LordHavoc)
		bl = blocklightscolor;
		cred = cl_dlights[lnum].color[0] * 256.0f;
		cgreen = cl_dlights[lnum].color[1] * 256.0f;
		cblue = cl_dlights[lnum].color[2] * 256.0f;

		for (t = 0; t < tmax; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s = 0; s < smax; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
				{
					brightness = rad - dist;
					if (cl_dlights[lnum].dark)
					{
						// clamp to 0
						bl[0] -= (int)(((brightness * cred) < bl[0]) ? (brightness * cred) : bl[0]);
						bl[1] -= (int)(((brightness * cgreen) < bl[1]) ? (brightness * cgreen) : bl[1]);
						bl[2] -= (int)(((brightness * cblue) < bl[2]) ? (brightness * cblue) : bl[2]);
					}
					else
					{
						bl[0] += (int)(brightness * cred);
						bl[1] += (int)(brightness * cgreen);
						bl[2] += (int)(brightness * cblue);
					}

					blocklights[t*smax + s] += (rad - dist)*256;
				}

				bl += 3;
			}
		}
	}
}


/*
===============
GL_SetupLightmapFmt

Used to setup the lightmap_format and lightmap_bytes
during init from VID_Init() and at every level change
from Mod_LoadLighting().
===============
*/
void GL_SetupLightmapFmt (void)
{
	// only GL_LUMINANCE and GL_RGBA are supported
	if (!q_strcasecmp(gl_lightmapfmt.string, "GL_LUMINANCE"))
		gl_lightmap_format = GL_LUMINANCE;
	else if (!q_strcasecmp(gl_lightmapfmt.string, "GL_RGBA"))
		gl_lightmap_format = GL_RGBA;
	else
	{
		gl_lightmap_format = GL_RGBA;
		Cvar_SetQuick (&gl_lightmapfmt, "GL_RGBA");
	}

	if (!host_initialized) // check for cmdline overrides
	{
		if (COM_CheckParm ("-lm_1"))
		{
			gl_lightmap_format = GL_LUMINANCE;
			Cvar_SetQuick (&gl_lightmapfmt, "GL_LUMINANCE");
		}
		else if (COM_CheckParm ("-lm_4"))
		{
			gl_lightmap_format = GL_RGBA;
			Cvar_SetQuick (&gl_lightmapfmt, "GL_RGBA");
		}
	}

	switch (gl_lightmap_format)
	{
	case GL_RGBA:
		lightmap_bytes = 4;
		lightmap_internalformat = 0x8058;	/* GL_RGBA8 */
		break;
	case GL_LUMINANCE:
		lightmap_bytes = 1;
		lightmap_internalformat = GL_LUMINANCE;
		break;
	}
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
static void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)
{
	int		smax, tmax;
	int		t, r, s, q;
	int		i, j, size;
	byte		*lightmap;
	unsigned int	scale;
	int		maps;
	unsigned int	*bl, *blcr, *blcg, *blcb;

	surf->cached_dlight = (surf->dlightframe == r_framecount);

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	size = smax*tmax;
	lightmap = surf->samples;

// set to full bright if no light data
	if (r_fullbright.integer || !cl.worldmodel->lightdata)
	{
		for (i = 0; i < size; i++)
		{
			if (gl_lightmap_format == GL_RGBA)
				blocklightscolor[i*3+0] =
				blocklightscolor[i*3+1] =
				blocklightscolor[i*3+2] = 65280;
			else
				blocklights[i] = 255*256;
		}
		goto store;
	}

// clear to no light
	if (gl_lightmap_format == GL_RGBA)
		memset(blocklightscolor, 0, size * 3 * sizeof(unsigned int));
	else
		memset(blocklights, 0, size * sizeof(unsigned int));

// add all the lightmaps
	if (lightmap)
	{
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ; maps++)
		{
			scale = d_lightstylevalue[surf->styles[maps]];
			surf->cached_light[maps] = scale;	// 8.8 fraction

			if (gl_lightmap_format == GL_RGBA)
			{
				for (i = 0, j = 0; i < size; i++)
				{
					blocklightscolor[i*3+0] += lightmap[j] * scale;
					blocklightscolor[i*3+1] += lightmap[++j] * scale;
					blocklightscolor[i*3+2] += lightmap[++j] * scale;
					j++;
				}

				lightmap += size * 3;
			}
			else
			{
				for (i = 0; i < size; i++)
					blocklights[i] += lightmap[i] * scale;
				lightmap += size;	// skip to next lightmap
			}
		}
	}

// add all the dynamic lights (only when r_dynamic is enabled)
	if (r_dynamic.integer && surf->dlightframe == r_framecount)
		R_AddDynamicLights (surf);

// bound, invert, and shift
store:
	switch (gl_lightmap_format)
	{
	case GL_RGBA:
		stride -= (smax<<2);

		blcr = &blocklightscolor[0];

		if (gl_coloredlight.integer)
		{
			for (i = 0; i < tmax; i++, dest += stride)
			{
				for (j = 0; j < smax; j++)
				{
					q = blcr[0] >> 7; if (q > 255) q = 255;
					r = blcr[1] >> 7; if (r > 255) r = 255;
					s = blcr[2] >> 7; if (s > 255) s = 255;
					dest[0] = q;
					dest[1] = r;
					dest[2] = s;
					dest[3] = 255;
					dest += 4;
					blcr += 3;
				}
			}
		}
		else
		{
			for (i = 0; i < tmax; i++, dest += stride)
			{
				for (j = 0; j < smax; j++)
				{
					q = blcr[0] >> 7; if (q > 255) q = 255;
					r = blcr[1] >> 7; if (r > 255) r = 255;
					s = blcr[2] >> 7; if (s > 255) s = 255;
					t = (q + r + s) / 3;
					if (t > 255) t = 255;
					dest[0] = t;
					dest[1] = t;
					dest[2] = t;
					dest[3] = 255;
					dest += 4;
					blcr += 3;
				}
			}
		}
		break;

	case GL_LUMINANCE:
		bl = blocklights;
		for (i = 0; i < tmax; i++, dest += stride)
		{
			for (j = 0; j < smax; j++)
			{
				t = *bl++;
				t >>= 7;
				if (t > 255)
					t = 255;
				dest[j] = 255-t;
			}
		}
		break;
	default:
		Sys_Error ("Bad lightmap format");
	}
}


/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
static texture_t *R_TextureAnimation (entity_t *e, texture_t *base)
{
	int		reletive;
	int		count;

	if (e->frame)
	{
		if (base->alternate_anims)
			base = base->alternate_anims;
	}

	if (!base->anim_total)
		return base;

	reletive = (int)(cl.time*10) % base->anim_total;

	count = 0;
	while (base->anim_min > reletive || base->anim_max <= reletive)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("%s: broken cycle", __thisfunc__);
		if (++count > 100)
			Sys_Error ("%s: infinite cycle", __thisfunc__);
	}

	return base;
}


/*
=============================================================

BRUSH MODELS

=============================================================
*/

/*
================
DrawGLWaterPoly

Warp the vertex coordinates
================
*/
static void DrawGLWaterPoly (glpoly_t *p)
{
	int	i;
	float	*v;
	vec3_t	nv;

	GL_ImmBegin ();
	/* color inherited from caller (R_RenderBrushPoly sets intensity/alpha) */
	v = p->verts[0];
	for (i = 0; i < p->numverts; i++, v+= VERTEXSIZE)
	{
		GL_ImmTexCoord2f (v[3], v[4]);

		if (r_waterwarp.integer && !GL_PostProcess_Active())
		{
			nv[0] = v[0] + 8*sin(v[1]*0.05+realtime)*sin(v[2]*0.05+realtime);
			nv[1] = v[1] + 8*sin(v[0]*0.05+realtime)*sin(v[2]*0.05+realtime);
			nv[2] = v[2];
			GL_ImmVertex3f (nv[0], nv[1], nv[2]);
		}
		else
		{
			GL_ImmVertex3f (v[0], v[1], v[2]);
		}
	}
	GL_ImmEnd (GL_TRIANGLE_FAN, &gl_shader_alias);
}

static void DrawGLWaterPolyMTexLM (glpoly_t *p)
{
	int	i;
	float	*v;
	vec3_t	nv;

	GL_ImmBegin ();
	/* color inherited from caller (R_RenderBrushPoly sets intensity/alpha) */
	v = p->verts[0];
	for (i = 0; i < p->numverts; i++, v+= VERTEXSIZE)
	{
		GL_ImmTexCoord2f (v[3], v[4]);
		GL_ImmLMCoord2f (v[5], v[6]);

		if (r_waterwarp.integer && !GL_PostProcess_Active())
		{
			nv[0] = v[0] + 8*sin(v[1]*0.05+realtime)*sin(v[2]*0.05+realtime);
			nv[1] = v[1] + 8*sin(v[0]*0.05+realtime)*sin(v[2]*0.05+realtime);
			nv[2] = v[2];
			GL_ImmVertex3f (nv[0], nv[1], nv[2]);
		}
		else
		{
			GL_ImmVertex3f (v[0], v[1], v[2]);
		}
	}
	GL_ImmEnd (GL_TRIANGLE_FAN, &gl_shader_world);
}

/*
================
DrawGLPoly
================
*/
static void DrawGLPoly (glpoly_t *p)
{
	int	i;
	float	*v;

	GL_ImmBegin ();
	/* color inherited from caller (R_RenderBrushPoly sets intensity/alpha) */
	v = p->verts[0];
	for (i = 0; i < p->numverts; i++, v+= VERTEXSIZE)
	{
		GL_ImmTexCoord2f (v[3], v[4]);
		GL_ImmVertex3f (v[0], v[1], v[2]);
	}
	GL_ImmEnd (GL_POLYGON, &gl_shader_alias);
}

static void DrawGLPolyMTex (glpoly_t *p)
{
	int	i;
	float	*v;

	GL_ImmBegin ();
	/* color inherited from caller (R_RenderBrushPoly sets intensity/alpha) */
	v = p->verts[0];
	for (i = 0; i < p->numverts; i++, v+= VERTEXSIZE)
	{
		GL_ImmTexCoord2f (v[3], v[4]);
		GL_ImmLMCoord2f (v[5], v[6]);

		GL_ImmVertex3f (v[0], v[1], v[2]);
	}
	GL_ImmEnd (GL_POLYGON, &gl_shader_world);
}



static void R_UpdateLightmaps (qboolean Translucent)
{
	unsigned int		i;
	glpoly_t	*p;

	if (r_fullbright.integer)
		return;

	glActiveTextureARB_fp (GL_TEXTURE1_ARB);

	if (! lightmap_textures[0])
	{
		// if lightmaps were hosed in a video mode change, make
		// sure we allocate new slots for lightmaps, otherwise
		// we'll probably overwrite some other existing textures.
		glGenTextures_fp(MAX_LIGHTMAPS, lightmap_textures);
	}

	for (i = 0; i < MAX_LIGHTMAPS; i++)
	{
		if (!lightmap_textures[i])
			break;		// no more used

		if (lightmap_modified[i])
		{
			lightmap_modified[i] = false;

			/* Update individual lightmap (kept for compatibility) */
			GL_Bind(lightmap_textures[i]);
			glTexImage2D_fp (GL_TEXTURE_2D, 0, lightmap_internalformat, BLOCK_WIDTH,
					BLOCK_HEIGHT, 0, gl_lightmap_format, GL_UNSIGNED_BYTE,
					lightmaps + i*BLOCK_WIDTH*BLOCK_HEIGHT*lightmap_bytes);

			/* Patch the dirty page in the atlas */
			if (lm_atlas_enabled && lm_atlas_texture)
			{
				int col = i % LM_ATLAS_COLS;
				int row = i / LM_ATLAS_COLS;
				glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
				glTexSubImage2D_fp(GL_TEXTURE_2D, 0,
						col * BLOCK_WIDTH, row * BLOCK_HEIGHT,
						BLOCK_WIDTH, BLOCK_HEIGHT,
						gl_lightmap_format, GL_UNSIGNED_BYTE,
						lightmaps + i*BLOCK_WIDTH*BLOCK_HEIGHT*lightmap_bytes);
				currenttexture = GL_UNUSED_TEXTURE;
			}
		}
	}

	glActiveTextureARB_fp (GL_TEXTURE0_ARB);
}


/*
================
R_RenderBrushPoly
================
*/
void R_RenderBrushPoly (entity_t *e, msurface_t *fa, qboolean override)
{
	texture_t	*t;
	byte		*base;
	int		maps;
	float		intensity, alpha_val;

	c_brush_polys++;

	glActiveTextureARB_fp(GL_TEXTURE0_ARB);

	intensity = 1.0f;
	alpha_val = 1.0f;

	if (e->drawflags & DRF_TRANSLUCENT)
	{
		glEnable_fp (GL_BLEND);
		alpha_val = r_wateralpha.value;
	}
	if ((e->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT)
	{
		// ent->abslight   0 - 255
		intensity = (float)e->abslight / 255.0f;
	}

	GL_ImmColor4f(intensity, intensity, intensity, alpha_val);

	if (fa->flags & SURF_DRAWSKY)
	{	// warp texture, no lightmaps
		EmitBothSkyLayers (fa);
		return;
	}

	t = R_TextureAnimation (e, fa->texinfo->texture);
	GL_Bind (t->gl_texturenum);

	if (fa->flags & SURF_DRAWTURB)
	{	// warp texture — sample light at surface center for tinting
		if (fa->polys && !r_fullbright.integer && intensity >= 1.0f)
		{
			extern vec3_t lightcolor;
			float *v = fa->polys->verts[0];
			vec3_t mid;
			float lv;
			mid[0] = v[0]; mid[1] = v[1]; mid[2] = v[2];
			R_LightPointColor(mid);
			lv = (lightcolor[0] + lightcolor[1] + lightcolor[2]) / (3.0f * 200.0f);
			if (lv > 1.0f) lv = 1.0f;
			if (lv < 0.15f) lv = 0.15f;
			GL_ImmColor4f(lv, lv, lv, alpha_val);
		}
		EmitWaterPolys (fa);
		return;
	}

	if (fa->flags & SURF_DRAWFENCE)
	{
		glDisable_fp(GL_BLEND);
		GL_SetAlphaThreshold(0.666f);
		if (r_alphatocoverage.integer)
			glEnable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	}

	if ((e->drawflags & DRF_TRANSLUCENT) ||
	    (e->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT)
	{
		if (fa->flags & SURF_UNDERWATER)
			DrawGLWaterPoly (fa->polys);
		else
			DrawGLPoly (fa->polys);
	}
	else
	{
		glActiveTextureARB_fp(GL_TEXTURE1_ARB);

		if (lm_atlas_texture)
			glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
		else
			GL_Bind (lightmap_textures[fa->lightmaptexturenum]);

		if (fa->flags & SURF_UNDERWATER)
			DrawGLWaterPolyMTexLM (fa->polys);
		else
			DrawGLPolyMTex (fa->polys);

		glActiveTextureARB_fp(GL_TEXTURE0_ARB);
	}

	// add the poly to the proper lightmap chain
	fa->polys->chain = lightmap_polys[fa->lightmaptexturenum];
	lightmap_polys[fa->lightmaptexturenum] = fa->polys;

	// check for lightmap modification
	for (maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
	{
		if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
			goto dynamic;
	}

	if (fa->dlightframe == r_framecount	// dynamic this frame
		|| fa->cached_dlight)		// dynamic previously
	{
dynamic:
		/* Always rebuild: r_dynamic only gates new dlight addition
		 * (inside R_BuildLightMap via dlightframe check), not cleanup.
		 * Skipping the rebuild leaves stale bright squares when r_dynamic
		 * is toggled off after a light was applied. */
		lightmap_modified[fa->lightmaptexturenum] = true;
		base = lightmaps + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
		base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
		R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
	}

	if (e->drawflags & DRF_TRANSLUCENT)
	{
		glDisable_fp (GL_BLEND);
	}

	if (fa->flags & SURF_DRAWFENCE)
	{
		GL_SetAlphaThreshold(0.01f);
		if (r_alphatocoverage.integer)
			glDisable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	}
}

void R_RenderBrushPolyMTex (entity_t *e, msurface_t *fa, qboolean override)
{
	texture_t	*t;
	byte		*base;
	int		maps;
	float		intensity, alpha_val;

	c_brush_polys++;

	glActiveTextureARB_fp(GL_TEXTURE0_ARB);

	intensity = 1.0f;
	alpha_val = 1.0f;

	if (e->drawflags & DRF_TRANSLUCENT)
	{
		glEnable_fp (GL_BLEND);
		alpha_val = r_wateralpha.value;
	}
	else
	{
		/* KIERO: Seems it's enabled when we enter here. */
		glDisable_fp (GL_BLEND);
	}

	if ((e->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT)
	{
		intensity = (float)e->abslight / 255.0f;
	}

	if (fa->flags & SURF_DRAWTURB)
	{
		glDisable_fp (GL_BLEND);
		glActiveTextureARB_fp(GL_TEXTURE1_ARB);
		glActiveTextureARB_fp(GL_TEXTURE0_ARB);

		intensity = 1.0;
	}

	if (!override)
		GL_ImmColor4f(intensity, intensity, intensity, alpha_val);

	if (fa->flags & SURF_DRAWSKY)
	{	// warp texture, no lightmaps
		EmitBothSkyLayers (fa);
		return;
	}

	glActiveTextureARB_fp(GL_TEXTURE0_ARB);
	t = R_TextureAnimation (e, fa->texinfo->texture);
	GL_Bind (t->gl_texturenum);

	if (fa->flags & SURF_DRAWFENCE)
	{
		glDisable_fp(GL_BLEND);
		GL_SetAlphaThreshold(0.666f);
		if (r_alphatocoverage.integer)
			glEnable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	}

	if (fa->flags & SURF_DRAWTURB)
	{
		GL_ImmColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		EmitWaterPolys (fa);
		//return;
	}
	else
	{
		if ((e->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT)
		{
			glActiveTextureARB_fp(GL_TEXTURE0_ARB);

			if (fa->flags & SURF_UNDERWATER)
				DrawGLWaterPoly (fa->polys);
			else
				DrawGLPoly (fa->polys);
		}
		else
		{
			glActiveTextureARB_fp(GL_TEXTURE1_ARB);
			if (lm_atlas_texture)
				glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
			else
				GL_Bind (lightmap_textures[fa->lightmaptexturenum]);

			if (fa->flags & SURF_UNDERWATER)
				DrawGLWaterPolyMTexLM (fa->polys);
			else
				DrawGLPolyMTex (fa->polys);
		}

		glActiveTextureARB_fp(GL_TEXTURE1_ARB);

		// add the poly to the proper lightmap chain
		fa->polys->chain = lightmap_polys[fa->lightmaptexturenum];
		lightmap_polys[fa->lightmaptexturenum] = fa->polys;

		// check for lightmap modification
		for (maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
		{
			if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
				goto dynamic1;
		}

		if (fa->dlightframe == r_framecount	// dynamic this frame
		    || fa->cached_dlight)		// dynamic previously
		{
dynamic1:
			lightmap_modified[fa->lightmaptexturenum] = true;
			base = lightmaps + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
			base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
			R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
		}
	}

	glActiveTextureARB_fp(GL_TEXTURE0_ARB);

	if (e->drawflags & DRF_TRANSLUCENT)
	{
		glDisable_fp (GL_BLEND);
	}

	if (fa->flags & SURF_DRAWFENCE)
	{
		GL_SetAlphaThreshold(0.01f);
		if (r_alphatocoverage.integer)
			glDisable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	}

	glActiveTextureARB_fp(GL_TEXTURE1_ARB);
}


/*
================
R_MirrorChain
================
*/
static void R_MirrorChain (msurface_t *s)
{
	if (mirror)
		return;
	mirror = true;
	mirror_plane = s->plane;
}


/*
================
R_DrawWaterSurfaces
================
*/
/* Per-liquid alpha: returns the appropriate alpha for a turb texture.
 * r_lavaalpha/r_slimealpha/r_telealpha override r_wateralpha when > 0. */
static float R_LiquidAlpha (const texture_t *t)
{
	if (t->name[0] == '*')
	{
		if (!q_strncasecmp(t->name + 1, "lava", 4))
		{	if (r_lavaalpha.value <= 0) return 1.0f;
			float a = r_lavaalpha.value; if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
		if (!q_strncasecmp(t->name + 1, "slime", 5))
		{	if (r_slimealpha.value <= 0) return 1.0f;
			float a = r_slimealpha.value; if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
		if (!q_strncasecmp(t->name + 1, "tele", 4))
		{	if (r_telealpha.value <= 0) return 1.0f;
			float a = r_telealpha.value; if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
	}
	{	float a = r_wateralpha.value; if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
}

void R_DrawWaterSurfaces (void)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;

	if (r_wateralpha.value < 0.1)
		r_wateralpha.value = 0.1;
	if (r_wateralpha.value > 1)
		r_wateralpha.value = 1;
	if (r_wateralpha.value == 1.0)
		return;

	glDepthMask_fp(0);

	//
	// go back to the world matrix
	//
	GL_LoadMatrixf (r_world_matrix);

	glEnable_fp (GL_BLEND);
	GL_SetAlphaThreshold(0.0f);	/* don't discard translucent water */
	GL_ImmColor4f (1,1,1,r_wateralpha.value);

	for (i = 0; i < cl.worldmodel->numtextures; i++)
	{
		t = cl.worldmodel->textures[i];
		if (!t)
			continue;
		s = t->texturechain;
		if (!s)
			continue;
		if (!(s->flags & SURF_DRAWTURB))
			continue;

		/* skip liquids that were drawn opaque in the main pass */
		if (R_LiquidAlpha(t) >= 1.0f && !(s->flags & SURF_TRANSLUCENT))
		{
			t->texturechain = NULL;
			continue;
		}

		GL_ImmColor4f (1,1,1, R_LiquidAlpha(t));

		// set modulate mode explicitly
		GL_Bind (t->gl_texturenum);

		for ( ; s ; s = s->texturechain)
			EmitWaterPolys (s);

		t->texturechain = NULL;
	}

	GL_ImmColor4f (1,1,1,1);
	GL_SetAlphaThreshold(0.01f);	/* restore default */
	glDisable_fp (GL_BLEND);
	glDepthMask_fp (1);
}

/*
================
DrawTextureChains
================
*/
/*
================
R_BatchEmitSurface

Convert a polygon (triangle fan) to triangles and add to the
current immediate-mode batch. Flushes and restarts if the batch
is nearly full. Handles lightmap dirty checks inline.
================
*/
static void R_BatchEmitSurface (msurface_t *fa)
{
	glpoly_t	*p = fa->polys;
	float		*v;
	int		j, nverts;
	byte		*base;
	int		maps;

	nverts = p->numverts;
	if (nverts < 3)
		return;

	/* Check if we need to flush before adding this surface.
	 * Each polygon of N verts produces (N-2)*3 triangle verts. */
	if (GL_ImmCount() + (nverts - 2) * 3 >= GL_IMM_MAX_VERTS - 6)
	{
		/* Flush current batch */
		GL_ImmEnd (GL_TRIANGLES, &gl_shader_world);
		GL_ImmBegin ();
	}

	/* No lightmap rebind needed — atlas UVs already point to the
	 * correct page within the single atlas texture */

	/* Emit as triangles (fan → triangles: v0-v1-v2, v0-v2-v3, ...) */
	v = p->verts[0];
	for (j = 2; j < nverts; j++)
	{
		float *v0 = p->verts[0];
		float *v1 = p->verts[j - 1];
		float *v2 = p->verts[j];

		GL_ImmTexCoord2f (v0[3], v0[4]);
		GL_ImmLMCoord2f (v0[5], v0[6]);
		GL_ImmVertex3f (v0[0], v0[1], v0[2]);

		GL_ImmTexCoord2f (v1[3], v1[4]);
		GL_ImmLMCoord2f (v1[5], v1[6]);
		GL_ImmVertex3f (v1[0], v1[1], v1[2]);

		GL_ImmTexCoord2f (v2[3], v2[4]);
		GL_ImmLMCoord2f (v2[5], v2[6]);
		GL_ImmVertex3f (v2[0], v2[1], v2[2]);
	}

	c_brush_polys++;

	/* Add to lightmap chain and check for dynamic updates */
	fa->polys->chain = lightmap_polys[fa->lightmaptexturenum];
	lightmap_polys[fa->lightmaptexturenum] = fa->polys;

	for (maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
	{
		if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
			goto dynamic_batch;
	}
	if (fa->dlightframe == r_framecount || fa->cached_dlight)
	{
dynamic_batch:
		lightmap_modified[fa->lightmaptexturenum] = true;
		base = lightmaps + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
		base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
		R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
	}
}

/* Forward declarations for static world VBO (defined below R_DrawWorld) */
static GLuint	world_vbo;
static GLuint	world_ibo;
static GLuint	world_vao;
static int	world_num_verts;
static int	world_num_indices;

static void DrawTextureChains (entity_t *e)
{
	int		i;
	msurface_t	*s;
	texture_t	*t;

	for (i = 0; i < cl.worldmodel->numtextures; i++)
	{
		t = cl.worldmodel->textures[i];
		if (!t)
			continue;
		s = t->texturechain;
		if (!s)
			continue;
		if (i == skytexturenum)
		{
			if (skybox_name[0])
			{
				// Process sky polys to build bounds for Sky_DrawSkyBox
				msurface_t *sky;
				for (sky = s; sky; sky = sky->texturechain)
					Sky_ProcessPoly (sky->polys);
			}
			else
			{
				R_DrawSkyChain (s);
			}
			t->texturechain = NULL;
			continue;
		}
		else if (i == mirrortexturenum && r_mirroralpha.value != 1.0)
		{
			R_MirrorChain (s);
			continue;
		}
		else
		{
			if ((s->flags & SURF_DRAWTURB) && r_wateralpha.value != 1.0
			    && ((s->flags & SURF_TRANSLUCENT) ||
			        R_LiquidAlpha(t) < 1.0f))
				continue;	// draw translucent water later

			if (((e->drawflags & DRF_TRANSLUCENT) ||
				(e->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT))
			{
				for ( ; s ; s = s->texturechain)
					R_RenderBrushPoly (e, s, false);
			}
			else if (lm_atlas_enabled && lm_atlas_texture && world_vao && e == &r_worldentity)
			{
				/* Static VBO path: world geometry is pre-uploaded.
				 * Just issue glDrawElements per visible surface
				 * using the pre-built IBO. Avoids per-vertex CPU work. */
				float mvp[16], mv[16];

				glBindVertexArray_fp(world_vao);

				/* ATTR_COLOR is not in the world VBO — set default to white.
				 * When a vertex attrib array is disabled, the generic
				 * attribute value is used (GL 2.0 spec). */
				glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);

				glUseProgram_fp(gl_shader_world.program);
				GL_GetMVP(mvp);
				GL_GetModelview(mv);
				if (gl_shader_world.u_mvp >= 0)
					glUniformMatrix4fv_fp(gl_shader_world.u_mvp, 1, GL_FALSE, mvp);
				if (gl_shader_world.u_modelview >= 0)
					glUniformMatrix4fv_fp(gl_shader_world.u_modelview, 1, GL_FALSE, mv);
				if (gl_shader_world.u_fog_density >= 0)
					glUniform1f_fp(gl_shader_world.u_fog_density, r_fog_density);
				if (gl_shader_world.u_fog_color >= 0)
					glUniform3f_fp(gl_shader_world.u_fog_color,
						       r_fog_color[0], r_fog_color[1], r_fog_color[2]);
				if (gl_shader_world.u_alpha_threshold >= 0)
					glUniform1f_fp(gl_shader_world.u_alpha_threshold, 0.01f);

				glActiveTextureARB_fp(GL_TEXTURE0_ARB);
				{
					texture_t *tt = R_TextureAnimation (e, s->texinfo->texture);
					GL_Bind (tt->gl_texturenum);
				}
				glActiveTextureARB_fp(GL_TEXTURE1_ARB);
				glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
				glActiveTextureARB_fp(GL_TEXTURE0_ARB);

				/* Collect visible surfaces, then batch-draw.
			 * Merge contiguous IBO ranges into single draw calls. */
			{
				/* First pass: collect into temp array */
				#define MAX_BATCH_SURFS 4096
				static msurface_t *batch_surfs[MAX_BATCH_SURFS];
				int batch_count = 0;

				for ( ; s ; s = s->texturechain)
				{
					if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB |
							SURF_DRAWFENCE | SURF_UNDERWATER))
					{
						glBindVertexArray_fp(0);
						glUseProgram_fp(0);
						R_RenderBrushPolyMTex (e, s, false);
						glBindVertexArray_fp(world_vao);
						glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
						glUseProgram_fp(gl_shader_world.program);
						{
							texture_t *tt = R_TextureAnimation (e, s->texinfo->texture);
							glActiveTextureARB_fp(GL_TEXTURE0_ARB);
							GL_Bind (tt->gl_texturenum);
						}
						continue;
					}

					if (s->vbo_numtris > 0 && batch_count < MAX_BATCH_SURFS)
						batch_surfs[batch_count++] = s;

					/* Track lightmap polys for dynamic updates */
					if (s->polys)
					{
						int maps;
						s->polys->chain = lightmap_polys[s->lightmaptexturenum];
						lightmap_polys[s->lightmaptexturenum] = s->polys;
						for (maps = 0; maps < MAXLIGHTMAPS && s->styles[maps] != 255; maps++)
						{
							if (d_lightstylevalue[s->styles[maps]] != s->cached_light[maps])
							{
								lightmap_modified[s->lightmaptexturenum] = true;
								break;
							}
						}
						if (s->dlightframe == r_framecount || s->cached_dlight)
							lightmap_modified[s->lightmaptexturenum] = true;
					}
				}

				/* Second pass: merge contiguous IBO ranges and draw */
				if (batch_count > 0)
				{
					int run_start = 0;
					int run_first_idx = batch_surfs[0]->vbo_firstindex;
					int run_total_idx = batch_surfs[0]->vbo_numtris * 3;
					int k;

					for (k = 1; k < batch_count; k++)
					{
						int expected = run_first_idx + run_total_idx;
						if (batch_surfs[k]->vbo_firstindex == expected)
						{
							/* Contiguous — extend the run */
							run_total_idx += batch_surfs[k]->vbo_numtris * 3;
						}
						else
						{
							/* Gap — flush the run */
							glDrawElements_fp(GL_TRIANGLES,
									  run_total_idx,
									  GL_UNSIGNED_INT,
									  (void *)((size_t)run_first_idx * sizeof(unsigned int)));
							c_brush_polys += (k - run_start);
							run_start = k;
							run_first_idx = batch_surfs[k]->vbo_firstindex;
							run_total_idx = batch_surfs[k]->vbo_numtris * 3;
						}
					}
					/* Flush final run */
					glDrawElements_fp(GL_TRIANGLES,
							  run_total_idx,
							  GL_UNSIGNED_INT,
							  (void *)((size_t)run_first_idx * sizeof(unsigned int)));
					c_brush_polys += (batch_count - run_start);
				}
			}

				glBindVertexArray_fp(0);
				glUseProgram_fp(0);
			}
			else if (lm_atlas_enabled && lm_atlas_texture)
			{
				/* ImmBegin fallback for non-world brush entities */
				GL_ImmColor4f (1, 1, 1, 1);

				glActiveTextureARB_fp(GL_TEXTURE0_ARB);
				{
					texture_t *tt = R_TextureAnimation (e, s->texinfo->texture);
					GL_Bind (tt->gl_texturenum);
				}
				glActiveTextureARB_fp(GL_TEXTURE1_ARB);
				glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
				glActiveTextureARB_fp(GL_TEXTURE0_ARB);

				GL_ImmBegin ();

				for ( ; s ; s = s->texturechain)
				{
					if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB |
							SURF_DRAWFENCE | SURF_UNDERWATER))
					{
						GL_ImmEnd (GL_TRIANGLES, &gl_shader_world);
						R_RenderBrushPolyMTex (e, s, false);
						{
							texture_t *tt = R_TextureAnimation (e, s->texinfo->texture);
							glActiveTextureARB_fp(GL_TEXTURE0_ARB);
							GL_Bind (tt->gl_texturenum);
						}
						GL_ImmBegin ();
						continue;
					}

					R_BatchEmitSurface (s);
				}

				GL_ImmEnd (GL_TRIANGLES, &gl_shader_world);
			}
			else
			{
				/* No atlas — per-surface lightmap binds */
				for ( ; s ; s = s->texturechain)
					R_RenderBrushPolyMTex (e, s, false);
			}
		}

		t->texturechain = NULL;
	}
}

/*
=================
R_DrawBrushModel
=================
*/
void R_DrawBrushModel (entity_t *e, qboolean Translucent)
{
	int		i, k;
	vec3_t		mins, maxs;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;
	qmodel_t	*clmodel;
	qboolean	rotated;

	currenttexture = GL_UNUSED_TEXTURE;
	GL_ImmResetState();

	clmodel = e->model;

	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		rotated = true;
		for (i = 0; i < 3; i++)
		{
			mins[i] = e->origin[i] - clmodel->radius;
			maxs[i] = e->origin[i] + clmodel->radius;
		}
	}
	else
	{
		rotated = false;
		VectorAdd (e->origin, clmodel->mins, mins);
		VectorAdd (e->origin, clmodel->maxs, maxs);
	}

	if (R_CullBox (mins, maxs))
		return;

#if 0 /* causes side effects in 16 bpp. alternative down below */
	/* Get rid of Z-fighting for textures by offsetting the
	 * drawing of entity models compared to normal polygons. */
	if (gl_zfix.integer)
	{
		glEnable_fp(GL_POLYGON_OFFSET_FILL);
		glEnable_fp(GL_POLYGON_OFFSET_LINE);
	}
#endif /* #if 0 */

	GL_ImmColor3f (1,1,1);
	memset (lightmap_polys, 0, sizeof(lightmap_polys));

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (rotated)
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

// calculate dynamic lighting for bmodel if it's not an
// instanced model
	if (clmodel->firstmodelsurface != 0 && !gl_flashblend.integer)
	{
		for (k = 0; k < MAX_DLIGHTS; k++)
		{
			if ((cl_dlights[k].die < cl.time) || (!cl_dlights[k].radius))
				continue;

			R_MarkLights (&cl_dlights[k], 1<<k,
					clmodel->nodes + clmodel->hulls[0].firstclipnode);
		}
	}

	GL_PushMatrix();
	e->angles[0] = -e->angles[0];	// stupid quake bug
	e->angles[2] = -e->angles[2];	// stupid quake bug
	/* hack the origin to prevent bmodel z-fighting
	 * http://forums.inside3d.com/viewtopic.php?t=1350 */
	if (gl_zfix.integer)
	{
		e->origin[0] -= DIST_EPSILON;
		e->origin[1] -= DIST_EPSILON;
		e->origin[2] -= DIST_EPSILON;
	}
	R_RotateForEntity (e);
	/* un-hack the origin */
	if (gl_zfix.integer)
	{
		e->origin[0] += DIST_EPSILON;
		e->origin[1] += DIST_EPSILON;
		e->origin[2] += DIST_EPSILON;
	}
	e->angles[0] = -e->angles[0];	// stupid quake bug
	e->angles[2] = -e->angles[2];	// stupid quake bug

	//
	// draw texture
	//
	for (i = 0; i < clmodel->nummodelsurfaces; i++, psurf++)
	{
	// find which side of the node we are on
		pplane = psurf->plane;

		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;

	// draw the polygon
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			R_RenderBrushPoly (e, psurf, false);
		}
	}

	/* Lightmaps applied via multitexture in R_RenderBrushPoly */
	GL_PopMatrix();
#if 0 /* see above... */
	if (gl_zfix.integer)
	{
		glDisable_fp(GL_POLYGON_OFFSET_FILL);
		glDisable_fp(GL_POLYGON_OFFSET_LINE);
	}
#endif /* #if 0 */
}


/*
=============================================================

WORLD MODEL

=============================================================
*/

/*
================
R_RecursiveWorldNode
================
*/
static void R_RecursiveWorldNode (mnode_t *node)
{
	int		c, side;
	mplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	double		dot;

	if (node->contents == CONTENTS_SOLID)
		return;

	if (node->visframe != r_visframecount)
		return;
	if (R_CullBox (node->minmaxs, node->minmaxs+3))
		return;

	/* CPU-side draw distance — skip subtrees beyond r_farclip.
	 * This replaces the old GPU far clip plane, avoiding artifacts
	 * with sky, entities, and scrolling sky at the clip boundary. */
	{
		float dx = ((node->minmaxs[0] + node->minmaxs[3]) * 0.5f) - r_origin[0];
		float dy = ((node->minmaxs[1] + node->minmaxs[4]) * 0.5f) - r_origin[1];
		float dz = ((node->minmaxs[2] + node->minmaxs[5]) * 0.5f) - r_origin[2];
		float dist_sq = dx*dx + dy*dy + dz*dz;
		float clip = r_farclip.value;
		if (dist_sq > clip * clip)
			return;
	}

	if (node->contents < 0)
	{
		pleaf = (mleaf_t *)node;
		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;
		if (c)
		{
			do
			{
				(*mark)->visframe = r_framecount;
				mark++;
			} while (--c);
		}
		if (pleaf->efrags)
			R_StoreEfrags (&pleaf->efrags);
		return;
	}

	plane = node->plane;
	switch (plane->type)
	{
	case PLANE_X: dot = modelorg[0] - plane->dist; break;
	case PLANE_Y: dot = modelorg[1] - plane->dist; break;
	case PLANE_Z: dot = modelorg[2] - plane->dist; break;
	default: dot = DotProduct(modelorg, plane->normal) - plane->dist; break;
	}
	side = (dot >= 0) ? 0 : 1;

	R_RecursiveWorldNode (node->children[side]);

	c = node->numsurfaces;
	if (c)
	{
		surf = cl.worldmodel->surfaces + node->firstsurface;
		if (dot < 0 -BACKFACE_EPSILON)
			side = SURF_PLANEBACK;
		else if (dot > BACKFACE_EPSILON)
			side = 0;

		for ( ; c ; c--, surf++)
		{
			if (surf->visframe != r_framecount)
				continue;
			if (!(surf->flags & SURF_UNDERWATER) && ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK)))
				continue;
			if (!mirror || surf->texinfo->texture != cl.worldmodel->textures[mirrortexturenum])
			{
				surf->texturechain = surf->texinfo->texture->texturechain;
				surf->texinfo->texture->texturechain = surf;
			}
		}
	}

	R_RecursiveWorldNode (node->children[!side]);
}

/* ------------------------------------------------------------------ */
/* Static world VBO — upload all world BSP surfaces at map load time   */
/* ------------------------------------------------------------------ */

/* Vertex: pos(3) + texcoord(2) + lmcoord(2) = 7 floats */
typedef struct {
	float	pos[3];
	float	st[2];		/* diffuse texture coords */
	float	lm[2];		/* lightmap atlas coords */
} worldvert_t;

void R_BuildWorldVBO (void)
{
	qmodel_t	*m = cl.worldmodel;
	msurface_t	*surf;
	glpoly_t	*p;
	worldvert_t	*verts;
	unsigned int	*indices;
	int		i, j, v_pos, idx_pos;
	int		total_verts = 0, total_tris = 0;

	if (!m)
		return;

	/* Free old VBO if reloading */
	if (world_vbo) { glDeleteBuffers_fp(1, &world_vbo); world_vbo = 0; }
	if (world_ibo) { glDeleteBuffers_fp(1, &world_ibo); world_ibo = 0; }
	if (world_vao) { glDeleteVertexArrays_fp(1, &world_vao); world_vao = 0; }

	/* Pass 1: count vertices and triangles */
	for (i = 0; i < m->numsurfaces; i++)
	{
		surf = &m->surfaces[i];
		p = surf->polys;
		if (!p || p->numverts < 3)
			continue;
		/* Skip sky and warped surfaces — they have special rendering */
		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;
		total_verts += p->numverts;
		total_tris += p->numverts - 2;
	}

	if (total_verts == 0)
		return;

	verts = (worldvert_t *) malloc(total_verts * sizeof(worldvert_t));
	indices = (unsigned int *) malloc(total_tris * 3 * sizeof(unsigned int));
	if (!verts || !indices)
	{
		free(verts);
		free(indices);
		return;
	}

	/* Pass 2: fill vertex and index data */
	v_pos = 0;
	idx_pos = 0;
	for (i = 0; i < m->numsurfaces; i++)
	{
		surf = &m->surfaces[i];
		p = surf->polys;
		if (!p || p->numverts < 3)
			continue;
		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;

		surf->vbo_firstvert = v_pos;
		surf->vbo_firstindex = idx_pos;
		surf->vbo_numtris = p->numverts - 2;

		/* Emit vertices */
		for (j = 0; j < p->numverts; j++)
		{
			float *v = p->verts[j];
			verts[v_pos].pos[0] = v[0];
			verts[v_pos].pos[1] = v[1];
			verts[v_pos].pos[2] = v[2];
			verts[v_pos].st[0] = v[3];
			verts[v_pos].st[1] = v[4];
			verts[v_pos].lm[0] = v[5];
			verts[v_pos].lm[1] = v[6];
			v_pos++;
		}

		/* Emit triangle fan indices */
		for (j = 2; j < p->numverts; j++)
		{
			indices[idx_pos++] = surf->vbo_firstvert;
			indices[idx_pos++] = surf->vbo_firstvert + j - 1;
			indices[idx_pos++] = surf->vbo_firstvert + j;
		}
	}

	/* Create GPU objects */
	glGenVertexArrays_fp(1, &world_vao);
	glBindVertexArray_fp(world_vao);

	glGenBuffers_fp(1, &world_vbo);
	glBindBuffer_fp(GL_ARRAY_BUFFER, world_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, v_pos * sizeof(worldvert_t),
			verts, GL_STATIC_DRAW);

	/* pos = location 0 */
	glEnableVertexAttribArray_fp(ATTR_POSITION);
	glVertexAttribPointer_fp(ATTR_POSITION, 3, GL_FLOAT, GL_FALSE,
				 sizeof(worldvert_t), (void *)0);
	/* texcoord = location 1 */
	glEnableVertexAttribArray_fp(ATTR_TEXCOORD);
	glVertexAttribPointer_fp(ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE,
				 sizeof(worldvert_t), (void *)(3 * sizeof(float)));
	/* lmcoord = location 2 */
	glEnableVertexAttribArray_fp(ATTR_LMCOORD);
	glVertexAttribPointer_fp(ATTR_LMCOORD, 2, GL_FLOAT, GL_FALSE,
				 sizeof(worldvert_t), (void *)(5 * sizeof(float)));

	glGenBuffers_fp(1, &world_ibo);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, world_ibo);
	glBufferData_fp(GL_ELEMENT_ARRAY_BUFFER, idx_pos * sizeof(unsigned int),
			indices, GL_STATIC_DRAW);

	glBindVertexArray_fp(0);

	world_num_verts = v_pos;
	world_num_indices = idx_pos;

	free(verts);
	free(indices);

	Con_SafePrintf("World VBO: %d verts, %d tris in static buffer\n",
		       world_num_verts, world_num_indices / 3);
}

void R_FreeWorldVBO (void)
{
	if (world_vbo) { glDeleteBuffers_fp(1, &world_vbo); world_vbo = 0; }
	if (world_ibo) { glDeleteBuffers_fp(1, &world_ibo); world_ibo = 0; }
	if (world_vao) { glDeleteVertexArrays_fp(1, &world_vao); world_vao = 0; }
	world_num_verts = 0;
	world_num_indices = 0;
}

/*
=============
R_DrawWorld
=============
*/
void R_DrawWorld (void)
{
	VectorCopy (r_refdef.vieworg, modelorg);

	currenttexture = GL_UNUSED_TEXTURE;

	GL_ImmColor4f (1.0f,1.0f,1.0f,1.0f);
	memset (lightmap_polys, 0, sizeof(lightmap_polys));

	// Reset sky bounds for skybox rendering
	if (skybox_name[0])
	{
		extern float skymins[2][6], skymaxs[2][6];
		int i;
		for (i = 0; i < 6; i++)
		{
			skymins[0][i] = skymins[1][i] = 9999;
			skymaxs[0][i] = skymaxs[1][i] = -9999;
		}
	}

#ifdef QUAKE2
	R_ClearSkyBox ();
#endif

	// Reset sky bounds for new frame
	{
		int i;
		extern float skymins[2][6], skymaxs[2][6];
		for (i = 0; i < 6; i++)
		{
			skymins[0][i] = skymins[1][i] = 9999;
			skymaxs[0][i] = skymaxs[1][i] = -9999;
		}
	}

	R_RecursiveWorldNode (cl.worldmodel->nodes);

	DrawTextureChains (&r_worldentity);

	// reset to texture unit 0
	glActiveTextureARB_fp (GL_TEXTURE1_ARB);
	glActiveTextureARB_fp (GL_TEXTURE0_ARB);

	R_UpdateLightmaps (false);

#ifdef QUAKE2
	R_DrawSkyBox ();
#endif
}


/*
=============================================================================

LIGHTMAP ALLOCATION

=============================================================================
*/

// returns a texture number and the position inside it
static unsigned int AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	unsigned int	texnum;

	for (texnum = 0; texnum < MAX_LIGHTMAPS; texnum++)
	{
		best = BLOCK_HEIGHT;

		for (i = 0; i < BLOCK_WIDTH - w; i++)
		{
			best2 = 0;

			for (j = 0; j < w; j++)
			{
				if (allocated[texnum][i+j] >= best)
					break;
				if (allocated[texnum][i+j] > best2)
					best2 = allocated[texnum][i+j];
			}
			if (j == w)
			{	// this is a valid spot
				*x = i;
				*y = best = best2;
			}
		}

		if (best + h > BLOCK_HEIGHT)
			continue;

		for (i = 0; i < w; i++)
			allocated[texnum][*x + i] = best + h;

		return texnum;
	}

	Sys_Error ("%s: full", __thisfunc__);
	return -1;	// shut up the compiler
}


#define	COLINEAR_EPSILON	0.001
static mvertex_t	*r_pcurrentvertbase;
static qmodel_t		*currentmodel;

/*
================
BuildSurfaceDisplayList
================
*/
static void BuildSurfaceDisplayList (msurface_t *fa)
{
	int		i, lindex, lnumverts;
	medge_t		*pedges, *r_pedge;
	float		*vec;
	float		s, t;
	glpoly_t	*poly;

// reconstruct the polygon
	pedges = currentmodel->edges;
	lnumverts = fa->numedges;

	//
	// draw texture
	//
	poly = (glpoly_t *) Hunk_AllocName (sizeof(glpoly_t) + (lnumverts-4) * VERTEXSIZE*sizeof(float), "poly");
	poly->next = fa->polys;
	poly->flags = fa->flags;
	fa->polys = poly;
	poly->numverts = lnumverts;

	for (i = 0; i < lnumverts; i++)
	{
		lindex = currentmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = r_pcurrentvertbase[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = r_pcurrentvertbase[r_pedge->v[1]].position;
		}
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s /= fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t /= fa->texinfo->texture->height;

		VectorCopy (vec, poly->verts[i]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;

		//
		// lightmap texture coordinates — mapped into atlas
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s*16;
		s += 8;
		s /= BLOCK_WIDTH*16;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t*16;
		t += 8;
		t /= BLOCK_HEIGHT*16;

		/* Remap page-local UV to atlas position (if atlas enabled) */
		if (lm_atlas_enabled)
		{
			int col = fa->lightmaptexturenum % LM_ATLAS_COLS;
			int row = fa->lightmaptexturenum / LM_ATLAS_COLS;
			s = (col + s) / (float)LM_ATLAS_COLS;
			t = (row + t) / (float)LM_ATLAS_ROWS;
		}

		poly->verts[i][5] = s;
		poly->verts[i][6] = t;
	}

	//
	// remove co-linear points - Ed
	//
	if (!gl_keeptjunctions.integer && !(fa->flags & SURF_UNDERWATER))
	{
		for (i = 0; i < lnumverts; ++i)
		{
			vec3_t	v1, v2;
			float	*prev, *curr, *next;

			prev = poly->verts[(i + lnumverts - 1) % lnumverts];
			curr = poly->verts[i];
			next = poly->verts[(i + 1) % lnumverts];

			VectorSubtract(curr, prev, v1);
			VectorNormalize(v1);
			VectorSubtract(next, prev, v2);
			VectorNormalize(v2);

			// skip co-linear points
			if ((fabs(v1[0] - v2[0]) <= COLINEAR_EPSILON) &&
			    (fabs(v1[1] - v2[1]) <= COLINEAR_EPSILON) &&
			    (fabs(v1[2] - v2[2]) <= COLINEAR_EPSILON))
			{
				int		j, k;
				for (j = i + 1; j < lnumverts; ++j)
				{
					for (k = 0; k < VERTEXSIZE; ++k)
						poly->verts[j - 1][k] = poly->verts[j][k];
				}
				--lnumverts;
				// retry next vertex next time, which is now current vertex
				--i;
			}
		}
	}

	poly->numverts = lnumverts;
}

/*
========================
GL_CreateSurfaceLightmap
========================
*/
static void GL_CreateSurfaceLightmap (msurface_t *surf)
{
	int	smax, tmax;
	byte	*base;

	if (surf->flags & (SURF_DRAWSKY|SURF_DRAWTURB))
		return;

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;

	surf->lightmaptexturenum = AllocBlock (smax, tmax, &surf->light_s, &surf->light_t);
	base = lightmaps + surf->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
	base += (surf->light_t * BLOCK_WIDTH + surf->light_s) * lightmap_bytes;
	R_BuildLightMap (surf, base, BLOCK_WIDTH*lightmap_bytes);
}


/*
==================
GL_BuildLightmaps

Builds the lightmap texture
with all the surfaces from all brush models
==================
*/
void GL_BuildLightmaps (void)
{
	int		i, j;
	qmodel_t	*m;

	memset (allocated, 0, sizeof(allocated));
	memset (lightmap_modified, 0, sizeof(lightmap_modified));
	memset (lightmap_polys, 0, sizeof(lightmap_polys));

	r_framecount = 1;		// no dlightcache

	if (! lightmap_textures[0])
	{
		glGenTextures_fp(MAX_LIGHTMAPS, lightmap_textures);
	}

	/* Decide atlas mode BEFORE building surfaces (UVs depend on it) */
	lm_atlas_enabled = (Cvar_VariableValue("gl_lmatlas") != 0);

	for (j = 1; j < MAX_MODELS; j++)
	{
		m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;

		r_pcurrentvertbase = m->vertexes;
		currentmodel = m;

		for (i = 0; i < m->numsurfaces; i++)
		{
			GL_CreateSurfaceLightmap (m->surfaces + i);
			if (m->surfaces[i].flags & SURF_DRAWTURB)
				continue;
#ifndef QUAKE2
			if (m->surfaces[i].flags & SURF_DRAWSKY)
				continue;
#endif
			if (!draw_reinit)
				BuildSurfaceDisplayList (m->surfaces + i);
		}
	}

	glActiveTextureARB_fp (GL_TEXTURE1_ARB);

	//
	// upload all lightmaps that were filled
	//
	lm_atlas_layers = 0;
	for (i = 0; i < MAX_LIGHTMAPS; i++)
	{
		if (!allocated[i][0])
			break;		// no more used
		lightmap_modified[i] = false;
		lm_atlas_layers++;

		GL_Bind(lightmap_textures[i]);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D_fp (GL_TEXTURE_2D, 0, lightmap_internalformat, BLOCK_WIDTH,
				BLOCK_HEIGHT, 0, gl_lightmap_format, GL_UNSIGNED_BYTE,
				lightmaps + i*BLOCK_WIDTH*BLOCK_HEIGHT*lightmap_bytes);
	}

	/* Build lightmap atlas — all pages in one 2D texture.
	 * Surfaces already have atlas-remapped UVs from BuildSurfaceDisplayList.
	 * One bind for ALL world surfaces, zero lightmap rebinds.
	 * Disabled on Intel GPUs due to driver timeout issues. */
	{
		byte *atlas = NULL;
		int page, row, col, y;
		int page_stride = BLOCK_WIDTH * lightmap_bytes;
		int atlas_stride = LM_ATLAS_WIDTH * lightmap_bytes;

		if (lm_atlas_enabled)
			atlas = (byte *) calloc(1, LM_ATLAS_WIDTH * LM_ATLAS_HEIGHT * lightmap_bytes);
		if (atlas)
		{
			for (page = 0; page < lm_atlas_layers; page++)
			{
				col = page % LM_ATLAS_COLS;
				row = page / LM_ATLAS_COLS;
				for (y = 0; y < BLOCK_HEIGHT; y++)
				{
					byte *src = lightmaps + page * BLOCK_WIDTH * BLOCK_HEIGHT * lightmap_bytes
							     + y * page_stride;
					byte *dst = atlas + (row * BLOCK_HEIGHT + y) * atlas_stride
							  + col * page_stride;
					memcpy(dst, src, page_stride);
				}
			}

			if (!lm_atlas_texture)
				glGenTextures_fp(1, &lm_atlas_texture);
			glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D_fp(GL_TEXTURE_2D, 0, lightmap_internalformat,
					LM_ATLAS_WIDTH, LM_ATLAS_HEIGHT, 0,
					gl_lightmap_format, GL_UNSIGNED_BYTE, atlas);
			free(atlas);

			Con_SafePrintf("Lightmap atlas: %d pages in %dx%d texture\n",
				       lm_atlas_layers, LM_ATLAS_WIDTH, LM_ATLAS_HEIGHT);
		}
	}

	glActiveTextureARB_fp (GL_TEXTURE0_ARB);
}

