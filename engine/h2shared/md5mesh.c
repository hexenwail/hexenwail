/* md5mesh.c -- MD5 skeletal model loader (Ironwail parity)
 *
 * Loads MD5mesh files into aliashdr_t with PV_IQM pose vertices, and the
 * sibling .md5anim (same base name) into the per-pose bone matrices the
 * skeletal shader reads out of ssbo_bones.  One MD5 animation frame becomes
 * one aliashdr_t frame with a single pose, so e->frame from the server
 * indexes it exactly like a .mdl and the existing lerp machinery in
 * R_AliasResolveLerp applies unchanged.  uhexen2-7ok0.2.
 */

#include "quakedef.h"
#include "gl_model.h"
#include "img_load.h"
#include "gl_shader.h"	/* gl_solid_white_texture, the missing-skin fallback */

#define MD5_MAX_WEIGHTS_PER_VERT	16	/* generous; real MD5 rarely exceeds 4-8 */

typedef struct {
	int		bone;
	float		factor;
	float		pos[3];		/* position in bone-local space */
} md5weight_t;

/* Skip to the start of the next line. */
static const char *MD5_SkipLine(const char *p)
{
	while (*p && *p != '\n') p++;
	if (*p == '\n') p++;
	return p;
}

/* strchr that returns p unchanged on miss so subsequent code can detect
 * the failure via the original cursor rather than dereferencing NULL+1. */
static const char *MD5_FindChar(const char *p, char c)
{
	const char *q = strchr(p, c);
	return q ? q : NULL;
}

/* Build a 3x4 row-major affine transform from a unit quaternion + translation.
 * Layout matches bonepose_t.mat: rows of (rotation, translate-component).
 * The shader reads this as a std430 mat3x4 (3 columns of vec4) and
 * transpose()s into a mat4x3 before applying — so the row-major write here
 * and the column-major read there cancel correctly. */
static void MD5_QuatTransToMat3x4(const float q[4], const float t[3], float m[12])
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float xx = x*x, yy = y*y, zz = z*z;
	float xy = x*y, xz = x*z, yz = y*z;
	float wx = w*x, wy = w*y, wz = w*z;

	m[0]  = 1.0f - 2.0f*(yy + zz);
	m[1]  = 2.0f*(xy - wz);
	m[2]  = 2.0f*(xz + wy);
	m[3]  = t[0];

	m[4]  = 2.0f*(xy + wz);
	m[5]  = 1.0f - 2.0f*(xx + zz);
	m[6]  = 2.0f*(yz - wx);
	m[7]  = t[1];

	m[8]  = 2.0f*(xz - wy);
	m[9]  = 2.0f*(yz + wx);
	m[10] = 1.0f - 2.0f*(xx + yy);
	m[11] = t[2];
}

/* Identity 3x4. */
static void MD5_IdentityMat3x4(float m[12])
{
	m[0]=1; m[1]=0; m[2]=0;  m[3]=0;
	m[4]=0; m[5]=1; m[6]=0;  m[7]=0;
	m[8]=0; m[9]=0; m[10]=1; m[11]=0;
}

/* dst = a * b, both 3x4 row-major affine.  Used to fold parent transforms
 * into children so the per-bone rest-pose world transform is correct. */
static void MD5_MulMat3x4(const float a[12], const float b[12], float dst[12])
{
	/* row r of result = a's row r times b (as 4x4 with last row 0 0 0 1). */
	for (int r = 0; r < 3; r++)
	{
		dst[r*4 + 0] = a[r*4+0]*b[0] + a[r*4+1]*b[4] + a[r*4+2]*b[8];
		dst[r*4 + 1] = a[r*4+0]*b[1] + a[r*4+1]*b[5] + a[r*4+2]*b[9];
		dst[r*4 + 2] = a[r*4+0]*b[2] + a[r*4+1]*b[6] + a[r*4+2]*b[10];
		dst[r*4 + 3] = a[r*4+0]*b[3] + a[r*4+1]*b[7] + a[r*4+2]*b[11] + a[r*4+3];
	}
}

/* Apply 3x4 affine to a position. */
static void MD5_TransformPoint(const float m[12], const float p[3], float out[3])
{
	out[0] = m[0]*p[0] + m[1]*p[1] + m[2]*p[2]  + m[3];
	out[1] = m[4]*p[0] + m[5]*p[1] + m[6]*p[2]  + m[7];
	out[2] = m[8]*p[0] + m[9]*p[1] + m[10]*p[2] + m[11];
}

/* Inverse of a rigid 3x4 affine: R is orthonormal, so R⁻¹ = Rᵀ and the
 * translation inverts as -Rᵀt.  MD5 joints carry rotation and translation
 * only -- no scale -- so the general 4x4 inverse is never needed, and the
 * transpose form is both exact and free of a determinant that could be
 * near zero. */
static void MD5_InvertRigidMat3x4(const float m[12], float dst[12])
{
	dst[0] = m[0];  dst[1] = m[4];  dst[2]  = m[8];
	dst[4] = m[1];  dst[5] = m[5];  dst[6]  = m[9];
	dst[8] = m[2];  dst[9] = m[6];  dst[10] = m[10];

	dst[3]  = -(dst[0]*m[3] + dst[1]*m[7] + dst[2] *m[11]);
	dst[7]  = -(dst[4]*m[3] + dst[5]*m[7] + dst[6] *m[11]);
	dst[11] = -(dst[8]*m[3] + dst[9]*m[7] + dst[10]*m[11]);
}

/* Reconstruct the implicit W of an id-Software MD5 quaternion.  Both the
 * .md5mesh joints and the .md5anim baseframe/frame components store only
 * X/Y/Z; W is the negative root, which is the convention id's own
 * Quat_computeW uses and the one MD5_QuatTransToMat3x4 above is written
 * against.  Getting the sign wrong here silently applies the conjugate --
 * every bone rotates the correct amount the wrong way. */
