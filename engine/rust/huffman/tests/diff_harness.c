// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for engine/hexenworld/shared/huffman.c and the Rust
// huffman module in engine/rust/src/huffman.rs.
//
// The C original is compiled TWICE, with all three exported names renamed:
//
//   c_  as the integrated Hexenwail client compiles it -- -DH2W_INTEGRATED,
//       and the Hexen II include order.  huffman.c's USE_HUNKMEM is 0 there,
//       so this variant mallocs its tree.  That is the variant whose
//       allocation the Rust module uses for both targets.
//
//   w_  as hwsv compiles it -- -DH2W -DSERVERONLY, the HexenWorld include
//       order.  USE_HUNKMEM is 1 there and the tree comes from
//       Hunk_AllocName, which this harness stubs.  The stub fills the block
//       with 0xA5 rather than zeroing it, so a build that depended on
//       uninitialised hunk memory would show up as a byte difference.
//
// Both variants are then compared with the Rust module.  The C-vs-C
// comparison runs first and on its own, because it is what makes the choice
// of one allocation path safe to make: the archive is built once and linked
// into both targets, so the Rust has to pick one, and it may only do so if
// the two C variants are indistinguishable at the byte level.
//
// HuffDecode on malformed input.  The C's GetBit has no bound: only the outer
// loop is limited, by tbits = inlen*8 - *in, and the header byte is
// attacker-controlled because net_udp.c decodes whatever arrived on the UDP
// socket.  With `*in` smaller than the real padding count the decoder is left
// mid-code at the end of the payload and reads on into whatever follows the
// caller's buffer.  The Rust module stops at the end of the buffer instead.
// The C's behaviour there is undefined, so there is no byte-for-byte answer to
// compare against, and this harness does not pretend otherwise:
//
//   * every decode runs in a forked child with the packet placed so that its
//     last byte abuts an unmapped page.  A read past the end is a SIGSEGV,
//     so "the C overran this input" and "the Rust did not" are both observed
//     rather than assumed.  Every input the C survives is compared against
//     the Rust byte for byte; that is the bulk of the corpus.
//
//   * when the C dies on the guard page, the same packet is also decoded by
//     the C in a buffer with 64 zero bytes of tail, which makes the C's read
//     defined (it is reading its own buffer) and gives the Rust's answer a
//     reference: the Rust decodes greedy symbols from bit 0 with the same
//     tree, so it stops earlier and its bytes must be a strict prefix of what
//     the tail run produced.  A Rust that read past the end would not be a
//     prefix, and a Rust that stopped early for no reason would not be
//     strict.  The tail is 64 bytes because the decoder reads at most one
//     code past the end, and FindTab refuses to build a code longer than 32
//     bits.
//
// What is covered, against issue #236's list:
//
//   * the frequency table: all 256 values compared bit for bit against
//     `static const float HuffFreq[256]`, built here from the same
//     hufffreq.h the C includes.  BuildTree compares frequencies with `<`,
//     so one differing float rewrites every encoded byte;
//   * round trips for every single byte value, and for 64 repetitions of
//     every byte value (which drives each symbol's actual code through the
//     encoder, since a one-byte input always takes the raw fallback);
//   * seeded random buffers, encoded and decoded, at several maxlen values;
//   * the 0xff raw fallback HuffEncode takes when compression does not help,
//     both directions, and how many of the corpus took it;
//   * the `--inlen` underflow path: inlen 0 and negative leave *outlen at 0
//     and touch nothing;
//   * decoding with maxlen smaller than the decoded length, including 0, and
//     the "out[maxlen - 1] is written already" boundary;
//   * crafted padding bytes: the padding bits of a valid packet set to 1,
//     which must not change the decoded bytes;
//   * truncated packets and a full sweep of the header byte, over the
//     malformed inputs above;
//   * a seeded fuzz sweep over malformed input.
//
// The fatal paths in the C (Sys_Error on allocation failure, "no huff node",
// "no one in node", "compression screwd", "minat1"/"minat2") have no test
// here and cannot have one: every one of them needs a frequency table or an
// allocator the public ABI does not expose, and with the fixed table
// BuildTree's two minima always exist and the tree is always complete.  They
// are reproduced in the Rust for the same conditions the C has them, and this
// note is here so their absence from the corpus is not read as coverage.
// Sys_Error itself aborts here, so a port that took one of those paths during
// the corpus would end the run loudly.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// The C's `static const float HuffFreq[256]`, from the header huffman.c
// includes as its initialiser.  Same preprocessing, same literals, so this is
// the C's table rather than a restatement of it.
static const float c_HuffFreq[256] = {
#include "hufffreq.h"
};

