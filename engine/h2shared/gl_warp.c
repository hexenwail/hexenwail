/*
 * gl_warp.c -- sky and water polygons
 * Copyright (C) 1996-1997  Id Software, Inc.
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
#include "gl_shader.h"
#include "gl_vbo.h"

/* ES 3.0 compatibility: GL_QUADS and GL_POLYGON don't exist */
#ifdef EMSCRIPTEN
#ifndef GL_QUADS
#define GL_QUADS 0
#endif
#ifndef GL_POLYGON
#define GL_POLYGON 0
#endif
#endif

int		skytexturenum;

static GLuint	solidskytexture, alphaskytexture;

static msurface_t	*warpface;

#define	SUBDIVIDE_SIZE	64

static void BoundPoly (int numverts, float *verts, vec3_t mins, vec3_t maxs)
{
	int		i, j;
	float	*v;

	mins[0] = mins[1] = mins[2] = 9999999;	/* FIXME: change these two to FLT_MAX/-FLT_MAX */
	maxs[0] = maxs[1] = maxs[2] = -9999999;
	v = verts;
	for (i = 0; i < numverts; i++)
	{
		for (j = 0; j < 3; j++, v++)
		{
			if (*v < mins[j])
				mins[j] = *v;
			if (*v > maxs[j])
				maxs[j] = *v;
		}
	}
}

