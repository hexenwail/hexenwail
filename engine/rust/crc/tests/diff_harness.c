// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for common/crc.c and the Rust crc module.  The C
// original is compiled with c_CRC_* names so both implementations live in one
// process.  Everything here is deterministic: the byte-step check is
// exhaustive over all 2^16 running values x 2^8 input bytes, and the block
// checks use a fixed-seed xorshift stream.

#include "q_stdinc.h"
#include "crc.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		return 1; \
	} \
} while (0)

extern void c_CRC_Init(unsigned short *crcvalue);
extern void c_CRC_ProcessByte(unsigned short *crcvalue, unsigned char data);
extern void c_CRC_ProcessBlock(unsigned char *start, unsigned short *crcvalue, int count);
extern unsigned short c_CRC_Value(unsigned short crcvalue);
extern unsigned short c_CRC_Block(unsigned char *start, int count);

// Engine callbacks other features in the consolidated archive reference.
// The crc module needs none of them; they exist only so the link succeeds
// whichever codegen units the linker pulls, and must never be reached.
void Sys_Error(const char *fmt, ...) { (void)fmt; abort(); }
void CON_Printf(unsigned int flags, const char *fmt, ...) { (void)flags; (void)fmt; abort(); }
void *Hunk_AllocName(int size, const char *name) { (void)size; (void)name; abort(); }

static uint32_t rng_state = 0x2545f491u;

static uint32_t rng(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rng_state = x;
}

static int abi_checks(void)
{
	// crc.h's unsigned short is what the Rust side declares as u16.
	CHECK(sizeof(unsigned short) == 2, "unsigned short is %zu bytes", sizeof(unsigned short));
	return 0;
}

static int init_checks(void)
{
	unsigned short c = 0x1234, r = 0x5678;

	c_CRC_Init(&c);
	CRC_Init(&r);
	CHECK(c == r && r == 0xffff, "CRC_Init differs: C=%04x Rust=%04x", c, r);
	return 0;
}

// Every (running value, byte) pair.  This exercises each table entry from
// every possible high byte, so a single wrong entry or shift cannot hide.
static int byte_checks(void)
{
	unsigned int v, d;

	for (v = 0; v <= 0xffff; ++v) {
		for (d = 0; d <= 0xff; ++d) {
			unsigned short c = (unsigned short)v, r = (unsigned short)v;

			c_CRC_ProcessByte(&c, (unsigned char)d);
			CRC_ProcessByte(&r, (unsigned char)d);
			if (c != r) {
				CHECK(0, "ProcessByte(%04x, %02x): C=%04x Rust=%04x", v, d, c, r);
			}
		}
	}
	return 0;
}

static int value_checks(void)
{
	unsigned int v;

	for (v = 0; v <= 0xffff; ++v) {
		unsigned short c = c_CRC_Value((unsigned short)v);
		unsigned short r = CRC_Value((unsigned short)v);

		CHECK(c == r && r == v, "CRC_Value(%04x): C=%04x Rust=%04x", v, c, r);
	}
	return 0;
}

// Known answers: this is CRC-16/CCITT-FALSE (a.k.a. the XMODEM table with a
// 0xffff seed).  The catalogue check value for "123456789" is 0x29b1.
static int known_answer_checks(void)
{
	unsigned char digits[] = "123456789";

	CHECK(c_CRC_Block(digits, 9) == 0x29b1, "C check value is %04x", c_CRC_Block(digits, 9));
	CHECK(CRC_Block(digits, 9) == 0x29b1, "Rust check value is %04x", CRC_Block(digits, 9));
	CHECK(c_CRC_Block(digits, 0) == 0xffff && CRC_Block(digits, 0) == 0xffff,
		"empty block is not the init value");
	return 0;
}

static int block_checks(void)
{
	enum { BUF = 8192 };
	static unsigned char c_buf[BUF + 16], r_buf[BUF + 16], pristine[BUF + 16];
	int trial;

	for (trial = 0; trial < (int)sizeof(pristine); ++trial)
		pristine[trial] = (unsigned char)rng();
	memcpy(c_buf, pristine, sizeof(pristine));
	memcpy(r_buf, pristine, sizeof(pristine));

	for (trial = 0; trial < 20000; ++trial) {
		// Lengths cover 0..BUF with a bias toward short blocks; offsets make
		// the start pointer deliberately unaligned.
		int len = (trial < 512) ? trial : (int)(rng() % (BUF + 1));
		int off = (int)(rng() % 16);
		unsigned short seed = (unsigned short)rng();
		unsigned short c = seed, r = seed, chained = seed;
		unsigned short cb, rb;
		int i;

		c_CRC_ProcessBlock(c_buf + off, &c, len);
		CRC_ProcessBlock(r_buf + off, &r, len);
		CHECK(c == r, "ProcessBlock(seed=%04x, off=%d, len=%d): C=%04x Rust=%04x",
			seed, off, len, c, r);

		cb = c_CRC_Block(c_buf + off, len);
		rb = CRC_Block(r_buf + off, len);
		CHECK(cb == rb, "CRC_Block(off=%d, len=%d): C=%04x Rust=%04x", off, len, cb, rb);

		// The block path must equal the byte path: engine callers use both
		// for the same kind of data (quakefs/model use bytes, pr_edict uses
		// blocks).
		if (len <= 512) {
			for (i = 0; i < len; ++i)
				CRC_ProcessByte(&chained, r_buf[off + i]);
			CHECK(chained == r, "Rust byte chain %04x != Rust block %04x (len=%d)",
				chained, r, len);
		}
	}

	CHECK(memcmp(c_buf, pristine, sizeof(pristine)) == 0, "C modified its input");
	CHECK(memcmp(r_buf, pristine, sizeof(pristine)) == 0, "Rust modified its input");
	return 0;
}

// count == 0 must not touch the data pointer, so NULL is legal there.
static int null_empty_checks(void)
{
	unsigned short c = 0xbeef, r = 0xbeef;

	c_CRC_ProcessBlock(NULL, &c, 0);
	CRC_ProcessBlock(NULL, &r, 0);
	CHECK(c == 0xbeef && r == 0xbeef, "empty block changed crc: C=%04x Rust=%04x", c, r);
	CHECK(c_CRC_Block(NULL, 0) == 0xffff && CRC_Block(NULL, 0) == 0xffff,
		"empty NULL block is not the init value");
	return 0;
}

int main(void)
{
	if (abi_checks() || init_checks() || byte_checks() || value_checks()
		|| known_answer_checks() || block_checks() || null_empty_checks())
		return 1;
	puts("crc differential harness: PASS");
	return 0;
}
