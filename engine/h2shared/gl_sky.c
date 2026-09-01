/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
//gl_sky.c

#include "quakedef.h"
#include "image.h"
#include "img_load.h"
#include "gl_shader.h"
#include "gl_pipeline.h"
#include "gl_vbo.h"
#include "gl_sky.h"

// Don't include gl_texmgr.h because GL_Bind macro in glquake.h conflicts
// Just declare what we need from it
extern gltexture_t *notexture;
extern gltexture_t *nulltexture;

// Source format enum from gl_texmgr.h
enum srcformat { SRC_INDEXED, SRC_LIGHTMAP, SRC_RGBA, SRC_EXTERNAL };
typedef uintptr_t src_offset_t;

extern gltexture_t *TexMgr_LoadImage (qmodel_t *owner, const char *name, int width, int height,
	enum srcformat format, byte *data, const char *source_file,
	src_offset_t source_offset, unsigned int flags);
extern void TexMgr_FreeTexture (gltexture_t *kill);

// Texture flags from gl_texmgr.h
#define TEXPREF_NONE			0x0000
#define TEXPREF_MIPMAP			0x0001
#define TEXPREF_LINEAR			0x0002
#define TEXPREF_NEAREST			0x0004
#define TEXPREF_ALPHA			0x0008
#define TEXPREF_PAD				0x0010
#define TEXPREF_PERSIST			0x0020
#define TEXPREF_OVERWRITE		0x0040
#define TEXPREF_NOPICMIP		0x0080
#define TEXPREF_FULLBRIGHT		0x0100
#define TEXPREF_NOBRIGHT		0x0200
#define TEXPREF_CONCHARS		0x0400
#define TEXPREF_WARPIMAGE		0x0800
#define TEXPREF_RGBA			0x1000
#define TEXPREF_TRANSPARENT		0x2000

// Multitexture functions from gl_texmgr.h
extern void GL_EnableMultitexture (void);
extern void GL_DisableMultitexture (void);

// Undef the GL_Bind macro so we can use the function from gl_texmgr
#undef GL_Bind

// Forward declaration of GL_Bind function from gl_texmgr
extern void GL_Bind (gltexture_t *texture);
extern qboolean have_stencil;

#define	MAX_CLIP_VERTS 64

// Local repo doesn't have ENTALPHA_ZERO
#define ENTALPHA_ZERO 0

// Local repo doesn't have CLAMP
#define CLAMP(min,val,max) ((val) < (min) ? (min) : ((val) > (max) ? (max) : (val)))

// Local repo doesn't have R_CullModelForEntity, stub it
static qboolean R_CullModelForEntity(entity_t *e)
{
	// Always return false (don't cull) for now
	return false;
}

GLfloat Fog_GetDensity(void);
GLfloat *Fog_GetColor(void);


int	rs_skypolys; //for r_speeds readout
int rs_skypasses; //for r_speeds readout
GLfloat	skyflatcolor[3];
float	skymins[2][6], skymaxs[2][6];

char	skybox_name[32] = ""; //name of current skybox, or "" if no skybox

gltexture_t	*skybox_textures[6];
GLuint	skybox_texnums[6]; // Actual OpenGL texture IDs (TexMgr_LoadImage returns same pointer)
gltexture_t	*solidskytexture, *alphaskytexture;

/* Projection far plane, in world units.  Ironwail's and QuakeSpasm's name for
 * this setting, which is why it lives here: the ported sky code needed it to
 * exist.  It was registered at 2048 and read by nobody for the whole of that
 * time, so typing it did nothing while the real far plane sat hardcoded at
 * 16384 in R_SetupGL.  Now wired up, and defaulted to that same 16384 so the
 * behaviour it used to lie about is the behaviour it now describes.
 *
 * Not to be confused with r_farclip, which is a CPU-side BSP-subtree cull
 * distance and is overwritten per level by protocol (gl_rmisc.c).  This one is
 * the frustum.  uhexen2-li13. */
cvar_t gl_farclip = {"gl_farclip", "16384", CVAR_ARCHIVE};
extern cvar_t r_skyalpha; /* defined in gl_rmain.c */
cvar_t r_fastsky = {"r_fastsky", "0", CVAR_NONE};
cvar_t r_sky_quality = {"r_sky_quality", "12", CVAR_NONE};
cvar_t r_skyfog = {"r_skyfog", "0.5", CVAR_NONE};
cvar_t r_skyspeed_back = {"r_skyspeed_back", "8", CVAR_ARCHIVE};	/* back/solid sky scroll speed (0 = frozen) */
cvar_t r_skyspeed_front = {"r_skyspeed_front", "16", CVAR_ARCHIVE};	/* front/alpha sky scroll speed */
cvar_t r_skybox_speed = {"r_skybox_speed", "0", CVAR_NONE};
cvar_t r_skywind = {"r_skywind", "1", CVAR_ARCHIVE};			/* per-skybox wind speed multiplier (uhexen2-typa) */
/* Draw skyboxes through the cubemap program rather than six 2D quads.  Ships
 * on because it is the one that gets the wind right across face edges; 0 is
 * the escape hatch if a driver mishandles the cubemap.  uhexen2-ctk9. */
cvar_t r_skycubemap = {"r_skycubemap", "1", CVAR_ARCHIVE};

static float sky_box_scroll; // UV scroll offset applied each frame in Sky_DrawSkyBox
/* The same scroll as a yaw angle, for the cubemap path.  One face width is a
 * quarter turn, so a scroll of 1.0 is pi/2.  uhexen2-ctk9. */
float sky_box_rot = 0.0f;

/* Per-skybox wind state, set by Sky_LoadWindCfg from gfx/env/<name>wind.cfg.
   Defaults disable wind (dist=0) so behavior matches pre-typa builds. */
static float skywind_dist   = 0.0f;	/* amplitude, clamped to [-2, 2] (0 = wind off) */
static float skywind_yaw    = 45.0f;	/* horizontal direction, degrees */
static float skywind_period = 30.0f;	/* seconds per oscillation cycle */
/* Parsed, stored and written back out, but NOT used when drawing: our sky
 * scroll is 2D, so pitch has no effect here.  It is kept anyway so that a
 * wind.cfg authored through these commands round-trips losslessly, both
 * through us and through Ironwail, which does use it. */
static float skywind_pitch  = 0.0f;	/* vertical direction, degrees */

/* Per-frame wind UV offset.  GL_ImmFlush pushes it to u_wind on whichever
 * program declares one, which is both sky programs (uhexen2-a5nn.4). */
float sky_wind_uv[2] = { 0.0f, 0.0f };
/* The same wind as an offset applied to the cubemap sample direction, in the
 * shader's swizzled space.  uhexen2-ctk9. */
float sky_wind_vec[3] = { 0.0f, 0.0f, 0.0f };

/* The six faces as a cubemap, built alongside the 2D faces when they all came
 * in at the same square size.  Zero means the skybox draws the old way. */
static GLuint	skybox_cubemap;
static byte	*sky_cube_face[6];	/* RGBA, malloc'd, freed once uploaded */
static int	sky_cube_size;		/* edge length, 0 until a face arrives */
static qboolean	sky_cube_mismatch;	/* a face disagreed; give up on the cubemap */

/* Skybox cache — Ironwail 0603c2bb — elimates stutter when maps precache skyboxes */
typedef struct skybox_s {
	struct skybox_s *next;
	char name[32];
	gltexture_t *textures[6];
	GLuint texnums[6];
	GLuint cubemap;		/* 0 when the faces would not make one (uhexen2-ctk9) */
} skybox_t;

static skybox_t *skybox_cache = NULL;
#define MAX_SKYBOX_CACHE 16

/* Per-map skybox memory.  Mods often set the skybox once on first map
 * entry via a non-BSP mechanism (stuffcmd, QC builtin, info command) that
 * does not re-fire on hub-travel revisit.  On revisit the engine's only
 * source of sky truth is the BSP worldspawn 'sky' key, which most mod
 * maps don't have, so the skybox silently disappeared.  Persist the
 * active skybox keyed on map name so we can re-apply it from Sky_NewMap
 * when worldspawn doesn't supply one.  uhexen2-7gl5. */