static void SubdividePolygon (int numverts, float *verts)
{
	int		i, j, k;
	vec3_t	mins, maxs;
	float	m;
	float	*v;
	vec3_t	front[64], back[64];
	int		f, b;
	float	dist[64];
	float	frac;
	glpoly_t	*poly;
	float	s, t;

	if (numverts > 60)
		Sys_Error ("numverts = %i", numverts);

	BoundPoly (numverts, verts, mins, maxs);

	for (i = 0; i < 3; i++)
	{
		m = (mins[i] + maxs[i]) * 0.5;
		m = SUBDIVIDE_SIZE * floor (m/SUBDIVIDE_SIZE + 0.5);
		if (maxs[i] - m < 8)
			continue;
		if (m - mins[i] < 8)
			continue;

		// cut it
		v = verts + i;
		for (j = 0; j < numverts; j++, v += 3)
			dist[j] = *v - m;

		// wrap cases
		dist[j] = dist[0];
		v -= i;
		VectorCopy (verts, v);

		f = b = 0;
		v = verts;
		for (j = 0; j < numverts; j++, v += 3)
		{
			if (dist[j] >= 0)
			{
				VectorCopy (v, front[f]);
				f++;
			}
			if (dist[j] <= 0)
			{
				VectorCopy (v, back[b]);
				b++;
			}
			if (dist[j] == 0 || dist[j+1] == 0)
				continue;
			if ( (dist[j] > 0) != (dist[j+1] > 0) )
			{
				// clip point
				frac = dist[j] / (dist[j] - dist[j+1]);
				for (k = 0; k < 3; k++)
					front[f][k] = back[b][k] = v[k] + frac*(v[3+k] - v[k]);
				f++;
				b++;
			}
		}

		SubdividePolygon (f, front[0]);
		SubdividePolygon (b, back[0]);
		return;
	}

	poly = (glpoly_t *) Hunk_AllocName (sizeof(glpoly_t) + (numverts-4) * VERTEXSIZE*sizeof(float), "subdv_poly");
	poly->next = warpface->polys;
	warpface->polys = poly;
	poly->numverts = numverts;
	for (i = 0; i < numverts; i++, verts += 3)
	{
		VectorCopy (verts, poly->verts[i]);
		s = DotProduct (verts, warpface->texinfo->vecs[0]);
		t = DotProduct (verts, warpface->texinfo->vecs[1]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;
	}

	/* Cache the world-space bbox so Sky_ProcessPoly /
	 * R_DrawSkyChain can frustum-cull this poly without re-walking
	 * verts every frame.  Without this, Hunk_AllocName's zero-init
	 * leaves mins == maxs == (0,0,0), and R_CullBox then rejects
	 * the poly whenever the camera isn't at the world origin —
	 * causing the skybox to disappear at most positions. */
	if (numverts > 0)
	{
		int j, k;
		VectorCopy (poly->verts[0], poly->mins);
		VectorCopy (poly->verts[0], poly->maxs);
		for (j = 1; j < numverts; j++)
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
================
GL_SubdivideSurface

Breaks a polygon up along axial 64 unit
boundaries so that turbulent and sky warps
can be done reasonably.
================
*/
void GL_SubdivideSurface (qmodel_t *mod, msurface_t *fa)
{
	vec3_t		verts[64];
	int		numverts;
	int		i;
	int		lindex;
	float		*vec;

	warpface = fa;

	//
	// convert edges back to a normal polygon
	//
	numverts = 0;
	for (i = 0; i < fa->numedges; i++)
	{
		lindex = mod->surfedges[fa->firstedge + i];

		if (lindex > 0)
			vec = mod->vertexes[mod->edges[lindex].v[0]].position;
		else
			vec = mod->vertexes[mod->edges[-lindex].v[1]].position;
		VectorCopy (vec, verts[numverts]);
		numverts++;
	}

	SubdividePolygon (numverts, verts[0]);
}

//=========================================================


// speed up sin calculations - Ed
static float	turbsin[] =
{
#include "gl_warp_sin.h"
};
#define TURBSCALE (256.0 / (2 * M_PI))

/*
=============
EmitWaterPolys

Does a water warp on the pre-fragmented glpoly_t chain
=============
*/
void EmitWaterPolys (msurface_t *fa)
{
	glpoly_t	*p;
	float		*v;
	int		i;
	float		s, t, os, ot;
	float		nz;
	float		ripple = gl_waterripple.value;
	qboolean	use_pixel_warp = (r_water_pixel_warp.integer != 0);
	const glprogram_t *shader = use_pixel_warp ? &gl_shader_turb : &gl_shader_alias;

	if (ripple < 0) ripple = 0;
	else if (ripple > 10) ripple = 10;

	/* When using the per-pixel warp shader, push u_ripple now (not
	 * cached by GL_ImmEnd); u_time is uploaded by the imm path's
	 * uniform cache.  uhexen2-9o7u. */
	if (use_pixel_warp && gl_shader_turb.u_ripple >= 0)
	{
		glUseProgram_fp(gl_shader_turb.program);
		glUniform1f_fp(gl_shader_turb.u_ripple, ripple);
		GL_ImmInvalidateState();
	}

	GL_ImmBegin ();
	for (p = fa->polys ; p ; p = p->next)
	{
		if (p->numverts < 3)
			continue;

		/* Check buffer space: (numverts-2)*3 triangle verts */
		if (GL_ImmCount() + (p->numverts - 2) * 3 >= GL_IMM_MAX_VERTS - 6)
		{
			GL_ImmEnd (GL_TRIANGLES, shader);
			GL_ImmBegin ();
		}

		/* Fan → triangles */
		for (i = 2; i < p->numverts; i++)
		{
			int vi;
			for (vi = 0; vi < 3; vi++)
			{
				int idx = (vi == 0) ? 0 : (vi == 1) ? i - 1 : i;
				v = p->verts[idx];

				if (use_pixel_warp)
				{
					/* Send raw texel-space UV (v[3]=os, v[4]=ot)
					 * and unwarped position — gl_shader_turb does
					 * the warp + Z ripple in the shader. */
					GL_ImmTexCoord2f (v[3], v[4]);
					GL_ImmVertex3f (v[0], v[1], v[2]);
				}
				else
				{
					/* Legacy CPU per-vertex warp: shift UV via
					 * 256-entry sin lookup and displace Z by the
					 * ripple sin. */
					os = v[3];
					ot = v[4];

					nz = v[2];
					if (ripple > 0)
						nz += ripple * sin(v[0]*0.05 + realtime) * sin(v[2]*0.05 + realtime);

					s = os + turbsin[(int)((ot*0.125 + realtime) * TURBSCALE) & 255];
					s *= (1.0/64);
					t = ot + turbsin[(int)((os*0.125 + realtime) * TURBSCALE) & 255];
					t *= (1.0/64);

					GL_ImmTexCoord2f (s, t);
					GL_ImmVertex3f (v[0], v[1], nz);
				}
			}
		}
	}
	GL_ImmEnd (GL_TRIANGLES, shader);
}


/*
=============
EmitSkyPolys
=============
*/
static void EmitSkyPolysMulti (msurface_t *fa)
{
	extern cvar_t r_skyspeed_back, r_skyspeed_front;
	glpoly_t	*p;
	float		*v;
	int		i;
	float	s, ss, t, tt;
	vec3_t		dir;
	float		length;
	float		alpha = CLAMP(0.0f, r_skyalpha.value, 1.0f);
	float		bspeed = r_skyspeed_back.value;
	float		fspeed = r_skyspeed_front.value;

	if (alpha >= 1.0f)
	{
		/* single-pass multitexture: blend both layers in the shader */
		GL_SetAlphaThreshold (0.0f);	/* two-layer sky mode */
		GL_Bind (solidskytexture);

		glActiveTextureARB_fp (GL_TEXTURE1_ARB);
		GL_Bind (alphaskytexture);
		glActiveTextureARB_fp (GL_TEXTURE0_ARB);

		for (p = fa->polys ; p ; p = p->next)
		{
			GL_ImmBegin ();
			GL_ImmColor4f(1.0f, 1.0f, 1.0f, 1.0f);
			for (i = 0, v = p->verts[0]; i < p->numverts; i++, v += VERTEXSIZE)
			{
				VectorSubtract (v, r_origin, dir);
				dir[2] *= 3;

				length = dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2];
				length = sqrt (length);
				length = 6*63/length;

				dir[0] *= length;
				dir[1] *= length;

				s = (realtime*bspeed + dir[0]) * (1.0/128);
				t = (realtime*bspeed + dir[1]) * (1.0/128);

				ss = (realtime*fspeed + dir[0]) * (1.0/128);
				tt = (realtime*fspeed + dir[1]) * (1.0/128);

				GL_ImmTexCoord2f (s, t);
				GL_ImmLMCoord2f (ss, tt);
				GL_ImmVertex3f (v[0], v[1], v[2]);
			}
			GL_ImmEnd (GL_POLYGON, &gl_shader_sky);
		}
	}
	else
	{
		/* two-pass: draw back layer opaque, then front layer blended
		 * with r_skyalpha controlling the front layer's opacity */
		GL_SetAlphaThreshold (1.0f);	/* single-texture mode */

		/* pass 1: solid (back) layer, fully opaque */
		GL_Bind (solidskytexture);

		for (p = fa->polys ; p ; p = p->next)
		{
			GL_ImmBegin ();
			GL_ImmColor3f(1.0f, 1.0f, 1.0f);
			for (i = 0, v = p->verts[0]; i < p->numverts; i++, v += VERTEXSIZE)
			{
				VectorSubtract (v, r_origin, dir);
				dir[2] *= 3;

				length = dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2];
				length = sqrt (length);
				length = 6*63/length;

				dir[0] *= length;
				dir[1] *= length;

				s = (realtime*bspeed + dir[0]) * (1.0/128);
				t = (realtime*bspeed + dir[1]) * (1.0/128);

				GL_ImmTexCoord2f (s, t);
				GL_ImmVertex3f (v[0], v[1], v[2]);
			}
			GL_ImmEnd (GL_POLYGON, &gl_shader_sky);
		}

		/* pass 2: alpha (front) layer, blended at r_skyalpha */
		GL_Bind (alphaskytexture);
		glEnable_fp(GL_BLEND);

		for (p = fa->polys ; p ; p = p->next)
		{
			GL_ImmBegin ();
			GL_ImmColor4f(1.0f, 1.0f, 1.0f, alpha);
			for (i = 0, v = p->verts[0]; i < p->numverts; i++, v += VERTEXSIZE)
			{
				VectorSubtract (v, r_origin, dir);
				dir[2] *= 3;

				length = dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2];
				length = sqrt (length);
				length = 6*63/length;

				dir[0] *= length;
				dir[1] *= length;

				ss = (realtime*fspeed + dir[0]) * (1.0/128);
				tt = (realtime*fspeed + dir[1]) * (1.0/128);

				GL_ImmTexCoord2f (ss, tt);
				GL_ImmVertex3f (v[0], v[1], v[2]);
			}
			GL_ImmEnd (GL_POLYGON, &gl_shader_sky);
		}

		glDisable_fp(GL_BLEND);
	}
}

