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
#include "gl_vbo.h"

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

// DrawGLPoly is static in gl_rsurf.c, so provide local implementation
static void DrawGLPoly(glpoly_t *p)
{
	int	i;
	float	*v;

	GL_ImmBegin();
	v = p->verts[0];
	for (i = 0; i < p->numverts; i++, v+= VERTEXSIZE)
	{
		GL_ImmTexCoord2f(v[3], v[4]);
		GL_ImmVertex3f(v[0], v[1], v[2]);
	}
	GL_ImmEnd(GL_POLYGON, &gl_shader_sky);
}

int	rs_skypolys; //for r_speeds readout
int rs_skypasses; //for r_speeds readout
GLfloat	skyflatcolor[3];
float	skymins[2][6], skymaxs[2][6];

char	skybox_name[32] = ""; //name of current skybox, or "" if no skybox

gltexture_t	*skybox_textures[6];
GLuint	skybox_texnums[6]; // Actual OpenGL texture IDs (TexMgr_LoadImage returns same pointer)
gltexture_t	*solidskytexture, *alphaskytexture;

/* uhexen2 doesn't have gl_farclip, use a default value */
cvar_t gl_farclip = {"gl_farclip", "2048", CVAR_NONE};
extern cvar_t r_skyalpha; /* defined in gl_rmain.c */
cvar_t r_fastsky = {"r_fastsky", "0", CVAR_NONE};
cvar_t r_sky_quality = {"r_sky_quality", "12", CVAR_NONE};
cvar_t r_skyfog = {"r_skyfog", "0.5", CVAR_NONE};

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

	if (strcmp(skybox_name, name) == 0)
		return; //no change

	//purge old textures
	for (i=0; i<6; i++)
	{
		if (skybox_textures[i] && skybox_textures[i] != notexture)
			TexMgr_FreeTexture (skybox_textures[i]);
		skybox_textures[i] = NULL;
		skybox_texnums[i] = 0;
	}

	//turn off skybox if sky is set to ""
	if (name[0] == 0)
	{
		skybox_name[0] = 0;
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
		return;
	}

	strcpy(skybox_name, name);
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
		return; //FIXME: how could this possibly ever happen? -- if there's no
	// worldspawn then the sever wouldn't send the loadmap message to the client

	data = COM_Parse(data);
	if (!data) //should never happen
		return; // error
	if (com_token[0] != '{') //should never happen
		return; // error
	while (1)
	{
		data = COM_Parse(data);
		if (!data)
			return; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			strcpy(key, com_token + 1);
		else
			strcpy(key, com_token);
		while (key[strlen(key)-1] == ' ') // remove trailing spaces
			key[strlen(key)-1] = 0;
		data = COM_Parse(data);
		if (!data)
			return; // error
		strcpy(value, com_token);

		if (!strcmp("sky", key))
			Sky_LoadSkyBox(value);

		if (!strcmp("skyfog", key))
			skyfog = atof(value);

#if 1 //also accept non-standard keys
		else if (!strcmp("skyname", key)) //half-life
			Sky_LoadSkyBox(value);
		else if (!strcmp("qlsky", key)) //quake lives
			Sky_LoadSkyBox(value);
#endif
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

	Cmd_AddCommand ("sky",Sky_SkyCommand_f);

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
void Sky_ProcessPoly (glpoly_t	*p)
{
	int			i;
	vec3_t		verts[MAX_CLIP_VERTS];

	if (!p || p->numverts <= 0 || p->numverts >= MAX_CLIP_VERTS)
		return;

	// Don't draw the polygon here - just update bounds
	// The actual sky rendering happens in Sky_DrawSky()

	//update sky bounds
	if (!r_fastsky.value)
	{
		for (i=0 ; i<p->numverts ; i++)
			VectorSubtract (p->verts[i], r_origin, verts[i]);
		Sky_ClipPoly (p->numverts, verts[0], 0);
	}
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

	// Force skybox to render at maximum depth (always behind everything)
	glDepthRange_fp(1.0, 1.0);

	// Disable face culling so faces are visible from inside
	glDisable_fp(GL_CULL_FACE);

	GL_SetAlphaThreshold (1.0f);	/* single-layer skybox mode */

	for (i=0 ; i<6 ; i++)
	{
		if (!skybox_texnums[skytexorder[i]])
			continue;

		// Bind the actual skybox texture
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
		GL_ImmEnd(GL_QUADS, &gl_shader_sky);

		rs_skypolys++;
		rs_skypasses++;

		if (Fog_GetDensity() > 0 && skyfog > 0)
		{
			float *c;

			c = Fog_GetColor();
			glEnable_fp(GL_BLEND);
			GL_ImmColor4f (c[0],c[1],c[2], CLAMP(0.0,skyfog,1.0));

			GL_ImmBegin();
			// Reverse winding order so faces are visible from inside the skybox
			Sky_EmitSkyBoxVertex (skymins[0][i], skymins[1][i], i);
			Sky_EmitSkyBoxVertex (skymaxs[0][i], skymins[1][i], i);
			Sky_EmitSkyBoxVertex (skymaxs[0][i], skymaxs[1][i], i);
			Sky_EmitSkyBoxVertex (skymins[0][i], skymaxs[1][i], i);
			GL_ImmEnd(GL_QUADS, &gl_shader_flat);

			GL_ImmColor3f(1, 1, 1);
			glDisable_fp(GL_BLEND);

			rs_skypasses++;
		}
	}

	// Restore GL state
	glDepthRange_fp(0.0, 1.0);
	glEnable_fp(GL_CULL_FACE);
}

//==============================================================================
//
//  RENDER CLOUDS
//
//==============================================================================

/*
==============
Sky_SetBoxVert
==============
*/
void Sky_SetBoxVert (float s, float t, int axis, vec3_t v)
{
	vec3_t		b;
	int			j, k;

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
}

/*
=============
Sky_GetTexCoord
=============
*/
void Sky_GetTexCoord (vec3_t v, float speed, float *s, float *t)
{
	vec3_t	dir;
	float	length, scroll;

	VectorSubtract(v, r_origin, dir);
	dir[2] *= 3;	// flatten the sphere

	length = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
	length = sqrt(length);
	length = 6 * 63 / length;

	scroll = cl.time*speed;
	scroll -= (int)scroll & ~127;

	*s = (scroll + dir[0] * length) * (1.0 / 128);
	*t = (scroll + dir[1] * length) * (1.0 / 128);
}

/*
===============
Sky_DrawFaceQuad
===============
*/
void Sky_DrawFaceQuad (glpoly_t *p)
{
	float	s, t;
	float	*v;
	int		i;

	if (r_skyalpha.value >= 1.0)
	{
		GL_Bind (solidskytexture);
		GL_EnableMultitexture();
		GL_Bind (alphaskytexture);

		GL_ImmBegin();
		for (i=0, v=p->verts[0] ; i<4 ; i++, v+=VERTEXSIZE)
		{
			Sky_GetTexCoord (v, 8, &s, &t);
			GL_ImmTexCoord2f (s, t);
			Sky_GetTexCoord (v, 16, &s, &t);
			GL_ImmLMCoord2f (s, t);
			GL_ImmVertex3f (v[0], v[1], v[2]);
		}
		GL_ImmEnd(GL_QUADS, &gl_shader_sky);

		GL_DisableMultitexture();

		rs_skypolys++;
		rs_skypasses++;
	}
	else
	{
		GL_Bind (solidskytexture);

		GL_ImmColor3f(1, 1, 1);

		GL_ImmBegin();
		for (i=0, v=p->verts[0] ; i<4 ; i++, v+=VERTEXSIZE)
		{
			Sky_GetTexCoord (v, 8, &s, &t);
			GL_ImmTexCoord2f (s, t);
			GL_ImmVertex3f(v[0], v[1], v[2]);
		}
		GL_ImmEnd(GL_QUADS, &gl_shader_sky);

		GL_Bind (alphaskytexture);
		glEnable_fp(GL_BLEND);

		GL_ImmColor4f(1, 1, 1, r_skyalpha.value);

		GL_ImmBegin();
		for (i=0, v=p->verts[0] ; i<4 ; i++, v+=VERTEXSIZE)
		{
			Sky_GetTexCoord (v, 16, &s, &t);
			GL_ImmTexCoord2f (s, t);
			GL_ImmVertex3f(v[0], v[1], v[2]);
		}
		GL_ImmEnd(GL_QUADS, &gl_shader_sky);

		glDisable_fp(GL_BLEND);

		rs_skypolys++;
		rs_skypasses += 2;
	}

	if (Fog_GetDensity() > 0 && skyfog > 0)
	{
		float *c;

		c = Fog_GetColor();
		glEnable_fp(GL_BLEND);
		GL_ImmColor4f(c[0],c[1],c[2], CLAMP(0.0,skyfog,1.0));

		GL_ImmBegin();
		for (i=0, v=p->verts[0] ; i<4 ; i++, v+=VERTEXSIZE)
			GL_ImmVertex3f(v[0], v[1], v[2]);
		GL_ImmEnd(GL_QUADS, &gl_shader_flat);

		GL_ImmColor3f (1, 1, 1);
		glDisable_fp(GL_BLEND);

		rs_skypasses++;
	}
}

/*
==============
Sky_DrawFace
==============
*/

void Sky_DrawFace (int axis)
{
	glpoly_t	*p;
	vec3_t		verts[4];
	int			i, j, start;
	float		di, qi, dj, qj;
	vec3_t		vup, vright, temp, temp2;

	Sky_SetBoxVert(-1.0, -1.0, axis, verts[0]);
	Sky_SetBoxVert(-1.0, 1.0, axis, verts[1]);
	Sky_SetBoxVert(1.0, 1.0, axis, verts[2]);
	Sky_SetBoxVert(1.0, -1.0, axis, verts[3]);

	start = Hunk_LowMark();
	p = (glpoly_t *)Hunk_Alloc(sizeof(glpoly_t));

	VectorSubtract(verts[2], verts[3], vup);
	VectorSubtract(verts[2], verts[1], vright);

	di = q_max((int)r_sky_quality.value, 1);
	qi = 1.0 / di;
	dj = (axis < 4) ? di * 2 : di; //subdivide vertically more than horizontally on skybox sides
	qj = 1.0 / dj;

	for (i = 0; i < di; i++)
	{
		for (j = 0; j < dj; j++)
		{
			if (i*qi < skymins[0][axis] / 2 + 0.5 - qi || i * qi > skymaxs[0][axis] / 2 + 0.5 ||
				j * qj < skymins[1][axis] / 2 + 0.5 - qj || j * qj > skymaxs[1][axis] / 2 + 0.5)
				continue;

			//if (i&1 ^ j&1) continue; //checkerboard test
			VectorScale(vright, qi*i, temp);
			VectorScale(vup, qj*j, temp2);
			VectorAdd(temp, temp2, temp);
			VectorAdd(verts[0], temp, p->verts[0]);

			VectorScale(vup, qj, temp);
			VectorAdd(p->verts[0], temp, p->verts[1]);

			VectorScale(vright, qi, temp);
			VectorAdd(p->verts[1], temp, p->verts[2]);

			VectorAdd(p->verts[0], temp, p->verts[3]);

			Sky_DrawFaceQuad(p);
		}
	}
	Hunk_FreeToLowMark(start);
}

/*
==============
Sky_DrawSkyLayers

draws the old-style scrolling cloud layers
==============
*/
void Sky_DrawSkyLayers (void)
{
	int i;

	for (i=0 ; i<6 ; i++)
		if (skymins[0][i] < skymaxs[0][i] && skymins[1][i] < skymaxs[1][i])
			Sky_DrawFace (i);
}

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
		glDisable_fp(GL_BLEND);
		GL_ImmColor4f(1, 1, 1, 1);
		glDepthFunc_fp(GL_LEQUAL);
		glDepthMask_fp(0);

		Sky_DrawSkyBox ();

		glDepthMask_fp(1);
		glDepthFunc_fp(GL_LEQUAL);
	}

	Fog_EnableGFog ();
}