typedef struct skymap_s {
	struct skymap_s	*next;
	char	mapname[MAX_QPATH];
	char	skyname[32];
} skymap_t;

static skymap_t	*skybox_per_map = NULL;

int		skytexorder[6] = {0,2,1,3,4,5}; //for skybox

vec3_t	skyclip[6] = {
	{1,1,0},
	{1,-1,0},
	{0,-1,1},
	{0,1,1},
	{1,0,1},
	{-1,0,1}
};

int	st_to_vec[6][3] =
{
	{3,-1,2},
	{-3,1,2},
	{1,3,2},
	{-1,-3,2},
 	{-2,-1,3},		// straight up
 	{2,-1,-3}		// straight down
};

int	vec_to_st[6][3] =
{
	{-2,3,1},
	{2,3,-1},
	{1,3,2},
	{-1,3,-2},
	{-2,-1,3},
	{-2,1,-3}
};

float	skyfog; // ericw

void Fog_DisableGFog(void);
void Fog_EnableGFog(void);

//==============================================================================
//
//  INIT
//
//==============================================================================

/*
=============
Sky_LoadTexture

A sky texture is 256*128, with the left side being a masked overlay
==============
*/
void Sky_LoadTexture (texture_t *mt)
{
	char		texturename[64];
	int			i, j, p, r, g, b, count;
	byte		*src;
	static byte	front_data[128 * 128]; //FIXME: Hunk_Alloc
	static byte	back_data[128 * 128]; //FIXME: Hunk_Alloc
	//unsigned int	transpix;
	unsigned	*rgba;

	/* Release the previous map's scrolling-sky textures so their slots
	 * in the managed_textures[] pool recycle (uhexen2-z7dj).  Without
	 * this, every map load with a scrolling sky leaks two slots — a
	 * long session through several maps would exhaust the pool. */
	if (solidskytexture)
	{
		TexMgr_FreeTexture (solidskytexture);
		solidskytexture = NULL;
	}
	if (alphaskytexture)
	{
		TexMgr_FreeTexture (alphaskytexture);
		alphaskytexture = NULL;
	}

	src = (byte *)mt + mt->offsets[0];

	// extract back layer and upload
	for (i = 0; i < 128; i++)
		for (j = 0; j < 128; j++)
			back_data[(i * 128) + j] = src[i * 256 + j + 128];

	/*
	r = g = b = 0;
	for (i = 0; i < 128; i++)
	{
		for (j = 0; j < 128; j++)
		{
			p = src[i * 256 + j + 128];
			rgba = &d_8to24table[p];
			back_data[(i * 128) + j] = *rgba;
			r += ((byte *)rgba)[0];
			g += ((byte *)rgba)[1];
			b += ((byte *)rgba)[2];
		}
	}

	((byte *)&transpix)[0] = r / (128 * 128);
	((byte *)&transpix)[1] = g / (128 * 128);
	((byte *)&transpix)[2] = b / (128 * 128);
	((byte *)&transpix)[3] = 0;
	*/

	q_snprintf(texturename, sizeof(texturename), "%s:%s_upsky", cl.worldmodel->name, mt->name);
	solidskytexture = TexMgr_LoadImage(cl.worldmodel, "upsky", 128, 128, SRC_INDEXED, back_data, "", (src_offset_t)back_data, TEXPREF_NONE);
	Con_Printf("Sky_LoadTexture: solid=%p texnum=%u, ", solidskytexture, solidskytexture ? solidskytexture->texnum : 0);
	//solidskytexture = TexMgr_LoadImage(cl.worldmodel, WADFILENAME":upsky", 128, 128, SRC_RGBA, back_data, WADFILENAME, 0, TEXPREF_RGBA | TEXPREF_LINEAR);

	//solidskytexture = nulltexture;

	// extract front layer and upload
	for (i = 0; i < 128; i++)
		for (j = 0; j < 128; j++)
		{
			front_data[(i * 128) + j] = src[i * 256 + j];
			if (front_data[(i * 128) + j] == 0)
				front_data[(i * 128) + j] = 255;
		}
	/*
	for (i = 0; i < 128; i++)
	{
		for (j = 0; j < 128; j++)
		{
			p = src[i * 256 + j];
			if (p == 0)
				front_data[(i * 128) + j] = transpix;
			else
				front_data[(i * 128) + j] = d_8to24table[p];
		}
	}
	*/
	q_snprintf(texturename, sizeof(texturename), "%s:%s_lowsky", cl.worldmodel->name, mt->name);
	alphaskytexture = TexMgr_LoadImage(cl.worldmodel, "lowsky", 128, 128, SRC_INDEXED, front_data, "", (src_offset_t)front_data, TEXPREF_ALPHA);
	Con_Printf("alpha=%p texnum=%u\n", alphaskytexture, alphaskytexture ? alphaskytexture->texnum : 0);
	//alphaskytexture = TexMgr_LoadImage(cl.worldmodel, WADFILENAME":lowsky", 128, 128, SRC_RGBA, front_data, WADFILENAME, 0, TEXPREF_ALPHA | TEXPREF_RGBA | TEXPREF_LINEAR);

	//alphaskytexture = notexture;

	// calculate r_fastsky color based on average of all opaque foreground colors
	r = g = b = count = 0;
	for (i = 0; i < 128; i++)
		for (j = 0; j < 128; j++)
		{
			p = src[i * 256 + j];
			if (p != 0)
			{
				rgba = &d_8to24table[p];
				r += ((byte *)rgba)[0];
				g += ((byte *)rgba)[1];
				b += ((byte *)rgba)[2];
				count++;
			}
		}
	skyflatcolor[0] = (float)r / (count * 255);
	skyflatcolor[1] = (float)g / (count * 255);
	skyflatcolor[2] = (float)b / (count * 255);
}

/*
==================
Sky_CacheLookup

Search cache for a skybox by name. Returns ptr if found, NULL otherwise.
==================
*/
static skybox_t *Sky_CacheLookup(const char *name)
{
	skybox_t *entry, *prev = NULL;
	for (entry = skybox_cache; entry; prev = entry, entry = entry->next)
	{
		/* Quake convention is case-insensitive paths; identical skyboxes
		 * typed differently would otherwise cache twice and collide with
		 * the 16-entry eviction.  uhexen2-mxx5.4. */
		if (q_strcasecmp(entry->name, name) == 0)
		{
			/* LRU-bump to head.  The currently-active skybox always
			 * aliases the head entry's textures, so head can never be
			 * Sky_CacheAdd's eviction tail.  Without this, the active
			 * entry could be evicted while skybox_textures[] still
			 * points into it — UAF on the next render frame.
			 * uhexen2-mxx5.2. */
			if (prev)
			{
				prev->next = entry->next;
				entry->next = skybox_cache;
				skybox_cache = entry;
			}
			return entry;
		}
	}
	return NULL;
}