/*
===============
EmitBothSkyLayers

Does a sky warp on the pre-fragmented glpoly_t chain
This will be called for brushmodels, the world
will have them chained together.
===============
*/
void EmitBothSkyLayers (msurface_t *fa)
{
	EmitSkyPolysMulti (fa);
}

#ifndef QUAKE2
extern qboolean R_CullBox (vec3_t mins, vec3_t maxs);

/*
=================
R_DrawSkyChain

Batched sky-chain renderer.  Walks every visible sky surface and
triangulates each glpoly_t fan into a single shared GL_TRIANGLES
batch.  This replaces the old per-poly GL_ImmBegin/GL_ImmEnd loop
which was costing ~12us per cycle on Mesa Iris Xe — with 2256 sky
polys on Coliseum-scale arenas that path was burning ~28ms per
frame just on driver state validation.

The warp UVs are computed inline (same formula as the legacy
EmitSkyPolysMulti); fan vertex 0 is recomputed for each emitted
triangle, but the math is a sqrt + a few multiplies and is dwarfed
by the GL submission savings.

For r_skyalpha < 1 we still take the two-pass legacy path because
batching across two render passes with different blend states is
not a clean refactor and r_skyalpha defaults to 1.
=================
*/
/* Inline sky-warp UV computation.  The macro form is used because we
 * fan-triangulate inline (vertex 0 of each poly is recomputed per
 * triangle) and the math is cheap enough that 3x redundancy is
 * dwarfed by what we save in GL submission overhead. */
#define SKYWARP_DIR(_vp, _dir) do { \
		(_dir)[0] = (_vp)[0] - r_origin[0]; \
		(_dir)[1] = (_vp)[1] - r_origin[1]; \
		(_dir)[2] = ((_vp)[2] - r_origin[2]) * 3.0f; \
		float _len = sqrt((_dir)[0]*(_dir)[0] + (_dir)[1]*(_dir)[1] + (_dir)[2]*(_dir)[2]); \
		_len = 6.0f * 63.0f / _len; \
		(_dir)[0] *= _len; \
		(_dir)[1] *= _len; \
	} while (0)

#define SKYWARP_VERT_MULTI(_v) do { \
		float *_vp = (_v); \
		float _dir[3]; \
		SKYWARP_DIR(_vp, _dir); \
		GL_ImmTexCoord2f((realtime*bspeed + _dir[0]) * (1.0f/128.0f), \
				 (realtime*bspeed + _dir[1]) * (1.0f/128.0f)); \
		GL_ImmLMCoord2f ((realtime*fspeed + _dir[0]) * (1.0f/128.0f), \
				 (realtime*fspeed + _dir[1]) * (1.0f/128.0f)); \
		GL_ImmVertex3f  (_vp[0], _vp[1], _vp[2]); \
	} while (0)

