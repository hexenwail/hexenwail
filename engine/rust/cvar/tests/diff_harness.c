// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for the Rust port of engine/h2shared/cvar.c.
//
// The C original is compiled with every exported name renamed (c_Cvar_*) and
// linked into this binary beside the Rust module, and the two are driven
// through one table of function pointers.  After every call the whole
// observable state is recorded into a byte trace -- the list order, every
// field of every node the harness registered, the config-dirty flag, the
// diagnostics CON_Printf produced, and every Z_Malloc/Z_Strdup/Z_Free the
// implementation performed -- and the two traces are compared byte for byte.
// Two implementations that are both wrong still pass a golden-only test; this
// is why the comparison is C against Rust rather than Rust against a literal.
//
// The harness owns the cvar_t nodes, as the engine does: the C's declarations
// are file-scope globals and Cvar_RegisterVariable only links them in.  The
// node pool is reset before each implementation runs a scenario, so the two
// never share mutated state.
//
// What is deliberately total about the recording:
//
//   * flags, so CVAR_CHANGED / CVAR_REGISTERED / CVAR_CALLBACK drift shows up;
//   * string identity, so a port that keeps the caller's literal instead of
//     copying it, or shares a default_string between two nodes, is visible;
//   * the allocator events, so a port that used a Rust allocator -- or freed a
//     pointer this harness never handed out -- fails rather than merely
//     producing different bytes;
//   * the callback count and order, because Cvar_SetQuick's no-change return
//     above the callback is load-bearing for the alias mirror.
//
// Calls that re-enter the API from a callback are part of the case list, not
// an edge case: SV_Callback_Serverinfo does exactly that in hwsv.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "quakedef.h"
#include "cvar.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "q_ctype.h"

/*----------------------------------------------------------------------------
 * the two implementations
 *--------------------------------------------------------------------------*/

// Every symbol cvar.c exports, renamed for the C original so it can share the
// binary with the Rust module.
#define DECLARE_CVAR_IMPL(P) \
	extern cvar_t *P##Cvar_FindVar (const char *var_name); \
	extern cvar_t *P##Cvar_FindVarAfter (const char *prev_name, unsigned int with_flags); \
	extern void P##Cvar_LockVar (const char *var_name); \
	extern void P##Cvar_UnlockVar (const char *var_name); \
	extern void P##Cvar_UnlockAll (void); \
	extern float P##Cvar_VariableValue (const char *var_name); \
	extern const char *P##Cvar_VariableString (const char *var_name); \
	extern void P##Cvar_MarkConfigDirty (void); \
	extern qboolean P##Cvar_ConfigDirty (void); \
	extern void P##Cvar_ConfigWritten (void); \
	extern void P##Cvar_SetQuick (cvar_t *var, const char *value); \
	extern void P##Cvar_SetValueQuick (cvar_t *var, const float value); \
	extern void P##Cvar_Set (const char *var_name, const char *value); \
	extern void P##Cvar_SetValue (const char *var_name, const float value); \
	extern void P##Cvar_SetROM (const char *var_name, const char *value); \
	extern void P##Cvar_SetValueROM (const char *var_name, const float value); \
	extern void P##Cvar_RegisterVariable (cvar_t *variable); \
	extern void P##Cvar_Reset (const char *name); \
	extern qboolean P##Cvar_HasValue (const cvar_t *var, const char *value); \
	extern void P##Cvar_Init (void); \
	extern void P##Cvar_SetCallback (cvar_t *var, cvarcallback_t func); \
	extern void P##Cvar_RegisterAlias (cvar_t *alias, cvar_t *target); \
	extern qboolean P##Cvar_Command (void); \
	extern void P##Cvar_MoveToFront (const char *name); \
	extern void P##Cvar_WriteVariables (FILE *f)

DECLARE_CVAR_IMPL(c_);
DECLARE_CVAR_IMPL();

// The Rust layout accessors, so the harness can check the struct it compiles
// against without restating Rust's offsets in C.
extern size_t CvarC_sizeof(void);
extern size_t CvarC_alignof(void);
extern size_t CvarC_offsetof_name(void);
extern size_t CvarC_offsetof_string(void);
extern size_t CvarC_offsetof_flags(void);
extern size_t CvarC_offsetof_value(void);
extern size_t CvarC_offsetof_integer(void);
extern size_t CvarC_offsetof_callback(void);
extern size_t CvarC_offsetof_next(void);
extern size_t CvarC_offsetof_default_string(void);

typedef void (*xcommand_t)(void);

