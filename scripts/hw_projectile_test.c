/* Standalone fixtures for engine/hexen2/cl_hw_projectiles.inc.
 * Build through the repository dev shell:
 *   nix develop --command cc -Wall -Wextra -Werror \
 *     -o hw_projectile_test scripts/hw_projectile_test.c
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char byte;
typedef float vec3_t[3];

static byte message[8192];
static int message_size;
static int msg_readcount;
static int msg_badread;
static int checks;
static int failures;

static int MSG_ReadByte (void)
{
	if (msg_readcount + 1 > message_size)
	{
		msg_badread = 1;
		return -1;
	}
	return message[msg_readcount++];
}

#include "../engine/hexen2/cl_hw_projectiles.inc"

static void check (int condition, const char *format, ...)
{
	va_list args;

	checks++;
	if (condition)
		return;
	failures++;
	fprintf (stderr, "FAIL: ");
	va_start (args, format);
	vfprintf (stderr, format, args);
	va_end (args);
	fputc ('\n', stderr);
}

#define CHECK(condition, ...) check ((condition), __VA_ARGS__)

static void reset_message (void)
{
	message_size = 0;
	msg_readcount = 0;
	msg_badread = 0;
	hwcl_num_packed_ravens = 0;
	hwcl_num_packed_missiles = 0;
	memset (hwcl_packed_ravens, 0, sizeof(hwcl_packed_ravens));
	memset (hwcl_packed_missiles, 0, sizeof(hwcl_packed_missiles));
}

static void put_byte (int value)
{
	message[message_size++] = (byte)value;
}

static void put_origin (int x, int y, int z, byte *bits)
{
	int packed_x = (x + 4096) >> 1;
	int packed_y = (y + 4096) >> 1;
	int packed_z = (z + 4096) >> 1;

	bits[0] = packed_x;
	bits[1] = (packed_x >> 8) | (packed_y << 4);
	bits[2] = packed_y >> 4;
	bits[3] = packed_z;
	bits[4] = packed_z >> 8;
}

static void put_raven (int x, int y, int z, int pitch, int yaw,
		int frame, int model)
{
	byte bits[6];
	int i;

	put_origin (x, y, z, bits);
	bits[4] |= ((pitch * 16 / 360) & 15) << 4;
	if (model == HWCL_RAVEN_MODEL)
		bits[5] = ((yaw * 32 / 360) & 31) | ((frame & 7) << 5);
	else
		bits[5] = (yaw * 256 / 360) & 255;
	for (i = 0; i < 6; i++)
		put_byte (bits[i]);
}

static void put_missile (int x, int y, int z, int type)
{
	byte bits[5];
	int i;

	put_origin (x, y, z, bits);
	bits[4] |= (type & 15) << 4;
	for (i = 0; i < 5; i++)
		put_byte (bits[i]);
}

static void test_zero_counts (void)
{
	reset_message ();
	put_byte (0);
	put_byte (0);
	put_byte (0x7e);
	HWCL_ParseNails ();
	CHECK (!msg_badread, "zero-count ravens marked bad");
	CHECK (hwcl_num_packed_ravens == 0, "zero-count ravens stored %d",
		hwcl_num_packed_ravens);
	CHECK (msg_readcount == 2, "zero-count ravens consumed %d bytes", msg_readcount);
	CHECK (MSG_ReadByte () == 0x7e, "zero-count ravens consumed the next opcode");

	reset_message ();
	put_byte (0);
	put_byte (0x6d);
	HWCL_ParsePackedMissiles ();
	CHECK (!msg_badread, "zero-count missiles marked bad");
	CHECK (hwcl_num_packed_missiles == 0, "zero-count missiles stored %d",
		hwcl_num_packed_missiles);
	CHECK (msg_readcount == 1, "zero-count missiles consumed %d bytes", msg_readcount);
	CHECK (MSG_ReadByte () == 0x6d, "zero-count missiles consumed the next opcode");
}

static void test_both_raven_groups (void)
{
	reset_message ();
	put_byte (1);
	put_raven (-100, 202, 304, 90, 180, 5, HWCL_RAVEN_MODEL);
	put_byte (1);
	put_raven (400, -502, 604, 45, 270, 0, HWCL_RAVEN2_MODEL);
	put_byte (0x55);
	HWCL_ParseNails ();

	CHECK (!msg_badread, "mixed raven groups marked bad");
	CHECK (hwcl_num_packed_ravens == 2, "mixed raven groups stored %d",
		hwcl_num_packed_ravens);
	CHECK (hwcl_packed_ravens[0].model == HWCL_RAVEN_MODEL,
		"first raven resolved to model group %d", hwcl_packed_ravens[0].model);
	CHECK (hwcl_packed_ravens[0].origin[0] == -100 &&
		hwcl_packed_ravens[0].origin[1] == 202 &&
		hwcl_packed_ravens[0].origin[2] == 304,
		"first raven origin did not round-trip");
	CHECK (hwcl_packed_ravens[0].angles[0] == 90 &&
		hwcl_packed_ravens[0].angles[1] == 180 &&
		hwcl_packed_ravens[0].frame == 5,
		"first raven pitch/yaw/frame did not round-trip");
	CHECK (hwcl_packed_ravens[1].model == HWCL_RAVEN2_MODEL &&
		hwcl_packed_ravens[1].angles[0] == 45 &&
		hwcl_packed_ravens[1].angles[1] == 270 &&
		hwcl_packed_ravens[1].frame == 0,
		"second raven pitch/yaw/frame did not round-trip");
	CHECK (MSG_ReadByte () == 0x55, "raven parser consumed the next opcode");
}

static void test_raven_capacity_and_overflow (void)
{
	int i;

	reset_message ();
	put_byte (HWCL_MAX_PACKED_RAVENS);
	for (i = 0; i < HWCL_MAX_PACKED_RAVENS; i++)
		put_raven (i * 2, 0, 0, 0, 0, i & 7, HWCL_RAVEN_MODEL);
	put_byte (0);
	put_byte (0x44);
	HWCL_ParseNails ();
	CHECK (hwcl_num_packed_ravens == HWCL_MAX_PACKED_RAVENS,
		"maximum raven packet stored %d", hwcl_num_packed_ravens);
	CHECK (MSG_ReadByte () == 0x44, "maximum raven packet consumed the next opcode");

	reset_message ();
	put_byte (HWCL_MAX_PACKED_RAVENS);
	for (i = 0; i < HWCL_MAX_PACKED_RAVENS; i++)
		put_raven (i * 2, 0, 0, 0, 0, 0, HWCL_RAVEN_MODEL);
	put_byte (2);
	put_raven (100, 200, 300, 0, 0, 0, HWCL_RAVEN2_MODEL);
	put_raven (102, 202, 302, 0, 0, 0, HWCL_RAVEN2_MODEL);
	put_byte (0x43);
	HWCL_ParseNails ();
	CHECK (hwcl_num_packed_ravens == HWCL_MAX_PACKED_RAVENS,
		"overflow raven packet stored %d", hwcl_num_packed_ravens);
	CHECK (MSG_ReadByte () == 0x43,
		"overflow raven packet failed to consume the second group");
}

static void test_raven_truncation_is_transactional (void)
{
	reset_message ();
	put_byte (1);
	put_raven (10, 20, 30, 0, 0, 0, HWCL_RAVEN_MODEL);
	put_byte (1);
	put_byte (1);
	put_byte (2);
	put_byte (3);
	put_byte (4);
	put_byte (5); /* one byte short */
	HWCL_ParseNails ();
	CHECK (msg_badread, "truncated raven packet was accepted");
	CHECK (hwcl_num_packed_ravens == 0,
		"truncated raven packet committed %d records", hwcl_num_packed_ravens);
	CHECK (msg_readcount == message_size,
		"truncated raven packet stopped at %d of %d", msg_readcount, message_size);
}