#define SKYWARP_VERT_BACK(_v) do { \
		float *_vp = (_v); \
		float _dir[3]; \
		SKYWARP_DIR(_vp, _dir); \
		GL_ImmTexCoord2f((realtime*bspeed + _dir[0]) * (1.0f/128.0f), \
				 (realtime*bspeed + _dir[1]) * (1.0f/128.0f)); \
		GL_ImmVertex3f  (_vp[0], _vp[1], _vp[2]); \
	} while (0)

#define SKYWARP_VERT_FRONT(_v) do { \
		float *_vp = (_v); \
		float _dir[3]; \
		SKYWARP_DIR(_vp, _dir); \
		GL_ImmTexCoord2f((realtime*fspeed + _dir[0]) * (1.0f/128.0f), \
				 (realtime*fspeed + _dir[1]) * (1.0f/128.0f)); \
		GL_ImmVertex3f  (_vp[0], _vp[1], _vp[2]); \
	} while (0)

/*
=================
R_DrawSkyChain

Batched sky-chain renderer.  Walks every visible sky surface and
triangulates each glpoly_t fan into one shared GL_TRIANGLES batch
per pass — replaces the old per-poly GL_ImmBegin/GL_ImmEnd loop
which cost ~12us per cycle on Mesa Iris Xe (with 2256 sky polys
on Coliseum-scale arenas, that path was burning ~28ms per frame
on driver state validation alone).

For r_skyalpha >= 1: single multitexture pass (back+front blended
in shader).  For r_skyalpha < 1 (default 0.67): two passes — solid
back layer then alpha-blended front layer.  Both modes batch the
whole chain into 1 (or 2 with overflow) GL_TRIANGLES draws per
pass.
=================
*/
void R_DrawSkyChain (msurface_t *s)
{
	extern cvar_t r_skyspeed_back, r_skyspeed_front;
	msurface_t	*fa;
	glpoly_t	*p;
	int		j;
	float		alpha = CLAMP(0.0f, r_skyalpha.value, 1.0f);
	float		bspeed = r_skyspeed_back.value;
	float		fspeed = r_skyspeed_front.value;

	/* Per-surface bbox cull: skip surfaces fully outside the view. */
#define EMIT_LOOP(VERT_MACRO, COLOR_SETUP) \
	do { \
		GL_ImmBegin (); \
		COLOR_SETUP; \
		for (fa = s ; fa ; fa = fa->texturechain) \
		{ \
			/* Per-surface bbox = union of cached per-poly bboxes
			 * (computed once in BuildSurfaceDisplayList). */ \
			vec3_t _mins, _maxs; \
			int _has_bbox = 0; \
			for (p = fa->polys ; p ; p = p->next) \
			{ \
				int _k; \
				if (p->numverts < 3) continue; \
				if (!_has_bbox) { \
					VectorCopy (p->mins, _mins); \
					VectorCopy (p->maxs, _maxs); \
					_has_bbox = 1; \
					continue; \
				} \
				for (_k = 0; _k < 3; _k++) { \
					if (p->mins[_k] < _mins[_k]) _mins[_k] = p->mins[_k]; \
					if (p->maxs[_k] > _maxs[_k]) _maxs[_k] = p->maxs[_k]; \
				} \
			} \
			if (_has_bbox && R_CullBox (_mins, _maxs)) \
				continue; \
			for (p = fa->polys ; p ; p = p->next) \
			{ \
				if (p->numverts < 3) continue; \
				if (GL_ImmCount() + (p->numverts - 2) * 3 >= GL_IMM_MAX_VERTS - 6) { \
					GL_ImmEnd (GL_TRIANGLES, &gl_shader_sky); \
					GL_ImmBegin (); \
					COLOR_SETUP; \
				} \
				for (j = 2; j < p->numverts; j++) { \
					VERT_MACRO (p->verts[0]); \
					VERT_MACRO (p->verts[j - 1]); \
					VERT_MACRO (p->verts[j]); \
				} \
			} \
		} \
		GL_ImmEnd (GL_TRIANGLES, &gl_shader_sky); \
	} while (0)

	if (alpha >= 1.0f)
	{
		/* single-pass: shader blends both layers using TU0/TU1 */
		GL_SetAlphaThreshold (0.0f);
		GL_Bind (solidskytexture);
		glActiveTextureARB_fp (GL_TEXTURE1_ARB);
		GL_Bind (alphaskytexture);
		glActiveTextureARB_fp (GL_TEXTURE0_ARB);
		EMIT_LOOP (SKYWARP_VERT_MULTI, GL_ImmColor4f(1.0f, 1.0f, 1.0f, 1.0f));
	}
	else
	{
		/* two-pass: opaque back, then alpha-blended front */
		GL_SetAlphaThreshold (1.0f);

		/* pass 1: back layer, opaque */
		GL_Bind (solidskytexture);
		EMIT_LOOP (SKYWARP_VERT_BACK, GL_ImmColor3f(1.0f, 1.0f, 1.0f));

		/* pass 2: front layer, blended at r_skyalpha */
		GL_Bind (alphaskytexture);
		glEnable_fp (GL_BLEND);
		EMIT_LOOP (SKYWARP_VERT_FRONT, GL_ImmColor4f(1.0f, 1.0f, 1.0f, alpha));
		glDisable_fp (GL_BLEND);
	}

#undef EMIT_LOOP
}

