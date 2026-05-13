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

/* ====================================================================
 * BINDLESS TEXTURE INFRASTRUCTURE (GL_ARB_bindless_texture)
 *
 * When gl_bindless_able is true, texture sampling is handle-indexed from
 * an SSBO instead of per-draw glBindTexture calls.  This union defines the
 * two paths:
 *
 * Bindless path (BINDLESS=1 in shaders):
 *  - Populate SSBO with uvec2 txhandle + fbhandle (64-bit each)
 *  - Vertex shader passes flat uvec4 to fragment
 *  - Fragment shader casts to sampler2D(uvec2)
 *  - No glBindTexture calls
 *
 * Fallback path (BINDLESS=0 in shaders):
 *  - Traditional glBindTexture before each draw
 *  - baseinstance uniform indexes the call SSBO
 *  - Sampler uniforms u_texture0, u_texture2 used normally
 *
 * Implementation note: full bindless integration requires refactoring the
 * DrawTextureChains submission logic to populate handles before drawing.
 * Current code (as of ctrx.4 partial) establishes the infrastructure;
 * full draw-path migration is a follow-up task.
 * ==================================================================== */

typedef struct {
	GLuint   flags;
	GLfloat  alpha;
	GLuint64 tx_handle;   /* diffuse texture bindless handle */
	GLuint64 fb_handle;   /* fullbright texture bindless handle */
} gl_bindless_call_t;

typedef struct {
	GLuint  flags;
	GLuint  baseinstance;
	GLuint  _pad0;
	GLuint  _pad1;
} gl_bound_call_t;

typedef union {
	gl_bindless_call_t bindless;
	gl_bound_call_t    bound;
} gl_draw_call_t;


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

/* Bindless texture SSBO for draw call data */
#define MAX_SURFACE_DRAW_CALLS 16384
static GLuint		drawcall_ssbo = 0;
static gl_draw_call_t	*drawcall_cpu_buffer = NULL;

static unsigned int	blocklights[18*18];
static unsigned int	blocklightscolor[18*18*3];	// colored light support. *3 for RGB to the definitions at the top

#define	BLOCK_WIDTH	128
#define	BLOCK_HEIGHT	128

static glpoly_t	*lightmap_polys[MAX_LIGHTMAPS];
static qboolean	lightmap_modified[MAX_LIGHTMAPS];

/* Dirty rect per lightmap page for sub-image uploads */
static int	lightmap_rectmin[MAX_LIGHTMAPS][2]; /* x, y */
static int	lightmap_rectmax[MAX_LIGHTMAPS][2]; /* x, y */

static void LM_ExpandDirtyRect (int page, int x, int y, int w, int h)
{
	if (!lightmap_modified[page])
	{
		lightmap_modified[page] = true;
		lightmap_rectmin[page][0] = x;
		lightmap_rectmin[page][1] = y;
		lightmap_rectmax[page][0] = x + w;
		lightmap_rectmax[page][1] = y + h;
	}
	else
	{
		if (x < lightmap_rectmin[page][0])
			lightmap_rectmin[page][0] = x;
		if (y < lightmap_rectmin[page][1])
			lightmap_rectmin[page][1] = y;
		if (x + w > lightmap_rectmax[page][0])
			lightmap_rectmax[page][0] = x + w;
		if (y + h > lightmap_rectmax[page][1])
			lightmap_rectmax[page][1] = y + h;
	}
}

static int	allocated[MAX_LIGHTMAPS][BLOCK_WIDTH];

// the lightmap texture data needs to be kept in
// main memory so texsubimage can update properly
static byte	lightmaps[4*MAX_LIGHTMAPS*BLOCK_WIDTH*BLOCK_HEIGHT];

/* Lightmap atlas — all pages in one 2D texture (16x16 grid of 128x128 pages = 2048x2048) */
#define LM_ATLAS_COLS	16
#define LM_ATLAS_ROWS	16
#define LM_ATLAS_WIDTH	(LM_ATLAS_COLS * BLOCK_WIDTH)	/* 2048 */
#define LM_ATLAS_HEIGHT	(LM_ATLAS_ROWS * BLOCK_HEIGHT)	/* 2048 */
GLuint	lm_atlas_texture;	/* non-static — accessed by gl_worldcull.c */
static int	lm_atlas_layers;	/* number of pages in the atlas */
qboolean	lm_atlas_enabled;	/* false = fall back to per-surface binds */

/* ====================================================================
 * BINDLESS TEXTURE BINDING & SSBO MANAGEMENT
 * ==================================================================== */

static inline void GL_BindDiffuse (GLuint texnum)
{
	if (!gl_bindless_able)
	{
		glActiveTexture_fp(GL_TEXTURE0);
		glBindTexture_fp(GL_TEXTURE_2D, texnum);
	}
}

static inline void GL_BindFullbright (GLuint texnum)
{
	if (!gl_bindless_able)
	{
		glActiveTexture_fp(GL_TEXTURE2);
		glBindTexture_fp(GL_TEXTURE_2D, texnum);
		glActiveTexture_fp(GL_TEXTURE0);
	}
}

static void GL_CreateDrawCallSSBO (void)
{
	size_t ssbo_size;

	if (!gl_bindless_able)
		return;

	ssbo_size = MAX_SURFACE_DRAW_CALLS * sizeof(gl_draw_call_t);
	drawcall_cpu_buffer = (gl_draw_call_t *)malloc(ssbo_size);
	if (!drawcall_cpu_buffer)
	{
		Con_Printf("GL_CreateDrawCallSSBO: failed to allocate CPU buffer\n");
		return;
	}

	glGenBuffers_fp(1, &drawcall_ssbo);
	glBindBuffer_fp(GL_COPY_WRITE_BUFFER, drawcall_ssbo);
	glBufferData_fp(GL_COPY_WRITE_BUFFER, ssbo_size, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer_fp(GL_COPY_WRITE_BUFFER, 0);

	Con_DPrintf("Created bindless draw-call SSBO (%zu bytes, %d calls)\n",
		    ssbo_size, MAX_SURFACE_DRAW_CALLS);
}

static void GL_DeleteDrawCallSSBO (void)
{
	if (drawcall_ssbo)
	{
		glDeleteBuffers_fp(1, &drawcall_ssbo);
		drawcall_ssbo = 0;
	}
	if (drawcall_cpu_buffer)
	{
		free(drawcall_cpu_buffer);
		drawcall_cpu_buffer = NULL;
	}
}

static void GL_UploadDrawCallSSBO (int num_calls)
{
	if (!gl_bindless_able || !drawcall_ssbo || !drawcall_cpu_buffer)
		return;

	if (num_calls <= 0 || num_calls > MAX_SURFACE_DRAW_CALLS)
		return;

	glBindBuffer_fp(GL_COPY_WRITE_BUFFER, drawcall_ssbo);
	glBufferSubData_fp(GL_COPY_WRITE_BUFFER, 0,
			   num_calls * sizeof(gl_draw_call_t),
			   drawcall_cpu_buffer);
	glBindBuffer_fp(GL_COPY_WRITE_BUFFER, 0);
}


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
texture_t *R_TextureAnimation (entity_t *e, texture_t *base)
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
	GL_ImmEnd (GL_TRIANGLE_FAN, OIT_InPass() ? &gl_shader_alias_oit : &gl_shader_alias);
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
	GL_ImmEnd (GL_TRIANGLE_FAN, OIT_InPass() ? &gl_shader_world_oit : &gl_shader_world);
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
	GL_ImmEnd (GL_POLYGON, OIT_InPass() ? &gl_shader_alias_oit : &gl_shader_alias);
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
	GL_ImmEnd (GL_POLYGON, OIT_InPass() ? &gl_shader_world_oit : &gl_shader_world);
}



/* Public so R_DrawBrushInstanced can flush dirty rects produced by
 * R_CollectBrushInstances *this frame* before the MDI dispatch — without
 * a second upload, brush-ent surfaces sample the previous frame's atlas
 * and, on rotating ents (drawbridge / pendulum), shimmer. */
void R_UpdateLightmaps (qboolean Translucent)
{
	unsigned int		i;
	glpoly_t	*p;

	if (r_fullbright.integer)
		return;

	glActiveTexture_fp (GL_TEXTURE1);

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
			int rx = lightmap_rectmin[i][0];
			int ry = lightmap_rectmin[i][1];
			int rw = lightmap_rectmax[i][0] - rx;
			int rh = lightmap_rectmax[i][1] - ry;
			byte *src;

			/* Clamp to page bounds */
			if (rx < 0) rx = 0;
			if (ry < 0) ry = 0;
			if (rx + rw > BLOCK_WIDTH)  rw = BLOCK_WIDTH - rx;
			if (ry + rh > BLOCK_HEIGHT) rh = BLOCK_HEIGHT - ry;

			lightmap_modified[i] = false;

			src = lightmaps + i*BLOCK_WIDTH*BLOCK_HEIGHT*lightmap_bytes
			    + ry * BLOCK_WIDTH * lightmap_bytes
			    + rx * lightmap_bytes;

			if (lm_atlas_enabled)
			{
				/* Atlas mode: upload only to atlas, skip per-page texture */
				if (lm_atlas_texture)
				{
					int col = i % LM_ATLAS_COLS;
					int row = i / LM_ATLAS_COLS;
					glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
					glPixelStorei_fp(GL_UNPACK_ROW_LENGTH, BLOCK_WIDTH);
					glTexSubImage2D_fp(GL_TEXTURE_2D, 0,
							col * BLOCK_WIDTH + rx,
							row * BLOCK_HEIGHT + ry,
							rw, rh,
							gl_lightmap_format, GL_UNSIGNED_BYTE, src);
					glPixelStorei_fp(GL_UNPACK_ROW_LENGTH, 0);
					currenttexture = GL_UNUSED_TEXTURE;
				}
			}
			else
			{
				/* Per-page mode: upload to individual page texture */
				GL_Bind(lightmap_textures[i]);
				glPixelStorei_fp(GL_UNPACK_ROW_LENGTH, BLOCK_WIDTH);
				glTexSubImage2D_fp(GL_TEXTURE_2D, 0,
						rx, ry, rw, rh,
						gl_lightmap_format, GL_UNSIGNED_BYTE, src);
				glPixelStorei_fp(GL_UNPACK_ROW_LENGTH, 0);
			}
		}
	}

	glActiveTexture_fp (GL_TEXTURE0);
}


/*
================
R_RenderBrushPoly
================
*/
static float R_LiquidAlpha (const texture_t *t); /* forward decl */

