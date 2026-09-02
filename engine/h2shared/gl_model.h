/*
 * model.h -- header for model loading and caching
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

#ifndef GL_MODEL_H
#define GL_MODEL_H

#include "genmodel.h"
#include "spritegn.h"

/*

d*_t structures are on-disk representations
m*_t structures are in-memory

*/

/*
==============================================================================

BRUSH MODELS

==============================================================================
*/


//
// in memory representation
//
// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct
{
	vec3_t		position;
} mvertex_t;

#define	SIDE_FRONT	0
#define	SIDE_BACK	1
#define	SIDE_ON		2


// plane_t structure
// !!! if this is changed, it must be changed in asm_i386.h too !!!
typedef struct mplane_s
{
	vec3_t	normal;
	float	dist;
	byte	type;			// for texture axis selection and fast side tests
	byte	signbits;		// signx + signy<<1 + signz<<1
	byte	pad[2];
} mplane_t;

/* Build a fullbright mask texture from indexed (palette) pixel data.
 * Pixels with index >= vid.fullbright (typically 224) are kept; others
 * become transparent.  Returns 0 if no fullbright pixels.  Used by
 * both alias-skin loading and BSP miptex loading.  uhexen2-sjvf. */
GLuint Mod_LoadFullbrightTexture (const char *name, byte *data, int width, int height);

typedef struct texture_s
{
	char		name[16];
	unsigned int	width, height;
	GLuint			gl_texturenum;
	GLuint			gl_fb_texturenum;	// fullbright mask, 0 if no fullbright pixels (uhexen2-sjvf)
	/* Material-map sidecars, 0 when the pack shipped none.  Only ever set
	 * on the external-replacement path: a BSP miptex is 8-bit indexed and
	 * carries no surface detail to recover, so there is nothing to derive
	 * these from without a replacement.  uhexen2-mfql. */
	GLuint			gl_norm_texturenum;	// tangent-space normal map (_norm / _bump)
	GLuint			gl_gloss_texturenum;	// specular mask (_gloss)
	struct msurface_s	*texturechain;	// for gl_texsort drawing
	int		anim_total;		// total tenths in sequence ( 0 = no)
	int		anim_min, anim_max;	// time for this frame min <=time< max
	struct texture_s *anim_next;		// in the animation sequence
	struct texture_s *alternate_anims;	// bmodels in frmae 1 use these
	unsigned int	offsets[MIPLEVELS];	// four mip maps stored
	int		content_class;	// CONTENTS_WATER/LAVA/SLIME, 0=unknown (uhexen2-8nvj)
	qboolean	translucent_turb; // vanilla SURF_TRANSLUCENT propagated to texture (uhexen2-ft2q)
} texture_t;


#define	SURF_PLANEBACK		2
#define	SURF_DRAWSKY		4
#define SURF_DRAWSPRITE		8
#define SURF_DRAWTURB		0x10
#define SURF_DRAWTILED		0x20
#define SURF_DRAWBACKGROUND	0x40
#define SURF_TRANSLUCENT	0x80	/* r_edge.asm checks this */
#define SURF_DRAWBLACK		0x100
#define SURF_UNDERWATER		0x200
#define SURF_DONTWARP		0x400
#define SURF_DRAWFENCE		0x800

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct
{
	unsigned int	v[2];
	unsigned int	cachededgeoffset;
} medge_t;

typedef struct
{
	float		vecs[2][4];
	texture_t	*texture;
	int		flags;
} mtexinfo_t;

#define	VERTEXSIZE	7

typedef struct glpoly_s
{
	struct	glpoly_s	*next;
	struct	glpoly_s	*chain;
	int		numverts;
	int		flags;		// for SURF_UNDERWATER
	vec3_t	mins, maxs;	// world-space bbox, computed once at load
	float	verts[4][VERTEXSIZE];	// variable sized (xyz s1t1 s2t2)
} glpoly_t;

