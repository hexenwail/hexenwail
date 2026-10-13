/*
 * Gate test for engine/hexen2/hw_particle_mapping.h (issue #309).
 *
 * A HexenWorld server sends a particle type as HexenWorld's client numbered
 * it.  cl_hw.c used to cast that byte to Hexen II's ptype_t, and the two
 * enums part ways at 24, so grenade smoke came out as pt_darken, red fire as
 * pt_snow and acid as pt_gravwell.
 *
 * The expectation below is HexenWorld's own enum, copied from its last
 * particle.h (engine/hexenworld/client/particle.h before the client was
 * folded into this one), under hw_ names.  It is written out again here on
 * purpose: the test must not take its answers from the table it checks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef float		vec3_t[3];
typedef unsigned char	byte;

#include "hw_particle_mapping.h"

static int	failures;

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "hw_particle_map: " __VA_ARGS__); \
			fputc('\n', stderr); \
			failures++; \
		} \
	} while (0)

typedef enum
{
	hw_static, hw_grav, hw_fastgrav, hw_slowgrav, hw_fire, hw_explode,
	hw_explode2, hw_blob, hw_blob2, hw_rain, hw_c_explode, hw_c_explode2,
	hw_spit, hw_fireball, hw_ice, hw_spell, hw_test, hw_quake, hw_rd,
	hw_vorpal, hw_setstaff, hw_magicmissile, hw_boneshard, hw_scarab,
	hw_darken, hw_grensmoke, hw_redfire, hw_acidball, hw_bluestep,
	hw_count
} hw_ptype_t;

/* The Hexen II type that runs each HexenWorld effect, by name. */
static const struct
{
	hw_ptype_t	wire;
	ptype_t		want;
	const char	*name;
} cases[] = {
	{ hw_static, pt_static, "static" },
	{ hw_grav, pt_grav, "grav" },
	{ hw_fastgrav, pt_fastgrav, "fastgrav" },
	{ hw_slowgrav, pt_slowgrav, "slowgrav" },
	{ hw_fire, pt_fire, "fire" },
	{ hw_explode, pt_explode, "explode" },
	{ hw_explode2, pt_explode2, "explode2" },
	{ hw_blob, pt_blob, "blob" },
	{ hw_blob2, pt_blob2, "blob2" },
	{ hw_rain, pt_rain, "rain" },
	{ hw_c_explode, pt_c_explode, "c_explode" },
	{ hw_c_explode2, pt_c_explode2, "c_explode2" },
	{ hw_spit, pt_spit, "spit" },
	{ hw_fireball, pt_fireball, "fireball" },
	{ hw_ice, pt_ice, "ice" },
	{ hw_spell, pt_spell, "spell" },
	{ hw_test, pt_test, "test" },
	{ hw_quake, pt_quake, "quake" },
	{ hw_rd, pt_rd, "rd" },
	{ hw_vorpal, pt_vorpal, "vorpal" },
	{ hw_setstaff, pt_setstaff, "setstaff" },
	{ hw_magicmissile, pt_magicmissile, "magicmissile" },
	{ hw_boneshard, pt_boneshard, "boneshard" },
	{ hw_scarab, pt_scarab, "scarab" },
	{ hw_darken, pt_darken, "darken" },
	{ hw_grensmoke, pt_grensmoke, "grensmoke" },
	{ hw_redfire, pt_redfire, "redfire" },
	{ hw_acidball, pt_acidball, "acidball" },
	{ hw_bluestep, pt_bluestep, "bluestep" },
};

static const char *EngineSourceDir (void)
{
#ifdef HW_PARTICLE_TEST_ENGINE_SOURCE_DIR
	return HW_PARTICLE_TEST_ENGINE_SOURCE_DIR;
#else
	return "engine";
#endif
}