void R_RenderBrushPoly (entity_t *e, msurface_t *fa, qboolean override)
{
	texture_t	*t;
	byte		*base;
	int		maps;
	float		intensity, alpha_val;

	c_brush_polys++;

	glActiveTexture_fp(GL_TEXTURE0);

	intensity = 1.0f;
	alpha_val = 1.0f;

	if (e->drawflags & DRF_TRANSLUCENT)
	{
		glEnable_fp (GL_BLEND);
		/* Translucent surfaces must not write depth — otherwise the
		 * alpha-blended brush ent occludes anything drawn after it
		 * (e.g. world lava in the R_DrawWaterSurfaces pass behind a
		 * func_illusionary).  uhexen2-t4kt.  Blend func gated for OIT
		 * pass. */
		if (!OIT_InPass())
			glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask_fp(0);
		alpha_val = r_wateralpha.value;
	}
	{
		int mls = e->drawflags & MLS_MASKIN;
		if (mls == MLS_ABSLIGHT)
		{
			// ent->abslight   0 - 255
			intensity = (float)e->abslight / 255.0f;
		}
		else if (mls != MLS_NONE)
		{
			/* MLS_FULLBRIGHT (1) / POWERMODE (2) / TORCH (3) /
			 * TOTALDARK (4): same dispatch as R_DrawAliasModel
			 * (gl_rmain.c:919-947) — sample the corresponding
			 * pre-baked lightstyle slot.  Without this brush ents
			 * with MLS_TORCH wouldn't flicker, MLS_FULLBRIGHT
			 * surfaces rendered as ordinary lightmap-lit, etc.
			 * uhexen2-j7rp. */
			intensity = (float)d_lightstylevalue[24 + mls] / 255.0f;
		}
	}

	GL_ImmColor4f(intensity, intensity, intensity, alpha_val);

	if (fa->flags & SURF_DRAWSKY)
	{	// warp texture, no lightmaps
		EmitBothSkyLayers (fa);
		/* Restore the DRF_TRANSLUCENT prelude state — the cleanup at
		 * the bottom of this function is bypassed by this early return
		 * and a leaked DepthMask=0 / BLEND-on would corrupt the next
		 * draw call.  uhexen2-j001.  Gated for OIT pass. */
		if ((e->drawflags & DRF_TRANSLUCENT) && !OIT_InPass())
		{
			glDepthMask_fp(1);
			glDisable_fp(GL_BLEND);
		}
		return;
	}

	t = R_TextureAnimation (e, fa->texinfo->texture);
	GL_Bind (t->gl_texturenum);
	/* Bind per-miptex fullbright mask at TU2.  R_RenderBrushPoly runs
	 * through gl_shader_world, so without this brush-ent surfaces inherit
	 * whatever TU2 the previous caller left (DrawTextureChains' teardown
	 * resets to null_fb, so brush ents otherwise render dim — missing the
	 * additive contribution that world surfaces of the same miptex get).
	 * Harmless on the turb early-return below (EmitWaterPolys uses
	 * gl_shader_alias, which doesn't sample u_texture2).  uhexen2-61bb. */
	glActiveTexture_fp(GL_TEXTURE2);
	glBindTexture_fp(GL_TEXTURE_2D,
		t->gl_fb_texturenum ? t->gl_fb_texturenum : gl_null_fb_texture);
	glActiveTexture_fp(GL_TEXTURE0);

	if (fa->flags & SURF_DRAWTURB)
	{	// warp texture — apply per-liquid alpha + light tinting
		float turb_alpha = R_LiquidAlpha(fa->texinfo->texture);
		qboolean turb_blend = (turb_alpha < 1.0f);
		/* R_LiquidAlpha is authoritative for turb surfaces.  The
		 * DRF_TRANSLUCENT block above set alpha_val = r_wateralpha and
		 * enabled BLEND assuming a regular surface; for a turb surface
		 * (e.g. func_illusionary over a lava pit) we override with the
		 * per-liquid alpha so default lava stays opaque even though
		 * the brush ent is flagged translucent.  uhexen2-gbmv,
		 * continuation of 479f1304f. */
		if (turb_blend)
		{
			glEnable_fp (GL_BLEND);
			if (!OIT_InPass())
				glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask_fp(0);
		}
		else if (!OIT_InPass())
		{
			/* Opaque turb (default lava) writes depth normally even
			 * on a DRF_TRANSLUCENT brush — the prelude set DepthMask
			 * to 0 assuming a translucent surface, but the per-liquid
			 * alpha override means this surface is actually opaque
			 * and must occlude geometry behind it.  uhexen2-j001.
			 * Gated for OIT pass. */
			glDisable_fp (GL_BLEND);
			glDepthMask_fp(1);
		}
		alpha_val = turb_alpha;
		/* Self-emissive turb textures (lava, teleporters, custom mod
		 * liquids) ignore the surrounding lightmap — they are their
		 * own light source.  Without this a func_illusionary lava
		 * brush in a dim pit gets multiplied by ambient lightmap
		 * brightness and reads as a brown smear instead of bright
		 * orange.  World turbs (drawn via R_DrawWaterSurfaces) already
		 * use (1,1,1,1) unconditionally — this restores parity for
		 * the brush-ent path.
		 *
		 * Original 086x scope was *lava/*tele only; uhexen2-6697
		 * widens to "anything starting with *, except *water*" so SoT
		 * mod content (*magic*, *lightnings, *skulls, *lowlight*, plus
		 * any other custom turb name) gets the same treatment without
		 * a per-name allowlist.  *water keeps the existing dim
		 * behavior since real water IS lit by its surroundings. */
		{
			texture_t *_t = fa->texinfo->texture;
			qboolean is_water =
				(_t->name[0] == '*' &&
				 !q_strncasecmp(_t->name + 1, "water", 5));
			qboolean self_emissive =
				(_t->name[0] == '*' && !is_water);

			if (fa->polys && !r_fullbright.integer &&
			    intensity >= 1.0f && !self_emissive)
			{
				extern vec3_t lightcolor;
				vec3_t saved_lc;
				float *v = fa->polys->verts[0];
				vec3_t mid;
				float lv;
				VectorCopy(lightcolor, saved_lc);
				mid[0] = v[0]; mid[1] = v[1]; mid[2] = v[2];
				R_LightPointColor(mid);
				lv = (lightcolor[0] + lightcolor[1] + lightcolor[2]) / (3.0f * 200.0f);
				if (lv > 1.0f) lv = 1.0f;
				if (lv < 0.15f) lv = 0.15f;
				GL_ImmColor4f(lv, lv, lv, alpha_val);
				VectorCopy(saved_lc, lightcolor);
			}
			else
				GL_ImmColor4f(intensity, intensity, intensity, alpha_val);
		}
		EmitWaterPolys (fa);
		/* Restore default state on every return path — the cleanup at
		 * the bottom of this function is bypassed by this early return,
		 * so any DepthMask=0 / BLEND-on left by the prelude or the
		 * turb_blend branch above would leak into the next draw.
		 * uhexen2-j001.  Gated for OIT pass. */
		if (!OIT_InPass())
		{
			glDepthMask_fp(1);
			glDisable_fp(GL_BLEND);
		}
		return;
	}

	if (fa->flags & SURF_DRAWFENCE)
	{
		/* Alpha-tested cutout — surviving pixels are fully opaque, so
		 * write depth normally even when the entity is DRF_TRANSLUCENT
		 * (which set DepthMask=0 above).  uhexen2-t4kt.  Gated for OIT
		 * pass. */
		if (!OIT_InPass())
		{
			glDisable_fp(GL_BLEND);
			glDepthMask_fp(1);
		}
		GL_SetAlphaThreshold(0.666f);
		if (r_alphatocoverage.integer)
			glEnable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	}

	/* MLS_ABSLIGHT skip restored from pre-90265f406. Brush entities with
	 * an absolute-light value (typically have no baked lightmap samples)
	 * must use the single-pass path: lightmap_atlas × abslight would be
	 * 0 × abslight = 0 (black) for any surface without samples, which is
	 * what bit pillars on romeric6 and similar maps. Vanilla Hexen II
	 * behavior — uniform abslight intensity, no per-texel variation.
	 * uhexen2-mxkm.
	 *
	 * Extended to any MLS_MASKIN != MLS_NONE (FULLBRIGHT / POWERMODE /
	 * TORCH / TOTALDARK) so the intensity computed above is what the
	 * surface actually renders — going through the MTex lightmap path
	 * would multiply intensity by the baked lightmap and the effect
	 * would be lost.  uhexen2-j7rp. */
	if ((e->drawflags & DRF_TRANSLUCENT) ||
	    (e->drawflags & MLS_MASKIN) != MLS_NONE)
	{
		if (fa->flags & SURF_UNDERWATER)
			DrawGLWaterPoly (fa->polys);
		else
			DrawGLPoly (fa->polys);
	}
	else
	{
		glActiveTexture_fp(GL_TEXTURE1);

		if (lm_atlas_texture)
			glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
		else
			GL_Bind (lightmap_textures[fa->lightmaptexturenum]);

		if (fa->flags & SURF_UNDERWATER)
			DrawGLWaterPolyMTexLM (fa->polys);
		else
			DrawGLPolyMTex (fa->polys);

		glActiveTexture_fp(GL_TEXTURE0);
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
		LM_ExpandDirtyRect(fa->lightmaptexturenum, fa->light_s, fa->light_t,
				   (fa->extents[0] >> 4) + 1, (fa->extents[1] >> 4) + 1);
		base = lightmaps + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
		base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
		R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
	}

	if (e->drawflags & DRF_TRANSLUCENT)
	{
		/* gated for OIT pass */
		if (!OIT_InPass())
		{
			glDepthMask_fp(1);
			glDisable_fp (GL_BLEND);
		}
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

	/* DRF_TRANSLUCENT entities are routed through R_RenderBrushPoly
	 * (the non-MTex sibling) by DrawTextureChains' branch on
	 * (DRF_TRANSLUCENT || ABSLIGHT); this MTex path is the else.  Dead
	 * DRF_TRANSLUCENT prelude + cleanup blocks (which had the same
	 * depth-state leak the j001 fix repaired in R_RenderBrushPoly) were
	 * removed in uhexen2-a5es. */
	c_brush_polys++;

	glActiveTexture_fp(GL_TEXTURE0);

	intensity = 1.0f;
	alpha_val = 1.0f;

	/* KIERO: Seems it's enabled when we enter here.  Gated for OIT pass. */
	if (!OIT_InPass())
		glDisable_fp (GL_BLEND);

	if ((e->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT)
	{
		intensity = (float)e->abslight / 255.0f;
	}

	if (fa->flags & SURF_DRAWTURB)
	{
		if (!OIT_InPass())
			glDisable_fp (GL_BLEND);
		glActiveTexture_fp(GL_TEXTURE1);
		glActiveTexture_fp(GL_TEXTURE0);

		intensity = 1.0;
	}

	if (!override)
		GL_ImmColor4f(intensity, intensity, intensity, alpha_val);

	if (fa->flags & SURF_DRAWSKY)
	{	// warp texture, no lightmaps
		EmitBothSkyLayers (fa);
		return;
	}

	glActiveTexture_fp(GL_TEXTURE0);
	t = R_TextureAnimation (e, fa->texinfo->texture);
	GL_Bind (t->gl_texturenum);
	/* Bind per-miptex fullbright mask at TU2 — same rationale as the
	 * sibling in R_RenderBrushPoly.  uhexen2-61bb. */
	glActiveTexture_fp(GL_TEXTURE2);
	glBindTexture_fp(GL_TEXTURE_2D,
		t->gl_fb_texturenum ? t->gl_fb_texturenum : gl_null_fb_texture);
	glActiveTexture_fp(GL_TEXTURE0);

	if (fa->flags & SURF_DRAWFENCE)
	{
		/* uhexen2-t4kt — see R_RenderBrushPoly.  Gated for OIT pass. */
		if (!OIT_InPass())
		{
			glDisable_fp(GL_BLEND);
			glDepthMask_fp(1);
		}
		GL_SetAlphaThreshold(0.666f);
		if (r_alphatocoverage.integer)
			glEnable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	}

	if (fa->flags & SURF_DRAWTURB)
	{
		float turb_alpha = R_LiquidAlpha(fa->texinfo->texture);
		if (turb_alpha < 1.0f && alpha_val >= 1.0f)
		{
			glEnable_fp (GL_BLEND);
			if (!OIT_InPass())
				glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask_fp(0);
		}
		GL_ImmColor4f(1.0f, 1.0f, 1.0f, turb_alpha < 1.0f ? turb_alpha : alpha_val);
		EmitWaterPolys (fa);
		if (turb_alpha < 1.0f && !OIT_InPass())
		{
			/* See R_RenderBrushPoly's matching cleanup. */
			glDepthMask_fp(1);
			glDisable_fp(GL_BLEND);
		}
		//return;
	}
	else
	{
		/* MLS_ABSLIGHT skip restored from pre-90265f406 — see
		 * R_RenderBrushPoly for the full reasoning. uhexen2-mxkm. */
		if ((e->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT)
		{
			glActiveTexture_fp(GL_TEXTURE0);

			if (fa->flags & SURF_UNDERWATER)
				DrawGLWaterPoly (fa->polys);
			else
				DrawGLPoly (fa->polys);
		}
		else
		{
			glActiveTexture_fp(GL_TEXTURE1);
			if (lm_atlas_texture)
				glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
			else
				GL_Bind (lightmap_textures[fa->lightmaptexturenum]);

			if (fa->flags & SURF_UNDERWATER)
				DrawGLWaterPolyMTexLM (fa->polys);
			else
				DrawGLPolyMTex (fa->polys);
		}

		glActiveTexture_fp(GL_TEXTURE1);

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
			LM_ExpandDirtyRect(fa->lightmaptexturenum, fa->light_s, fa->light_t,
					   (fa->extents[0] >> 4) + 1, (fa->extents[1] >> 4) + 1);
			base = lightmaps + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
			base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
			R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
		}
	}

	glActiveTexture_fp(GL_TEXTURE0);

	if (fa->flags & SURF_DRAWFENCE)
	{
		GL_SetAlphaThreshold(0.01f);
		if (r_alphatocoverage.integer)
			glDisable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	}

	glActiveTexture_fp(GL_TEXTURE1);
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
 *
 * Primary signal: texture->content_class, set at BSP load from the
 * CONTENTS of the brush volume(s) that use this texture (uhexen2-8nvj).
 * Disambiguates same-named textures across maps (e.g. *lowlight =
 * translucent water in Keep.bsp vs. opaque illusionary in Arena.bsp).
 *
 * Fallback: name substring match.  Used for brush-entity submodels
 * (which don't have BSP leaf assignment, so content_class stays 0)
 * and for unclassified turb textures sitting in CONTENTS_EMPTY/SOLID
 * volumes (illusionary brushes). */
static float R_LiquidAlpha (const texture_t *t)
{
	switch (t->content_class)
	{
	case CONTENTS_WATER:
		/* Vanilla H2 hardcoded SURF_TRANSLUCENT only for *lowlight +
		 * *rtex078 (we extended to mod patterns water/ice/glass in
		 * Mod_SetDrawingFlags).  Other '*' textures sharing a water leaf
		 * — notably *rtex346 (Castle gold pool in demo1.bsp) — are meant
		 * to be opaque regardless of r_wateralpha.  Lava/slime keep cvar
		 * control because their cvars are a Hexenwail extension, not a
		 * vanilla contract.  uhexen2-ft2q. */
		if (t->name[0] == '*' && !t->translucent_turb)
			return 1.0f;
		{ float a = r_wateralpha.value;
		  if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
	case CONTENTS_LAVA:
		if (r_lavaalpha.value <= 0) return 1.0f;
		{ float a = r_lavaalpha.value;
		  if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
	case CONTENTS_SLIME:
		if (r_slimealpha.value <= 0) return 1.0f;
		{ float a = r_slimealpha.value;
		  if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
	}

	if (t->name[0] == '*')
	{
		const char *n = t->name + 1;
		/* Substring "water" / "ice" / "glass" for water-named brush-ent
		 * submodels (Mathuzzz's winter.bsp: *coldwater on a brush ent,
		 * no leaf assignment, content_class stays 0). */
		if (strstr(n, "water") || strstr(n, "ice") ||
		    strstr(n, "glass"))
		{	float a = r_wateralpha.value;
			if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
		if (!q_strncasecmp(n, "lava", 4))
		{	if (r_lavaalpha.value <= 0) return 1.0f;
			float a = r_lavaalpha.value; if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
		if (!q_strncasecmp(n, "slime", 5))
		{	if (r_slimealpha.value <= 0) return 1.0f;
			float a = r_slimealpha.value; if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
		if (!q_strncasecmp(n, "tele", 4))
		{	if (r_telealpha.value <= 0) return 0.7f;
			float a = r_telealpha.value; if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a; }
	}
	/* Truly unclassified — illusionary turb brush in solid/empty space,
	 * non-prefixed name, no liquid CONTENTS.  r_turbalpha controls.
	 * Default 1.0 = opaque (preserves uhexen2-6697 Arena fix). */
	{
		float a = r_turbalpha.value;
		if (a < 0.1f) a = 0.1f; if (a > 1.0f) a = 1.0f; return a;
	}
}

void R_DrawWaterSurfaces (int phase)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;

	if (r_wateralpha.value < 0.1)
		r_wateralpha.value = 0.1;
	if (r_wateralpha.value > 1)
		r_wateralpha.value = 1;
	/* The previous early-return ("all liquid alphas == 1, drawn in
	 * main pass") was wrong — DrawTextureChains' fast path skips
	 * SURF_DRAWTURB and nulls the chain, so opaque turb (default
	 * lava) was never drawn anywhere.  Always run; the per-texture
	 * branch below handles both opaque and translucent liquids. */

	//
	// go back to the world matrix
	//
	GL_LoadMatrixf (r_world_matrix);

	GL_SetAlphaThreshold(0.0f);	/* don't discard translucent water */

	for (i = 0; i < cl.worldmodel->numtextures; i++)
	{
		float		a;
		qboolean	is_opaque;

		t = cl.worldmodel->textures[i];
		if (!t)
			continue;
		s = t->texturechain;
		if (!s)
			continue;
		if (!(s->flags & SURF_DRAWTURB))
			continue;

		a = R_LiquidAlpha(t);
		is_opaque = (a >= 1.0f);

		/* Phase gate: opaque liquids draw BEFORE OIT_BeginTranslucency
		 * (writes depth, no blend, scene FBO).  Translucent liquids
		 * draw INSIDE the OIT pass.  WATER_PHASE_ALL keeps the legacy
		 * "do both" behavior for the mirror code path. */
		if (phase == WATER_PHASE_OPAQUE && !is_opaque)
			continue;
		if (phase == WATER_PHASE_TRANSLUCENT && is_opaque)
			continue;

		if (is_opaque)
		{
			/* opaque liquid (e.g. lava): draw without blend */
			glDisable_fp (GL_BLEND);
			glDepthMask_fp(1);
			GL_ImmColor4f (1,1,1,1);
		}
		else
		{
			/* translucent liquid: draw with blend, no depth write.
			 * Blend func gated for OIT pass. */
			glEnable_fp (GL_BLEND);
			if (!OIT_InPass())
				glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask_fp(0);
			GL_ImmColor4f (1,1,1, a);
		}

		// set modulate mode explicitly
		GL_Bind (t->gl_texturenum);

		for ( ; s ; s = s->texturechain)
			EmitWaterPolys (s);

		/* Only NULL the chain for textures we actually drained — the
		 * phase-gated skips above leave their chains intact for the
		 * companion phase to process. */
		t->texturechain = NULL;
	}

	GL_ImmColor4f (1,1,1,1);
	GL_SetAlphaThreshold(0.01f);	/* restore default */
	/* Blend/depth-mask cleanup gated for OIT pass. */
	if (!OIT_InPass())
	{
		glDisable_fp (GL_BLEND);
		glDepthMask_fp (1);
	}
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
		LM_ExpandDirtyRect(fa->lightmaptexturenum, fa->light_s, fa->light_t,
				   (fa->extents[0] >> 4) + 1, (fa->extents[1] >> 4) + 1);
		base = lightmaps + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
		base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
		R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
	}
}

/* Set true when GPU compute cull handles solid surfaces this frame */
static qboolean gpu_cull_active;
cvar_t r_gpucull = {"r_gpucull", "1", CVAR_ARCHIVE};	/* GPU compute world surface culling */

/* World VBO state (non-static — accessed by gl_worldcull.c) */
GLuint	world_vbo;
GLuint	world_ibo;
GLuint	world_vao;
int	world_num_verts;
int	world_num_indices;

/* Sky stencil VBO — defined later, but referenced from DrawTextureChains. */
extern GLuint	sky_stencil_vbo;
extern GLuint	sky_stencil_ibo;
extern GLuint	sky_stencil_vao;
extern int	sky_stencil_total_indices;

/* Brush-batch session: when active, R_DrawBrushModel skips its
 * per-entity world-VBO state setup and uses the shared bind set
 * up by R_BeginBrushBatch.  R_DrawEntitiesOnList brackets the
 * brush-model loop with these calls so 100+ entities share one
 * shader/VAO/atlas/fog/alpha_threshold bind. */
qboolean brush_batch_active = false;

/* Walk a brush submodel's surfaces and render only the special ones
 * (sky / turb / fence / underwater) via the legacy R_RenderBrushPoly.
 * Used when the brush-instance collector has already drawn this
 * submodel's regular surfaces via R_DrawBrushInstanced. */
void R_DrawBrushModelSpecialOnly (entity_t *e)
{
	int i;
	msurface_t *psurf;
	float dot;
	qmodel_t *clmodel = e->model;
	vec3_t modelorg_local;
	qboolean rotated;

	if (!clmodel)
		return;
	/* uhexen2-view-dep: guarantee GL_BLEND is disabled on entry (same fix as
	 * R_DrawBrushModel). Prevent stale blending state from world rendering. */
	glDisable_fp(GL_BLEND);
	rotated = (e->angles[0] || e->angles[1] || e->angles[2]) ? true : false;

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg_local);
	if (rotated)
	{
		vec3_t fwd, rt, up, tmp;
		VectorCopy (modelorg_local, tmp);
		AngleVectors (e->angles, fwd, rt, up);
		modelorg_local[0] =  DotProduct (tmp, fwd);
		modelorg_local[1] = -DotProduct (tmp, rt);
		modelorg_local[2] =  DotProduct (tmp, up);
	}

	GL_PushMatrix ();
	e->angles[0] = -e->angles[0];
	e->angles[2] = -e->angles[2];
	R_RotateForEntity (e);
	e->angles[0] = -e->angles[0];
	e->angles[2] = -e->angles[2];

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];
	for (i = 0; i < clmodel->nummodelsurfaces; i++, psurf++)
	{
		mplane_t *pp;
		int spec = psurf->flags &
			(SURF_DRAWSKY | SURF_DRAWTURB |
			 SURF_DRAWFENCE | SURF_UNDERWATER);
		/* Pure-fence surfaces are handled by R_DrawBrushInstanced's
		 * fence pass, not here.  Skip them unless paired with another
		 * special flag (sky/turb/underwater) that needs legacy emit. */
		if (spec == 0 || spec == SURF_DRAWFENCE)
			continue;
		pp = psurf->plane;
		dot = DotProduct (modelorg_local, pp->normal) - pp->dist;
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
		    (!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			R_RenderBrushPoly (e, psurf, false);
		}
	}

	GL_PopMatrix ();
}

/* Rebuild a single surface's lightmap atlas tile if its lightstyles
 * changed or a dlight touched it.  Exposed for the brush-instance
 * collector in gl_rmain.c which walks surfaces directly without going
 * through DrawTextureChains' normal path. */
void R_LightmapRebuildIfDirty (msurface_t *surf)
{
	int maps;
	qboolean style_changed = false;
	if (!surf || !surf->polys)
		return;
	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
	{
		if (d_lightstylevalue[surf->styles[maps]] != surf->cached_light[maps])
		{
			style_changed = true;
			break;
		}
	}
	if (!(style_changed || surf->dlightframe == r_framecount || surf->cached_dlight))
		return;
	{
		byte *base = lightmaps +
		    surf->lightmaptexturenum * lightmap_bytes * BLOCK_WIDTH * BLOCK_HEIGHT;
		base += surf->light_t * BLOCK_WIDTH * lightmap_bytes
		    + surf->light_s * lightmap_bytes;
		R_BuildLightMap (surf, base, BLOCK_WIDTH * lightmap_bytes);
		LM_ExpandDirtyRect (surf->lightmaptexturenum,
		    surf->light_s, surf->light_t,
		    (surf->extents[0] >> 4) + 1, (surf->extents[1] >> 4) + 1);
	}
}

void R_BeginBrushBatch (void)
{
	extern float r_fog_density;
	extern float r_fog_color[3];

	if (!world_vao || !lm_atlas_enabled || !lm_atlas_texture || !world_ibo)
		return;
	/* Brush-batch fast path emits opaque non-special surfaces only — fence
	 * and underwater fall through to legacy.  Bind the early_fragment_tests
	 * variant for Hi-Z.  uhexen2-5c6r. */
	glBindVertexArray_fp(world_vao);
	glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
	glUseProgram_fp(gl_shader_world_opaque.program);
	if (gl_shader_world_opaque.u_fog_density >= 0)
		glUniform1f_fp(gl_shader_world_opaque.u_fog_density, r_fog_density);
	if (gl_shader_world_opaque.u_fog_color >= 0)
		glUniform3f_fp(gl_shader_world_opaque.u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
	if (gl_shader_world_opaque.u_alpha_threshold >= 0)
		glUniform1f_fp(gl_shader_world_opaque.u_alpha_threshold, 0.01f);
	glActiveTexture_fp(GL_TEXTURE1);
	glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
	glActiveTexture_fp(GL_TEXTURE2);
	glBindTexture_fp(GL_TEXTURE_2D, gl_null_fb_texture);	/* sjvf: default fb */
	glActiveTexture_fp(GL_TEXTURE0);
	GL_ImmInvalidateState();
	brush_batch_active = true;
}

void R_EndBrushBatch (void)
{
	if (!brush_batch_active)
		return;
	glBindVertexArray_fp(0);
	glUseProgram_fp(0);
	brush_batch_active = false;
}

/* CPU sub-pass timers + counters for r_speeds >= 2.
 * Defined here so DrawTextureChains and R_DrawWorld can both write to them;
 * read by R_ProfileReport in gl_rmain.c via extern. */
double	rprof_cpu_bsp;
double	rprof_cpu_lmupload;
double	rprof_cpu_gpucull;
double	rprof_cpu_chains;
double	rprof_cpu_chains_skystencil;
double	rprof_cpu_chains_skyproc;
double	rprof_cpu_chains_loop;
double	rprof_cpu_chains_deferred;
int	rprof_chains_n_surfwalk;
int	rprof_chains_n_lmrebuilt;
double	rprof_cpu_chains_lmbuild;
double	rprof_cpu_chains_surfwalk;
double	rprof_cpu_chains_gpufinish;
int	rprof_chains_n_fast;
int	rprof_chains_n_imm;
int	rprof_chains_n_slow;
int	rprof_chains_n_skypoly;

/* Bind the per-frame world-VBO render state (VAO, shader, MVP/MV/fog/alpha
 * uniforms, lightmap atlas on TU1, color attribute white, TU0 sticky). Called
 * once before the texture loop and again from inside the fast-path branch if
 * a sibling branch (mirror/translucent/sky-no-skybox) invalidated state. */
static void DrawTextureChains_BindWorldState (void)
{
	float mvp[16], mv[16];
	/* Fast path emits opaque non-special surfaces (fence/water/sky/turb get
	 * sorted into separate buffers and the deferred section rebinds the
	 * cutout variant explicitly).  Use the early_fragment_tests variant
	 * here.  uhexen2-5c6r. */
	glprogram_t *prog = &gl_shader_world_opaque;
	glBindVertexArray_fp(world_vao);
	glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
	glUseProgram_fp(prog->program);
	GL_GetMVP(mvp);
	GL_GetModelview(mv);
	if (prog->u_mvp >= 0)
		glUniformMatrix4fv_fp(prog->u_mvp, 1, GL_FALSE, mvp);
	if (prog->u_modelview >= 0)
		glUniformMatrix4fv_fp(prog->u_modelview, 1, GL_FALSE, mv);
	if (prog->u_fog_density >= 0)
		glUniform1f_fp(prog->u_fog_density, r_fog_density);
	if (prog->u_fog_color >= 0)
		glUniform3f_fp(prog->u_fog_color,
			       r_fog_color[0], r_fog_color[1], r_fog_color[2]);
	if (prog->u_alpha_threshold >= 0)
		glUniform1f_fp(prog->u_alpha_threshold, 0.01f);
	glActiveTexture_fp(GL_TEXTURE1);
	glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
	/* TU2 is the fullbright mask sampled by sworld_frag.  Default to the
	 * 1x1 black sentinel so unit 2 always has SOMETHING bound when the
	 * shader samples; per-texture transitions in DrawTextureChains will
	 * override with the real fullbright mask when the diffuse has fb
	 * pixels.  uhexen2-sjvf. */
	glActiveTexture_fp(GL_TEXTURE2);
	glBindTexture_fp(GL_TEXTURE_2D, gl_null_fb_texture);
	glActiveTexture_fp(GL_TEXTURE0);	/* leave TU0 sticky for diffuse */
	/* These uploads bypass GL_ImmEnd's uniform cache. If the next
	 * GL_ImmEnd reuses gl_shader_world (e.g. fallback brush path),
	 * its cache must miss so it re-uploads the right values. */
	GL_ImmInvalidateState();
}

static void DrawTextureChains (entity_t *e)
{
	int		i;
	msurface_t	*s;
	texture_t	*t;
	double		_t0;
	qboolean	world_state_set = false;
	static msurface_t *world_deferred[4096];
	int		world_deferred_count = 0;

	/* Reset per-frame path counters for r_speeds */
	if (e == &r_worldentity)
	{
		rprof_chains_n_fast = 0;
		rprof_chains_n_imm = 0;
		rprof_chains_n_slow = 0;
		rprof_chains_n_skypoly = 0;
		rprof_cpu_chains_skystencil = 0;
		rprof_cpu_chains_skyproc = 0;
		rprof_cpu_chains_loop = 0;
		rprof_cpu_chains_deferred = 0;
		rprof_chains_n_surfwalk = 0;
		rprof_chains_n_lmrebuilt = 0;
		rprof_cpu_chains_lmbuild = 0;
		rprof_cpu_chains_surfwalk = 0;
		rprof_cpu_chains_gpufinish = 0;
	}

	/* Validate world model before iterating textures */
	if (!cl.worldmodel || !cl.worldmodel->textures) {
		Con_SafePrintf("ERROR: DrawTextureChains - null worldmodel or textures\n");
		return;
	}

	/* Diagnostic: drain GPU before any chain work. If `gpufinish` time
	 * eats most of `chains`, the CPU was waiting on GPU prior work
	 * (compute cull / indirect draws / sky stencil pre-pass) before we
	 * could submit anything else. Toggleable via cvar so it stays cheap
	 * in non-profiling runs. */
	{
		extern cvar_t r_speeds_gpufinish;
		if (r_speeds.integer >= 2 && e == &r_worldentity && r_speeds_gpufinish.integer)
		{
			double _t0gf = Sys_DoubleTime();
			glFinish_fp();
			rprof_cpu_chains_gpufinish = Sys_DoubleTime() - _t0gf;
		}
	}

	/* Sky depth+stencil pre-pass: write sky surface polys to depth buffer
	 * (occludes geometry behind sky walls) and mark stencil=1 so skybox
	 * only draws in sky areas.
	 *
	 * Indices for every world sky surface are pre-baked into a static
	 * VBO/IBO at map load (R_BuildSkyStencilVBO).  Per frame we walk the
	 * visible sky chain, sort by index offset, and collapse contiguous
	 * runs into a single glDrawElements call each.  No triangulation, no
	 * buffer upload — this used to be the dominant CPU cost on large
	 * maps.  Falls back to the imm path when the VBO isn't available
	 * (e.g. WebGL2 init failure). */
	if (have_stencil && skytexturenum >= 0 &&
	    skytexturenum < cl.worldmodel->numtextures &&
	    cl.worldmodel->textures[skytexturenum] &&
	    cl.worldmodel->textures[skytexturenum]->texturechain)
	{
		msurface_t *sky;
		_t0 = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0;
		glEnable_fp(GL_STENCIL_TEST);
		glStencilFunc_fp(GL_ALWAYS, 1, 0xFF);
		glStencilOp_fp(GL_KEEP, GL_KEEP, GL_REPLACE);
		glDepthMask_fp(1);	/* ensure depth writes are on */
		glColorMask_fp(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

		if (sky_stencil_vao && sky_stencil_ibo && sky_stencil_total_indices > 0)
		{
			/* Fast path: collect (firstindex, numindices) for every
			 * visible sky surface, sort, collapse contiguous runs,
			 * issue one glDrawElements per run. */
			static struct { int first; int count; } sky_runs[8192];
			int n_runs = 0;
			float mvp[16];

			for (sky = cl.worldmodel->textures[skytexturenum]->texturechain;
			     sky; sky = sky->texturechain)
			{
				if (sky->sky_numindices <= 0) continue;
				if (n_runs < (int)(sizeof(sky_runs)/sizeof(sky_runs[0])))
				{
					sky_runs[n_runs].first = sky->sky_firstindex;
					sky_runs[n_runs].count = sky->sky_numindices;
					n_runs++;
				}
				if (e == &r_worldentity) rprof_chains_n_skypoly++;
			}

			if (n_runs > 0)
			{
				int j;
				/* tiny insertion sort by firstindex */
				for (j = 1; j < n_runs; j++)
				{
					int kf = sky_runs[j].first;
					int kc = sky_runs[j].count;
					int i2 = j - 1;
					while (i2 >= 0 && sky_runs[i2].first > kf)
					{
						sky_runs[i2+1] = sky_runs[i2];
						i2--;
					}
					sky_runs[i2+1].first = kf;
					sky_runs[i2+1].count = kc;
				}

				glBindVertexArray_fp(sky_stencil_vao);
				glUseProgram_fp(gl_shader_flat.program);
				GL_GetMVP(mvp);
				if (gl_shader_flat.u_mvp >= 0)
					glUniformMatrix4fv_fp(gl_shader_flat.u_mvp, 1, GL_FALSE, mvp);
				/* a_color attribute disabled in this VAO -> uses generic value */
				glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);

				/* Collapse contiguous runs and emit. */
				{
					int run_first = sky_runs[0].first;
					int run_count = sky_runs[0].count;
					for (j = 1; j < n_runs; j++)
					{
						if (sky_runs[j].first == run_first + run_count)
						{
							run_count += sky_runs[j].count;
							continue;
						}
						glDrawElements_fp(GL_TRIANGLES, run_count, GL_UNSIGNED_INT,
								  (const void *)(uintptr_t)(run_first * sizeof(unsigned int)));
						run_first = sky_runs[j].first;
						run_count = sky_runs[j].count;
					}
					glDrawElements_fp(GL_TRIANGLES, run_count, GL_UNSIGNED_INT,
							  (const void *)(uintptr_t)(run_first * sizeof(unsigned int)));
				}

				glBindVertexArray_fp(0);
				glUseProgram_fp(0);
				GL_ImmInvalidateState();
				/* Force the chain loop's hoisted world-state bind to redo
				 * itself; we just stomped on VAO + program. */
				world_state_set = false;
			}
		}
		else
		{
			/* Fallback: original per-frame triangulation through imm batcher. */
			GL_ImmBegin();
			for (sky = cl.worldmodel->textures[skytexturenum]->texturechain;
			     sky; sky = sky->texturechain)
			{
				glpoly_t *p;
				for (p = sky->polys; p; p = p->next)
				{
					int j;
					if (p->numverts < 3)
						continue;
					if (GL_ImmCount() + (p->numverts - 2) * 3 >= GL_IMM_MAX_VERTS - 6)
					{
						GL_ImmEnd(GL_TRIANGLES, &gl_shader_flat);
						GL_ImmBegin();
					}
					for (j = 2; j < p->numverts; j++)
					{
						GL_ImmVertex3f(p->verts[0][0], p->verts[0][1], p->verts[0][2]);
						GL_ImmVertex3f(p->verts[j-1][0], p->verts[j-1][1], p->verts[j-1][2]);
						GL_ImmVertex3f(p->verts[j][0], p->verts[j][1], p->verts[j][2]);
					}
					if (e == &r_worldentity) rprof_chains_n_skypoly++;
				}
			}
			GL_ImmEnd(GL_TRIANGLES, &gl_shader_flat);
		}

		glColorMask_fp(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		/* Leave stencil test ON: any world geometry that draws
		 * closer than the sky surface resets stencil to 0,
		 * so the skybox only fills truly visible sky pixels. */
		glStencilFunc_fp(GL_ALWAYS, 0, 0xFF);
		glStencilOp_fp(GL_KEEP, GL_KEEP, GL_REPLACE);
		if (r_speeds.integer >= 2 && e == &r_worldentity)
			rprof_cpu_chains_skystencil = Sys_DoubleTime() - _t0;
	}

	/* Hoist world VBO state out of the per-chain loop. Each visible
	 * texture chain previously did glUseProgram + 5 uniform uploads +
	 * VAO bind + lightmap bind on its own. Bind once here; the fast-path
	 * branch reuses the bound state. Mirror / translucent / non-skybox
	 * sky chains invalidate it via world_state_set=false and the fast
	 * path then re-binds via DrawTextureChains_BindWorldState. */
	/* Hoisted world VBO bind feeds the fast-path glDrawElements runs.
	 * Always run even when gpu_cull_active: GPU cull's compute filter
	 * skips a number of surface classes (DRAWTILED, DRAWBLACK, FENCE,
	 * UNDERWATER, etc.) and any surface not referenced by a marksurf
	 * in the BSP — those still need the chain pass to draw them.
	 * Depth test eliminates overdraw at negligible cost.  See
	 * commit f1e0fb378 for the original fix. */
	if (e == &r_worldentity && lm_atlas_enabled && lm_atlas_texture && world_vao)
	{
		DrawTextureChains_BindWorldState();
		world_state_set = true;
	}

	{
	double _t0loop = (r_speeds.integer >= 2 && e == &r_worldentity) ? Sys_DoubleTime() : 0;
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
			{
			double _t0sp = (r_speeds.integer >= 2 && e == &r_worldentity) ? Sys_DoubleTime() : 0;
			if (skybox_name[0])
			{
				/* Sky_ProcessPoly is data-only; world state stays valid. */
				msurface_t *sky;
				for (sky = s; sky; sky = sky->texturechain)
					Sky_ProcessPoly (sky->polys);
			}
			else
			{
				if (world_state_set)
				{
					glBindVertexArray_fp(0);
					glUseProgram_fp(0);
					world_state_set = false;
				}
				R_DrawSkyChain (s);
			}
			if (r_speeds.integer >= 2 && e == &r_worldentity)
				rprof_cpu_chains_skyproc = Sys_DoubleTime() - _t0sp;
			}
			t->texturechain = NULL;
			continue;
		}
		else if (i == mirrortexturenum && r_mirroralpha.value != 1.0)
		{
			if (world_state_set)
			{
				glBindVertexArray_fp(0);
				glUseProgram_fp(0);
				world_state_set = false;
			}
			R_MirrorChain (s);
			continue;
		}
		else
		{
			if (s->flags & SURF_DRAWTURB)
				continue;	/* turb (water/lava/slime/teleport)
						 * always drawn by R_DrawWaterSurfaces;
						 * the fast/imm/slow paths below skip
						 * DRAWTURB anyway and would null the
						 * texturechain before R_DrawWaterSurfaces
						 * sees it.  Opaque liquids (default
						 * r_wateralpha=1, r_lavaalpha=0) need
						 * EmitWaterPolys for the warp UVs. */

			if (((e->drawflags & DRF_TRANSLUCENT) ||
				(e->drawflags & MLS_MASKIN) != MLS_NONE))
			{
				if (world_state_set)
				{
					glBindVertexArray_fp(0);
					glUseProgram_fp(0);
					world_state_set = false;
				}
				for ( ; s ; s = s->texturechain)
					R_RenderBrushPoly (e, s, false);
			}
			else if (lm_atlas_enabled && lm_atlas_texture && world_vao && e == &r_worldentity)
			{
				if (e == &r_worldentity) rprof_chains_n_fast++;
				/* Static VBO path: world state was hoisted before the loop;
				 * if a sibling branch invalidated it, rebind now. Per-chain
				 * we only update the diffuse texture and emit merged
				 * glDrawElements runs.
				 *
				 * Previously this branch was guarded by
				 *   !(r_dynamic.integer && cl_dlights[0].radius > 0)
				 * so any active dlight forced the entire world onto the
				 * 10x slower per-vertex ImmBegin fallback. The guard was
				 * load-bearing because the fast path didn't call
				 * R_BuildLightMap, so dlight contributions never made it
                                 * into the atlas. Now that the loop below rebuilds the
                                 * atlas pixels for any dirty surface, the guard is
                                 * gone and dlight-heavy boss fights stay batched. */
				#define MAX_BATCH_SURFS 4096
				static msurface_t *batch_surfs[MAX_BATCH_SURFS];
				int batch_count = 0;

				/* Always draw via this fast path even when
				 * gpu_cull_active.  GPU cull's compute filter skips
				 * several surface classes (DRAWTILED, DRAWBLACK,
				 * FENCE, UNDERWATER, ...) and any surface not in a
				 * BSP marksurf list — those still need the chain
				 * pass.  The earlier skip_solid optimization saved
				 * ~0.1ms but reintroduced the regression that commit
				 * f1e0fb378 had already fixed.  Depth test
				 * eliminates the overdraw at negligible GPU cost. */

				texture_t *batch_texture = NULL;

				if (!world_state_set)
				{
					DrawTextureChains_BindWorldState();
					world_state_set = true;
				}
				{
					texture_t *tt = R_TextureAnimation (e, s->texinfo->texture);
					/* Bindless-aware texture binding: use helper functions
					 * that conditionally bind or defer to shader handle
					 * indexing based on gl_bindless_able. */
					GL_BindDiffuse(tt->gl_texturenum);
					GL_BindFullbright(tt->gl_fb_texturenum ? tt->gl_fb_texturenum : gl_null_fb_texture);

					/* Store texture for SSBO population during batch submission */
					batch_texture = tt;
				}

				{
				double _t0sw = (r_speeds.integer >= 2 && e == &r_worldentity) ? Sys_DoubleTime() : 0;
				for ( ; s ; s = s->texturechain)
				{
					if (e == &r_worldentity) rprof_chains_n_surfwalk++;
					if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
						continue;

					if (s->flags & (SURF_DRAWFENCE | SURF_UNDERWATER))
					{
						if (world_deferred_count < (int)(sizeof(world_deferred)/sizeof(world_deferred[0])))
							world_deferred[world_deferred_count++] = s;
						continue;
					}

					if (s->vbo_numtris > 0 && batch_count < MAX_BATCH_SURFS)
						batch_surfs[batch_count++] = s;

					/* Apply dlights to blocklights for this surface */
					if (r_dynamic.integer && s->dlightframe == r_framecount)
						R_AddDynamicLights(s);

					/* Track lightmap polys for dynamic updates */
					if (s->polys)
					{
						int maps;
						qboolean style_changed = false;
						s->polys->chain = lightmap_polys[s->lightmaptexturenum];
						lightmap_polys[s->lightmaptexturenum] = s->polys;
						for (maps = 0; maps < MAXLIGHTMAPS && s->styles[maps] != 255; maps++)
						{
							if (d_lightstylevalue[s->styles[maps]] != s->cached_light[maps])
							{
								style_changed = true;
								break;
							}
						}
						/* If lightstyles or dlights touched this surface,
						 * rebuild the CPU-side atlas pixels via the same
						 * path the slow ImmBegin fallback uses, then mark
						 * the rect dirty for upload. Without R_BuildLightMap
						 * the atlas would be uploaded with stale pixels and
						 * dlights would either flicker or paint surfaces
						 * with corrupt UVs. */
						if (style_changed ||
						    s->dlightframe == r_framecount ||
						    s->cached_dlight)
						{
							double _t0lm = (r_speeds.integer >= 2 && e == &r_worldentity) ? Sys_DoubleTime() : 0;
							byte *base = lightmaps +
								s->lightmaptexturenum *
								lightmap_bytes *
								BLOCK_WIDTH * BLOCK_HEIGHT;
							base += s->light_t * BLOCK_WIDTH * lightmap_bytes
								+ s->light_s * lightmap_bytes;
							R_BuildLightMap (s, base, BLOCK_WIDTH * lightmap_bytes);
							LM_ExpandDirtyRect(s->lightmaptexturenum,
									   s->light_s, s->light_t,
									   (s->extents[0] >> 4) + 1,
									   (s->extents[1] >> 4) + 1);
							if (e == &r_worldentity)
							{
								rprof_chains_n_lmrebuilt++;
								if (r_speeds.integer >= 2)
									rprof_cpu_chains_lmbuild += Sys_DoubleTime() - _t0lm;
							}
						}
					}
				}
				if (r_speeds.integer >= 2 && e == &r_worldentity)
					rprof_cpu_chains_surfwalk += Sys_DoubleTime() - _t0sw;
				}

				/* Merge contiguous IBO ranges and draw */
				if (batch_count > 0)
				{
					/* Populate single SSBO entry for batch texture (all surfs share same texture) */
					if (gl_bindless_able && drawcall_cpu_buffer && batch_texture)
					{
						gltexture_t *tx = gltextures + batch_texture->gl_texturenum;
						gltexture_t *fb = batch_texture->gl_fb_texturenum ? gltextures + batch_texture->gl_fb_texturenum : NULL;
						drawcall_cpu_buffer[0].bindless.tx_handle = tx ? tx->bindless_handle : 0;
						drawcall_cpu_buffer[0].bindless.fb_handle = fb ? fb->bindless_handle : 0;
						drawcall_cpu_buffer[0].bindless.flags = 0;
						drawcall_cpu_buffer[0].bindless.alpha = 1.0f;
						GL_UploadDrawCallSSBO(1);

						/* Bind SSBO for shader access */
						if (drawcall_ssbo)
							glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 0, drawcall_ssbo);
					}

					int run_start = 0;
					int run_first_idx = batch_surfs[0]->vbo_firstindex;
					int run_total_idx = batch_surfs[0]->vbo_numtris * 3;
					int k;

					for (k = 1; k < batch_count; k++)
					{
						int expected = run_first_idx + run_total_idx;
						if (batch_surfs[k]->vbo_firstindex == expected)
						{
							run_total_idx += batch_surfs[k]->vbo_numtris * 3;
						}
						else
						{
							if (run_first_idx + run_total_idx <= world_num_indices && run_first_idx >= 0 && run_total_idx > 0)
							{
								glDrawElements_fp(GL_TRIANGLES,
										  run_total_idx,
										  GL_UNSIGNED_INT,
										  (void *)((size_t)run_first_idx * sizeof(unsigned int)));
								c_brush_polys += (k - run_start);
							}
							run_start = k;
							run_first_idx = batch_surfs[k]->vbo_firstindex;
							run_total_idx = batch_surfs[k]->vbo_numtris * 3;
						}
					}
					if (run_first_idx + run_total_idx <= world_num_indices && run_first_idx >= 0 && run_total_idx > 0)
					{
						glDrawElements_fp(GL_TRIANGLES,
								  run_total_idx,
								  GL_UNSIGNED_INT,
								  (void *)((size_t)run_first_idx * sizeof(unsigned int)));
						c_brush_polys += (batch_count - run_start);
					}
				}
				/* No per-chain teardown — state is reused by the next
				 * fast-path chain and torn down once after the loop. */
			}
			else if (lm_atlas_enabled && lm_atlas_texture)
			{
				if (e == &r_worldentity) rprof_chains_n_imm++;
				if (world_state_set)
				{
					glBindVertexArray_fp(0);
					glUseProgram_fp(0);
					world_state_set = false;
				}
				/* ImmBegin fallback for non-world brush entities */
				GL_ImmColor4f (1, 1, 1, 1);

				glActiveTexture_fp(GL_TEXTURE0);
				{
					texture_t *tt = R_TextureAnimation (e, s->texinfo->texture);
					GL_Bind (tt->gl_texturenum);
				}
				glActiveTexture_fp(GL_TEXTURE1);
				glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
				glActiveTexture_fp(GL_TEXTURE0);

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
							glActiveTexture_fp(GL_TEXTURE0);
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
				if (e == &r_worldentity) rprof_chains_n_slow++;
				if (world_state_set)
				{
					glBindVertexArray_fp(0);
					glUseProgram_fp(0);
					world_state_set = false;
				}
				/* No atlas — per-surface lightmap binds */
				for ( ; s ; s = s->texturechain)
					R_RenderBrushPolyMTex (e, s, false);
			}
		}

		t->texturechain = NULL;
	}
	if (r_speeds.integer >= 2 && e == &r_worldentity)
		rprof_cpu_chains_loop = Sys_DoubleTime() - _t0loop;
	}

	/* Render fence / underwater surfs collected from every fast-path
	 * chain.  Batched VBO path: partition by alpha-cutout vs opaque
	 * underwater, sort by texture, emit one glDrawElements per
	 * (group, texture) instead of one GL_ImmBegin/End per surface.
	 * Legacy R_RenderBrushPolyMTex still handles the rare warp-active
	 * underwater case (r_waterwarp && !GL_PostProcess_Active). */
	{
	double _t0def = (r_speeds.integer >= 2 && e == &r_worldentity) ? Sys_DoubleTime() : 0;
	if (world_deferred_count > 0)
	{
		extern qboolean GL_PostProcess_Active(void);
		extern float r_fog_density;
		extern float r_fog_color[3];
		qboolean warp_active = (r_waterwarp.integer && !GL_PostProcess_Active());
		int d;

		if (world_state_set)
		{
			glBindVertexArray_fp(0);
			glUseProgram_fp(0);
			world_state_set = false;
		}

		if (e == &r_worldentity && lm_atlas_enabled && lm_atlas_texture && world_vao && world_ibo)
		{
			static msurface_t *fence_buf[4096];
			static msurface_t *water_buf[4096];
			int fence_n = 0, water_n = 0, legacy_n = 0;

			/* Partition.  A surface that's both fence and underwater
			 * with active warp goes legacy (rare). */
			for (d = 0; d < world_deferred_count; d++)
			{
				msurface_t *ds = world_deferred[d];
				if (!ds->polys || ds->vbo_numtris <= 0)
				{
					R_RenderBrushPolyMTex (e, ds, false);
					legacy_n++;
					continue;
				}
				if (warp_active && (ds->flags & SURF_UNDERWATER))
				{
					/* CPU vertex warp — only the legacy path
					 * does that. */
					R_RenderBrushPolyMTex (e, ds, false);
					legacy_n++;
					continue;
				}
				if (ds->flags & SURF_DRAWFENCE)
				{
					if (fence_n < (int)(sizeof(fence_buf)/sizeof(fence_buf[0])))
						fence_buf[fence_n++] = ds;
				}
				else
				{
					/* SURF_UNDERWATER without warp = treat as
					 * a regular world surface. */
					if (water_n < (int)(sizeof(water_buf)/sizeof(water_buf[0])))
						water_buf[water_n++] = ds;
				}
			}

			if (fence_n > 0 || water_n > 0)
			{
				/* Sort each group by vbo_firstindex so contiguous
				 * runs collapse into single glDrawElements calls. */
#define DEF_LESS(a,b) ((a)->vbo_firstindex < (b)->vbo_firstindex)
				/* tiny insertion sort — N is small (typ. <300) */
				int j;
				for (j = 1; j < fence_n; j++)
				{
					msurface_t *key = fence_buf[j];
					int i2 = j - 1;
					while (i2 >= 0 && DEF_LESS(key, fence_buf[i2]))
					{
						fence_buf[i2+1] = fence_buf[i2];
						i2--;
					}
					fence_buf[i2+1] = key;
				}
				for (j = 1; j < water_n; j++)
				{
					msurface_t *key = water_buf[j];
					int i2 = j - 1;
					while (i2 >= 0 && DEF_LESS(key, water_buf[i2]))
					{
						water_buf[i2+1] = water_buf[i2];
						i2--;
					}
					water_buf[i2+1] = key;
				}
#undef DEF_LESS

				/* Bind world VBO + shader once. */
				glBindVertexArray_fp(world_vao);
				glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
				glUseProgram_fp(gl_shader_world.program);
				{
					float mvp[16], mv[16];
					GL_GetMVP(mvp);
					GL_GetModelview(mv);
					if (gl_shader_world.u_mvp >= 0)
						glUniformMatrix4fv_fp(gl_shader_world.u_mvp, 1, GL_FALSE, mvp);
					if (gl_shader_world.u_modelview >= 0)
						glUniformMatrix4fv_fp(gl_shader_world.u_modelview, 1, GL_FALSE, mv);
				}
				if (gl_shader_world.u_fog_density >= 0)
					glUniform1f_fp(gl_shader_world.u_fog_density, r_fog_density);
				if (gl_shader_world.u_fog_color >= 0)
					glUniform3f_fp(gl_shader_world.u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
				glActiveTexture_fp(GL_TEXTURE1);
				glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
				glActiveTexture_fp(GL_TEXTURE2);
				glBindTexture_fp(GL_TEXTURE_2D, gl_null_fb_texture);	/* sjvf: default fb */
				glActiveTexture_fp(GL_TEXTURE0);
				GL_ImmInvalidateState();

				/* Emit one batched draw per texture run.  When the
				 * texture changes we flush the running glDrawElements
				 * coalescing of contiguous IBO ranges.  Also rebind
				 * fullbright mask at TU2.  uhexen2-sjvf. */
#define BIND_TEX_WITH_FB(_T_) do { \
		GL_Bind((_T_)->gl_texturenum); \
		glActiveTexture_fp(GL_TEXTURE2); \
		glBindTexture_fp(GL_TEXTURE_2D, \
			(_T_)->gl_fb_texturenum ? (_T_)->gl_fb_texturenum : gl_null_fb_texture); \
		glActiveTexture_fp(GL_TEXTURE0); \
	} while (0)
#define EMIT_BATCH(BUF, N, ALPHA_T, A2C_ON) do { \
		if ((N) <= 0) break; \
		if (gl_shader_world.u_alpha_threshold >= 0) \
			glUniform1f_fp(gl_shader_world.u_alpha_threshold, (ALPHA_T)); \
		if (A2C_ON) \
			glEnable_fp(GL_SAMPLE_ALPHA_TO_COVERAGE); \
		else \
			glDisable_fp(GL_SAMPLE_ALPHA_TO_COVERAGE); \
		texture_t *cur_tex_ = NULL; \
		int run_first_ = 0, run_total_ = 0, count_run_ = 0; \
		int kk_; \
		for (kk_ = 0; kk_ < (N); kk_++) \
		{ \
			msurface_t *_s = (BUF)[kk_]; \
			texture_t *_t = R_TextureAnimation(e, _s->texinfo->texture); \
			int _next = _s->vbo_firstindex; \
			int _len  = _s->vbo_numtris * 3; \
			if (cur_tex_ == _t && (run_first_ + run_total_) == _next) \
			{ \
				run_total_ += _len; \
				count_run_++; \
				continue; \
			} \
			if (cur_tex_ && run_total_ > 0) \
			{ \
				BIND_TEX_WITH_FB(cur_tex_); \
				glDrawElements_fp(GL_TRIANGLES, run_total_, GL_UNSIGNED_INT, \
				    (void *)((size_t)run_first_ * sizeof(unsigned int))); \
				c_brush_polys += count_run_; \
			} \
			cur_tex_   = _t; \
			run_first_ = _next; \
			run_total_ = _len; \
			count_run_ = 1; \
		} \
		if (cur_tex_ && run_total_ > 0) \
		{ \
			BIND_TEX_WITH_FB(cur_tex_); \
			glDrawElements_fp(GL_TRIANGLES, run_total_, GL_UNSIGNED_INT, \
			    (void *)((size_t)run_first_ * sizeof(unsigned int))); \
			c_brush_polys += count_run_; \
		} \
	} while (0)

				EMIT_BATCH(fence_buf, fence_n, 0.666f, r_alphatocoverage.integer);
				EMIT_BATCH(water_buf, water_n, 0.01f,  false);
#undef EMIT_BATCH

				/* Tear down */
				glDisable_fp(GL_SAMPLE_ALPHA_TO_COVERAGE);
				glBindVertexArray_fp(0);
				glUseProgram_fp(0);
			}
		}
		else
		{
			/* Legacy per-surface fallback */
			for (d = 0; d < world_deferred_count; d++)
				R_RenderBrushPolyMTex (e, world_deferred[d], false);
		}
	}
	if (r_speeds.integer >= 2 && e == &r_worldentity)
		rprof_cpu_chains_deferred = Sys_DoubleTime() - _t0def;
	}

	/* Final teardown of hoisted world state. */
	if (world_state_set)
	{
		glBindVertexArray_fp(0);
		glUseProgram_fp(0);
		world_state_set = false;
	}

	/* Reset TU2 fullbright sampler to the null sentinel.  Subsequent
	 * paths that reuse gl_shader_world (brush-ent legacy R_RenderBrushPoly,
	 * sky stencil pre-pass) won't pick up a stale per-texture fb mask
	 * from the chain we just finished.  uhexen2-sjvf. */
	glActiveTexture_fp(GL_TEXTURE2);
	glBindTexture_fp(GL_TEXTURE_2D, gl_null_fb_texture);
	glActiveTexture_fp(GL_TEXTURE0);

	if (have_stencil)
		glDisable_fp(GL_STENCIL_TEST);
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
	/* uhexen2-view-dep: guarantee GL_BLEND is disabled on entry. Different
	 * world surfaces leave GL_BLEND in different states depending on what's
	 * visible from the current angle. Without this reset, brush entities
	 * inherit stale blending state and render with view-dependent artifacts
	 * (transparency changing as you rotate the camera). */
	glDisable_fp(GL_BLEND);

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

			if (rotated)
			{
				// Transform light origin from world space to model space
				dlight_t	transformedlight;
				vec3_t		temp;
				vec3_t		forward, right, up;

				transformedlight = cl_dlights[k];
				VectorSubtract(cl_dlights[k].origin, e->origin, temp);
				AngleVectors(e->angles, forward, right, up);
				transformedlight.origin[0] = DotProduct(temp, forward);
				transformedlight.origin[1] = -DotProduct(temp, right);
				transformedlight.origin[2] = DotProduct(temp, up);

				R_MarkLights(&transformedlight, 1<<k,
						clmodel->nodes + clmodel->hulls[0].firstclipnode);
			}
			else
			{
				R_MarkLights(&cl_dlights[k], 1<<k,
						clmodel->nodes + clmodel->hulls[0].firstclipnode);
			}
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

	/* Fast path: VBO-batched submodel rendering.  Group visible non-
	 * special surfaces by texture, emit one glDrawElements per group
	 * via the shared world VBO/IBO.  Special surfaces (sky / turb /
	 * fence / underwater / translucent) fall through to the legacy
	 * per-surface R_RenderBrushPoly path which handles them. */
	if (world_vao && lm_atlas_enabled && lm_atlas_texture && world_ibo &&
	    !Translucent && !(e->drawflags & DRF_TRANSLUCENT) &&
	    (e->drawflags & MLS_MASKIN) == MLS_NONE &&
	    !(e->model->flags & EF_TRANSPARENT))
	{
		#define MAX_BMODEL_BATCH 4096
		static msurface_t *batch_surfs[MAX_BMODEL_BATCH];
		int batch_count = 0;
		int n_special = 0;
		float mvp[16], mv[16];
		int s_idx;
		texture_t *cur_tex = NULL;
		extern float r_fog_density;
		extern float r_fog_color[3];

		/* Always re-bind VAO + shader + lightmap atlas: the legacy
		 * fall-through pass for special surfaces (sky / turb /
		 * fence / underwater) inside this very function may bind
		 * gl_shader_sky and leave it active when it returns.  Mesa
		 * fast-paths redundant binds to the same object so this is
		 * cheap when nothing changed.  When a brush batch is active
		 * we still skip the program-resident fog/alpha/color uniform
		 * uploads — those persist across glUseProgram cycles.
		 * Fast path emits opaque non-special surfaces only; bind the
		 * early_fragment_tests variant.  uhexen2-5c6r. */
		glprogram_t *prog = &gl_shader_world_opaque;
		glBindVertexArray_fp(world_vao);
		glUseProgram_fp(prog->program);
		glActiveTexture_fp(GL_TEXTURE1);
		glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
		/* Default fb to null sentinel — brush ent fast path doesn't
		 * carry per-texture fb info, so brush ents render without fb
		 * additive contribution.  uhexen2-sjvf. */
		glActiveTexture_fp(GL_TEXTURE2);
		glBindTexture_fp(GL_TEXTURE_2D, gl_null_fb_texture);
		glActiveTexture_fp(GL_TEXTURE0);
		if (!brush_batch_active)
		{
			glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
			if (prog->u_fog_density >= 0)
				glUniform1f_fp(prog->u_fog_density, r_fog_density);
			if (prog->u_fog_color >= 0)
				glUniform3f_fp(prog->u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
			if (prog->u_alpha_threshold >= 0)
				glUniform1f_fp(prog->u_alpha_threshold, 0.01f);
			GL_ImmInvalidateState();
		}
		GL_GetMVP(mvp);
		GL_GetModelview(mv);
		if (prog->u_mvp >= 0)
			glUniformMatrix4fv_fp(prog->u_mvp, 1, GL_FALSE, mvp);
		if (prog->u_modelview >= 0)
			glUniformMatrix4fv_fp(prog->u_modelview, 1, GL_FALSE, mv);

		/* Build texture chains by walking surfaces in texturechain
		 * order via the model's textures.  Each chain is processed,
		 * issuing a single batched glDrawElements per texture. */
		for (s_idx = 0; s_idx < clmodel->nummodelsurfaces; s_idx++)
		{
			msurface_t *surf = &clmodel->surfaces[clmodel->firstmodelsurface + s_idx];
			pplane = surf->plane;
			dot = DotProduct (modelorg, pplane->normal) - pplane->dist;

			/* Backface cull */
			if (!(((surf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			      (!(surf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON))))
				continue;

			/* Special surfaces fall to legacy path */
			if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB |
					   SURF_DRAWFENCE | SURF_UNDERWATER))
			{
				surf->visframe = -1;	/* mark for legacy pass */
				n_special++;
				continue;
			}
			if (surf->vbo_numtris <= 0)
				continue;

			surf->visframe = r_framecount;	/* mark drawn here */

			/* Texture-grouped batching: when texture changes,
			 * flush current batch then start new one. */
			texture_t *t = R_TextureAnimation(e, surf->texinfo->texture);
			if (cur_tex != t && batch_count > 0)
			{
				/* Flush previous group */
				int run_start = 0;
				int run_first = batch_surfs[0]->vbo_firstindex;
				int run_total = batch_surfs[0]->vbo_numtris * 3;
				int kk;
				GL_Bind(cur_tex->gl_texturenum);
				for (kk = 1; kk < batch_count; kk++)
				{
					int expected = run_first + run_total;
					if (batch_surfs[kk]->vbo_firstindex == expected)
						run_total += batch_surfs[kk]->vbo_numtris * 3;
					else
					{
						glDrawElements_fp(GL_TRIANGLES, run_total,
						    GL_UNSIGNED_INT,
						    (void *)((size_t)run_first * sizeof(unsigned int)));
						c_brush_polys += (kk - run_start);
						run_start = kk;
						run_first = batch_surfs[kk]->vbo_firstindex;
						run_total = batch_surfs[kk]->vbo_numtris * 3;
					}
				}
				glDrawElements_fp(GL_TRIANGLES, run_total,
				    GL_UNSIGNED_INT,
				    (void *)((size_t)run_first * sizeof(unsigned int)));
				c_brush_polys += (batch_count - run_start);
				batch_count = 0;
			}
			cur_tex = t;

			/* Lightmap rebuild for surfaces touched by lightstyle
			 * change or dynamic light. */
			if (surf->polys)
			{
				int maps;
				qboolean style_changed = false;
				surf->polys->chain = lightmap_polys[surf->lightmaptexturenum];
				lightmap_polys[surf->lightmaptexturenum] = surf->polys;
				for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
				{
					if (d_lightstylevalue[surf->styles[maps]] != surf->cached_light[maps])
					{
						style_changed = true;
						break;
					}
				}
				if (style_changed ||
				    surf->dlightframe == r_framecount ||
				    surf->cached_dlight)
				{
					byte *base = lightmaps +
					    surf->lightmaptexturenum *
					    lightmap_bytes * BLOCK_WIDTH * BLOCK_HEIGHT;
					base += surf->light_t * BLOCK_WIDTH * lightmap_bytes
					    + surf->light_s * lightmap_bytes;
					R_BuildLightMap (surf, base, BLOCK_WIDTH * lightmap_bytes);
					LM_ExpandDirtyRect(surf->lightmaptexturenum,
					    surf->light_s, surf->light_t,
					    (surf->extents[0] >> 4) + 1,
					    (surf->extents[1] >> 4) + 1);
				}
			}

			if (batch_count < MAX_BMODEL_BATCH)
				batch_surfs[batch_count++] = surf;
		}

		/* Flush final batch */
		if (batch_count > 0 && cur_tex)
		{
			int run_start = 0;
			int run_first = batch_surfs[0]->vbo_firstindex;
			int run_total = batch_surfs[0]->vbo_numtris * 3;
			int kk;
			GL_Bind(cur_tex->gl_texturenum);
			for (kk = 1; kk < batch_count; kk++)
			{
				int expected = run_first + run_total;
				if (batch_surfs[kk]->vbo_firstindex == expected)
					run_total += batch_surfs[kk]->vbo_numtris * 3;
				else
				{
					glDrawElements_fp(GL_TRIANGLES, run_total,
					    GL_UNSIGNED_INT,
					    (void *)((size_t)run_first * sizeof(unsigned int)));
					c_brush_polys += (kk - run_start);
					run_start = kk;
					run_first = batch_surfs[kk]->vbo_firstindex;
					run_total = batch_surfs[kk]->vbo_numtris * 3;
				}
			}
			glDrawElements_fp(GL_TRIANGLES, run_total,
			    GL_UNSIGNED_INT,
			    (void *)((size_t)run_first * sizeof(unsigned int)));
			c_brush_polys += (batch_count - run_start);
		}

		/* Tear down VBO state only when no batch session is open;
		 * otherwise R_EndBrushBatch handles it after the loop. */
		if (!brush_batch_active)
		{
			glBindVertexArray_fp(0);
			glUseProgram_fp(0);
		}

		/* Legacy path for any special surfaces marked above */
		if (n_special > 0)
		{
			psurf = &clmodel->surfaces[clmodel->firstmodelsurface];
			for (i = 0; i < clmodel->nummodelsurfaces; i++, psurf++)
			{
				if (psurf->visframe != -1)
					continue;
				R_RenderBrushPoly (e, psurf, false);
			}
		}
		#undef MAX_BMODEL_BATCH
	}
	else
	{
		//
		// draw texture (legacy per-surface path for translucent / abslight / etc.)
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
			/* Skip the BSP-side backface cull for SURF_DRAWTURB
			 * (water / lava / slime / teleporter) and SURF_UNDERWATER —
			 * these are always added to the texturechain regardless of
			 * which side the camera is on.  Bare `dot < 0` flips sign
			 * frame-to-frame when the camera is co-planar with the
			 * surface (player standing at lava waterline / head-bob),
			 * which used to drop the entire liquid chain for one frame
			 * and showed straight through to the lake bottom.
			 * uhexen2-w01k. */
			if (!(surf->flags & (SURF_UNDERWATER | SURF_DRAWTURB)) &&
			    ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK)))
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
	worldvert_t	*verts;
	unsigned int	*indices;
	int		i, j, lindex, v_pos, idx_pos;
	int		total_verts = 0, total_tris = 0;
	int		lnumverts;
	medge_t		*pedge;
	mvertex_t	*pvert;
	float		s, t;
	vec3_t		vec;

	if (!m)
		return;

	/* Free old VBO if reloading */
	if (world_vbo) { glDeleteBuffers_fp(1, &world_vbo); world_vbo = 0; }
	if (world_ibo) { glDeleteBuffers_fp(1, &world_ibo); world_ibo = 0; }
	if (world_vao) { glDeleteVertexArrays_fp(1, &world_vao); world_vao = 0; }

	/* Pass 1: count vertices and triangles from BSP surfaces */
	for (i = 0; i < m->numsurfaces; i++)
	{
		surf = &m->surfaces[i];
		/* Skip sky and warped surfaces — they have special rendering */
		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;
		lnumverts = surf->numedges;
		if (lnumverts < 3)
			continue;
		total_verts += lnumverts;
		total_tris += lnumverts - 2;
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

	/* Pass 2: reconstruct vertices directly from BSP data */
	v_pos = 0;
	idx_pos = 0;
	for (i = 0; i < m->numsurfaces; i++)
	{
		surf = &m->surfaces[i];
		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;
		lnumverts = surf->numedges;
		if (lnumverts < 3)
			continue;

		surf->vbo_firstvert = v_pos;
		surf->vbo_firstindex = idx_pos;
		surf->vbo_numtris = lnumverts - 2;

		/* Reconstruct vertices from surfedges/edges/vertexes */
		for (j = 0; j < lnumverts; j++)
		{
			lindex = m->surfedges[surf->firstedge + j];

			/* Get vertex position from edge lookup */
			if (lindex > 0) {
				pedge = &m->edges[lindex];
				pvert = &m->vertexes[pedge->v[0]];
			} else {
				pedge = &m->edges[-lindex];
				pvert = &m->vertexes[pedge->v[1]];
			}
			VectorCopy(pvert->position, vec);

			/* Compute diffuse texture coordinates */
			s = DotProduct(vec, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3];
			s /= surf->texinfo->texture->width;
			t = DotProduct(vec, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3];
			t /= surf->texinfo->texture->height;

			verts[v_pos].pos[0] = vec[0];
			verts[v_pos].pos[1] = vec[1];
			verts[v_pos].pos[2] = vec[2];
			verts[v_pos].st[0] = s;
			verts[v_pos].st[1] = t;

			/* Compute lightmap texture coordinates */
			s = DotProduct(vec, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3];
			s -= surf->texturemins[0];
			s += surf->light_s * 16;
			s += 8;
			s /= BLOCK_WIDTH * 16;

			t = DotProduct(vec, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3];
			t -= surf->texturemins[1];
			t += surf->light_t * 16;
			t += 8;
			t /= BLOCK_HEIGHT * 16;

			/* Remap page-local UV to atlas position (if atlas enabled) */
			if (lm_atlas_enabled)
			{
				int col = surf->lightmaptexturenum % LM_ATLAS_COLS;
				int row = surf->lightmaptexturenum / LM_ATLAS_COLS;
				s = (col + s) / (float)LM_ATLAS_COLS;
				t = (row + t) / (float)LM_ATLAS_ROWS;
			}

			verts[v_pos].lm[0] = s;
			verts[v_pos].lm[1] = t;
			v_pos++;
		}

		/* Emit triangle fan indices */
		for (j = 2; j < lnumverts; j++)
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

	/* Create bindless draw-call SSBO if supported */
	GL_CreateDrawCallSSBO();
}

void R_FreeWorldVBO (void)
{
	if (world_vbo) { glDeleteBuffers_fp(1, &world_vbo); world_vbo = 0; }
	if (world_ibo) { glDeleteBuffers_fp(1, &world_ibo); world_ibo = 0; }
	if (world_vao) { glDeleteVertexArrays_fp(1, &world_vao); world_vao = 0; }
	if (lm_atlas_texture) { glDeleteTextures_fp(1, &lm_atlas_texture); lm_atlas_texture = 0; }
	GL_DeleteDrawCallSSBO();
	lm_atlas_enabled = false;
	world_num_verts = 0;
	world_num_indices = 0;
}

/*
=============
Sky stencil VBO — pre-baked fan triangulation of every world sky surface.
The depth/stencil pre-pass walks the visible sky chain and issues a
glDrawElements per surface (or per merged contiguous run); no per-frame
triangulation, no per-frame buffer upload.  Replaces ~0.9ms of GL_ImmBegin
work on big maps.
=============
*/
GLuint	sky_stencil_vbo;
GLuint	sky_stencil_ibo;
GLuint	sky_stencil_vao;
int	sky_stencil_total_indices;

void R_FreeSkyStencilVBO (void)
{
	if (sky_stencil_vbo) { glDeleteBuffers_fp(1, &sky_stencil_vbo); sky_stencil_vbo = 0; }
	if (sky_stencil_ibo) { glDeleteBuffers_fp(1, &sky_stencil_ibo); sky_stencil_ibo = 0; }
	if (sky_stencil_vao) { glDeleteVertexArrays_fp(1, &sky_stencil_vao); sky_stencil_vao = 0; }
	sky_stencil_total_indices = 0;
}

void R_BuildSkyStencilVBO (void)
{
	qmodel_t	*m;
	msurface_t	*surf;
	glpoly_t	*p;
	float		*verts;
	unsigned int	*indices;
	int		i, j, v_pos, idx_pos;
	int		total_verts = 0, total_idx = 0;

	R_FreeSkyStencilVBO();

	if (!cl.worldmodel)
		return;

	/* Only the worldmodel renders through the texture-chain sky pre-pass.
	 * Brush submodel sky goes through Sky_ProcessEntities (drawn at
	 * a different point in the frame), and we don't optimize that here. */
	m = cl.worldmodel;
	for (i = 0; i < m->numsurfaces; i++)
	{
		surf = &m->surfaces[i];
		surf->sky_firstindex = -1;
		surf->sky_numindices = 0;
		if (!(surf->flags & SURF_DRAWSKY)) continue;
		for (p = surf->polys; p; p = p->next)
		{
			if (p->numverts < 3) continue;
			total_verts += p->numverts;
			total_idx += (p->numverts - 2) * 3;
		}
	}

	if (total_verts == 0 || total_idx == 0)
		return;

	verts = (float *) malloc(total_verts * 3 * sizeof(float));
	indices = (unsigned int *) malloc(total_idx * sizeof(unsigned int));
	if (!verts || !indices)
	{
		free(verts);
		free(indices);
		return;
	}

	/* Pass 2: emit verts + per-surface fan-triangulated index runs */
	v_pos = 0;
	idx_pos = 0;
	for (i = 0; i < m->numsurfaces; i++)
	{
		int surf_start_idx;
		surf = &m->surfaces[i];
		if (!(surf->flags & SURF_DRAWSKY)) continue;
		surf_start_idx = idx_pos;
		for (p = surf->polys; p; p = p->next)
		{
			int p_first_vert = v_pos;
			if (p->numverts < 3) continue;
			for (j = 0; j < p->numverts; j++)
			{
				verts[v_pos*3 + 0] = p->verts[j][0];
				verts[v_pos*3 + 1] = p->verts[j][1];
				verts[v_pos*3 + 2] = p->verts[j][2];
				v_pos++;
			}
			for (j = 2; j < p->numverts; j++)
			{
				indices[idx_pos++] = p_first_vert;
				indices[idx_pos++] = p_first_vert + j - 1;
				indices[idx_pos++] = p_first_vert + j;
			}
		}
		if (idx_pos > surf_start_idx)
		{
			surf->sky_firstindex = surf_start_idx;
			surf->sky_numindices = idx_pos - surf_start_idx;
		}
	}

	glGenVertexArrays_fp(1, &sky_stencil_vao);
	glBindVertexArray_fp(sky_stencil_vao);

	glGenBuffers_fp(1, &sky_stencil_vbo);
	glBindBuffer_fp(GL_ARRAY_BUFFER, sky_stencil_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, v_pos * 3 * sizeof(float),
			verts, GL_STATIC_DRAW);

	glEnableVertexAttribArray_fp(ATTR_POSITION);
	glVertexAttribPointer_fp(ATTR_POSITION, 3, GL_FLOAT, GL_FALSE,
				 3 * sizeof(float), (void *)0);

	glGenBuffers_fp(1, &sky_stencil_ibo);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, sky_stencil_ibo);
	glBufferData_fp(GL_ELEMENT_ARRAY_BUFFER, idx_pos * sizeof(unsigned int),
			indices, GL_STATIC_DRAW);

	glBindVertexArray_fp(0);
	sky_stencil_total_indices = idx_pos;

	free(verts);
	free(indices);

	Con_SafePrintf("Sky stencil VBO: %d verts, %d tris\n",
		       v_pos, idx_pos / 3);
}

/*
=============
R_DrawWorld
=============
*/
void R_DrawWorld (void)
{
	double t0;
#define DW_BEGIN()	(t0 = (r_speeds.integer >= 2) ? Sys_DoubleTime() : 0)
#define DW_END(slot)	do { if (r_speeds.integer >= 2) (slot) = Sys_DoubleTime() - t0; } while (0)

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

	/* Always run CPU BSP walk — needed for sky polys, water surfaces,
	 * lightmap updates, and entity efrags regardless of GPU culling */
	DW_BEGIN();
	R_RecursiveWorldNode (cl.worldmodel->nodes);
	DW_END(rprof_cpu_bsp);

	/* Upload any dirty lightmap rects BEFORE drawing — the GPU cull path
	 * and VBO batch path both read the atlas texture directly. */
	DW_BEGIN();
	R_UpdateLightmaps (false);
	DW_END(rprof_cpu_lmupload);

	DW_BEGIN();
#ifndef __EMSCRIPTEN__
	gpu_cull_active = R_WorldCullAvailable() && r_gpucull.integer;
	if (gpu_cull_active)
	{
		/* GPU compute culling draws solid world surfaces.
		 * DrawTextureChains handles sky, water, fence + lightmap updates. */
		R_DispatchWorldCull();
		R_DrawWorldCulled();
	}
#else
	gpu_cull_active = false;
#endif
	DW_END(rprof_cpu_gpucull);

	DW_BEGIN();
	DrawTextureChains (&r_worldentity);
	DW_END(rprof_cpu_chains);
	gpu_cull_active = false;
#undef DW_BEGIN
#undef DW_END

	// reset to texture unit 0
	glActiveTexture_fp (GL_TEXTURE1);
	glActiveTexture_fp (GL_TEXTURE0);

#ifdef QUAKE2
	R_DrawSkyBox ();
#endif
}


/*
=============================================================================

LIGHTMAP ALLOCATION

=============================================================================
*/

static unsigned int last_lightmap_allocated = 0;

// returns a texture number and the position inside it
static unsigned int AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	unsigned int	texnum;

	for (texnum = last_lightmap_allocated; texnum < MAX_LIGHTMAPS; texnum++)
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

		last_lightmap_allocated = texnum;
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

	/* Cache the world-space bbox for downstream culling
	 * (Sky_ProcessPoly / R_DrawSkyChain frustum reject). */
	if (lnumverts > 0)
	{
		float *v0 = poly->verts[0];
		int j, k;
		VectorCopy (v0, poly->mins);
		VectorCopy (v0, poly->maxs);
		for (j = 1; j < lnumverts; j++)
		{
			float *v = poly->verts[j];
			for (k = 0; k < 3; k++)
			{
				if (v[k] < poly->mins[k]) poly->mins[k] = v[k];
				if (v[k] > poly->maxs[k]) poly->maxs[k] = v[k];
			}
		}
	}
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
	last_lightmap_allocated = 0;

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

	glActiveTexture_fp (GL_TEXTURE1);

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

	glActiveTexture_fp (GL_TEXTURE0);
}