typedef struct msurface_s
{
	int		visframe;	// should be drawn when node is crossed

	mplane_t	*plane;
	int		flags;

	int		firstedge;	// look up in model->surfedges[], negative numbers
	int		numedges;	// are backwards edges

	int		texturemins[2];
	int		extents[2];

	int		light_s, light_t;	// gl lightmap coordinates

	glpoly_t	*polys;			// multiple if warped
	struct	msurface_s	*texturechain;

	mtexinfo_t	*texinfo;

// lighting info
	int		dlightframe;
	unsigned int		dlightbits[4];	/* one bit per dlight, MAX_DLIGHTS bits; see DLIGHTBIT_* in client.h. uhexen2-liqz */

	unsigned int	lightmaptexturenum;
	byte		styles[MAXLIGHTMAPS];
	int		cached_light[MAXLIGHTMAPS];	// values currently used in lightmap
	qboolean	cached_dlight;			// true if dynamic light in cache
	byte		*samples;		// [numstyles*surfsize]

	int		vbo_firstvert;		// first vertex in static world VBO
	int		vbo_firstindex;		// first index in static world IBO
	int		vbo_numtris;		// triangle count for this surface

	/* Sky stencil pre-pass: pre-baked fan-triangulated indices
	 * into a separate static sky VBO/IBO (positions only).  Used
	 * to skip per-frame triangulation + glBufferData. */
	int		sky_firstindex;		// first index in sky stencil IBO (-1 if none)
	int		sky_numindices;		// triangle index count
} msurface_t;

typedef struct mnode_s
{
// common with leaf
	int		contents;		// 0, to differentiate from leafs
	int		visframe;		// node needs to be traversed if current

	float		minmaxs[6];		// for bounding box culling

	struct mnode_s	*parent;

// node specific
	mplane_t	*plane;
	struct mnode_s	*children[2];	

	unsigned int	firstsurface;
	unsigned int	numsurfaces;
} mnode_t;


typedef struct mleaf_s
{
// common with node
	int		contents;		// wil be a negative contents number
	int		visframe;		// node needs to be traversed if current

	float		minmaxs[6];		// for bounding box culling

	struct mnode_s	*parent;

// leaf specific
	byte		*compressed_vis;
	struct efrag_s	*efrags;

	msurface_t	**firstmarksurface;
	int		nummarksurfaces;
	int		key;			// BSP sequence number for leaf's contents
	byte		ambient_sound_level[NUM_AMBIENTS];
} mleaf_t;

#ifdef ENABLE_BSP2
typedef dclipnode2_t	mclipnode_t;
#else
typedef dclipnode_t	mclipnode_t;
#endif

// !!! if this is changed, it must be changed in asm_i386.h too !!!
typedef struct
{
	mclipnode_t	*clipnodes;
	mplane_t	*planes;
	int		firstclipnode;
	int		lastclipnode;
	vec3_t		clip_mins;
	vec3_t		clip_maxs;
} hull_t;

#define HULL_IMPLICIT	0	// Choose the hull based on bounding box- like in Quake
#define HULL_POINT	1	// 0 0 0, 0 0 0
#define HULL_PLAYER	2	// '-16 -16 0', '16 16 56'
#define HULL_SCORPION	3	// '-24 -24 -20', '24 24 20'
#define HULL_CROUCH	4	// '-16 -16 0', '16 16 28'
#define HULL_HYDRA	5	// '-28 -28 -24', '28 28 24'
#define HULL_GOLEM	6	// ???,???

/*
==============================================================================

SPRITE MODELS

==============================================================================
*/


// FIXME: shorten these?
typedef struct mspriteframe_s
{
	short		width;
	short		height;
	float		up, down, left, right;
	GLuint		gl_texturenum;
} mspriteframe_t;

typedef struct
{
	short		numframes;
	float		*intervals;
	mspriteframe_t	*frames[1];
} mspritegroup_t;

typedef struct
{
	spriteframetype_t	type;
	mspriteframe_t		*frameptr;
} mspriteframedesc_t;

typedef struct
{
	short			type;
	short			maxwidth;
	short			maxheight;
	short			numframes;
	float			beamlength;		// remove?
	//void			*cachespot;		// remove?
	mspriteframedesc_t	frames[1];
} msprite_t;


/*
==============================================================================

ALIAS MODELS

Alias models are position independent, so the cache manager can move them.
==============================================================================
*/

typedef struct
{
	int			firstpose;
	int			numposes;
	float			interval;
	trivertx_t		bboxmin;
	trivertx_t		bboxmax;
	int			frame;
	char			name[16];
} maliasframedesc_t;

typedef struct
{
	trivertx_t		bboxmin;
	trivertx_t		bboxmax;
	int			frame;
} maliasgroupframedesc_t;

typedef struct
{
	int			numframes;
	int			intervals;
	maliasgroupframedesc_t	frames[1];
} maliasgroup_t;

//this is only the GL version
typedef struct mtriangle_s {
	int			facesfront;
	unsigned short		vertindex[3];
	unsigned short		stindex[3];
} mtriangle_t;