/*----------------------------------------------------------------------------
 * the implementations
 *--------------------------------------------------------------------------*/

extern void w_HuffInit(void);
extern void w_HuffEncode(const unsigned char *in, unsigned char *out, int inlen, int *outlen);
extern void w_HuffDecode(const unsigned char *in, unsigned char *out, int inlen, int *outlen, const int maxlen);

extern void c_HuffInit(void);
extern void c_HuffEncode(const unsigned char *in, unsigned char *out, int inlen, int *outlen);
extern void c_HuffDecode(const unsigned char *in, unsigned char *out, int inlen, int *outlen, const int maxlen);

extern void HuffInit(void);
extern void HuffEncode(const unsigned char *in, unsigned char *out, int inlen, int *outlen);
extern void HuffDecode(const unsigned char *in, unsigned char *out, int inlen, int *outlen, const int maxlen);

// What the harness needs from the Rust side, in the shape the sizebuf, msg_io
// and info_str ports use for their layout accessors.
extern float Huffman_freq(unsigned long index);
extern int Huffman_lookup_len(unsigned long val);
extern unsigned int Huffman_lookup_bits(unsigned long val);

typedef void (*encode_fn)(const unsigned char *, unsigned char *, int, int *);
typedef void (*decode_fn)(const unsigned char *, unsigned char *, int, int *, const int);

struct impl {
	const char *name;
	void (*init)(void);
	encode_fn encode;
	decode_fn decode;
};

enum { IMPL_HWSV, IMPL_CLIENT, IMPL_RUST, IMPL_COUNT };

static const struct impl impls[IMPL_COUNT] = {
	{ "C hwsv (Hunk_AllocName)", w_HuffInit, w_HuffEncode, w_HuffDecode },
	{ "C client (H2W_INTEGRATED, malloc)", c_HuffInit, c_HuffEncode, c_HuffDecode },
	{ "Rust", HuffInit, HuffEncode, HuffDecode },
};

/*----------------------------------------------------------------------------
 * engine symbols
 *--------------------------------------------------------------------------*/

// huffman.c reaches these.  Neither is expected to fire: the fatal paths need
// a hostile frequency table or a failing allocator, and the fixed table
// cannot produce either.  Aborting rather than longjmp-ing keeps the harness
// free of setjmp and makes an unexpected fatal unmistakable.
void Sys_Error(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "FAIL: Sys_Error called: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	fflush(stderr);
	abort();
}

static int hunk_calls;

void *Hunk_AllocName(int size, const char *name)
{
	void *p = malloc((size_t)size);

	(void)name;
	if (!p) {
		fprintf(stderr, "FAIL: Hunk_AllocName(%d) failed\n", size);
		abort();
	}
	// 0xA5, not zero: the hunk path does not memset the tree, and filling it
	// with a recognisable pattern is what shows the build does not read it.
	memset(p, 0xA5, (size_t)size);
	hunk_calls++;
	return p;
}

/*----------------------------------------------------------------------------
 * result plumbing
 *--------------------------------------------------------------------------*/

#define MAX_OUT 640
#define MAX_PKT 256
#define TAIL 64
#define POISON 0xA5

static int checks;
static int failures;
static int fallbacks;
static int bounded_cases;
static int guard_deaths[IMPL_COUNT];

static void fail(const char *fmt, ...)
{
	va_list ap;

	failures++;
	fprintf(stderr, "FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static const char *first_diff(const unsigned char *a, const unsigned char *b, size_t n)
{
	static char buf[96];
	size_t i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			snprintf(buf, sizeof buf, "first difference at byte %zu "
				"(0x%02x vs 0x%02x)", i, a[i], b[i]);
			return buf;
		}
	}
	return "no difference";
}

struct result {
	int ok;			// child reached the end of the call
	int sig;		// signal that killed it, 0 if none
	int outlen;
	unsigned char out[MAX_OUT];
};

/*----------------------------------------------------------------------------
 * the guard page
 *--------------------------------------------------------------------------*/

static unsigned char *guard_region;
static size_t page_size;
static struct result *shared;

static unsigned char *guard_slot(int n)
{
	return guard_region + page_size - (size_t)n;
}

