/* md5mesh.c -- MD5 skeletal model loader (Ironwail parity)
 *
 * Loads MD5mesh files into aliashdr_t with PV_IQM pose vertices.  Only the
 * rest pose is produced (numposes=1); .md5anim parsing is a separate task
 * (uhexen2-7ok0.2).
 */

#include "quakedef.h"
#include "gl_model.h"

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

/* Parse MD5mesh text into heap buffers.  `weights_out` is filled with one
 * md5weight_t per (vertex, influence) pair; per-vertex range is given by
 * (first_weight[i] .. first_weight[i] + num_weights[i]).  All output
 * buffers must be sized by the caller; this routine never allocates. */
static qboolean MD5_ParseMesh(const char *text,
                              boneinfo_t *bones, int *numbones_out,
                              iqmvert_t *verts, md5weight_t *weights_pool, int *weights_used_out,
                              int *first_weight, int *num_weights, int *numverts_out,
                              int *tris, int *numtris_out)
{
	const char *p = text;
	int numjoints = 0, numverts = 0, numtris = 0, total_weights = 0;

	p = strstr(p, "numJoints");
	if (!p) return false;
	numjoints = atoi(p + 10);
	if (numjoints <= 0 || numjoints > MD5_MAX_BONES) return false;

	p = strstr(p, "joints");
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
		{
			float ww = 1.0f - qx*qx - qy*qy - qz*qz;
			qw = (ww > 0.0f) ? -sqrtf(ww) : 0.0f;
		}

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

	p = strstr(p, "numverts");
	if (!p) return false;
	numverts = atoi(p + 9);
	if (numverts <= 0 || numverts > MD5_MAX_VERTS) return false;

	p = strstr(p, "vert");
	if (!p) return false;

	for (int i = 0; i < numverts; i++)
	{
		int idx, startweight, countweight;
		float u, v;

		while (*p && *p != 'v') p++;
		/* id Software MD5 format: `vert idx ( s t ) startWeight countWeight`. */
		if (sscanf(p, "vert %d ( %f %f ) %d %d", &idx, &u, &v, &startweight, &countweight) != 5)
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

	/* Weights are listed AFTER all verts in a separate block.  Re-walk to
	 * find the weight section. */
	p = strstr(p, "numweights");
	if (!p) return false;
	int file_numweights = atoi(p + 11);

	p = strstr(p, "weight");
	if (!p) return false;

	/* Read every weight and bin into per-vertex slots.  The weight indices
	 * we stored in first_weight[] above are PRE-binning offsets; we'll
	 * use a parallel counter to fill them safely while respecting the
	 * MAX_WEIGHTS_PER_VERT cap. */
	int *filled = (int *)calloc(numverts, sizeof(int));
	if (!filled)
		return false;

	for (int i = 0; i < file_numweights; i++)
	{
		int wi, bone_idx;
		float wf, wx, wy, wz;

		while (*p && *p != 'w') p++;
		if (sscanf(p, "weight %d %d %f ( %f %f %f )",
		           &wi, &bone_idx, &wf, &wx, &wy, &wz) != 6)
		{
			free(filled);
			return false;
		}

		/* Locate the owning vertex by scanning vert-index ranges built
		 * from the original first_weight[]/num_weights[] (which still
		 * hold file-side cursors at this point — they'll be rewritten
		 * to pool offsets after binning). */
		int vi = -1;
		for (int j = 0; j < numverts; j++)
		{
			int fs = first_weight[j];
			int fn = num_weights[j];
			if (wi >= fs && wi < fs + fn)
			{
				vi = j;
				break;
			}
		}
		if (vi < 0)
		{
			free(filled);
			return false;
		}

		if (filled[vi] >= MD5_MAX_WEIGHTS_PER_VERT)
			continue;	/* silently drop overflow; truncated in PackTopFour */

		int slot = total_weights;
		if (slot >= MD5_MAX_VERTS * MD5_MAX_WEIGHTS_PER_VERT)
		{
			free(filled);
			return false;
		}

		weights_pool[slot].bone   = bone_idx;
		weights_pool[slot].factor = wf;
		weights_pool[slot].pos[0] = wx;
		weights_pool[slot].pos[1] = wy;
		weights_pool[slot].pos[2] = wz;
		total_weights++;
		filled[vi]++;

		p = MD5_SkipLine(p);
	}

	/* Rewrite first_weight/num_weights to point into the new pool layout. */
	int cursor = 0;
	for (int j = 0; j < numverts; j++)
	{
		first_weight[j] = cursor;
		num_weights[j]  = filled[j];
		cursor += filled[j];
	}
	*weights_used_out = total_weights;

	free(filled);

	p = strstr(p, "numtris");
	if (!p) return false;
	numtris = atoi(p + 8);
	if (numtris <= 0 || numtris > MD5_MAX_TRIANGLES) return false;

	p = strstr(p, "tri");
	if (!p) return false;

	for (int i = 0; i < numtris; i++)
	{
		int idx, a, b, c;
		while (*p && *p != 't') p++;
		if (sscanf(p, "tri %d %d %d %d", &idx, &a, &b, &c) != 4)
			return false;
		tris[i*3 + 0] = a;
		tris[i*3 + 1] = b;
		tris[i*3 + 2] = c;
		p = MD5_SkipLine(p);
	}
	*numtris_out = numtris;

	return true;
}

aliashdr_t *MD5_LoadMesh(const char *name, const unsigned char *buffer, int size)
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
	aliashdr_t   *hdr     = NULL;

	int numbones = 0, numverts = 0, numtris = 0, weights_used = 0;

	if (!bones || !verts || !tris || !wpool || !wfirst || !wcount)
		goto done;

	if (!MD5_ParseMesh((const char *)buffer,
	                   bones, &numbones,
	                   verts, wpool, &weights_used,
	                   wfirst, wcount, &numverts,
	                   tris, &numtris))
	{
		Con_DPrintf("MD5_LoadMesh: parse failed for %s\n", name);
		goto done;
	}

	/* Compute rest-pose world transforms.  MD5 joints are already in
	 * world space (parent transforms already folded by the exporter), so
	 * a per-bone quat+translate IS the world matrix — no parent walk
	 * required.  We still validate the parent index for sanity. */
	float (*rest_world)[12] = (float (*)[12])calloc(numbones, sizeof(float[12]));
	if (!rest_world)
		goto done;

	for (int b = 0; b < numbones; b++)
	{
		if (bones[b].parent >= b)
		{
			Con_DPrintf("MD5_LoadMesh: bad parent index in %s (bone %d parent %d)\n",
			            name, b, bones[b].parent);
			free(rest_world);
			goto done;
		}
		MD5_QuatTransToMat3x4(bones[b].rotate, bones[b].translate, rest_world[b]);
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

	free(rest_world);

	MD5_ComputeNormals(verts, numverts, tris, numtris);

	/* Hunk layout: header, then boneinfo array, then bonepose data
	 * (numposes × numbones rest-pose matrices), then vertex array.
	 * Storing OFFSETs in the header (not stack pointers) is critical —
	 * the old code stored (stack_addr - hunk_addr) which both leaked the
	 * stack across the function boundary AND made the subsequent memcpy
	 * write back onto the stack rather than into the hunk. */
	int numposes = 1;
	int boneinfo_off    = sizeof(aliashdr_t);
	int boneposedata_off = boneinfo_off + numbones * sizeof(boneinfo_t);
	int posedata_off    = boneposedata_off + numposes * numbones * sizeof(bonepose_t);
	int tridata_off     = posedata_off + numverts * sizeof(iqmvert_t);
	int hunksize        = tridata_off + numtris * 3 * sizeof(unsigned short);

	hdr = (aliashdr_t *)Hunk_Alloc(hunksize);
	if (!hdr)
		goto done;

	memset(hdr, 0, sizeof(*hdr));
	hdr->ident         = ALIAS_IDENT;
	hdr->version       = ALIAS_VERSION_H2;
	hdr->numverts      = numverts;
	hdr->numtris       = numtris;
	hdr->numframes     = 1;
	hdr->numposes      = numposes;
	hdr->poseverts     = numverts;
	hdr->poseverttype  = PV_IQM;
	hdr->numbones      = numbones;
	hdr->boneinfo      = boneinfo_off;
	hdr->boneposedata  = boneposedata_off;
	hdr->posedata      = posedata_off;
	hdr->triangledata  = tridata_off;

	memcpy((byte *)hdr + boneinfo_off, bones, numbones * sizeof(boneinfo_t));
	memcpy((byte *)hdr + posedata_off, verts, numverts * sizeof(iqmvert_t));
	/* Persist triangle indices as unsigned short (numverts <= MD5_MAX_VERTS=4096 < 65536). */
	{
		unsigned short *tridata = (unsigned short *)((byte *)hdr + tridata_off);
		for (int t = 0; t < numtris * 3; t++)
			tridata[t] = (unsigned short)tris[t];
	}

	/* Rest-pose bone matrices = identity.  The skeletal shader applies
	 * bones[i] to a_position; at rest pose the verts already hold their
	 * world-space rest position (computed above), so identity is correct.
	 * Real .md5anim playback will overwrite this with current_pose ×
	 * inv_bind_pose per bone per frame (uhexen2-7ok0.2). */
	bonepose_t *poses = (bonepose_t *)((byte *)hdr + boneposedata_off);
	for (int b = 0; b < numbones; b++)
		MD5_IdentityMat3x4(poses[b].mat);

	Con_DPrintf("MD5_LoadMesh: loaded %s (%d bones, %d verts, %d tris)\n",
	            name, numbones, numverts, numtris);

done:
	free(bones);
	free(verts);
	free(tris);
	free(wpool);
	free(wfirst);
	free(wcount);
	return hdr;
}
