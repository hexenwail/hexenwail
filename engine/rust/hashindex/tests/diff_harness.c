/*
 * Differential harness: the Rust port of hashindex against the C original.
 *
 * Both implementations live in this one binary.  The C original is compiled
 * separately with its symbols renamed (c_Hash_*), so the linker does not
 * collide with the Rust staticlib; see run_diff_harness.sh.  Every assertion
 * below is a comparison between the two, not an assertion about either alone.
 *
 * What is compared:
 *   - the raw contents of hash[] and indexChain[] (memcmp, so bit-exact)
 *   - the ORDER of each bucket's collision chain (walked via the header's
 *     Hash_First/Hash_Next), not merely which indices are present
 *   - the -1 sentinel on remove-then-lookup
 *   - the fatal path for a non-power-of-two size, which must abort in both
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/* hashindex.h needs qboolean, which it normally receives via quakedef.h (the
 * include order hashindex.c itself uses).  Pull in the header that actually
 * defines it -- `typedef int qboolean` at common/q_stdinc.h:126 -- rather than
 * re-typedefing here, so this harness cannot drift from the engine's ABI. */
#include "q_stdinc.h"

/* Gives hashindex_t and the four static inline helpers (Hash_First,
 * Hash_Next, Hash_GenerateKeyString, Hash_GenerateKeyInt). */
#include "hashindex.h"

/* The C original, compiled with -DHash_Add=c_Hash_Add etc. */
void c_Hash_Allocate(hashindex_t *hi, int hashSize);
void c_Hash_Free(hashindex_t *hi);
void c_Hash_Add(hashindex_t *hi, int key, int index);
void c_Hash_Remove(hashindex_t *hi, int key, int index);
void c_Hash_Clear(hashindex_t *hi);

/* Exported by the Rust crate so the C side can check the layout it computed
 * against its own offsetof(hashindex_t, ...). */
size_t HashIndexC_sizeof(void);
size_t HashIndexC_offsetof_hash_size(void);
size_t HashIndexC_offsetof_hash(void);
size_t HashIndexC_offsetof_index_chain(void);
size_t HashIndexC_offsetof_hash_mask(void);

/* Both implementations reach the SAME Sys_Error (sys.h:98), which is
 * FUNC_NORETURN.  Providing one definition here is what makes "aborts
 * identically" a meaningful claim rather than two different abort paths. */
void Sys_Error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	abort();
}

/* ---- reporting ---------------------------------------------------------- */

static int failures;
static long checks;

#define FAIL(...) do { failures++; \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

