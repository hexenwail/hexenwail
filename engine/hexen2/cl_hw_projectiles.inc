/*
 * Packed HexenWorld projectile decoding shared with the standalone fixture.
 * The including translation unit supplies MSG_ReadByte() and msg_badread.
 */
#define HWCL_MAX_PACKED_RAVENS 32
#define HWCL_MAX_PACKED_MISSILES 32

typedef struct
{
	vec3_t origin;
	vec3_t angles;
	int frame;
	int model;
} hwcl_packed_raven_t;

typedef struct
{
	vec3_t origin;
	int type;
} hwcl_packed_missile_t;

enum
{
	HWCL_RAVEN_MODEL,
	HWCL_RAVEN2_MODEL
};

static hwcl_packed_raven_t hwcl_packed_ravens[HWCL_MAX_PACKED_RAVENS];
static hwcl_packed_missile_t hwcl_packed_missiles[HWCL_MAX_PACKED_MISSILES];
static int hwcl_num_packed_ravens;
static int hwcl_num_packed_missiles;

static void HWCL_DecodePackedOrigin (const byte *bits, vec3_t origin)
{
	origin[0] = ((bits[0] + ((bits[1] & 15) << 8)) << 1) - 4096;
	origin[1] = (((bits[1] >> 4) + (bits[2] << 4)) << 1) - 4096;
	origin[2] = ((bits[3] + ((bits[4] & 15) << 8)) << 1) - 4096;
}

static void HWCL_DecodePackedRaven (const byte *bits, int model,
		hwcl_packed_raven_t *raven)
{
	HWCL_DecodePackedOrigin (bits, raven->origin);
	raven->angles[0] = 360.0f * (bits[4] >> 4) / 16.0f;
	raven->angles[1] = model == HWCL_RAVEN_MODEL ?
		360.0f * (bits[5] & 31) / 32.0f :
		360.0f * bits[5] / 256.0f;
	raven->angles[2] = 0;
	raven->frame = model == HWCL_RAVEN_MODEL ? (bits[5] >> 5) & 7 : 0;
	raven->model = model;
}

static void HWCL_ParsePackedMissiles (void)
{
	hwcl_packed_missile_t decoded[HWCL_MAX_PACKED_MISSILES];
	byte bits[5];
	int count, decoded_count = 0;
	int i, j;

	count = MSG_ReadByte ();
	if (count < 0)
		return;
	for (i = 0; i < count; i++)
	{
		for (j = 0; j < 5; j++)
			bits[j] = MSG_ReadByte ();
		if (!msg_badread && decoded_count < HWCL_MAX_PACKED_MISSILES)
		{
			HWCL_DecodePackedOrigin (bits, decoded[decoded_count].origin);
			decoded[decoded_count].type = bits[4] >> 4;
			decoded_count++;
		}
	}
	if (msg_badread)
		return;

	for (i = 0; i < decoded_count &&
		     hwcl_num_packed_missiles < HWCL_MAX_PACKED_MISSILES; i++)
		hwcl_packed_missiles[hwcl_num_packed_missiles++] = decoded[i];
}

static void HWCL_ParseNails (void)
{
	hwcl_packed_raven_t decoded[HWCL_MAX_PACKED_RAVENS];
	byte bits[6];
	int count, decoded_count = 0;
	int group, i, j;

	/* svc_nails is overloaded with two consecutive groups.  Read every wire
	 * record even after the frame-local presentation pool fills. */
	for (group = 0; group < 2; group++)
	{
		count = MSG_ReadByte ();
		if (count < 0)
			return;
		for (i = 0; i < count; i++)
		{
			for (j = 0; j < 6; j++)
				bits[j] = MSG_ReadByte ();
			if (!msg_badread && decoded_count < HWCL_MAX_PACKED_RAVENS)
			{
				HWCL_DecodePackedRaven (bits, group, &decoded[decoded_count]);
				decoded_count++;
			}
		}
	}
	if (msg_badread)
		return;

	for (i = 0; i < decoded_count &&
		     hwcl_num_packed_ravens < HWCL_MAX_PACKED_RAVENS; i++)
		hwcl_packed_ravens[hwcl_num_packed_ravens++] = decoded[i];
}