static void setup_guard(void)
{
	long ps = sysconf(_SC_PAGESIZE);

	if (ps <= 0) {
		fprintf(stderr, "FAIL: cannot read the page size\n");
		exit(2);
	}
	page_size = (size_t)ps;

	guard_region = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (guard_region == MAP_FAILED) {
		perror("mmap");
		exit(2);
	}
	// The page after the packet is unmapped, so any read past the end of the
	// caller's buffer is a SIGSEGV -- in code that is not instrumented and
	// was not compiled for it.
	if (mprotect(guard_region + page_size, page_size, PROT_NONE) != 0) {
		perror("mprotect");
		exit(2);
	}

	shared = mmap(NULL, sizeof *shared, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		perror("mmap(shared)");
		exit(2);
	}
}

// Decode one packet in a child process, with the packet's last byte against
// the guard page.  The child writes its result into shared memory, so a
// survival is a real answer and a death is a signal, never a hang.
static void guard_run(int impl, const unsigned char *pkt, int pktlen, int maxlen,
		struct result *out)
{
	pid_t pid;
	int status;

	memset(out, 0, sizeof *out);
	out->outlen = -1;
	shared->ok = 0;
	shared->sig = 0;
	shared->outlen = -1;
	memset(shared->out, POISON, sizeof shared->out);

	if (pktlen > 0) {
		if ((size_t)pktlen > page_size) {
			fprintf(stderr, "FAIL: packet larger than a page\n");
			exit(2);
		}
		memcpy(guard_slot(pktlen), pkt, (size_t)pktlen);
	}

	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(2);
	}
	if (pid == 0) {
		const unsigned char *in = pktlen > 0 ? guard_slot(pktlen) : (const unsigned char *)"";

		impls[impl].decode(in, shared->out, pktlen, &shared->outlen, maxlen);
		shared->ok = 1;
		_exit(0);
	}

	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		exit(2);
	}

	out->outlen = shared->outlen;
	out->ok = shared->ok;
	memcpy(out->out, shared->out, sizeof out->out);

	if (WIFSIGNALED(status)) {
		out->sig = WTERMSIG(status);
		out->ok = 0;
	} else if (WEXITSTATUS(status) != 0) {
		out->ok = 0;
	}
}

// The C's decode of the same packet, in a buffer with a zero tail, so the
// read that the guard page catches becomes a read of the C's own memory.  See
// the header comment: only used for inputs the C did not survive, and the
// Rust's answer must be a strict prefix of this one.
static void tail_run(int impl, const unsigned char *pkt, int pktlen, int maxlen,
		struct result *out)
{
	static unsigned char buf[MAX_PKT + TAIL];

	memcpy(buf, pkt, (size_t)pktlen);
	memset(buf + pktlen, 0, TAIL);

	memset(out, 0, sizeof *out);
	out->outlen = -1;
	impls[impl].decode(buf, out->out, pktlen, &out->outlen, maxlen);
	out->ok = 1;
}

/*----------------------------------------------------------------------------
 * comparisons
 *--------------------------------------------------------------------------*/

// Compare two decodes of the same packet: the reported length and the whole
// output buffer, poison included, so a write past the decoded length is a
// failure and not just a difference nobody looks at.
static void cmp_results(const char *what, const struct result *a, const char *an,
		const struct result *b, const char *bn)
{
	checks++;

	if (a->outlen != b->outlen) {
		fail("%s: %s reported *outlen %d, %s reported %d", what, an,
			a->outlen, bn, b->outlen);
		return;
	}
	if (memcmp(a->out, b->out, MAX_OUT) != 0) {
		fail("%s: %s and %s wrote different bytes (%s)", what, an, bn,
			first_diff(a->out, b->out, MAX_OUT));
		return;
	}
	if (a->outlen > MAX_OUT - 1) {
		fail("%s: %s reported *outlen %d, larger than the buffer", what,
			an, a->outlen);
	}
}

// `rr`, when not NULL, receives the three decodes so the caller can make its
// own assertion about them -- the round trip does that, and running the
// decode a second time just to look at the result would double the fork
// count for nothing.
static void decode_case(const char *what, const unsigned char *pkt, int pktlen, int maxlen,
		struct result *rr)
{
	struct result r[IMPL_COUNT];
	int i;

	for (i = 0; i < IMPL_COUNT; i++) {
		guard_run(i, pkt, pktlen, maxlen, &r[i]);
		if (r[i].sig != 0)
			guard_deaths[i]++;
		if (rr)
			rr[i] = r[i];
	}

	checks++;