static float MD5_QuatW(float x, float y, float z)
{
	float ww = 1.0f - x*x - y*y - z*z;
	return (ww > 0.0f) ? -sqrtf(ww) : 0.0f;
}

/* ------------------------------------------------------------------ */
/* .md5anim tokenizer                                                  */
/*                                                                     */
/* The frame blocks are a free-form stream of floats with no guaranteed */
/* line structure, and every hierarchy line carries a trailing          */
/* `// parent` comment, so the sscanf-per-line style the mesh parser    */
/* uses does not survive here.  Scan tokens instead: whitespace and the */
/* purely decorative '(' / ')' are separators, '//' runs to end of      */
/* line, and '{' / '}' are left in the stream so block structure stays  */
/* checkable.                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	const char	*p;
} md5tok_t;

static void MD5_SkipFluff(md5tok_t *t)
{
	for (;;)
	{
		while (*t->p && (unsigned char)*t->p <= ' ')
			t->p++;
		if (t->p[0] == '/' && t->p[1] == '/')
		{
			while (*t->p && *t->p != '\n')
				t->p++;
			continue;
		}
		if (*t->p == '(' || *t->p == ')')
		{
			t->p++;
			continue;
		}
		break;
	}
}

static qboolean MD5_ReadFloat(md5tok_t *t, float *out)
{
	char *end;

	MD5_SkipFluff(t);
	*out = (float)strtod(t->p, &end);
	if (end == t->p)
		return false;
	t->p = end;
	return true;
}

static qboolean MD5_ReadInt(md5tok_t *t, int *out)
{
	float f;

	if (!MD5_ReadFloat(t, &f))
		return false;
	*out = (int)f;
	return true;
}

static qboolean MD5_ReadName(md5tok_t *t, char *out, size_t outsz)
{
	size_t n = 0;

	MD5_SkipFluff(t);
	if (*t->p != '"')
		return false;
	t->p++;
	while (*t->p && *t->p != '"')
	{
		if (n + 1 < outsz)
			out[n++] = *t->p;
		t->p++;
	}
	out[n] = 0;
	if (*t->p != '"')
		return false;
	t->p++;
	return true;
}

static qboolean MD5_Expect(md5tok_t *t, char c)
{
	MD5_SkipFluff(t);
	if (*t->p != c)
		return false;
	t->p++;
	return true;
}

/* Find `keyword` as a whole token at or after `from`.  A bare strstr would
 * match "frame" inside both "baseframe" and "numFrames"; the mesh parser
 * gets away with that style only because its sections happen to be ordered
 * so the false hits land harmlessly. */
