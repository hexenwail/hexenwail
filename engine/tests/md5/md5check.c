/* Standalone check of the bind-pose block md5mesh.c now keeps for r_showskel.
 * Loads engine/tests/md5/chain.{md5mesh,md5anim} and recomputes joint world
 * positions with the exact composition R_ShowSkel_JointPos uses, then compares
 * against the values gen3.py's geometry forces.
 *
 * Then the skin: MD5_LoadSkin's texture and its _glow / _luma sidecar, through
 * stubbed image loaders (GitHub #492). */
#include "quakedef.h"
#include "gl_model.h"
#include "img_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- stubs ------------------------------------------------------------- */
static char stub_dir[512];

/* Con_Printf / Con_DPrintf are macros over this in printsys.h. */
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
	snprintf(full, sizeof(full), "%s/%s", stub_dir, strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	f = fopen(full, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = (byte *) malloc((size_t)n + 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
	buf[n] = 0; fclose(f);
	return buf;
}

/* The skin stubs answer from these switches, so one run can load the mesh
 * with no skin, with a skin, and with a skin plus its glow sidecar.  Texture
 * numbers: 1 is the white sentinel, 2 a loaded skin, 3 a loaded glow. */
static int	stub_have_skin, stub_have_glow;
static char	stub_glow_asked[256];

byte *IMG_LoadExternalTexture (const char *name, int *w, int *h, qboolean *a)
{
	(void)name;
	if (!stub_have_skin) return NULL;
	*w = *h = 1; *a = false;
	return (byte *) calloc(1, 4);
}

qboolean IMG_LoadReplacementGlow (const char *name, const char *modelname, imgreplace_t *out)
{
	(void)modelname;
	q_strlcpy(stub_glow_asked, name, sizeof(stub_glow_asked));
	memset(out, 0, sizeof(*out));
	return stub_have_glow ? true : false;
}
void IMG_FreeReplacement (imgreplace_t *r) { (void)r; }

GLuint gl_solid_white_texture = 1;
GLuint GL_LoadTexture (const char *id, byte *data, int w, int h, int flags)
{ (void)id; (void)data; (void)w; (void)h; (void)flags; return 2; }
GLuint GL_LoadReplacement (const char *id, const struct imgreplace_s *r, int flags)
{ (void)id; (void)r; (void)flags; return 3; }

/* --- the composition under test ---------------------------------------- */
static void ConcatRowMajor3x4 (const float a[12], const float b[12], float out[12])
{
	int r;
	for (r = 0; r < 3; r++)
	{
		out[r*4+0] = a[r*4+0]*b[0] + a[r*4+1]*b[4] + a[r*4+2]*b[8];
		out[r*4+1] = a[r*4+0]*b[1] + a[r*4+1]*b[5] + a[r*4+2]*b[9];
		out[r*4+2] = a[r*4+0]*b[2] + a[r*4+1]*b[6] + a[r*4+2]*b[10];
		out[r*4+3] = a[r*4+0]*b[3] + a[r*4+1]*b[7] + a[r*4+2]*b[11] + a[r*4+3];
	}
}

static int fails = 0;

static void check_i (const char *what, int got, int want)
{
	printf("  %-26s %d  expect %d  %s\n", what, got, want, got == want ? "ok" : "FAIL");
	if (got != want) fails++;
}

static void check (const char *what, const float got[3], float x, float y, float z)
{
	float e = 1e-4f;
	int ok = (fabsf(got[0]-x) < e) && (fabsf(got[1]-y) < e) && (fabsf(got[2]-z) < e);
	printf("  %-22s (%7.3f %7.3f %7.3f)  expect (%6.1f %6.1f %6.1f)  %s\n",
	       what, got[0], got[1], got[2], x, y, z, ok ? "ok" : "FAIL");
	if (!ok) fails++;
}

int main (int argc, char **argv)
{
	aliashdr_t	*hdr;
	byte		*mesh;
	vec3_t		mins, maxs;
	const bonepose_t *bind, *poses;
	const boneinfo_t *bones;
	int		f, b;

	if (argc < 2) { fprintf(stderr, "usage: %s <dir-with-chain.md5mesh>\n", argv[0]); return 2; }
	q_strlcpy(stub_dir, argv[1], sizeof(stub_dir));

	mesh = FS_LoadMallocFile("chain.md5mesh", NULL);
	if (!mesh) { fprintf(stderr, "cannot read chain.md5mesh\n"); return 2; }

	VectorClear(mins); VectorClear(maxs);
	hdr = MD5_LoadMesh("models/test/chain.md5mesh", mesh, 1024*1024, mins, maxs);
	if (!hdr) { fprintf(stderr, "MD5_LoadMesh failed\n"); return 1; }

	printf("numbones=%d numposes=%d numframes=%d bindpose=%d boneposedata=%d\n",
	       hdr->numbones, hdr->numposes, hdr->numframes, hdr->bindpose, hdr->boneposedata);
	if (!hdr->bindpose) { fprintf(stderr, "hdr->bindpose is 0 -- block not written\n"); return 1; }

	bind  = (const bonepose_t *)((const byte *)hdr + hdr->bindpose);
	poses = (const bonepose_t *)((const byte *)hdr + hdr->boneposedata);
	bones = (const boneinfo_t *)((const byte *)hdr + hdr->boneinfo);

	for (f = 0; f < hdr->numposes; f++)
	{
		printf("frame %d:\n", f);
		for (b = 0; b < hdr->numbones; b++)
		{
			float	world[12];
			vec3_t	pos;
			char	label[64];

			/* exactly R_ShowSkel_JointPos with root = identity */
			ConcatRowMajor3x4 (poses[f*hdr->numbones + b].mat, bind[b].mat, world);
			pos[0] = world[3]; pos[1] = world[7]; pos[2] = world[11];

			snprintf(label, sizeof(label), "%s (parent %d)", bones[b].name, bones[b].parent);
			if (f == 0)
				check(label, pos, 0.f, 0.f, b * 10.f);
			else if (b < 2)
				check(label, pos, 0.f, 0.f, b * 10.f);
			else
				check(label, pos, 0.f, -10.f, 10.f);
		}
	}

	/* Skins.  The load above had no skin image: white sentinel, no glow.
	 * An MD5 skin is RGBA, so its glow can only come from the _glow / _luma
	 * sidecar of the skin path that resolved -- the shader name with any
	 * extension stripped -- and must reach all four animation slots, which
	 * R_DrawAliasModel's fullbright pass indexes.  GitHub #492. */
	printf("skins\n");
	check_i("no skin: texture", (int)hdr->gl_texturenum[0][0], 1);
	check_i("no skin: glow", (int)hdr->gl_fb_texturenum[0][0], 0);

	stub_have_skin = 1; stub_have_glow = 0; stub_glow_asked[0] = 0;
	hdr = MD5_LoadMesh("models/test/chain.md5mesh", mesh, 1024*1024, mins, maxs);
	if (!hdr) { fprintf(stderr, "MD5_LoadMesh failed\n"); return 1; }
	check_i("skin: texture", (int)hdr->gl_texturenum[0][0], 2);
	check_i("skin, no sidecar: glow", (int)hdr->gl_fb_texturenum[0][0], 0);

	stub_have_glow = 1; stub_glow_asked[0] = 0;
	hdr = MD5_LoadMesh("models/test/chain.md5mesh", mesh, 1024*1024, mins, maxs);
	if (!hdr) { fprintf(stderr, "MD5_LoadMesh failed\n"); return 1; }
	check_i("glow slot 0", (int)hdr->gl_fb_texturenum[0][0], 3);
	check_i("glow slot 3", (int)hdr->gl_fb_texturenum[0][3], 3);
	check_i("skin under the glow", (int)hdr->gl_texturenum[0][3], 2);
	printf("  %-26s \"%s\"\n", "glow sidecar of", stub_glow_asked);
	if (strcmp(stub_glow_asked, "models/test/chain"))
	{
		printf("  FAIL: expected \"models/test/chain\"\n");
		fails++;
	}
	stub_have_skin = stub_have_glow = 0;

	printf(fails ? "\nFAILED (%d)\n" : "\nall joint positions and skins match\n", fails);
	return fails ? 1 : 0;
}