/* Pose vertex format type (for multi-format GPU upload) */
typedef enum {
	PV_QUAKE1 = 0,	/* 8-bit XYZ + lightnormal (Hexen II native) */
	PV_MD3 = 1,	/* 16-bit XYZ + spherical normal (MD3 models) */
	PV_IQM = 2,	/* Skeletal animation with bone indices/weights (MD5mesh) */
} poseverttype_t;

/* MD3 model format constants */
/* "IDP3", as a little-endian int read of the four bytes an .md3 starts with.
 * The value here used to spell "MMD3", which no MD3 file has ever begun with;
 * the structs below were wrong too (no name[64], no num_frames, and the
 * surface header missing its ident).  Nothing noticed, because until
 * uhexen2-2ah9 nothing in this tree had ever parsed one -- the MD3 work that
 * landed in 2026-05 was the GPU vertex format and its shader decode. */
#define MD3_IDENT			(('3'<<24)+('P'<<16)+('D'<<8)+'I')	/* "IDP3" */
#define MD3_VERSION			15
#define MD3_XYZ_SCALE		(1.0f/64.0f)
#define MD3_MAX_FRAMES		1024
#define MD3_MAX_TAGS		16
#define MD3_MAX_SURFACES	32
#define MD3_MAX_SHADERS		256
#define MD3_MAX_VERTS		4096
#define MD3_MAX_TRIANGLES	8192

/* MD3 binary format structures */
typedef struct {
	int		ident;		/* MD3_IDENT */
	int		version;	/* MD3_VERSION */
	char		name[64];	/* model name, as the exporter wrote it */
	int		flags;		/* unused */
	int		num_frames;	/* animation frames */
	int		num_tags;	/* tags per frame (attachment points; unused here) */
	int		num_surfaces;	/* number of surfaces */
	int		num_skins;	/* unused; the surfaces carry the shaders */
	int		ofs_frames;	/* offset to md3Frame_t[num_frames] */
	int		ofs_tags;	/* offset to tag data */
	int		ofs_surfaces;	/* offset to the first md3Surface_t */
	int		ofs_end;	/* end of file */
} md3Header_t;

typedef struct {
	vec3_t		bounds[2];	/* bounding box */
	vec3_t		localOrigin;	/* base origin for this frame */
	float		radius;		/* radius */
	char		name[16];	/* frame name */
} md3Frame_t;

/* Every offset in here is relative to the START OF THE SURFACE, not to the
 * file -- including ofs_end, which is both this surface's length and where
 * the next one begins. */
typedef struct {
	int		ident;		/* MD3_IDENT again */
	char		name[64];	/* surface name */
	int		flags;		/* unused */
	int		num_frames;	/* must match the header's */
	int		num_shaders;	/* number of shaders */
	int		num_verts;	/* vertices per frame */
	int		num_triangles;	/* number of triangles */
	int		ofs_triangles;	/* offset to md3Triangle_t[num_triangles] */
	int		ofs_shaders;	/* offset to md3Shader_t[num_shaders] */
	int		ofs_st;		/* offset to md3St_t[num_verts] */
	int		ofs_verts;	/* offset to md3Vertex_t[num_frames*num_verts] */
	int		ofs_end;	/* surface length; offset to the next surface */
} md3Surface_t;

typedef struct {
	char		name[64];	/* shader name */
	int		shaderIndex;	/* shader index (unused in engine) */
} md3Shader_t;

typedef struct {
	int		indexes[3];	/* vertex indices */
} md3Triangle_t;

typedef struct {
	float		s, t;		/* texture coordinates */
} md3St_t;

/* On disk the normal is one little-endian short, low byte longitude and high
 * byte latitude (Quake III tr_surface.c).  Split into two bytes here so the
 * SSBO the GPU reads is byte-for-byte the file's, in the order both decoders
 * expect: normal[0] longitude, normal[1] latitude. */
typedef struct {
	short		xyz[3];		/* position (MD3_XYZ_SCALE to get float) */
	unsigned char	normal[2];	/* [0] = longitude, [1] = latitude */
} md3Vertex_t;

/* IQM (skeletal) vertex and bone data structures */
typedef struct {
	float		xyz[3];		/* rest-pose position in bone-local space */
	int8_t		norm[4];	/* compressed normal (xyz used, w padding) */
	float		st[2];		/* UV coords */
	uint8_t		weight[4];	/* normalized bone weights (sum = 255) */
	uint8_t		idx[4];		/* bone indices (up to 4 influences) */
} iqmvert_t;

