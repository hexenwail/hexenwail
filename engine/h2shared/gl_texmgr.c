/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
*/

// Minimal texture manager for uhexen2 - provides functions needed by gl_sky.c and gl_fog.c

#include "quakedef.h"

// Undef the GL_Bind macro so we can define our function
#undef GL_Bind

/* gltexture_t is already defined in glquake.h with a simpler structure.
 * We provide compatibility functions for gl_sky.c and gl_fog.c.
 */

extern GLuint currenttexture;

/* Palette tables - these are defined elsewhere in uhexen2 */
extern unsigned int d_8to24table[256];
extern unsigned int d_8to24table_fbright[256];
extern unsigned int d_8to24table_nobright[256];
extern unsigned int d_8to24table_conchars[256];

/* Texture flags for TEXPREF_* - need to match what gl_sky.c expects */
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

enum srcformat {SRC_INDEXED, SRC_LIGHTMAP, SRC_RGBA, SRC_EXTERNAL};
typedef uintptr_t src_offset_t;

/* Special texture placeholders */
static gltexture_t notexture_val;
static gltexture_t nulltexture_val;
gltexture_t *notexture = &notexture_val;
gltexture_t *nulltexture = &nulltexture_val;

/* Stub implementations for TexMgr functions needed by gl_sky.c and gl_fog.c */

gltexture_t *TexMgr_FindTexture (qmodel_t *owner, char *name)
{
	/* uhexen2 uses a different texture management approach */
	return NULL;
}

gltexture_t *TexMgr_NewTexture (void)
{
	/* uhexen2 uses a different texture management approach */
	return NULL;
}

void TexMgr_FreeTexture (gltexture_t *kill)
{
	/* uhexen2 uses a different texture management approach */
}

void TexMgr_FreeTextures (unsigned int flags, unsigned int mask)
{
	/* uhexen2 uses a different texture management approach */
}

void TexMgr_FreeTexturesForOwner (qmodel_t *owner)
{
	/* uhexen2 uses a different texture management approach */
}

void TexMgr_LoadPalette (void)
{
	/* Palette loading is handled elsewhere in uhexen2 */
}

void TexMgr_NewGame (void)
{
	TexMgr_FreeTextures (0, TEXPREF_PERSIST);
	TexMgr_LoadPalette ();
}

void TexMgr_Init (void)
{
	/* Initialize placeholder textures */
	notexture->texnum = 0;
	strcpy(notexture->identifier, "notexture");
	notexture->width = 2;
	notexture->height = 2;
	notexture->flags = 0;

	nulltexture->texnum = 0;
	strcpy(nulltexture->identifier, "nulltexture");
	nulltexture->width = 2;
	nulltexture->height = 2;
	nulltexture->flags = 0;
}

/* Simple pool for gltexture_t structures returned by TexMgr_LoadImage */
#define MAX_MANAGED_TEXTURES 32
static gltexture_t managed_textures[MAX_MANAGED_TEXTURES];
static int num_managed_textures = 0;

/* TexMgr_LoadImage - simplified for uhexen2 compatibility */
gltexture_t *TexMgr_LoadImage (qmodel_t *owner, char *name, int width, int height, enum srcformat format,
                               byte *data, char *source_file, src_offset_t source_offset, unsigned flags)
{
	gltexture_t *tex;
	GLuint texnum;
	unsigned int *rgba;
	int i, pixels;

	if (num_managed_textures >= MAX_MANAGED_TEXTURES)
	{
		Con_Printf("TexMgr_LoadImage: pool full\n");
		return notexture;
	}

	tex = &managed_textures[num_managed_textures++];

	/* Convert indexed data to RGBA through palette */
	pixels = width * height;
	if (format == SRC_INDEXED)
	{
		static unsigned int temp_rgba[256 * 256]; /* max 256x256 indexed texture */
		if (pixels > (int)(sizeof(temp_rgba) / sizeof(temp_rgba[0])))
		{
			Con_Printf("TexMgr_LoadImage: indexed texture too large\n");
			return notexture;
		}
		for (i = 0; i < pixels; i++)
			temp_rgba[i] = d_8to24table[data[i]];
		rgba = temp_rgba;
		/* d_8to24table already has alpha=255 for 0-254, alpha=0 for 255 */
	}
	else
	{
		rgba = (unsigned int *)data;	/* already RGBA */
	}

	/* Generate and upload the texture */
	glGenTextures_fp(1, &texnum);
	glBindTexture_fp(GL_TEXTURE_2D, texnum);

	if (flags & TEXPREF_NEAREST)
	{
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else
	{
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

	tex->texnum = texnum;
	tex->width = width;
	tex->height = height;
	tex->flags = flags;
	q_strlcpy(tex->identifier, name, sizeof(tex->identifier));

	return tex;
}

void TexMgr_ReloadImage (gltexture_t *glt, int shirt, int pants)
{
	/* Stub for compatibility */
}

void TexMgr_ReloadImages (void)
{
	/* Stub for compatibility */
}

void TexMgr_ReloadNobrightImages (void)
{
	/* Stub for compatibility */
}

void TexMgr_SetTexLevel(void)
{
	/* Stub for compatibility */
}

float TexMgr_FrameUsage(void)
{
	return 0.0f;
}

int TexMgr_Pad(int s)
{
	return s;
}

int TexMgr_SafeTextureSize(int s)
{
	return s;
}

int TexMgr_PadConditional(int s)
{
	return s;
}

/*
================
GL_Bind - implementation using uhexen2's currenttexture
================
*/
void GL_Bind (gltexture_t *texture)
{
	if (!texture)
		return;

	if (currenttexture != texture->texnum)
	{
		currenttexture = texture->texnum;
		glBindTexture_fp(GL_TEXTURE_2D, currenttexture);
	}
}

/*
================
GL_EnableMultitexture
================
*/
void GL_EnableMultitexture(void)
{
	glActiveTextureARB_fp(GL_TEXTURE1_ARB);
}

/*
================
GL_DisableMultitexture
================
*/
void GL_DisableMultitexture(void)
{
	glActiveTextureARB_fp(GL_TEXTURE0_ARB);
}
