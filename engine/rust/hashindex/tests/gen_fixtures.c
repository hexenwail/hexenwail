// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixture generator for the hashindex replay.  Links the C original
// (engine/h2shared/hashindex.c, unmodified) and prints OPERATION SEQUENCES with
// the observable result of each, so tests/fixtures/hashindex.txt can be audited
// against C at any time:
//
//   bash engine/rust/hashindex/tests/gen_fixtures.sh          # rewrite the fixture
//   bash engine/rust/hashindex/tests/gen_fixtures.sh --check  # fail if it drifted
//
// Deterministic: a fixed-seed xorshift32, no clock, no pid.  Format and
// coverage: tests/fixtures/README.md.
//
// The C original is stateful, so a record is one step of a script: an operation
// (add, rem, clr, free, alloc) or an observation of the table afterwards (state,
// look).  The replay performs the same steps on the Rust port and compares each
// observation.  The observables are exactly what the engine's callers can see:
// the two arrays, the size/mask fields, the NULL-ness of the pointers, and the
// chain order Hash_First/Hash_Next walks.

#include "q_stdinc.h"
#include "hashindex.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

/* The engine's only failure path.  The replay stubs the same symbol, so what is
 * frozen is the formatted message, exactly as the C prints it. */
void Sys_Error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	abort();
}

/* ---- seeded RNG --------------------------------------------------------- */

static uint32_t rng_state = 0x2545f491u;

static uint32_t rng(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rng_state = x;
}

/* ---- the table under test and its observations ------------------------- */

static hashindex_t hi;

/* FNV-1a 64 over the little-endian bytes of each int, so the digest is the same
 * on any host.  Small tables are written out in full instead. */
static uint64_t fnv_ints(const int *a, int n)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	int i, b;
	for (i = 0; i < n; i++)
		for (b = 0; b < 4; b++) {
			h ^= (uint64_t)(((uint32_t)a[i] >> (8 * b)) & 0xff);
			h *= 0x100000001b3ULL;
		}
	return h;
}

static void desc(const int *a, int n)
{
	int i;
	if (a == NULL) {
		printf(" -");
	} else if (n <= 32) {
		putchar(' ');
		for (i = 0; i < n; i++)
			printf(i ? ",%d" : "%d", a[i]);
	} else {
		printf(" fnv:%016llx", (unsigned long long)fnv_ints(a, n));
	}
}

/* state <hashSize> <hashMask> <hash==NULL> <indexChain==NULL> <hash[]> <indexChain[]> */
static void st(void)
{
	printf("state %d %d %d %d", hi.hashSize, hi.hashMask,
			hi.hash == NULL, hi.indexChain == NULL);
	desc(hi.hash, hi.hashSize);
	desc(hi.indexChain, hi.hashSize);
	putchar('\n');
}

/* look <key> <n> <idx>...: the chain Hash_First/Hash_Next walks, bounded at
 * hashSize + 1 entries so a cycle (out-of-contract double add) still ends. */
static void look(int key)
{
	int n = 0, i, ids[70000];
	for (i = Hash_First(&hi, key); i != -1 && n <= hi.hashSize; i = Hash_Next(&hi, i))
		ids[n++] = i;
	printf("look %d %d", key, n);
	for (i = 0; i < n; i++)
		printf(" %d", ids[i]);
	putchar('\n');
}

/* ---- operations (each prints the record, then does it on the C) --------- */

static void op_zero(const char *name)
{
	printf("zero %s\n", name);
	memset(&hi, 0, sizeof hi);
}
static void op_seq(const char *name, int size)
{
	printf("seq %s %d\n", name, size);
	memset(&hi, 0, sizeof hi);
	Hash_Allocate(&hi, size);
}
static void op_alloc(int size) { printf("alloc %d\n", size); Hash_Allocate(&hi, size); }
static void op_add(int key, int idx) { printf("add %d %d\n", key, idx); Hash_Add(&hi, key, idx); }
static void op_rem(int key, int idx) { printf("rem %d %d\n", key, idx); Hash_Remove(&hi, key, idx); }
static void op_clr(void) { printf("clr\n"); Hash_Clear(&hi); }
static void op_free(void) { printf("free\n"); Hash_Free(&hi); }