typedef struct cvar_impl_s {
	const char *name;
	cvar_t *(*FindVar)(const char *);
	cvar_t *(*FindVarAfter)(const char *, unsigned int);
	void (*LockVar)(const char *);
	void (*UnlockVar)(const char *);
	void (*UnlockAll)(void);
	float (*VariableValue)(const char *);
	const char *(*VariableString)(const char *);
	void (*MarkConfigDirty)(void);
	qboolean (*ConfigDirty)(void);
	void (*ConfigWritten)(void);
	void (*SetQuick)(cvar_t *, const char *);
	void (*SetValueQuick)(cvar_t *, float);
	void (*Set)(const char *, const char *);
	void (*SetValue)(const char *, float);
	void (*SetROM)(const char *, const char *);
	void (*SetValueROM)(const char *, float);
	void (*RegisterVariable)(cvar_t *);
	void (*Reset)(const char *);
	qboolean (*HasValue)(const cvar_t *, const char *);
	void (*Init)(void);
	void (*SetCallback)(cvar_t *, cvarcallback_t);
	void (*RegisterAlias)(cvar_t *, cvar_t *);
	qboolean (*Command)(void);
	void (*MoveToFront)(const char *);
	void (*WriteVariables)(FILE *);
} cvar_impl_t;

enum { IMPL_C = 0, IMPL_RUST = 1, IMPL_COUNT = 2 };

static const cvar_impl_t impls[IMPL_COUNT] = {
	{
		"C",
		c_Cvar_FindVar, c_Cvar_FindVarAfter, c_Cvar_LockVar,
		c_Cvar_UnlockVar, c_Cvar_UnlockAll, c_Cvar_VariableValue,
		c_Cvar_VariableString, c_Cvar_MarkConfigDirty,
		c_Cvar_ConfigDirty, c_Cvar_ConfigWritten, c_Cvar_SetQuick,
		c_Cvar_SetValueQuick, c_Cvar_Set, c_Cvar_SetValue, c_Cvar_SetROM,
		c_Cvar_SetValueROM, c_Cvar_RegisterVariable, c_Cvar_Reset,
		c_Cvar_HasValue, c_Cvar_Init, c_Cvar_SetCallback,
		c_Cvar_RegisterAlias, c_Cvar_Command, c_Cvar_MoveToFront,
		c_Cvar_WriteVariables,
	},
	{
		"Rust",
		Cvar_FindVar, Cvar_FindVarAfter, Cvar_LockVar,
		Cvar_UnlockVar, Cvar_UnlockAll, Cvar_VariableValue,
		Cvar_VariableString, Cvar_MarkConfigDirty,
		Cvar_ConfigDirty, Cvar_ConfigWritten, Cvar_SetQuick,
		Cvar_SetValueQuick, Cvar_Set, Cvar_SetValue, Cvar_SetROM,
		Cvar_SetValueROM, Cvar_RegisterVariable, Cvar_Reset,
		Cvar_HasValue, Cvar_Init, Cvar_SetCallback,
		Cvar_RegisterAlias, Cvar_Command, Cvar_MoveToFront,
		Cvar_WriteVariables,
	},
};

static const cvar_impl_t *cur;

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

#define TRACE_MAX (1024 * 1024)

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
 * the allocator the cvar strings must go through
 *--------------------------------------------------------------------------*/

// Z_Malloc / Z_Strdup / Z_Free as the engine's zone behaves for these calls:
// zero-filled memory, and a free that can only be handed pointers this harness
// allocated.  Every event is recorded, so a port that allocated a string with
// a Rust allocator shows up as a missing Z_Malloc rather than as a crash
// somewhere later.

#define ALLOC_MAX 256

static struct {
	void *p;
	size_t n;
	int freed;
} allocs[ALLOC_MAX];
static int alloc_n;

static int alloc_live(void *p)
{
	int i;

	for (i = 0; i < alloc_n; i++)
		if (allocs[i].p == p && !allocs[i].freed)
			return 1;
	return 0;
}

void *Z_Malloc(int size, int zone_id)
{
	void *p;

	(void)zone_id;
	rec_int(0x2411);
	rec_int(size);
	if (size <= 0)
		fail("Z_Malloc(%d)", size);
	p = malloc((size_t)size);
	if (!p) {
		fail("Z_Malloc: out of memory");
		abort();
	}
	memset(p, 0, (size_t)size);
	if (alloc_n < ALLOC_MAX) {
		allocs[alloc_n].p = p;
		allocs[alloc_n].n = (size_t)size;
		allocs[alloc_n].freed = 0;
		alloc_n++;
	}
	return p;
}

void Z_Free(void *ptr)
{
	int i;

	rec_int(0x2F7E);
	if (!ptr) {
		rec_int(-1);
		return;
	}
	for (i = 0; i < alloc_n; i++) {
		if (allocs[i].p == ptr && !allocs[i].freed) {
			rec_int(i);
			allocs[i].freed = 1;
			free(ptr);
			return;
		}
	}
	// A pointer the engine never handed out: a Rust allocator, or a free of
	// something already freed.  Both are bugs the C cannot have.
	rec_int(-2);
	fail("Z_Free of a pointer Z_Malloc/Z_Strdup never handed out");
}

char *Z_Strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = Z_Malloc((int)n, 1);
	memcpy(p, s, n);
	return p;
}

/*----------------------------------------------------------------------------
 * the rest of the engine the port reaches
 *--------------------------------------------------------------------------*/

static char printf_buf[256];