/* Alias model format constants */
#define	ALIAS_IDENT		(('A') | (('L') << 8) | (('S') << 16) | (('2') << 24))
/* Hexen II native alias ("ALS2") format version. Distinct from genmodel.h's
   ALIAS_VERSION (6) -- the legacy on-disk version the genmodel tool emits,
   which the loaders still accept. Kept under a separate name so the genmodel.h
   include (line 26) no longer collides with this engine-side constant. */
#define	ALIAS_VERSION_H2	8

/* MD5mesh format constants and limits */
#define MD5_MAX_BONES		128
#define MD5_MAX_VERTS		4096
#define MD5_MAX_TRIANGLES	8192
#define MD5_MAX_FRAMES		1024

typedef struct {
	float		mat[12];	/* 3x4 row-major bone transform matrix */
} bonepose_t;

typedef struct {
	char		name[32];	/* bone name */
	int		parent;		/* parent bone index (-1 for root) */
	float		translate[3];	/* rest-pose translation */
	float		rotate[4];	/* rest-pose rotation (quaternion) */
	float		scale[3];	/* rest-pose scale */
} boneinfo_t;

#define	MAX_SKINS	32
typedef struct {
	int		ident;
	int		version;
	vec3_t		scale;
	vec3_t		scale_origin;
	float		boundingradius;
	vec3_t		eyeposition;
	int		numskins;
	int		skinwidth;
	int		skinheight;
	int		numverts;
	int		numtris;
	int		numframes;
	synctype_t	synctype;
	int		flags;
	float		size;

	int		numposes;
	int		poseverts;
	int		posedata;	// numposes*poseverts trivert_t (or md3pose_t for PV_MD3, or iqmvert_t for PV_IQM)
	int		commands;	// gl command list with embedded s/t
	poseverttype_t	poseverttype;	// PV_QUAKE1, PV_MD3, or PV_IQM (for multi-format GPU upload)

	/* Skeletal animation data (PV_IQM only) */
	int		numbones;	// number of bones (0 if not skeletal)
	int		boneinfo;	// offset to boneinfo_t array (numbones entries)
	int		bindpose;	// offset to bonepose_t array (numbones entries): rest-pose
					// world transform per bone.  boneposedata is already
					// multiplied by its inverse, so this is what puts an
					// animated bone back into model space -- see R_ShowSkeletons.
	int		boneposedata;	// offset to bonepose_t array (numposes*numbones entries)
	int		triangledata;	// offset to unsigned short[numtris*3] triangle indices (PV_IQM; 0 if not skeletal)

	GLuint		gl_texturenum[MAX_SKINS][4];
	GLuint		gl_fb_texturenum[MAX_SKINS][4];	// fullbright mask textures
	maliasframedesc_t	frames[1];	// variable sized
} aliashdr_t;

#define	MAXALIASVERTS	2000
#define	MAXALIASFRAMES	256
#define	MAXALIASTRIS	2048

/* GPU-resident alias model data for AZDO rendering */
typedef struct {
	GLuint	vao;		/* VAO for this model */
	GLuint	vbo_verts;	/* vertex attribute VBO (iqmvert_t data, PV_IQM only) */
	GLuint	vbo_tc;		/* static texcoord VBO */
	GLuint	ibo;		/* index buffer (triangulated) */
	GLuint	ssbo_pose;	/* all poses' trivertx_t data (GL 4.3, PV_QUAKE1) */
	GLuint	ssbo_pose_md3;	/* all poses' md3Vertex_t data (GL 4.3, PV_MD3) */
	GLuint	ssbo_bones;	/* bone pose matrices (GL 4.3, PV_IQM) */
	GLuint	tex_pose;	/* pose data as R32UI texture (ES 3.0 compatible, PV_QUAKE1) */
	int	num_indices;	/* total triangle indices */
	int	poseverts;	/* verts per pose (for shader indexing) */
	int	numposes;	/* total pose count */
	int	numbones;	/* number of bones (0 if not skeletal) */
	poseverttype_t poseverttype;	/* PV_QUAKE1, PV_MD3, or PV_IQM */
	qboolean valid;		/* true if GPU data was created successfully */
} alias_gpu_mesh_t;

/* Per-instance data for SSBO-based instanced alias model drawing.
 * Scale/origin baked into the world matrix CPU-side (Ironwail approach).
 * Must match the std430 SSBO layout in the instanced shader. */
typedef struct {
	float	worldmatrix[12]; /* transposed 4x3 model→world (rows) */
	float	light_color[3];	/* RGB tinted light (shade_light folded in) */
	float	alpha;		/* entity alpha */
	int	pose0;		/* current pose * poseverts */
	int	pose1;		/* previous pose * poseverts */
	float	blend;		/* pose interpolation (1=pose0, 0=pose1) */
	int	shadedot_row;	/* index into shadedots table (0-15) */
} alias_instance_t;		/* 80 bytes */

