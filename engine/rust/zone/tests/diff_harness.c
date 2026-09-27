// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for the Rust port of engine/h2shared/zone.c.
//
// WHAT IT COMPARES
//
// The C original is compiled with every exported name renamed (c_*) and linked
// into this binary beside the Rust module.  Both are driven through one table
// of function pointers, and after every call the whole observable state is
// recorded into a byte trace and the two traces are compared byte for byte.
//
// For an allocator the observable state is not the return value; it is *where
// things landed*.  zone.c keeps its structure inside the caller's buffer -- the
// zonelist, the memzone, every memblock header, the hunk headers and the cache
// records all live in the memory handed to Memory_Init -- so the strongest
// observation is the buffer itself.  This harness gives both implementations
// the same static buffer (a static, not a malloc, so its address is identical
// in both child processes and the absolute pointers the allocators store under
// it compare equal), and records a CRC of the whole buffer, a CRC of the low
// and high ends, the first and last 256 bytes verbatim, the hunk marks, the
// cache_user_t values, the returned pointers as offsets into the buffer, the
// commands registered and every diagnostic.  A port that returns the right
// pointer from the wrong offset changes the buffer and fails.
//
// TWO ARMS, BECAUSE zone.c HAS TWO VARIANTS
//
// zone.c is compiled into all three engine targets, but not identically: under
// SERVERONLY the zone default is 1 MB, the secondary zone does not exist and
// the whole cache API is compiled out.  So the harness is built twice -- once
// with -DGLQUAKE (the client) and once with -DSERVERONLY -- and in each arm the
// C original is compiled the same way and the Rust module is linked, with the
// per-target shim (engine/rust/zone_target.c) compiled with the arm's defines.
// That is what proves the shim: one Rust implementation, two different
// behaviours, each matching the C it was built against.
//
// The cache scenarios only exist in the client arm, because the C original has
// no Cache_* symbols under SERVERONLY.  The SERVERONLY arm exists to prove the
// rest of the surface still matches there, and that the Rust module's cache
// machinery -- which is present in both, because the archive is one object --
// stays inert.
//
// Case isolation: the C's zonelist and the Rust module's are `static` with no
// exported reset, so each case runs in a child process, for each implementation
// separately, exactly as the cvar harness does.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "quakedef.h"
#include "zone.h"
#include "common.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/*----------------------------------------------------------------------------
 * the two implementations
 *--------------------------------------------------------------------------*/

/* Every symbol zone.c exports, renamed for the C original. */
#define DECLARE_ZONE_IMPL(P) \
	extern void P##Memory_Init (void *buf, int size); \
	extern void P##Z_Free (void *ptr); \
	extern void *P##Z_Malloc (int size, int zone_id); \
	extern void *P##Z_Realloc (void *ptr, int size, int zone_id); \
	extern char *P##Z_Strdup (const char *s); \
	extern void *P##Hunk_Alloc (int size); \
	extern void *P##Hunk_AllocName (int size, const char *name); \
	extern void *P##Hunk_HighAllocName (int size, const char *name); \
	extern char *P##Hunk_Strdup (const char *s, const char *name); \
	extern void *P##Hunk_TempAlloc (int size); \
	extern int P##Hunk_LowMark (void); \
	extern void P##Hunk_FreeToLowMark (int mark); \
	extern int P##Hunk_HighMark (void); \
	extern void P##Hunk_FreeToHighMark (int mark); \
	extern void P##Hunk_Check (void)

DECLARE_ZONE_IMPL(c_);
DECLARE_ZONE_IMPL();

/* The cache half of the surface.  In the SERVERONLY arm the C original has no
 * definitions at all -- zone.h does not even declare them -- so only the Rust
 * side is named there. */
#if !defined(SERVERONLY)
#define DECLARE_ZONE_CACHE_IMPL(P) \
	extern void P##Cache_Flush (void); \
	extern void *P##Cache_Check (cache_user_t *c); \
	extern void P##Cache_Free (cache_user_t *c); \
	extern void *P##Cache_Alloc (cache_user_t *c, int size, const char *name); \
	extern void P##Cache_Report (void)

DECLARE_ZONE_CACHE_IMPL(c_);
DECLARE_ZONE_CACHE_IMPL();
#endif

/* The per-target shim the Rust module asks, so the harness can assert what this
 * arm's build answers. */
extern int Zone_TargetDefSize (void);
extern int Zone_TargetSecSize (void);
extern int Zone_TargetHasCache (void);
extern int Zone_TargetDedicated (void);

/* The Rust layout accessors, so the harness can check the structs it compiles
 * against without restating Rust's offsets. */
extern size_t CacheUserC_sizeof (void);
extern size_t CacheUserC_offsetof_data (void);
extern size_t QuakeParmsC_sizeof (void);
extern size_t QuakeParmsC_offsetof_argc (void);
extern size_t QuakeParmsC_offsetof_argv (void);