	if (r[IMPL_RUST].sig != 0) {
		fail("%s: the Rust read past the end of the caller's buffer "
			"(killed by signal %d) with pktlen %d, header 0x%02x, "
			"maxlen %d", what, r[IMPL_RUST].sig, pktlen,
			pktlen > 0 ? pkt[0] : 0, maxlen);
		return;
	}
	if (!r[IMPL_RUST].ok) {
		fail("%s: the Rust decode did not complete", what);
		return;
	}

	if (r[IMPL_HWSV].sig != 0 && r[IMPL_CLIENT].sig != 0) {
		// The bounded case: the C reads past the end of the buffer, which is
		// exactly what the Rust refuses to do.  No in-bounds C answer exists
		// to compare with, so the check is the one described in the header --
		// the Rust stopped, and what it produced is a strict prefix of the
		// C's decode of the same packet with a defined tail.
		struct result tail;

		bounded_cases++;
		tail_run(IMPL_CLIENT, pkt, pktlen, maxlen, &tail);

		checks++;
		if ((size_t)r[IMPL_RUST].outlen > (size_t)tail.outlen) {
			fail("%s: the bounded Rust decode produced %d bytes, more "
				"than the C's %d with a defined tail", what,
				r[IMPL_RUST].outlen, tail.outlen);
		} else if (memcmp(r[IMPL_RUST].out, tail.out, (size_t)r[IMPL_RUST].outlen) != 0) {
			fail("%s: the bounded Rust decode is not a prefix of the C's "
				"decode with a defined tail", what);
		} else if (r[IMPL_RUST].outlen <= maxlen && r[IMPL_RUST].outlen >= tail.outlen) {
			fail("%s: the C read on past the buffer but stopped at the "
				"same place as the bounded Rust decode (%d/%d bytes)",
				what, r[IMPL_RUST].outlen, tail.outlen);
		}
		return;
	}

	if (r[IMPL_HWSV].sig != 0 || r[IMPL_CLIENT].sig != 0) {
		fail("%s: one C variant overran the buffer and the other did not "
			"(hwsv %d, client %d)", what, r[IMPL_HWSV].sig,
			r[IMPL_CLIENT].sig);
		return;
	}
	if (!r[IMPL_HWSV].ok || !r[IMPL_CLIENT].ok) {
		fail("%s: a C variant did not complete the decode", what);
		return;
	}

	// The allocation choice, checked before either C variant is compared with
	// the Rust: hunk-allocated tree and malloc'd tree must be the same bytes.
	cmp_results(what, &r[IMPL_HWSV], impls[IMPL_HWSV].name,
		&r[IMPL_CLIENT], impls[IMPL_CLIENT].name);
	cmp_results(what, &r[IMPL_CLIENT], impls[IMPL_CLIENT].name,
		&r[IMPL_RUST], impls[IMPL_RUST].name);
}

static void encode_case(const char *what, const unsigned char *in, int inlen)
{
	unsigned char out[IMPL_COUNT][MAX_OUT];
	int outlen[IMPL_COUNT];
	int i;

	for (i = 0; i < IMPL_COUNT; i++) {
		memset(out[i], POISON, MAX_OUT);
		outlen[i] = -1;
		impls[i].encode(in, out[i], inlen, &outlen[i]);
	}

	// The two C variants first, then each against the Rust.
	checks++;
	if (outlen[IMPL_HWSV] != outlen[IMPL_CLIENT]
		|| memcmp(out[IMPL_HWSV], out[IMPL_CLIENT], MAX_OUT) != 0) {
		fail("%s: the two C variants encoded differently (%s), so the "
			"one-allocation choice is not safe", what,
			outlen[IMPL_HWSV] != outlen[IMPL_CLIENT]
			? "different lengths" : first_diff(out[IMPL_HWSV],
				out[IMPL_CLIENT], MAX_OUT));
	} else {
		checks++;
		if (outlen[IMPL_CLIENT] != outlen[IMPL_RUST]
			|| memcmp(out[IMPL_CLIENT], out[IMPL_RUST], MAX_OUT) != 0) {
			fail("%s: the C and the Rust encoded differently (%s)",
				what, outlen[IMPL_CLIENT] != outlen[IMPL_RUST]
				? "different lengths" : first_diff(out[IMPL_CLIENT],
					out[IMPL_RUST], MAX_OUT));
		}
	}

	// `*outlen == inlen + 1` is exactly the raw fallback: the compressed
	// length is *outlen >= inlen + 1, and the fallback then sets it to
	// inlen + 1.  Counting it keeps the 0xff path from going untested.
	if (outlen[IMPL_CLIENT] == inlen + 1)
		fallbacks++;
}