/* alias_inst_header_t removed in uhexen2-8pc2: the view-projection
 * matrix moved out of the instance SSBO into a uniform mat4 so the
 * streaming ring (gl_buffer.c) uploads only the instance array. */

/* Cap matches MAX_VISEDICTS — every visedict could in principle be an
 * alias model.  Smaller caps silently truncate the instanced draw on
 * dense maps. */
#define MAX_ALIAS_INSTANCES	16384
#define MAX_ALIAS_BATCHES	256

typedef struct {
	aliashdr_t	*hdr;		/* model -- determines VAO + pose texture */
	GLuint		skin_tex;	/* resolved skin texture */
	GLuint		fb_tex;		/* fullbright texture (0 if none) */
	int		first;		/* first instance index */
	int		count;		/* number of instances */
} alias_batch_t;

#define MAX_ALIAS_MODELS 256
extern alias_gpu_mesh_t alias_gpu_meshes[MAX_ALIAS_MODELS];
extern int num_alias_gpu_meshes;

alias_gpu_mesh_t *GL_GetAliasGPUMesh (aliashdr_t *hdr);
void GL_MakeAliasGPUMesh (aliashdr_t *hdr);
void GL_FreeAliasGPUMeshes (void);
extern	aliashdr_t	*pheader;

/* MD5mesh skeletal model loader.  out_mins/out_maxs receive the union of the
 * sibling .md5anim's per-frame bounds, or an all-zero box when the model has
 * no animation (or the anim carried no bounds block) — the caller then falls
 * back to the rest-pose vertex extent.  Both may be NULL. */
aliashdr_t *MD5_LoadMesh(const char *name, const unsigned char *buffer, int size,
                         vec3_t out_mins, vec3_t out_maxs);
/* md3mesh.c -- uhexen2-2ah9.  Produces a PV_MD3 aliashdr_t; out_mins/out_maxs
 * receive the union of the file's own per-frame bounding boxes. */
aliashdr_t *MD3_LoadMesh (const char *name, const byte *buffer, int size,
			  vec3_t out_mins, vec3_t out_maxs);
void MD3_DecodeNormal (const md3Vertex_t *v, vec3_t out);
extern	stvert_t	stverts[MAXALIASVERTS];
extern	mtriangle_t	triangles[MAXALIASTRIS];
extern	trivertx_t	*poseverts[MAXALIASFRAMES];

//===================================================================

//
// entity effects
//
#ifndef H2W /* see below for hexenworld */
#define	EF_BRIGHTFIELD			0x00000001
#endif
#define	EF_MUZZLEFLASH			0x00000002
#define	EF_BRIGHTLIGHT			0x00000004
#define	EF_DIMLIGHT			0x00000008
#define	EF_DARKLIGHT			0x00000010
#define	EF_DARKFIELD			0x00000020
#define	EF_LIGHT			0x00000040
#define	EF_NODRAW			0x00000080

#ifdef H2W
/* The only difference between Raven's hw-0.15 binary release and the
 * later HexenC source release is the EF_BRIGHTFIELD and EF_ONFIRE values:
 * the original binary releases had them as 1 and 1024 respectively, but
 * the later hcode src releases have them flipped: EF_BRIGHTFIELD = 1024
 * and EF_ONFIRE = 1, which is a BIG BOO BOO. (On the other hand, Siege
 * binary and source releases have EF_BRIGHTFIELD and EF_ONFIRE values as
 * 1 and 1024, which makes the mess even messier.. Sigh..)
 * The hexenworld engine src release also have EF_BRIGHTFIELD as 1024 and
 * EF_ONFIRE as 1, therefore uHexen2 sticks to those values.
 */
#define	EF_ONFIRE			0x00000001
#define	EF_BRIGHTFIELD			0x00000400
#define	EF_POWERFLAMEBURN		0x00000800
#define	EF_UPDATESOUND			0x00002000

#define	EF_POISON_GAS			0x00200000
#define	EF_ACIDBLOB			0x00400000
//#define	EF_PURIFY2_EFFECT		0x00200000
//#define	EF_AXE_EFFECT			0x00400000
//#define	EF_SWORD_EFFECT			0x00800000
//#define	EF_TORNADO_EFFECT		0x01000000
#define	EF_ICESTORM_EFFECT		0x02000000
//#define	EF_ICEBALL_EFFECT		0x04000000
//#define	EF_METEOR_EFFECT		0x08000000
#define	EF_HAMMER_EFFECTS		0x10000000
#define	EF_BEETLE_EFFECTS		0x20000000
#endif /* H2W */

