/*
 * mod_placeholder.c -- stand-in mesh for a model the gamecode precached and
 * the filesystem does not have.
 * Copyright (C) 2026  Hexenwail contributors
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

/* A map built against a mod the player does not have precaches models that
 * are not on disk.  Every other Quake-family engine keeps such a map running
 * and reports the gaps; Hexenwail used to Sys_Error out of Mod_LoadModel.
 * Rather than teach the server and both renderers to carry a NULL qmodel_t,
 * we hand the ordinary alias loader an ordinary alias model that we generate
 * here, so nothing downstream needs a special case.
 *
 * Deliberate divergence from Ironwail, which has no model-level fallback at
 * all -- it inherits vanilla Quake's fatal Host_Error.  Only the *look* is
 * borrowed: the same black/pink checker Ironwail substitutes for a missing
 * texture, so a missing model reads on screen as the same class of problem.
 */

#include "quakedef.h"
#include "mod_placeholder.h"

/* Ironwail's notexture checker colour (Quake/gl_texmgr.c notexture_data). */
#define PH_PINK_R	159
#define PH_PINK_G	91
#define PH_PINK_B	83

#define PH_SKIN_DIM	16	/* skin is PH_SKIN_DIM square */
#define PH_SKIN_BLOCK	4	/* checker square, in texels */
#define PH_NUMVERTS	24	/* 6 quads; corners are unshared so that every
				 * face gets the whole skin rather than a
				 * smear of one shared corner's texcoord */
#define PH_NUMTRIS	12
#define PH_HALFSIZE	16.0f	/* cube spans -16..+16, about the size of a
				 * Hexen II pickup, so the gap is obvious
				 * without swallowing the room */

/* Cube faces as (origin, edge u, edge v) in unit coords, ordered so that
 * u cross v is the outward normal.  Emitting corners o, o+u, o+u+v, o+v then
 * gives front-facing winding without any per-face special casing. */
static const struct {
	int	origin[3];
	int	u[3];
	int	v[3];
	int	normalindex;	/* nearest entry in common/anorms.h */
} ph_faces[6] = {
	{ {1,0,0}, {0,1,0}, {0,0,1},  52 },	/* +X */
	{ {0,0,0}, {0,0,1}, {0,1,0}, 143 },	/* -X */
	{ {0,1,0}, {0,0,1}, {1,0,0},  32 },	/* +Y */
	{ {0,0,0}, {1,0,0}, {0,0,1}, 104 },	/* -Y */
	{ {0,0,1}, {1,0,0}, {0,1,0},   5 },	/* +Z */
	{ {0,0,0}, {0,1,0}, {1,0,0},  84 },	/* -Z */
};

/* Skin texcoords for the four corners emitted per face, in the same order. */
static const int ph_facest[4][2] = {
	{ 0, 0 },
	{ PH_SKIN_DIM - 1, 0 },
	{ PH_SKIN_DIM - 1, PH_SKIN_DIM - 1 },
	{ 0, PH_SKIN_DIM - 1 },
};

#define PH_BUFSIZE	(sizeof(mdl_t) \
			 + sizeof(daliasskintype_t) \
			 + (PH_SKIN_DIM * PH_SKIN_DIM) \
			 + (PH_NUMVERTS * sizeof(stvert_t)) \
			 + (PH_NUMTRIS * sizeof(dtriangle_t)) \
			 + sizeof(daliasframetype_t) \
			 + sizeof(daliasframe_t) \
			 + (PH_NUMVERTS * sizeof(trivertx_t)))

static byte	ph_buffer[PH_BUFSIZE];
static qboolean	ph_built;

/*
=================
Mod_PlaceholderColor

Nearest palette entry to an RGB triplet.  Index 255 is excluded: Hexen II
treats it as transparent, so picking it would punch holes in the marker.
=================
*/
static byte Mod_PlaceholderColor (int r, int g, int b)
{
	const byte	*pal = host_basepal;
	int		i, best = 0;
	long		bestdist = -1;

	if (!pal)
		return 0;

	for (i = 0; i < 255; i++)
	{
		long	dr = (long)pal[i*3+0] - r;
		long	dg = (long)pal[i*3+1] - g;
		long	db = (long)pal[i*3+2] - b;
		long	dist = dr*dr + dg*dg + db*db;

		if (bestdist < 0 || dist < bestdist)
		{
			bestdist = dist;
			best = i;
		}
	}

	return (byte)best;
}