static char *ReadSourceFile (const char *relative_path)
{
	char	path[1024];
	FILE	*f;
	long	len;
	char	*buf;

	if ((size_t)snprintf(path, sizeof(path), "%s/%s", EngineSourceDir(),
			     relative_path) >= sizeof(path))
		return NULL;
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char *) malloc((size_t)len + 1);
	if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len)
	{
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[len] = 0;
	fclose(f);
	return buf;
}

static int Count (const char *hay, const char *needle)
{
	int	n = 0;

	for (hay = strstr(hay, needle); hay; hay = strstr(hay + 1, needle))
		n++;
	return n;
}

int main (void)
{
	size_t	i;
	int	v;
	char	*src;

	/* HexenWorld's numbering, as the wire carries it. */
	CHECK(hw_count == 29 && hw_scarab == 23 && hw_darken == 24 &&
	      hw_grensmoke == 25 && hw_redfire == 26 && hw_acidball == 27 &&
	      hw_bluestep == 28, "the test's copy of HexenWorld's enum is off");
	CHECK(HW_PARTICLE_TYPES == hw_count, "table has %d types, HexenWorld has %d",
	      HW_PARTICLE_TYPES, (int)hw_count);
	CHECK(sizeof(cases) / sizeof(cases[0]) == hw_count, "a wire value has no case");

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		CHECK((size_t)cases[i].wire == i, "case %d is out of order", (int)i);
		CHECK(HW_H2ParticleMap((int)cases[i].wire) == cases[i].want,
		      "wire %d (HW pt_%s) -> %d, want %d", (int)cases[i].wire,
		      cases[i].name, (int)HW_H2ParticleMap((int)cases[i].wire),
		      (int)cases[i].want);
	}

	/* The bug itself: from 24 up, the cast picked a different effect. */
	for (v = hw_darken; v <= hw_bluestep; v++)
		CHECK((int)HW_H2ParticleMap(v) != v, "wire %d still maps to itself", v);

	/* Everything a byte can hold that HexenWorld never defined, and the
	 * values either side of the int range a caller could pass. */
	for (v = hw_count; v <= 255; v++)
		CHECK(HW_H2ParticleMap(v) == pt_static, "wire %d is not pt_static", v);
	CHECK(HW_H2ParticleMap(-1) == pt_static, "-1 is not pt_static");
	CHECK(HW_H2ParticleMap(256) == pt_static, "256 is not pt_static");

	/* Hexen II's own numbers must not move: saved games and its own
	 * svc_particle2 carry them.  HexenWorld's two extras go on the end. */
	CHECK(pt_scarab == 23 && pt_acidball == 24 && pt_darken == 25 &&
	      pt_snow == 26 && pt_gravwell == 27 && pt_redfire == 28 &&
	      pt_grensmoke == 29 && pt_bluestep == 30, "a Hexen II ptype_t number moved");

	/* The three HexenWorld readers go through the map, and none casts. */
	src = ReadSourceFile("hexen2/cl_hw.c");
	CHECK(src != NULL, "cannot read hexen2/cl_hw.c under %s", EngineSourceDir());
	if (src)
	{
		CHECK(Count(src, "effect = HW_H2ParticleMap (MSG_ReadByte ());") == 3,
		      "cl_hw.c: svc_particle2/3/4 must each map the wire type");
		CHECK(Count(src, "(ptype_t)") == 0 && Count(src, "(ptype_t )") == 0,
		      "cl_hw.c casts something to ptype_t");
		free(src);
	}

	/* pt_bluestep is simulated, not merely numbered. */
	src = ReadSourceFile("hexen2/r_part.c");
	CHECK(src != NULL, "cannot read hexen2/r_part.c under %s", EngineSourceDir());
	if (src)
	{
		CHECK(Count(src, "case pt_bluestep:") == 1,
		      "r_part.c: R_UpdateParticles has no pt_bluestep case");
		free(src);
	}

	if (failures)
	{
		fprintf(stderr, "hw_particle_map: %d failure(s)\n", failures);
		return 1;
	}
	printf("hw_particle_map: every HexenWorld wire type 0..28 maps to its Hexen II effect\n");
	return 0;
}
