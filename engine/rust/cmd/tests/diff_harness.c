// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for the Rust port of engine/h2shared/cmd.c.
//
// The C original is compiled with every exported name renamed (c_Cmd_*) and
// linked beside the Rust module in one binary, and the two are driven through
// one table of function pointers.  After every call the whole observable state
// is recorded into a byte trace and the two traces are compared byte for byte.
// A golden-only test would pass for two implementations that are both wrong;
// this compares C against Rust.
//
// One binary per target arm, because cmd.c really does differ per target: the
// client-only listing surface exists only where SERVERONLY is not defined, and
// the H2W dispatch arm only in the hwsv build.  The same defines are applied to
// the C original, to this harness and to engine/rust/cmd_target.c, so the Rust
// module's per-target predicates answer exactly what the C compiled.
//
// What is deliberately total about the recording:
//
//   * the tokenizer's output -- argc, every argv byte, and each token's offset
//     inside the zone buffer, so a port that allocates or frees at a different
//     point is visible even when the bytes agree;
//   * the hunk low mark, which is where the command registry's nodes and the
//     command buffer's storage come from;
//   * the cvar list, which Cmd_Init registers into and Cmd_CheckCommand walks;
//   * answers from Cmd_Exists / Cmd_CheckCommand / Cmd_AliasExists over a fixed
//     probe list, which is how the registry and the alias table are observed in
//     the arms where the listing surface does not exist;
//   * every diagnostic, because most of cmd.c's failure paths are prints rather
//     than aborts.
//
// Each case runs in a child process per implementation: cmd.c's registry, alias
// table and tokenizer state are static in both implementations with no exported
// global to reset, and a child also turns a crash into one reported case rather
// than a dead gate.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "quakedef.h"
#include "cmd.h"
#include "cvar.h"

#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*----------------------------------------------------------------------------
 * the two implementations
 *--------------------------------------------------------------------------*/

#define DECLARE_CMD_IMPL(P) \
	extern void P##Cbuf_Init (void); \
	extern void P##Cbuf_Clear (void); \
	extern void P##Cbuf_AddText (const char *text); \
	extern void P##Cbuf_InsertText (const char *text); \
	extern void P##Cbuf_Execute (void); \
	extern void P##Cmd_Init (void); \
	extern void P##Cmd_AddCommand (const char *cmd_name, xcommand_t function); \
	extern qboolean P##Cmd_Exists (const char *cmd_name); \
	extern qboolean P##Cmd_AliasExists (const char *alias_name); \
	extern qboolean P##Cmd_CheckCommand (const char *partial); \
	extern void P##Cmd_MoveToFront (const char *cmd_name); \
	extern int P##Cmd_Argc (void); \
	extern const char *P##Cmd_Argv (int arg); \
	extern const char *P##Cmd_Args (void); \
	extern int P##Cmd_CheckParm (const char *parm); \
	extern void P##Cmd_TokenizeString (const char *text); \
	extern void P##Cmd_ExecuteString (const char *text, cmd_source_t src); \
	extern void P##Cmd_StuffCmds_f (void); \
	extern const char *P##Cmd_StartupScript (void); \
	extern void P##Cmd_Unalias_f (void); \
	extern void P##Cmd_Unaliasall_f (void); \
	extern int P##ListCommands (const char *prefix, const char **buf, int pos); \
	extern int P##ListCvars (const char *prefix, const char **buf, int pos); \
	extern int P##ListAlias (const char *prefix, const char **buf, int pos); \
	extern cmd_source_t P##cmd_source

DECLARE_CMD_IMPL(c_);
DECLARE_CMD_IMPL();

/* The Rust layout accessors, so the harness can check the structs it compiles
 * against without restating Rust's offsets in C. */
extern size_t CmdFunctionC_sizeof(void);
extern size_t CmdFunctionC_alignof(void);
extern size_t CmdFunctionC_offsetof_next(void);
extern size_t CmdFunctionC_offsetof_name(void);
extern size_t CmdFunctionC_offsetof_function(void);
extern size_t CmdAliasC_sizeof(void);
extern size_t CmdAliasC_alignof(void);
extern size_t CmdAliasC_offsetof_next(void);
extern size_t CmdAliasC_offsetof_name(void);
extern size_t CmdAliasC_offsetof_value(void);
extern int Cmd_MAX_ARGS(void);
extern int Cmd_MAX_ALIAS_NAME(void);

/* cmd_function_t and cmdalias_t are private to cmd.c, so the harness spells
 * out copies and compares them with Rust's view (and with abi_layout.c). */
typedef struct abi_cmd_function_s {
	struct abi_cmd_function_s *next;
	const char *name;
	xcommand_t function;
} abi_cmd_function_t;