/* operation followed by the full state, for the small directed scripts */
#define ADD(k, i) do { op_add((k), (i)); st(); } while (0)
#define REM(k, i) do { op_rem((k), (i)); st(); } while (0)
#define CLR()     do { op_clr(); st(); } while (0)
#define FREE()    do { op_free(); st(); } while (0)

/* ---- fatal paths: forked, the message is frozen -------------------------- */

typedef struct { const char *setup; const char *op; int a, b; } fatal_t;

static void run_fatal(const fatal_t *f)
{
	int n;
	memset(&hi, 0, sizeof hi);
	if (sscanf(f->setup, "alloc:%d", &n) == 1) {
		Hash_Allocate(&hi, n);
	} else if (sscanf(f->setup, "freed:%d", &n) == 1) {
		Hash_Allocate(&hi, n);
		Hash_Free(&hi);
	} else if (strcmp(f->setup, "zero") != 0) {
		_exit(97);
	}
	if (strcmp(f->op, "alloc") == 0)
		Hash_Allocate(&hi, f->a);
	else if (strcmp(f->op, "add") == 0)
		Hash_Add(&hi, f->a, f->b);
	else if (strcmp(f->op, "rem") == 0)
		Hash_Remove(&hi, f->a, f->b);
	else
		_exit(98);
	_exit(0); /* did not abort */
}

static void emit_fatal(const fatal_t *f)
{
	int pfd[2], status;
	char msg[512];
	size_t total = 0;
	ssize_t n;
	pid_t pid;

	fflush(NULL);
	if (pipe(pfd) != 0) { perror("pipe"); exit(2); }
	pid = fork();
	if (pid < 0) { perror("fork"); exit(2); }
	if (pid == 0) {
		close(pfd[0]);
		if (dup2(pfd[1], STDERR_FILENO) < 0) _exit(3);
		close(pfd[1]);
		run_fatal(f);
	}
	close(pfd[1]);
	while ((n = read(pfd[0], msg + total, sizeof msg - 1 - total)) > 0)
		total += (size_t)n;
	close(pfd[0]);
	msg[total] = '\0';
	while (total > 0 && (msg[total - 1] == '\n' || msg[total - 1] == '\r'))
		msg[--total] = '\0';
	if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); exit(2); }
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT || total == 0) {
		fprintf(stderr, "gen_fixtures: fatal case %s %s %d %d did not abort with a message (status %d)\n",
				f->setup, f->op, f->a, f->b, status);
		exit(1);
	}
	printf("fatal %s %s %d %d %s\n", f->setup, f->op, f->a, f->b, msg);
}

/* ---- directed scripts ---------------------------------------------------- */

static void layout(void)
{
	printf("layout %zu %zu %zu %zu %zu\n", sizeof(hashindex_t),
			offsetof(hashindex_t, hashSize), offsetof(hashindex_t, hash),
			offsetof(hashindex_t, indexChain), offsetof(hashindex_t, hashMask));
}

static void script_empty(void)
{
	int k;
	op_zero("zero-struct");
	st();
	CLR();                  /* Hash_Clear on an unallocated table returns */
	FREE();                 /* so does Hash_Free */
	for (k = 0; k < 8; k = k * 2 + 1) {
		int sz = 1 << k;
		char nm[32];
		snprintf(nm, sizeof nm, "empty-%d", sz);
		op_seq(nm, sz);
		st();
		look(0); look(1); look(-1); look(sz); look(0x7fffffff);
		CLR();              /* clear of an empty table */
		look(5);
		FREE();
		FREE();             /* double free is a no-op; size and mask stay */
		CLR();              /* clear after free returns */
	}
}

static void script_size1(void)
{
	/* hashSize 1: mask 0, every key is the one bucket, only index 0 exists */
	op_seq("size1", 1);
	st();
	ADD(0, 0);
	look(12345); look(-7);
	REM(99, 0);
	look(0);
	ADD(-1, 0);
	CLR();
	look(0);
	REM(0, 0);              /* remove after clear: absent, still defined */
	ADD(0x7fffffff, 0);
	REM(-2147483647 - 1, 0);
	st();
}