//===================================================================

//
// Whole model
//

typedef enum {mod_brush, mod_sprite, mod_alias} modtype_t;

/* EF_ changes must also be made in both model.h and gl_model.h
   and you MUST check with the constants in gamecode, as well. */

#define	EF_ROCKET		(1 << 0 )	/* leave a trail				*/
#define	EF_GRENADE		(1 << 1 )	/* leave a trail				*/
#define	EF_GIB			(1 << 2 )	/* leave a trail				*/
#define	EF_ROTATE		(1 << 3 )	/* rotate (bonus items)				*/
#define	EF_TRACER		(1 << 4 )	/* green split trail				*/
#define	EF_ZOMGIB		(1 << 5 )	/* small blood trail				*/
#define	EF_TRACER2		(1 << 6 )	/* orange split trail + rotate			*/
#define	EF_TRACER3		(1 << 7 )	/* purple trail					*/
#define	EF_FIREBALL		(1 << 8 )	/* Yellow transparent trail in all directions	*/
#define	EF_ICE			(1 << 9 )	/* Blue-white transparent trail, with gravity	*/
#define	EF_MIP_MAP		(1 << 10)	/* This model has mip-maps			*/
#define	EF_SPIT			(1 << 11)	/* Black transparent trail with negative light	*/
#define	EF_TRANSPARENT		(1 << 12)	/* Transparent sprite				*/
#define	EF_SPELL		(1 << 13)	/* Vertical spray of particles			*/
#define	EF_HOLEY		(1 << 14)	/* Solid model with color 0			*/
#define	EF_SPECIAL_TRANS	(1 << 15)	/* Translucency through the particle table	*/
#define	EF_FACE_VIEW		(1 << 16)	/* Poly Model always faces you			*/
#define	EF_VORP_MISSILE		(1 << 17)	/* leave a trail at top and bottom of model	*/
#define	EF_SET_STAFF		(1 << 18)	/* slowly move up and left/right		*/
#define	EF_MAGICMISSILE		(1 << 19)	/* a trickle of blue/white particles with gravity	*/
#define	EF_BONESHARD		(1 << 20)	/* a trickle of brown particles with gravity		*/
#define	EF_SCARAB		(1 << 21)	/* white transparent particles with little gravity	*/
#define	EF_ACIDBALL		(1 << 22)	/* Green drippy acid shit				*/
#define	EF_BLOODSHOT		(1 << 23)	/* Blood rain shot trail				*/

#define	EF_MIP_MAP_FAR		(1 << 24)	/* Set per frame, this model will use the far mip map	*/

/* Engine-set model flags (placed above the H2 EF_* range to avoid aliasing
 * the on-disk MDL flags read into qmodel_t->flags by Mod_LoadAliasModel*).
 * Set by Mod_SetExtraFlags via the r_nolerp_list cvar; Ironwail port. */
#define	MOD_NOLERP		(1 << 25)	/* don't lerp animation (torches, flames, v_weapons) */
#define	MOD_NOSHADOW		(1 << 26)	/* excluded from r_shadows by r_noshadow_list */

#define	EF_SPIN			(1 << 4)	/* Inky: Rotate without floating upside down */
#define	EF_FLOAT			(1 << 5)	/* Inky: Float upside down without rotating */
#define	EF_GLOW				(1 << 6)	/* Inky: Feature a custom glowing orb around the model, override the various XF_*GLOW presets */
#define	EF_ILLUMINATE		(1 << 7)	/* Inky: Cast light dynamically around */

// slots for qmodel_t->glow_settings
#define	GLOW_SETTINGS_COUNT 10
#define	COLOR_R 0
#define	COLOR_G 1
#define	COLOR_B 2
#define	COLOR_A 3
#define	ORB_OFFSET_X 4
#define	ORB_OFFSET_Y 5
#define	ORB_OFFSET_Z 6
#define	ORB_RADIUS 7
#define	LIGHT_STYLE 8
#define	LIGHT_RADIUS 9

// Per-entity PimpModel overrides
typedef struct {
	qboolean	active;
	int		ex_flags;
	int		trail_flags;	// per-entity model flags (EF_ROCKET, EF_GIB, etc.)
	qboolean	trail_override;	// true if trail_flags should replace model->flags
	float		glow_settings[GLOW_SETTINGS_COUNT];
} pimp_override_t;

void R_ClearPimpOverrides (void);
pimp_override_t *R_GetPimpOverride (int entnum);
int R_GetEntityModelFlags (entity_t *e);