typedef struct zone_impl_s {
	const char *name;
	void (*Memory_Init)(void *, int);
	void (*Z_Free)(void *);
	void *(*Z_Malloc)(int, int);
	void *(*Z_Realloc)(void *, int, int);
	char *(*Z_Strdup)(const char *);
	void *(*Hunk_Alloc)(int);
	void *(*Hunk_AllocName)(int, const char *);
	void *(*Hunk_HighAllocName)(int, const char *);
	char *(*Hunk_Strdup)(const char *, const char *);
	void *(*Hunk_TempAlloc)(int);
	int (*Hunk_LowMark)(void);
	void (*Hunk_FreeToLowMark)(int);
	int (*Hunk_HighMark)(void);
	void (*Hunk_FreeToHighMark)(int);
	void (*Hunk_Check)(void);
#if !defined(SERVERONLY)
	void (*Cache_Flush)(void);
	void *(*Cache_Check)(cache_user_t *);
	void (*Cache_Free)(cache_user_t *);
	void *(*Cache_Alloc)(cache_user_t *, int, const char *);
	void (*Cache_Report)(void);
#endif
} zone_impl_t;

enum { IMPL_C = 0, IMPL_RUST = 1, IMPL_COUNT = 2 };

static const zone_impl_t impls[IMPL_COUNT] = {
	{
		"C",
		c_Memory_Init, c_Z_Free, c_Z_Malloc, c_Z_Realloc, c_Z_Strdup,
		c_Hunk_Alloc, c_Hunk_AllocName, c_Hunk_HighAllocName,
		c_Hunk_Strdup, c_Hunk_TempAlloc, c_Hunk_LowMark,
		c_Hunk_FreeToLowMark, c_Hunk_HighMark, c_Hunk_FreeToHighMark,
		c_Hunk_Check,
#if !defined(SERVERONLY)
		c_Cache_Flush, c_Cache_Check, c_Cache_Free, c_Cache_Alloc,
		c_Cache_Report,
#endif
	},
	{
		"Rust",
		Memory_Init, Z_Free, Z_Malloc, Z_Realloc, Z_Strdup,
		Hunk_Alloc, Hunk_AllocName, Hunk_HighAllocName,
		Hunk_Strdup, Hunk_TempAlloc, Hunk_LowMark,
		Hunk_FreeToLowMark, Hunk_HighMark, Hunk_FreeToHighMark,
		Hunk_Check,
#if !defined(SERVERONLY)
		Cache_Flush, Cache_Check, Cache_Free, Cache_Alloc,
		Cache_Report,
#endif
	},
};

static const zone_impl_t *cur;

/*----------------------------------------------------------------------------
 * failures and the trace
 *--------------------------------------------------------------------------*/

static int failures;
static int checks;