#undef SKYWARP_VERT_MULTI
#undef SKYWARP_VERT_BACK
#undef SKYWARP_VERT_FRONT
#undef SKYWARP_DIR

#endif


/*
=================================================================
Quake 2 environment sky
=================================================================
*/

#ifdef QUAKE2
static GLuint	sky_tex[6];
/*
=================================================================

PCX Loading

=================================================================
*/

typedef struct
{
	char	manufacturer;
	char	version;
	char	encoding;
	char	bits_per_pixel;
	unsigned short	xmin,ymin,xmax,ymax;
	unsigned short	hres,vres;
	unsigned char	palette[48];
	char	reserved;
	char	color_planes;
	unsigned short	bytes_per_line;
	unsigned short	palette_type;
	char	filler[58];
	unsigned int	data;	// unbounded
} pcx_t;

static byte	*pcx_rgb;

/*
============
LoadPCX
============
*/
void LoadPCX (FILE *f)
{
	pcx_t	*pcx, pcxbuf;
	byte	palette[768];
	byte	*pix;
	int		x, y;
	int		dataByte, runLength;
	int		count;

//
// parse the PCX file
//
	fread (&pcxbuf, 1, sizeof(pcxbuf), f);

	pcx = &pcxbuf;

	if (pcx->manufacturer != 0x0a
		|| pcx->version != 5
		|| pcx->encoding != 1
		|| pcx->bits_per_pixel != 8
		|| pcx->xmax >= 320
		|| pcx->ymax >= 256)
	{
		Con_Printf ("Bad pcx file\n");
		return;
	}

	// seek to palette
	fseek (f, -768, SEEK_END);
	fread (palette, 1, 768, f);

	fseek (f, sizeof(pcxbuf) - 4, SEEK_SET);

	count = (pcx->xmax + 1) * (pcx->ymax + 1);
	pcx_rgb = Hunk_AllocName(count * 4, "pcxfile_data");

	for (y = 0 ; y <= pcx->ymax ; y++)
	{
		pix = pcx_rgb + 4*y*(pcx->xmax + 1);
		for (x = 0 ; x <= pcx->ymax ; )
		{
			dataByte = fgetc(f);

			if ((dataByte & 0xC0) == 0xC0)
			{
				runLength = dataByte & 0x3F;
				dataByte = fgetc(f);
			}
			else
				runLength = 1;

			while (runLength-- > 0)
			{
				pix[0] = palette[dataByte*3];
				pix[1] = palette[dataByte*3+1];
				pix[2] = palette[dataByte*3+2];
				pix[3] = 255;
				pix += 4;
				x++;
			}
		}
	}
}

/*
=========================================================

TARGA LOADING

=========================================================
*/

typedef struct _TargaHeader
{
	unsigned char	id_length, colormap_type, image_type;
	unsigned short	colormap_index, colormap_length;
	unsigned char	colormap_size;
	unsigned short	x_origin, y_origin, width, height;
	unsigned char	pixel_size, attributes;
} TargaHeader;

static TargaHeader	targa_header;
static byte		*targa_rgba;

int fgetLittleShort (FILE *f)
{
	byte	b1, b2;

	b1 = fgetc(f);
	b2 = fgetc(f);

	return (short)(b1 + b2*256);
}

int fgetLittleLong (FILE *f)
{
	byte	b1, b2, b3, b4;

	b1 = fgetc(f);
	b2 = fgetc(f);
	b3 = fgetc(f);
	b4 = fgetc(f);

	return b1 + (b2<<8) + (b3<<16) + (b4<<24);
}