// The C original is linked under renamed symbols, so its diagnostics name
// themselves "c_Cvar_Set" where the shipping build says "Cvar_Set".  That is a
// harness artifact, not a port difference, so it is removed here and the
// comparison sees what the engine's own build would print.
static void normalize_diag(void)
{
	if (strncmp(printf_buf, "c_", 2) == 0)
		memmove(printf_buf, printf_buf + 2, strlen(printf_buf + 2) + 1);
}

void CON_Printf(unsigned int flags, const char *fmt, ...)
{
	va_list ap;

	(void)flags;
	va_start(ap, fmt);
	vsnprintf(printf_buf, sizeof printf_buf, fmt, ap);
	va_end(ap);
	normalize_diag();

	rec_int(0x9911);
	rec_str(printf_buf);
}

#define CMD_MAX 64

static struct {
	char name[32];
	xcommand_t fn;
} cmds[CMD_MAX];
static int cmd_n;

void Cmd_AddCommand(const char *name, xcommand_t fn)
{
	rec_int(0xADD0);
	rec_str(name);
	if (cmd_n < CMD_MAX) {
		snprintf(cmds[cmd_n].name, sizeof cmds[cmd_n].name, "%s", name);
		cmds[cmd_n].fn = fn;
		cmd_n++;
	}
}

qboolean Cmd_Exists(const char *name)
{
	int i;

	for (i = 0; i < cmd_n; i++)
		if (!strcmp(name, cmds[i].name))
			return true;
	return false;
}

static int cmd_argc;
static const char *cmd_argv[8];

const char *Cmd_Argv(int arg)
{
	if (arg < 0 || arg >= cmd_argc)
		return "";
	return cmd_argv[arg];
}

int Cmd_Argc(void)
{
	return cmd_argc;
}

// q_strcasecmp is the engine's (engine/h2shared/common.c), copied with its
// q_tolower so the two implementations compare the same strings the same way.
int q_strcasecmp(const char *s1, const char *s2)
{
	const char *p1 = s1;
	const char *p2 = s2;
	char c1, c2;

	if (p1 == p2)
		return 0;

	do {
		c1 = (char)q_tolower((unsigned char)*p1++);
		c2 = (char)q_tolower((unsigned char)*p2++);
		if (c1 == '\0')
			break;
	} while (c1 == c2);

	return (int)(c1 - c2);
}

/*----------------------------------------------------------------------------
 * the cvar nodes the harness owns
 *--------------------------------------------------------------------------*/

#define NODE_MAX 40

static cvar_t nodes[NODE_MAX];
static int node_count;

static void node_init(int i, const char *name, const char *value, unsigned int flags)
{
	memset(&nodes[i], 0, sizeof nodes[i]);
	nodes[i].name = name;
	nodes[i].string = (char *)value;
	nodes[i].flags = flags;
	if (i + 1 > node_count)
		node_count = i + 1;
}

static int node_index(const cvar_t *v)
{
	int i;

	if (!v)
		return -1;
	for (i = 0; i < node_count; i++)
		if (&nodes[i] == v)
			return i;
	return -2;
}

/*----------------------------------------------------------------------------
 * callbacks
 *--------------------------------------------------------------------------*/

static int cb_calls;

static void cb_record(cvar_t *var)
{
	rec_int(0xC0DE);
	rec_str(var->name);
	cb_calls++;

	// The count in the trace keeps a callback that fires on a redundant set
	// distinguishable from one that does not.
	rec_int(cb_calls);
}

static void cb_set_other(cvar_t *var);
static void cb_set_self(cvar_t *var);

/*----------------------------------------------------------------------------
 * state recording
 *--------------------------------------------------------------------------*/

// Everything observable after a call: the dirty flag, the list order as the
// exported enumeration reports it, and every field of every node -- including
// which of the strings are pointers this harness allocated, so a port that
// kept a caller's literal or shared a default_string is visible.
static void rec_state(void)
{
	int i;
	const cvar_t *p;
	int order_n = 0;
	unsigned int bits;

	rec_int(cur->ConfigDirty());

	p = cur->FindVarAfter("", 0);
	while (p) {
		rec_str(p->name);
		order_n++;
		if (order_n > NODE_MAX + 2) {
			fail("the cvar list does not terminate");
			break;
		}
		p = cur->FindVarAfter(p->name, 0);
	}
	rec_int(order_n);

	rec_int(node_count);
	for (i = 0; i < node_count; i++) {
		cvar_t *v = &nodes[i];

		rec_str(v->name ? v->name : "(null)");
		rec_int(v->string ? 1 : 0);
		rec_int(v->string ? alloc_live((void *)v->string) : 0);
		rec_str(v->string ? v->string : "(null)");
		rec_int(v->default_string ? 1 : 0);
		rec_int(v->default_string ? alloc_live((void *)v->default_string) : 0);
		rec_str(v->default_string ? v->default_string : "(null)");
		rec_int((int)v->flags);
		memcpy(&bits, &v->value, sizeof bits);
		rec_int((int)bits);
		rec_int(v->integer);
		rec_int(v->callback != NULL);
		rec_int(node_index(v->next));
	}
}

