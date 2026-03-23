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

extern float r_fog_density;
extern float r_fog_color[3];

static void GL_BuildWorldVBO (void);

int		gl_lightmap_format = GL_RGBA;
cvar_t		gl_lightmapfmt = {"gl_lightmapfmt", "GL_RGBA", CVAR_NONE};
int		lightmap_bytes = 4;		// 1, 2, or 4. default is 4 for GL_RGBA
static int	lightmap_internalformat = 0x8058;	// GL_RGBA8: sized internal format for glTexImage2D
GLuint		lightmap_textures[MAX_LIGHTMAPS];
GLuint		lightmap_array_texture;		/* GL_TEXTURE_2D_ARRAY for all lightmaps */
int		lightmap_array_layers;		/* number of layers allocated */

static unsigned int	blocklights[18*18];
static unsigned int	blocklightscolor[18*18*3];	// colored light support. *3 for RGB to the definitions at the top

#define	BLOCK_WIDTH	128
#define	BLOCK_HEIGHT	128

static glpoly_t	*lightmap_polys[MAX_LIGHTMAPS];
static qboolean	lightmap_modified[MAX_LIGHTMAPS];

static int	allocated[MAX_LIGHTMAPS][BLOCK_WIDTH];

// the lightmap texture data needs to be kept in
// main memory so texsubimage can update properly
static byte	lightmaps[4*MAX_LIGHTMAPS*BLOCK_WIDTH*BLOCK_HEIGHT];

/* Static world VBO — all world surface triangles uploaded once at level load */
typedef struct {
	float	pos[3];
	float	texcoord[2];
	float	lmcoord[2];
	float	lmlayer;
	float	color[4];
} worldvert_t;
#define WORLD_STRIDE	(12 * sizeof(float))

static GLuint	world_vbo;
static GLuint	world_vao;
static int	world_num_verts;
static int	world_max_verts;

/* Scratch arrays for glMultiDrawArrays — collected per texture chain */
#define MAX_MULTIDRAW	4096
static GLint	multidraw_first[MAX_MULTIDRAW];
static GLsizei	multidraw_count[MAX_MULTIDRAW];


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
	for (i = 0; i < size; i++)
	{
		if (gl_lightmap_format == GL_RGBA)
			blocklightscolor[i*3+0] =
			blocklightscolor[i*3+1] =
			blocklightscolor[i*3+2] = 0;
		else
			blocklights[i] = 0;
	}

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

// add all the dynamic lights
	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights (surf);

