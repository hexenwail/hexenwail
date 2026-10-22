// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fixture generator for the huffman replay.  Links the C original
// (engine/hexenworld/shared/huffman.c, unmodified) and prints every expected
// value from it, so tests/fixtures/huffman.txt can be audited against C at any
// time:
//
//   bash engine/rust/huffman/tests/gen_fixtures.sh          # rewrite
//   bash engine/rust/huffman/tests/gen_fixtures.sh --check  # fail if drifted
//
// Deterministic: a fixed-seed splitmix64 (the one the retired differential
// harness used), no clock, no pid.  Format: see tests/fixtures/README.md.
//
// The C is built twice, as the retired harness did.  The client variant
// (-DH2W_INTEGRATED, malloc tree) is #included below, so its statics
// (HuffLookup) can be dumped; it is the allocation path the Rust port uses for
// both targets.  The hwsv variant (-DH2W -DSERVERONLY, Hunk_AllocName, stubbed
// here) is linked as w_HuffInit/w_HuffEncode/w_HuffDecode.  Every case is run
// through both and the generator aborts if they ever differ in bytes or in
// whether they overran their buffer: the "one allocation path is safe" finding
// is therefore re-proved every time the fixture is audited.
//
// HuffDecode on malformed input.  The C's GetBit has no bound, so a crafted
// header byte makes it read past the declared packet.  Each decode therefore
// runs in a forked child with the packet's last byte against an unmapped page:
//
//   * the child survives -> `dec`: the C's answer is exact, and the Rust must
//     match it byte for byte;
//   * the child dies     -> `ovr`: the C read past its buffer, which the Rust
//     deliberately does not.  The C is then run again on the same packet with
//     a 64-byte zero tail (a defined read), and THAT result is recorded.  The
//     Rust's answer must be a strict prefix of it.  See replay.rs.

#include "q_stdinc.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// The client variant of the C original, in this translation unit.
#include "huffman.c"

extern void w_HuffInit(void);
extern void w_HuffEncode(const unsigned char *in, unsigned char *out, int inlen, int *outlen);
extern void w_HuffDecode(const unsigned char *in, unsigned char *out, int inlen, int *outlen, const int maxlen);

void Sys_Error(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "gen_fixtures: Sys_Error: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	abort();
}

static int hunk_calls;

// The hwsv variant's allocator.  0xA5 rather than zero: the hunk path does not
// memset the tree, and a recognisable fill shows if the build ever read it.
void *Hunk_AllocName(int size, const char *name)
{
	void *p = malloc((size_t)size);

	(void)name;
	if (!p)
		Sys_Error("Hunk_AllocName(%d) failed", size);
	memset(p, 0xA5, (size_t)size);
	hunk_calls++;
	return p;
}

#define MAX_OUT 2048
#define MAX_PKT 512
#define TAIL 64
#define POISON 0xA5

static void die(const char *what)
{
	fprintf(stderr, "gen_fixtures: %s\n", what);
	exit(1);
}

/*----------------------------------------------------------------------------
 * output
 *--------------------------------------------------------------------------*/

static void hex(const unsigned char *p, int n)
{
	int i;

	if (n <= 0) {
		fputs(" -", stdout);
		return;
	}
	putchar(' ');
	for (i = 0; i < n; i++)
		printf("%02x", p[i]);
}

static int is_rep(const unsigned char *p, int n)
{
	int i;

	for (i = 1; i < n; i++)
		if (p[i] != p[0])
			return 0;
	return n >= 16;
}

// A run of one byte is written rep:XX:N so the 64-repetition payloads stay
// readable and small.
static void payload(const unsigned char *p, int n)
{
	if (is_rep(p, n))
		printf(" rep:%02x:%d", p[0], n);
	else
		hex(p, n);
}

/*----------------------------------------------------------------------------
 * seeded RNG
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

/*----------------------------------------------------------------------------
 * guarded decode
 *--------------------------------------------------------------------------*/

typedef void (*decode_fn)(const unsigned char *, unsigned char *, int, int *, const int);

struct result {
	int survived;			// reached the end of the call
	int outlen;
	unsigned char out[MAX_OUT];
};