/*
==================
Sky_CacheAdd

Add a skybox to cache. Evicts oldest entry if cache is full.
==================
*/
static skybox_t *Sky_CacheAdd(const char *name, gltexture_t **textures, GLuint *texnums, GLuint cubemap)
{
	skybox_t *entry, *keep, *kill;
	int count, i;

	entry = (skybox_t *)malloc(sizeof(skybox_t));
	if (!entry)
		return NULL;

	q_strlcpy(entry->name, name, sizeof(entry->name));
	memcpy(entry->textures, textures, sizeof(entry->textures));
	memcpy(entry->texnums, texnums, sizeof(entry->texnums));
	entry->cubemap = cubemap;
	entry->next = skybox_cache;
	skybox_cache = entry;

	/* Single-pass eviction: walk forward MAX_SKYBOX_CACHE - 1 steps from the
	 * head we just prepended, then sever and free everything past that.
	 * Replaces the prior two-pass count-then-find-tail with a fragile
	 * 'if (prev && prev != skybox_cache)' guard that silently skipped
	 * eviction when prev happened to match head (uhexen2-mxx5.9). */
	count = 1;
	keep = skybox_cache;
	while (keep->next && count < MAX_SKYBOX_CACHE)
	{
		keep = keep->next;
		count++;
	}
	kill = keep->next;
	keep->next = NULL;
	while (kill)
	{
		skybox_t *next = kill->next;
		/* notexture is a static placeholder, not ours to free.  Stable
		 * for process lifetime, so this comparison is sound. */
		for (i = 0; i < 6; i++)
		{
			if (kill->textures[i] && kill->textures[i] != notexture)
				TexMgr_FreeTexture(kill->textures[i]);
		}
		/* The cache owns the cubemap exactly as it owns the faces, so this
		 * is the only place an evicted one may be deleted (uhexen2-ctk9). */
		if (kill->cubemap)
			glDeleteTextures_fp (1, &kill->cubemap);
		free(kill);
		kill = next;
	}

	return skybox_cache;
}

/*
==================
Sky_CacheFlush

Free entire skybox cache.
==================
*/
void Sky_CacheFlush(void)
{
	skybox_t *entry, *next;
	int i;

	for (entry = skybox_cache; entry; entry = next)
	{
		next = entry->next;
		for (i = 0; i < 6; i++)
		{
			if (entry->textures[i] && entry->textures[i] != notexture)
				TexMgr_FreeTexture(entry->textures[i]);
		}
		if (entry->cubemap)
			glDeleteTextures_fp (1, &entry->cubemap);
		free(entry);
	}
	skybox_cache = NULL;

	/* Reset the active view: skybox_textures[] aliased pointers we just
	 * freed.  Without this, the next render frame derefs dangling memory
	 * and the next Sky_LoadSkyBox(skybox_name) would early-return on the
	 * name match without ever reloading.  uhexen2-mxx5.3. */
	skybox_name[0] = 0;
	skybox_cubemap = 0;	/* aliased an entry we just deleted */
	for (i = 0; i < 6; i++)
	{
		skybox_textures[i] = NULL;
		skybox_texnums[i] = 0;
	}
}

/*
==================
Sky_RememberForMap

Record the active skybox name against the current map so a later
Sky_NewMap revisit can re-apply it when worldspawn doesn't.  Records
the empty string too, so a console `sky ""` disable persists.
uhexen2-7gl5.
==================
*/
static void Sky_RememberForMap (const char *skyname)
{
	skymap_t	*entry;
	const char	*mapname;

	if (!cl.worldmodel || !cl.worldmodel->name[0])
		return;
	mapname = cl.worldmodel->name;
	for (entry = skybox_per_map; entry; entry = entry->next)
	{
		if (q_strcasecmp(entry->mapname, mapname) == 0)
		{
			q_strlcpy(entry->skyname, skyname ? skyname : "", sizeof(entry->skyname));
			return;
		}
	}
	entry = (skymap_t *)malloc(sizeof(skymap_t));
	if (!entry)
		return;
	q_strlcpy(entry->mapname, mapname, sizeof(entry->mapname));
	q_strlcpy(entry->skyname, skyname ? skyname : "", sizeof(entry->skyname));
	entry->next = skybox_per_map;
	skybox_per_map = entry;
}

static const char *Sky_RecallForMap (void)
{
	skymap_t	*entry;

	if (!cl.worldmodel || !cl.worldmodel->name[0])
		return NULL;
	for (entry = skybox_per_map; entry; entry = entry->next)
	{
		if (q_strcasecmp(entry->mapname, cl.worldmodel->name) == 0)
			return entry->skyname;
	}
	return NULL;
}

/*
==============
Sky_ResetCubemapBuild / Sky_StashCubeFace / Sky_BuildCubemap

The skybox as a single cubemap, assembled from the same six images the 2D
faces come from.  uhexen2-ctk9.

WHY: r_skywind animates a skybox by sliding a UV offset across six independent
2D faces (uhexen2-typa), and a 2D slide cannot stay continuous across a cube
edge -- the offset that is correct on +X is not the same offset on +Y, so the
seams pull apart the moment the wind is non-zero.  Sampling one cubemap by
direction has no seams to pull apart.  It also lets skywind_pitch mean
something: it has been parsed and clamped since uhexen2-typa and then dropped
on the floor, because a 2D offset has nowhere to put a vertical component.

FACE ORDER is Ironwail's cubemap_order (Quake/gl_sky.c), which is valid here
because our st_to_vec puts the faces on the same world axes theirs does:
rt/lf on +X/-X, bk/ft on +Y/-Y, up/dn on +Z/-Z.  No rotation or flip -- the
straight copy upstream does is right for us too, and the shader's axis swizzle
absorbs the rest.

Built only when all six faces arrived at the same square size, which is the
same condition upstream imposes; anything else keeps the 2D path.
==============
*/
/* Drops the active handle and any half-collected staging pixels.  Does NOT
 * delete the texture: the skybox cache owns it, exactly as it owns the six
 * face textures, and deleting here would dangle a cache entry -- the mistake
 * uhexen2-mxx5.2 records for the faces. */
static void Sky_ResetCubemapBuild (void)
{
	int i;

	skybox_cubemap = 0;
	for (i = 0; i < 6; i++)
	{
		free (sky_cube_face[i]);
		sky_cube_face[i] = NULL;
	}
	sky_cube_size = 0;
	sky_cube_mismatch = false;
}

/* Keep a copy of one face's RGBA while the loader still has it.  The loader
 * frees or hunk-drops `data` immediately after uploading the 2D texture, and
 * reading it back off the GPU is not an option on the ES tier. */
static void Sky_StashCubeFace (int face, const byte *rgba, int width, int height)
{
	size_t bytes;

	if (sky_cube_mismatch || !rgba || face < 0 || face > 5)
		return;

	if (width != height)		/* cube faces must be square */
	{
		sky_cube_mismatch = true;
		return;
	}
	if (sky_cube_size == 0)
		sky_cube_size = width;
	else if (width != sky_cube_size)	/* and all the same size */
	{
		sky_cube_mismatch = true;
		return;
	}

	bytes = (size_t)width * (size_t)height * 4;
	free (sky_cube_face[face]);
	sky_cube_face[face] = (byte *) malloc (bytes);
	if (!sky_cube_face[face])
	{
		sky_cube_mismatch = true;
		return;
	}
	memcpy (sky_cube_face[face], rgba, bytes);
}