/*
=============
LoadTGA
=============
*/
void LoadTGA (FILE *fin)
{
	int		columns, rows, numPixels;
	byte	*pixbuf;
	int		row, column;

	targa_header.id_length = fgetc(fin);
	targa_header.colormap_type = fgetc(fin);
	targa_header.image_type = fgetc(fin);

	targa_header.colormap_index = fgetLittleShort(fin);
	targa_header.colormap_length = fgetLittleShort(fin);
	targa_header.colormap_size = fgetc(fin);
	targa_header.x_origin = fgetLittleShort(fin);
	targa_header.y_origin = fgetLittleShort(fin);
	targa_header.width = fgetLittleShort(fin);
	targa_header.height = fgetLittleShort(fin);
	targa_header.pixel_size = fgetc(fin);
	targa_header.attributes = fgetc(fin);

	if (targa_header.image_type != 2 && targa_header.image_type != 10)
		Sys_Error ("%s: Only type 2 and 10 targa RGB images supported", __thisfunc__);

	if ((targa_header.pixel_size != 32 && targa_header.pixel_size != 24) ||
			targa_header.colormap_type !=0)
		Sys_Error ("%s: Only 32 or 24 bit images supported (no colormaps)", __thisfunc__);

	columns = targa_header.width;
	rows = targa_header.height;
	numPixels = columns * rows;

	targa_rgba = Hunk_AllocName(numPixels * 4, "tgafile_data");

	if (targa_header.id_length != 0)	// skip TARGA image comment
		fseek(fin, targa_header.id_length, SEEK_CUR);

	if (targa_header.image_type == 2)
	{
	// Uncompressed, RGB images
		for (row = rows-1; row >= 0; row--)
		{
			pixbuf = targa_rgba + row*columns*4;
			for (column = 0; column < columns; column++)
			{
				unsigned char	red, green, blue, alphabyte;
				switch (targa_header.pixel_size)
				{
				case 24:
					blue = getc(fin);
					green = getc(fin);
					red = getc(fin);
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = 255;
					break;
				case 32:
					blue = getc(fin);
					green = getc(fin);
					red = getc(fin);
					alphabyte = getc(fin);
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = alphabyte;
					break;
				}
			}
		}
	}
	else if (targa_header.image_type == 10)
	{
	// Runlength encoded RGB images
		unsigned char	red, green, blue, alphabyte;
		unsigned char	packetHeader, packetSize, j;
		for (row = rows-1; row >= 0; row--)
		{
			pixbuf = targa_rgba + row*columns*4;
			for (column = 0 ; column < columns ; )
			{
				packetHeader = getc(fin);
				packetSize = 1 + (packetHeader & 0x7f);
				if (packetHeader & 0x80)
				{	// run-length packet
					switch (targa_header.pixel_size)
					{
					case 24:
						blue = getc(fin);
						green = getc(fin);
						red = getc(fin);
						alphabyte = 255;
						break;
					case 32:
						blue = getc(fin);
						green = getc(fin);
						red = getc(fin);
						alphabyte = getc(fin);
						break;
					}

					for (j = 0; j < packetSize; j++)
					{
						*pixbuf++ = red;
						*pixbuf++ = green;
						*pixbuf++ = blue;
						*pixbuf++ = alphabyte;
						column++;
						if (column == columns)
						{	// run spans across rows
							column = 0;
							if (row > 0)
								row--;
							else
								goto breakOut;
							pixbuf = targa_rgba + row*columns*4;
						}
					}
				}
				else
				{	// non run-length packet
					for (j = 0; j < packetSize; j++)
					{
						switch (targa_header.pixel_size)
						{
						case 24:
							blue = getc(fin);
							green = getc(fin);
							red = getc(fin);
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = 255;
							break;
						case 32:
							blue = getc(fin);
							green = getc(fin);
							red = getc(fin);
							alphabyte = getc(fin);
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = alphabyte;
							break;
						}
						column++;
						if (column == columns)
						{	// pixel packet run spans across rows
							column = 0;
							if (row > 0)
								row--;
							else
								goto breakOut;
							pixbuf = targa_rgba + row*columns*4;
						}
					}
				}
			}
			breakOut:;
		}
	}

	fclose(fin);
}


/*
==================
R_LoadSkys
==================
*/

static char	*suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};

void R_LoadSkys (void)
{
	int	i, mark;
	FILE	*f;
	char	name[64], texname[20];

	for (i = 0; i < 6; i++)
	{
		q_snprintf (name, sizeof(name), "gfx/env/bkgtst%s.tga", suf[i]);
		FS_OpenFile (name, &f, NULL);
		if (!f)
		{
			Con_Printf ("Couldn't load %s\n", name);
			continue;
		}

		mark = Hunk_LowMark();
		LoadTGA (f);
	//	LoadPCX (f);

		q_snprintf(texname, sizeof(texname), "skybox%i", i);
		sky_tex[i] = GL_LoadTexture(texname, targa_rgba, 256, 256, TEX_RGBA|TEX_LINEAR);
		Hunk_FreeToLowMark(mark);
	}
}


static vec3_t	skyclip[6] =
{
	{  1,  1,  0 },
	{  1, -1,  0 },
	{  0, -1,  1 },
	{  0,  1,  1 },
	{  1,  0,  1 },
	{ -1,  0,  1 }
};

//int	c_sky;

// 1 = s, 2 = t, 3 = 2048
static int	st_to_vec[6][3] =
{
	{  3, -1,  2 },
	{ -3,  1,  2 },

	{  1,  3,  2 },
	{ -1, -3,  2 },

	{ -2, -1,  3 },	// 0 degrees yaw, look straight up
	{  2, -1, -3 }	// look straight down

//	{ -1,  2,  3 },
//	{  1,  2, -3 }
};

// s = [0]/[2], t = [1]/[2]
static int	vec_to_st[6][3] =
{
	{ -2,  3,  1 },
	{  2,  3, -1 },

	{  1,  3,  2 },
	{ -1,  3, -2 },

	{ -2, -1,  3 },
	{ -2,  1, -3 }

//	{ -1,  2,  3 },
//	{  1,  2, -3 }
};

static float	skymins[2][6], skymaxs[2][6];