// The harness's own counters, so a port that stops calling a stub -- e.g. one
// that stopped consulting Cmd_Exists before registering -- is visible even
// when the resulting node state matches.
static void rec_counters(void)
{
	rec_int(cmd_n);
	rec_int(cb_calls);
}

static void op_reset(void)
{
	int i;

	for (i = 0; i < alloc_n; i++)
		if (!allocs[i].freed) {
			free(allocs[i].p);
			allocs[i].freed = 1;
		}
	alloc_n = 0;
	cmd_n = 0;
	cmd_argc = 0;
	cb_calls = 0;
	printf_buf[0] = 0;
}

static void scenario_reset(void)
{
	memset(nodes, 0, sizeof nodes);
	node_count = 0;
	op_reset();
}

/*----------------------------------------------------------------------------
 * helpers the scenarios speak in
 *--------------------------------------------------------------------------*/

static void call(const char *what)
{
	rec_int(0x0CA11);
	rec_str(what);
}

static void argc_set(int argc, const char *a0, const char *a1, const char *a2,
		const char *a3)
{
	cmd_argc = argc;
	cmd_argv[0] = a0;
	cmd_argv[1] = a1;
	cmd_argv[2] = a2;
	cmd_argv[3] = a3;
}

static xcommand_t cmd_find(const char *name)
{
	int i;

	for (i = 0; i < cmd_n; i++)
		if (!strcmp(name, cmds[i].name))
			return cmds[i].fn;
	return NULL;
}

/*----------------------------------------------------------------------------
 * scenarios
 *--------------------------------------------------------------------------*/

static void layout_check(void)
{
	check(CvarC_sizeof() == sizeof(cvar_t), "CvarC_sizeof: Rust %zu, C %zu",
		CvarC_sizeof(), sizeof(cvar_t));
	check(CvarC_alignof() == _Alignof(cvar_t), "CvarC_alignof: Rust %zu, C %zu",
		CvarC_alignof(), _Alignof(cvar_t));
	check(CvarC_offsetof_name() == offsetof(cvar_t, name),
		"offsetof(name): Rust %zu, C %zu", CvarC_offsetof_name(),
		offsetof(cvar_t, name));
	check(CvarC_offsetof_string() == offsetof(cvar_t, string),
		"offsetof(string): Rust %zu, C %zu", CvarC_offsetof_string(),
		offsetof(cvar_t, string));
	check(CvarC_offsetof_flags() == offsetof(cvar_t, flags),
		"offsetof(flags): Rust %zu, C %zu", CvarC_offsetof_flags(),
		offsetof(cvar_t, flags));
	check(CvarC_offsetof_value() == offsetof(cvar_t, value),
		"offsetof(value): Rust %zu, C %zu", CvarC_offsetof_value(),
		offsetof(cvar_t, value));
	check(CvarC_offsetof_integer() == offsetof(cvar_t, integer),
		"offsetof(integer): Rust %zu, C %zu", CvarC_offsetof_integer(),
		offsetof(cvar_t, integer));
	check(CvarC_offsetof_callback() == offsetof(cvar_t, callback),
		"offsetof(callback): Rust %zu, C %zu", CvarC_offsetof_callback(),
		offsetof(cvar_t, callback));
	check(CvarC_offsetof_next() == offsetof(cvar_t, next),
		"offsetof(next): Rust %zu, C %zu", CvarC_offsetof_next(),
		offsetof(cvar_t, next));
	check(CvarC_offsetof_default_string() == offsetof(cvar_t, default_string),
		"offsetof(default_string): Rust %zu, C %zu",
		CvarC_offsetof_default_string(),
		offsetof(cvar_t, default_string));
	// The C's positional initialisers depend on default_string being last.
	check(offsetof(cvar_t, default_string) > offsetof(cvar_t, next),
		"the harness assumes default_string is the last field");
}

// Registration: the caller's value is copied, the node is linked in at the
// head, CVAR_REGISTERED and the callback rule are applied, and a duplicate or
// a command name is refused without touching the list.
static void scenario_register(void)
{
	node_init(0, "alpha", "1", CVAR_ARCHIVE);
	node_init(1, "beta", "two words", 0);

	call("register alpha");
	cur->RegisterVariable(&nodes[0]);
	rec_state();

	call("register beta");
	cur->RegisterVariable(&nodes[1]);
	rec_state();

	// duplicate name: refused
	node_init(2, "alpha", "9", 0);
	call("register a duplicate");
	cur->RegisterVariable(&nodes[2]);
	rec_state();

	// a name that is a command: refused
	cur->Init();
	node_init(3, "toggle", "1", 0);
	call("register a command name");
	cur->RegisterVariable(&nodes[3]);
	rec_state();

	// an unregistered node can still be found by name?  No: it is not in the
	// list, so the lookup misses.
	call("find an unregistered node");
	check(cur->FindVar("gamma") == NULL, "[%s]: found an unregistered cvar",
		cur->name);
	rec_int(node_index(cur->FindVar("gamma")));
	rec_state();
	rec_counters();
}

