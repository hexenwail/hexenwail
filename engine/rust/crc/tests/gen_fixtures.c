// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fixture generator for the crc replay.  Links the C original (common/crc.c,
// unmodified) and prints every expected value from it, so tests/fixtures/crc.txt
// can be audited against C at any time:
//
//   bash engine/rust/crc/tests/gen_fixtures.sh          # rewrite the fixture
//   bash engine/rust/crc/tests/gen_fixtures.sh --check  # fail if it drifted
//
// Deterministic: a fixed-seed xorshift32 (the one the retired differential
// harness used), no clock, no pid.  Format: see tests/fixtures/README.md.

#include "q_stdinc.h"
#include "crc.h"

#include <stdint.h>
#include <stdio.h>

static uint32_t rng_state = 0x2545f491u;

static uint32_t rng(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rng_state = x;
}

static void hex(const unsigned char *p, int n)
{
	int i;

	if (n == 0) {
		fputs(" -", stdout);
		return;
	}
	putchar(' ');
	for (i = 0; i < n; ++i)
		printf("%02x", p[i]);
}

static void emit_block(int len, int seeded)
{
	unsigned char buf[512];
	unsigned short seed = (unsigned short)rng(), crc;
	int i;

	for (i = 0; i < len; ++i)
		buf[i] = (unsigned char)rng();
	if (seeded) {
		crc = seed;
		CRC_ProcessBlock(buf, &crc, len);
		printf("pblock %04x", seed);
		hex(buf, len);
		printf(" %04x\n", crc);
	} else {
		printf("block");
		hex(buf, len);
		printf(" %04x\n", CRC_Block(buf, len));
	}
}

static void emit_byte(unsigned short v, unsigned char d)
{
	unsigned short c = v;

	CRC_ProcessByte(&c, d);
	printf("byte %04x %02x %04x\n", v, d, c);
}

int main(void)
{
	static const unsigned short edge_v[] = { 0x0000, 0x0001, 0x00ff, 0x0100,
		0x1021, 0x7fff, 0x8000, 0xff00, 0xffff };
	static const unsigned short value_v[] = { 0x0000, 0x0001, 0x1234, 0x8000,
		0xbeef, 0xffff };
	unsigned char digits[] = "123456789";
	unsigned short c = 0x1234;
	unsigned int i, j;
	int len;

	puts("# crc fixtures, generated from common/crc.c by tests/gen_fixtures.c.");
	puts("# Do not edit by hand: bash engine/rust/crc/tests/gen_fixtures.sh");
	puts("# init <crc>");
	puts("# byte <crc-in> <data> <crc-out>");
	puts("# value <in> <out>");
	puts("# block <data|-> <crc>               CRC_Block");
	puts("# pblock <seed> <data|-> <crc-out>   CRC_ProcessBlock");

	CRC_Init(&c);
	printf("init %04x\n", c);

	// crc=0 makes the output table[data], so these 256 lines cover every
	// table entry directly.
	for (i = 0; i <= 0xff; ++i)
		emit_byte(0x0000, (unsigned char)i);
	// Edge running values against the data bytes that matter.
	for (i = 0; i < sizeof(edge_v) / sizeof(edge_v[0]); ++i)
		for (j = 0; j < 4; ++j)
			emit_byte(edge_v[i], (unsigned char[]){ 0x00, 0x01, 0x80, 0xff }[j]);
	// Seeded random (running value, byte) pairs: the high byte feeds the table
	// index and the low byte the shift.
	for (i = 0; i < 64; ++i) {
		unsigned short v = (unsigned short)rng();
		emit_byte(v, (unsigned char)rng());
	}

	for (i = 0; i < sizeof(value_v) / sizeof(value_v[0]); ++i)
		printf("value %04x %04x\n", value_v[i], CRC_Value(value_v[i]));

	// CRC-16/CCITT-FALSE catalogue check value, 0x29b1.
	printf("block");
	hex(digits, 9);
	printf(" %04x\n", CRC_Block(digits, 9));

	// Every length 0..40 (loop entry, exit, the off-by-one boundary), plain
	// and with a random running seed, then a few longer blocks.
	for (len = 0; len <= 40; ++len) {
		emit_block(len, 0);
		emit_block(len, 1);
	}
	for (len = 41; len <= 300; len += 37) {
		emit_block(len, 0);
		emit_block(len, 1);
	}
	emit_block(512, 0);
	emit_block(512, 1);
	return 0;
}