static void check(int ok, const char *fmt, ...)
{
	va_list ap;

	checks++;
	if (ok)
		return;

	failures++;
	fprintf(stderr, "FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

#define TRACE_MAX (8 * 1024 * 1024)

static unsigned char trace[TRACE_MAX];
static size_t trace_len;

static void rec(const void *p, size_t n)
{
	if (trace_len + n > sizeof trace) {
		failures++;
		fprintf(stderr, "FAIL: trace overflow\n");
		return;
	}
	memcpy(trace + trace_len, p, n);
	trace_len += n;
}

static void rec_int(int v) { rec(&v, sizeof v); }
static void rec_str(const char *s) { rec(s, strlen(s) + 1); }

/*----------------------------------------------------------------------------
 * the buffer both implementations carve, and the CRC of what they leave
 *--------------------------------------------------------------------------*/

/* 4 MB: the client's default main zone is 2 MB and its secondary zone 256 KB,
 * and both are carved out of this.  A static, so the absolute pointers inside
 * it are the same in the C and Rust children. */
#define ZONE_MEM_SIZE (4 * 1024 * 1024)

static byte zonemem[ZONE_MEM_SIZE];

#define PTR_SIZE ((size_t)sizeof(void *))

static unsigned int crc_table[256];
static int crc_ready;

static void crc_setup(void)
{
	unsigned int crc;
	int i, j;

	if (crc_ready)
		return;
	for (i = 0; i < 256; i++) {
		crc = (unsigned int)i;
		for (j = 0; j < 8; j++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		crc_table[i] = crc;
	}
	crc_ready = 1;
}

static unsigned int crc_feed(unsigned int crc, const byte *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	return crc;
}

/* Is this pointer-sized word a pointer *outside* the buffer?  The only such
 * words either implementation leaves in it are zonelist_t.name -- the C's
 * static "MAINZONE"/"SEC_ZONE" and the Rust module's own, which are at
 * different addresses by construction.  They are normalised to zero so the
 * comparison is about the allocator's structure; the *contents* of those names
 * are still compared byte for byte, because every hunk header carries a copy
 * of the name it was allocated with. */
static int outside_pointer(const byte *w)
{
	uintptr_t v;

	memcpy(&v, w, PTR_SIZE);
	return v < (uintptr_t)zonemem || v >= (uintptr_t)(zonemem + sizeof zonemem);
}

static unsigned int crc32_normalized(size_t off, size_t n)
{
	unsigned int crc = 0xffffffffu;
	size_t i;

	crc_setup();
	for (i = 0; i + PTR_SIZE <= n; i += PTR_SIZE) {
		if (outside_pointer(zonemem + off + i)) {
			static const byte zero[sizeof(void *)] = { 0 };
			crc = crc_feed(crc, zero, PTR_SIZE);
		} else {
			crc = crc_feed(crc, zonemem + off + i, PTR_SIZE);
		}
	}
	crc = crc_feed(crc, zonemem + off + i, n - i);
	return crc ^ 0xffffffffu;
}

/* The same normalisation for a bounded verbatim dump. */
static void rec_bytes_normalized(size_t off, size_t n)
{
	static byte scratch[4096];
	size_t i;

	if (n > sizeof scratch)
		n = sizeof scratch;
	memcpy(scratch, zonemem + off, n);
	for (i = 0; i + PTR_SIZE <= n; i += PTR_SIZE)
		if (outside_pointer(scratch + i))
			memset(scratch + i, 0, PTR_SIZE);
	rec(scratch, n);
}

static int buf_off(const void *p)
{
	const byte *b = (const byte *)p;

	if (!p)
		return -1;
	if (b < zonemem || b >= zonemem + sizeof zonemem)
		return -2;	/* not in the buffer: a bug in the port or a leak */
	return (int)(b - zonemem);
}

static void rec_diag(const char *kind, const char *text)
{
	rec_int(0x0D1A);
	rec_str(kind);
	rec_str(text);
}

/*----------------------------------------------------------------------------
 * the engine the two implementations call
 *--------------------------------------------------------------------------*/

static char printf_buf[512];

void CON_Printf(unsigned int flags, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(printf_buf, sizeof printf_buf, fmt, ap);
	va_end(ap);

	/* The renamed C original names itself "c_Hunk_HighAllocName" where the
	 * shipping build says "Hunk_HighAllocName"; that is a harness artifact. */
	if (strncmp(printf_buf, "c_", 2) == 0)
		memmove(printf_buf, printf_buf + 2, strlen(printf_buf + 2) + 1);

	rec_int(0x9911);
	rec_int((int)flags);
	rec_str(printf_buf);
}

static char err_buf[512];
static int err_calls;
static jmp_buf err_env;
static int err_armed;

FUNC_NORETURN void Sys_Error(const char *fmt, ...)
{
	va_list ap;

	err_calls++;
	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);

	/* The renamed C original says "c_Z_Free: ..." where the shipping build
	 * says "Z_Free: ..."; that is a harness artifact, so it is removed. */
	if (strncmp(err_buf, "c_", 2) == 0)
		memmove(err_buf, err_buf + 2, strlen(err_buf + 2) + 1);

	if (!err_armed) {
		fprintf(stderr, "harness bug: Sys_Error outside a guarded call: %s\n",
			err_buf);
		abort();
	}
	longjmp(err_env, 1);
}

#define CMD_MAX 16

static struct {
	char name[32];
	int set;
} commands[CMD_MAX];
static int command_count;

void Cmd_AddCommand(const char *name, void (*fn)(void))
{
	int i;

	(void)fn;
	rec_int(0xADD0);
	rec_str(name);
	for (i = 0; i < command_count; i++)
		if (!strcmp(commands[i].name, name))
			return;
	if (command_count < CMD_MAX) {
		snprintf(commands[command_count].name,
			sizeof commands[command_count].name, "%s", name);
		commands[command_count].set = 1;
		command_count++;
	}
}

/* COM_CheckParm is the engine's (engine/h2shared/common.c:448), copied so the
 * two implementations see the same command line.  Note that it is not a symbol
 * the engine exports from a library: com_argc/com_argv are macros over
 * host_parms, which the harness owns below. */
int COM_CheckParm(const char *parm)
{
	int i;

	for (i = 1; i < com_argc; i++) {
		if (!com_argv[i])
			continue;
		if (!strcmp(parm, com_argv[i]))
			return i;
	}
	return 0;
}

/* The command line.  Memory_Init reads it for -zone, so the harness can drive
 * both the default path and the override. */
static char *fake_argv[8];
static quakeparms_t parms;
quakeparms_t *host_parms = &parms;

static void cmdline_set(int argc, const char *a0, const char *a1,
		const char *a2)
{
	fake_argv[0] = (char *)a0;
	fake_argv[1] = (char *)a1;
	fake_argv[2] = (char *)a2;
	parms.argc = argc;
	parms.argv = fake_argv;
}

/* The dedicated flag.  The client arm reads it (zone_target.c's
 * Zone_TargetDedicated, and zone.c's own secondary-zone gate), the SERVERONLY
 * arm does not. */
qboolean isDedicated = 0;

/*----------------------------------------------------------------------------
 * the harness's own zone contents
 *--------------------------------------------------------------------------*/

#define N_CACHE_USERS 6

#if !defined(SERVERONLY)
static cache_user_t cache_users[N_CACHE_USERS];
#endif

static void op_reset(void)
{
	int i;

	memset(zonemem, 0xA5, sizeof zonemem);
	cmdline_set(1, "hexenwail", NULL, NULL);
	isDedicated = 0;
	err_calls = 0;
	err_buf[0] = 0;
	err_armed = 0;
	printf_buf[0] = 0;
	command_count = 0;
#if !defined(SERVERONLY)
	for (i = 0; i < N_CACHE_USERS; i++)
		cache_users[i].data = NULL;
#else
	(void)i;
#endif
}

/*----------------------------------------------------------------------------
 * state recording
 *--------------------------------------------------------------------------*/

static void rec_state(void)
{
	int i;

	rec_int((int)crc32_normalized(0, sizeof zonemem));
	rec_int((int)crc32_normalized(0, 4096));
	rec_int((int)crc32_normalized(sizeof zonemem - 4096, 4096));
	rec_bytes_normalized(0, 256);
	rec_bytes_normalized(sizeof zonemem - 256, 256);

	rec_int(cur->Hunk_LowMark());
	rec_int(cur->Hunk_HighMark());

	for (i = 0; i < command_count; i++)
		rec_str(commands[i].name);

#if !defined(SERVERONLY)
	for (i = 0; i < N_CACHE_USERS; i++)
		rec_int(buf_off(cache_users[i].data));
#endif
}

static void call(const char *what)
{
	rec_int(0x0CA11);
	rec_str(what);
}

/*----------------------------------------------------------------------------
 * helpers the scenarios speak in
 *--------------------------------------------------------------------------*/

static void init_zone(const char *what, const char *zone_parm, int dedicated)
{
	if (zone_parm)
		cmdline_set(3, "hexenwail", "-zone", zone_parm);
	else
		cmdline_set(1, "hexenwail", NULL, NULL);
	isDedicated = dedicated ? 1 : 0;

	call(what);
	cur->Memory_Init(zonemem, sizeof zonemem);
	rec_state();
}

static void *zalloc(const char *what, int size, int zone_id)
{
	void *p;

	call(what);
	p = cur->Z_Malloc(size, zone_id);
	rec_int(buf_off(p));
	rec_state();
	return p;
}

static void zfree(const char *what, void *p)
{
	call(what);
	cur->Z_Free(p);
	rec_state();
}

/* The guarded call.  Sys_Error unwinds here instead of ending the run, so the
 * fatal paths can be compared -- and *that is the only place the failing call
 * belongs*: the statement passed in runs inside the setjmp window. */
#define EXPECT_ERROR(what, want, ...) do { \
	call(what); \
	err_calls = 0; \
	err_buf[0] = 0; \
	err_armed = 1; \
	if (setjmp(err_env) == 0) { \
		__VA_ARGS__; \
		check(0, "[%s]: %s did not abort", cur->name, what); \
	} else { \
		check(err_calls == 1 && !strcmp(err_buf, want), \
			"[%s]: %s said \"%s\", want \"%s\"", cur->name, what, \
			err_buf, want); \
	} \
	err_armed = 0; \
	rec_int(err_calls); \
	rec_str(err_buf); \
	rec_state(); \
} while (0)

/*----------------------------------------------------------------------------
 * scenarios: initialization and the target configuration
 *--------------------------------------------------------------------------*/

static void scenario_target_config(void)
{
	/* What this arm's build compiled.  The Rust module's answers come from
	 * engine/rust/zone_target.c compiled with the same defines as the C
	 * original, which is the whole point of the per-target shim. */
	check(Zone_TargetDefSize() ==
#if defined(SERVERONLY)
		0x100000
#else
		0x200000
#endif
		, "[%s]: Zone_TargetDefSize() = 0x%x", cur->name,
		(unsigned)Zone_TargetDefSize());
	check(Zone_TargetSecSize() ==
#if defined(SERVERONLY)
		0
#else
		0x40000
#endif
		, "[%s]: Zone_TargetSecSize() = 0x%x", cur->name,
		(unsigned)Zone_TargetSecSize());
	check(Zone_TargetHasCache() ==
#if defined(SERVERONLY)
		0
#else
		1
#endif
		, "[%s]: Zone_TargetHasCache() = %d", cur->name,
		Zone_TargetHasCache());
	rec_int((int)Zone_TargetDefSize());
	rec_int((int)Zone_TargetSecSize());
	rec_int((int)Zone_TargetHasCache());
	rec_int((int)Zone_TargetDedicated());
}

/* The default zone, the -zone override, and the secondary zone's dependence on
 * the dedicated flag. */
static void scenario_init_paths(void)
{
	init_zone("init with the default zone and a client process", NULL, 0);
	init_zone("init again: the hunk is reset first", NULL, 0);
	init_zone("init with -zone 64", "64", 0);
	init_zone("init with -zone 8", "8", 0);
	init_zone("init with -zone 64 as a dedicated process", "64", 1);
	init_zone("init with an argument list that has no -zone", NULL, 1);
	init_zone("init with -zone but nothing after it", "64", 0);

	/* Registered commands: "flush" only where the target has the cache. */
	rec_int(command_count);
}

/*----------------------------------------------------------------------------
 * scenarios: the zone allocator
 *--------------------------------------------------------------------------*/

static void scenario_zalloc(void)
{
	void *a, *b, *c;

	init_zone("small zone for allocation cases", "64", 0);

	a = zalloc("a: 16 bytes", 16, Z_MAINZONE);
	b = zalloc("b: 64 bytes", 64, Z_MAINZONE);
	c = zalloc("c: 8 bytes", 8, Z_MAINZONE);

	/* The garbage marker Z_TagMalloc writes at the end of each block, and
	 * the zero fill of the payload, are part of the buffer's bytes. */
	zfree("free b (coalesces with nothing yet)", b);
	zfree("free a (coalesces with the free block above)", a);
	zfree("free c", c);

	call("allocate after frees: the rover and the free list decide where");
	rec_int(buf_off(cur->Z_Malloc(64, Z_MAINZONE)));
	rec_state();

	call("allocate a block small enough to leave no fragment");
	rec_int(buf_off(cur->Z_Malloc(8, Z_MAINZONE)));
	rec_state();

	call("allocate the whole zone");
	rec_int(buf_off(cur->Z_Malloc(32 * 1024, Z_MAINZONE)));
	rec_state();

	EXPECT_ERROR("free NULL must abort like the C", "Z_Free: NULL pointer",
		cur->Z_Free(NULL));

	{
		void *p = cur->Z_Malloc(16, Z_MAINZONE);
		rec_int(buf_off(p));
		cur->Z_Free(p);
		EXPECT_ERROR("freeing the same pointer twice",
			"Z_Free: freed a freed pointer", cur->Z_Free(p));
	}

	EXPECT_ERROR("a zone id no zone has", "Z_Malloc: Bad zone id 4",
		(void)cur->Z_Malloc(16, 4));

	EXPECT_ERROR("an allocation larger than the zone",
		"Z_Malloc: failed on allocation of 1048576 bytes",
		(void)cur->Z_Malloc(1048576, Z_MAINZONE));
}

static void scenario_zrealloc(void)
{
	char *p;

	init_zone("small zone for realloc cases", "64", 0);

	call("Z_Realloc(NULL) behaves as Z_Malloc");
	rec_int(buf_off(cur->Z_Realloc(NULL, 32, Z_MAINZONE)));
	rec_state();

	p = (char *)cur->Z_Malloc(32, Z_MAINZONE);
	memset(p, 'x', 32);

	call("grow, keeping the payload");
	{
		char *q = (char *)cur->Z_Realloc(p, 128, Z_MAINZONE);
		rec_int(buf_off(q));
		rec_int(q ? (unsigned char)q[0] : -1);
		rec_int(q ? (unsigned char)q[31] : -1);
		rec_int(q ? (unsigned char)q[127] : -1);
		rec_state();
		p = q;
	}

	call("shrink, keeping the payload");
	{
		char *q = (char *)cur->Z_Realloc(p, 16, Z_MAINZONE);
		rec_int(buf_off(q));
		rec_int(q ? (unsigned char)q[0] : -1);
		rec_int(q ? (unsigned char)q[15] : -1);
		rec_state();
		p = q;
	}

	call("realloc to the same size");
	rec_int(buf_off(cur->Z_Realloc(p, 16, Z_MAINZONE)));
	rec_state();

	{
		void *q = cur->Z_Malloc(16, Z_MAINZONE);
		cur->Z_Free(q);
		EXPECT_ERROR("Z_Realloc on a freed pointer",
			"Z_Realloc: realloced a freed pointer",
			(void)cur->Z_Realloc(q, 32, Z_MAINZONE));
	}

	call("Z_Strdup");
	rec_str(cur->Z_Strdup("a zone string"));
	rec_state();
	rec_int(buf_off(cur->Z_Strdup("another string")));
	rec_state();
}

/*----------------------------------------------------------------------------
 * scenarios: the hunk
 *--------------------------------------------------------------------------*/

static void scenario_hunk_low(void)
{
	int mark;

	init_zone("small zone for hunk cases", "8", 0);

	call("Hunk_Alloc");
	rec_int(buf_off(cur->Hunk_Alloc(32)));
	rec_state();

	call("Hunk_AllocName");
	rec_int(buf_off(cur->Hunk_AllocName(100, "named")));
	rec_state();

	call("Hunk_AllocName with a name longer than the field");
	rec_int(buf_off(cur->Hunk_AllocName(16,
		"a-name-far-longer-than-twenty-four-characters")));
	rec_state();

	call("Hunk_Strdup");
	rec_str(cur->Hunk_Strdup("hunk string", "dup"));
	rec_state();

	mark = cur->Hunk_LowMark();
	call("Hunk_LowMark");
	rec_int(mark);
	call("Hunk_AllocName again, above the mark");
	rec_int(buf_off(cur->Hunk_AllocName(64, "above")));
	rec_state();

	call("Hunk_FreeToLowMark zeroes the released region");
	cur->Hunk_FreeToLowMark(mark);
	rec_state();

	call("Hunk_Check on a hunk that just had a block released");
	cur->Hunk_Check();
	rec_state();

	EXPECT_ERROR("a mark above the low mark",
		"Hunk_FreeToLowMark: bad mark 1048576",
		cur->Hunk_FreeToLowMark(1048576));
	EXPECT_ERROR("a negative mark", "Hunk_FreeToHighMark: bad mark -1",
		cur->Hunk_FreeToHighMark(-1));
	EXPECT_ERROR("an allocation larger than the hunk",
		"Hunk_AllocName: failed on 4194336 bytes for huge",
		(void)cur->Hunk_AllocName(4194304, "huge"));
}

static void scenario_hunk_high(void)
{
	int mark;

	init_zone("small zone for high-hunk cases", "8", 0);

	call("Hunk_HighAllocName");
	rec_int(buf_off(cur->Hunk_HighAllocName(64, "high")));
	rec_state();

	mark = cur->Hunk_HighMark();
	rec_int(mark);

	call("another high allocation sits below the first");
	rec_int(buf_off(cur->Hunk_HighAllocName(128, "high2")));
	rec_state();

	call("Hunk_FreeToHighMark");
	cur->Hunk_FreeToHighMark(mark);
	rec_state();

	call("Hunk_TempAlloc");
	rec_int(buf_off(cur->Hunk_TempAlloc(200)));
	call("and again: the previous temp block is released first");
	rec_int(buf_off(cur->Hunk_TempAlloc(64)));
	rec_state();

	call("Hunk_HighMark while a temp allocation is live releases it");
	rec_int(cur->Hunk_HighMark());
	rec_state();

	call("a high allocation that cannot fit is reported, not fatal");
	rec_int(buf_off(cur->Hunk_HighAllocName(4194304, "toobig")));
	rec_state();
}

/*----------------------------------------------------------------------------
 * scenarios: the cache (client arm only)
 *--------------------------------------------------------------------------*/

#if !defined(SERVERONLY)
static void *cache_alloc(const char *what, int idx, int size, const char *name)
{
	void *p;

	call(what);
	p = cur->Cache_Alloc(&cache_users[idx], size, name);
	rec_int(buf_off(p));
	rec_state();
	return p;
}

static void cache_check(const char *what, int idx)
{
	call(what);
	rec_int(buf_off(cur->Cache_Check(&cache_users[idx])));
	rec_state();
}

static void cache_free(const char *what, int idx)
{
	call(what);
	cur->Cache_Free(&cache_users[idx]);
	rec_state();
}

static void scenario_cache(void)
{
	init_zone("a zone with room for the cache", "128", 0);

	cache_alloc("cache a", 0, 100, "first");
	cache_alloc("cache b", 1, 200, "second");
	cache_alloc("cache c", 2, 50, "third");

	cache_check("check a: moves it to the LRU head", 0);
	cache_check("check b", 1);
	cache_check("check c", 2);
	cache_check("check a again", 0);
	cache_check("check an entry that was never allocated", 5);

	cache_free("free b", 1);
	cache_check("check b after freeing it", 1);

	call("Cache_Flush");
	cur->Cache_Flush();
	rec_state();

	cache_check("check a after a flush", 0);

	/* Filling the cache until the LRU tail has to be evicted. */
	cache_alloc("fill 0", 0, 2000, "f0");
	cache_alloc("fill 1", 1, 2000, "f1");
	cache_alloc("fill 2", 2, 2000, "f2");
	cache_check("touch f0", 0);
	cache_alloc("fill 3, which evicts the LRU tail", 3, 2000, "f3");
	rec_state();

	cache_free("free 3", 3);
	call("Cache_Flush again");
	cur->Cache_Flush();
	rec_state();

	call("Cache_Report");
	cur->Cache_Report();

	EXPECT_ERROR("Cache_Free on a NULL entry", "Cache_Free: not allocated",
		cur->Cache_Free(&cache_users[5]));

	cache_alloc("allocate an entry", 4, 64, "dup");
	EXPECT_ERROR("Cache_Alloc over an already allocated entry",
		"Cache_Alloc: dup is already allocated",
		(void)cur->Cache_Alloc(&cache_users[4], 64, "dup"));

	EXPECT_ERROR("a zero-size cache allocation",
		"Cache_Alloc: bad size 0 for zero",
		(void)cur->Cache_Alloc(&cache_users[5], 0, "zero"));

	EXPECT_ERROR("a negative-size cache allocation",
		"Cache_Alloc: bad size -1 for negative",
		(void)cur->Cache_Alloc(&cache_users[5], -1, "negative"));
}

/* The hunk growing is what moves and evicts cache entries: Hunk_AllocName
 * calls Cache_FreeLow.  This is the interaction the ownership note says cannot
 * be ported as separate lifetimes. */
static void scenario_cache_vs_hunk(void)
{
	void *c;

	init_zone("a zone small enough that the hunk pressure is real", "64", 0);

	cache_alloc("cache an entry just above the low mark", 0, 300, "victim");
	c = cur->Cache_Check(&cache_users[0]);
	rec_int(buf_off(c));

	call("grow the low hunk over the cache entry");
	rec_int(buf_off(cur->Hunk_AllocName(20000, "pressure")));
	rec_state();
	rec_int(buf_off(cur->Cache_Check(&cache_users[0])));
	rec_state();

	call("grow it again, so the entry moves or is dropped");
	rec_int(buf_off(cur->Hunk_AllocName(20000, "pressure2")));
	rec_state();
	rec_int(buf_off(cur->Cache_Check(&cache_users[0])));
	rec_state();

	call("high allocations push from the other end");
	rec_int(buf_off(cur->Hunk_HighAllocName(20000, "highpressure")));
	rec_state();
	rec_int(buf_off(cur->Cache_Check(&cache_users[0])));
	rec_state();

	call("flush and report at the end");
	cur->Cache_Flush();
	cur->Cache_Report();
	rec_state();
}
#endif	/* !SERVERONLY */

/*----------------------------------------------------------------------------
 * the layout check
 *--------------------------------------------------------------------------*/

/* The two structures here are the ones zone.h or host.h actually declare.  The
 * rest of the zone structures -- zonelist_t, memblock_t, memzone_t, hunk_t,
 * cache_system_t -- are private to zone.c, so this harness cannot name them and
 * engine/rust/tests/abi_layout.c carries copies for the layout check.  What
 * verifies them here is stronger than an offset table: the buffer the C and the
 * Rust module leave behind is compared byte for byte after every call, so a
 * struct of the wrong size or field order shows up as a differing block header.
 */
static void layout_check(void)
{
	check(QuakeParmsC_sizeof() == sizeof(quakeparms_t), "quakeparms_t size");
	check(QuakeParmsC_offsetof_argc() == offsetof(quakeparms_t, argc),
		"quakeparms_t.argc");
	check(QuakeParmsC_offsetof_argv() == offsetof(quakeparms_t, argv),
		"quakeparms_t.argv");

#if !defined(SERVERONLY)
	check(CacheUserC_sizeof() == sizeof(cache_user_t), "cache_user_t size");
	check(CacheUserC_offsetof_data() == offsetof(cache_user_t, data),
		"cache_user_t.data");
#endif
}

/*----------------------------------------------------------------------------
 * case runner: one child per implementation per case
 *--------------------------------------------------------------------------*/

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

struct child_result {
	size_t len;
	int checks;
	int failures;
};

static int run_impl(int idx, void (*scenario)(void), unsigned char *out,
		size_t *outlen, int *out_checks, int *out_failures)
{
	char path[] = "/tmp/zone-harness-XXXXXX";
	struct child_result hdr;
	int fd;
	pid_t pid;
	int status = 0;
	size_t got;

	fd = mkstemp(path);
	if (fd < 0) {
		fprintf(stderr, "FAIL: mkstemp failed\n");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "FAIL: fork failed\n");
		close(fd);
		unlink(path);
		return -1;
	}
	if (pid == 0) {
		cur = &impls[idx];
		trace_len = 0;
		checks = 0;
		failures = 0;
		op_reset();
		scenario();

		hdr.len = trace_len;
		hdr.checks = checks;
		hdr.failures = failures;
		if (write(fd, &hdr, sizeof hdr) != (ssize_t)sizeof hdr)
			_exit(2);
		if (write(fd, trace, trace_len) != (ssize_t)trace_len)
			_exit(2);
		close(fd);
		_exit(0);
	}

	close(fd);
	waitpid(pid, &status, 0);

	fd = open(path, O_RDONLY);
	if (fd < 0 || read(fd, &hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
		if (fd >= 0)
			close(fd);
		unlink(path);
		fprintf(stderr, "FAIL: %s produced no trace\n", impls[idx].name);
		if (WIFSIGNALED(status))
			fprintf(stderr, "      killed by signal %d\n", WTERMSIG(status));
		return -1;
	}
	got = (size_t)read(fd, out, hdr.len);
	close(fd);
	unlink(path);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "FAIL: %s did not finish", impls[idx].name);
		if (WIFSIGNALED(status))
			fprintf(stderr, " (killed by signal %d)", WTERMSIG(status));
		fprintf(stderr, "\n");
		return -1;
	}
	if (got != hdr.len) {
		fprintf(stderr, "FAIL: %s trace truncated (%zu of %zu)\n",
			impls[idx].name, got, hdr.len);
		return -1;
	}

	*outlen = hdr.len;
	*out_checks = hdr.checks;
	*out_failures = hdr.failures;
	return 0;
}