// Setting: same-length and different-length replacement, the no-change early
// return, the CVAR_CHANGED flag, the config-dirty flag and the callback.
static void scenario_set(void)
{
	node_init(0, "same", "abcd", CVAR_ARCHIVE);
	node_init(1, "grow", "a", CVAR_ARCHIVE);
	node_init(2, "shrink", "abcdef", 0);
	node_init(3, "cb", "0", CVAR_ARCHIVE);

	cur->RegisterVariable(&nodes[0]);
	cur->RegisterVariable(&nodes[1]);
	cur->RegisterVariable(&nodes[2]);
	cur->RegisterVariable(&nodes[3]);
	cur->SetCallback(&nodes[3], cb_record);
	rec_state();

	call("set same-length");
	cur->Set("same", "wxyz");
	rec_state();

	call("set the same value again");
	cur->Set("same", "wxyz");
	rec_state();

	call("grow the string");
	cur->Set("grow", "0123456789");
	rec_state();

	call("shrink the string");
	cur->Set("shrink", "x");
	rec_state();

	call("set a callback cvar");
	cur->Set("cb", "1");
	rec_state();

	call("set it to the same value");
	cur->Set("cb", "1");
	rec_state();

	call("set an unknown cvar");
	cur->Set("nosuch", "1");
	rec_state();

	call("SetQuick directly");
	cur->SetQuick(&nodes[0], "ab");
	rec_state();

	call("SetQuick on the same value");
	cur->SetQuick(&nodes[0], "ab");
	rec_state();
	rec_counters();
}

// The two value setters format numbers identically, including the C's
// trailing-zero trim, and go through atof/_atoi on the way back.
static void scenario_values(void)
{
	static const float values[] = {
		0.0f, 1.0f, -1.0f, 2.0f, 0.5f, 1.25f, -0.125f, 100.0f, 3.14159f,
		123456.0f, 0.1f, -273.15f,
	};
	unsigned int i;

	node_init(0, "num", "0", CVAR_ARCHIVE);
	node_init(1, "numq", "0", 0);
	cur->RegisterVariable(&nodes[0]);
	cur->RegisterVariable(&nodes[1]);
	rec_state();

	for (i = 0; i < sizeof values / sizeof values[0]; i++) {
		char what[64];

		snprintf(what, sizeof what, "SetValue %g", values[i]);
		call(what);
		cur->SetValue("num", values[i]);
		rec_state();

		snprintf(what, sizeof what, "SetValueQuick %g", values[i]);
		call(what);
		cur->SetValueQuick(&nodes[1], values[i]);
		rec_state();
	}
	rec_counters();
}

// CVAR_ROM and CVAR_LOCKED refuse a plain set; Cvar_SetROM lifts ROM for the
// duration of its own set, and the lock commands are the only way past LOCKED.
static void scenario_rom_locked(void)
{
	node_init(0, "rom", "1", CVAR_ROM);
	node_init(1, "locked", "1", CVAR_LOCKED);
	node_init(2, "plain", "1", 0);
	cur->RegisterVariable(&nodes[0]);
	cur->RegisterVariable(&nodes[1]);
	cur->RegisterVariable(&nodes[2]);
	rec_state();

	call("set a ROM cvar");
	cur->Set("rom", "2");
	rec_state();

	call("SetROM");
	cur->SetROM("rom", "2");
	rec_state();

	call("SetValueROM");
	cur->SetValueROM("rom", 3.5f);
	rec_state();

	call("set a locked cvar");
	cur->Set("locked", "2");
	rec_state();

	call("unlock it");
	cur->UnlockVar("locked");
	call("set it now");
	cur->Set("locked", "2");
	rec_state();

	call("lock it again");
	cur->LockVar("locked");
	call("set it while locked");
	cur->Set("locked", "3");
	rec_state();

	call("lock a plain cvar and unlock everything");
	cur->LockVar("plain");
	cur->UnlockAll();
	cur->Set("plain", "9");
	rec_state();

	call("lock/unlock an unknown cvar");
	cur->LockVar("nosuch");
	cur->UnlockVar("nosuch");
	rec_state();
	rec_counters();
}

