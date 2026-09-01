/* Standalone check of md3mesh.c, the .md3 parser added under uhexen2-2ah9.
 *
 * The parser has to satisfy a contract nothing else in the engine states out
 * loud: GL_MakeAliasGPUMesh numbers pose vertices by walking the GL command
 * list, so hdr->posedata must be in command-list order and hdr->poseverts must
 * be that walk's count.  Get either wrong and the model still loads, still
 * draws, and is simply the wrong shape -- which is exactly the failure a unit
 * test is for and a screenshot is not.
 *
 * Reads engine/tests/md3/quad.md3 (see gen.py, which documents the geometry).
 * Exits 1 on any mismatch.  Build recipe in README.md.
 */
#include "quakedef.h"
#include "gl_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- stubs ------------------------------------------------------------- */
static char stub_dir[512];

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list a;
	if (flags == 2) return;		/* _PRINT_DEVEL */
	va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
}

void *Hunk_Alloc (int size) { return calloc(1, (size_t)size); }

byte *FS_LoadMallocFile (const char *path, unsigned int *path_id)
{
	char full[1024];
	FILE *f; long n; byte *buf;
	(void)path_id;
	snprintf(full, sizeof(full), "%s/%s", stub_dir,
		 strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	f = fopen(full, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = (byte *) malloc((size_t)n + 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
	buf[n] = 0; fclose(f);
	if (path_id) *path_id = 0;
	return buf;
}
static long last_size;

byte *IMG_LoadExternalTexture (const char *name, int *w, int *h, qboolean *a)
{ (void)name; (void)w; (void)h; (void)a; return NULL; }

GLuint gl_solid_white_texture = 1;
GLuint GL_LoadTexture (const char *id, byte *data, int w, int h, int flags)
{ (void)id; (void)data; (void)w; (void)h; (void)flags; return 1; }

/* --- checks ------------------------------------------------------------ */
static int fails = 0;

static void check_i (const char *what, int got, int want)
{
	int ok = (got == want);
	printf("  %-26s %8d   expect %8d   %s\n", what, got, want, ok ? "ok" : "FAIL");
	if (!ok) fails++;
}

static void check_v (const char *what, const float got[3], float x, float y, float z)
{
	float e = 1e-4f;
	int ok = (fabsf(got[0]-x) < e) && (fabsf(got[1]-y) < e) && (fabsf(got[2]-z) < e);
	printf("  %-26s (%7.3f %7.3f %7.3f)  expect (%6.2f %6.2f %6.2f)  %s\n",
	       what, got[0], got[1], got[2], x, y, z, ok ? "ok" : "FAIL");
	if (!ok) fails++;
}

static void check_st (const char *what, const int *cmd, int v, float s, float t)
{
	float gs, gt;
	int ok;
	memcpy(&gs, &cmd[1 + v*2 + 0], 4);
	memcpy(&gt, &cmd[1 + v*2 + 1], 4);
	ok = (fabsf(gs - s) < 1e-5f) && (fabsf(gt - t) < 1e-5f);
	printf("  %-26s (%7.3f %7.3f)          expect (%6.2f %6.2f)          %s\n",
	       what, gs, gt, s, t, ok ? "ok" : "FAIL");
	if (!ok) fails++;
}

/* Decode a pose vertex exactly as GL_DrawAliasFrameMD3 does. */
static void posepos (const aliashdr_t *hdr, int pose, int vert, vec3_t out)
{
	const md3Vertex_t *v = (const md3Vertex_t *)((const byte *)hdr + hdr->posedata);
	v += pose * hdr->poseverts + vert;
	out[0] = v->xyz[0] * MD3_XYZ_SCALE;
	out[1] = v->xyz[1] * MD3_XYZ_SCALE;
	out[2] = v->xyz[2] * MD3_XYZ_SCALE;
}

int main (int argc, char **argv)
{
	aliashdr_t	*hdr;
	byte		*file;
	vec3_t		mins, maxs, v, n;
	const int	*cmd;
	FILE		*f;
	char		full[1024];
	long		size;

	if (argc < 2) { fprintf(stderr, "usage: %s <dir-with-quad.md3>\n", argv[0]); return 2; }
	q_strlcpy(stub_dir, argv[1], sizeof(stub_dir));

	snprintf(full, sizeof(full), "%s/quad.md3", stub_dir);
	f = fopen(full, "rb");
	if (!f) { fprintf(stderr, "cannot read %s\n", full); return 2; }
	fseek(f, 0, SEEK_END); size = ftell(f); fclose(f);
	file = FS_LoadMallocFile("quad.md3", NULL);
	if (!file) { fprintf(stderr, "cannot read quad.md3\n"); return 2; }
	last_size = size;

	VectorClear(mins); VectorClear(maxs);
	hdr = MD3_LoadMesh("models/test/quad.md3", file, (int)size, mins, maxs);
	if (!hdr) { fprintf(stderr, "MD3_LoadMesh failed\n"); return 1; }

	printf("shape\n");
	check_i("poseverttype", (int)hdr->poseverttype, PV_MD3);
	check_i("numframes", hdr->numframes, 2);
	check_i("numposes", hdr->numposes, 2);
	check_i("numtris", hdr->numtris, 2);		/* one per surface */
	check_i("poseverts", hdr->poseverts, 6);	/* 3 per triangle, unshared */
	check_i("numskins", hdr->numskins, 1);

	/* scale must be the identity: MD3 positions are already model units and
	 * the draw path folds scale into every alias model's matrix. */
	printf("scale\n");
	check_v("scale", hdr->scale, 1.0f, 1.0f, 1.0f);
	check_v("scale_origin", hdr->scale_origin, 0.0f, 0.0f, 0.0f);

	/* Command list: two runs of 3, then the terminator.  The st values are
	 * the second surface's for the second run, which is what proves the
	 * surfaces were concatenated rather than the first one loaded twice. */
	printf("command list\n");
	cmd = (const int *)((const byte *)hdr + hdr->commands);
	check_i("cmd[0] run length", cmd[0], 3);
	check_st("front v0 st", cmd, 0, 0.0f, 0.0f);
	check_st("front v1 st", cmd, 1, 1.0f, 0.0f);
	check_st("front v2 st", cmd, 2, 0.0f, 1.0f);
	cmd += 7;
	check_i("cmd[7] run length", cmd[0], 3);
	check_st("back v0 st", cmd, 0, 1.0f, 1.0f);
	check_st("back v1 st", cmd, 1, 0.0f, 1.0f);
	check_st("back v2 st", cmd, 2, 1.0f, 0.0f);
	cmd += 7;
	check_i("terminator", cmd[0], 0);

	/* Pose data, in that same order.  Vertex 3 is the second surface's
	 * first vertex: if the pose array were laid out per surface rather than
	 * per command-list position, this is where it would show. */
	printf("pose 0\n");
	posepos(hdr, 0, 0, v); check_v("vert 0", v,  0.0f,  0.0f, 0.0f);
	posepos(hdr, 0, 1, v); check_v("vert 1", v,  1.0f,  0.0f, 0.0f);
	posepos(hdr, 0, 2, v); check_v("vert 2", v,  0.0f,  1.0f, 0.0f);
	posepos(hdr, 0, 3, v); check_v("vert 3 (2nd surface)", v, 0.0f, 0.0f, 1.0f);
	posepos(hdr, 0, 4, v); check_v("vert 4", v, -1.0f,  0.0f, 1.0f);
	posepos(hdr, 0, 5, v); check_v("vert 5", v,  0.0f, -1.0f, 1.0f);

	printf("pose 1 (every Z +1)\n");
	posepos(hdr, 1, 0, v); check_v("vert 0", v,  0.0f,  0.0f, 1.0f);
	posepos(hdr, 1, 5, v); check_v("vert 5", v,  0.0f, -1.0f, 2.0f);

	/* Normals.  lng 0 -> (0,0,1); lng 64 (a quarter turn) -> (1,0,0).  The
	 * byte order here is the whole point: the file stores one little-endian
	 * short, low byte longitude, and reading it the other way round gives
	 * (0,0,1) for both surfaces. */
	printf("normals\n");
	{
		const md3Vertex_t *pv = (const md3Vertex_t *)((const byte *)hdr + hdr->posedata);
		MD3_DecodeNormal(&pv[0], n); check_v("front (+Z)", n, 0.0f, 0.0f, 1.0f);
		MD3_DecodeNormal(&pv[3], n); check_v("back  (+X)", n, 1.0f, 0.0f, 0.0f);
	}

	/* Bounds: the union of the two frames' own boxes, not a re-derivation
	 * from the vertices -- frame 1 reaches to z=2 and no vertex of frame 0
	 * does. */
	printf("bounds\n");
	check_v("mins", mins, -1.0f, -1.0f, 0.0f);
	check_v("maxs", maxs,  1.0f,  1.0f, 2.0f);

	/* Frame names come from the file. */
	printf("frames\n");
	printf("  %-26s \"%s\" / \"%s\"\n", "names", hdr->frames[0].name, hdr->frames[1].name);
	if (strcmp(hdr->frames[0].name, "rest") || strcmp(hdr->frames[1].name, "lifted"))
	{
		printf("  FAIL: expected \"rest\" / \"lifted\"\n");
		fails++;
	}

	/* A truncated file must be refused, not parsed from its own bad
	 * numbers: every offset in an .md3 is self-declared. */
	printf("truncation\n");
	if (MD3_LoadMesh("models/test/quad.md3", file, (int)size / 2, mins, maxs) != NULL)
	{
		printf("  FAIL: half a file parsed\n");
		fails++;
	}
	else
		printf("  %-26s refused                             ok\n", "half a file");

	free(file);
	printf("\n%s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}