static void DrawSkyPolygon (int nump, vec3_t vecs)
{
	int		i, j;
	vec3_t	v, av;
	float	s, t, dv;
	int		axis;
	float	*vp;

//	c_sky++;
#if 0
	GL_ImmBegin ();
	for (i = 0; i < nump; i++, vecs += 3)
	{
		VectorAdd(vecs, r_origin, v);
		GL_ImmVertex3f (v[0], v[1], v[2]);
	}
	GL_ImmEnd (GL_POLYGON, &gl_shader_world);
	return;
#endif
	// decide which face it maps to
	VectorClear (v);
	for (i = 0, vp = vecs; i < nump; i++, vp += 3)
	{
		VectorAdd (vp, v, v);
	}
	av[0] = fabs(v[0]);
	av[1] = fabs(v[1]);
	av[2] = fabs(v[2]);
	if (av[0] > av[1] && av[0] > av[2])
	{
		if (v[0] < 0)
			axis = 1;
		else
			axis = 0;
	}
	else if (av[1] > av[2] && av[1] > av[0])
	{
		if (v[1] < 0)
			axis = 3;
		else
			axis = 2;
	}
	else
	{
		if (v[2] < 0)
			axis = 5;
		else
			axis = 4;
	}

	// project new texture coords
	for (i = 0; i < nump; i++, vecs += 3)
	{
		j = vec_to_st[axis][2];
		if (j > 0)
			dv = vecs[j - 1];
		else
			dv = -vecs[-j - 1];

		j = vec_to_st[axis][0];
		if (j < 0)
			s = -vecs[-j -1] / dv;
		else
			s = vecs[j-1] / dv;

		j = vec_to_st[axis][1];
		if (j < 0)
			t = -vecs[-j -1] / dv;
		else
			t = vecs[j-1] / dv;

		if (s < skymins[0][axis])
			skymins[0][axis] = s;
		if (t < skymins[1][axis])
			skymins[1][axis] = t;
		if (s > skymaxs[0][axis])
			skymaxs[0][axis] = s;
		if (t > skymaxs[1][axis])
			skymaxs[1][axis] = t;
	}
}

#define	MAX_CLIP_VERTS	64
static void ClipSkyPolygon (int nump, vec3_t vecs, int stage)
{
	float	*norm;
	float	*v;
	qboolean	front, back;
	float	d, e;
	float	dists[MAX_CLIP_VERTS];
	int		sides[MAX_CLIP_VERTS];
	vec3_t	newv[2][MAX_CLIP_VERTS];
	int		newc[2];
	int		i, j;

	if (nump > MAX_CLIP_VERTS-2)
		Sys_Error ("%s: MAX_CLIP_VERTS", __thisfunc__);
	if (stage == 6)
	{	// fully clipped, so draw it
		DrawSkyPolygon (nump, vecs);
		return;
	}

	front = back = false;
	norm = skyclip[stage];
	for (i = 0, v = vecs; i < nump; i++, v += 3)
	{
		d = DotProduct (v, norm);
		if (d > ON_EPSILON)
		{
			front = true;
			sides[i] = SIDE_FRONT;
		}
		else if (d < ON_EPSILON)
		{
			back = true;
			sides[i] = SIDE_BACK;
		}
		else
			sides[i] = SIDE_ON;
		dists[i] = d;
	}

	if (!front || !back)
	{	// not clipped
		ClipSkyPolygon (nump, vecs, stage+1);
		return;
	}

	// clip it
	sides[i] = sides[0];
	dists[i] = dists[0];
	VectorCopy (vecs, (vecs+(i*3)) );
	newc[0] = newc[1] = 0;

	for (i = 0, v = vecs; i < nump; i++, v += 3)
	{
		switch (sides[i])
		{
		case SIDE_FRONT:
			VectorCopy (v, newv[0][newc[0]]);
			newc[0]++;
			break;
		case SIDE_BACK:
			VectorCopy (v, newv[1][newc[1]]);
			newc[1]++;
			break;
		case SIDE_ON:
			VectorCopy (v, newv[0][newc[0]]);
			newc[0]++;
			VectorCopy (v, newv[1][newc[1]]);
			newc[1]++;
			break;
		}

		if (sides[i] == SIDE_ON || sides[i+1] == SIDE_ON || sides[i+1] == sides[i])
			continue;

		d = dists[i] / (dists[i] - dists[i+1]);
		for (j = 0; j < 3; j++)
		{
			e = v[j] + d*(v[j+3] - v[j]);
			newv[0][newc[0]][j] = e;
			newv[1][newc[1]][j] = e;
		}
		newc[0]++;
		newc[1]++;
	}

	// continue
	ClipSkyPolygon (newc[0], newv[0][0], stage+1);
	ClipSkyPolygon (newc[1], newv[1][0], stage+1);
}

/*
=================
R_DrawSkyChain
=================
*/
extern qboolean R_CullBox (vec3_t mins, vec3_t maxs);

void R_DrawSkyChain (msurface_t *s)
{
	msurface_t	*fa;
	int		i, j;
	vec3_t	verts[MAX_CLIP_VERTS];
	vec3_t	mins, maxs;
	glpoly_t	*p;
	float		*v0;

//	c_sky = 0;
	GL_Bind(solidskytexture);

	// calculate vertex values for sky box
	for (fa = s ; fa ; fa = fa->texturechain)
	{
		for (p = fa->polys ; p ; p = p->next)
		{
			if (p->numverts < 3)
				continue;

			/* Frustum-cull the poly's world-space bbox before
			 * the recursive 6-plane ClipSkyPolygon walk. On big
			 * arena maps with thousands of sky polys (Coliseum
			 * of War: 2256), most are off-screen each frame and
			 * don't contribute to skybox face bounds — same
			 * optimization as Sky_ProcessPoly in gl_sky.c. */
			v0 = p->verts[0];
			VectorCopy (v0, mins);
			VectorCopy (v0, maxs);
			for (i = 1; i < p->numverts; i++)
			{
				float *v = p->verts[i];
				for (j = 0; j < 3; j++)
				{
					if (v[j] < mins[j]) mins[j] = v[j];
					if (v[j] > maxs[j]) maxs[j] = v[j];
				}
			}
			if (R_CullBox (mins, maxs))
				continue;

			for (i = 0; i < p->numverts; i++)
			{
				VectorSubtract (p->verts[i], r_origin, verts[i]);
			}
			ClipSkyPolygon (p->numverts, verts[0], 0);
		}
	}
}


