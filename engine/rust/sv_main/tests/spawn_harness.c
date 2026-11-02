/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * spawn_harness.c -- engine/rust/src/sv_main_spawn.rs against the C original
 * of Hexen II's SV_SpawnServer and its map_checks readout (E2 / slice E of
 * #469), in one binary.
 *
 * run_spawn_harness.sh builds this three times, once per way the engine
 * compiles sv_main.c: -DSERVERONLY (h2ded), -DGLQUAKE (glhexen2, gles) and
 * -DWEBSOFT (the web software renderer), each with the real
 * engine/rust/sv_main_target.c and engine/rust/sv_main_variadic.c compiled
 * the same way and linked against the one Rust archive.  The fixture's
 * SV_SpawnServer goes in renamed to c_SV_SpawnServer; its three static
 * helpers are reached through c_ref_spawn.c's doors, and the Rust's through
 * its SvMainSpawn_Test* hooks.
 *
 * Every engine function the spawn calls is a recorder here: it appends its
 * name, its arguments, and the state it can observe to the call log, then
 * does what the engine's does to the state the spawn reads afterwards
 * (Host_ClearMemory zeroes sv, Hunk_AllocName hands out arena memory, the
 * cvar setters set the cvar, PR_LoadProgs installs the case's progs,
 * ED_LoadFromFile the case's map_checks census, SV_CreateBaseline the case's
 * signon sizes, ...).  CON_Printf records every console line with its flag;
 * SV_PrintMapCheck is the real C, so the checklist text goes through it.
 *
 * Four kinds of case:
 *   spawn      SV_SpawnServer (sometimes twice, a changelevel), forked once
 *              per side from the same snapshot.  Each child serialises the
 *              call log, the console log, and every field the spawn can have
 *              written -- sv field by field and as a whole with its pointers
 *              made position-independent, svs and all sixteen client slots,
 *              the arena allocations, the progs globals, the cvars, and the
 *              engine globals -- and the parent compares the two byte for
 *              byte.  Sys_Error and Host_Error record and exit(42), so a
 *              fatal case compares too.
 *   checklist  SV_PrintMapChecklist alone, in process: the console text,
 *              the call log and numwarnings.
 *   title      SV_MapCheckTitle over random byte strings and buffer sizes,
 *              with a guard band after the buffer.
 *   thresh     SV_MapCheckThresh over edges and random ints.
 * Plus one directed Rust-only case: a world model with MAX_MODELS
 * submodels, where the C indexes past three arrays and the port aborts.
 *
 * Before the cases, the facts table sv_main_spawn.rs reads is checked
 * against this compile's headers, and the Rust struct's layout against C's.
 */

#include "quakedef.h"
#include "q_ctype.h"
#include "filenames.h"
#include "sv_main_target.h"

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>

#if defined(SERVERONLY)
#define HARNESS_CONFIG	"serveronly"
#define HARNESS_CLIENT	0
#elif defined(WEBSOFT)
#define HARNESS_CONFIG	"websoft"
#define HARNESS_CLIENT	1
#elif defined(GLQUAKE)
#define HARNESS_CONFIG	"glquake"
#define HARNESS_CLIENT	1
#else
#error "build with -DSERVERONLY, -DGLQUAKE or -DWEBSOFT"
#endif

/* ---- the ported functions ---- */
void		c_SV_SpawnServer (const char *server, const char *startspot);
mapcheck_t	c_ref_MapCheckThresh (int current, int target);
void		c_ref_MapCheckTitle (char *out, size_t outsize, const char *in);
void		c_ref_PrintMapChecklist (void);
int		SvMainSpawn_TestMapCheckThresh (int current, int target);
void		SvMainSpawn_TestMapCheckTitle (char *out, size_t outsize, const char *in);
void		SvMainSpawn_TestPrintMapChecklist (void);
size_t		SvMainSpawnFacts_sizeof (void);
size_t		SvMainSpawnFacts_offsetof_ss_active (void);
size_t		SvMainSpawnFacts_offsetof_sv_active (void);
size_t		SvMainSpawnFacts_offsetof_mc_skill_ents (void);
size_t		SvMainSpawnFacts_offsetof_g_serverflags (void);
size_t		SvMainSpawnFacts_offsetof_localmodel_size (void);

/* ---- the engine's storage ---- */
server_t	sv;
server_static_t	svs;
sv_globals_t	sv_globals;
dprograms_t	*progs;
dfunction_t	*pr_functions;
int		pr_edict_size;
int		sv_protocol;
qboolean	sv_protocol_userset;
char		localmodels[MAX_MODELS][8];
int		current_skill;
int		sv_physics_walk;
double		host_frametime;
int		sv_max_edicts;
char		fs_gamedir_nopath[MAX_QPATH];
client_t	*host_client;
cvar_t		hostname = {"hostname", "", CVAR_NONE};
cvar_t		coop = {"coop", "0", CVAR_NONE};
cvar_t		deathmatch = {"deathmatch", "0", CVAR_NONE};
cvar_t		skill = {"skill", "1", CVAR_NONE};
cvar_t		map_checks = {"map_checks", "0", CVAR_NONE};
cvar_t		developer = {"developer", "0", CVAR_NONE};
cvar_t		max_edicts = {"max_edicts", "16384", CVAR_NONE};
cvar_t		max_temp_edicts = {"max_temp_edicts", "30", CVAR_NONE};
cvar_t		randomclass = {"randomclass", "0", CVAR_NONE};
#if HARNESS_CLIENT
float		scr_centertime_off;
int		total_loading_size, current_loading_size, loading_stage;
#endif
#if defined(GLQUAKE)
qboolean	flush_textures;
#endif

static cvar_t	*harness_cvars[] = {
	&hostname, &coop, &deathmatch, &skill, &map_checks, &developer,
	&max_edicts, &max_temp_edicts, &randomclass
};
#define NUM_CVARS	((int)(sizeof harness_cvars / sizeof harness_cvars[0]))
static char	cvar_strings[NUM_CVARS][64];

/* ---- logs ---- */
#define LOGSIZE	(1 << 22)
static char	calllog[LOGSIZE];
static size_t	calllog_len;
static char	conlog[LOGSIZE];
static size_t	conlog_len;

/* The fixture's __func__ is c_SV_SpawnServer; the Rust says SV_SpawnServer. */
static void normalize (char *s)
{
	char *p;
	for (p = s; (p = strstr (p, "c_SV_")) != NULL; )
		memmove (p, p + 2, strlen (p + 2) + 1);
}

static void append (char *log, size_t *len, const char *s)
{
	size_t n = strlen (s);
	if (*len + n >= LOGSIZE)
		n = LOGSIZE - 1 - *len;
	memcpy (log + *len, s, n);
	*len += n;
	log[*len] = 0;
}

static void log_call (const char *fmt, ...) FUNC_PRINTF(1,2);
static void log_call (const char *fmt, ...)
{
	va_list	ap;
	char	tmp[8192];
	va_start (ap, fmt);
	vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	normalize (tmp);
	append (calllog, &calllog_len, tmp);
}

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list	ap;
	char	tmp[8192];
	snprintf (tmp, sizeof tmp, "[%u] ", flags);
	append (conlog, &conlog_len, tmp);
	va_start (ap, fmt);
	vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	normalize (tmp);
	append (conlog, &conlog_len, tmp);
}

/* ---- output: one buffer per side, tagged so a mismatch names its field ---- */
static unsigned char	*outbuf;
static size_t		outlen, outcap;

static void out_bytes (const void *p, size_t n)
{
	if (outlen + n > outcap) {
		outcap = (outlen + n) * 2 + 4096;
		outbuf = realloc (outbuf, outcap);
		if (!outbuf) { perror ("realloc"); _exit (3); }
	}
	memcpy (outbuf + outlen, p, n);
	outlen += n;
}
static void out_tag (const char *t)
{
	out_bytes ("\n#", 2);
	out_bytes (t, strlen (t));
	out_bytes ("\n", 1);
}
static void out_int (int v)		{ out_bytes (&v, sizeof v); }
static void out_u64 (uint64_t v)	{ out_bytes (&v, sizeof v); }
static void out_float (float v)		{ out_bytes (&v, sizeof v); }
static void out_double (double v)	{ out_bytes (&v, sizeof v); }
static void out_str (const char *s)
{
	if (!s) { out_int (-1); return; }
	out_int ((int)strlen (s));
	out_bytes (s, strlen (s));
}