static void script_chains(void)
{
	int i;
	/* One bucket, LIFO insertion, then remove head / middle / tail */
	op_seq("lifo-head-middle-tail", 8);
	for (i = 0; i < 8; i++)
		ADD(3, i);
	look(3);
	REM(3, 7);              /* head */
	look(3);
	REM(3, 0);              /* tail */
	REM(3, 4);              /* middle */
	look(3); look(11);      /* 11 & 7 == 3: aliases the same bucket */
	REM(3, 5); REM(3, 6); REM(3, 3); REM(3, 2); REM(3, 1);
	look(3);                /* emptied */
	for (i = 0; i < 8; i++)
		ADD(3 + 8 * i, i);  /* refill through aliasing keys */
	look(3);

	/* Several buckets interleaved */
	op_seq("interleaved", 16);
	for (i = 0; i < 16; i++)
		ADD(i * 5, i);
	for (i = 0; i < 16; i++)
		look(i);
	REM(0, 0); REM(35, 7); REM(75, 15);
	for (i = 0; i < 16; i++)
		look(i);
	CLR();
	for (i = 0; i < 16; i++)
		look(i);

	/* Remove an index that is not in the table; remove twice */
	op_seq("remove-absent", 8);
	ADD(1, 0); ADD(1, 1); ADD(1, 2);
	REM(1, 5);              /* never added: chain[5] is set to -1, nothing else moves */
	REM(1, 1);
	REM(1, 1);              /* again: now absent */
	REM(2, 3);              /* other bucket, empty */
	look(1);

	/* Clear leaves indexChain stale; operations after it see the stale slots */
	op_seq("clear-stale-chain", 8);
	ADD(0, 0); ADD(0, 1); ADD(0, 2); ADD(5, 3); ADD(5, 4);
	CLR();
	look(0); look(5);
	REM(0, 1);              /* stale index removed after clear */
	REM(5, 4);
	ADD(0, 2);              /* re-add a stale index: chain[2] overwritten */
	look(0);
	ADD(5, 4); ADD(5, 3);
	look(5);
	CLR(); CLR();

	/* Removing with the wrong key walks the wrong bucket (defined, lossy) */
	op_seq("remove-wrong-key", 8);
	ADD(1, 0); ADD(1, 1); ADD(1, 2); ADD(2, 3);
	REM(2, 1);              /* index 1 lives in bucket 1, key says bucket 2 */
	look(1); look(2);
	REM(1, 0);
	look(1);
}

static void script_keys(void)
{
	static const int keys[] = {
		0, 1, -1, 2, -2, 7, 8, -8, 255, 256, -256, 65535, 65536, -65536,
		0x7fffffff, -2147483647 - 1, 0x55555555, (int)0xaaaaaaaa,
	};
	int i, nk = (int)(sizeof keys / sizeof keys[0]);

	op_seq("extreme-keys-32", 32);
	for (i = 0; i < nk; i++)
		ADD(keys[i], i);
	for (i = 0; i < nk; i++)
		look(keys[i]);
	for (i = 0; i < nk; i += 2)
		REM(keys[i], i);
	for (i = 0; i < nk; i++)
		look(keys[i]);

	/* The same key under distinct indices: one chain in insertion order */
	op_seq("duplicate-keys", 8);
	for (i = 0; i < 6; i++)
		ADD(42, i);
	look(42);
	REM(42, 3); REM(42, 5); REM(42, 0);
	look(42);

	/* The same index added twice with the same key makes a one-element cycle;
	 * lookups are bounded.  Out of contract, still defined. */
	op_seq("double-add-same-index", 4);
	ADD(1, 2);
	ADD(1, 2);
	look(1);
	op_seq("double-add-in-chain", 8);
	ADD(1, 0); ADD(1, 1); ADD(1, 1);
	look(1);
	REM(1, 1);              /* removing the looped head: hash[] takes chain[1], which is 1 */
	look(1);
	op_seq("double-add-other-bucket", 8);
	ADD(1, 2);
	ADD(2, 2);
	look(1); look(2);
}

/* Keys the engine really builds: Hash_GenerateKeyString over pak entry names,
 * case-sensitive and not.  The generator calls the real header inline. */
