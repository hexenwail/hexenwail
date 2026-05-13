/* md5mesh.c -- MD5 skeletal model loader (Ironwail parity)
 *
 * Loads MD5mesh + MD5anim format files with skeletal animation support.
 * Converts to internal IQM (iqmvert_t) format for GPU-accelerated skinning.
 */

#include "quakedef.h"

/* Helper: skip to next line */
static const char *MD5_SkipLine(const char *p)
{
	while (*p && *p != '\n') p++;
	if (*p == '\n') p++;
	return p;
}

/* Parse MD5mesh file: joints, vertices, triangles */
static qboolean MD5_ParseMesh(const char *text, boneinfo_t *bones, int *numbones_out,
                              iqmvert_t *verts, int *numverts_out,
                              int *tris, int *numtris_out)
{
	const char *p = text;
	int numjoints = 0, numverts = 0, numtris = 0;

	p = strstr(p, "numJoints");
	if (!p) return false;
	numjoints = atoi(p + 10);
	if (numjoints <= 0 || numjoints > MD5_MAX_BONES) return false;

	p = strstr(p, "joints");
	if (!p) return false;
	p = strchr(p, '{') + 1;

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
		if (sscanf(p, "%d ( %f %f %f ) ( %f %f %f %f )", &parent, &tx, &ty, &tz, &qx, &qy, &qz, &qw) != 8)
			return false;

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
	p = strchr(p, '}') + 1;
	*numbones_out = numjoints;

	p = strstr(p, "numverts");
	if (!p) return false;
	numverts = atoi(p + 9);
	if (numverts <= 0 || numverts > MD5_MAX_VERTS) return false;

	p = strstr(p, "vert");
	for (int i = 0; i < numverts; i++)
	{
		int idx, numweights, weight_idx;
		float u, v, weight_val;

		while (*p && *p != 'v') p++;
		if (sscanf(p, "vert %d ( %f %f ) %d %d", &idx, &u, &v, &numweights, &weight_idx) != 5)
			return false;

		verts[i].st[0] = u;
		verts[i].st[1] = v;
		verts[i].xyz[0] = verts[i].xyz[1] = verts[i].xyz[2] = 0;
		verts[i].norm[0] = verts[i].norm[1] = verts[i].norm[2] = verts[i].norm[3] = 0;
		memset(verts[i].weight, 0, 4);
		memset(verts[i].idx, 0, 4);

		for (int w = 0; w < numweights && w < 4; w++)
		{
			p = MD5_SkipLine(p);
			if (sscanf(p, " weight %d %d %f", &idx, &weight_idx, &weight_val) != 3)
				return false;

			verts[i].idx[w] = weight_idx;
			verts[i].weight[w] = (uint8_t)(weight_val * 255 + 0.5f);
		}
		p = MD5_SkipLine(p);
	}
	*numverts_out = numverts;

	p = strstr(p, "numtris");
	if (!p) return false;
	numtris = atoi(p + 8);
	if (numtris <= 0 || numtris > MD5_MAX_TRIANGLES) return false;

	p = strstr(p, "tri");
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

/* Compute smooth vertex normals */
static void MD5_ComputeNormals(iqmvert_t *verts, int numverts, int *tris, int numtris)
{
	for (int i = 0; i < numtris; i++)
	{
		vec3_t e1, e2, normal;
		int i0 = tris[i*3], i1 = tris[i*3+1], i2 = tris[i*3+2];

		VectorSubtract(*(vec3_t*)verts[i1].xyz, *(vec3_t*)verts[i0].xyz, e1);
		VectorSubtract(*(vec3_t*)verts[i2].xyz, *(vec3_t*)verts[i0].xyz, e2);
		CrossProduct(e1, e2, normal);

		for (int j = 0; j < 3; j++)
		{
			int vi = tris[i*3 + j];
			verts[vi].norm[0] += normal[0];
			verts[vi].norm[1] += normal[1];
			verts[vi].norm[2] += normal[2];
		}
	}

	for (int i = 0; i < numverts; i++)
	{
		vec3_t n = { verts[i].norm[0], verts[i].norm[1], verts[i].norm[2] };
		float len = VectorLength(n);
		if (len > 0.001f)
		{
			VectorScale(n, 1.0f / len, n);
			verts[i].norm[0] = n[0];
			verts[i].norm[1] = n[1];
			verts[i].norm[2] = n[2];
		}
	}
}

/* Load MD5mesh file and create aliashdr_t */
aliashdr_t *MD5_LoadMesh(const char *name, const unsigned char *buffer, int size)
{
	boneinfo_t bones[MD5_MAX_BONES];
	iqmvert_t verts[MD5_MAX_VERTS];
	int tris[MD5_MAX_TRIANGLES * 3];
	int numbones = 0, numverts = 0, numtris = 0;

	if (!MD5_ParseMesh((const char *)buffer, bones, &numbones, verts, &numverts, tris, &numtris))
	{
		Con_DPrintf("MD5_LoadMesh: parse failed for %s\n", name);
		return NULL;
	}

	MD5_ComputeNormals(verts, numverts, tris, numtris);

	int hunksize = sizeof(aliashdr_t) + numbones * sizeof(boneinfo_t) + numverts * sizeof(iqmvert_t);
	aliashdr_t *hdr = (aliashdr_t *)Hunk_Alloc(hunksize);
	if (!hdr) return NULL;

	memset(hdr, 0, sizeof(*hdr));
	hdr->ident = ALIAS_IDENT;
	hdr->version = ALIAS_VERSION;
	hdr->numverts = numverts;
	hdr->numtris = numtris;
	hdr->numframes = 1;
	hdr->numposes = 1;
	hdr->poseverts = numverts;
	hdr->poseverttype = PV_IQM;
	hdr->numbones = numbones;
	hdr->boneinfo = (byte *)bones - (byte *)hdr;
	hdr->posedata = (byte *)verts - (byte *)hdr;

	memcpy((byte *)hdr + hdr->boneinfo, bones, numbones * sizeof(boneinfo_t));
	memcpy((byte *)hdr + hdr->posedata, verts, numverts * sizeof(iqmvert_t));

	Con_DPrintf("MD5_LoadMesh: loaded %s (%d bones, %d verts, %d tris)\n",
	            name, numbones, numverts, numtris);

	return hdr;
}