// XF_ Extra model effects set by engine: qmodel_t->ex_flags
// effects are model name dependent
#define XF_TORCH_GLOW		(1 << 0 )	/* glowing torches				*/
#define XF_TORCH_GLOW_EGYPT	(1 << 30)	/* glowing torches, egypt			*/
#define XF_GLOW			(1 << 1 )	/* other glows					*/
#define XF_MISSILE_GLOW		(1 << 2 )	/* missile glows				*/
/* Suppress the dynamic light a placed light-source model would otherwise cast.
 * Hexen II's dlights do not test occlusion, so a torch or cast-light near a
 * wall spills onto whatever is behind it; with no shadow-casting in the engine
 * (see uhexen2-kd3g) the only lever a mapper has is to take a specific light
 * out of the dlight set and let the baked lightmap carry it.  Set from
 * misc_modelpimp spawnflag 64.  uhexen2-kd3g */
#define XF_NO_DLIGHT		(1 << 8 )	/* don't cast a dynamic light			*/

typedef struct qmodel_s
{
	char		name[MAX_QPATH];
	unsigned int	path_id;		// path id of the game directory
							// that this model came from
	int		needload;		// bmodels and sprites don't cache normally

	modtype_t	type;
	int		numframes;
	synctype_t	synctype;

	int		flags;
	int		ex_flags;
	qboolean	has_sky_surf;	/* cached at load: any surface in this submodel has SURF_DRAWSKY */
	/* Any liquid face in this model carries lightmap samples -- i.e. the map
	 * was compiled with lit water (ericw-tools, on by default since 2019).
	 * Ironwail gl_model.h:504.  Cached so the draw path can ask the question
	 * once per map instead of per surface.  uhexen2-a5nn.2. */
	qboolean	haslitwater;

//
// volume occupied by the model graphics
//
	vec3_t		mins, maxs;
	float		radius;

//
// brush model
//
	int		firstmodelsurface, nummodelsurfaces;

	int		numsubmodels;
	dmodel_t	*submodels;

	int		numplanes;
	mplane_t	*planes;

	int		numleafs;		// number of visible leafs, not counting 0
	mleaf_t		*leafs;

	int		numvertexes;
	mvertex_t	*vertexes;

	int		numedges;
	medge_t		*edges;

	int		numnodes;
	mnode_t		*nodes;

	int		numtexinfo;
	mtexinfo_t	*texinfo;

	int		numsurfaces;
	msurface_t	*surfaces;

	int		numsurfedges;
	int		*surfedges;

	int		numclipnodes;
	mclipnode_t	*clipnodes;

	int		nummarksurfaces;
	msurface_t	**marksurfaces;

	hull_t		hulls[MAX_MAP_HULLS];

	int		numtextures;
	texture_t	**textures;

	byte		*visdata;
	byte		*lightdata;
	char		*entities;

//
// additional model data
//
	float		glow_settings[GLOW_SETTINGS_COUNT];
	float		glow_color[4];		// RGBA color for glow effect

	/* Max (per-vertex peak displacement in world units, divided by the
	 * model's bbox diagonal) across every pose pair that R_AliasResolveLerp
	 * actually blends — intra-multi-pose-group adjacencies plus adjacent
	 * single-pose-frame pairs.  Computed at load (Mod_ComputeFlipbookRatio)
	 * and read by Mod_SetExtraFlags: ratios above r_lerp_autodetect_threshold
	 * mean adjacent poses are unrelated geometry sheets (flipbook flames,
	 * torches, waterfalls), not deformations of one mesh, so morph
	 * interpolation smears them — MOD_NOLERP suppresses the blend.  Zero
	 * for non-alias models or models with one pose.  uhexen2-f807. */
	float		flipbook_max_ratio;

	/* True when this model is EF_HOLEY but its replacement skin carries
	 * authored, graduated alpha rather than a binary cutout mask.
	 *
	 * EF_HOLEY means "index 0 is a hole" -- a genuinely binary statement
	 * about an 8-bit palette skin, and the two places that act on it treat
	 * it as such: GL_Upload32 binarizes alpha at 128 for TEX_HOLEY|TEX_RGBA
	 * so the mask is clean, and R_DrawAliasModel draws the model with
	 * blending off behind a 0.666 alpha test.  Both are right for a cutout
	 * and destroy a translucent skin.  An RGBA replacement has no palette
	 * indices left, so a pack is free to ship soft alpha under an EF_HOLEY
	 * header, and SoT's sot/models/mist_0.tga does exactly that: 123 alpha
	 * levels, 85% of texels partially transparent, and not one fully
	 * opaque texel in the image (max alpha 183).  Run through the cutout
	 * path it lost 95% of itself and drew the remainder solid.
	 *
	 * Set at skin load by Mod_LoadAllSkins, which then withholds TEX_HOLEY
	 * (keeping TEX_ALPHA) so the upload leaves the ramp alone, and read by
	 * R_DrawAliasModel to take the blended branch instead of the alpha
	 * test.  uhexen2-5zv5. */
	qboolean	skin_soft_alpha;

	/* True when this model's skin came from an external RGBA replacement
	 * rather than the palette-indexed skin in the .mdl.
	 *
	 * It exists for EF_SPECIAL_TRANS.  That flag means "translucency through
	 * the particle table": the 8-bit upload writes ColorPercent[] into the
	 * alpha channel, and ColorPercent is a TRANSPARENCY, so the draw path
	 * pairs it with a reversed blend func (GL_ONE_MINUS_SRC_ALPHA,
	 * GL_SRC_ALPHA).  The two halves agree and Raven's content looks right.
	 *
	 * A replacement TGA/PNG carries the universal convention instead --
	 * alpha is OPACITY, 255 means opaque -- so the same reversed func renders
	 * it inside out.  That is BloodShot's "tga alpha channels are inverted
	 * for these flags", and why EF_HOLEY models were unaffected: only
	 * EF_SPECIAL_TRANS reverses the func.  uhexen2-4y6w. */
	qboolean	skin_replaced;

	/* Snapshot of the model's load-time flags / ex_flags / glow_settings,
	 * captured at the end of Mod_LoadAliasModel{,New}.  PimpModel writes
	 * through to mod->flags etc. so misc_modelpimp can change rendering
	 * for every entity sharing this model on the current map; on map
	 * change Mod_RestoreAliasModelDefaults walks mod_known[] and resets
	 * to these snapshots so the next map starts from MDL defaults.
	 * uhexen2-oq0a. */
	int		orig_flags;
	int		orig_ex_flags;
	float		orig_glow_settings[GLOW_SETTINGS_COUNT];
	qboolean	orig_state_saved;

	/* MD5mesh models are mod_alias but keep their aliashdr_t on the hunk
	 * (like sprites do) instead of in the cache heap.  Cache_Check /
	 * Cache_Free derive a cache_system_t from the 16 bytes preceding
	 * cache.data and splice the LRU chain through them, so they must not
	 * run on a hunk pointer.  uhexen2-zjux. */
	qboolean	cache_is_hunk;

	/* This slot stands in for a file that is not on disk.  Set by
	 * Mod_ForNamePlaceholder and honoured by Mod_LoadModel so that a cache
	 * eviction regenerates the mesh instead of retrying the missing file. */
	qboolean	is_placeholder;

	cache_user_t	cache;		// only access through Mod_Extradata
} qmodel_t;