typedef struct abi_cmd_alias_s {
	struct abi_cmd_alias_s *next;
	char name[32];
	char *value;
} abi_cmd_alias_t;

typedef struct cmd_impl_s {
	const char *name;
	void (*Cbuf_Init)(void);
	void (*Cbuf_Clear)(void);
	void (*Cbuf_AddText)(const char *);
	void (*Cbuf_InsertText)(const char *);
	void (*Cbuf_Execute)(void);
	void (*Init)(void);
	void (*AddCommand)(const char *, xcommand_t);
	qboolean (*Exists)(const char *);
	qboolean (*AliasExists)(const char *);
	qboolean (*CheckCommand)(const char *);
	void (*MoveToFront)(const char *);
	int (*Argc)(void);
	const char *(*Argv)(int);
	const char *(*Args)(void);
	int (*CheckParm)(const char *);
	void (*TokenizeString)(const char *);
	void (*ExecuteString)(const char *, cmd_source_t);
	void (*StuffCmds_f)(void);
	const char *(*StartupScript)(void);
	void (*Unalias_f)(void);
	void (*Unaliasall_f)(void);
	int (*ListCommands)(const char *, const char **, int);
	int (*ListCvars)(const char *, const char **, int);
	int (*ListAlias)(const char *, const char **, int);
	cmd_source_t *source;
} cmd_impl_t;

enum { IMPL_C = 0, IMPL_RUST = 1, IMPL_COUNT = 2 };

// The C's listing surface is compiled only outside SERVERONLY, so the C row
// points at nothing in that arm; the Rust module keeps the functions in every
// arm (unreachable there, as the C's symbols are in the targets that do not
// call them) and the serveronly arm's cases never call them.
#ifdef SERVERONLY
#define C_LIST_FNS NULL, NULL, NULL
#else
#define C_LIST_FNS c_ListCommands, c_ListCvars, c_ListAlias
#endif

static const cmd_impl_t impls[IMPL_COUNT] = {
	{
		"C",
		c_Cbuf_Init, c_Cbuf_Clear, c_Cbuf_AddText, c_Cbuf_InsertText,
		c_Cbuf_Execute, c_Cmd_Init, c_Cmd_AddCommand, c_Cmd_Exists,
		c_Cmd_AliasExists, c_Cmd_CheckCommand, c_Cmd_MoveToFront, c_Cmd_Argc,
		c_Cmd_Argv, c_Cmd_Args, c_Cmd_CheckParm, c_Cmd_TokenizeString,
		c_Cmd_ExecuteString, c_Cmd_StuffCmds_f, c_Cmd_StartupScript,
		c_Cmd_Unalias_f, c_Cmd_Unaliasall_f, C_LIST_FNS,
		&c_cmd_source,
	},
	{
		"Rust",
		Cbuf_Init, Cbuf_Clear, Cbuf_AddText, Cbuf_InsertText,
		Cbuf_Execute, Cmd_Init, Cmd_AddCommand, Cmd_Exists,
		Cmd_AliasExists, Cmd_CheckCommand, Cmd_MoveToFront, Cmd_Argc,
		Cmd_Argv, Cmd_Args, Cmd_CheckParm, Cmd_TokenizeString,
		Cmd_ExecuteString, Cmd_StuffCmds_f, Cmd_StartupScript,
		Cmd_Unalias_f, Cmd_Unaliasall_f, ListCommands, ListCvars,
		ListAlias, &cmd_source,
	},
};

static const cmd_impl_t *cur;

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
 * the zone the port allocates from
 *--------------------------------------------------------------------------*/

// The Rust zone port owns the allocator, so the harness owns only the buffer
// and asks Memory_Init for a fresh zone in each child.  Recording offsets
// inside it is what makes an allocation-order difference visible.

#define ZONE_SIZE (8 * 1024 * 1024)

static byte zone_buf[ZONE_SIZE];

extern void Memory_Init(void *buf, int size);
extern int Hunk_LowMark(void);

static int zone_off(const void *p)
{
	const byte *b = (const byte *)p;

	if (b >= zone_buf && b < zone_buf + ZONE_SIZE)
		return (int)(b - zone_buf);
	return -1;
}

/*----------------------------------------------------------------------------
 * the engine the port reaches
 *--------------------------------------------------------------------------*/

static char printf_buf[1024];

// The C original is linked under renamed symbols, so its diagnostics name
// themselves "c_Cmd_Set" where the shipping build says "Cmd_Set".  That is a
// harness artifact, so it is removed here.
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

static jmp_buf err_env;
static int err_armed;
static int err_calls;
static char err_buf[256];