static struct result *shared;
static unsigned char *guard_region;
static size_t page_size;

static void setup_guard(void)
{
	long ps = sysconf(_SC_PAGESIZE);

	if (ps <= 0)
		die("cannot read the page size");
	page_size = (size_t)ps;
	guard_region = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (guard_region == MAP_FAILED)
		die("mmap");
	if (mprotect(guard_region + page_size, page_size, PROT_NONE) != 0)
		die("mprotect");
	shared = mmap(NULL, sizeof *shared, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED)
		die("mmap(shared)");
}

static void guard_run(decode_fn fn, const unsigned char *pkt, int pktlen, int maxlen,
		struct result *r)
{
	pid_t pid;
	int status;
	unsigned char *slot = guard_region + page_size - (size_t)pktlen;

	memset(shared, 0, sizeof *shared);
	shared->outlen = -1;
	memset(shared->out, POISON, sizeof shared->out);
	if (pktlen > 0)
		memcpy(slot, pkt, (size_t)pktlen);

	fflush(NULL);
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		alarm(20);
		fn(pktlen > 0 ? slot : (const unsigned char *)"", shared->out, pktlen,
			&shared->outlen, maxlen);
		shared->survived = 1;
		_exit(0);
	}
	if (waitpid(pid, &status, 0) < 0)
		die("waitpid");
	*r = *shared;
	if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM)
		die("the C decoder did not finish (a tree walk that never reaches a leaf)");
	if (WIFSIGNALED(status) || WEXITSTATUS(status) != 0)
		r->survived = 0;
}

static void tail_run(const unsigned char *pkt, int pktlen, int maxlen, struct result *r)
{
	static unsigned char buf[MAX_PKT + TAIL];

	memcpy(buf, pkt, (size_t)pktlen);
	memset(buf + pktlen, 0, TAIL);
	memset(r->out, POISON, sizeof r->out);
	r->outlen = -1;
	HuffDecode(buf, r->out, pktlen, &r->outlen, maxlen);
	r->survived = 1;
}

// min(outlen, maxlen): the number of bytes HuffDecode actually stored.
static int written(const struct result *r, int maxlen)
{
	int n = r->outlen < maxlen ? r->outlen : maxlen;

	return n < 0 ? 0 : n;
}

// Everything the decoder did not store must still be the poison fill.
static void check_poison(const struct result *r, int maxlen, const char *what)
{
	int i;

	for (i = written(r, maxlen); i < MAX_OUT; i++)
		if (r->out[i] != POISON) {
			fprintf(stderr, "gen_fixtures: %s wrote out[%d] beyond what it reports\n", what, i);
			exit(1);
		}
}

static int n_dec, n_ovr;

// One decode through both C variants, one record out.
static void decode_case(const unsigned char *pkt, int pktlen, int maxlen)
{
	struct result rc, rw;

	guard_run(HuffDecode, pkt, pktlen, maxlen, &rc);
	guard_run(w_HuffDecode, pkt, pktlen, maxlen, &rw);

	if (rc.survived != rw.survived)
		die("the two C variants disagree about overrunning their buffer");

	if (!rc.survived) {
		struct result t;

		tail_run(pkt, pktlen, maxlen, &t);
		check_poison(&t, maxlen, "tail run");
		printf("ovr %d", maxlen);
		hex(pkt, pktlen);
		printf(" %d", t.outlen);
		hex(t.out, written(&t, maxlen));
		putchar('\n');
		n_ovr++;
		return;
	}

	if (rc.outlen != rw.outlen || memcmp(rc.out, rw.out, MAX_OUT) != 0)
		die("the two C variants decoded differently");
	check_poison(&rc, maxlen, "decode");

	printf("dec %d", maxlen);
	hex(pkt, pktlen);
	printf(" %d", rc.outlen);
	hex(rc.out, written(&rc, maxlen));
	putchar('\n');
	n_dec++;
}

/*----------------------------------------------------------------------------
 * encode
 *--------------------------------------------------------------------------*/

static unsigned char last_payload[MAX_PKT];
static int last_len;
static unsigned char last_pkt[MAX_OUT];
static int last_pktlen;