// bound, invert, and shift
store:
	switch (gl_lightmap_format)
	{
	case GL_RGBA:
		stride -= (smax<<2);

		blcr = &blocklightscolor[0];
		blcg = &blocklightscolor[1];
		blcb = &blocklightscolor[2];

		for (i = 0; i < tmax; i++, dest += stride)
		{
			for (j = 0; j < smax; j++)
			{
				q = *blcr;
				q >>= 7;
				r = *blcg;
				r >>= 7;
				s = *blcb;
				s >>= 7;

				if (q > 255)
					q = 255;
				if (r > 255)
					r = 255;
				if (s > 255)
					s = 255;

				if (gl_coloredlight.integer)
				{
					dest[0] = q; //255 - q;
					dest[1] = r; //255 - r;
					dest[2] = s; //255 - s;
					dest[3] = 255; //(q+r+s)/3;
				}
				else
				{
					t = (int) ((float)q * 0.33f + (float)s * 0.33f + (float)r * 0.33f);

					if (t > 255)
						t = 255;
					dest[0] = t;
					dest[1] = t;
					dest[2] = t;
					dest[3] = 255; //t;
				}

				dest += 4;

				blcr += 3;
				blcg += 3;
				blcb += 3;
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

		if (r_waterwarp.integer)
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

		if (r_waterwarp.integer)
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

			/* Update individual 2D texture (for brush entity path) */
			GL_Bind(lightmap_textures[i]);
			glTexImage2D_fp (GL_TEXTURE_2D, 0, lightmap_internalformat, BLOCK_WIDTH,
					BLOCK_HEIGHT, 0, gl_lightmap_format, GL_UNSIGNED_BYTE,
					lightmaps + i*BLOCK_WIDTH*BLOCK_HEIGHT*lightmap_bytes);

			/* Update corresponding layer in the texture array */
			if (lightmap_array_texture && glTexSubImage3D_fp)
			{
				glBindTexture_fp(GL_TEXTURE_2D_ARRAY, lightmap_array_texture);
				glTexSubImage3D_fp(GL_TEXTURE_2D_ARRAY, 0,
						0, 0, i,
						BLOCK_WIDTH, BLOCK_HEIGHT, 1,
						gl_lightmap_format, GL_UNSIGNED_BYTE,
						lightmaps + i*BLOCK_WIDTH*BLOCK_HEIGHT*lightmap_bytes);
				glBindTexture_fp(GL_TEXTURE_2D_ARRAY, 0);
			}
		}
	}

	glActiveTextureARB_fp (GL_TEXTURE0_ARB);

	/* Build static VBO for world surfaces (only on first load,
	 * not on video reinit — surface polys don't change) */
	if (!draw_reinit)
	{
		Con_DPrintf("[GL] Building world VBO\n");
		GL_BuildWorldVBO ();
	}

	Con_DPrintf("[GL] BuildLightmaps: complete\n");
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

	if (!override)
		GL_ImmColor4f(intensity, intensity, intensity, alpha_val);

	if (fa->flags & SURF_DRAWSKY)
	{	// warp texture, no lightmaps
		EmitBothSkyLayers (fa);
		return;
	}

	t = R_TextureAnimation (e, fa->texinfo->texture);
	GL_Bind (t->gl_texturenum);

	if (fa->flags & SURF_DRAWTURB)
	{	// warp texture, no lightmaps
		EmitWaterPolys (fa);
		return;
	}

	if (fa->flags & SURF_DRAWFENCE)
	{
		glDisable_fp(GL_BLEND);
		GL_SetAlphaThreshold(0.666f);
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

		/* Use lightmap texture array with per-vertex layer */
		glBindTexture_fp(GL_TEXTURE_2D_ARRAY, lightmap_array_texture);
		GL_ImmLMLayer ((float)fa->lightmaptexturenum);

		if (fa->flags & SURF_UNDERWATER)
			DrawGLWaterPolyMTexLM (fa->polys);
		else
			DrawGLPolyMTex (fa->polys);

		glBindTexture_fp(GL_TEXTURE_2D_ARRAY, 0);
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
		if (r_dynamic.integer)
		{
			lightmap_modified[fa->lightmaptexturenum] = true;
			base = lightmaps + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
			base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
			R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
		}
	}

	if (e->drawflags & DRF_TRANSLUCENT)
	{
		glDisable_fp (GL_BLEND);
	}

	if (fa->flags & SURF_DRAWFENCE)
		GL_SetAlphaThreshold(0.01f);
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
			glBindTexture_fp(GL_TEXTURE_2D_ARRAY, lightmap_array_texture);
			GL_ImmLMLayer ((float)fa->lightmaptexturenum);

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
			if (r_dynamic.integer)
			{
				lightmap_modified[fa->lightmaptexturenum] = true;
				base = lightmaps + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
				base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
				R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
			}
		}
	}

	glActiveTextureARB_fp(GL_TEXTURE0_ARB);

	if (e->drawflags & DRF_TRANSLUCENT)
	{
		glDisable_fp (GL_BLEND);
	}

	if (fa->flags & SURF_DRAWFENCE)
		GL_SetAlphaThreshold(0.01f);

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

		//if ((s->flags & SURF_DRAWTURB) && (s->flags & SURF_TRANSLUCENT))
		if (s->flags & SURF_TRANSLUCENT)
			GL_ImmColor4f (1,1,1,r_wateralpha.value);
		else
			GL_ImmColor4f (1,1,1,1);

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
R_CheckSurfaceLightmap