FUNC_NORETURN void Sys_Error(const char *fmt, ...)
{
	va_list ap;

	err_calls++;
	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);
	normalize_diag();

	rec_int(0xE44E);
	rec_str(err_buf);

	if (!err_armed) {
		fprintf(stderr, "harness bug: Sys_Error outside a guarded call: %s\n",
			err_buf);
		abort();
	}

	longjmp(err_env, 1);
}

// COM_Parse, com_token, q_strcasecmp and q_strncasecmp are the engine's
// (engine/h2shared/common.c), copied here with their helpers so both
// implementations parse and compare identically.  The alternative is linking
// the whole of common.c, which reaches the filesystem and the host layer.
char com_token[1024];

const char *COM_Parse(const char *data)
{
	int c;
	int len;

	len = 0;
	com_token[0] = 0;

	if (!data)
		return NULL;

skipwhite:
	while ((c = *data) <= ' ') {
		if (c == 0)
			return NULL;	// end of file
		data++;
	}

	// skip // comments
	if (c == '/' && data[1] == '/') {
		while (*data && *data != '\n')
			data++;
		goto skipwhite;
	}

	// skip /*..*/ comments
	if (c == '/' && data[1] == '*') {
		data += 2;
		while (*data && !(*data == '*' && data[1] == '/'))
			data++;
		if (*data)
			data += 2;
		goto skipwhite;
	}

	// handle quoted strings specially
	if (c == '\"') {
		data++;
		while (1) {
			if ((c = *data) != 0)
				++data;
			if (c == '\"' || !c) {
				com_token[len] = 0;
				return data;
			}
			if (len < (int)sizeof(com_token) - 1)
				com_token[len++] = (char)c;
		}
	}

	// parse a regular word
	do {
		if (len < (int)sizeof(com_token) - 1)
			com_token[len++] = (char)c;
		data++;
		c = *data;
	} while (c > ' ');

	com_token[len] = 0;
	return data;
}