static uint64_t hash_bytes (const void *p, size_t n)
{
	const unsigned char *b = p;
	uint64_t h = 0xcbf29ce484222325ULL ^ n;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t w;
		memcpy (&w, b + i, 8);
		h ^= w;
		h *= 0x9e3779b97f4a7c15ULL;
		h ^= h >> 29;
	}
	for (; i < n; i++) {
		h ^= b[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* ---- deterministic PRNG ---- */
static uint64_t	rng_state;
static void rng_seed (uint64_t s) { rng_state = s * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL; }
static uint64_t rng_next (void)
{
	uint64_t x = rng_state;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	rng_state = x;
	return x;
}
static int rng_int (int lo, int hi)	/* inclusive */
{
	return lo + (int)(rng_next () % (uint64_t)((int64_t)hi - lo + 1));
}
static int one_in (int n)	{ return rng_int (0, n - 1) == 0; }
static int pick (const int *v, int n)	{ return v[rng_int (0, n - 1)]; }

/* ---- the arena: Hunk_AllocName's memory ---- */
static unsigned char	*arena;
static size_t		arena_cap, arena_used;
static int		hunk_fill;		/* 0: zeroed, as the hunk is; else a byte */
#define MAXALLOCS	16
static struct { size_t off, size; char name[16]; } allocs[MAXALLOCS];
static int		nallocs;

/* ---- the world the stubs present ---- */
static qmodel_t		world_model;
#define NSUBMODELS	6
static qmodel_t		sub_models[NSUBMODELS];
static char		entities_text[] = "{\n\"classname\" \"worldspawn\"\n}\n";
static byte		lightdata_token[4], visdata_token[4];
#if HARNESS_CLIENT
#define MAXTEX		10
static texture_t	tex_store[MAXTEX];
static texture_t	*tex_ptrs[MAXTEX];
#endif
static dprograms_t	case_progs;
#define MAXFUNCS	400
static dfunction_t	case_funcs[MAXFUNCS];
static float		g_floats[8];
static string_t		g_strings[4];
static client_t		clients[MAX_CLIENTS];
static ex_inventory_page_t	inv_pages[MAX_CLIENTS * MAX_INVENTORY_EX_PAGES];
static ex_item_t	preset_items[MAX_ITEMS_EX];
static ex_inventory_page_t	preset_pages[4];

/* ---- the case's script ---- */
static char	server_buf[2][160];
static const char *case_server[2];
static char	startspot_buf[2][160];
static const char *case_startspot[2];
static int	case_two;		/* a second spawn after the first */
static int	case_cl2;		/* changelevel_issued before the second */
static int	case_world_fail[2];
static int	case_numsubmodels;
static int	case_subnull_mod;	/* Mod_ForName("*N") is NULL when N % this == 1 */
static int	case_has_alpha;
static int	case_rc_present;
static int	case_edict_size;
static int	case_scribble;
static int	case_preset_items, case_preset_pages;
static int	case_inv_null[MAX_CLIENTS];
static int	case_pts_exists;
static const char *case_levelname;
static char	levelname_buf[2200];
static int	case_census[13];	/* the map_checks counters ED_LoadFromFile sets */
static char	case_changelevel[MAX_QPATH], case_changespot[64];
static char	case_midi[128];
static int	case_cd;
static float	case_message;
static int	case_nbuf, case_bufsize[MAX_SIGNON_BUFFERS], case_cursize;
static int	setstring_count;

static const char *client_loading (void)
{
#if HARNESS_CLIENT
	static char buf[128];
	snprintf (buf, sizeof buf, " loading=%d/%d/%d centertime=%a",
		  total_loading_size, current_loading_size, loading_stage,
		  (double)scr_centertime_off);
	return buf;
#else
	return "";
#endif
}

/* ---- pointer tokens: make a pointer position-independent ---- */
static int	edict_count_hint;

static uint64_t ptok (const void *p)
{
	const unsigned char *b = p;
	int k;
	if (!p)
		return 0;
	if (b >= (unsigned char *)&sv && b < (unsigned char *)&sv + sizeof sv)
		return 0x1000000000ULL + (uint64_t)(b - (unsigned char *)&sv);
	if (arena && b >= arena && b < arena + arena_cap)
		return 0x2000000000ULL + (uint64_t)(b - arena);
	if (b >= (unsigned char *)localmodels && b < (unsigned char *)localmodels + sizeof localmodels)
		return 0x3000000000ULL + (uint64_t)(b - (unsigned char *)localmodels);
	if (p == &world_model)
		return 0x4000000000ULL;
	for (k = 0; k < NSUBMODELS; k++)
		if (p == &sub_models[k])
			return 0x4000000001ULL + (uint64_t)k;
	if (b >= (unsigned char *)preset_items && b < (unsigned char *)preset_items + sizeof preset_items)
		return 0x5000000000ULL + (uint64_t)(b - (unsigned char *)preset_items);
	if (b >= (unsigned char *)preset_pages && b < (unsigned char *)preset_pages + sizeof preset_pages)
		return 0x5100000000ULL + (uint64_t)(b - (unsigned char *)preset_pages);
	if (b >= (unsigned char *)clients && b < (unsigned char *)clients + sizeof clients)
		return 0x6000000000ULL + (uint64_t)(b - (unsigned char *)clients);
	if (b >= (unsigned char *)inv_pages && b < (unsigned char *)inv_pages + sizeof inv_pages)
		return 0x7000000000ULL + (uint64_t)(b - (unsigned char *)inv_pages);
	if (b >= (unsigned char *)g_floats && b < (unsigned char *)g_floats + sizeof g_floats)
		return 0x8000000000ULL + (uint64_t)(b - (unsigned char *)g_floats);
	if (b >= (unsigned char *)g_strings && b < (unsigned char *)g_strings + sizeof g_strings)
		return 0x8100000000ULL + (uint64_t)(b - (unsigned char *)g_strings);
	return 0xdead000000000000ULL;	/* somewhere else: a mismatch unless both agree */
}

/* The precache sentinel: whichever object each side uses, it must be one
 * object, zeroed, in neither sv nor the arena nor any harness store, and
 * (checked across a changelevel) the same object on every spawn. */
static const char	*first_dummy;

static uint64_t dummy_tok (const char *p)
{
	static const char zero[8];
	if (!p)
		return 0;
	if (ptok (p) != 0xdead000000000000ULL)
		return ptok (p);
	if (memcmp (p, zero, 8) != 0)
		return 0xbad0000000000001ULL;
	return 0xd000000000000000ULL;
}

/* ---- serialisation ---- */
static void out_sizebuf_canon (const char *tag, const sizebuf_t *b)
{
	out_tag (tag);
	out_int (b->allowoverflow);
	out_int (b->overflowed);
	out_u64 (ptok (b->data));
	out_int (b->maxsize);
	out_int (b->cursize);
	out_str (b->name);
}

static void serialize_state (const char *part)
{
	static server_t	scratch;
	client_t	cl;
	int		i;

	out_tag (part);
	out_tag ("calllog");
	out_bytes (calllog, calllog_len);
	out_tag ("conlog");
	out_bytes (conlog, conlog_len);

	out_tag ("globals");
	out_int (sv_protocol);
	out_int (current_skill);
	out_int (sv_physics_walk);
	out_double (host_frametime);
	out_int (sv_max_edicts);
	out_int (pr_edict_size);
	out_u64 (ptok (host_client));
#if HARNESS_CLIENT
	out_float (scr_centertime_off);
	out_int (total_loading_size);
	out_int (current_loading_size);
	out_int (loading_stage);
#endif
#if defined(GLQUAKE)
	out_int (flush_textures);
#endif
	out_tag ("cvars");
	for (i = 0; i < NUM_CVARS; i++) {
		out_str (harness_cvars[i]->string);
		out_float (harness_cvars[i]->value);
		out_int (harness_cvars[i]->integer);
	}
	out_tag ("progs globals");
	out_bytes (g_floats, sizeof g_floats);
	out_bytes (g_strings, sizeof g_strings);

	out_tag ("sv scalars");
	out_int (sv.active);
	out_int (sv.paused);
	out_int (sv.loadgame);
	out_double (sv.time);
	out_str (sv.name);
	out_str (sv.startspot);
	out_str (sv.modelname);
	out_str (sv.mod_name);
	out_str (sv.midi_name);
	out_int (sv.cd_track);
	out_int (sv.state);
	out_int (sv.num_edicts);
	out_int (sv.num_ex_items);
	out_int (sv.num_signon_buffers);
	out_u64 (ptok (sv.worldmodel));
	out_u64 (ptok (sv.states));
	out_u64 (ptok (sv.edicts));
	out_u64 (ptok (sv.ex_items));
	out_u64 (ptok (sv.ex_inventory_pages));
	out_tag ("sv.mapchecks");
	out_bytes (&sv.mapchecks, sizeof sv.mapchecks);
	out_sizebuf_canon ("sv.datagram", &sv.datagram);
	out_sizebuf_canon ("sv.reliable_datagram", &sv.reliable_datagram);
	out_sizebuf_canon ("sv.signon", &sv.signon);
	out_tag ("precache");
	out_u64 (dummy_tok (sv.sound_precache[0]));
	out_u64 (dummy_tok (sv.model_precache[0]));
	out_int (sv.sound_precache[0] == sv.model_precache[0]);
	out_int (first_dummy == NULL || sv.model_precache[0] == NULL ||
		 sv.model_precache[0] == first_dummy);
	for (i = 1; i < MAX_MODELS; i++) {
		out_u64 (ptok (sv.model_precache[i]));
		out_u64 (ptok (sv.models[i]));
	}
	out_u64 (ptok (sv.models[0]));
	for (i = 1; i < MAX_SOUNDS; i++)
		out_u64 (ptok (sv.sound_precache[i]));

	/* the whole of sv, pointers replaced by their tokens */
	out_tag ("sv whole");
	memcpy (&scratch, &sv, sizeof sv);
	scratch.worldmodel = (struct qmodel_s *)(uintptr_t)ptok (sv.worldmodel);
	for (i = 0; i < MAX_MODELS; i++) {
		scratch.model_precache[i] = (const char *)(uintptr_t)(i ? ptok (sv.model_precache[i]) : dummy_tok (sv.model_precache[i]));
		scratch.models[i] = (struct qmodel_s *)(uintptr_t)ptok (sv.models[i]);
	}
	for (i = 0; i < MAX_SOUNDS; i++)
		scratch.sound_precache[i] = (const char *)(uintptr_t)(i ? ptok (sv.sound_precache[i]) : dummy_tok (sv.sound_precache[i]));
	for (i = 0; i < MAX_LIGHTSTYLES; i++)
		scratch.lightstyles[i] = (const char *)(uintptr_t)ptok (sv.lightstyles[i]);
	scratch.states = (client_state2_t *)(uintptr_t)ptok (sv.states);
	scratch.edicts = (edict_t *)(uintptr_t)ptok (sv.edicts);
	scratch.ex_items = (ex_item_t *)(uintptr_t)ptok (sv.ex_items);
	scratch.ex_inventory_pages = (ex_inventory_page_t *)(uintptr_t)ptok (sv.ex_inventory_pages);
	scratch.datagram.data = (byte *)(uintptr_t)ptok (sv.datagram.data);
	scratch.datagram.name = NULL;
	scratch.reliable_datagram.data = (byte *)(uintptr_t)ptok (sv.reliable_datagram.data);
	scratch.reliable_datagram.name = NULL;
	scratch.signon.data = (byte *)(uintptr_t)ptok (sv.signon.data);
	scratch.signon.name = NULL;
	out_u64 (hash_bytes (&scratch, sizeof scratch));

	out_tag ("svs");
	out_int (svs.maxclients);
	out_int (svs.maxclientslimit);
	out_u64 (ptok (svs.clients));
	out_int (svs.serverflags);
	out_int (svs.changelevel_issued);
	out_tag ("clients");
	for (i = 0; i < MAX_CLIENTS; i++) {
		memcpy (&cl, &clients[i], sizeof cl);
		out_int (cl.active);
		out_int (cl.send_all_v);
		out_u64 (ptok (cl.edict));
		out_u64 (ptok (cl.ex_inventory));
		cl.edict = (edict_t *)(uintptr_t)ptok (cl.edict);
		cl.ex_inventory = (ex_inventory_page_t *)(uintptr_t)ptok (cl.ex_inventory);
		out_u64 (hash_bytes (&cl, sizeof cl));
	}

	out_tag ("arena");
	out_int (nallocs);
	for (i = 0; i < nallocs; i++) {
		size_t n = allocs[i].size;
		if (!strcmp (allocs[i].name, "edicts")) {
			/* every write is below num_edicts; two past it shows a loop
			 * that runs long */
			size_t lim = (size_t)(edict_count_hint + 2) * (size_t)pr_edict_size;
			if (sv.num_edicts > edict_count_hint)
				lim = (size_t)(sv.num_edicts + 2) * (size_t)pr_edict_size;
			if (lim < n)
				n = lim;
		} else if (n > (1u << 20)) {
			n = 1u << 20;
		}
		out_u64 (allocs[i].off);
		out_u64 (allocs[i].size);
		out_u64 (hash_bytes (arena + allocs[i].off, n));
	}
	out_tag ("presets");
	out_u64 (hash_bytes (preset_items, sizeof preset_items));
	out_u64 (hash_bytes (preset_pages, sizeof preset_pages));
	out_u64 (hash_bytes (inv_pages, sizeof inv_pages));
}

/* ---- fatal stubs: record, then end the child the same way on both sides ---- */
static int	in_child;
static int	out_fd = -1;

static void flush_and_exit (int code)
{
	size_t done = 0;
	while (done < outlen) {
		ssize_t w = write (out_fd, outbuf + done, outlen - done);
		if (w <= 0) {
			if (w < 0 && errno == EINTR)
				continue;
			_exit (4);
		}
		done += (size_t)w;
	}
	close (out_fd);
	/* exit(), not _exit(): libgcov's exit handler dumps this child's
	 * coverage */
	exit (code);
}

static void fatal_record (const char *kind, const char *fmt, va_list ap)
{
	char buf[2048];
	vsnprintf (buf, sizeof buf, fmt, ap);
	normalize (buf);
	log_call ("%s: %s\n", kind, buf);
	if (!in_child) {
		fprintf (stderr, "harness bug: %s outside a child: %s\n", kind, buf);
		abort ();
	}
	serialize_state ("fatal");
	flush_and_exit (42);
}

FUNC_NORETURN void Sys_Error (const char *error, ...)
{
	va_list ap;
	va_start (ap, error);
	fatal_record ("Sys_Error", error, ap);
	va_end (ap);
	exit (99);
}

FUNC_NORETURN void Host_Error (const char *error, ...)
{
	va_list ap;
	va_start (ap, error);
	fatal_record ("Host_Error", error, ap);
	va_end (ap);
	exit (99);
}

/* ---- the engine functions SV_SpawnServer calls ---- */
static cvar_t *find_cvar (const char *name)
{
	int i;
	for (i = 0; i < NUM_CVARS; i++)
		if (!strcmp (harness_cvars[i]->name, name))
			return harness_cvars[i];
	return NULL;
}

static void set_cvar_string (cvar_t *v, const char *s)
{
	int i;
	for (i = 0; i < NUM_CVARS; i++)
		if (harness_cvars[i] == v)
			break;
	snprintf (cvar_strings[i], sizeof cvar_strings[i], "%s", s);
	v->string = cvar_strings[i];
}

void Cvar_Set (const char *var_name, const char *value)
{
	cvar_t *v = find_cvar (var_name);
	log_call ("Cvar_Set(%s,%s)\n", var_name, value);
	if (!v)
		return;
	set_cvar_string (v, value);
	v->value = (float)atof (value);
	v->integer = (int)v->value;
}

void Cvar_SetValue (const char *var_name, const float value)
{
	cvar_t *v = find_cvar (var_name);
	char s[64];
	log_call ("Cvar_SetValue(%s,%a)\n", var_name, (double)value);
	if (!v)
		return;
	snprintf (s, sizeof s, "%g", (double)value);
	set_cvar_string (v, s);
	v->value = value;
	v->integer = (int)value;
}

int SaveGamestate (qboolean ClientsOnly)
{
	log_call ("SaveGamestate(%d) name=[%s]\n", (int)ClientsOnly, sv.name);
	return 0;
}

void SV_SendReconnect (void)
{
	log_call ("SV_SendReconnect active=%d name=[%s]\n", sv.active, sv.name);
}

void Host_ClearMemory (void)
{
	log_call ("Host_ClearMemory hostname=[%s] skill=%d/%a current_skill=%d dm=[%s]%s\n",
		  hostname.string, skill.integer, (double)skill.value, current_skill,
		  deathmatch.string, client_loading ());
	memset (&sv, 0, sizeof sv);
	arena_used = 0;
	nallocs = 0;
	/* Host_ClearMemory always leaves these NULL in the engine; presetting
	 * them reaches SV_SpawnServer's already-allocated arms. */
	if (case_preset_items)
		sv.ex_items = preset_items;
	if (case_preset_pages)
		sv.ex_inventory_pages = preset_pages;
}

void PR_LoadProgs (void)
{
	log_call ("PR_LoadProgs name=[%s] startspot=[%s] mc_active=%d%s\n",
		  sv.name, sv.startspot, sv.mapchecks.active, client_loading ());
	progs = &case_progs;
	pr_functions = case_funcs;
	pr_edict_size = case_edict_size;
	memset (&sv_globals, 0, sizeof sv_globals);
	sv_globals.coop = &g_floats[0];
	sv_globals.deathmatch = &g_floats[1];
	sv_globals.randomclass = case_rc_present ? &g_floats[2] : NULL;
	sv_globals.serverflags = &g_floats[3];
	sv_globals.mapname = &g_strings[0];
	sv_globals.startspot = &g_strings[1];
	/* Nothing in the engine writes sv.name here; scribbling it makes the
	 * C's second q_strlcpy of the name observable. */
	if (case_scribble)
		snprintf (sv.name, sizeof sv.name, "scribbled%d", case_scribble);
}

qboolean PR_HasField (const char *name)
{
	log_call ("PR_HasField(%s) -> %d protocol=%d\n", name, case_has_alpha, sv_protocol);
	return case_has_alpha ? true : false;
}

void Host_LoadStrings (void)
{
	log_call ("Host_LoadStrings protocol=%d%s\n", sv_protocol, client_loading ());
}

#if HARNESS_CLIENT
void D_ShowLoadingSize (void)
{
	log_call ("D_ShowLoadingSize%s\n", client_loading ());
}

void SCR_EndLoadingPlaque (void)
{
	log_call ("SCR_EndLoadingPlaque%s active=%d\n", client_loading (), sv.active);
}
#endif

void *Hunk_AllocName (int size, const char *name)
{
	size_t off;
	log_call ("Hunk_AllocName(%d,%s) at %zu\n", size, name, arena_used);
	if (size < 0)
		Sys_Error ("Hunk_Alloc: bad size: %i", size);
	off = (arena_used + 15) & ~(size_t)15;
	if (off + (size_t)size > arena_cap)
		Sys_Error ("Hunk_Alloc: failed on %i bytes", size);
	if (nallocs < MAXALLOCS) {
		allocs[nallocs].off = off;
		allocs[nallocs].size = (size_t)size;
		snprintf (allocs[nallocs].name, sizeof allocs[nallocs].name, "%s", name);
		nallocs++;
	}
	/* The arena is never written in the parent, so a child's allocation
	 * is zeroed, as the hunk's is.  A filled one makes every write and
	 * every clear the spawn does observable. */
	if (hunk_fill && size <= (4 << 20))
		memset (arena + off, hunk_fill, (size_t)size);
	arena_used = off + (size_t)size;
	return arena + off;
}

static const char *sizebuf_id (const void *b)
{
	if (b == &sv.datagram) return "sv.datagram";
	if (b == &sv.reliable_datagram) return "sv.reliable_datagram";
	if (b == &sv.signon) return "sv.signon";
	return "?";
}

void SZ_Init (sizebuf_t *buf, byte *data, int length)
{
	log_call ("SZ_Init(%s,%llx,%d)\n", sizebuf_id (buf),
		  (unsigned long long)ptok (data), length);
	memset (buf, 0, sizeof *buf);
	buf->data = data;
	buf->maxsize = length;
}

edict_t *EDICT_NUM (int n)
{
	log_call ("EDICT_NUM(%d)\n", n);
	if (n < 0 || n >= sv_max_edicts)
		Host_Error ("%s: bad number %i", "EDICT_NUM", n);
	return (edict_t *)((byte *)sv.edicts + n * pr_edict_size);
}

void ED_ClearEdict (edict_t *e)
{
	log_call ("ED_ClearEdict(%llx)\n", (unsigned long long)ptok (e));
	memset (&e->v, 0, progs->entityfields * 4);
	memset (&e->baseline, 0, sizeof (e->baseline));
	e->free = false;
}

qmodel_t *Mod_ForName (const char *name, qboolean crash)
{
	int n;
	log_call ("Mod_ForName(%s,%d)\n", name, (int)crash);
	if (name[0] == '*') {
		n = atoi (name + 1);
		if (case_subnull_mod && n % case_subnull_mod == 1)
			return NULL;
		return &sub_models[n % NSUBMODELS];
	}
	if (case_world_fail[case_two > 1])
		return NULL;
	q_strlcpy (world_model.name, name, sizeof world_model.name);
	return &world_model;
}

void SV_ParseWorldspawnSky (void)
{
	log_call ("SV_ParseWorldspawnSky world=%llx models1=%llx\n",
		  (unsigned long long)ptok (sv.worldmodel),
		  (unsigned long long)ptok (sv.models[1]));
}

void SV_ClearWorld (void)
{
	log_call ("SV_ClearWorld mod_name=[%s]\n", sv.mod_name);
}

char *va (const char *format, ...)
{
	static char buf[1024];
	va_list ap;
	va_start (ap, format);
	vsnprintf (buf, sizeof buf, format, ap);
	va_end (ap);
	return buf;
}

ex_inventory_page_t *SV_ClientInventoryPage (int clientnum)
{
	ex_inventory_page_t *p = case_inv_null[clientnum] ? NULL : &inv_pages[clientnum];
	log_call ("SV_ClientInventoryPage(%d) pages=%llx -> %llx\n", clientnum,
		  (unsigned long long)ptok (sv.ex_inventory_pages),
		  (unsigned long long)ptok (p));
	return p;
}

int PR_SetEngineString (const char *s)
{
	int r = 7000 + setstring_count++;
	log_call ("PR_SetEngineString(%s) -> %d\n", s, r);
	return r;
}

void ED_LoadFromFile (const char *data)
{
	edict_t *e = sv.edicts;
	log_call ("ED_LoadFromFile(%s) e0 free=%d model=%d modelindex=%a solid=%a "
		  "movetype=%a coop=%a dm=%a rc=%a flags=%a map=%d spot=%d "
		  "active=%d state=%d num_ex=%d%s\n",
		  data ? data : "(null)", e->free, e->v.model,
		  (double)e->v.modelindex, (double)e->v.solid, (double)e->v.movetype,
		  (double)g_floats[0], (double)g_floats[1], (double)g_floats[2],
		  (double)g_floats[3], g_strings[0], g_strings[1],
		  sv.active, (int)sv.state, sv.num_ex_items, client_loading ());
	/* the census, as ED_LoadFromFile's worldspawn and classname walk
	 * leave it */
	sv.mapchecks.numwarnings = case_census[0];
	sv.mapchecks.trigger_changelevel = case_census[1];
	sv.mapchecks.valid_changelevel = case_census[2];
	sv.mapchecks.intermission = case_census[3];
	sv.mapchecks.skill_triggers = case_census[4];
	sv.mapchecks.coop_spawns = case_census[5];
	sv.mapchecks.dm_spawns = case_census[6];
	sv.mapchecks.sp_spawns = case_census[7];
	sv.mapchecks.skill_ents[0] = case_census[8];
	sv.mapchecks.skill_ents[1] = case_census[9];
	sv.mapchecks.skill_ents[2] = case_census[10];
	q_strlcpy (sv.mapchecks.changelevel, case_changelevel, sizeof sv.mapchecks.changelevel);
	q_strlcpy (sv.mapchecks.changespot, case_changespot, sizeof sv.mapchecks.changespot);
	q_strlcpy (sv.midi_name, case_midi, sizeof sv.midi_name);
	sv.cd_track = (byte)case_cd;
	e->v.message = case_message;
}

void SV_Physics (void)
{
	log_call ("SV_Physics frametime=%a state=%d active=%d\n",
		  host_frametime, (int)sv.state, sv.active);
}

void SV_CreateBaseline (void)
{
	int i;
	log_call ("SV_CreateBaseline signon=%d/%d\n", sv.num_signon_buffers, sv.signon.cursize);
	sv.num_signon_buffers = case_nbuf;
	/* Entries past the count are filled too: the spawn's total must read
	 * exactly the first num_signon_buffers of them, and a zero there
	 * would hide a loop that runs one long. */
	for (i = 0; i < MAX_SIGNON_BUFFERS; i++)
		sv.signon_size[i] = case_bufsize[i];
	sv.signon.cursize = case_cursize;
}

void SV_SendServerinfo (client_t *client)
{
	log_call ("SV_SendServerinfo(%llx) host_client=%llx\n",
		  (unsigned long long)ptok (client), (unsigned long long)ptok (host_client));
}

const char *COM_SkipPath (const char *pathname)
{
	const char	*last;

	last = pathname;
	while (*pathname)
	{
		if (IS_DIR_SEPARATOR(*pathname))
			last = pathname + 1;
		pathname++;
	}
	return last;
}

qboolean FS_FileExists (const char *filename, unsigned int *path_id)
{
	log_call ("FS_FileExists(%s,%s) -> %d\n", filename, path_id ? "id" : "NULL", case_pts_exists);
	return case_pts_exists ? true : false;
}

const char *SV_GetLevelname (void)
{
	log_call ("SV_GetLevelname\n");
	return case_levelname;
}

/* common.c's, verbatim */
int q_strncasecmp(const char *s1, const char *s2, size_t n)
{
	const char * p1 = s1;
	const char * p2 = s2;
	char c1, c2;

	if (p1 == p2 || n == 0)
		return 0;

	do
	{
		c1 = q_tolower (*p1++);
		c2 = q_tolower (*p2++);
		if (c1 == '\0' || c1 != c2)
			break;
	} while (--n > 0);

	return (int)(c1 - c2);
}

/* ---- the facts table, checked against this compile's headers ---- */
static int check_facts (void)
{
	const sv_main_spawn_facts_t *f = &SvMainSpawn_TargetFacts;
	int bad = 0;
#define CKI(name, expect) \
	if (f->name != (expect)) { \
		printf ("facts mismatch: %s = %d, header says %d\n", \
			#name, (int)f->name, (int)(expect)); bad = 1; }
#define CKZ(name, expect) \
	if (f->name != (size_t)(expect)) { \
		printf ("facts mismatch: %s = %zu, header says %zu\n", \
			#name, f->name, (size_t)(expect)); bad = 1; }
#define MS(type, member)	sizeof (((type *)0)->member)
	CKI (client_build, HARNESS_CLIENT);
#if defined(GLQUAKE)
	CKI (glquake_build, 1);
#else
	CKI (glquake_build, 0);
#endif
	CKI (print_normal, _PRINT_NORMAL);
	CKI (print_devel, _PRINT_DEVEL);
	CKI (print_safe, _PRINT_SAFE);
	CKI (mapcheck_failed, MAPCHECK_FAILED);
	CKI (mapcheck_partial, MAPCHECK_PARTIAL);
	CKI (mapcheck_ok, MAPCHECK_OK);
	CKI (mapcheck_na, MAPCHECK_NA);
	CKI (protocol_uh2_114, PROTOCOL_UH2_114);
	CKI (protocol_hexenwail_1, PROTOCOL_HEXENWAIL_1);
	CKI (min_edicts, MIN_EDICTS);
	CKI (max_edicts, MAX_EDICTS);
	CKI (max_models, MAX_MODELS);
	CKI (max_qpath, MAX_QPATH);
	CKI (max_ospath, MAX_OSPATH);
	CKI (max_signon_size, MAX_SIGNON_SIZE);
	CKI (max_signon_buffers, MAX_SIGNON_BUFFERS);
	CKI (max_items_ex, MAX_ITEMS_EX);
	CKI (max_inventory_ex_pages, MAX_INVENTORY_EX_PAGES);
	CKI (solid_bsp, SOLID_BSP);
	CKI (movetype_push, MOVETYPE_PUSH);
	CKI (ss_loading, ss_loading);
	CKI (ss_active, ss_active);
	CKZ (sv_active, offsetof (server_t, active));
	CKZ (sv_paused, offsetof (server_t, paused));
	CKZ (sv_time, offsetof (server_t, time));
	CKZ (sv_name, offsetof (server_t, name));
	CKZ (sv_name_size, MS (server_t, name));
	CKZ (sv_midi_name, offsetof (server_t, midi_name));
	CKZ (sv_mod_name, offsetof (server_t, mod_name));
	CKZ (sv_mod_name_size, MS (server_t, mod_name));
	CKZ (sv_cd_track, offsetof (server_t, cd_track));
	CKZ (sv_startspot, offsetof (server_t, startspot));
	CKZ (sv_startspot_size, MS (server_t, startspot));
	CKZ (sv_modelname, offsetof (server_t, modelname));
	CKZ (sv_modelname_size, MS (server_t, modelname));
	CKZ (sv_worldmodel, offsetof (server_t, worldmodel));
	CKZ (sv_model_precache, offsetof (server_t, model_precache));
	CKZ (sv_models, offsetof (server_t, models));
	CKZ (sv_sound_precache, offsetof (server_t, sound_precache));
	CKZ (sv_states, offsetof (server_t, states));
	CKZ (sv_num_edicts, offsetof (server_t, num_edicts));
	CKZ (sv_edicts, offsetof (server_t, edicts));
	CKZ (sv_state, offsetof (server_t, state));
	CKZ (sv_datagram, offsetof (server_t, datagram));
	CKZ (sv_datagram_buf, offsetof (server_t, datagram_buf));
	CKZ (sv_datagram_buf_size, MS (server_t, datagram_buf));
	CKZ (sv_reliable_datagram, offsetof (server_t, reliable_datagram));
	CKZ (sv_reliable_datagram_buf, offsetof (server_t, reliable_datagram_buf));
	CKZ (sv_reliable_datagram_buf_size, MS (server_t, reliable_datagram_buf));
	CKZ (sv_signon, offsetof (server_t, signon));
	CKZ (sv_num_signon_buffers, offsetof (server_t, num_signon_buffers));
	CKZ (sv_signon_size, offsetof (server_t, signon_size));
	CKZ (sv_signon_bufs, offsetof (server_t, signon_bufs));
	CKZ (sv_ex_items, offsetof (server_t, ex_items));
	CKZ (sv_ex_inventory_pages, offsetof (server_t, ex_inventory_pages));
	CKZ (sv_num_ex_items, offsetof (server_t, num_ex_items));
	CKZ (mc_active, offsetof (server_t, mapchecks.active));
	CKZ (mc_numwarnings, offsetof (server_t, mapchecks.numwarnings));
	CKZ (mc_changelevel, offsetof (server_t, mapchecks.changelevel));
	CKZ (mc_changespot, offsetof (server_t, mapchecks.changespot));
	CKZ (mc_trigger_changelevel, offsetof (server_t, mapchecks.trigger_changelevel));
	CKZ (mc_valid_changelevel, offsetof (server_t, mapchecks.valid_changelevel));
	CKZ (mc_intermission, offsetof (server_t, mapchecks.intermission));
	CKZ (mc_skill_triggers, offsetof (server_t, mapchecks.skill_triggers));
	CKZ (mc_coop_spawns, offsetof (server_t, mapchecks.coop_spawns));
	CKZ (mc_dm_spawns, offsetof (server_t, mapchecks.dm_spawns));
	CKZ (mc_sp_spawns, offsetof (server_t, mapchecks.sp_spawns));
	CKZ (mc_skill_ents, offsetof (server_t, mapchecks.skill_ents));
	CKZ (svs_maxclients, offsetof (server_static_t, maxclients));
	CKZ (svs_clients, offsetof (server_static_t, clients));
	CKZ (svs_serverflags, offsetof (server_static_t, serverflags));
	CKZ (svs_changelevel_issued, offsetof (server_static_t, changelevel_issued));
	CKZ (client_size, sizeof (client_t));
	CKZ (client_active, offsetof (client_t, active));
	CKZ (client_edict, offsetof (client_t, edict));
	CKZ (client_send_all_v, offsetof (client_t, send_all_v));
	CKZ (client_ex_inventory, offsetof (client_t, ex_inventory));
	CKZ (edict_free, offsetof (edict_t, free));
	CKZ (edict_freetime, offsetof (edict_t, freetime));
	CKZ (edict_v, offsetof (edict_t, v));
	CKZ (v_model, offsetof (entvars_t, model));
	CKZ (v_modelindex, offsetof (entvars_t, modelindex));
	CKZ (v_solid, offsetof (entvars_t, solid));
	CKZ (v_movetype, offsetof (entvars_t, movetype));
	CKZ (v_message, offsetof (entvars_t, message));
	CKZ (sizebuf_name, offsetof (sizebuf_t, name));
	CKZ (sizebuf_cursize, offsetof (sizebuf_t, cursize));
	CKZ (client_state2_size, sizeof (client_state2_t));
	CKZ (ex_item_size, sizeof (ex_item_t));
	CKZ (ex_item_id, offsetof (ex_item_t, id));
	CKZ (ex_item_icon, offsetof (ex_item_t, icon));
	CKZ (ex_inventory_page_size, sizeof (ex_inventory_page_t));
	CKZ (progs_numfunctions, offsetof (dprograms_t, numfunctions));
	CKZ (progs_entityfields, offsetof (dprograms_t, entityfields));
	CKZ (function_size, sizeof (dfunction_t));
	CKZ (function_first_statement, offsetof (dfunction_t, first_statement));
	CKZ (g_coop, offsetof (sv_globals_t, coop));
	CKZ (g_deathmatch, offsetof (sv_globals_t, deathmatch));
	CKZ (g_randomclass, offsetof (sv_globals_t, randomclass));
	CKZ (g_mapname, offsetof (sv_globals_t, mapname));
	CKZ (g_startspot, offsetof (sv_globals_t, startspot));
	CKZ (g_serverflags, offsetof (sv_globals_t, serverflags));
	CKZ (qmodel_name, offsetof (qmodel_t, name));
	CKZ (qmodel_numsubmodels, offsetof (qmodel_t, numsubmodels));
	CKZ (qmodel_lightdata, offsetof (qmodel_t, lightdata));
	CKZ (qmodel_visdata, offsetof (qmodel_t, visdata));
	CKZ (qmodel_entities, offsetof (qmodel_t, entities));
#if HARNESS_CLIENT
	CKZ (qmodel_numtextures, offsetof (qmodel_t, numtextures));
	CKZ (qmodel_textures, offsetof (qmodel_t, textures));
	CKZ (texture_name, offsetof (texture_t, name));
	CKZ (texture_width, offsetof (texture_t, width));
	CKZ (texture_height, offsetof (texture_t, height));
	if (MS (texture_t, width) != sizeof (unsigned int) || MS (texture_t, height) != sizeof (unsigned int)) {
		printf ("facts mismatch: texture_t width/height are not unsigned int\n"); bad = 1; }
#else
	CKZ (qmodel_numtextures, 0);
	CKZ (qmodel_textures, 0);
	CKZ (texture_name, 0);
	CKZ (texture_width, 0);
	CKZ (texture_height, 0);
#endif
	CKZ (localmodel_size, sizeof (localmodels[0]));
	if (sizeof (entvars_t) % 4 != 0) {
		printf ("facts mismatch: entvars_t is not a whole number of fields\n"); bad = 1; }
	/* the Rust struct's layout against this one */
	if (SvMainSpawnFacts_sizeof () != sizeof (sv_main_spawn_facts_t) ||
	    SvMainSpawnFacts_offsetof_ss_active () != offsetof (sv_main_spawn_facts_t, ss_active) ||
	    SvMainSpawnFacts_offsetof_sv_active () != offsetof (sv_main_spawn_facts_t, sv_active) ||
	    SvMainSpawnFacts_offsetof_mc_skill_ents () != offsetof (sv_main_spawn_facts_t, mc_skill_ents) ||
	    SvMainSpawnFacts_offsetof_g_serverflags () != offsetof (sv_main_spawn_facts_t, g_serverflags) ||
	    SvMainSpawnFacts_offsetof_localmodel_size () != offsetof (sv_main_spawn_facts_t, localmodel_size)) {
		printf ("facts mismatch: the Rust SvMainSpawnFacts layout differs from sv_main_spawn_facts_t\n");
		bad = 1;
	}
#undef CKI
#undef CKZ
#undef MS
	if (!bad)
		printf ("layouts: sv_main_spawn facts match the headers (%s)\n", HARNESS_CONFIG);
	return bad;
}

/* ---- generators ---- */
static void gen_text (char *buf, size_t cap, int maxlen)
{
	int n = rng_int (0, maxlen), i;
	if ((size_t)n >= cap)
		n = (int)cap - 1;
	for (i = 0; i < n; i++) {
		int c;
		switch (rng_int (0, 9)) {
		case 0: c = rng_int (1, 31); break;		/* control */
		case 1: c = 127; break;
		case 2: case 3: c = ' '; break;
		case 4: c = rng_int (128, 255); break;
		case 5: c = '\n'; break;
		default: c = rng_int ('a', 'z'); break;
		}
		buf[i] = (char)c;
	}
	buf[n] = 0;
}

static void gen_name (char *buf, size_t cap)
{
	static const char *names[] = {
		"demo1", "DEMO1", "village1", "Village1", "e", "meso1", "tower",
		"cath", "castle4", "romeric1", "egypt1", "", "a%sb", "x/y"
	};
	int k = rng_int (0, 17), i, n;
	if (k < 14) {
		snprintf (buf, cap, "%s", names[k]);
		return;
	}
	n = ((int[]){ 62, 63, 64, 100 })[k - 14];
	if ((size_t)n >= cap)
		n = (int)cap - 1;
	for (i = 0; i < n; i++)
		buf[i] = (char)('a' + (i * 7 + k) % 26);
	buf[n] = 0;
}

static void gen_case_name_pair (int which)
{
	gen_name (server_buf[which], sizeof server_buf[which]);
	case_server[which] = server_buf[which];
	switch (rng_int (0, 9)) {
	case 0: case_startspot[which] = NULL; break;
	case 1: case_startspot[which] = NULL; break;
	case 2: case_startspot[which] = ""; break;
	case 3: case_startspot[which] = "start1"; break;
	default:
		gen_name (startspot_buf[which], sizeof startspot_buf[which]);
		case_startspot[which] = startspot_buf[which];
		break;
	}
}

static float gen_float_value (void)
{
	static const float v[] = {
		0.0f, 1.0f, 2.0f, -1.0f, 0.5f, -0.5f, 1.5f, 3.0f, 16384.0f
	};
	return v[rng_int (0, (int)(sizeof v / sizeof v[0]) - 1)];
}

static void set_cvar_value (cvar_t *v, float value)
{
	char s[64];
	snprintf (s, sizeof s, "%g", (double)value);
	set_cvar_string (v, s);
	v->value = value;
	v->integer = (int)value;
}


static void gen_census (void)
{
	static const int cnt[] = { 0, 1, 2, 3, 4, 5, 6, 37, -1, 1 << 20 };
	int i;
	for (i = 0; i < 13; i++)
		case_census[i] = pick (cnt, 10);
	case_census[0] = one_in (3) ? rng_int (0, 5) : 0;	/* numwarnings */
	/* valid_changelevel: equal to trigger_changelevel half the time */
	if (rng_int (0, 1))
		case_census[2] = case_census[1];
	/* skill_ents: all equal often, so the triggers decide */
	if (rng_int (0, 1))
		case_census[9] = case_census[10] = case_census[8];
	switch (rng_int (0, 4)) {
	case 0: case_changelevel[0] = 0; break;
	case 1: snprintf (case_changelevel, sizeof case_changelevel, "%s", "village2"); break;
	case 2: memset (case_changelevel, 'q', sizeof case_changelevel - 1);
		case_changelevel[sizeof case_changelevel - 1] = 0; break;
	case 3: snprintf (case_changelevel, sizeof case_changelevel, "%s", "100%s%d"); break;
	default: gen_text (case_changelevel, sizeof case_changelevel, 20); break;
	}
	switch (rng_int (0, 3)) {
	case 0: case 1: case_changespot[0] = 0; break;
	case 2: snprintf (case_changespot, sizeof case_changespot, "%s", "start2"); break;
	default: gen_text (case_changespot, sizeof case_changespot, 70); break;
	}
	switch (rng_int (0, 3)) {
	case 0: case_midi[0] = 0; break;
	case 1: snprintf (case_midi, sizeof case_midi, "%s", "casa1"); break;
	case 2: memset (case_midi, 'm', sizeof case_midi - 1);
		case_midi[sizeof case_midi - 1] = 0; break;
	default: gen_text (case_midi, sizeof case_midi, 30); break;
	}
	case_cd = ((int[]){ 0, 0, 1, 7, 255 })[rng_int (0, 4)];
	case_message = ((float[]){ 0.0f, 1.0f, 5.0f, -1.0f, 0.5f, 1.9f, -0.5f,
		NAN, INFINITY, -INFINITY, 3e9f, 2147483520.0f, 12.0f })[rng_int (0, 12)];
	switch (rng_int (0, 6)) {
	case 0: case_levelname = NULL; break;
	case 1: case_levelname = ""; break;
	case 2: case_levelname = "The Village of Blackmarsh"; break;
	case 3: memset (levelname_buf, 'T', 2100); levelname_buf[2100] = 0;
		case_levelname = levelname_buf; break;
	case 4: snprintf (levelname_buf, sizeof levelname_buf, "\n\n  \t%s", "");
		case_levelname = levelname_buf; break;
	default: gen_text (levelname_buf, sizeof levelname_buf, 60);
		case_levelname = levelname_buf; break;
	}
	case_pts_exists = rng_int (0, 1);
}

static void gen_world (void)
{
	int lightdata = rng_int (0, 3) != 0, visdata = rng_int (0, 2) != 0;
	memset (&world_model, 0, sizeof world_model);
	memset (sub_models, 0, sizeof sub_models);
	world_model.lightdata = lightdata ? lightdata_token : NULL;
	world_model.visdata = visdata ? visdata_token : NULL;
	world_model.entities = one_in (10) ? NULL : entities_text;
	case_numsubmodels = ((int[]){ 0, 1, 1, 2, 3, 6, 11, 40, 300 })[rng_int (0, 8)];
	if (one_in (200))
		case_numsubmodels = MAX_MODELS - 1;
	world_model.numsubmodels = case_numsubmodels;
	case_subnull_mod = one_in (4) ? rng_int (2, 5) : 0;
#if HARNESS_CLIENT
	{
		int i, n = rng_int (0, 3) ? rng_int (0, MAXTEX) : 0;
		static const char *texnames[] = {
			"sky1", "sky4", "skybox", "sky", "SKY1", "wall", "sk", "*water"
		};
		memset (tex_store, 0, sizeof tex_store);
		for (i = 0; i < MAXTEX; i++) {
			texture_t *t = &tex_store[i];
			tex_ptrs[i] = one_in (6) ? NULL : t;
			if (one_in (12)) {
				/* a full 16-byte name, no terminator: both sides hand the
				 * same pointer to the same printf */
				memcpy (t->name, "skyaaaaaaaaaaaaa", 16);
			} else {
				snprintf (t->name, sizeof t->name, "%s", texnames[rng_int (0, 7)]);
			}
			t->width = rng_int (0, 2) ? 256 : (unsigned)pick ((int[]){ 128, 255, 257, 512, 0 }, 5);
			t->height = rng_int (0, 2) ? 128 : (unsigned)pick ((int[]){ 64, 127, 129, 256, 0 }, 5);
		}
		world_model.numtextures = n;
		world_model.textures = tex_ptrs;
	}
#endif
}

static void gen_progs (void)
{
	int i, n;
	static const int special[] = { -106, -107, -108, -110, -111, -112, -113, INT_MIN, -1, 0 };
	memset (&case_progs, 0, sizeof case_progs);
	memset (case_funcs, 0, sizeof case_funcs);
	n = rng_int (0, 3) ? rng_int (0, 30) : rng_int (0, MAXFUNCS);
	case_progs.numfunctions = n;
	for (i = 0; i < n; i++) {
		switch (rng_int (0, 5)) {
		case 0: case_funcs[i].first_statement = -rng_int (1, 105); break;
		case 1: case_funcs[i].first_statement = -rng_int (113, 200); break;
		case 2: case_funcs[i].first_statement = rng_int (0, 5000); break;
		default: case_funcs[i].first_statement = rng_int (-1, 1) * rng_int (0, 3); break;
		}
		case_funcs[i].parm_start = rng_int (0, 100);
	}
	if (n > 0 && rng_int (0, 2) == 0)
		case_funcs[rng_int (0, n - 1)].first_statement = pick (special, 10);
	if (n > 1 && one_in (4))
		case_funcs[n - 1].first_statement = pick (special, 10);
	case_progs.entityfields = (int)(sizeof (entvars_t) / 4) + (one_in (2) ? 0 : rng_int (1, 16));
	case_edict_size = case_progs.entityfields * 4 + (int)sizeof (edict_t) - (int)sizeof (entvars_t);
	case_has_alpha = rng_int (0, 2) == 0;
	case_rc_present = rng_int (0, 3) != 0;
}

static void setup_spawn (uint64_t seed)
{
	int i;
	rng_seed (seed);
	memset (&sv, 0, sizeof sv);
	memset (&svs, 0, sizeof svs);
	memset (&sv_globals, 0, sizeof sv_globals);
	memset (clients, 0, sizeof clients);
	memset (inv_pages, 0, sizeof inv_pages);
	memset (preset_items, 0x11, sizeof preset_items);
	memset (preset_pages, 0x22, sizeof preset_pages);
	memset (localmodels, 0, sizeof localmodels);
	for (i = 1; i < MAX_MODELS; i++)
		snprintf (localmodels[i], sizeof localmodels[i], "*%i", i);
	calllog_len = conlog_len = 0;
	calllog[0] = conlog[0] = 0;
	outlen = 0;
	arena_used = 0;
	nallocs = 0;
	setstring_count = 0;
	first_dummy = NULL;
	progs = NULL;
	pr_functions = NULL;
	pr_edict_size = (int)sizeof (edict_t);

	/* cvars */
	set_cvar_string (&hostname, rng_int (0, 2) ? "" : "box");
	hostname.value = 0; hostname.integer = 0;
	set_cvar_value (&coop, one_in (3) ? gen_float_value () : 0.0f);
	set_cvar_value (&deathmatch, one_in (2) ? gen_float_value () : 0.0f);
	{
		static const int skills[] = { -5, -1, 0, 1, 2, 3, 4, 5, 9, INT_MIN, INT_MAX };
		int s = pick (skills, 11);
		set_cvar_value (&skill, (float)s);
		skill.integer = s;
	}
	set_cvar_value (&map_checks, rng_int (0, 2) ? 1.0f : 0.0f);
	set_cvar_value (&developer, one_in (4) ? 1.0f : 0.0f);
	{
		static const float me[] = {
			255.0f, 256.0f, 256.5f, 300.7f, 1024.0f, 31999.9f, 32000.0f,
			32001.0f, -5.0f, NAN, INFINITY, -INFINITY, 1e10f, 255.9f, 600.0f
		};
		set_cvar_value (&max_edicts, me[rng_int (0, 14)]);
	}
	{
		/* past sv_max_edicts, EDICT_NUM Host_Errors: about one case in
		 * twenty */
		int t = ((int[]){ 0, 1, 2, 5, 20, 30, 64, -3 })[rng_int (0, 7)];
		if (one_in (25))
			t = ((int[]){ 250, 300, 20000 })[rng_int (0, 2)];
		set_cvar_value (&max_temp_edicts, (float)t);
	}
	set_cvar_value (&randomclass, gen_float_value ());

	/* engine globals, left as a previous map would */
	sv_protocol = ((int[]){ 18, 19, 20, 21, 100 })[rng_int (0, 4)];
	sv_protocol_userset = one_in (3) ? true : false;
	current_skill = rng_int (-3, 7);
	sv_physics_walk = rng_int (0, 9);
	host_frametime = 0.0137;
	sv_max_edicts = rng_int (0, 40000);
	snprintf (fs_gamedir_nopath, sizeof fs_gamedir_nopath, "%s", rng_int (0, 1) ? "data1" : "portals");
#if HARNESS_CLIENT
	scr_centertime_off = 3.5f;
	total_loading_size = rng_int (0, 200);
	current_loading_size = rng_int (0, 200);
	loading_stage = rng_int (0, 3);
#endif
#if defined(GLQUAKE)
	flush_textures = rng_int (0, 1) ? true : false;
#endif
	for (i = 0; i < 8; i++)
		g_floats[i] = (float)rng_int (-9, 9);
	for (i = 0; i < 4; i++)
		g_strings[i] = rng_int (0, 99);

	/* the server a previous map left */
	sv.active = rng_int (0, 1);
	gen_case_name_pair (0);
	switch (rng_int (0, 5)) {
	case 0: q_strlcpy (sv.name, case_server[0], sizeof sv.name); break;
	case 1: /* the same name, another case */
		q_strlcpy (sv.name, case_server[0], sizeof sv.name);
		for (i = 0; sv.name[i]; i++)
			if (sv.name[i] >= 'a' && sv.name[i] <= 'z')
				sv.name[i] = (char)(sv.name[i] - 32);
		break;
	case 2: sv.name[0] = 0; break;
	default: gen_name (sv.name, sizeof sv.name); break;
	}
	snprintf (sv.startspot, sizeof sv.startspot, "%s", "oldspot");
	if (one_in (30)) {
		/* the caller hands sv.name itself, which Host_ClearMemory zeroes */
		case_server[0] = sv.name;
	}
	if (one_in (30))
		case_startspot[0] = sv.startspot;
	sv.mapchecks.active = rng_int (0, 1);
	sv.ex_items = (ex_item_t *)preset_items;	/* stale, cleared by Host_ClearMemory */

	/* clients */
	svs.maxclients = one_in (4) ? MAX_CLIENTS : rng_int (1, MAX_CLIENTS);
	svs.maxclientslimit = MAX_CLIENTS;
	svs.clients = clients;
	svs.serverflags = ((int[]){ 0, 1, 15, -1, 0x7fffffff, 16777217 })[rng_int (0, 5)];
	svs.changelevel_issued = rng_int (0, 1);
	for (i = 0; i < MAX_CLIENTS; i++) {
		clients[i].active = rng_int (0, 2) == 0;
		clients[i].send_all_v = rng_int (0, 1);
		/* a previous map's page: the dangling pointer uhexen2-rutp was */
		clients[i].ex_inventory = &preset_pages[i % 4];
		clients[i].edict = (edict_t *)(void *)&preset_items[i];
		case_inv_null[i] = one_in (5);
	}
	host_client = one_in (2) ? &clients[rng_int (0, MAX_CLIENTS - 1)] : NULL;

	gen_progs ();
	gen_world ();
	gen_census ();
	case_world_fail[0] = one_in (12);
	case_world_fail[1] = one_in (12);
	case_scribble = one_in (8) ? rng_int (1, 9) : 0;
	case_preset_items = one_in (10);
	case_preset_pages = one_in (10);
	hunk_fill = rng_int (0, 1) ? 0 : 0xa5;
	case_nbuf = one_in (20) ? MAX_SIGNON_BUFFERS - 1 : rng_int (0, 3);
	for (i = 0; i < MAX_SIGNON_BUFFERS; i++)
		case_bufsize[i] = rng_int (0, MAX_SIGNON_SIZE);
	case_cursize = rng_int (0, MAX_SIGNON_SIZE);
	case_two = one_in (6);
	case_cl2 = rng_int (0, 1);
	gen_case_name_pair (1);
	edict_count_hint = svs.maxclients + 1 + max_temp_edicts.integer;
	if (edict_count_hint < 1)
		edict_count_hint = 1;
}

/* ---- running a side ---- */
static int	harness_side;	/* 0 = C, 1 = Rust */

static void spawn_once (int which)
{
	if (harness_side == 0)
		c_SV_SpawnServer (case_server[which], case_startspot[which]);
	else
		SV_SpawnServer (case_server[which], case_startspot[which]);
}

static void run_spawn_child (void)
{
	in_child = 1;
	spawn_once (0);
	serialize_state ("spawn 1");
	if (case_two) {
		first_dummy = sv.model_precache[0];
		svs.changelevel_issued = case_cl2;
		case_two = 2;
		calllog_len = conlog_len = 0;
		spawn_once (1);
		serialize_state ("spawn 2");
	}
	flush_and_exit (0);
}

static int run_forked (void (*body) (void), unsigned char **res, size_t *reslen, int *status)
{
	int fds[2];
	pid_t pid;
	static unsigned char *buf;
	static size_t cap;
	size_t len = 0;

	if (pipe (fds) < 0) { perror ("pipe"); return -1; }
	fflush (stdout);
	pid = fork ();
	if (pid < 0) { perror ("fork"); return -1; }
	if (pid == 0) {
		close (fds[0]);
		out_fd = fds[1];
		outlen = 0;
		body ();
		_exit (5);
	}
	close (fds[1]);
	for (;;) {
		ssize_t r;
		if (len + 65536 > cap) {
			cap = (len + 65536) * 2;
			buf = realloc (buf, cap);
			if (!buf) { perror ("realloc"); exit (3); }
		}
		r = read (fds[0], buf + len, cap - len);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		len += (size_t)r;
	}
	close (fds[0]);
	while (waitpid (pid, status, 0) < 0)
		if (errno != EINTR) { perror ("waitpid"); return -1; }
	*res = buf;
	*reslen = len;
	return 0;
}

static void report_mismatch (const char *kind, uint64_t seed,
			     const unsigned char *a, size_t alen,
			     const unsigned char *b, size_t blen)
{
	size_t i, n = alen < blen ? alen : blen, t;
	for (i = 0; i < n && a[i] == b[i]; i++)
		;
	printf ("MISMATCH %s seed %llu: %zu vs %zu bytes, first difference at %zu",
		kind, (unsigned long long)seed, alen, blen, i);
	/* the tag before the difference */
	for (t = i; t > 0 && !(a[t - 1] == '\n' && t >= 2 && a[t - 2] == '#'); t--)
		;
	for (t = i; t > 1; t--)
		if (a[t - 2] == '\n' && a[t - 1] == '#')
			break;
	if (t > 1) {
		size_t e = t;
		while (e < alen && a[e] != '\n')
			e++;
		printf (" (in #%.*s)", (int)(e - t), a + t);
	}
	printf ("\n");
	if (getenv ("HARNESS_KEEP")) {
		FILE *f = fopen ("mismatch_c.bin", "wb");
		if (f) { fwrite (a, 1, alen, f); fclose (f); }
		f = fopen ("mismatch_rust.bin", "wb");
		if (f) { fwrite (b, 1, blen, f); fclose (f); }
		printf ("  wrote mismatch_c.bin and mismatch_rust.bin\n");
	}
}

static long	spawn_cases, spawn_fatal, spawn_fail_world, spawn_two, spawn_checklists;
static long	spawn_upgrades_114, spawn_upgrades_alpha;

/* SV_SpawnServer's branches whose false arm has no line of its own, so
 * gcov cannot show it ran: how often each arm was taken.  The runner
 * floors both sides in every config. */
enum {
	ARM_ACTIVE,		/* if (sv.active) SV_SendReconnect */
	ARM_CHANGELEVEL,	/* if (svs.changelevel_issued) SaveGamestate */
	ARM_COOP_CVARS,		/* if (coop.integer) Cvar_Set ("deathmatch", "0") */
	ARM_STARTSPOT,		/* if (startspot) q_strlcpy */
	ARM_USERSET,		/* if (!sv_protocol_userset): counted as userset */
	ARM_EX_ITEMS,		/* if (sv.ex_items == NULL) */
	ARM_EX_PAGES,		/* if (sv.ex_inventory_pages == NULL) */
	ARM_RANDOMCLASS,	/* if (sv_globals.randomclass) */
	ARM_COOP_GLOBALS,	/* if (coop.integer) *coop = ... else *deathmatch = ... */
	NUM_ARMS
};
static const char *arm_names[NUM_ARMS] = {
	"active", "changelevel_issued", "coop_cvars", "startspot", "userset",
	"ex_items_null", "ex_pages_null", "randomclass", "coop_globals"
};
static long	arm_counts[NUM_ARMS][2];	/* [arm][taken?] */

static void count_arm (int arm, int taken)
{
	arm_counts[arm][taken ? 1 : 0]++;
}

static int run_spawn_case (uint64_t seed)
{
	unsigned char *r;
	size_t len_c, len_r;
	int st_c, st_r;
	static unsigned char *keep;
	static size_t keepcap;

	setup_spawn (seed);
	harness_side = 0;
	if (run_forked (run_spawn_child, &r, &len_c, &st_c) < 0)
		return 1;
	if (len_c > keepcap) {
		keepcap = len_c * 2;
		keep = realloc (keep, keepcap);
	}
	memcpy (keep, r, len_c);
	harness_side = 1;
	if (run_forked (run_spawn_child, &r, &len_r, &st_r) < 0)
		return 1;
	spawn_cases++;
	if (st_c != st_r) {
		printf ("MISMATCH spawn seed %llu: the C child exited %#x, the Rust child %#x\n",
			(unsigned long long)seed, st_c, st_r);
		return 1;
	}
	if (!WIFEXITED (st_c) || (WEXITSTATUS (st_c) != 0 && WEXITSTATUS (st_c) != 42)) {
		printf ("FAIL spawn seed %llu: both children died the same unexpected way (%#x); harness bug\n",
			(unsigned long long)seed, st_c);
		return 1;
	}
	if (len_c != len_r || memcmp (keep, r, len_c) != 0) {
		report_mismatch ("spawn", seed, keep, len_c, r, len_r);
		return 1;
	}
	if (WEXITSTATUS (st_c) == 42)
		spawn_fatal++;
	if (memmem (keep, len_c, "Couldn't spawn server", 21))
		spawn_fail_world++;
	if (memmem (keep, len_c, "#spawn 2", 8))
		spawn_two++;
	if (memmem (keep, len_c, "Map checklist (", 15))
		spawn_checklists++;
	if (memmem (keep, len_c, "extended builtins", 17))
		spawn_upgrades_114++;
	if (memmem (keep, len_c, "Progs uses .alpha", 17))
		spawn_upgrades_alpha++;
	/* Both arms of each branch line coverage cannot see, for the first
	 * spawn, from the inputs the parent set: the early tests are always
	 * reached (nothing before them is fatal); the late ones only when the
	 * first spawn returned ("#spawn 1" is written after the call) with
	 * its world loaded. */
	{
		int late = memmem (keep, len_c, "\n#spawn 1\n", 10) && !case_world_fail[0];
		count_arm (ARM_ACTIVE, sv.active);
		count_arm (ARM_CHANGELEVEL, svs.changelevel_issued);
		count_arm (ARM_COOP_CVARS, coop.integer != 0);
		count_arm (ARM_STARTSPOT, case_startspot[0] != NULL);
		count_arm (ARM_USERSET, sv_protocol_userset);
		if (late) {
			count_arm (ARM_EX_ITEMS, !case_preset_items);
			count_arm (ARM_EX_PAGES, !case_preset_pages);
			count_arm (ARM_RANDOMCLASS, case_rc_present);
			count_arm (ARM_COOP_GLOBALS, coop.integer != 0);
		}
	}
	return 0;
}

/* The directed case: MAX_MODELS submodels.  The C indexes localmodels[],
 * sv.model_precache[] and sv.models[] one past their ends; the port's
 * index check must abort before it writes anything out of bounds. */
static void run_abort_child (void)
{
	in_child = 1;
	SV_SpawnServer (case_server[0], case_startspot[0]);
	flush_and_exit (0);
}

static int run_abort_case (void)
{
	unsigned char *r;
	size_t len;
	int st;
	setup_spawn (12345);
	case_world_fail[0] = 0;
	world_model.numsubmodels = MAX_MODELS;
	if (run_forked (run_abort_child, &r, &len, &st) < 0)
		return 1;
	if (!WIFSIGNALED (st) || WTERMSIG (st) != SIGABRT) {
		printf ("FAIL abort case: MAX_MODELS submodels did not abort the port (status %#x)\n", st);
		return 1;
	}
	return 0;
}

/* ---- the checklist alone, in process ---- */
static void setup_checklist (uint64_t seed)
{
	static edict_t	e0;
	rng_seed (seed);
	memset (&sv, 0, sizeof sv);
	memset (&e0, 0, sizeof e0);
	calllog_len = conlog_len = 0;
	calllog[0] = conlog[0] = 0;
	outlen = 0;
	gen_world ();
	gen_census ();
	sv.worldmodel = &world_model;
	sv.edicts = &e0;
	gen_name (sv.name, sizeof sv.name);
	switch (rng_int (0, 5)) {
	case 0: q_snprintf (sv.modelname, sizeof sv.modelname, "maps/%s.bsp", sv.name); break;
	case 1: snprintf (sv.modelname, sizeof sv.modelname, "%s", "maps/sub/dir/y.bsp"); break;
	case 2: snprintf (sv.modelname, sizeof sv.modelname, "%s", "nodir"); break;
	case 3: sv.modelname[0] = 0; break;
	case 4: snprintf (sv.modelname, sizeof sv.modelname, "%s", "maps/"); break;
	default: snprintf (sv.modelname, sizeof sv.modelname, "%s", "a\\b\\c.bsp"); break;
	}
	sv.mapchecks.active = 1;
	sv.mapchecks.numwarnings = case_census[0];
	sv.mapchecks.trigger_changelevel = case_census[1];
	sv.mapchecks.valid_changelevel = case_census[2];
	sv.mapchecks.intermission = case_census[3];
	sv.mapchecks.skill_triggers = case_census[4];
	sv.mapchecks.coop_spawns = case_census[5];
	sv.mapchecks.dm_spawns = case_census[6];
	sv.mapchecks.sp_spawns = case_census[7];
	sv.mapchecks.skill_ents[0] = case_census[8];
	sv.mapchecks.skill_ents[1] = case_census[9];
	sv.mapchecks.skill_ents[2] = case_census[10];
	/* the thresholds, at and either side of each */
	if (one_in (3))
		sv.mapchecks.coop_spawns = rng_int (-1, 4);
	if (one_in (3))
		sv.mapchecks.dm_spawns = rng_int (-1, 6);
	if (one_in (4))
		sv.mapchecks.trigger_changelevel = rng_int (0, 2);
	if (one_in (4))
		sv.mapchecks.valid_changelevel = rng_int (0, 2);
	q_strlcpy (sv.mapchecks.changelevel, case_changelevel, sizeof sv.mapchecks.changelevel);
	q_strlcpy (sv.mapchecks.changespot, case_changespot, sizeof sv.mapchecks.changespot);
	q_strlcpy (sv.midi_name, case_midi, sizeof sv.midi_name);
	sv.cd_track = (byte)case_cd;
	e0.v.message = case_message;
	if (one_in (5)) {
		/* a clean map, so the footer's zero and one-warning arms run,
		 * then sometimes one check broken */
		world_model.lightdata = lightdata_token;
		world_model.visdata = visdata_token;
#if HARNESS_CLIENT
		world_model.numtextures = rng_int (0, 1);
		tex_ptrs[0] = &tex_store[0];
		snprintf (tex_store[0].name, sizeof tex_store[0].name, "%s", "sky1");
		tex_store[0].width = 256;
		tex_store[0].height = 128;
#endif
		sv.mapchecks.numwarnings = 0;
		sv.mapchecks.trigger_changelevel = sv.mapchecks.valid_changelevel = rng_int (1, 3);
		sv.mapchecks.skill_triggers = 1;
		sv.mapchecks.sp_spawns = rng_int (1, 2);
		sv.mapchecks.coop_spawns = rng_int (3, 4);
		sv.mapchecks.dm_spawns = rng_int (5, 6);
		sv.cd_track = 3;
		case_levelname = "Blackmarsh";
		switch (rng_int (0, 9)) {
		case 0: world_model.lightdata = NULL; break;
		case 1: sv.mapchecks.coop_spawns = 2; break;
		case 2: sv.mapchecks.dm_spawns = 4; break;
		case 3: sv.mapchecks.sp_spawns = 0; break;
		case 4: case_levelname = ""; break;
		default: break;
		}
	}
}

static void checklist_side (int side, uint64_t seed)
{
	setup_checklist (seed);
	if (side == 0)
		c_ref_PrintMapChecklist ();
	else
		SvMainSpawn_TestPrintMapChecklist ();
	out_tag ("calllog");
	out_bytes (calllog, calllog_len);
	out_tag ("conlog");
	out_bytes (conlog, conlog_len);
	out_tag ("numwarnings");
	out_int (sv.mapchecks.numwarnings);
	out_tag ("sv whole");
	out_u64 (hash_bytes (&sv, sizeof sv));
}

static long	checklist_cases, checklist_warnings_shapes[3];

static int run_checklist_case (uint64_t seed)
{
	static unsigned char *keep;
	static size_t keepcap;
	size_t len_c;
	checklist_side (0, seed);
	len_c = outlen;
	if (len_c > keepcap) {
		keepcap = len_c * 2;
		keep = realloc (keep, keepcap);
	}
	memcpy (keep, outbuf, len_c);
	checklist_side (1, seed);
	checklist_cases++;
	if (outlen != len_c || memcmp (keep, outbuf, len_c) != 0) {
		report_mismatch ("checklist", seed, keep, len_c, outbuf, outlen);
		return 1;
	}
	if (strstr (conlog, "Nothing to look at."))
		checklist_warnings_shapes[0]++;
	else if (strstr (conlog, " 1 item to look at."))
		checklist_warnings_shapes[1]++;
	else
		checklist_warnings_shapes[2]++;
	return 0;
}

/* ---- SV_MapCheckTitle ---- */
static long	title_cases;

static int run_title_case (uint64_t seed)
{
	static char in[3000];
	char a[1100], b[1100];
	size_t outsize;
	const char *src = in;
	rng_seed (seed);
	if (one_in (3))
		gen_text (in, sizeof in, 40);
	else if (one_in (20)) {
		memset (in, 'w', 2500);
		in[2500] = 0;
	} else
		gen_text (in, sizeof in, 8);
	if (one_in (40))
		src = NULL;
	outsize = rng_int (0, 3) ? (size_t)rng_int (1, 12) : (one_in (2) ? 1024 : (size_t)rng_int (1, 1024));
	memset (a, 0xee, sizeof a);
	memset (b, 0xee, sizeof b);
	c_ref_MapCheckTitle (a, outsize, src);
	SvMainSpawn_TestMapCheckTitle (b, outsize, src);
	title_cases++;
	if (memcmp (a, b, sizeof a) != 0) {
		printf ("MISMATCH title seed %llu: outsize %zu, \"%s\" vs \"%s\"\n",
			(unsigned long long)seed, outsize, a, b);
		return 1;
	}
	return 0;
}

/* ---- SV_MapCheckThresh ---- */
static long	thresh_cases;

static int run_thresh_case (int current, int target)
{
	int a = (int)c_ref_MapCheckThresh (current, target);
	int b = SvMainSpawn_TestMapCheckThresh (current, target);
	thresh_cases++;
	if (a != b) {
		printf ("MISMATCH thresh (%d, %d): C %d, Rust %d\n", current, target, a, b);
		return 1;
	}
	return 0;
}

/* ---- main ---- */
int main (int argc, char **argv)
{
	long scale = 1, i;
	long n_spawn, n_check, n_title, n_thresh;
	uint64_t seed0 = 1;
	int fails = 0;
	const char *s;
	(void)argc; (void)argv;

	setvbuf (stdout, NULL, _IOLBF, 0);
	if ((s = getenv ("HARNESS_SCALE")) != NULL)
		scale = atol (s);
	if ((s = getenv ("HARNESS_SEED")) != NULL)
		seed0 = strtoull (s, NULL, 0);
	if (scale < 1)
		scale = 1;

	if (check_facts ())
		return 1;

	/* the hunk: room for sixteen clients' states and a full edict list
	 * of the largest progs, never touched by the parent */
	arena_cap = (size_t)MAX_CLIENTS * sizeof (client_state2_t)
		  + (size_t)MAX_EDICTS * ((size_t)sizeof (edict_t) + 64)
		  + (size_t)MAX_ITEMS_EX * sizeof (ex_item_t)
		  + (size_t)MAX_CLIENTS * MAX_INVENTORY_EX_PAGES * sizeof (ex_inventory_page_t)
		  + (1u << 20);
	arena = mmap (NULL, arena_cap, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (arena == MAP_FAILED) { perror ("mmap"); return 1; }

	n_spawn = 4000 * scale;
	n_check = 20000 * scale;
	n_title = 40000 * scale;
	n_thresh = 20000 * scale;
	if ((s = getenv ("HARNESS_SPAWN")) != NULL)
		n_spawn = atol (s);

	for (i = 0; i < n_spawn && fails < 5; i++)
		fails += run_spawn_case (seed0 + (uint64_t)i);
	if (!fails)
		fails += run_abort_case ();
	for (i = 0; i < n_check && fails < 5; i++)
		fails += run_checklist_case (seed0 * 7919 + (uint64_t)i);
	for (i = 0; i < n_title && fails < 5; i++)
		fails += run_title_case (seed0 * 104729 + (uint64_t)i);
	{
		static const int edge[] = {
			INT_MIN, INT_MIN + 1, -2, -1, 0, 1, 2, 3, 4, 5, 6, INT_MAX - 1, INT_MAX
		};
		int a, b;
		for (a = 0; a < 13; a++)
			for (b = 0; b < 13; b++)
				fails += run_thresh_case (edge[a], edge[b]);
		rng_seed (seed0);
		for (i = 0; i < n_thresh && fails < 5; i++) {
			int c = (int)(uint32_t)rng_next ();
			int t = (int)(uint32_t)rng_next ();
			if (rng_int (0, 1)) {
				c = rng_int (-3, 8);
				t = rng_int (-3, 8);
			}
			fails += run_thresh_case (c, t);
		}
	}

	if (fails) {
		printf ("RESULT: FAIL (%d)\n", fails);
		return 1;
	}
	printf ("outcomes: spawn %ld (fatal %ld, world missing %ld, changelevel %ld, "
		"checklist printed %ld, upgraded to 21 %ld, to 100 %ld)\n",
		spawn_cases, spawn_fatal, spawn_fail_world, spawn_two,
		spawn_checklists, spawn_upgrades_114, spawn_upgrades_alpha);
	printf ("          checklist %ld (nothing to look at %ld, one item %ld, more %ld), "
		"title %ld, thresh %ld, abort 1\n",
		checklist_cases, checklist_warnings_shapes[0], checklist_warnings_shapes[1],
		checklist_warnings_shapes[2], title_cases, thresh_cases);
	{
		int a;
		printf ("          arms (taken/not):");
		for (a = 0; a < NUM_ARMS; a++)
			printf (" %s %ld/%ld", arm_names[a], arm_counts[a][1], arm_counts[a][0]);
		printf ("\n");
	}
	printf ("sv_main_spawn %s config: PASS (%ld cases)\n", HARNESS_CONFIG,
		spawn_cases + checklist_cases + title_cases + thresh_cases + 1);
	printf ("RESULT: PASS\n");
	return 0;
}