// Reads: VariableValue/String, HasValue's numeric-vs-textual rule, and the
// FindVarAfter enumeration with and without a flag filter.
static void scenario_reads(void)
{
	const char *strings[] = { "", "0", "1", "1.0", "1.000", "abc", "-2", "2" };
	unsigned int i;

	node_init(0, "num", "1.000", CVAR_ARCHIVE);
	node_init(1, "str", "abc", CVAR_ARCHIVE);
	node_init(2, "plain", "7", 0);
	cur->RegisterVariable(&nodes[0]);
	cur->RegisterVariable(&nodes[1]);
	cur->RegisterVariable(&nodes[2]);
	rec_state();

	call("VariableValue of a number");
	check(cur->VariableValue("num") == 1.0f, "[%s]: VariableValue(num) = %f",
		cur->name, (double)cur->VariableValue("num"));
	rec_int(1);
	call("VariableValue of a non-number");
	check(cur->VariableValue("str") == 0.0f,
		"[%s]: VariableValue(str) = %f", cur->name,
		(double)cur->VariableValue("str"));
	rec_int(1);
	call("VariableValue of an unknown cvar");
	check(cur->VariableValue("nosuch") == 0.0f,
		"[%s]: VariableValue(nosuch) = %f", cur->name,
		(double)cur->VariableValue("nosuch"));
	rec_int(1);

	call("VariableString of a registered cvar");
	rec_str(cur->VariableString("num"));
	call("VariableString of an unknown cvar");
	rec_str(cur->VariableString("nosuch"));

	for (i = 0; i < sizeof strings / sizeof strings[0]; i++) {
		rec_int(cur->HasValue(&nodes[0], strings[i]));
		rec_int(cur->HasValue(&nodes[1], strings[i]));
	}
	rec_int(cur->HasValue(NULL, "1"));
	rec_int(cur->HasValue(&nodes[0], NULL));

	call("FindVarAfter with no filter");
	{
		const cvar_t *p = cur->FindVarAfter("", 0);
		while (p) {
			rec_str(p->name);
			p = cur->FindVarAfter(p->name, 0);
		}
	}
	call("FindVarAfter filtered on CVAR_ARCHIVE");
	{
		const cvar_t *p = cur->FindVarAfter("", CVAR_ARCHIVE);
		while (p) {
			rec_str(p->name);
			p = cur->FindVarAfter(p->name, CVAR_ARCHIVE);
		}
	}
	call("FindVarAfter from an unknown name");
	rec_int(node_index(cur->FindVarAfter("nosuch", 0)));
	rec_state();
	rec_counters();
}

// Cvar_MoveToFront: the order is observable, so this is list surgery the port
// has to reproduce exactly, including that the head itself is the only thing
// that moves when the named cvar is already at the front.
static void scenario_order(void)
{
	node_init(0, "one", "1", 0);
	node_init(1, "two", "2", 0);
	node_init(2, "three", "3", 0);
	cur->RegisterVariable(&nodes[0]);
	cur->RegisterVariable(&nodes[1]);
	cur->RegisterVariable(&nodes[2]);
	rec_state();

	call("move the head to the front");
	cur->MoveToFront("three");
	rec_state();

	call("move the middle cvar to the front");
	cur->MoveToFront("one");
	rec_state();

	call("move the tail to the front");
	cur->MoveToFront("two");
	rec_state();

	call("move an unknown cvar");
	cur->MoveToFront("nosuch");
	rec_state();
	rec_counters();
}

// config.cfg: only archived cvars are written, in list order, and the dirty
// flag tracks changes that would make the file stale.
static void scenario_config(void)
{
	FILE *f;
	char buf[512];
	size_t n;

	node_init(0, "archive_a", "1", CVAR_ARCHIVE);
	node_init(1, "not_archived", "2", 0);
	node_init(2, "archive_b", "3", CVAR_ARCHIVE);
	cur->RegisterVariable(&nodes[0]);
	cur->RegisterVariable(&nodes[1]);
	cur->RegisterVariable(&nodes[2]);

	call("ConfigDirty at the start");
	rec_int(cur->ConfigDirty());
	call("MarkConfigDirty");
	cur->MarkConfigDirty();
	rec_int(cur->ConfigDirty());
	call("ConfigWritten");
	cur->ConfigWritten();
	rec_int(cur->ConfigDirty());

	call("set an archived cvar");
	cur->Set("archive_a", "changed");
	rec_int(cur->ConfigDirty());
	call("set a non-archived cvar");
	cur->ConfigWritten();
	cur->Set("not_archived", "changed");
	rec_int(cur->ConfigDirty());

	call("WriteVariables");
	f = tmpfile();
	if (!f) {
		fail("tmpfile failed");
		abort();
	}
	cur->WriteVariables(f);
	rewind(f);
	n = fread(buf, 1, sizeof buf - 1, f);
	buf[n] = 0;
	fclose(f);
	rec_str(buf);
	rec_state();
	rec_counters();
}

// Aliases: a second spelling that mirrors the target both ways, is never
// archived, chains the target's own callback, and refuses the three cases the
// C refuses.
static void scenario_aliases(void)
{
	node_init(0, "target", "0", CVAR_ARCHIVE);
	node_init(1, "alias", "0", CVAR_ARCHIVE);
	node_init(2, "second", "0", 0);
	cur->RegisterVariable(&nodes[0]);

	call("alias a target's own callback first");
	cur->SetCallback(&nodes[0], cb_record);

	call("register the alias");
	cur->RegisterAlias(&nodes[1], &nodes[0]);
	rec_state();

	call("set through the alias");
	cur->Set("alias", "1");
	rec_state();

	call("set through the target");
	cur->Set("target", "2");
	rec_state();

	call("set the alias to the value it already mirrors");
	cur->Set("alias", "2");
	rec_state();

	call("reset the target");
	cur->Reset("target");
	rec_state();

	call("reset the alias");
	cur->Reset("alias");
	rec_state();

	// A second alias on the same target, and an alias of an alias, are both
	// refused.
	call("alias an already-aliased target");
	cur->RegisterAlias(&nodes[2], &nodes[0]);
	rec_state();
	call("alias from an already-aliased alias");
	cur->RegisterAlias(&nodes[2], &nodes[1]);
	rec_state();

	// An unregistered target is refused.
	call("alias an unregistered target");
	cur->RegisterAlias(&nodes[2], &nodes[2]);
	rec_state();
	rec_counters();
}