Track lightmap chain and check for dynamic updates on a surface.
Used by both the immediate-mode batch path and static VBO path.
================
*/
static void R_CheckSurfaceLightmap (msurface_t *fa)
{
	byte	*base;
	int	maps;

	fa->polys->chain = lightmap_polys[fa->lightmaptexturenum];
	lightmap_polys[fa->lightmaptexturenum] = fa->polys;

	for (maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
	{
		if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
			goto dynamic_check;
	}
	if (fa->dlightframe == r_framecount || fa->cached_dlight)
	{
dynamic_check:
		if (r_dynamic.integer)
		{
			lightmap_modified[fa->lightmaptexturenum] = true;
			base = lightmaps + fa->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
			base += fa->light_t * BLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
			R_BuildLightMap (fa, base, BLOCK_WIDTH*lightmap_bytes);
		}
	}
}

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
	int		j, nverts;
	byte		*base;
	int		maps;
	float		layer = (float)fa->lightmaptexturenum;

	nverts = p->numverts;
	if (nverts < 3)
		return;

	/* Check if we need to flush before adding this surface.
	 * Each polygon of N verts produces (N-2)*3 triangle verts. */
	if (GL_ImmCount() + (nverts - 2) * 3 >= GL_IMM_MAX_VERTS - 6)
	{
		GL_ImmEnd (GL_TRIANGLES, &gl_shader_world);
		GL_ImmBegin ();
	}

	/* Set lightmap layer for this surface — no texture rebind needed
	 * since we use GL_TEXTURE_2D_ARRAY bound once for all layers */
	GL_ImmLMLayer (layer);

	/* Emit as triangles (fan → triangles: v0-v1-v2, v0-v2-v3, ...) */
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

	R_CheckSurfaceLightmap (fa);
}

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
			if ((s->flags & SURF_DRAWTURB) && r_wateralpha.value != 1.0)
				continue;	// draw translucent water later

			if (((e->drawflags & DRF_TRANSLUCENT) ||
				(e->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT))
			{
				for ( ; s ; s = s->texturechain)
					R_RenderBrushPoly (e, s, false);
			}
			else if (world_vao && lightmap_array_texture)
			{
				/* Static VBO path: vertices pre-uploaded at level load.
				 * Bind lightmap array + diffuse texture, then draw
				 * visible surfaces from static VBO. Zero per-frame
				 * vertex streaming. */
				float mvp[16];
				float mv[16];

				/* Bind diffuse texture on unit 0 */
				glActiveTextureARB_fp(GL_TEXTURE0_ARB);
				{
					texture_t *tt = R_TextureAnimation (e, s->texinfo->texture);
					GL_Bind (tt->gl_texturenum);
				}

				/* Bind lightmap array on unit 1 */
				glActiveTextureARB_fp(GL_TEXTURE1_ARB);
				glBindTexture_fp(GL_TEXTURE_2D_ARRAY, lightmap_array_texture);
				glActiveTextureARB_fp(GL_TEXTURE0_ARB);

				/* Activate shader and set uniforms */
				glUseProgram_fp(gl_shader_world.program);
				GL_GetMVP(mvp);
				if (gl_shader_world.u_mvp >= 0)
					glUniformMatrix4fv_fp(gl_shader_world.u_mvp, 1, GL_FALSE, mvp);
				if (gl_shader_world.u_modelview >= 0)
				{
					GL_GetModelview(mv);
					glUniformMatrix4fv_fp(gl_shader_world.u_modelview, 1, GL_FALSE, mv);
				}
				if (gl_shader_world.u_fog_density >= 0)
					glUniform1f_fp(gl_shader_world.u_fog_density, r_fog_density);
				if (gl_shader_world.u_fog_color >= 0)
					glUniform3f_fp(gl_shader_world.u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
				if (gl_shader_world.u_alpha_threshold >= 0)
					glUniform1f_fp(gl_shader_world.u_alpha_threshold, 0.01f);

				glBindVertexArray_fp(world_vao);

				{
				int ndraw = 0;

				for ( ; s ; s = s->texturechain)
				{
					if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB |
							SURF_DRAWFENCE | SURF_UNDERWATER))
					{
						/* Flush batch before falling back */
						if (ndraw > 0)
						{
							if (glMultiDrawArrays_fp)
								glMultiDrawArrays_fp(GL_TRIANGLES, multidraw_first, multidraw_count, ndraw);
							else for (i = 0; i < ndraw; i++)
								glDrawArrays_fp(GL_TRIANGLES, multidraw_first[i], multidraw_count[i]);
							ndraw = 0;
						}
						glBindVertexArray_fp(0);
						glUseProgram_fp(0);
						R_RenderBrushPolyMTex (e, s, false);
						glActiveTextureARB_fp(GL_TEXTURE0_ARB);
						{
							texture_t *tt = R_TextureAnimation (e, s->texinfo->texture);
							GL_Bind (tt->gl_texturenum);
						}
						glActiveTextureARB_fp(GL_TEXTURE1_ARB);
						glBindTexture_fp(GL_TEXTURE_2D_ARRAY, lightmap_array_texture);
						glActiveTextureARB_fp(GL_TEXTURE0_ARB);
						glUseProgram_fp(gl_shader_world.program);
						glBindVertexArray_fp(world_vao);
						continue;
					}

					if (s->vbo_num_verts > 0 && ndraw < MAX_MULTIDRAW)
					{
						multidraw_first[ndraw] = s->vbo_first_vert;
						multidraw_count[ndraw] = s->vbo_num_verts;
						ndraw++;
						c_brush_polys++;
					}

					R_CheckSurfaceLightmap (s);
				}

				/* Flush remaining batch */
				if (ndraw > 0)
				{
					if (glMultiDrawArrays_fp)
						glMultiDrawArrays_fp(GL_TRIANGLES, multidraw_first, multidraw_count, ndraw);
					else for (i = 0; i < ndraw; i++)
						glDrawArrays_fp(GL_TRIANGLES, multidraw_first[i], multidraw_count[i]);
				}
				}

				glBindVertexArray_fp(0);
				glUseProgram_fp(0);

				/* Restore GL state for subsequent rendering */
				glActiveTextureARB_fp(GL_TEXTURE1_ARB);
				glBindTexture_fp(GL_TEXTURE_2D_ARRAY, 0);
				glActiveTextureARB_fp(GL_TEXTURE0_ARB);
				currenttexture = GL_UNUSED_TEXTURE; /* force rebind */
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
		return;		// solid

	if (node->visframe != r_visframecount)
		return;
	if (R_CullBox (node->minmaxs, node->minmaxs+3))
		return;

// if a leaf node, draw stuff
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

	// deal with model fragments in this leaf
		if (pleaf->efrags)
			R_StoreEfrags (&pleaf->efrags);

		return;
	}