// values for qmodel_t->needload
#define	NL_PRESENT		0
#define	NL_NEEDS_LOADED		1
#define	NL_UNREFERENCED		2

//============================================================================

void	Mod_Init (void);
void	Mod_ClearAll (void);

/* True when any loaded brush model has a _norm/_bump or _gloss sidecar.
 * Sampled once per map by R_NewMap to decide whether the world shader's
 * material path runs at all.  uhexen2-mfql. */
qboolean Mod_MaterialMapsPresent (void);
void	Mod_SaveAliasModelDefaults (qmodel_t *mod);	/* uhexen2-oq0a */
void	Mod_RestoreAliasModelDefaults (void);		/* uhexen2-oq0a */
void	Mod_SetExtraFlags (qmodel_t *mod);		/* Ironwail r_nolerp_list */
qmodel_t *Mod_ForName (const char *name, qboolean crash);
/* For gamecode-driven precaches only: substitutes a checkerboard box and
 * warns instead of dying when the file is absent.  Engine assets keep
 * Mod_ForName(..., true) -- see the note on the definition. */
qmodel_t *Mod_ForNamePlaceholder (const char *name);
qmodel_t *Mod_FindName (const char *name);
void	*Mod_Extradata (qmodel_t *mod);	// handles caching
void	Mod_TouchModel (const char *name);
void	Mod_ReloadTextures (void);
void	Mod_ReuploadAliasSkins (qmodel_t *mod);

mleaf_t *Mod_PointInLeaf (vec3_t p, qmodel_t *model);
byte	*Mod_LeafPVS (mleaf_t *leaf, qmodel_t *model);

#endif	/* GL_MODEL_H */