static char q_tolower_c(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int q_strcasecmp(const char *s1, const char *s2)
{
	const char *p1 = s1;
	const char *p2 = s2;
	char c1, c2;

	if (p1 == p2)
		return 0;

	do {
		c1 = q_tolower_c(*p1++);
		c2 = q_tolower_c(*p2++);
		if (c1 == '\0')
			break;
	} while (c1 == c2);

	return (int)(c1 - c2);
}

int q_strncasecmp(const char *s1, const char *s2, size_t n)
{
	int c1, c2;

	do {
		c1 = *s1++;
		c2 = *s2++;

		if (!n--)
			return 0;		/* strings are equal until end point */

		if (c1 != c2) {
			c1 = q_tolower_c((char)c1);
			c2 = q_tolower_c((char)c2);
			if (c1 != c2)
				return c1 < c2 ? -1 : 1;
		}
	} while (c1);
	return 0;			/* strings are equal */
}

// The filesystem entry points quakefs.c owns until Phase 6 slice 4.  The
// harness controls what they return so Cmd_Exec_f and Cmd_StartupScript can be
// driven without a real gamedir.
static char fs_path_buf[256];
static const char *fs_hunk_text;	// what FS_LoadHunkFile returns
static int fs_file_exists;

byte *FS_LoadHunkFile(const char *path, unsigned int *path_id)
{
	(void)path_id;
	if (fs_hunk_text && !strcmp(path, "test.cfg"))
		return (byte *)fs_hunk_text;
	return NULL;
}

char *FS_MakePath(int base, int *error, const char *path)
{
	(void)base;
	if (error)
		*error = 0;
	snprintf(fs_path_buf, sizeof fs_path_buf, "/tmp/%s", path);
	return fs_path_buf;
}

qboolean FS_FileExists(const char *filename, unsigned int *path_id)
{
	(void)filename;
	(void)path_id;
	return fs_file_exists;
}

/*----------------------------------------------------------------------------
 * the cvar substrate
 *--------------------------------------------------------------------------*/

// The archive is built without the cvar feature -- cmd.rs needs only cvar_t's
// layout from it -- so the harness supplies the cvar functions both arms call.
// Cvar_Init registers its console commands through the *unrenamed*
// Cmd_AddCommand, which is the port under test, so it must be a no-op here:
// otherwise the C arm would register those commands into the Rust registry
// while the Rust arm registered them into its own, and the two registries would
// differ for a reason that has nothing to do with cmd.c.

static cvar_t *cvar_head;

static cvar_t *find_var(const char *name);

void Cvar_Init(void) {}

void Cvar_RegisterVariable(cvar_t *variable)
{
	const char *value = variable->string ? variable->string : "";

	// As cvar.c does: a name that is already registered is refused, so a
	// second Cmd_Init cannot link the same node to itself.
	if (find_var(variable->name))
		return;

	// As cvar.c does: copy the value off, keep it as the default, then set it
	// back through the same path so value/integer are derived from the string.
	variable->default_string = strdup(value);
	variable->string = strdup(value);
	variable->value = (float)atof(value);
	variable->integer = (int)variable->value;
	variable->flags |= (1u << 10);	/* CVAR_REGISTERED */
	variable->next = cvar_head;
	cvar_head = variable;
}

static cvar_t *find_var(const char *name)
{
	cvar_t *v;

	for (v = cvar_head; v; v = v->next)
		if (!strcmp(name, v->name))
			return v;
	return NULL;
}

const char *Cvar_VariableString(const char *var_name)
{
	cvar_t *v = find_var(var_name);

	return v ? v->string : "";
}

cvar_t *Cvar_FindVarAfter(const char *prev_name, unsigned int with_flags)
{
	cvar_t *v;

	if (*prev_name) {
		v = find_var(prev_name);
		if (!v)
			return NULL;
		v = v->next;
	} else {
		v = cvar_head;
	}
	while (v) {
		if ((v->flags & with_flags) || !with_flags)
			break;
		v = v->next;
	}
	return v;
}

qboolean Cvar_Command(void)
{
	return false;
}

static void cvar_reset(void)
{
	cvar_head = NULL;
}

/*----------------------------------------------------------------------------
 * the host parameters and the shim predicates
 *--------------------------------------------------------------------------*/

// quakeparms_t: com_argc/com_argv are macros over host_parms, so the harness
// owns the struct the port reads.
static quakeparms_t parms;

// The port reads host_parms->argv for as long as it runs, so the arrays it
// points at outlive the scenario block that fills them.
static char *argv_no_plus[3];
static char *argv_plus[6];

// The engine's host_parms lives in host.c; the harness owns it here, and the
// ports read com_argc/com_argv through it.
quakeparms_t *host_parms = &parms;

// The zone port's four per-target values are the zone gate's business, not
// this one's; the cmd harness answers them itself so that both arms see the
// same zone.  HasCache is 0 on purpose: with it set, the zone port's
// Memory_Init registers its "flush" command through the *unrenamed*
// Cmd_AddCommand -- the Rust registry, in both arms -- and the C registry
// could never have the matching entry, which would show up in every listing
// case for a reason that has nothing to do with cmd.c.  The same reasoning is
// why the cvar substrate's Cvar_Init registers nothing.
int Zone_TargetDefSize(void) { return 0x200000; }
int Zone_TargetSecSize(void) { return 0x40000; }
int Zone_TargetHasCache(void) { return 0; }
int Zone_TargetDedicated(void) { return 0; }

// COM_CheckParm (engine/h2shared/common.c) over the same host_parms, so the
// zone port's -zone parsing sees the harness's command line.
int COM_CheckParm(const char *parm)
{
	int i;

	for (i = 1; i < host_parms->argc; i++)
		if (host_parms->argv[i] && !q_strcasecmp(parm, host_parms->argv[i]))
			return i;
	return 0;
}

// Cmd_AddCommand refuses once this is set; the engine defines it, and the port
// reads it as an extern, so the harness owns it here.  qboolean rather than int
// because that is how host.h declares it (qboolean is an enum in this TU).
qboolean host_initialized;

/* The per-target predicates come from engine/rust/cmd_target.c, compiled with
 * the arm's defines by run_diff_harness.sh; they are declared in cmd.h terms
 * here so the harness never needs its own copy. */
extern int Cmd_TargetHasClientLists(void);
extern int Cmd_TargetIsH2W(void);
extern int Cmd_TargetHasBuiltinStartupScript(void);

/*----------------------------------------------------------------------------
 * state recording
 *--------------------------------------------------------------------------*/

static int cmd_initialized;

static void rec_state(void)
{
	int i;
	cvar_t *var;


	// the tokenizer's public view
	rec_int(cur->Argc());
	for (i = 0; i < cur->Argc(); i++) {
		const char *a = cur->Argv(i);
		rec_str(a);
		rec_int(zone_off(a));
	}
	// Cmd_Args is NOT recorded here: it points into the caller's text, so after
	// the call has returned the bytes behind it belong to a dead stack frame.
	// The tokenize scenario records it while its own buffer is alive.

	// where the hunk stands: command nodes and the command buffer come from it
	rec_int(Hunk_LowMark());

	// the cvar list, which Cmd_Init registers into
	var = Cvar_FindVarAfter("", 0);
	while (var) {
		rec_str(var->name);
		rec_str(var->string ? var->string : "(null)");
		rec_int((int)var->flags);
		var = var->next;
	}
	rec_int(-1);

	// registry and alias probes that every arm can answer
	{
		static const char *probe[] = {
			"stuffcmds", "exec", "echo", "alias", "unalias",
			"unaliasall", "wait", "commands", "cmdlist", "cvarlist",
			"aliaslist", "apropos", "find", "notacommand", NULL,
		};
		for (i = 0; probe[i]; i++) {
			rec_int(cur->Exists(probe[i]) ? 1 : 0);
			rec_int(cur->CheckCommand(probe[i]) ? 1 : 0);
			rec_int(cur->AliasExists(probe[i]) ? 1 : 0);
		}
	}

	rec_int((int)*cur->source);
}

static void cvar_reset(void);

static void op_reset(void)
{
	cvar_reset();
	err_calls = 0;
	err_armed = 0;
	err_buf[0] = 0;
	printf_buf[0] = 0;
	fs_hunk_text = NULL;
	fs_file_exists = 0;
	cmd_initialized = 0;
}

static void scenario_reset(void)
{
	op_reset();
	memset(zone_buf, 0xee, sizeof zone_buf);
	Memory_Init(zone_buf, ZONE_SIZE);
}

/*----------------------------------------------------------------------------
 * helpers the scenarios speak in
 *--------------------------------------------------------------------------*/

static void call(const char *what)
{
	rec_int(0x0CA11);
	rec_str(what);
}

// A guarded call: Sys_Error unwinds here instead of ending the run.
static void guarded(const char *what, void (*fn)(void))
{
	call(what);
	err_armed = 1;
	err_calls = 0;
	if (setjmp(err_env) == 0)
		fn();
	err_armed = 0;
	rec_int(err_calls);
	if (err_calls)
		rec_str(err_buf);
}

static void tokenize(const char *text)
{
	call("tokenize");
	cur->TokenizeString(text);
	// Here the harness's own literal is still alive, so Cmd_Args is readable.
	rec_str(cur->Args());
	rec_state();
}

static void echo_cmd(void) { CON_Printf(0, "echo-ran\n"); }

/*----------------------------------------------------------------------------
 * scenarios
 *--------------------------------------------------------------------------*/

static void layout_check(void)
{
	check(CmdFunctionC_sizeof() == sizeof(abi_cmd_function_t),
		"CmdFunctionC_sizeof: Rust %zu, C %zu", CmdFunctionC_sizeof(),
		sizeof(abi_cmd_function_t));
	check(CmdFunctionC_alignof() == _Alignof(abi_cmd_function_t),
		"CmdFunctionC_alignof: Rust %zu, C %zu", CmdFunctionC_alignof(),
		_Alignof(abi_cmd_function_t));
	check(CmdFunctionC_offsetof_next() == offsetof(abi_cmd_function_t, next),
		"offsetof(next): Rust %zu, C %zu", CmdFunctionC_offsetof_next(),
		offsetof(abi_cmd_function_t, next));
	check(CmdFunctionC_offsetof_name() == offsetof(abi_cmd_function_t, name),
		"offsetof(name): Rust %zu, C %zu", CmdFunctionC_offsetof_name(),
		offsetof(abi_cmd_function_t, name));
	check(CmdFunctionC_offsetof_function()
			== offsetof(abi_cmd_function_t, function),
		"offsetof(function): Rust %zu, C %zu",
		CmdFunctionC_offsetof_function(),
		offsetof(abi_cmd_function_t, function));

	check(CmdAliasC_sizeof() == sizeof(abi_cmd_alias_t),
		"CmdAliasC_sizeof: Rust %zu, C %zu", CmdAliasC_sizeof(),
		sizeof(abi_cmd_alias_t));
	check(CmdAliasC_alignof() == _Alignof(abi_cmd_alias_t),
		"CmdAliasC_alignof: Rust %zu, C %zu", CmdAliasC_alignof(),
		_Alignof(abi_cmd_alias_t));
	check(CmdAliasC_offsetof_next() == offsetof(abi_cmd_alias_t, next),
		"alias offsetof(next): Rust %zu, C %zu", CmdAliasC_offsetof_next(),
		offsetof(abi_cmd_alias_t, next));
	check(CmdAliasC_offsetof_name() == offsetof(abi_cmd_alias_t, name),
		"alias offsetof(name): Rust %zu, C %zu", CmdAliasC_offsetof_name(),
		offsetof(abi_cmd_alias_t, name));
	check(CmdAliasC_offsetof_value() == offsetof(abi_cmd_alias_t, value),
		"alias offsetof(value): Rust %zu, C %zu", CmdAliasC_offsetof_value(),
		offsetof(abi_cmd_alias_t, value));

	check(Cmd_MAX_ARGS() == 80, "Cmd_MAX_ARGS: Rust %d, C 80", Cmd_MAX_ARGS());
	check(Cmd_MAX_ALIAS_NAME() == 32, "Cmd_MAX_ALIAS_NAME: Rust %d, C 32",
		Cmd_MAX_ALIAS_NAME());
}

// Cmd_Init: the registry it builds, the engine-defaults cvar it registers, and
// the target-dependent registrations.
static void scenario_init(void)
{
	guarded("Cmd_Init", cur->Init);
	rec_state();

	// Where the command buffer's storage comes from is observable through the
	// hunk, so Cbuf_Init is part of the same picture.
	guarded("Cbuf_Init", cur->Cbuf_Init);
	rec_state();

	guarded("Cmd_Init again", cur->Init);
	rec_state();
}

// The tokenizer: whitespace, quotes, comments, newlines, the MAX_ARGS clamp.
static void scenario_tokenize(void)
{
	static const char *cases[] = {
		"",
		" ",
		"one",
		"one two three",
		"   spaced\ttabs  ",
		"\"quoted string\" tail",
		"a // comment\nb",
		"a /* block */ b",
		"\nsecond",
		"cmd arg1;arg2",
		"\"unterminated",
		"caret\"with space\"",
		NULL,
	};
	int i;

	for (i = 0; cases[i]; i++) {
		tokenize(cases[i]);
	}

	// the MAX_ARGS clamp: 82 tokens in, 80 out
	{
		char big[1024];
		int n = 0;

		big[0] = 0;
		for (i = 0; i < 82; i++) {
			n += snprintf(big + n, sizeof big - (size_t)n, "t%d ", i);
		}
		tokenize(big);
	}
}

// Cbuf: append, insert, execute, the quoted-semicolon rule and cmd_wait.
static void scenario_cbuf(void)
{
	guarded("Cbuf_Init", cur->Cbuf_Init);

	call("AddText one");
	cur->Cbuf_AddText("echo a\n");
	rec_state();

	call("AddText with semicolons");
	cur->Cbuf_AddText("echo b;echo c\n");
	rec_state();

	call("AddText quoted semicolon");
	cur->Cbuf_AddText("echo \"d;e\"\n");
	rec_state();

	call("Execute");
	cur->Cbuf_Execute();
	rec_state();

	call("InsertText");
	cur->Cbuf_InsertText("echo f\n");
	rec_state();

	call("Execute again");
	cur->Cbuf_Execute();
	rec_state();

	// wait leaves the rest of the buffer for the next call
	call("AddText with wait");
	cur->Cbuf_AddText("wait\necho g\n");
	rec_state();
	call("Execute (stops at wait)");
	cur->Cbuf_Execute();
	rec_state();
	call("Execute (drains the rest)");
	cur->Cbuf_Execute();
	rec_state();

	call("Clear");
	cur->Cbuf_Clear();
	rec_state();
}

// Aliases: creation, reuse, removal, the length limits, and what the alias
// actually runs.
static void scenario_aliases(void)
{
	char longname[64];
	int i;

	guarded("Cbuf_Init", cur->Cbuf_Init);
	guarded("Cmd_Init", cur->Init);

	for (i = 0; i < 40; i++)
		longname[i] = 'x';
	longname[40] = 0;

	call("alias with no args");
	cur->ExecuteString("alias", src_command);
	rec_state();

	call("define an alias");
	cur->ExecuteString("alias greet echo hello", src_command);
	rec_state();

	call("query it by name");
	cur->ExecuteString("alias greet", src_command);
	rec_state();

	call("redefine it");
	cur->ExecuteString("alias greet echo goodbye", src_command);
	rec_state();

	call("run it");
	cur->ExecuteString("greet", src_command);
	rec_state();

	call("drain the buffer");
	cur->Cbuf_Execute();
	rec_state();

	call("query an unknown alias");
	cur->ExecuteString("alias nosuch", src_command);
	rec_state();

	call("an over-long name");
	{
		char line[128];

		snprintf(line, sizeof line, "alias %s echo x", longname);
		cur->ExecuteString(line, src_command);
	}
	rec_state();

	call("unalias");
	cur->ExecuteString("unalias greet", src_command);
	rec_state();

	call("unalias an unknown alias");
	cur->ExecuteString("unalias nosuch", src_command);
	rec_state();

	call("Unaliasall with an empty table");
	cur->Unaliasall_f();
	rec_state();

	call("two aliases then unaliasall");
	cur->ExecuteString("alias a1 echo one", src_command);
	cur->ExecuteString("alias a2 echo two", src_command);
	cur->Unaliasall_f();
	rec_state();
}

// Dispatch: a registered command, an alias, a cvar, a legacy name, an unknown
// name, and the argv a command sees.
static void scenario_dispatch(void)
{
	guarded("Cbuf_Init", cur->Cbuf_Init);
	guarded("Cmd_Init", cur->Init);

	call("register a command");
	cur->AddCommand("harnesscmd", echo_cmd);
	rec_state();

	call("run it");
	cur->ExecuteString("harnesscmd arg1 arg2", src_command);
	rec_state();

	call("run the same command in mixed case");
	cur->ExecuteString("HarnessCmd", src_command);
	rec_state();

	call("register a duplicate");
	cur->AddCommand("harnesscmd", echo_cmd);
	rec_state();

	call("register a command named after a cvar");
	cur->AddCommand("cfg_enginedefaults", echo_cmd);
	rec_state();

	/* Every name in the C's legacy_cmds[] (cmd.c:865-871), not just the first:
	 * a name the port drops shows up here as "Unknown command" where the C
	 * stays silent, and a name it invents is caught by the gate's comparison
	 * of the two lists.  Probing one name only is what let an invented entry
	 * through once. */
	{
		static const char *const legacy[] = {
			"gl_ztrick", "gl_max_size", "sys_delay", "r_transwater",
			"_windowed_mouse", "vid_stretch_by_2", "vid_config_y",
			"vid_config_x", "_vid_default_mode_win",
			"_vid_default_mode", "_vid_wait_override",
			"vid_nopageflip", "sys_quake2",
		};
		unsigned int li;

		for (li = 0; li < sizeof legacy / sizeof legacy[0]; li++) {
			char legacy_line[64];

			snprintf(legacy_line, sizeof legacy_line, "%s 1", legacy[li]);
			call(legacy_line);
			cur->ExecuteString(legacy_line, src_command);
			rec_state();
		}
	}

	call("an unknown command");
	cur->ExecuteString("nosuchcommand x", src_command);
	rec_state();

	call("empty input");
	cur->ExecuteString("", src_command);
	rec_state();

	call("checkparm");
	cur->ExecuteString("harnesscmd -width 320 +height 200", src_command);
	rec_int(cur->CheckParm("-width"));
	rec_int(cur->CheckParm("+height"));
	rec_int(cur->CheckParm("-nosuch"));
	rec_state();

	call("move to front");
	cur->MoveToFront("harnesscmd");
	rec_state();
	cur->MoveToFront("nosuchcmd");
	rec_state();
}

// The startup script and stuffcmds, both of which read things the harness
// controls.
static void scenario_startup(void)
{
	guarded("Cmd_Init", cur->Init);

	call("startup script");
	rec_str(cur->StartupScript());
	rec_state();

	call("startup script with hexen.rc present");
	fs_file_exists = 1;
	rec_str(cur->StartupScript());
	rec_state();

	call("stuffcmds with no +commands");
	{
		char *argv[3];

		argv[0] = (char *)"hexenwail";
		argv[1] = (char *)"-width";
		argv[2] = (char *)"320";
		host_parms->argc = 3;
		host_parms->argv = argv;
	}

	guarded("stuffcmds", cur->StuffCmds_f);
	rec_state();

	call("stuffcmds with +commands");
	argv_plus[0] = (char *)"hexenwail";
	argv_plus[1] = (char *)"+map";
	argv_plus[2] = (char *)"demo1";
	argv_plus[3] = (char *)"+skill";
	argv_plus[4] = (char *)"3";
	argv_plus[5] = (char *)"-nosound";
	host_parms->argc = 6;
	host_parms->argv = argv_plus;
	guarded("stuffcmds again (one shot)", cur->StuffCmds_f);
	rec_state();
}

// A command registered with a NULL handler.  In hwsv the C prints the H2W
// diagnostic; in the other arms it calls through NULL, which is why this case
// only runs in the H2W arm.
#ifdef H2W
// A NULL-handler command: hwsv's arm prints the diagnostic.  The other arms
// call through NULL -- the C crashes -- so this case exists only here.
static void scenario_null_handler(void)
{
	guarded("Cmd_Init", cur->Init);

	call("register a NULL-handler command");
	cur->AddCommand("forwarded", NULL);
	rec_state();

	call("run it");
	cur->ExecuteString("forwarded arg", src_command);
	rec_state();
}
#endif

// Cmd_AddCommand after host_initialized aborts, and the diagnostic is the
// whole observable: the port must print the C's sentence, not its own.
static void add_late(void)
{
	cur->AddCommand("latecommand", echo_cmd);
}

static void scenario_after_init(void)
{
	guarded("Cmd_Init", cur->Init);
	call("register while host_initialized is clear");
	cur->AddCommand("earlycommand", echo_cmd);
	rec_state();

	host_initialized = 1;
	guarded("add after host_initialized", add_late);
	rec_state();
	host_initialized = 0;

	call("add again once it is clear");
	cur->AddCommand("latecommand", echo_cmd);
	rec_state();
}

/*----------------------------------------------------------------------------
 * the client-only listing surface
 *--------------------------------------------------------------------------*/

#ifndef SERVERONLY
static void scenario_lists(void)
{
	const char *buf[8];
	int n, i;

	guarded("Cmd_Init", cur->Init);
	cur->AddCommand("harnesscmd", echo_cmd);
	cur->ExecuteString("alias ha echo hello", src_command);

	call("ListCommands no prefix");
	n = cur->ListCommands(NULL, NULL, 0);
	rec_int(n);
	rec_state();

	call("ListCommands prefix");
	n = cur->ListCommands("cmd", NULL, 0);
	rec_int(n);
	rec_state();

	call("ListCommands into a buffer");
	memset((void *)buf, 0, sizeof buf);
	n = cur->ListCommands("cmd", buf, 0);
	rec_int(n);
	for (i = 0; i < n && i < 8; i++)
		rec_str(buf[i]);
	rec_state();

	call("ListCvars no prefix");
	n = cur->ListCvars(NULL, NULL, 0);
	rec_int(n);
	rec_state();

	call("ListCvars prefix");
	n = cur->ListCvars("cfg_", NULL, 0);
	rec_int(n);
	rec_state();

	call("ListAlias no prefix");
	n = cur->ListAlias(NULL, NULL, 0);
	rec_int(n);
	rec_state();

	call("ListAlias prefix");
	n = cur->ListAlias("ha", NULL, 0);
	rec_int(n);
	rec_state();

	call("cmdlist");
	cur->ExecuteString("cmdlist", src_command);
	rec_state();

	call("cvarlist");
	cur->ExecuteString("cvarlist cfg_", src_command);
	rec_state();

	call("aliaslist");
	cur->ExecuteString("aliaslist", src_command);
	rec_state();

	call("apropos");
	cur->ExecuteString("apropos harness", src_command);
	rec_state();

	call("find");
	cur->ExecuteString("find cfg_", src_command);
	rec_state();
}
#endif

/*----------------------------------------------------------------------------
 * case runner
 *--------------------------------------------------------------------------*/

struct child_result {
	size_t len;
	int checks;
	int failures;
};

static void run_case(const char *what, void (*scenario)(void));

static int run_impl(int idx, void (*scenario)(void), unsigned char *out,
		size_t *outlen, int *out_checks, int *out_failures)
{
	char path[] = "/tmp/cmd-harness-XXXXXX";
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
			fprintf(stderr, "      killed by signal %d\n",
				WTERMSIG(status));
		return -1;
	}
	got = (size_t)read(fd, out, hdr.len);
	close(fd);
	unlink(path);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "FAIL: %s did not finish", impls[idx].name);
		if (WIFSIGNALED(status))
			fprintf(stderr, " (killed by signal %d)",
				WTERMSIG(status));
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

	if (bad) {
		failures++;
		return;
	}

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
		if (getenv("CMD_DUMP")) {
			char p0[128], p1[128];
			FILE *fp;

			snprintf(p0, sizeof p0, "%s.c.bin", getenv("CMD_DUMP"));
			snprintf(p1, sizeof p1, "%s.rust.bin", getenv("CMD_DUMP"));
			fp = fopen(p0, "wb");
			if (fp) { fwrite(out[IMPL_C], 1, len[IMPL_C], fp); fclose(fp); }
			fp = fopen(p1, "wb");
			if (fp) { fwrite(out[i], 1, len[i], fp); fclose(fp); }
		}
	}
}

/*----------------------------------------------------------------------------
 * main
 *--------------------------------------------------------------------------*/

int main(void)
{
	printf("cmd differential harness: the C original and the Rust module "
		"(arm: %s)\n",
#if defined(H2W)
		"H2W + SERVERONLY"
#elif defined(SERVERONLY)
		"SERVERONLY"
#else
		"client"
#endif
	);

	layout_check();

	run_case("init", scenario_init);
	run_case("tokenize", scenario_tokenize);
	run_case("cbuf", scenario_cbuf);
	run_case("aliases", scenario_aliases);
	run_case("dispatch", scenario_dispatch);
	run_case("startup and stuffcmds", scenario_startup);
	run_case("add after host_initialized", scenario_after_init);
#ifndef SERVERONLY
	run_case("listing surface", scenario_lists);
#endif
#ifdef H2W
	run_case("NULL handler", scenario_null_handler);
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