/*
=================
Mod_SynthPlaceholderMDL
=================
*/
void *Mod_SynthPlaceholderMDL (void)
{
	mdl_t			*hdr;
	daliasskintype_t	*skintype;
	byte			*skin;
	stvert_t		*stverts;
	dtriangle_t		*tris;
	daliasframetype_t	*frametype;
	daliasframe_t		*frame;
	trivertx_t		*verts;
	byte			*p;
	byte			pink, black;
	int			f, c, x, y;

	if (ph_built)
		return ph_buffer;

	memset (ph_buffer, 0, sizeof(ph_buffer));
	p = ph_buffer;

	hdr = (mdl_t *)p;
	p += sizeof(mdl_t);

	hdr->ident = LittleLong (IDPOLYHEADER);
	hdr->version = LittleLong (ALIAS_VERSION);
	hdr->numskins = LittleLong (1);
	hdr->skinwidth = LittleLong (PH_SKIN_DIM);
	hdr->skinheight = LittleLong (PH_SKIN_DIM);
	hdr->numverts = LittleLong (PH_NUMVERTS);
	hdr->numtris = LittleLong (PH_NUMTRIS);
	hdr->numframes = LittleLong (1);
	hdr->synctype = (synctype_t) LittleLong (ST_SYNC);
	hdr->flags = LittleLong (0);
	hdr->size = LittleFloat (1.0f);
	hdr->boundingradius = LittleFloat (PH_HALFSIZE * 1.7320508f);

	/* Byte vertex coords 0..255 map onto -PH_HALFSIZE..+PH_HALFSIZE. */
	for (c = 0; c < 3; c++)
	{
		hdr->scale[c] = LittleFloat ((PH_HALFSIZE * 2.0f) / 255.0f);
		hdr->scale_origin[c] = LittleFloat (-PH_HALFSIZE);
		hdr->eyeposition[c] = LittleFloat (0.0f);
	}

	skintype = (daliasskintype_t *)p;
	p += sizeof(daliasskintype_t);
	skintype->type = (aliasskintype_t) LittleLong (ALIAS_SKIN_SINGLE);

	skin = p;
	p += PH_SKIN_DIM * PH_SKIN_DIM;

	pink  = Mod_PlaceholderColor (PH_PINK_R, PH_PINK_G, PH_PINK_B);
	black = Mod_PlaceholderColor (0, 0, 0);

	for (y = 0; y < PH_SKIN_DIM; y++)
	{
		for (x = 0; x < PH_SKIN_DIM; x++)
		{
			int	odd = ((x / PH_SKIN_BLOCK) ^ (y / PH_SKIN_BLOCK)) & 1;
			skin[y * PH_SKIN_DIM + x] = odd ? black : pink;
		}
	}

	stverts = (stvert_t *)p;
	p += PH_NUMVERTS * sizeof(stvert_t);

	tris = (dtriangle_t *)p;
	p += PH_NUMTRIS * sizeof(dtriangle_t);

	frametype = (daliasframetype_t *)p;
	p += sizeof(daliasframetype_t);
	frametype->type = (aliasframetype_t) LittleLong (ALIAS_SINGLE);

	frame = (daliasframe_t *)p;
	p += sizeof(daliasframe_t);
	q_strlcpy (frame->name, "placeholder", sizeof(frame->name));
	for (c = 0; c < 3; c++)
	{
		frame->bboxmin.v[c] = 0;
		frame->bboxmax.v[c] = 255;
	}

	verts = (trivertx_t *)p;
	p += PH_NUMVERTS * sizeof(trivertx_t);

	for (f = 0; f < 6; f++)
	{
		int	base = f * 4;

		for (c = 0; c < 4; c++)
		{
			int	n = base + c;
			int	pos[3];

			/* corner order o, o+u, o+u+v, o+v */
			int	su = (c == 1 || c == 2);
			int	sv = (c == 2 || c == 3);

			for (x = 0; x < 3; x++)
			{
				pos[x] = ph_faces[f].origin[x]
				       + (su ? ph_faces[f].u[x] : 0)
				       + (sv ? ph_faces[f].v[x] : 0);
				verts[n].v[x] = pos[x] ? 255 : 0;
			}
			verts[n].lightnormalindex = (byte)ph_faces[f].normalindex;

			stverts[n].onseam = LittleLong (0);
			stverts[n].s = LittleLong (ph_facest[c][0]);
			stverts[n].t = LittleLong (ph_facest[c][1]);
		}

		tris[f*2+0].facesfront = LittleLong (1);
		tris[f*2+0].vertindex[0] = LittleLong (base + 0);
		tris[f*2+0].vertindex[1] = LittleLong (base + 1);
		tris[f*2+0].vertindex[2] = LittleLong (base + 2);

		tris[f*2+1].facesfront = LittleLong (1);
		tris[f*2+1].vertindex[0] = LittleLong (base + 0);
		tris[f*2+1].vertindex[1] = LittleLong (base + 2);
		tris[f*2+1].vertindex[2] = LittleLong (base + 3);
	}

	ph_built = true;

	return ph_buffer;
}
