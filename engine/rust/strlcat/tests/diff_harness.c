// SPDX-License-Identifier: ISC
//
// Differential harness for common/strlcat.c and the Rust strlcat module.  The
// C original is compiled as c_q_strlcat, so both implementations receive the
// same source bytes and independently owned destination buffers seeded with the
// same contents, in one process.
//
// WHAT IT PINS, beyond "the two destinations agree byte for byte":
//
//   - the return value is strlen(src) + MIN(siz, strlen(initial dst)), which is
//     the truncation contract callers such as cmd.c test with `>= sizeof(cmd)`;
//   - a destination with no NUL inside `siz` is not written to at all, and
//     its return value still adds the whole of strlen(src);
//   - bytes outside the permitted write span -- both the canary guards around
//     the destination and the untouched tail inside it -- keep their initial
//     value, which is what catches a copy that runs one byte long;
//   - `siz == 0` writes nothing and never dereferences the destination, so the
//     harness may pass NULL for both implementations.

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

extern size_t c_q_strlcat(char *dst, const char *src, size_t siz);
extern size_t q_strlcat(char *dst, const char *src, size_t siz);

/* Other optional Rust units can be pulled from the static archive by some
 * linkers.  strlcat itself reaches none of these callbacks. */
void Sys_Error(const char *fmt, ...) { (void)fmt; abort(); }
void CON_Printf(unsigned int flags, const char *fmt, ...) { (void)flags; (void)fmt; abort(); }
void *Hunk_AllocName(int size, const char *name) { (void)size; (void)name; abort(); }

#define GUARD 16
#define MAX_SOURCE 256
#define MAX_SIZE 288
#define MAX_OFFSET 7
#define STORAGE (GUARD + MAX_OFFSET + MAX_SIZE + GUARD)
#define SOURCE_STORAGE (GUARD + MAX_OFFSET + MAX_SOURCE + 1 + GUARD)
#define CANARY 0xa5

/* The initial destination contents, one shape per pass.  Between them they
 * cover every way the destination length can sit relative to `siz`: a length of
 * zero, a length of exactly siz-1 (room for the terminator alone), a length of
 * siz (no NUL to find, so the caller gets the "cannot append" path), and an
 * intermediate length that truncates whenever source_len does not fit. */
enum dst_kind {
	DST_EMPTY = 0,		/* dst[0] == '\0' */
	DST_LAST_BYTE_NUL,	/* NUL in the final usable byte, dlen == siz-1 */
	DST_NO_NUL,		/* no NUL anywhere in siz, dlen == siz */
	DST_PARTIAL,		/* NUL at a case-derived index, dlen < siz */
	DST_KIND_COUNT
};

/* Deterministic per-case seed: the byte stream depends on the case coordinates,
 * not on the order the loops visit them, so a failure is reproducible from the
 * printed coordinates alone. */
static uint32_t seed_for(size_t kind, size_t source_len, size_t siz,
	size_t source_offset, size_t destination_offset)
{
	uint32_t h = 0x811c9dc5u;

	h = (h ^ (uint32_t)kind) * 16777619u;
	h = (h ^ (uint32_t)source_len) * 16777619u;
	h = (h ^ (uint32_t)siz) * 16777619u;
	h = (h ^ (uint32_t)source_offset) * 16777619u;
	h = (h ^ (uint32_t)destination_offset) * 16777619u;
	return h ? h : 1u;
}

static uint32_t next_byte(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *state = x;
}

/* Destination fill bytes must be nonzero: a zero would end the C scan early and
 * turn a "no terminator" case into a partial one. */
static unsigned char nonzero_byte(uint32_t *state)
{
	unsigned char byte;

	do {
		byte = (unsigned char)next_byte(state);
	} while (byte == 0);
	return byte;
}