// Encode through both variants, require identical bytes, print the record.
// Bytes past outlen are not recorded: when the compressed attempt loses to the
// raw fallback it leaves scratch bits there, and no caller reads them.
static void encode_case(const unsigned char *in, int inlen)
{
	unsigned char a[MAX_OUT], b[MAX_OUT];
	int alen = -1, blen = -1;

	memset(a, POISON, sizeof a);
	memset(b, POISON, sizeof b);
	HuffEncode(in, a, inlen, &alen);
	w_HuffEncode(in, b, inlen, &blen);
	if (alen != blen || memcmp(a, b, MAX_OUT) != 0)
		die("the two C variants encoded differently");

	printf("enc");
	payload(in, inlen);
	printf(" %d", alen);
	// The raw fallback is the 0xff marker followed by the payload verbatim;
	// write it as `raw` instead of repeating the payload.
	if (alen == inlen + 1 && a[0] == 0xff && memcmp(a + 1, in, (size_t)inlen) == 0)
		fputs(" raw", stdout);
	else
		hex(a, alen);
	putchar('\n');

	memcpy(last_payload, in, (size_t)inlen);
	last_len = inlen;
	memcpy(last_pkt, a, (size_t)alen);
	last_pktlen = alen;
}

// Decode the packet of the preceding enc record: `decl <maxlen> <outlen>`.
// The decoded bytes are the payload's first min(outlen, maxlen) bytes; the
// generator checks that of the C, and replay.rs checks it of the Rust.
static int n_decl;

static void decode_last(int maxlen)
{
	struct result rc, rw;
	int n;

	guard_run(HuffDecode, last_pkt, last_pktlen, maxlen, &rc);
	guard_run(w_HuffDecode, last_pkt, last_pktlen, maxlen, &rw);
	if (!rc.survived || !rw.survived)
		die("a round-trip decode overran its buffer");
	if (rc.outlen != rw.outlen || memcmp(rc.out, rw.out, MAX_OUT) != 0)
		die("the two C variants decoded differently");
	check_poison(&rc, maxlen, "round-trip decode");

	n = written(&rc, maxlen);
	if (memcmp(rc.out, last_payload, (size_t)n) != 0)
		die("a round trip did not return the payload prefix");
	if (maxlen >= last_len && rc.outlen != last_len)
		die("a round trip returned the wrong length");

	printf("decl %d %d\n", maxlen, rc.outlen);
	n_decl++;
}

static void round_trip(const unsigned char *in, int inlen)
{
	encode_case(in, inlen);
	decode_last(inlen);
	if (inlen > 1)
		decode_last(inlen - 1);
	decode_last(0);
}

/*----------------------------------------------------------------------------
 * the corpus
 *--------------------------------------------------------------------------*/

// The eight shortest-code symbols (ties by value), so a payload built from
// them is guaranteed to compress and the packet therefore carries a real
// padding count instead of the 0xff raw marker.
static void compressible_payload(unsigned char *buf, int len)
{
	int best[8], i, j, k;

	for (k = 0; k < 8; k++) {
		int pick = -1;

		for (i = 0; i < 256; i++) {
			int taken = 0;

			for (j = 0; j < k; j++)
				if (best[j] == i)
					taken = 1;
			if (taken)
				continue;
			if (pick < 0 || HuffLookup[i].len < HuffLookup[pick].len)
				pick = i;
		}
		best[k] = pick;
	}
	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)best[i % 8];
}

static void tables(void)
{
	int i;

	puts("# freq <idx> <IEEE-754 bits of the C's HuffFreq[idx]>");
	for (i = 0; i < 256; i++) {
		uint32_t bits;

		memcpy(&bits, &HuffFreq[i], sizeof bits);
		printf("freq %d %08x\n", i, bits);
	}
	puts("# code <symbol> <length> <bits> (the C's HuffLookup after HuffInit)");
	for (i = 0; i < 256; i++)
		printf("code %d %d %x\n", i, HuffLookup[i].len, HuffLookup[i].bits);
}