#define REQUIRE(cond) do { checks++; \
	if (!(cond)) FAIL("%s", #cond); } while (0)

/* ---- deterministic RNG --------------------------------------------------
 * xorshift64*, so the key/op stream is reproducible run to run and on any
 * platform.  A differential test that fails intermittently is worse than none.
 */
static uint64_t rng_state;
static void rng_seed(uint64_t s) { rng_state = s ? s : 1; }
static uint32_t rng_next(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (uint32_t)((rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}

/* ---- comparison primitives ---------------------------------------------- */

#define CAP 1024
#define CAP_BYTES ((size_t)CAP * sizeof(int))

static int cmp_arrays(hashindex_t *a, hashindex_t *b, const char *what)
{
	if (a->hashSize != b->hashSize) {
		FAIL("%s: hashSize %d vs %d", what, a->hashSize, b->hashSize);
		return 0;
	}
	if (a->hashMask != b->hashMask) {
		FAIL("%s: hashMask %d vs %d", what, a->hashMask, b->hashMask);
		return 0;
	}
	if (memcmp(a->hash, b->hash, CAP_BYTES) != 0) {
		int i;
		for (i = 0; i < CAP; i++)
			if (a->hash[i] != b->hash[i]) {
				FAIL("%s: hash[%d] %d vs %d", what, i,
						a->hash[i], b->hash[i]);
				break;
			}
		return 0;
	}
	if (memcmp(a->indexChain, b->indexChain, CAP_BYTES) != 0) {
		int i;
		for (i = 0; i < CAP; i++)
			if (a->indexChain[i] != b->indexChain[i]) {
				FAIL("%s: indexChain[%d] %d vs %d", what, i,
						a->indexChain[i], b->indexChain[i]);
				break;
			}
		return 0;
	}
	return 1;
}

/*
 * Walk one bucket's chain and write the indices in visit order.
 * Returns the count, or -1 on a runaway (a cycle or an over-long chain --
 * either way a divergence worth failing on rather than looping forever).
 */
static int walk_chain(hashindex_t *hi, int key, int *out, int max)
{
	int n = 0;
	int i;
	for (i = Hash_First(hi, key); i != -1; i = Hash_Next(hi, i)) {
		if (n >= max)
			return -1;
		out[n++] = i;
	}
	return n;
}

/*
 * Compare the full hash state including per-bucket chain ORDER.  Two tables
 * can hold the same set of indices and still differ in order, which is
 * exactly the bug a set-equality check would miss.
 */
static int cmp_state(hashindex_t *a, hashindex_t *b, const char *what)
{
	int bucket, na, nb;
	int oa[CAP + 1], ob[CAP + 1];
	int ok = 1;

	ok &= cmp_arrays(a, b, what);
	if (!ok)
		return 0;

	for (bucket = 0; bucket < CAP; bucket++) {
		na = walk_chain(a, bucket, oa, CAP + 1);
		nb = walk_chain(b, bucket, ob, CAP + 1);
		checks++;
		if (na != nb) {
			FAIL("%s: bucket %d chain length %d vs %d", what, bucket, na, nb);
			ok = 0;
			break;
		}
		if (na <= 0)
			continue;
		if (memcmp(oa, ob, (size_t)na * sizeof(int)) != 0) {
			int j;
			for (j = 0; j < na; j++)
				if (oa[j] != ob[j]) {
					FAIL("%s: bucket %d position %d: %d vs %d",
							what, bucket, j, oa[j], ob[j]);
					break;
				}
			ok = 0;
			break;
		}
	}
	return ok;
}

/* ---- abort helper -------------------------------------------------------
 * Runs fn(hi) in a forked child and reports whether it died on SIGABRT.
 * fork() rather than a signal handler because the point is that the process
 * does not continue -- and both implementations share one Sys_Error, so the
 * comparison is of the call path, not of the abort itself.
 */
static int aborts(void (*fn)(hashindex_t *), hashindex_t *hi)
{
	pid_t pid;
	int status;

	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(2);
	}
	if (pid == 0) {
		/* Silence the Sys_Error text; we only care about the signal. */
		if (freopen("/dev/null", "w", stderr) == NULL)
			; /* best effort: the abort is what matters, not the message */
		fn(hi);
		/* Reaching here means it did NOT abort. */
		_exit(0);
	}
	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		exit(2);
	}
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

/* ---- targeted cases ----------------------------------------------------- */

static hashindex_t g_hi;

static void do_alloc_bad(hashindex_t *hi) { Hash_Allocate(hi, 5); }      /* not a power of two */
static void do_alloc_bad_c(hashindex_t *hi) { c_Hash_Allocate(hi, 5); }
static void do_alloc_zero(hashindex_t *hi) { Hash_Allocate(hi, 0); }
static void do_alloc_zero_c(hashindex_t *hi) { c_Hash_Allocate(hi, 0); }
static void do_add_uninit(hashindex_t *hi) { memset(hi, 0, sizeof(*hi)); Hash_Add(hi, 1, 1); }
static void do_add_uninit_c(hashindex_t *hi) { memset(hi, 0, sizeof(*hi)); c_Hash_Add(hi, 1, 1); }
/* index must be within [0, hashSize); the second reason Hash_Add can abort */
static void do_add_range(hashindex_t *hi) { Hash_Add(hi, 1, CAP); }
static void do_add_range_c(hashindex_t *hi) { c_Hash_Add(hi, 1, CAP); }

static void targeted_tests(void)
{
	hashindex_t r, c;
	int i;

	/* 1. Layout identity, verified from the C side. */
	REQUIRE(sizeof(hashindex_t) == HashIndexC_sizeof());
	REQUIRE(offsetof(hashindex_t, hashSize) == HashIndexC_offsetof_hash_size());
	REQUIRE(offsetof(hashindex_t, hash) == HashIndexC_offsetof_hash());
	REQUIRE(offsetof(hashindex_t, indexChain) == HashIndexC_offsetof_index_chain());
	REQUIRE(offsetof(hashindex_t, hashMask) == HashIndexC_offsetof_hash_mask());
	printf("layout ok: sizeof=%zu offsets=0/%zu/%zu/%zu\n",
		sizeof(hashindex_t),
		offsetof(hashindex_t, hash),
		offsetof(hashindex_t, indexChain),
		offsetof(hashindex_t, hashMask));

	/* 2. Fresh allocation: array state identical (all -1 via memset). */
	memset(&r, 0, sizeof(r));
	memset(&c, 0, sizeof(c));
	Hash_Allocate(&r, CAP);
	c_Hash_Allocate(&c, CAP);
	REQUIRE(cmp_state(&r, &c, "fresh allocate"));
	for (i = 0; i < CAP; i++)
		REQUIRE(r.hash[i] == -1);
	printf("fresh allocate ok\n");

	/* 3. Same-bucket insertion order must match exactly (head insertion). */
	for (i = 0; i < 40; i++) {
		Hash_Add(&r, 7, i);        /* 7 & 1023 == 7 : all in one bucket */
		c_Hash_Add(&c, 7, i);
	}
	REQUIRE(cmp_state(&r, &c, "same-bucket insert"));
	{
		/* The newest insert is the head, and the chain is strictly LIFO. */
		int chain[64], n = walk_chain(&r, 7, chain, 64);
		REQUIRE(n == 40);
		for (i = 0; i < n; i++)
			REQUIRE(chain[i] == 39 - i);
	}
	printf("same-bucket order ok (40-deep chain, LIFO)\n");

	/* 4. Remove head, remove tail, remove a middle entry. */
	Hash_Remove(&r, 7, 39); c_Hash_Remove(&c, 7, 39);   /* head  */
	REQUIRE(cmp_state(&r, &c, "remove head"));
	Hash_Remove(&r, 7, 0);  c_Hash_Remove(&c, 7, 0);    /* tail  */
	REQUIRE(cmp_state(&r, &c, "remove tail"));
	Hash_Remove(&r, 7, 20); c_Hash_Remove(&c, 7, 20);   /* middle */
	REQUIRE(cmp_state(&r, &c, "remove middle"));

	/* 5. Remove an index that is NOT in the table: both must leave the hash
	 *    untouched and still clear that index's chain slot. */
	Hash_Remove(&r, 7, 500); c_Hash_Remove(&c, 7, 500);
	REQUIRE(cmp_state(&r, &c, "remove absent"));
	REQUIRE(r.indexChain[500] == -1);

	/* 6. Remove-then-lookup returns the -1 sentinel in both. */
	Hash_Remove(&r, 7, 1); c_Hash_Remove(&c, 7, 1);
	REQUIRE(Hash_First(&r, 7) != -1);
	{
		int found = 0, j;
		for (j = Hash_First(&r, 7); j != -1; j = Hash_Next(&r, j))
			if (j == 1) found = 1;
		REQUIRE(found == 0);
	}
	Hash_Remove(&r, 7, 2); c_Hash_Remove(&c, 7, 2);
	REQUIRE(cmp_state(&r, &c, "remove then lookup"));

	/* 7. Hash_Clear clears hash[] only; indexChain[] must be untouched. */
	{
		int chain_before[CAP];
		memcpy(chain_before, r.indexChain, CAP_BYTES);
		Hash_Clear(&r); c_Hash_Clear(&c);
		REQUIRE(cmp_state(&r, &c, "after clear"));
		for (i = 0; i < CAP; i++)
			REQUIRE(r.hash[i] == -1);
		REQUIRE(memcmp(chain_before, r.indexChain, CAP_BYTES) == 0);
	}
	printf("clear ok (hash[] reset, indexChain[] preserved)\n");

	/* 8. Hash_Free nulls the pointers but leaves size/mask alone. */
	Hash_Free(&r); c_Hash_Free(&c);
	REQUIRE(r.hash == NULL && c.hash == NULL);
	REQUIRE(r.indexChain == NULL && c.indexChain == NULL);
	REQUIRE(r.hashSize == c.hashSize);
	REQUIRE(r.hashMask == c.hashMask);
	printf("free ok (pointers nulled, size/mask retained)\n");

	/* 9. The fatal path must abort in both.  Same Sys_Error, so what is being
	 *    compared is that each implementation reaches it for these inputs. */
	{
		struct { const char *what; void (*r)(hashindex_t *); void (*c)(hashindex_t *); }
		cases[] = {
			{ "Hash_Allocate(5) not a power of two", do_alloc_bad,  do_alloc_bad_c  },
			{ "Hash_Allocate(0)",                    do_alloc_zero, do_alloc_zero_c },
			{ "Hash_Add uninitialised",              do_add_uninit, do_add_uninit_c },
			{ "Hash_Add index out of range",         do_add_range,  do_add_range_c  },
		};
		size_t k;
		for (k = 0; k < sizeof(cases)/sizeof(cases[0]); k++) {
			int br = aborts(cases[k].r, &g_hi);
			int bc = aborts(cases[k].c, &g_hi);
			checks++;
			printf("abort parity: %-38s rust=%s c=%s\n",
				cases[k].what, br ? "SIGABRT" : "returned",
				bc ? "SIGABRT" : "returned");
			if (br != bc)
				FAIL("abort parity mismatch for %s", cases[k].what);
			if (!br || !bc)
				FAIL("expected both to abort for %s", cases[k].what);
		}
	}
}

/* ---- randomised differential workload ----------------------------------- */

#define N_OPS     1200000L   /* > 10^6 operations */
#define CMP_EVERY 8192L

static void workload(void)
{
	hashindex_t r, c;
	static int keyof[CAP];
	static int present[CAP];
	long op, adds = 0, rems = 0, looks = 0, cmps = 0;
	int live = 0;

	memset(&r, 0, sizeof(r));
	memset(&c, 0, sizeof(c));
	memset(present, 0, sizeof(present));

	Hash_Allocate(&r, CAP);
	c_Hash_Allocate(&c, CAP);

	for (op = 0; op < N_OPS; op++) {
		uint32_t roll = rng_next() % 100;

		if (roll < 55) {
			/* Insert into a free slot.  Hash_Add's contract is that the
			 * index is not already present with that key, so the harness
			 * respects that rather than testing out-of-contract input. */
			int idx = -1, tries;
			for (tries = 0; tries < 8; tries++) {
				int cand = (int)(rng_next() % CAP);
				if (!present[cand]) { idx = cand; break; }
			}
			if (idx < 0) {
				int j;
				for (j = 0; j < CAP; j++)
					if (!present[j]) { idx = j; break; }
			}
			if (idx >= 0) {
				int key = (int)rng_next();
				keyof[idx] = key;
				present[idx] = 1;
				live++;
				adds++;
				Hash_Add(&r, key, idx);
				c_Hash_Add(&c, key, idx);
			}
		} else if (roll < 85) {
			/* Remove a present slot. */
			int idx = -1, tries;
			if (live > 0) {
				for (tries = 0; tries < 8; tries++) {
					int cand = (int)(rng_next() % CAP);
					if (present[cand]) { idx = cand; break; }
				}
				if (idx < 0) {
					int j;
					for (j = 0; j < CAP; j++)
						if (present[j]) { idx = j; break; }
				}
				if (idx >= 0) {
					present[idx] = 0;
					live--;
					rems++;
					Hash_Remove(&r, keyof[idx], idx);
					c_Hash_Remove(&c, keyof[idx], idx);
				}
			}
		} else {
			/* Look up an arbitrary key and compare the chain order. */
			int key = (int)rng_next();
			int oa[CAP + 1], ob[CAP + 1];
			int na = walk_chain(&r, key, oa, CAP + 1);
			int nb = walk_chain(&c, key, ob, CAP + 1);
			looks++;
			checks++;
			if (na != nb) {
				FAIL("op %ld: chain length %d vs %d for key %d", op, na, nb, key);
			} else if (na > 0 && memcmp(oa, ob, (size_t)na * sizeof(int)) != 0) {
				FAIL("op %ld: chain order differs for key %d", op, key);
			}
		}

		if (op % CMP_EVERY == 0) {
			cmps++;
			if (!cmp_state(&r, &c, "workload"))
				break;
		}
	}

	if (cmp_state(&r, &c, "workload final"))
		cmps++;

	printf("workload: ops=%ld (adds=%ld removes=%ld lookups=%ld) "
		"live=%d full-state comparisons=%ld\n",
		N_OPS, adds, rems, looks, live, cmps);
	printf("final state: %ld bucket chains compared in order\n", (long)CAP);

	Hash_Free(&r);
	Hash_Free(&c);
}

int main(void)
{
	printf("=== hashindex differential harness: Rust staticlib vs C original ===\n");
	printf("sizeof(hashindex_t)=%zu  capacity=%d\n\n", sizeof(hashindex_t), CAP);

	rng_seed(0xC0FFEE123456789ULL);
	targeted_tests();
	printf("\n");
	rng_seed(0xC0FFEE123456789ULL);
	workload();

	printf("\n---------------------------------------------\n");
	printf("checks run: %ld   failures: %d\n", checks, failures);
	printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: PASS");
	return failures ? 1 : 0;
}