static void test_missile_capacity_overflow_and_truncation (void)
{
	int i;

	reset_message ();
	put_byte (HWCL_MAX_PACKED_MISSILES);
	for (i = 0; i < HWCL_MAX_PACKED_MISSILES; i++)
		put_missile (i * 2, -i * 2, 100, i == 0 ? 1 : 2);
	put_byte (0x32);
	HWCL_ParsePackedMissiles ();
	CHECK (hwcl_num_packed_missiles == HWCL_MAX_PACKED_MISSILES,
		"maximum missile packet stored %d", hwcl_num_packed_missiles);
	CHECK (hwcl_packed_missiles[0].type == 1 &&
		hwcl_packed_missiles[1].type == 2,
		"missile types were not preserved");
	CHECK (hwcl_packed_missiles[1].origin[0] == 2 &&
		hwcl_packed_missiles[1].origin[1] == -2 &&
		hwcl_packed_missiles[1].origin[2] == 100,
		"missile origin did not round-trip");
	CHECK (MSG_ReadByte () == 0x32, "maximum missile packet consumed next opcode");

	reset_message ();
	put_byte (HWCL_MAX_PACKED_MISSILES + 1);
	for (i = 0; i < HWCL_MAX_PACKED_MISSILES + 1; i++)
		put_missile (i * 2, 0, 0, 2);
	put_byte (0x31);
	HWCL_ParsePackedMissiles ();
	CHECK (hwcl_num_packed_missiles == HWCL_MAX_PACKED_MISSILES,
		"overflow missile packet stored %d", hwcl_num_packed_missiles);
	CHECK (MSG_ReadByte () == 0x31,
		"overflow missile packet failed to consume every record");

	reset_message ();
	put_byte (2);
	put_missile (10, 20, 30, 1);
	put_byte (1);
	put_byte (2);
	put_byte (3);
	put_byte (4); /* one byte short */
	HWCL_ParsePackedMissiles ();
	CHECK (msg_badread, "truncated missile packet was accepted");
	CHECK (hwcl_num_packed_missiles == 0,
		"truncated missile packet committed %d records", hwcl_num_packed_missiles);
	CHECK (msg_readcount == message_size,
		"truncated missile packet stopped at %d of %d", msg_readcount, message_size);
}

int main (void)
{
	test_zero_counts ();
	test_both_raven_groups ();
	test_raven_capacity_and_overflow ();
	test_raven_truncation_is_transactional ();
	test_missile_capacity_overflow_and_truncation ();

	printf ("hw_projectile_test: %d checks, %d failure(s)\n", checks, failures);
	return failures != 0;
}