// The maximum alias count is a fixed 32, so the 33rd is refused with a
// diagnostic rather than corrupting the table.
static void scenario_alias_limit(void)
{
	static cvar_t many[40];
	int i;

	memset(many, 0, sizeof many);
	node_init(0, "base", "0", 0);
	cur->RegisterVariable(&nodes[0]);

	for (i = 0; i < 33; i++) {
		char name[32];
		char value[8];

		snprintf(name, sizeof name, "alias_%d", i);
		snprintf(value, sizeof value, "%d", i);
		many[i].name = strdup(name);
		many[i].string = strdup(value);
		many[i].flags = 0;
		cur->RegisterAlias(&many[i], &nodes[0]);
	}

	call("the alias table after 33 registrations");
	rec_int(node_index(cur->FindVar("alias_0")));
	rec_int(node_index(cur->FindVar("alias_31")));
	rec_int(node_index(cur->FindVar("alias_32")));
	rec_state();
	rec_counters();
}

// Cvar_Command and the seven console commands Cvar_Init registers.  The
// commands are reached through the pointers this harness captured from
// Cmd_AddCommand, with a synthetic argv.
static void scenario_commands(void)
{
	xcommand_t fn;

	node_init(0, "cv", "start", CVAR_ARCHIVE);
	node_init(1, "other", "1", CVAR_ARCHIVE);
	cur->Init();
	cur->RegisterVariable(&nodes[0]);
	cur->RegisterVariable(&nodes[1]);
	rec_state();

	call("Cvar_Command with no argument prints");
	argc_set(1, "cv", NULL, NULL, NULL);
	rec_int(cur->Command());
	rec_state();

	call("Cvar_Command with an argument sets");
	argc_set(2, "cv", "set", NULL, NULL);
	rec_int(cur->Command());
	rec_state();

	call("Cvar_Command on an unknown name");
	argc_set(1, "nosuch", NULL, NULL, NULL);
	rec_int(cur->Command());
	rec_state();

	call("Cmd_Argc is 0");
	cmd_argc = 0;
	rec_int(cur->Command());
	rec_state();

	fn = cmd_find("cycle");
	call("cycle with too few arguments");
	argc_set(1, "cycle", NULL, NULL, NULL);
	if (fn) fn();
	rec_state();

	fn = cmd_find("cycle");
	call("cycle to the next value");
	argc_set(4, "cycle", "cv", "a", "b");
	if (fn) fn();
	rec_state();

	fn = cmd_find("cycle");
	call("cycle again");
	if (fn) fn();
	rec_state();

	fn = cmd_find("cycleback");
	call("cycleback steps the other way");
	argc_set(4, "cycleback", "cv", "a", "b");
	if (fn) fn();
	rec_state();

	fn = cmd_find("cycle");
	call("cycle on an unknown cvar");
	argc_set(3, "cycle", "nosuch", "a", NULL);
	if (fn) fn();
	rec_state();

	fn = cmd_find("inc");
	call("inc with no amount");
	argc_set(2, "inc", "other", NULL, NULL);
	if (fn) fn();
	rec_state();

	fn = cmd_find("inc");
	call("inc with an amount");
	argc_set(3, "inc", "other", "2.5", NULL);
	if (fn) fn();
	rec_state();

	fn = cmd_find("inc");
	call("inc with the wrong argument count");
	argc_set(1, "inc", NULL, NULL, NULL);
	if (fn) fn();
	rec_state();

	fn = cmd_find("toggle");
	call("toggle a non-zero cvar");
	argc_set(2, "toggle", "other", NULL, NULL);
	if (fn) fn();
	rec_state();

	fn = cmd_find("toggle");
	call("toggle a zero cvar");
	if (fn) fn();
	rec_state();

	fn = cmd_find("toggle");
	call("toggle with the wrong argument count");
	argc_set(1, "toggle", NULL, NULL, NULL);
	if (fn) fn();
	rec_state();

	fn = cmd_find("reset");
	call("reset a cvar");
	cur->Set("cv", "changed");
	argc_set(2, "reset", "cv", NULL, NULL);
	if (fn) fn();
	rec_state();

	fn = cmd_find("resetall");
	call("resetall");
	cur->Set("cv", "changed");
	cur->Set("other", "5");
	if (fn) fn();
	rec_state();

	fn = cmd_find("resetcfg");
	call("resetcfg");
	cur->Set("cv", "changed");
	cur->Set("other", "5");
	if (fn) fn();
	rec_state();

	// The names Cvar_Init registers, in order.
	call("the registered command names");
	{
		int i;
		for (i = 0; i < cmd_n; i++)
			rec_str(cmds[i].name);
	}
	rec_state();
	rec_counters();
}