// node is just a decision point, so go down the apropriate sides

// find which side of the node we are on
	plane = node->plane;

	switch (plane->type)
	{
	case PLANE_X:
		dot = modelorg[0] - plane->dist;
		break;
	case PLANE_Y:
		dot = modelorg[1] - plane->dist;
		break;
	case PLANE_Z:
		dot = modelorg[2] - plane->dist;
		break;
	default:
		dot = DotProduct (modelorg, plane->normal) - plane->dist;
		break;
	}

	if (dot >= 0)
		side = 0;
	else
		side = 1;

// recurse down the children, front side first
	R_RecursiveWorldNode (node->children[side]);

// draw stuff
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

			// don't backface underwater surfaces, because they warp
			if (!(surf->flags & SURF_UNDERWATER) && ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK)))
				continue;	// wrong side

			// sorting by texture, just store it out
			if (!mirror
				|| surf->texinfo->texture != cl.worldmodel->textures[mirrortexturenum])
			{
				surf->texturechain = surf->texinfo->texture->texturechain;
				surf->texinfo->texture->texturechain = surf;
			}
		}
	}

// recurse down the back side
	R_RecursiveWorldNode (node->children[!side]);
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
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s*16;
		s += 8;
		s /= BLOCK_WIDTH*16; //fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t*16;
		t += 8;
		t /= BLOCK_HEIGHT*16; //fa->texinfo->texture->height;

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
GL_BuildWorldVBO

