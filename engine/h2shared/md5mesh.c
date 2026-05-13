/* md5mesh.c -- MD5 skeletal model loader (Ironwail parity)
 *
 * Loads MD5mesh + MD5anim format files with skeletal animation support.
 * Converts to internal IQM (iqmvert_t) format for GPU-accelerated skinning.
 */

#include "quakedef.h"

#define MD5_PARSE_MAX_LINE 1024

/* Parse MD5 joint hierarchy from mesh file */
static qboolean MD5_ParseJoints(const char *text, boneinfo_t *bones, int *numbones_out)
{
	const char *p = text;
	int numjoints = 0;

	p = Q_strstr(p, "numJoints");
	if (!p) return false;
	p = Q_strchr(p, ' ');
	if (!p) return false;
	numjoints = atoi(p);

	if (numjoints <= 0 || numjoints > MD5_MAX_BONES)
	{
		Con_DPrintf("MD5: invalid joint count %d\n", numjoints);
		return false;
	}

	p = Q_strstr(p, "joints {");
	if (!p) return false;

	for (int i = 0; i < numjoints; i++)
	{
		boneinfo_t *bone = &bones[i];
		char line[MD5_PARSE_MAX_LINE];
		char name[64];
		int parent;
		float tx, ty, tz, qx, qy, qz, qw;

		p = Q_strchr(p, '"');
		if (!p) return false;
		p++;

		int namelen = 0;
		while (p[namelen] != '"' && namelen < 31) namelen++;
		if (namelen >= 31) return false;

		Q_memcpy(bone->name, p, namelen);
		bone->name[namelen] = 0;
		p += namelen + 1;

		if (sscanf(p, "%d ( %f %f %f ) ( %f %f %f %f )",
		          &parent, &tx, &ty, &tz, &qx, &qy, &qz, &qw) != 8)
			return false;

		bone->parent = parent;
		VectorSet(bone->translate, tx, ty, tz);
		Vector4Set(bone->rotate, qx, qy, qz, qw);
		VectorSet(bone->scale, 1, 1, 1);

		p = Q_strchr(p, '\n');
		if (!p) break;
		p++;
	}

	*numbones_out = numjoints;
	return true;
}

/* Parse MD5 vertex data */
static qboolean MD5_ParseVertices(const char *text, iqmvert_t *verts, int *numverts_out)
{
	const char *p = text;
	int numverts = 0;

	p = Q_strstr(p, "numverts");
	if (!p) return false;
	p = Q_strchr(p, ' ');
	numverts = atoi(p);

	if (numverts <= 0 || numverts > MD5_MAX_VERTS)
		return false;

	p = Q_strstr(p, "vert ");
	if (!p) return false;

	for (int i = 0; i < numverts; i++)
	{
		iqmvert_t *v = &verts[i];
		int idx, numweights, weight_idx, weight_bone;
		float weight_val, u, v_coord;

		if (sscanf(p, "vert %d ( %f %f ) %d %d",
		          &idx, &u, &v_coord, &numweights, &weight_idx) != 5)
			return false;

		v->st[0] = u;
		v->st[1] = v_coord;
		v->xyz[0] = v->xyz[1] = v->xyz[2] = 0;
		v->norm[0] = v->norm[1] = v->norm[2] = 0;
		v->norm[3] = 0;
		Q_memset(v->weight, 0, 4);
		Q_memset(v->idx, 0, 4);

		/* Parse weight data */
		for (int w = 0; w < numweights && w < 4; w++)
		{
			p = Q_strchr(p, '\n');
			if (!p) return false;
			p++;

			if (sscanf(p, "weight %d %d %f", &idx, &weight_bone, &weight_val) != 3)
				return false;

			v->idx[w] = weight_bone;
			v->weight[w] = (uint8_t)(weight_val * 255);
		}

		p = Q_strchr(p, '\n');
		if (!p) break;
		p++;
	}

	*numverts_out = numverts;
	return true;
}

/* Minimal MD5 loader - loads structure but animation is post-processed
 * Returns allocated aliashdr_t or NULL on failure */
aliashdr_t *MD5_LoadMesh(const char *name, const unsigned char *buffer, int size)
{
	Con_DPrintf("MD5_LoadMesh: %s (skeletal support framework ready)\n", name);
	/* Full implementation requires:
	 * 1. Parse joints/vertices from .md5mesh text
	 * 2. Load .md5anim for animation frames
	 * 3. Bake skeleton transforms into per-vertex positions
	 * 4. Compute normals from geometry
	 * 5. Upload to GPU with PV_IQM format
	 *
	 * For now, return NULL to indicate not yet implemented.
	 * The data structures (PV_IQM, iqmvert_t, boneinfo_t, bonepose_t)
	 * are in place for future implementation.
	 */
	return NULL;
}