static void Sky_BuildCubemap (void)
{
	/* GL cube face i takes skybox face cubemap_order[i]: ft bk up dn rt lf. */
	static const int cubemap_order[6] = { 3, 1, 4, 5, 0, 2 };
	int i;

	for (i = 0; i < 6; i++)
		if (!sky_cube_face[i])
			sky_cube_mismatch = true;

	if (sky_cube_mismatch || sky_cube_size <= 0)
	{
		Con_DPrintf ("Sky: no cubemap (faces differ in size or are incomplete), using 2D faces\n");
		goto done;
	}

	glGenTextures_fp (1, &skybox_cubemap);
	glBindTexture_fp (GL_TEXTURE_CUBE_MAP, skybox_cubemap);
	for (i = 0; i < 6; i++)
		glTexImage2D_fp (GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8,
				 sky_cube_size, sky_cube_size, 0,
				 GL_RGBA, GL_UNSIGNED_BYTE, sky_cube_face[cubemap_order[i]]);
	glTexParameterf_fp (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf_fp (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf_fp (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture_fp (GL_TEXTURE_CUBE_MAP, 0);

	Con_DPrintf ("Sky: cubemap built, %dx%d per face\n", sky_cube_size, sky_cube_size);

done:
	/* The pixels have served their purpose either way. */
	for (i = 0; i < 6; i++)
	{
		free (sky_cube_face[i]);
		sky_cube_face[i] = NULL;
	}
}

/*
==================
Sky_LoadSkyBox
==================
*/
const char	*suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};
void Sky_LoadSkyBox (const char *name)
{
	int		i, mark, width, height, namelen;
	char	filename[MAX_OSPATH];
	byte	*data;
	qboolean nonefound = true;
	skybox_t *cached;

	/* Quake-convention case-insensitive match — see uhexen2-mxx5.4. */
	if (q_strcasecmp(skybox_name, name) == 0)
		return; //no change

	/* Check cache first */
	cached = Sky_CacheLookup(name);
	if (cached)
	{
		q_strlcpy(skybox_name, cached->name, sizeof(skybox_name));
		memcpy(skybox_textures, cached->textures, sizeof(skybox_textures));
		memcpy(skybox_texnums, cached->texnums, sizeof(skybox_texnums));
		skybox_cubemap = cached->cubemap;
		Sky_LoadWindCfg(name);
		Sky_RememberForMap(name);
		return;
	}

	/* Clear the active view.  Do NOT free: skybox_textures[] either
	 * aliases pointers owned by a cache entry (set by the Sky_CacheLookup
	 * memcpy above on a hit, or by the prior load's Sky_CacheAdd) or is
	 * already NULL/notexture.  The cache is the sole owner; freeing here
	 * dangled the cache entry and produced UAF on the next cache hit and
	 * a double-free on later eviction/flush.  Today this was latent only
	 * because TexMgr_FreeTexture is a no-op stub — it goes live the
	 * moment that stub is replaced.  uhexen2-mxx5.2. */
	for (i=0; i<6; i++)
	{
		skybox_textures[i] = NULL;
		skybox_texnums[i] = 0;
	}
	/* Same ownership rule as the faces above: the cache owns it, so drop the
	 * handle without deleting.  Also resets the staging state a previous
	 * failed build may have left behind.  uhexen2-ctk9. */
	Sky_ResetCubemapBuild ();

	//turn off skybox if sky is set to ""
	if (name[0] == 0)
	{
		skybox_name[0] = 0;
		Sky_LoadWindCfg ("");
		Sky_RememberForMap ("");
		return;
	}

	namelen = 0;
	while (true)
	{
		if (name[namelen] == 0)
			break;

		namelen++;
	}

	//load textures
	for (i=0; i<6; i++)
	{
		char filepath[MAX_OSPATH];
		qboolean has_alpha;
		int alpha;

		mark = Hunk_LowMark ();

		// Construct base filename without extension
		if (name[namelen-1] == '_')
			q_snprintf (filename, sizeof(filename), "gfx/env/%s%s", name, suf[i]);
		else
			q_snprintf(filename, sizeof(filename), "gfx/env/%s_%s", name, suf[i]);

		// Try external files first (PNG, TGA, PCX)
		// PNG
		q_snprintf(filepath, sizeof(filepath), "%s.png", filename);
		data = IMG_LoadPNG(filepath, &width, &height, &alpha);
		if (data)
		{
			Con_DPrintf("Loaded external skybox: %s (face %d: %s)\n", filepath, i, suf[i]);
			// Use unique name for each face to prevent texture manager from caching
			char texname[128];
			q_snprintf(texname, sizeof(texname), "%s_face%d", filepath, i);
			skybox_textures[i] = TexMgr_LoadImage (cl.worldmodel, texname, width, height, SRC_RGBA, data, filepath, 0, TEXPREF_NONE);
			skybox_texnums[i] = skybox_textures[i]->texnum; // Save texnum before next call overwrites it
			Sky_StashCubeFace (i, data, width, height);
			Con_DPrintf("  -> texture[%d] = %p, texnum = %u\n", i, (void*)skybox_textures[i], skybox_texnums[i]);
			free(data);
			nonefound = false;
		}
		else
		{
			// TGA
			q_snprintf(filepath, sizeof(filepath), "%s.tga", filename);
			data = IMG_LoadTGA(filepath, &width, &height, &alpha);
			if (data)
			{
				Con_DPrintf("Loaded external skybox: %s (face %d: %s)\n", filepath, i, suf[i]);
				// Use unique name for each face to prevent texture manager from caching
				char texname[128];
				q_snprintf(texname, sizeof(texname), "%s_face%d", filepath, i);
				skybox_textures[i] = TexMgr_LoadImage (cl.worldmodel, texname, width, height, SRC_RGBA, data, filepath, 0, TEXPREF_NONE);
				skybox_texnums[i] = skybox_textures[i]->texnum; // Save texnum before next call overwrites it
				Sky_StashCubeFace (i, data, width, height);
				Con_DPrintf("  -> texture[%d] = %p, texnum = %u\n", i, (void*)skybox_textures[i], skybox_texnums[i]);
				free(data);
				nonefound = false;
			}
			else
			{
				// PCX
				q_snprintf(filepath, sizeof(filepath), "%s.pcx", filename);
				data = IMG_LoadPCX(filepath, &width, &height);
				if (data)
				{
					Con_DPrintf("Loaded external skybox: %s (face %d: %s)\n", filepath, i, suf[i]);
					// Use unique name for each face to prevent texture manager from caching
					char texname[128];
					q_snprintf(texname, sizeof(texname), "%s_face%d", filepath, i);
					skybox_textures[i] = TexMgr_LoadImage (cl.worldmodel, texname, width, height, SRC_RGBA, data, filepath, 0, TEXPREF_NONE);
					skybox_texnums[i] = skybox_textures[i]->texnum; // Save texnum before next call overwrites it
					Sky_StashCubeFace (i, data, width, height);
					Con_DPrintf("  -> texture[%d] = %p, texnum = %u\n", i, (void*)skybox_textures[i], skybox_texnums[i]);
					free(data);
					nonefound = false;
				}
				else
				{
					// Fall back to Image_LoadImage for PAK files
					data = Image_LoadImage (filename, &width, &height);
					if (data)
					{
						skybox_textures[i] = TexMgr_LoadImage (cl.worldmodel, filename, width, height, SRC_RGBA, data, filename, 0, TEXPREF_NONE);
						skybox_texnums[i] = skybox_textures[i]->texnum;
						Sky_StashCubeFace (i, data, width, height);
						nonefound = false;
					}
					else
					{
						// Try alternate skies/ path
						if (name[namelen - 1] == '_')
							q_snprintf(filename, sizeof(filename), "skies/%s%s", name, suf[i]);
						else
							q_snprintf(filename, sizeof(filename), "skies/%s_%s", name, suf[i]);

						data = Image_LoadImage(filename, &width, &height);
						if (data)
						{
							skybox_textures[i] = TexMgr_LoadImage(cl.worldmodel, filename, width, height, SRC_RGBA, data, filename, 0, TEXPREF_NONE);
							skybox_texnums[i] = skybox_textures[i]->texnum;
							Sky_StashCubeFace (i, data, width, height);
							nonefound = false;
						}
						else
						{
							Con_Printf("Couldn't load %s\n", filename);
							skybox_textures[i] = notexture;
							skybox_texnums[i] = notexture ? notexture->texnum : 0;
						}
					}
				}
			}
		}
		Hunk_FreeToLowMark (mark);
	}

	if (nonefound) // go back to scrolling sky if skybox is totally missing
	{
		for (i=0; i<6; i++)
		{
			if (skybox_textures[i] && skybox_textures[i] != notexture)
				TexMgr_FreeTexture (skybox_textures[i]);
			skybox_textures[i] = NULL;
			skybox_texnums[i] = 0;
		}
		skybox_name[0] = 0;
		Sky_LoadWindCfg ("");
		Sky_ResetCubemapBuild ();
		return;
	}

	Sky_BuildCubemap ();

	q_strlcpy(skybox_name, name, sizeof(skybox_name));
	Sky_LoadWindCfg (skybox_name);

	/* Add to cache for future loads */
	Sky_CacheAdd(name, skybox_textures, skybox_texnums, skybox_cubemap);

	Sky_RememberForMap(name);
}

/*
==================
Sky_NormalizeWind

Ironwail's ranges, applied on every path that can change these, so a file we
write is one their loader accepts unchanged and vice versa.
==================
*/
static void Sky_NormalizeWind (void)
{
	skywind_dist  = CLAMP(-2.0f, skywind_dist, 2.0f);
	skywind_yaw   = (float) fmod (skywind_yaw, 360.0);
	skywind_pitch = (float) (fmod (skywind_pitch + 90.0, 180.0) - 90.0);
}

/* Every skywind command needs a sky to attach the numbers to. */
static qboolean Sky_WindReady (void)
{
	if (!skybox_name[0])
	{
		Con_Printf ("No skybox loaded\n");
		return false;
	}
	return true;
}

/*
==================
Skywind_f -- print or set the wind parameters live
==================
*/
static void Skywind_f (void)
{
	if (!Sky_WindReady())
		return;

	if (Cmd_Argc() < 2)
	{
		Con_Printf ("usage: %s <distance> [yaw] [period] [pitch]\n"
			    "current values:\n"
			    "   distance is %g\n"
			    "   yaw      is %g\n"
			    "   period   is %g\n"
			    "   pitch    is %g  (stored and saved, but not drawn with)\n",
			    Cmd_Argv(0), skywind_dist, skywind_yaw, skywind_period, skywind_pitch);
		return;
	}

	skywind_dist = (float) atof (Cmd_Argv(1));
	if (Cmd_Argc() >= 3) skywind_yaw    = (float) atof (Cmd_Argv(2));
	if (Cmd_Argc() >= 4) skywind_period = (float) atof (Cmd_Argv(3));
	if (Cmd_Argc() >= 5) skywind_pitch  = (float) atof (Cmd_Argv(4));
	Sky_NormalizeWind ();
}

/*
==================
Skywind_LookDir_f -- take the direction from where the camera is pointing

The point of the whole command set: aim the view the way you want the clouds
to travel, then skywind_save.  The yaw is inverted so the clouds come TOWARDS
the player rather than receding, which is what "looking into the wind" means.
==================
*/
static void Skywind_LookDir_f (void)
{
	if (cls.state != ca_connected || !Sky_WindReady())
		return;

	skywind_yaw   = (float) (cl.viewangles[YAW] + 180.0);
	skywind_pitch = -cl.viewangles[PITCH];

	if (Cmd_Argc() >= 2)
		skywind_period = (float) atof (Cmd_Argv(1));
	else if (skywind_period == 0.0f)
		skywind_period = 30.0f;

	if (Cmd_Argc() >= 3)
		skywind_dist = (float) atof (Cmd_Argv(2));
	else if (skywind_dist == 0.0f)
		skywind_dist = 1.0f;

	Sky_NormalizeWind ();
	Con_Printf ("skywind: dist=%g yaw=%g period=%g pitch=%g\n",
		    skywind_dist, skywind_yaw, skywind_period, skywind_pitch);
}

/*
==================
Skywind_Rotate_f -- nudge the direction without retyping it
==================
*/
static void Skywind_Rotate_f (void)
{
	if (!Sky_WindReady())
		return;

	if (Cmd_Argc() < 2)
	{
		Con_Printf ("usage: %s <yawdelta> [pitchdelta]\n", Cmd_Argv(0));
		return;
	}

	skywind_yaw += (float) atof (Cmd_Argv(1));
	if (Cmd_Argc() >= 3)
		skywind_pitch += (float) atof (Cmd_Argv(2));
	Sky_NormalizeWind ();
	Con_Printf ("skywind: yaw=%g pitch=%g\n", skywind_yaw, skywind_pitch);
}

/*
==================
Skywind_Save_f -- write the current parameters back out

Writes under the USERDIR copy of the gamedir, not the game directory itself:
the latter is frequently read-only (a pak, or a store path), and the userdir
is both writable and on the search path, so Skywind_Load_f finds what this
wrote.  The full path is printed so an author can copy it next to their
skybox when they are happy with it.
==================
*/
static void Skywind_Save_f (void)
{
	char	dir[MAX_OSPATH];
	char	relname[MAX_QPATH];
	char	path[MAX_OSPATH];
	FILE	*f;

	if (!Sky_WindReady())
		return;

	q_strlcpy (dir, FS_MakePath (FS_USERDIR, NULL, "gfx"), sizeof(dir));
	Sys_mkdir (dir, false);
	q_strlcpy (dir, FS_MakePath (FS_USERDIR, NULL, "gfx/env"), sizeof(dir));
	Sys_mkdir (dir, false);

	q_snprintf (relname, sizeof(relname), "gfx/env/%swind.cfg", skybox_name);
	q_strlcpy (path, FS_MakePath (FS_USERDIR, NULL, relname), sizeof(path));

	f = fopen (path, "wt");
	if (!f)
	{
		Con_Printf ("Couldn't write '%s'\n", path);
		return;
	}

	/* Same line shape Ironwail writes and both engines parse. */
	fprintf (f, "// distance yaw period pitch\n"
		    "skywind %g %g %g %g\n",
		 skywind_dist, skywind_yaw, skywind_period, skywind_pitch);
	fclose (f);

	Con_Printf ("Wrote %s\n", path);
}

/*
==================
Skywind_Load_f -- re-read the cfg for the current skybox
==================
*/
static void Skywind_Load_f (void)
{
	if (!Sky_WindReady())
		return;

	Sky_LoadWindCfg (skybox_name);
	Con_Printf ("skywind: dist=%g yaw=%g period=%g pitch=%g\n",
		    skywind_dist, skywind_yaw, skywind_period, skywind_pitch);
}

/*
==================
Sky_LoadWindCfg

Loads gfx/env/<name>wind.cfg if present (Ironwail format):
    skywind <dist> <yaw> <period> <pitch>
- dist:   amplitude, clamped to [-2, 2] (0 = wind off)
- yaw:    horizontal angle, degrees
- period: seconds per oscillation cycle (divided by r_skywind.value)
- pitch:  parsed but ignored — sky scroll is 2D (no vertical component)
Resets to wind-off defaults when no file is present or name is empty.
==================
*/
void Sky_LoadWindCfg (const char *name)
{
	char	filename[MAX_OSPATH];
	byte	*data;
	const char *p;
	int	mark;

	/* defaults — wind disabled, matches pre-typa behavior */
	skywind_dist   = 0.0f;
	skywind_yaw    = 45.0f;
	skywind_period = 30.0f;
	skywind_pitch  = 0.0f;
	sky_wind_uv[0] = sky_wind_uv[1] = 0.0f;
	sky_wind_vec[0] = sky_wind_vec[1] = sky_wind_vec[2] = 0.0f;

	if (!name || !name[0])
		return;

	q_snprintf (filename, sizeof(filename), "gfx/env/%swind.cfg", name);
	mark = Hunk_LowMark ();
	data = FS_LoadHunkFile (filename, NULL);
	if (!data)
		return;

	p = COM_Parse ((const char*)data);
	if (p && q_strcasecmp(com_token, "skywind") == 0)
	{
		if ((p = COM_Parse(p)) != NULL) skywind_dist   = (float)atof(com_token);
		if ((p = COM_Parse(p)) != NULL) skywind_yaw    = (float)atof(com_token);
		if ((p = COM_Parse(p)) != NULL) skywind_period = (float)atof(com_token);
		/* fourth token = pitch: stored for round-tripping, not drawn with */
		if ((p = COM_Parse(p)) != NULL) skywind_pitch  = (float)atof(com_token);
		Sky_NormalizeWind ();
		Con_DPrintf ("Sky wind: dist=%g yaw=%g period=%g pitch=%g (from %s)\n",
			     skywind_dist, skywind_yaw, skywind_period, skywind_pitch, filename);
	}
	else
	{
		Con_DPrintf ("Sky wind: %s missing 'skywind' keyword, ignoring\n", filename);
	}
	Hunk_FreeToLowMark (mark);
}

/*
==================
Sky_UpdateWind

Computes the per-frame wind UV offset from the parsed per-skybox params
and the global r_skywind scalar.  Phase oscillates as a triangle wave
so wind blows one way, then reverses.  Result is stashed in sky_wind_uv
for GL_ImmFlush to push to u_wind on the sky programs.  Called once per
frame from Sky_DrawSky (only path that uses skyboxes).
==================
*/
void Sky_UpdateWind (void)
{
	float	dist, period;
	double	phase;
	float	yaw_rad, sy, cy;

	dist = skywind_dist;
	period = (skywind_period > 0.0f && r_skywind.value > 0.0f)
		 ? skywind_period / r_skywind.value : 0.0f;
	phase = (period > 0.0f) ? (cl.time * 0.5 / period) : 0.5;
	phase -= floor (phase) + 0.5;	/* triangle wave: [-0.5, 0.5) */

	yaw_rad = (float)(skywind_yaw * (M_PI / 180.0));
	sy = sinf (yaw_rad);
	cy = cosf (yaw_rad);
	sky_wind_uv[0] = (float)(dist * cy * phase);
	sky_wind_uv[1] = (float)(dist * sy * phase);

	/* The same wind as a direction offset for the cubemap path (uhexen2-ctk9).
	 *
	 * This is where skywind_pitch finally does something.  It has been parsed,
	 * clamped and written back out since uhexen2-typa, and then dropped --
	 * a 2D UV offset has nowhere to put a vertical component, so the cfg field
	 * round-tripped through the engine without ever reaching the screen.
	 *
	 * Built in world axes and then swizzled the way ssky_cube_vert swizzles
	 * the view direction, so the offset lands in the same space as the vector
	 * it is added to. */
	{
		float	pitch_rad = (float)(skywind_pitch * (M_PI / 180.0));
		float	cp = cosf (pitch_rad);
		float	sp = sinf (pitch_rad);
		float	amt = (float)(dist * phase);
		float	wx = amt * cy * cp;	/* world +X */
		float	wy = amt * sy * cp;	/* world +Y */
		float	wz = amt * sp;		/* world +Z */

		sky_wind_vec[0] = -wy;
		sky_wind_vec[1] =  wz;
		sky_wind_vec[2] =  wx;
	}
}

/*
=================
Sky_NewMap
=================
*/
void Sky_NewMap (void)
{
	char	key[128], value[4096];
	const char	*data;
	int		i;
	qboolean	worldspawn_set_sky = false;
	const char	*recalled;

	//
	// initially no sky
	//
	skybox_name[0] = 0;
	for (i=0; i<6; i++)
	{
		skybox_textures[i] = NULL;
		skybox_texnums[i] = 0;
	}
	skyfog = r_skyfog.value;

	//
	// read worldspawn (this is so ugly, and shouldn't it be done on the server?)
	//
	data = cl.worldmodel->entities;
	if (!data)
		goto recall; //FIXME: how could this possibly ever happen? -- if there's no
	// worldspawn then the sever wouldn't send the loadmap message to the client

	data = COM_Parse(data);
	if (!data) //should never happen
		goto recall;
	if (com_token[0] != '{') //should never happen
		goto recall;
	while (1)
	{
		data = COM_Parse(data);
		if (!data)
			goto recall;
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			q_strlcpy(key, com_token + 1, sizeof(key));
		else
			q_strlcpy(key, com_token, sizeof(key));
		while (key[0] && key[strlen(key)-1] == ' ') // remove trailing spaces
			key[strlen(key)-1] = 0;
		data = COM_Parse(data);
		if (!data)
			goto recall;
		q_strlcpy(value, com_token, sizeof(value));

		if (!strcmp("sky", key))
		{
			Sky_LoadSkyBox(value);
			worldspawn_set_sky = true;
		}

		if (!strcmp("skyfog", key))
			skyfog = atof(value);

#if 1 //also accept non-standard keys
		else if (!strcmp("skyname", key)) //half-life
		{
			Sky_LoadSkyBox(value);
			worldspawn_set_sky = true;
		}
		else if (!strcmp("qlsky", key)) //quake lives
		{
			Sky_LoadSkyBox(value);
			worldspawn_set_sky = true;
		}
#endif
	}

recall:
	/* If worldspawn didn't touch the sky key, restore from per-map
	 * memory.  Handles hub-travel revisit on mod maps that set the sky
	 * via a non-BSP mechanism (stuffcmd, QC builtin, info command) that
	 * fires only on first map entry — Sky_NewMap unconditionally cleared
	 * skybox_name above and the saved-state restore doesn't re-fire the
	 * QC sky setup.  uhexen2-7gl5. */
	if (!worldspawn_set_sky)
	{
		recalled = Sky_RecallForMap();
		if (recalled)
			Sky_LoadSkyBox(recalled);
	}
}

/*
=================
Sky_SkyCommand_f
=================
*/
void Sky_SkyCommand_f (void)
{
	switch (Cmd_Argc())
	{
	case 1:
		Con_Printf("\"sky\" is \"%s\"\n", skybox_name);
		break;
	case 2:
		Sky_LoadSkyBox(Cmd_Argv(1));
		break;
	default:
		Con_Printf("usage: sky <skyname>\n");
	}
}

/*
====================
R_SetSkyfog_f -- ericw
====================
*/
static void R_SetSkyfog_f (cvar_t *var)
{
// clear any skyfog setting from worldspawn
	skyfog = var->value;
}

/*
=============
Sky_Init
=============
*/
void Sky_Init (void)
{
	int		i;

	Cvar_RegisterVariable (&gl_farclip);
	Cvar_RegisterVariable (&r_fastsky);
	Cvar_RegisterVariable (&r_sky_quality);
	Cvar_RegisterVariable (&r_skyfog);
	Cvar_SetCallback (&r_skyfog, R_SetSkyfog_f);
	Cvar_RegisterVariable (&r_skyspeed_back);
	Cvar_RegisterVariable (&r_skyspeed_front);
	Cvar_RegisterVariable (&r_skybox_speed);
	Cvar_RegisterVariable (&r_skywind);
	Cvar_RegisterVariable (&r_skycubemap);

	Cmd_AddCommand ("sky",Sky_SkyCommand_f);
	Cmd_AddCommand ("skywind", Skywind_f);
	Cmd_AddCommand ("skywind_load", Skywind_Load_f);
	Cmd_AddCommand ("skywind_save", Skywind_Save_f);
	Cmd_AddCommand ("skywind_rotate", Skywind_Rotate_f);
	Cmd_AddCommand ("skywind_lookdir", Skywind_LookDir_f);

	for (i=0; i<6; i++)
	{
		skybox_textures[i] = NULL;
		skybox_texnums[i] = 0;
	}
}

//==============================================================================
//
//  PROCESS SKY SURFS
//
//==============================================================================

/*
=================
Sky_ProjectPoly

update sky bounds
=================
*/
void Sky_ProjectPoly (int nump, vec3_t vecs)
{
	int		i,j;
	vec3_t	v, av;
	float	s, t, dv;
	int		axis;
	float	*vp;

	// decide which face it maps to
	VectorCopy (vec3_origin, v);
	for (i=0, vp=vecs ; i<nump ; i++, vp+=3)
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
	for (i=0 ; i<nump ; i++, vecs+=3)
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

/*
=================
Sky_ClipPoly
=================
*/
void Sky_ClipPoly (int nump, vec3_t vecs, int stage)
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
		Sys_Error ("Sky_ClipPoly: MAX_CLIP_VERTS");
	if (stage == 6) // fully clipped
	{
		Sky_ProjectPoly (nump, vecs);
		return;
	}

	front = back = false;
	norm = skyclip[stage];
	for (i=0, v = vecs ; i<nump ; i++, v+=3)
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
		Sky_ClipPoly (nump, vecs, stage+1);
		return;
	}

	// clip it
	sides[i] = sides[0];
	dists[i] = dists[0];
	VectorCopy (vecs, (vecs+(i*3)) );
	newc[0] = newc[1] = 0;

	for (i=0, v = vecs ; i<nump ; i++, v+=3)
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
		for (j=0 ; j<3 ; j++)
		{
			e = v[j] + d*(v[j+3] - v[j]);
			newv[0][newc[0]][j] = e;
			newv[1][newc[1]][j] = e;
		}
		newc[0]++;
		newc[1]++;
	}

	// continue
	Sky_ClipPoly (newc[0], newv[0][0], stage+1);
	Sky_ClipPoly (newc[1], newv[1][0], stage+1);
}