static void script_strings(void)
{
	static const char *names[] = {
		"progs.dat", "PROGS.DAT", "gfx/menu/title0.lmp", "GFX/MENU/TITLE0.LMP",
		"maps/demo1.bsp", "maps/demo2.bsp", "maps/demo3.bsp", "models/axe.mdl",
		"models/ball.mdl", "sound/weapons/axeswing.wav", "sound/weapons/axeswing.WAV",
		"strings.txt", "default.cfg", "autoexec.cfg", "pics/conchars.lmp", "",
		"a", "A", "ab", "ba",
	};
	int i, nn = (int)(sizeof names / sizeof names[0]), cs;

	for (cs = 0; cs < 2; cs++) {
		op_seq(cs ? "names-case-sensitive-32" : "names-case-insensitive-32", 32);
		for (i = 0; i < nn; i++) {
			int key = Hash_GenerateKeyString(&hi, names[i], cs);
			printf("# \"%s\"\n", names[i]);
			ADD(key, i);
		}
		for (i = 0; i < nn; i++)
			look(Hash_GenerateKeyString(&hi, names[i], cs));
		for (i = 1; i < nn; i += 3) {
			REM(Hash_GenerateKeyString(&hi, names[i], cs), i);
			look(Hash_GenerateKeyString(&hi, names[i], cs));
		}
	}

	/* Integer keys through Hash_GenerateKeyInt, in a power-of-two table */
	op_seq("int-keys-32", 32);
	for (i = 0; i < 40 && i < 32; i++)
		ADD(Hash_GenerateKeyInt(&hi, i * 37 - 100), i);
	for (i = 0; i < 32; i++)
		look(Hash_GenerateKeyInt(&hi, i));
}

/* Growth: the engine never rehashes in place; a caller frees and allocates a
 * bigger table and re-adds every entry.  Same entries, doubling sizes. */
static void script_growth(void)
{
	static const int keys[20] = {
		3, 11, 19, 27, 35, 43, 51, 59, 67, 75, 100, -5, -13, 4096, 4097, 65, 66, 129, 130, 131,
	};
	int sz, i;

	op_seq("growth-8-to-128", 8);
	for (i = 0; i < 8; i++)
		ADD(keys[i], i);
	for (sz = 16; sz <= 128; sz *= 2) {
		FREE();             /* mask and size are stale here until the next alloc */
		op_alloc(sz);
		st();
		for (i = 0; i < 20 && i < sz; i++)
			ADD(keys[i], i);
		for (i = 0; i < 20 && i < sz; i++)
			look(keys[i]);
	}
	FREE();
	op_alloc(2);            /* shrink again on the same struct */
	ADD(keys[0], 0); ADD(keys[1], 1);
	look(keys[0]);
	CLR();
	FREE();

	/* A large table: the size feeds malloc and memset, and the state is a digest */
	{
		int lk[40];
		op_seq("large-65536", 65536);
		st();
		for (i = 0; i < 40; i++) {
			lk[i] = (int)(rng() & 0xffff0000u) | ((i * 1637) & 0xffff);
			op_add(lk[i], (i * 1637) & 0xffff);
		}
		st();
		for (i = 0; i < 40; i += 2)
			op_rem(lk[i], (i * 1637) & 0xffff);
		st();
		for (i = 0; i < 40; i++)
			look(lk[i]);
		FREE();
	}
}

/* ---- seeded long sequences ------------------------------------------------ */

static int pick_key(int kind, int size)
{
	switch (kind) {
	case 0:  return (int)rng();                                  /* any 32-bit key */
	case 1:  return (int)(rng() % (uint32_t)(size * 2));         /* small pool: heavy collisions */
	case 2:  return (int)(rng() % 4u) + size * (int)(rng() % 9u) - size * 4; /* aliases of a few buckets */
	default: {
		static const int ex[] = { 0, -1, 1, 0x7fffffff, -2147483647 - 1, 0x40000000, -2, 255, 256, -256 };
		return ex[rng() % (sizeof ex / sizeof ex[0])];
	}
	}
}