static void single_and_repeated(void)
{
	unsigned char one[1], rep[64];
	int x;

	puts("# enc <payload> <outlen> <packet>: HuffEncode (output prefilled 0xa5)");
	puts("# decl <maxlen> <outlen>: decode the packet of the preceding enc");
	puts("#   1 byte always takes the raw 0xff fallback; 64 x one byte drives that");
	puts("#   symbol's own code through the encoder");
	for (x = 0; x < 256; x++) {
		one[0] = (unsigned char)x;
		encode_case(one, 1);
		decode_last(1);
		decode_last(0);

		memset(rep, x, sizeof rep);
		round_trip(rep, (int)sizeof rep);
	}
	// The empty payload: compressed length 1 >= inlen + 1, so the raw marker.
	encode_case((const unsigned char *)"", 0);
	decode_last(8);
}

// A symbol drawn with probability proportional to the C's own frequency table,
// so the payload looks like the traffic the table was built from and the
// encoder takes the compressed path instead of the raw fallback.
static unsigned char weighted_symbol(void)
{
	static uint64_t cum[256];
	int i;

	if (cum[255] == 0) {
		uint64_t sum = 0;

		for (i = 0; i < 256; i++) {
			sum += (uint64_t)(HuffFreq[i] * 1000000.0f) + 1;
			cum[i] = sum;
		}
	}
	{
		uint64_t r = rng_next() % cum[255];

		for (i = 0; i < 255 && r >= cum[i]; i++)
			;
	}
	return (unsigned char)i;
}

static void random_buffers(void)
{
	unsigned char buf[MAX_PKT];
	int n, i;

	puts("# seeded payloads (uniform, then weighted by the frequency table): encode,");
	puts("# then decode at maxlen len, len-1, 0");
	for (n = 0; n < 48; n++) {
		int len = 1 + rng_range(n % 3 == 2 ? 300 : 128);

		for (i = 0; i < len; i++)
			buf[i] = n < 12 ? (unsigned char)(rng_next() >> 24) : weighted_symbol();
		round_trip(buf, len);
	}
}

static void crafted_padding(void)
{
	unsigned char in[96], pkt[MAX_OUT];
	int pktlen = -1, bit, pad;

	puts("# padding bits of a valid packet as encoded, all set, all clear");
	compressible_payload(in, (int)sizeof in);
	memset(pkt, 0, sizeof pkt);
	HuffEncode(in, pkt, (int)sizeof in, &pktlen);
	if (pkt[0] == 0xff)
		die("crafted padding: the payload did not compress");
	pad = pkt[0];
	decode_case(pkt, pktlen, MAX_OUT - 1);
	for (bit = 0; bit < pad; bit++)
		pkt[pktlen - 1] |= (unsigned char)(0x80 >> bit);
	decode_case(pkt, pktlen, MAX_OUT - 1);
	for (bit = 0; bit < pad; bit++)
		pkt[pktlen - 1] &= (unsigned char)~(0x80 >> bit);
	decode_case(pkt, pktlen, MAX_OUT - 1);
}

static void crafted_truncation(void)
{
	unsigned char in[80], pkt[MAX_OUT], cut_pkt[MAX_OUT];
	int pktlen = -1, cut;

	puts("# truncated packets: header kept, then header 0");
	compressible_payload(in, (int)sizeof in);
	memset(pkt, 0, sizeof pkt);
	HuffEncode(in, pkt, (int)sizeof in, &pktlen);
	if (pkt[0] == 0xff)
		die("crafted truncation: the payload did not compress");
	for (cut = 1; cut < pktlen; cut++) {
		memcpy(cut_pkt, pkt, (size_t)cut);
		decode_case(cut_pkt, cut, MAX_OUT - 1);
		cut_pkt[0] = 0;
		decode_case(cut_pkt, cut, MAX_OUT - 1);
	}
}

static void crafted_headers(void)
{
	unsigned char in[32], pkt[MAX_OUT];
	int pktlen = -1, h;

	puts("# every header byte over one compressed packet");
	compressible_payload(in, (int)sizeof in);
	memset(pkt, 0, sizeof pkt);
	HuffEncode(in, pkt, (int)sizeof in, &pktlen);
	if (pkt[0] == 0xff)
		die("crafted header sweep: the payload did not compress");
	for (h = 0; h < 256; h++) {
		pkt[0] = (unsigned char)h;
		decode_case(pkt, pktlen, 64);
	}
}