/*
================
Sky_ProcessPoly
================
*/
extern qboolean R_CullBox (vec3_t mins, vec3_t maxs);

void Sky_ProcessPoly (glpoly_t	*p)
{
	int			i;
	vec3_t		verts[MAX_CLIP_VERTS];

	if (!p || p->numverts <= 0 || p->numverts >= MAX_CLIP_VERTS)
		return;

	if (r_fastsky.value)
		return;

	/* World-space bbox is cached on the poly at load time
	 * (BuildSurfaceDisplayList).  Frustum-reject early so the
	 * recursive Sky_ClipPoly walk only runs for polys actually
	 * in front of the camera. */
	if (R_CullBox (p->mins, p->maxs))
		return;

	for (i=0 ; i<p->numverts ; i++)
		VectorSubtract (p->verts[i], r_origin, verts[i]);
	Sky_ClipPoly (p->numverts, verts[0], 0);
}

/*
================
Sky_ProcessTextureChains -- handles sky polys in world model
================
*/
void Sky_ProcessTextureChains (void)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;

	//if (!r_drawworld_cheatsafe)
	//	return;

	//for (i=0 ; i<cl.worldmodel->numtextures ; i++)
	{
		//t = cl.worldmodel->textures[i];
		t = cl.worldmodel->textures[skytexturenum];

		//if (!t || !t->texturechains[chain_world] || !(t->texturechains[chain_world]->flags & SURF_DRAWSKY))
		//if (!t || !t->gltexture || !(t->gltexture->flags & SURF_DRAWSKY))
		//if (!t || !t->gltexture || (t->gltexture->texnum != skytexturenum))
		//	continue;

		//for (s = t->texturechains[chain_world]; s; s = s->texturechain)
		//for (s = cl.worldmodel->surfaces; s; s = s->texturechain)
		//	if (!s->culled)
		//		Sky_ProcessPoly (s->polys);
		//for (s = cl.worldmodel->surfaces; s; s = s->texturechain)
		int blah = 0;
		s = cl.worldmodel->surfaces;
		//for (s = cl.worldmodel->surfaces; s; s++)
		for (i = 0; i < cl.worldmodel->numsurfaces; i++, s++)
		{
			blah++;
			// Local repo doesn't have s->culled member
			if (s->flags & SURF_DRAWSKY)
				Sky_ProcessPoly(s->polys);
		}
	}
}