static void seeded(const char *name, int size, uint32_t seed, int nops, int kind)
{
	static int present[4096], keyof[4096];
	int op, i, live = 0;

	rng_state = seed;
	op_seq(name, size);
	memset(present, 0, sizeof present);
	for (op = 0; op < nops; op++) {
		uint32_t r = rng() % 100;
		if (r < 48) {
			int idx = -1;
			for (i = 0; i < 6; i++) {
				int c = (int)(rng() % (uint32_t)size);
				if (!present[c]) { idx = c; break; }
			}
			for (i = 0; idx < 0 && i < size; i++)
				if (!present[i]) idx = i;
			if (idx >= 0) {
				keyof[idx] = pick_key(kind, size);
				present[idx] = 1;
				live++;
				op_add(keyof[idx], idx);
			}
		} else if (r < 72) {
			int idx = -1;
			for (i = 0; i < 6 && live; i++) {
				int c = (int)(rng() % (uint32_t)size);
				if (present[c]) { idx = c; break; }
			}
			for (i = 0; idx < 0 && live && i < size; i++)
				if (present[i]) idx = i;
			if (idx >= 0) {
				present[idx] = 0;
				live--;
				op_rem(keyof[idx], idx);
			}
		} else if (r < 77) {
			/* remove an absent index (stale or never used) */
			int idx = -1;
			for (i = 0; i < 6; i++) {
				int c = (int)(rng() % (uint32_t)size);
				if (!present[c]) { idx = c; break; }
			}
			if (idx >= 0)
				op_rem(pick_key(kind, size), idx);
		} else if (r < 92) {
			int key = (live && (rng() & 1)) ? keyof[rng() % (uint32_t)size] : pick_key(kind, size);
			look(key);
		} else if (r < 95) {
			op_clr();
			memset(present, 0, sizeof present);
			live = 0;
		} else if (r < 97) {
			op_free();
			op_alloc(size);
			memset(present, 0, sizeof present);
			live = 0;
		} else {
			st();
		}
		if (op % 16 == 15)
			st();
	}
	st();
}

int main(void)
{
	static const fatal_t fatals[] = {
		{ "zero",      "alloc", 5, 0 },            /* not a power of two */
		{ "zero",      "alloc", 0, 0 },
		{ "zero",      "alloc", -4, 0 },
		{ "zero",      "alloc", 3, 0 },
		{ "zero",      "alloc", 6, 0 },
		{ "zero",      "alloc", 12, 0 },
		{ "zero",      "alloc", 1023, 0 },
		{ "zero",      "alloc", -2147483647 - 1, 0 },
		{ "alloc:8",   "alloc", 8, 0 },            /* already initialised */
		{ "alloc:1",   "alloc", 16, 0 },
		{ "alloc:8",   "alloc", 5, 0 },            /* both wrong: size is checked first */
		{ "zero",      "add", 1, 1 },              /* uninitialised */
		{ "freed:8",   "add", 1, 1 },
		{ "alloc:8",   "add", 1, 8 },              /* index == hashSize */
		{ "alloc:8",   "add", 1, 9 },
		{ "alloc:8",   "add", 1, -1 },
		{ "alloc:8",   "add", 1, 2147483647 },
		{ "alloc:8",   "add", 1, -2147483647 - 1 },
		{ "alloc:1",   "add", 1, 1 },
		{ "zero",      "add", 1, -1 },             /* uninitialised wins over range */
		{ "zero",      "rem", 1, 1 },
		{ "freed:8",   "rem", 1, 1 },
		{ "freed:4",   "rem", -1, 0 },
	};
	size_t i;

	printf("# Frozen from engine/h2shared/hashindex.c by tests/gen_fixtures.sh.\n");
	printf("# Format: tests/fixtures/README.md.  Do not edit by hand.\n");
	layout();

	script_empty();
	script_size1();
	script_chains();
	script_keys();
	script_strings();
	script_growth();

	seeded("seeded-2-small-pool",    2,   0x1001u, 60,  1);
	seeded("seeded-4-any-key",       4,   0x1002u, 80,  0);
	seeded("seeded-8-small-pool",    8,   0x1003u, 120, 1);
	seeded("seeded-8-aliases",       8,   0x1004u, 120, 2);
	seeded("seeded-16-extremes",     16,  0x1005u, 120, 3);
	seeded("seeded-16-aliases",      16,  0x1006u, 160, 2);
	seeded("seeded-32-small-pool",   32,  0x1007u, 160, 1);
	seeded("seeded-64-any-key",      64,  0x1008u, 200, 0);
	seeded("seeded-64-aliases",      64,  0x1009u, 200, 2);
	seeded("seeded-256-small-pool",  256, 0x100au, 300, 1);
	seeded("seeded-256-any-key",     256, 0x100bu, 300, 0);
	seeded("seeded-1024-small-pool", 1024, 0x100cu, 400, 1);

	for (i = 0; i < sizeof fatals / sizeof fatals[0]; i++)
		emit_fatal(&fatals[i]);
	return 0;
}
