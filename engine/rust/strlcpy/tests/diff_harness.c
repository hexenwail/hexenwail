// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for common/strlcpy.c and the Rust strlcpy module.  The
// C original is compiled as c_q_strlcpy, so both implementations receive the
// same source bytes and independently owned destination buffers in one process.

#include <stddef.h>
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

extern size_t c_q_strlcpy(char *dst, const char *src, size_t siz);
extern size_t q_strlcpy(char *dst, const char *src, size_t siz);

/* Other optional Rust units can be pulled from the static archive by some
 * linkers.  strlcpy itself reaches none of these callbacks. */
void Sys_Error(const char *fmt, ...) { (void)fmt; abort(); }
void CON_Printf(unsigned int flags, const char *fmt, ...) { (void)flags; (void)fmt; abort(); }
void *Hunk_AllocName(int size, const char *name) { (void)size; (void)name; abort(); }

#define GUARD 16
#define MAX_SOURCE 256
#define MAX_SIZE 288
#define MAX_OFFSET 7
#define STORAGE (GUARD + MAX_OFFSET + MAX_SIZE + GUARD)
#define CANARY 0xa5

static uint32_t rng_state = 0x6d2b79f5u;

static uint32_t rng(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rng_state = x;
}

static int check_buffer_bounds(const char *which, const unsigned char *buf,
	size_t start, size_t siz)
{
	size_t i;

	for (i = 0; i < STORAGE; ++i) {
		if (i < start || i >= start + siz)
			CHECK(buf[i] == CANARY, "%s wrote outside its %zu-byte destination at %zu",
				which, siz, i);
	}
	return 0;
}

static int check_case(size_t source_len, size_t siz, size_t source_offset,
	size_t destination_offset)
{
	unsigned char source[GUARD + MAX_OFFSET + MAX_SOURCE + 1 + GUARD];
	unsigned char c_store[STORAGE], r_store[STORAGE];
	char *c_dst, *r_dst;
	char *src;
	size_t c_ret, r_ret, start = GUARD + destination_offset;
	size_t i, written;

	memset(source, CANARY, sizeof(source));
	for (i = 0; i < source_len; ++i) {
		unsigned char byte;
		do {
			byte = (unsigned char)rng();
		} while (byte == 0);
		source[GUARD + source_offset + i] = byte;
	}
	source[GUARD + source_offset + source_len] = 0;
	src = (char *)&source[GUARD + source_offset];

	memset(c_store, CANARY, sizeof(c_store));
	memset(r_store, CANARY, sizeof(r_store));
	c_dst = (char *)&c_store[start];
	r_dst = (char *)&r_store[start];

	/* The C implementation permits a null destination when no byte can be
	 * stored; the Rust ABI must preserve that no-dereference property. */
	c_ret = c_q_strlcpy(siz == 0 ? NULL : c_dst, src, siz);
	r_ret = q_strlcpy(siz == 0 ? NULL : r_dst, src, siz);

	CHECK(c_ret == r_ret, "return differs (src=%zu size=%zu): C=%zu Rust=%zu",
		source_len, siz, c_ret, r_ret);
	CHECK(r_ret == source_len, "return is %zu, expected source length %zu", r_ret, source_len);
	CHECK(memcmp(c_store, r_store, sizeof(c_store)) == 0,
		"destination differs (src=%zu size=%zu src_off=%zu dst_off=%zu)",
		source_len, siz, source_offset, destination_offset);
	CHECK(check_buffer_bounds("C", c_store, start, siz) == 0, "C bounds");
	CHECK(check_buffer_bounds("Rust", r_store, start, siz) == 0, "Rust bounds");

	if (siz != 0) {
		written = source_len < siz - 1 ? source_len : siz - 1;
		CHECK((unsigned char)r_dst[written] == 0,
			"destination lacks terminator (src=%zu size=%zu)", source_len, siz);
		CHECK(memcmp(r_dst, src, written) == 0,
			"destination prefix differs (src=%zu size=%zu)", source_len, siz);
	}
	return 0;
}

int main(void)
{
	size_t source_len, siz, source_offset, destination_offset;
	unsigned long checks = 0;

	for (source_len = 0; source_len <= MAX_SOURCE; ++source_len) {
		for (siz = 0; siz <= MAX_SIZE; ++siz) {
			for (source_offset = 0; source_offset <= MAX_OFFSET; ++source_offset) {
				destination_offset = (source_len + siz + source_offset) % (MAX_OFFSET + 1);
				if (check_case(source_len, siz, source_offset, destination_offset))
					return 1;
				++checks;
			}
		}
	}

	printf("strlcpy differential harness: PASS (%lu cases)\n", checks);
	return 0;
}