/*----------------------------------------------------------------------------
 * the frequency table
 *--------------------------------------------------------------------------*/

static void freq_table(void)
{
	float rust[256];
	unsigned char a[sizeof rust], b[sizeof c_HuffFreq];
	int i, diff = -1;

	for (i = 0; i < 256; i++)
		rust[i] = Huffman_freq((unsigned long)i);

	memcpy(a, rust, sizeof a);
	memcpy(b, c_HuffFreq, sizeof b);

	checks++;
	if (memcmp(a, b, sizeof a) != 0) {
		for (i = 0; i < 256; i++) {
			if (memcmp((unsigned char *)&rust[i],
					(const unsigned char *)&c_HuffFreq[i], 4) != 0) {
				diff = i;
				break;
			}
		}
		fail("frequency table: value %d differs (Rust %a, C %a); "
			"BuildTree compares with <, so one float rewrites the "
			"whole tree", diff, (double)rust[diff],
			(double)c_HuffFreq[diff]);
	} else {
		printf("  frequency table: 256/256 values match bit for bit\n");
	}

	// A table of zeros would compare equal to a table of zeros: fail loudly
	// if the generated array ever collapses.
	checks++;
	{
		int nonzero = 0;
		for (i = 0; i < 256; i++)
			if (c_HuffFreq[i] > 0.0f)
				nonzero++;
		if (nonzero != 256)
			fail("frequency table: only %d of the C's 256 values are "
				"positive, so the corpus has no teeth", nonzero);
	}
}

/*----------------------------------------------------------------------------
 * the lookup table
 *--------------------------------------------------------------------------*/

static void lookup_table(void)
{
	unsigned int bits[256];
	int len[256];
	uint64_t kraft = 0;
	int i, j, structure_ok = 1;

	for (i = 0; i < 256; i++) {
		len[i] = Huffman_lookup_len((unsigned long)i);
		bits[i] = Huffman_lookup_bits((unsigned long)i);
	}

	// A complete prefix code over 256 symbols has Kraft sum exactly 1, and
	// exactly 1 is checkable in integers with 2^32 as the denominator.
	for (i = 0; i < 256; i++) {
		if (len[i] < 1 || len[i] > 32) {
			fail("lookup table: symbol %d has code length %d", i, len[i]);
			return;
		}
		kraft += (uint64_t)1 << (32 - len[i]);
	}
	if (kraft != ((uint64_t)1 << 32)) {
		fail("lookup table: the Kraft sum is not 1 (got %llu in units of "
			"2^-32), so the code is not complete",
			(unsigned long long)kraft);
		return;
	}

	// Prefix-free: no code may be a prefix of another.
	for (i = 0; i < 256 && structure_ok; i++) {
		for (j = 0; j < 256; j++) {
			if (i == j || len[i] > len[j])
				continue;
			if ((bits[j] >> (len[j] - len[i])) == bits[i]) {
				fail("lookup table: symbol %d's code (%d bits) is a "
					"prefix of symbol %d's (%d bits)",
					i, len[i], j, len[j]);
				structure_ok = 0;
				break;
			}
		}
	}

	checks++;
	if (!structure_ok)
		return;

	printf("  lookup table: 256 codes, prefix-free, Kraft sum 1\n");
}