/*
==============
R_ClearSkyBox
==============
*/
void R_ClearSkyBox (void)
{
	int		i;

	for (i = 0; i < 6; i++)
	{
		skymins[0][i] = skymins[1][i] = 9999999;	/* FIXME: change these two to FLT_MAX/-FLT_MAX */
		skymaxs[0][i] = skymaxs[1][i] = -9999999;
	}
}


static void MakeSkyVec (float s, float t, int axis)
{
	vec3_t	v, b;
	int		j, k;

	b[0] = s*2048;
	b[1] = t*2048;
	b[2] = 2048;

	for (j = 0; j < 3; j++)
	{
		k = st_to_vec[axis][j];
		if (k < 0)
			v[j] = -b[-k - 1];
		else
			v[j] = b[k - 1];
		v[j] += r_origin[j];
	}

	// avoid bilerp seam
	s = (s + 1)*0.5;
	t = (t + 1)*0.5;

	if (s < 1.0/512)
		s = 1.0/512;
	else if (s > 511.0/512)
		s = 511.0/512;
	if (t < 1.0/512)
		t = 1.0/512;
	else if (t > 511.0/512)
		t = 511.0/512;

	t = 1.0 - t;
	GL_ImmTexCoord2f (s, t);
	GL_ImmVertex3f (v[0], v[1], v[2]);
}

/*
==============
R_DrawSkyBox
==============
*/
static int	skytexorder[6] = {0, 2, 1, 3, 4, 5};

void R_DrawSkyBox (void)
{
	int		i;

#if 0
	glEnable_fp (GL_BLEND);
	GL_ImmColor4f (1,1,1,0.5);
	glDisable_fp (GL_DEPTH_TEST);
#endif
	for (i = 0; i < 6; i++)
	{
		if ( (skymins[0][i] >= skymaxs[0][i]) || (skymins[1][i] >= skymaxs[1][i]) )
			continue;

		GL_Bind(sky_tex[skytexorder[i]]);
#if 0
		skymins[0][i] = -1;
		skymins[1][i] = -1;
		skymaxs[0][i] = 1;
		skymaxs[1][i] = 1;
#endif
		GL_ImmBegin ();
		MakeSkyVec (skymins[0][i], skymins[1][i], i);
		MakeSkyVec (skymins[0][i], skymaxs[1][i], i);
		MakeSkyVec (skymaxs[0][i], skymaxs[1][i], i);
		MakeSkyVec (skymaxs[0][i], skymins[1][i], i);
		GL_ImmEnd (GL_QUADS, &gl_shader_world);
	}
#if 0
	glDisable_fp (GL_BLEND);
	GL_ImmColor4f (1,1,1,0.5);
	glEnable_fp (GL_DEPTH_TEST);
#endif
}

#endif	/* end of Quake2 sky */

//===============================================================


/*
=============
R_InitSky

A sky texture is 256*128, with the right side being a masked overlay
==============
*/
void R_InitSky (texture_t *mt)
{
	int		i, j, p;
	byte		*src;
	unsigned int	trans[128*128];
	unsigned int	transpix;
	int		r, g, b;
	unsigned int	*rgba;

	src = (byte *)mt + mt->offsets[0];

	// make an average value for the back to avoid
	// a fringe on the top level
	r = g = b = 0;
	for (i = 0; i < 128; i++)
	{
		for (j = 0; j < 128; j++)
		{
			p = src[i*256 + j + 128];
			rgba = &d_8to24table[p];
			trans[(i*128) + j] = *rgba;
			r += ((byte *)rgba)[0];
			g += ((byte *)rgba)[1];
			b += ((byte *)rgba)[2];
		}
	}

	((byte *)&transpix)[0] = r / (128*128);
	((byte *)&transpix)[1] = g / (128*128);
	((byte *)&transpix)[2] = b / (128*128);
	((byte *)&transpix)[3] = 0;

	solidskytexture = GL_LoadTexture("upsky", (byte *)trans, 128, 128, TEX_RGBA|TEX_LINEAR);

	for (i = 0; i < 128; i++)
	{
		for (j = 0; j < 128; j++)
		{
			p = src[i*256 + j];
			if (p == 0)
				trans[(i*128) + j] = transpix;
			else
				trans[(i*128) + j] = d_8to24table[p];
		}
	}

	alphaskytexture = GL_LoadTexture("lowsky", (byte *)trans, 128, 128, TEX_ALPHA|TEX_RGBA|TEX_LINEAR);
}