/*
================
Sky_ProcessEntities -- handles sky polys on brush models
================
*/
void Sky_ProcessEntities (void)
{
	entity_t	*e;
	msurface_t	*s;
	glpoly_t	*p;
	int			i,j,k,mark;
	float		dot;
	qboolean	rotated;
	vec3_t		temp, forward, right, up;

	if (!r_drawentities.value)
		return;

	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		e = cl_visedicts[i];

		if (e->model->type != mod_brush)
			continue;

		/* Cached at load time — most brush submodels (doors,
		 * platforms, decorative props) have zero sky surfaces.
		 * Skip the full nummodelsurfaces walk for them. */
		if (!e->model->has_sky_surf)
			continue;

		if (R_CullModelForEntity(e))
			continue;

		// Local repo doesn't have entity_t->alpha member
		// if (e->alpha == ENTALPHA_ZERO)
		//	continue;

		VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
		if (e->angles[0] || e->angles[1] || e->angles[2])
		{
			rotated = true;
			AngleVectors (e->angles, forward, right, up);
			VectorCopy (modelorg, temp);
			modelorg[0] = DotProduct (temp, forward);
			modelorg[1] = -DotProduct (temp, right);
			modelorg[2] = DotProduct (temp, up);
		}
		else
			rotated = false;

		s = &e->model->surfaces[e->model->firstmodelsurface];

		for (j=0 ; j<e->model->nummodelsurfaces ; j++, s++)
		{
			if (s->flags & SURF_DRAWSKY)
			{
				dot = DotProduct (modelorg, s->plane->normal) - s->plane->dist;
				if (((s->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
					(!(s->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
				{
					//copy the polygon and translate manually, since Sky_ProcessPoly needs it to be in world space
					mark = Hunk_LowMark();
					p = (glpoly_t *) Hunk_Alloc (sizeof(*s->polys)); //FIXME: don't allocate for each poly
					p->numverts = s->polys->numverts;
					for (k=0; k<p->numverts; k++)
					{
						if (rotated)
						{
							p->verts[k][0] = e->origin[0] + s->polys->verts[k][0] * forward[0]
														  - s->polys->verts[k][1] * right[0]
														  + s->polys->verts[k][2] * up[0];
							p->verts[k][1] = e->origin[1] + s->polys->verts[k][0] * forward[1]
														  - s->polys->verts[k][1] * right[1]
														  + s->polys->verts[k][2] * up[1];
							p->verts[k][2] = e->origin[2] + s->polys->verts[k][0] * forward[2]
														  - s->polys->verts[k][1] * right[2]
														  + s->polys->verts[k][2] * up[2];
						}
						else
							VectorAdd(s->polys->verts[k], e->origin, p->verts[k]);
					}
					Sky_ProcessPoly (p);
					Hunk_FreeToLowMark (mark);
				}
			}
		}
	}
}

//==============================================================================
//
//  RENDER SKYBOX
//
//==============================================================================

/*
==============
Sky_EmitSkyBoxVertex
==============
*/
void Sky_EmitSkyBoxVertex (float s, float t, int axis)
{
	vec3_t		v, b;
	int			j, k;
	float		w, h;
	static int debug_once = 0;

	// Use modest distance that works without clipping issues
	float skybox_distance = 1000.0;
	b[0] = s * skybox_distance / sqrt(3.0);
	b[1] = t * skybox_distance / sqrt(3.0);
	b[2] = skybox_distance / sqrt(3.0);

	for (j=0 ; j<3 ; j++)
	{
		k = st_to_vec[axis][j];
		if (k < 0)
			v[j] = -b[-k - 1];
		else
			v[j] = b[k - 1];
		v[j] += r_origin[j];
	}

	// convert from range [-1,1] to [0,1]
	s = (s+1)*0.5;
	t = (t+1)*0.5;

	// avoid bilerp seam
	w = skybox_textures[skytexorder[axis]]->width;
	h = skybox_textures[skytexorder[axis]]->height;

	s = s * (w-1)/w + 0.5/w;
	t = t * (h-1)/h + 0.5/h;

	// apply horizontal scroll (GL_REPEAT handles wrapping)
	s += sky_box_scroll;

	t = 1.0 - t;
	GL_ImmTexCoord2f (s, t);
	GL_ImmVertex3f(v[0], v[1], v[2]);
}

/*
==============
Sky_DrawSkyBox

FIXME: eliminate cracks by adding an extra vert on tjuncs
==============
*/
void Sky_DrawSkyBox (void)
{
	int		i;
	qboolean	use_cubemap;
	glprogram_t	*skyprog = &gl_shader_sky_boxside;

	/* This function used to save, set and restore u_alpha_threshold, because
	 * the one sky program branched on it and that uniform is engine-global
	 * alpha-test state.  Getting it wrong here was uhexen2-khsa: left hot at
	 * 1.0 it turned the next entity's discard into "if (color.a < 1.0)",
	 * which on NVIDIA killed roughly half the fragments of a fully opaque
	 * skin -- its texture filter returns values a hair under 1.0 for most
	 * sub-texel positions where AMD's returns exactly 1.0.  The screen-door.
	 * uhexen2-sp7v added the save/restore this had been missing; uhexen2-a5nn.4
	 * removes the need for one, since gl_shader_sky_boxside has no mode to
	 * switch. */

	// update UV scroll offset (same unit as r_skyspeed_*: scroll units/sec, 128 = full texture)
	sky_box_scroll = (float)fmod(cl.time * r_skybox_speed.value * (1.0 / 128.0), 1.0);
	sky_box_rot = sky_box_scroll * (float)(M_PI * 0.5);

	// Force skybox to render at maximum depth (always behind everything).
	// Reversed-Z: max-depth is 0.0; standard: 1.0.
	R_SetDepthRange (gl_clipcontrol_able ? 0.0f : 1.0f, gl_clipcontrol_able ? 0.0f : 1.0f);

	// Disable face culling so faces are visible from inside
	R_SetCull (false);

	/* The cubemap path draws the same six quads, but every one of them samples
	 * one cubemap by direction instead of its own 2D face, so there is no
	 * per-face bind and -- the point of it -- no seam for r_skywind to pull
	 * apart.  Falls back to the 2D faces when the images would not make a
	 * cubemap, or when r_skycubemap is off.  uhexen2-ctk9. */
	use_cubemap = (skybox_cubemap != 0) && r_skycubemap.integer &&
		      (gl_shader_sky_cubemap.program != 0);
	if (use_cubemap)
	{
		glBindTexture_fp(GL_TEXTURE_CUBE_MAP, skybox_cubemap);
		skyprog = &gl_shader_sky_cubemap;
	}

	for (i=0 ; i<6 ; i++)
	{
		if (!use_cubemap && !skybox_texnums[skytexorder[i]])
			continue;

		if (!use_cubemap)	/* Bind the actual skybox texture */
			glBindTexture_fp(GL_TEXTURE_2D, skybox_texnums[skytexorder[i]]);

		skymins[0][i] = -1;
		skymins[1][i] = -1;
		skymaxs[0][i] = 1;
		skymaxs[1][i] = 1;

		GL_ImmBegin();
		// Reverse winding order so faces are visible from inside the skybox
		Sky_EmitSkyBoxVertex (skymins[0][i], skymins[1][i], i);
		Sky_EmitSkyBoxVertex (skymaxs[0][i], skymins[1][i], i);
		Sky_EmitSkyBoxVertex (skymaxs[0][i], skymaxs[1][i], i);
		Sky_EmitSkyBoxVertex (skymins[0][i], skymaxs[1][i], i);
		GL_ImmEnd(GL_QUADS, skyprog);

		rs_skypolys++;
		rs_skypasses++;

		if (Fog_GetDensity() > 0 && skyfog > 0)
		{
			float *c;

			c = Fog_GetColor();
			R_SetBlend (true);
			GL_ImmColor4f (c[0],c[1],c[2], CLAMP(0.0,skyfog,1.0));

			GL_ImmBegin();
			// Reverse winding order so faces are visible from inside the skybox
			Sky_EmitSkyBoxVertex (skymins[0][i], skymins[1][i], i);
			Sky_EmitSkyBoxVertex (skymaxs[0][i], skymins[1][i], i);
			Sky_EmitSkyBoxVertex (skymaxs[0][i], skymaxs[1][i], i);
			Sky_EmitSkyBoxVertex (skymins[0][i], skymaxs[1][i], i);
			GL_ImmEnd(GL_QUADS, &gl_shader_flat);

			GL_ImmColor3f(1, 1, 1);
			R_SetBlend (false);

			rs_skypasses++;
		}
	}

	// Restore GL state — full window-Z range is symmetric, no flip needed
	if (use_cubemap)
		glBindTexture_fp(GL_TEXTURE_CUBE_MAP, 0);
	R_SetDepthRange (0.0, 1.0);
	R_SetCull (true);
}

//==============================================================================
//
//  RENDER CLOUDS
//
//==============================================================================

/* REMOVED 2026-09-01 (uhexen2-a5nn.4): Sky_SetBoxVert, Sky_GetTexCoord,
 * Sky_DrawFaceQuad, Sky_DrawFace and Sky_DrawSkyLayers, plus the local
 * DrawGLPoly further up.
 *
 * QuakeSpasm's subdivided sky-face path, which nothing has called here for a
 * long time: Sky_DrawSkyLayers had no caller and no declaration in any header,
 * and it was the only route to the other four.  The compiler had been saying
 * so about DrawGLPoly the whole time -- "defined but not used" -- and nobody
 * was reading the warning.  The scrolling sky reaches the screen through
 * R_DrawSkyChain in gl_warp.c instead, and skyboxes through Sky_DrawSkyBox.
 *
 * Deleted rather than ported because splitting the sky program would otherwise
 * have meant deciding which of the two new programs unreachable code wanted.
 * It also carried a real defect for whoever revived it: Sky_DrawFaceQuad
 * called glUniform4f on gl_shader_sky.u_skyfog while a different program was
 * still current -- uniform writes land on the bound program, and GL 4.3 core
 * raises GL_INVALID_OPERATION with none bound -- so its sky fog could never
 * have worked. Recover from git history if it is ever wanted; it will need
 * that fixed and a program chosen. */

/*
==============
Sky_DrawSky

called once per frame before drawing anything else
==============
*/
void Sky_DrawSky (void)
{
	int				i;

	// If no skybox is loaded, scrolling sky is handled by R_DrawSkyChain
	if (!skybox_name[0])
		return;

	// Recompute per-skybox wind UV offset before any sky draw this frame
	Sky_UpdateWind ();

	//
	// process brush entities for sky
	//
	Fog_DisableGFog ();
	if (Fog_GetDensity() > 0)
	{
		float *c = Fog_GetColor();
		GL_ImmColor3f(c[0], c[1], c[2]);
	}
	else
		GL_ImmColor3f(skyflatcolor[0], skyflatcolor[1], skyflatcolor[2]);

	Sky_ProcessEntities ();
	GL_ImmColor3f(1, 1, 1);

	//
	// render skybox
	//
	if (!r_fastsky.value && !(Fog_GetDensity() > 0 && skyfog >= 1))
	{
		R_SetBlend (false);
		GL_ImmColor4f(1, 1, 1, 1);
		if (have_stencil)
		{
			/* Draw skybox only where stencil=1 (sky surface pixels);
			 * world pixels remain untouched, stencil=0 there since the
			 * world chain resets it.
			 *
			 * Depth-mask OFF, matching Ironwail / QuakeSpasm: sky
			 * SURFACES write depth (the pre-pass in DrawTextureChains
			 * does that), the skybox itself does not.  That keeps the
			 * sky-brush wall depth alive for every later pass, so
			 * geometry a mapper deliberately hid behind a sky brush --
			 * a standard visibility/perf trick -- stays hidden.
			 *
			 * This intentionally reverts uhexen2-4h32, which turned the
			 * mask ON here so that glDepthRange(0,0) inside
			 * Sky_DrawSkyBox would stamp far-plane depth over the wall
			 * depth and make enemies / sprites visible through
			 * sky-windowed openings.  That traded away sky occlusion
			 * for every map to fix one; 4h32's case gets a targeted
			 * solution instead.  uhexen2-ixdb -- read both beads before
			 * flipping this back.
			 *
			 * GL_ALWAYS stays: with writes off the skybox is a pure
			 * stencil-limited colour fill, and the pinned far-plane
			 * depth range would otherwise fail the test against the
			 * wall depth the pre-pass just wrote. */
			R_SetStencilTest (true);
			R_SetStencilFunc (GL_EQUAL, 1, 0xFF);
			R_SetStencilOp (GL_KEEP, GL_KEEP, GL_KEEP);
			R_SetDepthFunc (GL_ALWAYS);
			R_SetDepthMask (false);
		}
		else
		{
			R_SetDepthFunc (gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL);
			R_SetDepthMask (false);
		}

		Sky_DrawSkyBox ();

		R_SetDepthMask (true);
		R_SetDepthFunc (gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL);
		if (have_stencil)
			R_SetStencilTest (false);
	}

	Fog_EnableGFog ();
}