Triangulate all world surfaces and upload to a static VBO.
Called after GL_BuildLightmaps when all surface polys are built.
========================
*/
static void GL_BuildWorldVBO (void)
{
	int		i, j, total_verts;
	qmodel_t	*m;
	msurface_t	*surf;
	worldvert_t	*verts, *v;
	glpoly_t	*p;

	/* Delete old VBO if rebuilding */
	if (world_vbo)
	{
		glDeleteBuffers_fp(1, &world_vbo);
		world_vbo = 0;
	}
	if (world_vao)
	{
		glDeleteVertexArrays_fp(1, &world_vao);
		world_vao = 0;
	}
	world_num_verts = 0;

	/* Count total triangle vertices needed */
	total_verts = 0;
	m = cl.model_precache[1];	/* worldmodel */
	if (!m) return;
	for (i = 0; i < m->numsurfaces; i++)
	{
		surf = m->surfaces + i;
		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;
		p = surf->polys;
		if (!p || p->numverts < 3)
			continue;
		total_verts += (p->numverts - 2) * 3;
	}

	if (total_verts == 0)
		return;

	Con_DPrintf("World VBO: %d tris, %d KB\n", total_verts / 3,
		   (int)(total_verts * sizeof(worldvert_t) / 1024));

	/* Allocate and fill vertex buffer */
	verts = (worldvert_t *) malloc(total_verts * sizeof(worldvert_t));
	if (!verts)
	{
		Con_Printf("GL_BuildWorldVBO: out of memory for %d verts\n", total_verts);
		return;
	}

	v = verts;
	for (i = 0; i < m->numsurfaces; i++)
	{
		surf = m->surfaces + i;
		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
		{
			surf->vbo_first_vert = 0;
			surf->vbo_num_verts = 0;
			continue;
		}
		p = surf->polys;
		if (!p || p->numverts < 3)
		{
			surf->vbo_first_vert = 0;
			surf->vbo_num_verts = 0;
			continue;
		}

		surf->vbo_first_vert = (int)(v - verts);
		surf->vbo_num_verts = (p->numverts - 2) * 3;

		/* Triangulate fan → triangles */
		for (j = 2; j < p->numverts; j++)
		{
			float *v0 = p->verts[0];
			float *v1 = p->verts[j - 1];
			float *v2 = p->verts[j];

#define FILL_VERT(dst, src) do { \
	(dst)->pos[0] = (src)[0]; (dst)->pos[1] = (src)[1]; (dst)->pos[2] = (src)[2]; \
	(dst)->texcoord[0] = (src)[3]; (dst)->texcoord[1] = (src)[4]; \
	(dst)->lmcoord[0] = (src)[5]; (dst)->lmcoord[1] = (src)[6]; \
	(dst)->lmlayer = (float)surf->lightmaptexturenum; \
	(dst)->color[0] = 1; (dst)->color[1] = 1; (dst)->color[2] = 1; (dst)->color[3] = 1; \
} while(0)

			FILL_VERT(v, v0); v++;
			FILL_VERT(v, v1); v++;
			FILL_VERT(v, v2); v++;
#undef FILL_VERT
		}
	}

	world_num_verts = total_verts;
	world_max_verts = total_verts;

	/* Create VAO + VBO */
	if (!world_vao)
		glGenVertexArrays_fp(1, &world_vao);
	glBindVertexArray_fp(world_vao);

	if (!world_vbo)
		glGenBuffers_fp(1, &world_vbo);
	glBindBuffer_fp(GL_ARRAY_BUFFER, world_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, total_verts * sizeof(worldvert_t),
			 verts, GL_STATIC_DRAW);

	/* Vertex attributes — same layout as immvert_t */
	glEnableVertexAttribArray_fp(ATTR_POSITION);
	glVertexAttribPointer_fp(ATTR_POSITION, 3, GL_FLOAT, GL_FALSE,
				  WORLD_STRIDE, (void *)0);
	glEnableVertexAttribArray_fp(ATTR_TEXCOORD);
	glVertexAttribPointer_fp(ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE,
				  WORLD_STRIDE, (void *)(3 * sizeof(float)));
	glEnableVertexAttribArray_fp(ATTR_LMCOORD);
	glVertexAttribPointer_fp(ATTR_LMCOORD, 2, GL_FLOAT, GL_FALSE,
				  WORLD_STRIDE, (void *)(5 * sizeof(float)));
	glEnableVertexAttribArray_fp(ATTR_LMLAYER);
	glVertexAttribPointer_fp(ATTR_LMLAYER, 1, GL_FLOAT, GL_FALSE,
				  WORLD_STRIDE, (void *)(7 * sizeof(float)));
	glEnableVertexAttribArray_fp(ATTR_COLOR);
	glVertexAttribPointer_fp(ATTR_COLOR, 4, GL_FLOAT, GL_FALSE,
				  WORLD_STRIDE, (void *)(8 * sizeof(float)));

	glBindVertexArray_fp(0);
	glBindBuffer_fp(GL_ARRAY_BUFFER, 0);

	free(verts);
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

	Con_DPrintf("[GL] BuildLightmaps: start (reinit=%d)\n", draw_reinit);

	memset (allocated, 0, sizeof(allocated));
	memset (lightmap_modified, 0, sizeof(lightmap_modified));
	memset (lightmap_polys, 0, sizeof(lightmap_polys));

	r_framecount = 1;		// no dlightcache

	if (! lightmap_textures[0])
	{
		glGenTextures_fp(MAX_LIGHTMAPS, lightmap_textures);
	}

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
	// Count used lightmap layers and upload all that were filled
	//
	lightmap_array_layers = 0;
	for (i = 0; i < MAX_LIGHTMAPS; i++)
	{
		if (!allocated[i][0])
			break;		// no more used
		lightmap_modified[i] = false;
		lightmap_array_layers++;

		/* Keep individual textures for legacy brush entity path */
		GL_Bind(lightmap_textures[i]);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D_fp (GL_TEXTURE_2D, 0, lightmap_internalformat, BLOCK_WIDTH,
				BLOCK_HEIGHT, 0, gl_lightmap_format, GL_UNSIGNED_BYTE,
				lightmaps + i*BLOCK_WIDTH*BLOCK_HEIGHT*lightmap_bytes);
	}

	/* Create GL_TEXTURE_2D_ARRAY for batched world rendering */
	if (glTexImage3D_fp && lightmap_array_layers > 0)
	{
		if (!lightmap_array_texture)
			glGenTextures_fp(1, &lightmap_array_texture);
		glBindTexture_fp(GL_TEXTURE_2D_ARRAY, lightmap_array_texture);
		glTexParameterf_fp(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf_fp(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameterf_fp(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf_fp(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage3D_fp(GL_TEXTURE_2D_ARRAY, 0, lightmap_internalformat,
				BLOCK_WIDTH, BLOCK_HEIGHT, lightmap_array_layers, 0,
				gl_lightmap_format, GL_UNSIGNED_BYTE, lightmaps);
		glBindTexture_fp(GL_TEXTURE_2D_ARRAY, 0);
	}
	else
		lightmap_array_texture = 0;

	Con_SafePrintf("Lightmaps: %d pages (%dx%d) in texture array\n",
		       lightmap_array_layers, BLOCK_WIDTH, BLOCK_HEIGHT);

	glActiveTextureARB_fp (GL_TEXTURE0_ARB);
}