static int check_case(size_t kind, size_t source_len, size_t siz,
	size_t source_offset, size_t destination_offset)
{
	unsigned char source[SOURCE_STORAGE], source_copy[SOURCE_STORAGE];
	unsigned char initial[STORAGE], expected[STORAGE];
	unsigned char c_store[STORAGE], r_store[STORAGE];
	uint32_t state = seed_for(kind, source_len, siz, source_offset, destination_offset);
	char *c_dst, *r_dst, *src;
	size_t start = GUARD + destination_offset;
	size_t dlen, written, i, c_ret, r_ret;

	memset(source, CANARY, sizeof(source));
	for (i = 0; i < source_len; ++i)
		source[GUARD + source_offset + i] = nonzero_byte(&state);
	source[GUARD + source_offset + source_len] = 0;
	src = (char *)&source[GUARD + source_offset];

	memset(initial, CANARY, sizeof(initial));
	for (i = 0; i < siz; ++i)
		initial[start + i] = nonzero_byte(&state);
	switch (kind) {
	case DST_EMPTY:
		if (siz != 0)
			initial[start] = 0;
		break;
	case DST_LAST_BYTE_NUL:
		if (siz != 0)
			initial[start + siz - 1] = 0;
		break;
	case DST_NO_NUL:
		break;
	case DST_PARTIAL:
		if (siz != 0)
			initial[start + ((source_len + destination_offset) % siz)] = 0;
		break;
	default:
		fprintf(stderr, "internal: unknown destination kind %zu\n", kind);
		return 1;
	}

	/* strlen(initial dst) as an observer would measure it, not as we built it. */
	dlen = siz;
	for (i = 0; i < siz; ++i) {
		if (initial[start + i] == 0) {
			dlen = i;
			break;
		}
	}

	memcpy(source_copy, source, sizeof(source));
	/* The expected final image: initial contents, then exactly the append the
	 * contract permits.  When dlen == siz the contract permits nothing. */
	memcpy(expected, initial, sizeof(expected));
	if (dlen < siz) {
		written = source_len < siz - 1 - dlen ? source_len : siz - 1 - dlen;
		memcpy(&expected[start + dlen], src, written);
		expected[start + dlen + written] = 0;
	}

	memcpy(c_store, initial, sizeof(c_store));
	memcpy(r_store, initial, sizeof(r_store));
	c_dst = (char *)&c_store[start];
	r_dst = (char *)&r_store[start];

	/* siz == 0 is the one case where the C original never reads dst, so both
	 * implementations are handed a NULL to prove the Rust side keeps that. */
	c_ret = c_q_strlcat(siz == 0 ? NULL : c_dst, src, siz);
	r_ret = q_strlcat(siz == 0 ? NULL : r_dst, src, siz);

	CHECK(c_ret == r_ret,
		"return differs (kind=%zu src=%zu size=%zu src_off=%zu dst_off=%zu):"
		" C=%zu Rust=%zu", kind, source_len, siz, source_offset,
		destination_offset, c_ret, r_ret);
	CHECK(r_ret == dlen + source_len,
		"return is %zu, expected strlen(src)+MIN(siz,strlen(dst)) = %zu + %zu"
		" (kind=%zu size=%zu)", r_ret, source_len, dlen, kind, siz);

	CHECK(memcmp(c_store, r_store, sizeof(c_store)) == 0,
		"destination differs (kind=%zu src=%zu size=%zu src_off=%zu dst_off=%zu)",
		kind, source_len, siz, source_offset, destination_offset);
	CHECK(memcmp(r_store, expected, sizeof(r_store)) == 0,
		"Rust destination is not the contract's image"
		" (kind=%zu src=%zu size=%zu src_off=%zu dst_off=%zu)",
		kind, source_len, siz, source_offset, destination_offset);
	CHECK(memcmp(c_store, expected, sizeof(c_store)) == 0,
		"C destination is not the contract's image"
		" (kind=%zu src=%zu size=%zu src_off=%zu dst_off=%zu)",
		kind, source_len, siz, source_offset, destination_offset);

	/* Canary guards on both sides of the destination, stated separately from
	 * the image comparison so a write one byte past the end names itself. */
	for (i = 0; i < STORAGE; ++i) {
		if (i < start || i >= start + siz) {
			CHECK(c_store[i] == CANARY,
				"C wrote outside its %zu-byte destination at %zu", siz, i);
			CHECK(r_store[i] == CANARY,
				"Rust wrote outside its %zu-byte destination at %zu", siz, i);
		}
	}
	CHECK(memcmp(source, source_copy, sizeof(source)) == 0,
		"an implementation modified the source (kind=%zu src=%zu size=%zu)",
		kind, source_len, siz);
	return 0;
}

int main(void)
{
	size_t kind, source_len, siz, source_offset, destination_offset;
	unsigned long checks = 0;

	for (kind = 0; kind < DST_KIND_COUNT; ++kind) {
		for (source_len = 0; source_len <= MAX_SOURCE; ++source_len) {
			for (siz = 0; siz <= MAX_SIZE; ++siz) {
				for (source_offset = 0; source_offset <= MAX_OFFSET;
					++source_offset) {
					destination_offset =
						(source_len + siz + source_offset) % (MAX_OFFSET + 1);
					if (check_case(kind, source_len, siz, source_offset,
							destination_offset))
						return 1;
					++checks;
				}
			}
		}
	}

	printf("strlcat differential harness: PASS (%lu cases)\n", checks);
	return 0;
}