static void crafted_raw(void)
{
	unsigned char pkt[MAX_OUT];
	int i, len;

	puts("# raw 0xff packets, maxlen below, at and above the payload size");
	for (len = 1; len <= 40; len += 13) {
		pkt[0] = 0xff;
		for (i = 0; i < len; i++)
			pkt[i + 1] = (unsigned char)(0x80 + i);
		for (i = 0; i <= len + 2; i += (i < len ? 7 : 1))
			decode_case(pkt, len + 1, i);
		decode_case(pkt, len + 1, MAX_OUT - 1);
	}
	pkt[0] = 0xff;
	decode_case(pkt, 1, 64);
}

static int n_under;

static void crafted_underflow(void)
{
	unsigned char pkt[8], out[MAX_OUT], out_w[MAX_OUT];
	int inlen;

	puts("# under <inlen>: inlen <= 0 reports outlen 0 and writes nothing");
	memset(pkt, 0xff, sizeof pkt);
	for (inlen = 0; inlen >= -2; inlen--) {
		int a = -12345, b = -12345, i;

		memset(out, POISON, sizeof out);
		memset(out_w, POISON, sizeof out_w);
		HuffDecode(pkt, out, inlen, &a, 64);
		w_HuffDecode(pkt, out_w, inlen, &b, 64);
		if (a != 0 || b != 0)
			die("underflow: outlen is not 0");
		for (i = 0; i < MAX_OUT; i++)
			if (out[i] != POISON || out_w[i] != POISON)
				die("underflow: the decoder wrote");
		printf("under %d\n", inlen);
		n_under++;
	}
}

static void crafted_maxlen(void)
{
	unsigned char in[40], pkt[MAX_OUT];
	int pktlen = -1, maxlen, i;

	puts("# maxlen 0..42 over one packet: out[maxlen - 1] is written already");
	for (i = 0; i < (int)sizeof in; i++)
		in[i] = (unsigned char)('a' + (i % 26));
	memset(pkt, 0, sizeof pkt);
	HuffEncode(in, pkt, (int)sizeof in, &pktlen);
	for (maxlen = 0; maxlen <= (int)sizeof in + 2; maxlen++)
		decode_case(pkt, pktlen, maxlen);
}

static void fuzz_malformed(void)
{
	int n;

	puts("# seeded fuzz: random bytes as packets, header biased small or 0xff");
	for (n = 0; n < 160; n++) {
		unsigned char pkt[MAX_PKT];
		int len = 1 + rng_range(40);
		int i, maxlen;

		for (i = 0; i < len; i++)
			pkt[i] = (unsigned char)(rng_next() >> 24);
		if ((n & 1) == 0)
			pkt[0] = (unsigned char)rng_range(16);
		if ((n % 7) == 0)
			pkt[0] = 0xff;
		maxlen = (int)(rng_next() % (uint64_t)(len + 4));
		decode_case(pkt, len, maxlen);
	}
}

int main(void)
{
	setup_guard();
	HuffInit();
	w_HuffInit();
	if (hunk_calls != 1)
		die("the hwsv variant did not call Hunk_AllocName exactly once");

	puts("# Frozen output of engine/hexenworld/shared/huffman.c; see README.md.");
	puts("# Generated by tests/gen_fixtures.sh. Do not edit.");
	tables();
	single_and_repeated();
	random_buffers();
	puts("# dec <maxlen> <packet> <outlen> <bytes stored>: C survived its guard page");
	puts("# ovr <maxlen> <packet> <tail outlen> <tail bytes>: C overran; zero-tail run");
	crafted_padding();
	crafted_truncation();
	crafted_headers();
	crafted_raw();
	crafted_underflow();
	crafted_maxlen();
	fuzz_malformed();

	fprintf(stderr, "gen_fixtures: %d dec, %d ovr, %d decl, %d under\n",
		n_dec, n_ovr, n_decl, n_under);
	return 0;
}