static const char *MD5_FindKeyword(const char *from, const char *keyword)
{
	size_t	klen = strlen(keyword);
	const char *p = from;

	while ((p = strstr(p, keyword)) != NULL)
	{
		char after = p[klen];
		/* Deliberately no p[-1] read when p == from: `from` can be the
		 * start of the buffer.  Callers always resume on a token
		 * boundary, so accepting a match there is correct. */
		qboolean start_ok = (p == from) || ((unsigned char)p[-1] <= ' ');

		if (start_ok && (after == 0 || (unsigned char)after <= ' '))
			return p;
		p += klen;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* .md5anim parsing                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
	int		numframes;
	float		framerate;
	bonepose_t	*poses;		/* numframes * numbones, malloc'd */
	vec3_t		mins, maxs;	/* union of the per-frame bounds block */
	qboolean	have_bounds;
} md5anim_t;

/* Per-joint animation channel description from the `hierarchy` block. */
typedef struct {
	int	parent;
	int	flags;		/* bit 0..5 = tx ty tz qx qy qz are animated */
	int	start;		/* first index into a frame's component array */
} md5channel_t;

/* Parse `text` into out->poses, one bonepose_t per (frame, bone).
 *
 * Each stored matrix is `frame_world[b] * inv_rest[b]` -- the transform that
 * carries a vertex from its REST world position (which is what md5mesh
 * bakes into iqmvert_t.xyz) to its animated world position.  At the rest
 * pose the two cancel to identity, which is exactly the fallback the
 * no-anim path writes, so both paths agree on what the shader is applying.
 *
 * `numbones` and the joint order must match the mesh; an exporter emits the
 * two files together, and a mismatch means the pair does not belong to each
 * other. */
static qboolean MD5_ParseAnim(const char *text, const boneinfo_t *bones,
                              int numbones, const float (*inv_rest)[12],
                              md5anim_t *out)
{
	md5tok_t	t;
	const char	*p;
	int		numframes = 0, numjoints = 0, numcomps = 0;
	int		framerate = 24;
	md5channel_t	*chan = NULL;
	float		*basepos = NULL, *baserot = NULL;
	float		*comps = NULL;
	float		(*world)[12] = NULL;
	qboolean	ok = false;
	int		i, f, b;

	memset(out, 0, sizeof(*out));

	p = MD5_FindKeyword(text, "numFrames");
	if (!p) return false;
	numframes = atoi(p + 9);
	if (numframes <= 0 || numframes > MD5_MAX_FRAMES)
		return false;

	p = MD5_FindKeyword(text, "numJoints");
	if (!p) return false;
	numjoints = atoi(p + 9);
	if (numjoints != numbones)
		return false;

	p = MD5_FindKeyword(text, "frameRate");
	if (p)
	{
		framerate = atoi(p + 9);
		if (framerate <= 0)
			framerate = 24;
	}

	p = MD5_FindKeyword(text, "numAnimatedComponents");
	if (!p) return false;
	numcomps = atoi(p + 21);
	if (numcomps < 0 || numcomps > numjoints * 6)
		return false;

	chan    = (md5channel_t *) calloc(numjoints, sizeof(md5channel_t));
	basepos = (float *)        calloc(numjoints * 3, sizeof(float));
	baserot = (float *)        calloc(numjoints * 4, sizeof(float));
	world   = (float (*)[12])  calloc(numjoints, sizeof(float[12]));
	/* calloc(0) may legitimately return NULL, and a pose-locked animation
	 * with every channel constant has numcomps == 0.  Ask for one element
	 * so the NULL check below stays a real out-of-memory test. */
	comps   = (float *)        calloc(numcomps > 0 ? numcomps : 1, sizeof(float));
	out->poses = (bonepose_t *) calloc((size_t)numframes * numbones, sizeof(bonepose_t));

	if (!chan || !basepos || !baserot || !world || !comps || !out->poses)
		goto done;

	/* --- hierarchy: parent + animated-channel layout per joint --- */
	p = MD5_FindKeyword(text, "hierarchy");
	if (!p) goto done;
	t.p = p + 9;
	if (!MD5_Expect(&t, '{')) goto done;
	for (i = 0; i < numjoints; i++)
	{
		char name[64];

		if (!MD5_ReadName(&t, name, sizeof(name))) goto done;
		if (!MD5_ReadInt(&t, &chan[i].parent)) goto done;
		if (!MD5_ReadInt(&t, &chan[i].flags)) goto done;
		if (!MD5_ReadInt(&t, &chan[i].start)) goto done;

		/* Parent must already be resolved when this joint is composed --
		 * the same forward-reference guarantee MD5_LoadMesh checks on the
		 * mesh side, and what lets the single forward pass below stand in
		 * for a recursive walk. */
		if (chan[i].parent >= i)
			goto done;
		if (chan[i].parent != bones[i].parent)
			goto done;	/* skeletons disagree: wrong anim for this mesh */
		if (chan[i].start < 0 || chan[i].start > numcomps)
			goto done;
	}
	if (!MD5_Expect(&t, '}')) goto done;

	/* --- bounds: one (min)(max) pair per frame, in model space --- */
	p = MD5_FindKeyword(t.p, "bounds");
	if (p)
	{
		md5tok_t bt;

		bt.p = p + 6;
		if (MD5_Expect(&bt, '{'))
		{
			qboolean good = true;

			for (f = 0; f < numframes && good; f++)
			{
				float v[6];

				for (i = 0; i < 6; i++)
				{
					if (!MD5_ReadFloat(&bt, &v[i]))
					{
						good = false;
						break;
					}
				}
				if (!good)
					break;
				if (!out->have_bounds)
				{
					VectorSet(out->mins, v[0], v[1], v[2]);
					VectorSet(out->maxs, v[3], v[4], v[5]);
					out->have_bounds = true;
				}
				else
				{
					for (i = 0; i < 3; i++)
					{
						if (v[i]   < out->mins[i]) out->mins[i] = v[i];
						if (v[3+i] > out->maxs[i]) out->maxs[i] = v[3+i];
					}
				}
			}
			/* A malformed bounds block costs us the culling box, not the
			 * animation: fall back to the rest-pose bbox rather than
			 * refusing to load. */
			if (!good)
				out->have_bounds = false;
		}
	}

	/* --- baseframe: the value every non-animated channel keeps --- */
	p = MD5_FindKeyword(t.p, "baseframe");
	if (!p) goto done;
	t.p = p + 9;
	if (!MD5_Expect(&t, '{')) goto done;
	for (i = 0; i < numjoints; i++)
	{
		float	q[3];

		if (!MD5_ReadFloat(&t, &basepos[i*3 + 0])) goto done;
		if (!MD5_ReadFloat(&t, &basepos[i*3 + 1])) goto done;
		if (!MD5_ReadFloat(&t, &basepos[i*3 + 2])) goto done;
		if (!MD5_ReadFloat(&t, &q[0])) goto done;
		if (!MD5_ReadFloat(&t, &q[1])) goto done;
		if (!MD5_ReadFloat(&t, &q[2])) goto done;

		baserot[i*4 + 0] = q[0];
		baserot[i*4 + 1] = q[1];
		baserot[i*4 + 2] = q[2];
		baserot[i*4 + 3] = MD5_QuatW(q[0], q[1], q[2]);
	}
	if (!MD5_Expect(&t, '}')) goto done;

	/* --- frame N { ... }: numAnimatedComponents floats each --- */
	for (f = 0; f < numframes; f++)
	{
		int	index = -1;

		p = MD5_FindKeyword(t.p, "frame");
		if (!p) goto done;
		t.p = p + 5;
		if (!MD5_ReadInt(&t, &index)) goto done;
		if (index != f) goto done;	/* frames must arrive in order */
		if (!MD5_Expect(&t, '{')) goto done;

		for (i = 0; i < numcomps; i++)
		{
			if (!MD5_ReadFloat(&t, &comps[i]))
				goto done;
		}
		if (!MD5_Expect(&t, '}')) goto done;

		for (b = 0; b < numjoints; b++)
		{
			float	pos[3], q[4], local[12];
			int	k = chan[b].start;

			VectorCopy(&basepos[b*3], pos);
			q[0] = baserot[b*4 + 0];
			q[1] = baserot[b*4 + 1];
			q[2] = baserot[b*4 + 2];

			/* Channel bits are consumed in x,y,z,qx,qy,qz order from the
			 * joint's start index -- the layout the exporter promised in
			 * the hierarchy block.  Bounds were validated above, but each
			 * read is still guarded: `start + popcount(flags)` is what
			 * must fit, and only the file claims those agree. */
			if ((chan[b].flags & 1)  && k < numcomps) pos[0] = comps[k++];
			if ((chan[b].flags & 2)  && k < numcomps) pos[1] = comps[k++];
			if ((chan[b].flags & 4)  && k < numcomps) pos[2] = comps[k++];
			if ((chan[b].flags & 8)  && k < numcomps) q[0]   = comps[k++];
			if ((chan[b].flags & 16) && k < numcomps) q[1]   = comps[k++];
			if ((chan[b].flags & 32) && k < numcomps) q[2]   = comps[k++];

			q[3] = MD5_QuatW(q[0], q[1], q[2]);

			MD5_QuatTransToMat3x4(q, pos, local);

			if (chan[b].parent < 0)
				memcpy(world[b], local, sizeof(float[12]));
			else
				MD5_MulMat3x4(world[chan[b].parent], local, world[b]);

			MD5_MulMat3x4(world[b], inv_rest[b],
			              out->poses[(size_t)f * numbones + b].mat);
		}
	}

	out->numframes = numframes;
	out->framerate = (float)framerate;
	ok = true;

done:
	free(chan);
	free(basepos);
	free(baserot);
	free(world);
	free(comps);
	if (!ok)
	{
		free(out->poses);
		out->poses = NULL;
	}
	return ok;
}

/* Load the .md5anim that sits beside `meshname` (same path, swapped
 * extension).  Returns false when there is no anim file, which is not an
 * error: the caller falls back to a static rest pose. */
static qboolean MD5_LoadAnim(const char *meshname, const boneinfo_t *bones,
                             int numbones, const float (*inv_rest)[12],
                             md5anim_t *out)
{
	char	animname[MAX_QPATH];
	byte	*text;
	qboolean ok;
	char	*dot;

	memset(out, 0, sizeof(*out));

	if (q_strlcpy(animname, meshname, sizeof(animname)) >= sizeof(animname))
		return false;	/* truncated: the swapped name would not be this model's */

	/* A '.' that is not in the final path component is a directory name, not
	 * an extension -- swapping there would rewrite the path. */
	dot = strrchr(animname, '.');
	if (!dot || strchr(dot, '/') || strchr(dot, '\\'))
		return false;
	if ((size_t)(dot - animname) + strlen(".md5anim") + 1 > sizeof(animname))
		return false;
	strcpy(dot, ".md5anim");

	/* Deliberately the malloc loader.  The .md5mesh text this parse runs
	 * alongside is still live, and for any real model it lives on the temp
	 * hunk (FS_LoadStackFile spills there past 1KB) -- a second temp-hunk
	 * load would hand back the same memory and shred the buffer the mesh
	 * parser is reading. */
	text = FS_LoadMallocFile(animname, NULL);
	if (!text)
		return false;

	ok = MD5_ParseAnim((const char *)text, bones, numbones, inv_rest, out);
	free(text);

	if (!ok)
		Con_Printf("MD5: %s failed to parse; using rest pose\n", animname);

	return ok;
}

/* Resolve the mesh's `shader` string to a skin texture.  MD5 has no palette
 * concept, so this is the external-image path only -- a model with no
 * matching image on disk still loads and draws untextured rather than
 * failing. */
static void MD5_LoadSkin(aliashdr_t *hdr, const char *modelname, const char *shader)
{
	char		stripped[MAX_QPATH];
	char		*dot, *slash;
	byte		*rgba;
	int		w = 0, h = 0, i;
	qboolean	has_alpha = false;
	GLuint		tex;

	q_strlcpy(stripped, shader, sizeof(stripped));

	/* Exporters write the material path with whatever extension the source
	 * art had.  IMG_LoadExternalTexture appends its own candidates, so
	 * leaving one on produces "skin.tga.tga" and no hits. */
	dot = strrchr(stripped, '.');
	slash = strrchr(stripped, '/');
	if (dot && (!slash || dot > slash))
		*dot = '\0';

	rgba = stripped[0] ? IMG_LoadExternalTexture(stripped, &w, &h, &has_alpha) : NULL;
	if (rgba)
	{
		tex = GL_LoadTexture(stripped, rgba, w, h,
		                     TEX_MIPMAP | TEX_RGBA | (has_alpha ? TEX_ALPHA : 0));
		free(rgba);
	}
	else
	{
		/* Fall back to the 1x1 white sentinel rather than leaving numskins
		 * at 0.  The alias draw path binds gl_texturenum[skin][anim]
		 * unconditionally, and a zero there is texture object 0 -- which a
		 * core profile leaves undefined, so a missing skin would read as
		 * black or garbage instead of as an obviously untextured model. */
		Con_DPrintf("MD5: no skin image for %s (shader \"%s\")\n", modelname, shader);
		tex = gl_solid_white_texture;
	}

	if (!tex)
		return;

	/* One skin, and the same texture in all four animation slots: the
	 * alias draw path indexes gl_texturenum[skin][(int)(cl.time*10)&3]
	 * unconditionally, so leaving slots 1..3 at zero would strobe the
	 * model untextured three frames out of four. */
	hdr->numskins = 1;
	for (i = 0; i < 4; i++)
		hdr->gl_texturenum[0][i] = tex;
}

/* Take the top-4 weights for a vertex by magnitude and pack into
 * iqmvert_t (idx[4], weight[4] normalized to sum 255).  When the vertex
 * has more than 4 influences, the truncated mass is redistributed across
 * the top 4 instead of being silently dropped.  Returns false if there
 * are no weights at all. */
static qboolean MD5_PackTopFourWeights(const md5weight_t *src, int n, iqmvert_t *dst)
{
	int top[4] = { -1, -1, -1, -1 };
	float top_w[4] = { 0, 0, 0, 0 };

	for (int i = 0; i < n; i++)
	{
		float w = src[i].factor;
		for (int j = 0; j < 4; j++)
		{
			if (w > top_w[j])
			{
				for (int k = 3; k > j; k--) { top_w[k] = top_w[k-1]; top[k] = top[k-1]; }
				top_w[j] = w;
				top[j] = i;
				break;
			}
		}
	}

	float sum = top_w[0] + top_w[1] + top_w[2] + top_w[3];
	if (sum <= 0.0f)
		return false;

	for (int j = 0; j < 4; j++)
	{
		if (top[j] < 0)
		{
			dst->idx[j] = 0;
			dst->weight[j] = 0;
			continue;
		}
		dst->idx[j] = (uint8_t)src[top[j]].bone;
		float nw = top_w[j] / sum;
		int q = (int)(nw * 255.0f + 0.5f);
		dst->weight[j] = (uint8_t)q_min(q, 255);
	}

	/* Force the weights to sum to exactly 255 by absorbing rounding error
	 * into the largest weight (top[0]).  Prevents lighting/skinning drift
	 * on vertices whose four normalized weights round below or above 255. */
	int s = dst->weight[0] + dst->weight[1] + dst->weight[2] + dst->weight[3];
	if (s != 255)
		dst->weight[0] = (uint8_t)q_min(255, q_max(0, dst->weight[0] + (255 - s)));

	return true;
}

/* Smooth per-vertex normals from rest-pose XYZ. */
static void MD5_ComputeNormals(iqmvert_t *verts, int numverts, const int *tris, int numtris)
{
	for (int i = 0; i < numverts; i++)
		verts[i].norm[0] = verts[i].norm[1] = verts[i].norm[2] = verts[i].norm[3] = 0;

	float *acc = (float *)calloc(numverts * 3, sizeof(float));
	if (!acc)
		return;

	for (int i = 0; i < numtris; i++)
	{
		int i0 = tris[i*3], i1 = tris[i*3+1], i2 = tris[i*3+2];
		if (i0 < 0 || i0 >= numverts || i1 < 0 || i1 >= numverts || i2 < 0 || i2 >= numverts)
			continue;

		vec3_t e1, e2, n;
		VectorSubtract(*(vec3_t*)verts[i1].xyz, *(vec3_t*)verts[i0].xyz, e1);
		VectorSubtract(*(vec3_t*)verts[i2].xyz, *(vec3_t*)verts[i0].xyz, e2);
		CrossProduct(e1, e2, n);

		for (int j = 0; j < 3; j++)
		{
			int vi = tris[i*3 + j];
			acc[vi*3 + 0] += n[0];
			acc[vi*3 + 1] += n[1];
			acc[vi*3 + 2] += n[2];
		}
	}

	for (int i = 0; i < numverts; i++)
	{
		vec3_t n = { acc[i*3], acc[i*3+1], acc[i*3+2] };
		float len = VectorLength(n);
		if (len > 0.001f)
		{
			VectorScale(n, 127.0f / len, n);
			verts[i].norm[0] = (int8_t)n[0];
			verts[i].norm[1] = (int8_t)n[1];
			verts[i].norm[2] = (int8_t)n[2];
		}
	}

	free(acc);
}

/* Parse MD5mesh text into heap buffers.  `weights_pool` is filled with one
 * md5weight_t per (vertex, influence) pair; per-vertex range is given by
 * (first_weight[i] .. first_weight[i] + num_weights[i]).  All output
 * buffers must be sized by the caller; this routine never allocates them.
 *
 * Only the first `mesh { }` block is read -- a multi-mesh MD5 would need
 * per-mesh skins and index rebasing, which the aliashdr_t single-skin,
 * single-index-buffer shape has no room for.
 *
 * Section lookups all restart from the top of the mesh block rather than
 * walking forward from the previous one.  id's own writer emits
 * numverts/vert, then numtris/tri, then numweights/weight, and a
 * forward-only walk that reads weights before triangles can never find
 * `numtris` again -- it is behind the cursor.  Restarting also means the
 * order is not something this parser cares about. */
static qboolean MD5_ParseMesh(const char *text,
                              boneinfo_t *bones, int *numbones_out,
                              iqmvert_t *verts, md5weight_t *weights_pool, int *weights_used_out,
                              int *first_weight, int *num_weights, int *numverts_out,
                              int *tris, int *numtris_out,
                              char *shader_out, size_t shader_sz)
{
	const char *p = text;
	const char *mesh_start;
	int numjoints = 0, numverts = 0, numtris = 0, total_weights = 0;
	int file_numweights = 0;
	int *owner = NULL, *filled = NULL, *pool_base = NULL;
	qboolean ok = false;

	p = MD5_FindKeyword(text, "numJoints");
	if (!p) return false;
	numjoints = atoi(p + 9);
	if (numjoints <= 0 || numjoints > MD5_MAX_BONES) return false;

	/* Resume the search PAST the numJoints token: "numJoints" itself ends
	 * in "joints", and a plain strstr from the same cursor matches that
	 * substring instead of the block header. */
	p = MD5_FindKeyword(p + 9, "joints");
	if (!p) return false;
	p = MD5_FindChar(p, '{');
	if (!p) return false;
	p++;

	for (int i = 0; i < numjoints; i++)
	{
		float tx, ty, tz, qx, qy, qz, qw;
		int parent;
		char name[32];

		while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
		if (*p == '"')
		{
			p++;
			int len = 0;
			while (p[len] && p[len] != '"' && len < 31) len++;
			memcpy(name, p, len);
			name[len] = 0;
			p += len + 1;
		}
		else
		{
			name[0] = 0;
		}

		/* id Software .md5mesh stores only 3 quat components; W is implicit and
		 * reconstructed as negative (canonical half-angle, lower hemisphere). */
		if (sscanf(p, "%d ( %f %f %f ) ( %f %f %f )", &parent, &tx, &ty, &tz, &qx, &qy, &qz) != 7)
			return false;
		qw = MD5_QuatW(qx, qy, qz);

		q_strlcpy(bones[i].name, name, 31);
		bones[i].parent = parent;
		VectorSet(bones[i].translate, tx, ty, tz);
		bones[i].rotate[0] = qx;
		bones[i].rotate[1] = qy;
		bones[i].rotate[2] = qz;
		bones[i].rotate[3] = qw;
		VectorSet(bones[i].scale, 1, 1, 1);

		p = MD5_SkipLine(p);
	}
	p = MD5_FindChar(p, '}');
	if (!p) return false;
	p++;
	*numbones_out = numjoints;

	/* Everything from here on is inside the first mesh block. */
	mesh_start = p;

	/* Optional skin path, in the mesh block's preamble ahead of numverts. */
	if (shader_out && shader_sz)
	{
		const char *sh = MD5_FindKeyword(mesh_start, "shader");
		const char *nv = MD5_FindKeyword(mesh_start, "numverts");

		shader_out[0] = '\0';
		if (sh && (!nv || sh < nv))
		{
			md5tok_t st;
			st.p = sh + 6;
			if (!MD5_ReadName(&st, shader_out, shader_sz))
				shader_out[0] = '\0';
		}
	}

	/* --- verts --- */
	p = MD5_FindKeyword(mesh_start, "numverts");
	if (!p) return false;
	numverts = atoi(p + 8);
	if (numverts <= 0 || numverts > MD5_MAX_VERTS) return false;

	p += 8;
	for (int i = 0; i < numverts; i++)
	{
		int idx, startweight, countweight;
		float u, v;

		p = MD5_FindKeyword(p, "vert");
		if (!p) return false;
		/* id Software MD5 format: `vert idx ( s t ) startWeight countWeight`. */
		if (sscanf(p, "vert %d ( %f %f ) %d %d", &idx, &u, &v, &startweight, &countweight) != 5)
			return false;
		if (startweight < 0 || countweight < 0)
			return false;

		verts[i].st[0] = u;
		verts[i].st[1] = v;
		/* xyz/norm/weight/idx filled in post-parse from the weight pool */
		verts[i].xyz[0] = verts[i].xyz[1] = verts[i].xyz[2] = 0;
		verts[i].norm[0] = verts[i].norm[1] = verts[i].norm[2] = verts[i].norm[3] = 0;
		memset(verts[i].weight, 0, 4);
		memset(verts[i].idx, 0, 4);

		/* Stash the file-side weight range; the binning pass below
		 * rewrites these into pool-side offsets. */
		first_weight[i] = startweight;
		num_weights[i]  = countweight;

		p = MD5_SkipLine(p);
	}
	*numverts_out = numverts;

	/* --- triangles --- */
	p = MD5_FindKeyword(mesh_start, "numtris");
	if (!p) return false;
	numtris = atoi(p + 7);
	if (numtris <= 0 || numtris > MD5_MAX_TRIANGLES) return false;

	p += 7;
	for (int i = 0; i < numtris; i++)
	{
		int idx, a, b, c;

		p = MD5_FindKeyword(p, "tri");
		if (!p) return false;
		if (sscanf(p, "tri %d %d %d %d", &idx, &a, &b, &c) != 4)
			return false;
		if (a < 0 || a >= numverts || b < 0 || b >= numverts || c < 0 || c >= numverts)
			return false;
		tris[i*3 + 0] = a;
		tris[i*3 + 1] = b;
		tris[i*3 + 2] = c;
		p = MD5_SkipLine(p);
	}
	*numtris_out = numtris;

	/* --- weights --- */
	p = MD5_FindKeyword(mesh_start, "numweights");
	if (!p) return false;
	file_numweights = atoi(p + 10);
	if (file_numweights < 0)
		return false;

	/* Map each file-side weight index to the vertex that claims it, so the
	 * binning loop below is a lookup rather than a scan over every vertex
	 * per weight (which is O(numverts * numweights) -- a quarter of a
	 * billion comparisons on a max-size model). */
	owner     = (int *) calloc(file_numweights > 0 ? file_numweights : 1, sizeof(int));
	filled    = (int *) calloc(numverts, sizeof(int));
	pool_base = (int *) calloc(numverts, sizeof(int));
	if (!owner || !filled || !pool_base)
		goto cleanup;

	for (int i = 0; i < file_numweights; i++)
		owner[i] = -1;

	{
		int cursor = 0;

		for (int j = 0; j < numverts; j++)
		{
			int fs = first_weight[j];
			int fn = num_weights[j];
			int capped = (fn > MD5_MAX_WEIGHTS_PER_VERT) ? MD5_MAX_WEIGHTS_PER_VERT : fn;

			for (int k = 0; k < fn; k++)
			{
				if (fs + k >= 0 && fs + k < file_numweights)
					owner[fs + k] = j;
			}

			/* Reserve the pool slice up front so a weight can be written
			 * to its own vertex's slice no matter what order the file
			 * lists them in.  The old code assigned slots in file order
			 * and only worked because well-formed exporters happen to
			 * emit them vertex-major. */
			pool_base[j] = cursor;
			cursor += capped;
		}
		if (cursor > MD5_MAX_VERTS * MD5_MAX_WEIGHTS_PER_VERT)
			goto cleanup;
	}

	p = MD5_FindKeyword(mesh_start, "numweights") + 10;
	for (int i = 0; i < file_numweights; i++)
	{
		int wi, bone_idx, vi, slot;
		float wf, wx, wy, wz;

		p = MD5_FindKeyword(p, "weight");
		if (!p) goto cleanup;
		if (sscanf(p, "weight %d %d %f ( %f %f %f )",
		           &wi, &bone_idx, &wf, &wx, &wy, &wz) != 6)
			goto cleanup;

		if (wi < 0 || wi >= file_numweights)
			goto cleanup;
		vi = owner[wi];
		if (vi < 0)
			goto cleanup;	/* a weight no vertex claims */

		if (filled[vi] >= MD5_MAX_WEIGHTS_PER_VERT)
		{
			p = MD5_SkipLine(p);
			continue;	/* silently drop overflow; truncated in PackTopFour */
		}

		slot = pool_base[vi] + filled[vi];
		weights_pool[slot].bone   = bone_idx;
		weights_pool[slot].factor = wf;
		weights_pool[slot].pos[0] = wx;
		weights_pool[slot].pos[1] = wy;
		weights_pool[slot].pos[2] = wz;
		filled[vi]++;
		total_weights++;

		p = MD5_SkipLine(p);
	}

	/* Rewrite first_weight/num_weights to point into the pool layout. */
	for (int j = 0; j < numverts; j++)
	{
		first_weight[j] = pool_base[j];
		num_weights[j]  = filled[j];
	}
	*weights_used_out = total_weights;

	ok = true;

cleanup:
	free(owner);
	free(filled);
	free(pool_base);
	return ok;
}

aliashdr_t *MD5_LoadMesh(const char *name, const unsigned char *buffer, int size,
                         vec3_t out_mins, vec3_t out_maxs)
{
	(void)size;

	/* Heap-allocated parse buffers — putting these on the stack overflows
	 * thread stacks (uhexen2-7ok0.1: ~240KB combined for verts+tris). */
	boneinfo_t   *bones   = (boneinfo_t *)  calloc(MD5_MAX_BONES,                              sizeof(boneinfo_t));
	iqmvert_t    *verts   = (iqmvert_t *)   calloc(MD5_MAX_VERTS,                              sizeof(iqmvert_t));
	int          *tris    = (int *)         calloc(MD5_MAX_TRIANGLES * 3,                      sizeof(int));
	md5weight_t  *wpool   = (md5weight_t *) calloc(MD5_MAX_VERTS * MD5_MAX_WEIGHTS_PER_VERT,   sizeof(md5weight_t));
	int          *wfirst  = (int *)         calloc(MD5_MAX_VERTS,                              sizeof(int));
	int          *wcount  = (int *)         calloc(MD5_MAX_VERTS,                              sizeof(int));
	float        (*rest_world)[12] = NULL;
	float        (*inv_rest)[12]   = NULL;
	aliashdr_t   *hdr     = NULL;
	md5anim_t     anim;
	char          shader[MAX_QPATH];

	int numbones = 0, numverts = 0, numtris = 0, weights_used = 0;
	int numposes = 1, numframes = 1;
	float frameinterval = 0.1f;

	memset(&anim, 0, sizeof(anim));
	shader[0] = '\0';

	if (!bones || !verts || !tris || !wpool || !wfirst || !wcount)
		goto done;

	if (!MD5_ParseMesh((const char *)buffer,
	                   bones, &numbones,
	                   verts, wpool, &weights_used,
	                   wfirst, wcount, &numverts,
	                   tris, &numtris,
	                   shader, sizeof(shader)))
	{
		Con_DPrintf("MD5_LoadMesh: parse failed for %s\n", name);
		goto done;
	}

	/* Compute rest-pose world transforms.  MD5 joints are already in
	 * world space (parent transforms already folded by the exporter), so
	 * a per-bone quat+translate IS the world matrix — no parent walk
	 * required.  We still validate the parent index for sanity. */
	rest_world = (float (*)[12])calloc(numbones, sizeof(float[12]));
	inv_rest   = (float (*)[12])calloc(numbones, sizeof(float[12]));
	if (!rest_world || !inv_rest)
		goto done;

	for (int b = 0; b < numbones; b++)
	{
		if (bones[b].parent >= b)
		{
			Con_DPrintf("MD5_LoadMesh: bad parent index in %s (bone %d parent %d)\n",
			            name, b, bones[b].parent);
			goto done;
		}
		MD5_QuatTransToMat3x4(bones[b].rotate, bones[b].translate, rest_world[b]);
		MD5_InvertRigidMat3x4(rest_world[b], inv_rest[b]);
	}

	/* For each vertex, accumulate rest-pose XYZ from its weights and
	 * pack the top 4 influences for the GPU. */
	for (int i = 0; i < numverts; i++)
	{
		const md5weight_t *vw = &wpool[wfirst[i]];
		int n = wcount[i];

		vec3_t acc = { 0, 0, 0 };
		for (int w = 0; w < n; w++)
		{
			int b = vw[w].bone;
			if (b < 0 || b >= numbones)
				continue;
			float world[3];
			MD5_TransformPoint(rest_world[b], vw[w].pos, world);
			acc[0] += world[0] * vw[w].factor;
			acc[1] += world[1] * vw[w].factor;
			acc[2] += world[2] * vw[w].factor;
		}
		verts[i].xyz[0] = acc[0];
		verts[i].xyz[1] = acc[1];
		verts[i].xyz[2] = acc[2];

		if (n > 0)
			MD5_PackTopFourWeights(vw, n, &verts[i]);
	}

	MD5_ComputeNormals(verts, numverts, tris, numtris);

	/* Sibling .md5anim, if one is on the searchpath.  Absent is normal and
	 * not an error — a prop model has no animation and stays at rest. */
	if (MD5_LoadAnim(name, bones, numbones, (const float (*)[12])inv_rest, &anim))
	{
		numframes = numposes = anim.numframes;
		frameinterval = 1.0f / anim.framerate;
	}

	/* Hunk layout: header (whose trailing frames[] is sized for numframes),
	 * then boneinfo array, then the bind pose (numbones matrices), then
	 * bonepose data (numposes × numbones matrices), then the vertex array,
	 * then triangle indices.
	 * Storing OFFSETs in the header (not stack pointers) is critical —
	 * the old code stored (stack_addr - hunk_addr) which both leaked the
	 * stack across the function boundary AND made the subsequent memcpy
	 * write back onto the stack rather than into the hunk. */
	int header_size     = sizeof(aliashdr_t) + (numframes - 1) * sizeof(maliasframedesc_t);
	int boneinfo_off    = header_size;
	int bindpose_off    = boneinfo_off + numbones * (int)sizeof(boneinfo_t);
	int boneposedata_off = bindpose_off + numbones * (int)sizeof(bonepose_t);
	int posedata_off    = boneposedata_off + numposes * numbones * (int)sizeof(bonepose_t);
	int tridata_off     = posedata_off + numverts * (int)sizeof(iqmvert_t);
	int hunksize        = tridata_off + numtris * 3 * (int)sizeof(unsigned short);

	hdr = (aliashdr_t *)Hunk_Alloc(hunksize);
	if (!hdr)
		goto done;

	memset(hdr, 0, header_size);
	hdr->ident         = ALIAS_IDENT;
	hdr->version       = ALIAS_VERSION_H2;
	hdr->numverts      = numverts;
	hdr->numtris       = numtris;
	hdr->numframes     = numframes;
	hdr->numposes      = numposes;
	hdr->poseverts     = numverts;
	hdr->poseverttype  = PV_IQM;
	hdr->numbones      = numbones;
	hdr->boneinfo      = boneinfo_off;
	hdr->bindpose      = bindpose_off;
	hdr->boneposedata  = boneposedata_off;
	hdr->posedata      = posedata_off;
	hdr->triangledata  = tridata_off;
	hdr->synctype      = ST_SYNC;

	/* iqmvert_t.xyz is already in model units; the draw path still pushes
	 * scale/scale_origin onto the modelview for every alias model, so an
	 * all-zero scale (what memset leaves) collapses the mesh to a point. */
	VectorSet(hdr->scale, 1.0f, 1.0f, 1.0f);
	VectorSet(hdr->scale_origin, 0.0f, 0.0f, 0.0f);

	/* One MD5 animation frame per aliashdr_t frame, one pose each, so
	 * e->frame from the server indexes this exactly like a .mdl and
	 * R_AliasResolveLerp's single-pose blending applies unchanged. */
	for (int f = 0; f < numframes; f++)
	{
		hdr->frames[f].firstpose = f;
		hdr->frames[f].numposes  = 1;
		hdr->frames[f].interval  = frameinterval;
		hdr->frames[f].frame     = f;
		q_snprintf(hdr->frames[f].name, sizeof(hdr->frames[f].name), "frame%d", f);
	}

	memcpy((byte *)hdr + boneinfo_off, bones, numbones * sizeof(boneinfo_t));

	/* Rest-pose world transform per bone, kept rather than discarded with
	 * rest_world.  Nothing in the draw path needs it -- the GPU gets
	 * anim_world x inv_rest_world and the vertices are already in rest
	 * world space -- but r_showskel does: multiplying a bone matrix by
	 * this cancels the inv_rest_world factor and recovers the animated
	 * joint position, which is the only thing there is to draw a line
	 * between.  Ironwail made the same change in 4349da96, moving its
	 * outposes from Z_Malloc to the hunk for exactly this.  A bonepose_t
	 * is a bare float[12], but copy through .mat rather than memcpy the
	 * block, so a future field on the struct cannot silently misalign it. */
	{
		bonepose_t *bindposes = (bonepose_t *)((byte *)hdr + bindpose_off);

		for (int b = 0; b < numbones; b++)
			memcpy(bindposes[b].mat, rest_world[b], sizeof(float[12]));
	}
	memcpy((byte *)hdr + posedata_off, verts, numverts * sizeof(iqmvert_t));
	/* Persist triangle indices as unsigned short (numverts <= MD5_MAX_VERTS=4096 < 65536). */
	{
		unsigned short *tridata = (unsigned short *)((byte *)hdr + tridata_off);
		for (int t = 0; t < numtris * 3; t++)
			tridata[t] = (unsigned short)tris[t];
	}

	/* Bone matrices.  Each one maps a vertex from its REST world position
	 * (which is what the loop above baked into iqmvert_t.xyz) to its
	 * animated world position: anim_world[b] × inv_rest_world[b].  With no
	 * .md5anim that product is identity by definition, which is the static
	 * rest pose. */
	{
		bonepose_t *poses = (bonepose_t *)((byte *)hdr + boneposedata_off);

		if (anim.poses)
			memcpy(poses, anim.poses,
			       (size_t)numposes * numbones * sizeof(bonepose_t));
		else
			for (int b = 0; b < numbones; b++)
				MD5_IdentityMat3x4(poses[b].mat);
	}

	MD5_LoadSkin(hdr, name, shader);

	/* Culling bounds.  The animated bounds from the .md5anim are the real
	 * extent — a rest-pose box clips limbs out of view the moment the
	 * animation reaches past it — so prefer them and let the caller fall
	 * back to the rest-pose verts when the file had none. */
	if (out_mins && out_maxs)
	{
		if (anim.have_bounds)
		{
			VectorCopy(anim.mins, out_mins);
			VectorCopy(anim.maxs, out_maxs);
		}
		else
		{
			VectorClear(out_mins);
			VectorClear(out_maxs);
		}
	}

	Con_DPrintf("MD5_LoadMesh: loaded %s (%d bones, %d verts, %d tris, %d frames)\n",
	            name, numbones, numverts, numtris, numframes);

done:
	free(anim.poses);
	free(rest_world);
	free(inv_rest);
	free(bones);
	free(verts);
	free(tris);
	free(wpool);
	free(wfirst);
	free(wcount);
	return hdr;
}