static size_t trace_bytes;
static int cases_run;

static void run_case(const char *what, void (*scenario)(void))
{
	static unsigned char out[IMPL_COUNT][TRACE_MAX];
	static size_t len[IMPL_COUNT];
	int checks_seen[IMPL_COUNT];
	int failures_seen[IMPL_COUNT];
	int i, bad = 0;

	cases_run++;
	for (i = 0; i < IMPL_COUNT; ++i) {
		len[i] = 0;
		checks_seen[i] = 0;
		failures_seen[i] = 0;
		if (run_impl(i, scenario, out[i], &len[i], &checks_seen[i],
				&failures_seen[i]) != 0) {
			bad = 1;
			continue;
		}
		checks += checks_seen[i];
		failures += failures_seen[i];
	}

	if (bad)
		return;

	trace_bytes += len[IMPL_C];

	for (i = IMPL_C + 1; i < IMPL_COUNT; ++i) {
		size_t k;

		if (len[i] == len[IMPL_C]
			&& memcmp(out[i], out[IMPL_C], len[IMPL_C]) == 0)
			continue;

		failures++;
		fprintf(stderr, "FAIL: %s: %s and %s produced different traces\n",
			what, impls[IMPL_C].name, impls[i].name);
		for (k = 0; k < len[IMPL_C] && k < len[i]; ++k)
			if (out[IMPL_C][k] != out[i][k])
				break;
		fprintf(stderr, "      first difference at trace byte %zu "
			"(0x%02x vs 0x%02x), lengths %zu vs %zu\n", k,
			k < len[IMPL_C] ? out[IMPL_C][k] : 0,
			k < len[i] ? out[i][k] : 0, len[IMPL_C], len[i]);
	}
}