/*----------------------------------------------------------------------------
 * the corpus
 *--------------------------------------------------------------------------*/

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t rng_next(void)
{
	uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static int rng_range(int n)
{
	return (int)(rng_next() % (uint64_t)n);
}

// Encode a payload, then decode the result back.  The decode is checked
// against the original payload as well as across the implementations, so a
// codec that agrees with itself but not with the input fails.
static void round_trip(const char *what, const unsigned char *payload, int len, int maxlen)
{
	unsigned char pkt[MAX_OUT];
	int pktlen;
	struct result r[IMPL_COUNT];

	memset(pkt, POISON, sizeof pkt);
	HuffEncode(payload, pkt, len, &pktlen);
	encode_case(what, payload, len);

	decode_case(what, pkt, pktlen, maxlen, r);

	if (maxlen >= len && r[IMPL_CLIENT].ok) {
		checks++;
		if (r[IMPL_CLIENT].outlen != len
			|| memcmp(r[IMPL_CLIENT].out, payload, (size_t)len) != 0) {
			fail("%s: the round trip did not return the payload "
				"(%d bytes back, %d expected)", what,
				r[IMPL_CLIENT].outlen, len);
		}
	}
}

static void single_byte_values(void)
{
	unsigned char one[1], rep[64];
	int x;

	for (x = 0; x < 256; x++) {
		char what[64];

		one[0] = (unsigned char)x;
		snprintf(what, sizeof what, "single byte 0x%02x", x);
		encode_case(what, one, 1);

		// A one-byte input always takes the raw fallback -- the compressed
		// length is at least 2, which is inlen + 1 -- so this is the 0xff
		// path from both directions.
		{
			unsigned char pkt[MAX_OUT];
			int pktlen;

			memset(pkt, POISON, sizeof pkt);
			HuffEncode(one, pkt, 1, &pktlen);
			checks++;
			if (pktlen != 2 || pkt[0] != 0xff || pkt[1] != x)
				fail("single byte 0x%02x: expected a 2-byte 0xff "
					"fallback, got %d bytes starting 0x%02x",
					x, pktlen, pkt[0]);
			decode_case(what, pkt, pktlen, 1, NULL);
			decode_case(what, pkt, pktlen, 0, NULL);
		}

		// 64 repetitions instead: long enough that compression is used, so
		// this drives symbol x's actual code through both encoders and back
		// through both decoders.
		memset(rep, x, sizeof rep);
		snprintf(what, sizeof what, "64 x 0x%02x", x);
		round_trip(what, rep, (int)sizeof rep, (int)sizeof rep);
	}
}

static void random_buffers(void)
{
	unsigned char payload[MAX_PKT];
	int n;

	for (n = 0; n < 240; n++) {
		char what[64];
		int len = 1 + rng_range(MAX_PKT - 1);
		int i;

		for (i = 0; i < len; i++)
			payload[i] = (unsigned char)(rng_next() >> 24);

		snprintf(what, sizeof what, "random payload #%d (len %d)", n, len);
		encode_case(what, payload, len);

		round_trip(what, payload, len, len);
		round_trip(what, payload, len, len - 1 > 0 ? len - 1 : 0);
		round_trip(what, payload, len, 0);
		round_trip(what, payload, len, MAX_OUT - 1);
	}
}

// Padding bits are outside the code stream: the header byte says how many
// there are, and the decoder stops before them.  Setting them to 1 must not
// change a single decoded byte -- and in the C they are whatever the caller's
// output buffer happened to hold, so this is the one part of the encoding
// that is genuinely unspecified.
// The eight shortest-code symbols, found with the C's own encoder: encoding n
// copies of one byte costs 1 + ceil(n * len / 8) bytes, so the smallest output
// is the shortest code.  Half of the crafted checks below are about the
// padding count and the header byte, and a payload that falls back to 0xff has
// neither -- the padding bits only exist on the compressed path.
static void compressible_payload(unsigned char *buf, int len)
{
	unsigned char in[128], out[MAX_OUT];
	int best[8], bestlen[8];
	int i, j, outlen;

	for (i = 0; i < 8; i++) {
		best[i] = -1;
		bestlen[i] = 1 << 30;
	}
	for (i = 0; i < 256; i++) {
		memset(in, i, sizeof in);
		memset(out, 0, sizeof out);
		c_HuffEncode(in, out, (int)sizeof in, &outlen);
		for (j = 0; j < 8; j++) {
			if (outlen < bestlen[j]) {
				memmove(best + j + 1, best + j, sizeof(int) * (size_t)(7 - j));
				memmove(bestlen + j + 1, bestlen + j, sizeof(int) * (size_t)(7 - j));
				best[j] = i;
				bestlen[j] = outlen;
				break;
			}
		}
	}

	checks++;
	for (i = 0; i < 8; i++) {
		if (best[i] < 0)
			fail("compressible payload: could not rank the codes");
	}

	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)best[i % 8];
}