// Re-entrancy: a callback that sets another cvar, one that sets the cvar being
// set, and an alias whose target's callback is chained.  The no-change return
// above the callback is what makes the self-set case terminate.
static cvar_t *re_other;

static void cb_set_other(cvar_t *var)
{
	(void)var;
	cb_record(var);
	if (re_other)
		cur->Set("other", "from-callback");
}

static void cb_set_self(cvar_t *var)
{
	cb_record(var);
	cur->Set("self", "again");
}

static void scenario_reentrancy(void)
{
	node_init(0, "self", "0", CVAR_ARCHIVE);
	node_init(1, "other", "0", CVAR_ARCHIVE);
	node_init(2, "target", "0", CVAR_ARCHIVE);
	node_init(3, "alias", "0", 0);

	cur->RegisterVariable(&nodes[0]);
	cur->RegisterVariable(&nodes[1]);
	cur->RegisterVariable(&nodes[2]);
	cur->SetCallback(&nodes[1], cb_set_other);
	re_other = &nodes[1];
	cur->SetCallback(&nodes[0], cb_set_self);
	cur->SetCallback(&nodes[2], cb_record);
	cur->RegisterAlias(&nodes[3], &nodes[2]);
	rec_state();

	call("set the cvar whose callback sets another");
	cur->Set("other", "1");
	rec_state();

	call("set the cvar whose callback sets itself");
	cur->Set("self", "1");
	rec_state();

	call("set through the alias of a callback-bearing target");
	cur->Set("alias", "1");
	rec_state();

	call("set the target of the alias");
	cur->Set("target", "2");
	rec_state();

	rec_counters();
}

/*----------------------------------------------------------------------------
 * case runner
 *--------------------------------------------------------------------------*/

// Each case runs in a child process, for each implementation separately.
//
// cvar.c's list head and alias table are `static` in the C original and in the
// Rust module, so a case that registered a cvar leaves the next case's lookup
// walking nodes this harness has since reused -- the C's Cvar_FindVar would
// strcmp against a node whose name pointer no longer describes it.  A fresh
// process is the only way to start each implementation from the state the
// engine starts in, and it also turns a crash into one reported case rather
// than a dead gate.  The trace comes back through a file because the child's
// memory is not visible to the parent.

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

struct child_result {
	size_t len;
	int checks;
	int failures;
};

// The comparison is over the traces rather than over a list of assertions, so
// the summary reports both: how many literal expectations were checked and how
// many bytes of observable state were compared.  The gate floors both.
static size_t trace_bytes;
static int cases_run;

static int run_impl(int idx, void (*scenario)(void), unsigned char *out,
		size_t *outlen, int *out_checks, int *out_failures)
{
	char path[] = "/tmp/cvar-harness-XXXXXX";
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
		// The parent's totals are inherited by the fork; zeroing them here
		// keeps what comes back the child's own count rather than a running
		// total the parent would add a second time.
		checks = 0;
		failures = 0;
		scenario_reset();
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

static void run_case(const char *what, void (*scenario)(void))
{
	static unsigned char out[IMPL_COUNT][TRACE_MAX];
	static size_t len[IMPL_COUNT];
	int checks_seen[IMPL_COUNT];
	int failures_seen[IMPL_COUNT];
	int i, bad = 0;
	char line[256];

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
		// Child-side check() counts are the harness's own expectations, so
		// they are folded back into the totals.
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
		snprintf(line, sizeof line,
			"FAIL: %s: %s and %s produced different traces\n",
			what, impls[IMPL_C].name, impls[i].name);
		fputs(line, stderr);
		for (k = 0; k < len[IMPL_C] && k < len[i]; ++k)
			if (out[IMPL_C][k] != out[i][k])
				break;
		fprintf(stderr, "      first difference at trace byte %zu "
			"(0x%02x vs 0x%02x), lengths %zu vs %zu\n", k,
			k < len[IMPL_C] ? out[IMPL_C][k] : 0,
			k < len[i] ? out[i][k] : 0, len[IMPL_C], len[i]);
	}
}

int main(void)
{
	printf("cvar differential harness: the C original as glhexen2 compiles it "
		"and the Rust module\n");

	layout_check();

	run_case("register", scenario_register);
	run_case("set", scenario_set);
	run_case("values", scenario_values);
	run_case("rom and locked", scenario_rom_locked);
	run_case("reads", scenario_reads);
	run_case("order", scenario_order);
	run_case("config", scenario_config);
	run_case("aliases", scenario_aliases);
	run_case("alias limit", scenario_alias_limit);
	run_case("commands", scenario_commands);
	run_case("re-entrancy", scenario_reentrancy);

	printf("checked %d expectations, %d failures, %d cases, %zu trace bytes\n",
		checks, failures, cases_run, trace_bytes);
	if (failures) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