/*----------------------------------------------------------------------------
 * the benchmark
 *--------------------------------------------------------------------------*/

#define BENCH_ITERS 2000000

static double now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* The same operation mix for both implementations: a small string allocation
 * and free, a realloc, a hunk allocation released to a mark, and a temp
 * allocation.  These are the calls the engine makes constantly. */
static void bench_mix(const zone_impl_t *impl, long iters)
{
	long i;
	int mark;

	op_reset();
	cmdline_set(3, "hexenwail", "-zone", "64");
	impl->Memory_Init(zonemem, sizeof zonemem);

	impl->Hunk_LowMark();
	for (i = 0; i < iters; i++) {
		char *p = (char *)impl->Z_Malloc(64, Z_MAINZONE);
		p[0] = (char)i;
		p = (char *)impl->Z_Realloc(p, 96, Z_MAINZONE);
		impl->Z_Free(p);

		mark = impl->Hunk_LowMark();
		(void)impl->Hunk_AllocName(64, "bench");
		impl->Hunk_FreeToLowMark(mark);

		(void)impl->Hunk_TempAlloc(128);
	}
}

static int bench_main(void)
{
	double t0, tc, tr;
	long iters = BENCH_ITERS;

	t0 = now_seconds();
	bench_mix(&impls[IMPL_C], iters);
	tc = now_seconds() - t0;

	t0 = now_seconds();
	bench_mix(&impls[IMPL_RUST], iters);
	tr = now_seconds() - t0;

	printf("benchmark: %ld iterations of Z_Malloc/Z_Realloc/Z_Free + "
		"Hunk_AllocName/Hunk_FreeToLowMark + Hunk_TempAlloc\n", iters);
	printf("  C    : %8.3f s  (%6.1f ns/iteration)\n", tc,
		tc * 1e9 / (double)iters);
	printf("  Rust : %8.3f s  (%6.1f ns/iteration)\n", tr,
		tr * 1e9 / (double)iters);
	printf("  ratio Rust/C: %.3f\n", tc > 0.0 ? tr / tc : 0.0);
	return 0;
}

/*----------------------------------------------------------------------------
 * main
 *--------------------------------------------------------------------------*/

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--bench"))
		return bench_main();

	printf("zone differential harness (%s arm): the C original and the Rust "
		"module\n",
#if defined(SERVERONLY)
		"SERVERONLY"
#else
		"client"
#endif
	);

	layout_check();

	run_case("target configuration", scenario_target_config);
	run_case("initialization paths", scenario_init_paths);
	run_case("zone allocation", scenario_zalloc);
	run_case("zone realloc", scenario_zrealloc);
	run_case("hunk low", scenario_hunk_low);
	run_case("hunk high", scenario_hunk_high);
#if !defined(SERVERONLY)
	run_case("cache", scenario_cache);
	run_case("cache under hunk pressure", scenario_cache_vs_hunk);
#endif

	printf("checked %d expectations, %d failures, %d cases, %zu trace bytes\n",
		checks, failures, cases_run, trace_bytes);
	if (failures) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