static void crafted_padding(void)
{
	// Eight distinct symbols repeated: compressible enough that the encoder
	// never falls back to 0xff, which would leave no padding bits to craft.
	unsigned char payload[96];
	unsigned char pkt[MAX_OUT];
	int pktlen, bit, padding;
	unsigned char *last;

	compressible_payload(payload, (int)sizeof payload);

	memset(pkt, 0, sizeof pkt);
	HuffEncode(payload, pkt, (int)sizeof payload, &pktlen);

	checks++;
	if (pkt[0] == 0xff) {
		fail("crafted padding: the payload did not compress, so this "
			"check has nothing to look at");
		return;
	}

	padding = pkt[0];
	decode_case("padding: as encoded", pkt, pktlen, MAX_OUT - 1, NULL);

	last = pkt + pktlen - 1;
	for (bit = 0; bit < padding; bit++)
		last[0] |= (unsigned char)(0x80 >> bit);
	decode_case("padding: all padding bits set", pkt, pktlen, MAX_OUT - 1, NULL);

	for (bit = 0; bit < padding; bit++)
		last[0] &= (unsigned char)~(0x80 >> bit);
	decode_case("padding: all padding bits clear", pkt, pktlen, MAX_OUT - 1, NULL);
}

// Truncating a valid packet is the malformed shape a hostile sender reaches
// most easily: the header still claims the original padding, but the payload
// is short.  Some of these the C survives and some it does not; the guard
// page is what tells the two apart, and decode_case handles both.
static void crafted_truncation(void)
{
	unsigned char payload[128];
	unsigned char pkt[MAX_OUT];
	unsigned char truncated[MAX_OUT];
	int pktlen, cut;

	compressible_payload(payload, (int)sizeof payload);

	memset(pkt, 0, sizeof pkt);
	HuffEncode(payload, pkt, (int)sizeof payload, &pktlen);

	// No fallback, or there is no padding count to truncate against.
	checks++;
	if (pkt[0] == 0xff)
		fail("crafted truncation: the payload did not compress");

	for (cut = 1; cut < pktlen; cut++) {
		char what[64];

		memcpy(truncated, pkt, (size_t)cut);
		snprintf(what, sizeof what, "truncated to %d of %d bytes", cut, pktlen);
		decode_case(what, truncated, cut, MAX_OUT - 1, NULL);

		// The same packet with the padding count lowered, which makes the
		// decoder run to the last real bit and then want more.
		truncated[0] = 0;
		snprintf(what, sizeof what, "truncated to %d bytes, header 0", cut);
		decode_case(what, truncated, cut, MAX_OUT - 1, NULL);
	}
}

// Every header byte over a fixed payload: the padding count is the one field
// a sender controls completely, so this sweeps the whole space of "how many
// bits does the receiver think it has".
static void crafted_headers(void)
{
	unsigned char payload[64];
	unsigned char pkt[MAX_OUT];
	int pktlen, h;

	compressible_payload(payload, (int)sizeof payload);

	memset(pkt, 0, sizeof pkt);
	HuffEncode(payload, pkt, (int)sizeof payload, &pktlen);

	checks++;
	if (pkt[0] == 0xff)
		fail("crafted header sweep: the payload did not compress");

	for (h = 0; h < 256; h++) {
		char what[64];

		pkt[0] = (unsigned char)h;
		snprintf(what, sizeof what, "header 0x%02x over %d bytes", h, pktlen);
		decode_case(what, pkt, pktlen, 64, NULL);
	}
}

// The raw fallback, decoded with maxlen below, at and above the payload size.
static void crafted_raw(void)
{
	unsigned char pkt[MAX_OUT];
	int i, len;

	for (len = 1; len <= 40; len += 13) {
		char what[64];

		pkt[0] = 0xff;
		for (i = 0; i < len; i++)
			pkt[i + 1] = (unsigned char)(0x80 + i);

		snprintf(what, sizeof what, "raw packet of %d bytes", len);
		for (i = 0; i <= len + 2; i += (i < len ? 7 : 1))
			decode_case(what, pkt, len + 1, i, NULL);
		decode_case(what, pkt, len + 1, MAX_OUT - 1, NULL);
	}

	// A lone 0xff: *outlen becomes 0 and nothing is written.
	pkt[0] = 0xff;
	decode_case("raw packet of no bytes", pkt, 1, 64, NULL);
}

// `--inlen` underflow: HuffDecode decrements inlen before it reads anything,
// so 0 and negative both report 0 and touch neither buffer.
static void crafted_underflow(void)
{
	unsigned char pkt[8];
	struct result r[IMPL_COUNT];
	int inlen;

	memset(pkt, 0xff, sizeof pkt);

	for (inlen = 0; inlen >= -2; inlen--) {
		int i;

		for (i = 0; i < IMPL_COUNT; i++) {
			memset(&r[i], 0, sizeof r[i]);
			r[i].outlen = -12345;
			memset(r[i].out, POISON, MAX_OUT);
			impls[i].decode(pkt, r[i].out, inlen, &r[i].outlen, 64);
		}

		checks++;
		if (r[IMPL_CLIENT].outlen != 0)
			fail("inlen %d: the C reported *outlen %d, expected 0",
				inlen, r[IMPL_CLIENT].outlen);
		if (r[IMPL_HWSV].outlen != 0 || r[IMPL_RUST].outlen != 0)
			fail("inlen %d: *outlen is %d (hwsv) and %d (Rust), "
				"expected 0 for both", inlen, r[IMPL_HWSV].outlen,
				r[IMPL_RUST].outlen);

		checks++;
		for (i = 0; i < MAX_OUT; i++) {
			if (r[IMPL_RUST].out[i] != POISON) {
				fail("inlen %d: the Rust wrote out[%d] on the "
					"underflow path", inlen, i);
				break;
			}
		}
	}
}

// maxlen below the decoded length, including 0, and the boundary the C's
// comment names: "out[maxlen - 1] is written already".
static void crafted_maxlen(void)
{
	unsigned char payload[40];
	unsigned char pkt[MAX_OUT];
	int pktlen, maxlen, i;

	for (i = 0; i < (int)sizeof payload; i++)
		payload[i] = (unsigned char)('a' + (i % 26));

	memset(pkt, 0, sizeof pkt);
	HuffEncode(payload, pkt, (int)sizeof payload, &pktlen);

	for (maxlen = 0; maxlen <= (int)sizeof payload + 2; maxlen++) {
		char what[64];

		snprintf(what, sizeof what, "maxlen %d", maxlen);
		decode_case(what, pkt, pktlen, maxlen, NULL);
	}
}

// Seeded fuzz over malformed input: random bytes as packets, so most headers
// are wrong, some packets are far too short for their header, and the C's
// guard page does the classifying.
static void fuzz_malformed(void)
{
	int n;

	for (n = 0; n < 600; n++) {
		unsigned char pkt[MAX_PKT];
		char what[64];
		int len = 1 + rng_range(96);
		int i, maxlen;

		for (i = 0; i < len; i++)
			pkt[i] = (unsigned char)(rng_next() >> 24);

		// Bias the header towards small values as well, so the sweep is not
		// dominated by packets that decode to nothing.
		if ((n & 1) == 0)
			pkt[0] = (unsigned char)rng_range(16);
		if ((n % 7) == 0)
			pkt[0] = 0xff;

		maxlen = (int)(rng_next() % (uint64_t)(len + 4));
		snprintf(what, sizeof what, "fuzz #%d (len %d, header 0x%02x, "
			"maxlen %d)", n, len, pkt[0], maxlen);
		decode_case(what, pkt, len, maxlen, NULL);
	}
}

/*----------------------------------------------------------------------------
 * main
 *--------------------------------------------------------------------------*/

int main(void)
{
	int i;

	printf("huffman differential harness: the C as hwsv compiles it, the C as "
		"the\nintegrated client compiles it, and the Rust module\n");

	setup_guard();

	for (i = 0; i < IMPL_COUNT; i++)
		impls[i].init();

	checks++;
	if (hunk_calls != 1)
		fail("Hunk_AllocName was called %d times by the hwsv variant's "
			"HuffInit, expected exactly 1", hunk_calls);

	// The Rust's HuffInit ran before this point too; its allocator is malloc,
	// so hunk_calls staying at 1 is the check that it did not reach into the
	// hunk behind hwsv's back.

	freq_table();
	lookup_table();
	single_byte_values();
	random_buffers();
	crafted_padding();
	crafted_truncation();
	crafted_headers();
	crafted_raw();
	crafted_underflow();
	crafted_maxlen();
	fuzz_malformed();

	printf("  raw 0xff fallbacks: %d encodes\n", fallbacks);
	printf("  bounded decodes (the C read past the payload it was given, the "
		"Rust did not): %d\n", bounded_cases);
	printf("  guard-page deaths: hwsv %d, client %d, Rust %d\n",
		guard_deaths[IMPL_HWSV], guard_deaths[IMPL_CLIENT],
		guard_deaths[IMPL_RUST]);

	checks++;
	if (fallbacks < 256)
		fail("only %d encodes took the raw 0xff fallback, which is too "
			"few for that path to be covered", fallbacks);
	checks++;
	if (bounded_cases < 50)
		fail("only %d malformed inputs reached the bounded read, so the "
			"case this port exists to bound is barely exercised",
			bounded_cases);

	printf("checked %d expectations, %d failures\n", checks, failures);
	if (failures != 0) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}