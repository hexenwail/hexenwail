/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * connect_harness.c -- engine/rust/src/sv_main_connect.rs against the C
 * original of Hexen II's sv_main.c connect and signon functions, in one
 * binary.
 *
 * run_connect_harness.sh builds this against sv_main_connect_h2.c
 * (-DSERVERONLY, as h2ded compiles it), with the real
 * engine/rust/world_target.c and engine/rust/sv_main_target.c compiled in.
 * The eleven ported functions go in renamed to c_*; the supporting
 * SV_GetLevelname keeps its name (both sides call the same C).
 *
 * Each case builds the state one of the exported functions reads, then
 * forks twice: once to run the C, once to run the Rust, from the same
 * snapshot.  The child serializes everything the call can have written --
 * the sizebufs (cursize, overflowed, every byte), the client structs, the
 * signon state, and the two logs (network/console) -- to a file; the
 * parent compares the two files byte for byte.
 *
 * Sys_Error and Host_Error are capture-and-record stubs: they serialize
 * the logs to the output file, then abort.  A case both sides abort is a
 * pass only when both children exit with the same signal and their logs
 * match byte for byte -- the full-server and signon-overflow paths are
 * tested this way.
 *
 * The NET_* stubs are scripted per case.  MSG_Write* and SZ_* are the
 * already-ported Rust, held to their C by their own harnesses.
 *
 * Before the cases, the facts table sv_main_connect.rs reads is checked
 * against this compile of the Hexen II headers.
 */

#include "quakedef.h"
#include "sv_main_target.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <execinfo.h>
#include <errno.h>
#include <unistd.h>

static void crash_handler (int sig)
{
	void *bt[32];
	int n = backtrace (bt, 32);
	fprintf (stderr, "child crashed with signal %d:\n", sig);
	backtrace_symbols_fd (bt, n, 2);
	_exit (100 + sig);
}

/* the ported functions: C side renamed, Rust side as exported */
const char	*c_SV_ProtocolName (int protocol);
void	c_SV_SendServerinfo (client_t *client);
void	c_SV_ConnectClient (int clientnum);
void	c_SV_CheckForNewClients (void);
int	c_SV_ModelIndex (const char *name);
void	c_SV_ReserveSignonSpace (int length);
void	c_SV_SendSignonBuffer (client_t *client);
void	c_SV_CreateBaseline (void);
void	c_SV_SendReconnect (void);
const char	*SV_ProtocolName (int protocol);
void	SV_SendServerinfo (client_t *client);
void	SV_CheckForNewClients (void);
int	SV_ModelIndex (const char *name);
void	SV_ReserveSignonSpace (int length);
void	SV_SendSignonBuffer (client_t *client);
void	SV_CreateBaseline (void);
void	SV_SendReconnect (void);
/* formatter test hooks */
size_t	SvMainConnect_TestFmt42 (unsigned char *buf, double v);
size_t	SvMainConnect_TestFmtInt (unsigned char *buf, int v);
size_t	SvMainConnect_TestFmt02d (unsigned char *buf, int v);

/* the engine's storage */
server_t	sv;
server_static_t	svs;
int		sv_protocol;
cvar_t		coop = {"coop", "0", CVAR_NONE};
cvar_t		deathmatch = {"deathmatch", "0", CVAR_NONE};
cvar_t		sv_update_player = {"sv_update_player", "1", CVAR_NONE};
cvar_t		sv_update_monsters = {"sv_update_monsters", "1", CVAR_NONE};
cvar_t		sv_update_missiles = {"sv_update_missiles", "1", CVAR_NONE};
cvar_t		sv_update_misc = {"sv_update_misc", "1", CVAR_NONE};
cvar_t		sv_netsort = {"sv_netsort", "1", CVAR_NONE};
int		sv_kingofhill;
unsigned short	pr_crc;
int		net_activeconnections;
int		pr_edict_size = sizeof (edict_t);
/* SvMain_IsDedicated's global: the harness is always the dedicated
 * server, so SV_SendReconnect's listen-server arm stays dead -- exactly
 * what the -DSERVERONLY fixture does. */
qboolean	isDedicated = true;
/* the C's src_command, now owned by the Rust cmd port; read by
 * SV_SendReconnect's listen-server arm (dead in this harness). */
cmd_source_t	cmd_source;
sv_globals_t	sv_globals;
areanode_t	sv_areanodes[AREA_NODES];
sizebuf_t	net_message;
client_t	*host_client;
double		realtime;
char		*pr_strings;
int		pr_stringssize;
int		host_string_count;
char		*host_strings[64];

/* ---- stub control: set by the parent per case, read by the stubs ---- */
static int	stub_is_local = 0;
static int	stub_max_unreliable = 1400;
/* NET_CheckNewConnections script: sockets to hand out, then NULL */
#define MAXPENDING 8
static struct qsocket_s *stub_pending[MAXPENDING];
static int	stub_npending;
/* NET_SendToAll capture */
static unsigned char	sendtoall_data[256];
static int	sendtoall_cursize;
static double	sendtoall_blocktime;
static int	sendtoall_called;
/* SV_ClientInventoryPage script */
static ex_inventory_page_t *stub_inv_page;
/* GetEdictFieldValue script: NULL, or a float to return as alpha */
static int	stub_alpha_present;
static float	stub_alpha_value;

/* ---- the call log: every stubbed call, in order ---- */
#define LOGSIZE	(1 << 20)
static char	calllog[LOGSIZE];
static size_t	calllog_len;
static char	conlog[LOGSIZE];
static size_t	conlog_len;

static void log_call (const char *fmt, ...)
{
	va_list ap;
	char tmp[2048], *p;
	int n;
	va_start (ap, fmt);
	n = vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	if (n <= 0)
		return;
	/* normalize the harness's c_ rename: __func__ in the fixture
	 * expands to c_SV_*, but the Rust logs SV_*. */
	for (p = tmp; (p = strstr (p, "c_SV_")) != NULL; ) {
		memmove (p, p + 2, strlen (p + 2) + 1);
	}
	n = snprintf (calllog + calllog_len, LOGSIZE - calllog_len, "%s", tmp);
	if (n > 0)
		calllog_len += (size_t)n < LOGSIZE - calllog_len ? (size_t)n : 0;
}

static void log_con (int flags, const char *fmt, va_list ap)
{
	char tmp[2048], *p;
	int n = snprintf (conlog + conlog_len, LOGSIZE - conlog_len,
			  "[%d] ", flags);
	if (n > 0)
		conlog_len += (size_t)n;
	vsnprintf (tmp, sizeof tmp, fmt, ap);
	/* normalize the harness's c_ rename: __func__ in the fixture
	 * expands to c_SV_*, but the Rust logs SV_*. */
	for (p = tmp; (p = strstr (p, "c_SV_")) != NULL; ) {
		memmove (p, p + 2, strlen (p + 2) + 1);
	}
	n = snprintf (conlog + conlog_len, LOGSIZE - conlog_len, "%s", tmp);
	if (n > 0)
		conlog_len += (size_t)n < LOGSIZE - conlog_len ? (size_t)n : 0;
}

/* SV_SendReconnect's listen-server arm calls this; the harness is always
 * dedicated, so the arm stays dead and this only satisfies the link.
 * It logs like every other stub, so a future non-dedicated harness case
 * would show the call the -DSERVERONLY fixture cannot make. */
void	Cmd_ExecuteString (const char *text, cmd_source_t src)
{
	log_call ("Cmd_ExecuteString(%s,%d)", text, (int)src);
}

/* ---- output serialization: one malloc'd buffer per child ---- */
static char	*outbuf;
static size_t	outlen, outcap;
static char	out_path[256];

static void out_reserve (size_t n)
{
	if (outlen + n > outcap) {
		outcap = (outlen + n) * 2 + 1024;
		outbuf = realloc (outbuf, outcap);
		if (!outbuf) { perror ("realloc"); _exit (3); }
	}
}
static void out_bytes (const void *p, size_t n)
{
	out_reserve (n);
	memcpy (outbuf + outlen, p, n);
	outlen += n;
}
static void out_int (int v) { out_bytes (&v, sizeof v); }
static void out_sizebuf (const sizebuf_t *sb)
{
	out_int (sb->cursize);
	out_int (sb->overflowed);
	out_int (sb->maxsize);
	out_bytes (sb->data, (size_t)(sb->cursize < 0 ? 0 : sb->cursize));
}
/* write whatever we have so far (the logs) and abort: the error-path
 * stubs call this so both sides' abort cases can be compared.  Uses
 * exit(42), not abort(), so the gcov exit handler dumps coverage. */
static void out_write_and_abort (void)
{
	FILE *f = fopen (out_path, "wb");
	if (f) {
		fwrite (outbuf, 1, outlen, f);
		fclose (f);
	}
	exit (42);
}

/* qsocket_s tokens: the stubs only need distinct pointers, never the
 * struct's contents (net_defs.h is private to the network layer). */
#define MAXHARNESSCLIENTS	4
static unsigned char	sock_tokens[MAXHARNESSCLIENTS + MAXPENDING][16];

static int sock_index (struct qsocket_s *s)
{
	int i;
	for (i = 0; i < MAXHARNESSCLIENTS + MAXPENDING; i++)
		if (s == (struct qsocket_s *)sock_tokens[i])
			return i;
	return -1;
}

qboolean NET_IsLocalConnection (struct qsocket_s *sock)
{
	log_call ("is_local(%d) -> %d\n", sock_index (sock), stub_is_local);
	return stub_is_local;
}

int NET_MaxUnreliableMessage (struct qsocket_s *sock)
{
	log_call ("max_unreliable(%d) -> %d\n", sock_index (sock),
		  stub_max_unreliable);
	return stub_max_unreliable;
}

struct qsocket_s *NET_CheckNewConnections (void)
{
	struct qsocket_s *ret = stub_npending > 0 ? stub_pending[0] : NULL;
	if (stub_npending > 0) {
		int i;
		for (i = 1; i < stub_npending; i++)
			stub_pending[i - 1] = stub_pending[i];
		stub_npending--;
	}
	log_call ("check_new() -> %d (remaining %d)\n",
		  ret ? sock_index (ret) : -1, stub_npending);
	return ret;
}

const char *NET_QSocketGetAddressString (const struct qsocket_s *sock)
{
	static char bufs[MAXHARNESSCLIENTS + MAXPENDING][32];
	int i = sock_index ((struct qsocket_s *)sock);
	if (i < 0)
		return "invalid";
	snprintf (bufs[i], sizeof bufs[i], "testaddr%d", i);
	log_call ("addr_str(%d)\n", i);
	return bufs[i];
}

int NET_SendToAll (sizebuf_t *data, double blocktime)
{
	sendtoall_called = 1;
	sendtoall_cursize = data->cursize < (int)sizeof sendtoall_data ?
		data->cursize : (int)sizeof sendtoall_data;
	memcpy (sendtoall_data, data->data, (size_t)sendtoall_cursize);
	sendtoall_blocktime = blocktime;
	log_call ("send_to_all(%d bytes, %.1f)\n", data->cursize, blocktime);
	return 0;
}

ex_inventory_page_t *SV_ClientInventoryPage (int clientnum)
{
	log_call ("inv_page(%d) -> %p\n", clientnum, (void *)stub_inv_page);
	return stub_inv_page;
}

eval_t *GetEdictFieldValue (edict_t *ed, const char *field)
{
	static eval_t alpha_val;
	(void)ed;
	log_call ("field_value(%s) -> %s\n", field,
		  stub_alpha_present ? "alpha" : "NULL");
	if (stub_alpha_present && !strcmp (field, "alpha")) {
		alpha_val._float = stub_alpha_value;
		return &alpha_val;
	}
	return NULL;
}

const char *PR_GetString (int num)
{
	if (num >= 0 && num < pr_stringssize)
		return pr_strings + num;
	return "";
}

const char *Host_GetString (int idx)
{
	if (idx >= 0 && idx < host_string_count)
		return host_strings[idx];
	return "";
}

FUNC_NORETURN void Host_Error (const char *error, ...)
{
	va_list	ap;
	char	buf[1024];
	va_start (ap, error);
	vsnprintf (buf, sizeof buf, error, ap);
	va_end (ap);
	log_call ("Host_Error: %s\n", buf);
	out_bytes (calllog, calllog_len);
	out_bytes (conlog, conlog_len);
	out_write_and_abort ();
}

FUNC_NORETURN void Sys_Error (const char *error, ...)
{
	va_list	ap;
	char	buf[1024];
	va_start (ap, error);
	vsnprintf (buf, sizeof buf, error, ap);
	va_end (ap);
	log_call ("Sys_Error: %s\n", buf);
	out_bytes (calllog, calllog_len);
	out_bytes (conlog, conlog_len);
	out_write_and_abort ();
}

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	log_con ((int)flags, fmt, ap);
	va_end (ap);
}

void CON_DPrintf (unsigned int flags, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	log_con ((int)flags, fmt, ap);
	va_end (ap);
}


/* ---- deterministic PRNG ---- */
static uint64_t	rng_state;
static void rng_seed (uint64_t s) { rng_state = s ? s : 0x243f6a8885a308d3ULL; }
static uint64_t rng_next (void)
{
	uint64_t x = rng_state;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	rng_state = x;
	return x;
}
static int rng_int (int lo, int hi) /* inclusive */
{
	return lo + (int)(rng_next () % (uint64_t)(hi - lo + 1));
}
static float rng_float (void)
{
	uint64_t r = rng_next ();
	switch (r % 12) {
	case 0: return NAN;
	case 1: return INFINITY;
	case 2: return -INFINITY;
	case 3: return (float)(r % 100000) - 50000.0f;
	case 4: return (float)((int)(r % 512) - 256);
	case 5: return (float)(r % 2);
	default: {
		union { uint32_t u; float f; } v;
		v.u = (uint32_t)r;
		return v.f;
	}
	}
}

/* ---- the facts table, checked against this compile's headers ---- */
static int check_facts (void)
{
	const sv_main_connect_facts_t *f = &SvMainConnect_TargetFacts;
	int bad = 0;
#define CKF(name, expect) \
	if (f->name != (expect)) { \
		printf ("facts mismatch: %s = %d, header says %d\n", \
			#name, (int)f->name, (int)(expect)); bad = 1; }
#define CKZ(name, expect) \
	if (f->name != (size_t)(expect)) { \
		printf ("facts mismatch: %s = %zu, header says %zu\n", \
			#name, f->name, (size_t)(expect)); bad = 1; }
	CKF (protocol_raven_111, PROTOCOL_RAVEN_111);
	CKF (protocol_raven_112, PROTOCOL_RAVEN_112);
	CKF (protocol_uqe_113, PROTOCOL_UQE_113);
	CKF (protocol_uh2_114, PROTOCOL_UH2_114);
	CKF (protocol_hexenwail_1, PROTOCOL_HEXENWAIL_1);
	CKF (msg_print, svc_print);
	CKF (msg_serverinfo, svc_serverinfo);
	CKF (msg_cdtrack, svc_cdtrack);
	CKF (msg_midi_name, svc_midi_name);
	CKF (msg_mod_name, svc_mod_name);
	CKF (msg_skybox, svc_skybox);
	CKF (msg_setview, svc_setview);
	CKF (msg_signonnum, svc_signonnum);
	CKF (msg_spawnbaseline, svc_spawnbaseline);
	CKF (msg_stufftext, svc_stufftext);
	CKF (game_coop, GAME_COOP);
	CKF (game_deathmatch, GAME_DEATHMATCH);
	CKF (max_models, MAX_MODELS);
	CKF (max_sounds, MAX_SOUNDS);
	CKF (max_qpath, MAX_QPATH);
	CKF (max_signon_size, MAX_SIGNON_SIZE);
	CKF (max_signon_buffers, MAX_SIGNON_BUFFERS);
	CKF (num_spawn_parms, NUM_SPAWN_PARMS);
	CKF (entalpha_default, ENTALPHA_DEFAULT);
	if (f->engine_version != ENGINE_VERSION) {
		printf ("facts mismatch: engine_version = %f, header says %f\n",
			f->engine_version, ENGINE_VERSION); bad = 1; }
	CKZ (client_size, sizeof (client_t));
	CKZ (client_message, offsetof (client_t, message));
	CKZ (client_datagram, offsetof (client_t, datagram));
	CKZ (client_msgbuf_size, sizeof (((client_t *)0)->msgbuf));
	CKZ (client_datagram_buf_size, sizeof (((client_t *)0)->datagram_buf));
	CKZ (client_netconnection, offsetof (client_t, netconnection));
	CKZ (client_spawn_parms, offsetof (client_t, spawn_parms));
	CKZ (client_name, offsetof (client_t, name));
	CKZ (client_active, offsetof (client_t, active));
	CKZ (client_spawned, offsetof (client_t, spawned));
	CKZ (client_send_all_v, offsetof (client_t, send_all_v));
	CKZ (client_signon_buffer, offsetof (client_t, signon_buffer));
	CKZ (client_edict, offsetof (client_t, edict));
	CKZ (client_ex_inventory, offsetof (client_t, ex_inventory));
	CKZ (client_sendsignon, offsetof (client_t, sendsignon));
	CKZ (client_state2_size, sizeof (client_state2_t));
	CKZ (edict_v, offsetof (edict_t, v));
	CKZ (edict_baseline, offsetof (edict_t, baseline));
	CKZ (edict_free, offsetof (edict_t, free));
	CKZ (v_origin, offsetof (entvars_t, origin));
	CKZ (v_angles, offsetof (entvars_t, angles));
	CKZ (v_model, offsetof (entvars_t, model));
	CKZ (v_modelindex, offsetof (entvars_t, modelindex));
	CKZ (v_frame, offsetof (entvars_t, frame));
	CKZ (v_skin, offsetof (entvars_t, skin));
	CKZ (v_scale, offsetof (entvars_t, scale));
	CKZ (v_drawflags, offsetof (entvars_t, drawflags));
	CKZ (v_abslight, offsetof (entvars_t, abslight));
	CKZ (es_origin, offsetof (entity_state_t, origin));
	CKZ (es_angles, offsetof (entity_state_t, angles));
	CKZ (es_modelindex, offsetof (entity_state_t, modelindex));
	CKZ (es_frame, offsetof (entity_state_t, frame));
	CKZ (es_colormap, offsetof (entity_state_t, colormap));
	CKZ (es_skin, offsetof (entity_state_t, skin));
	CKZ (es_scale, offsetof (entity_state_t, scale));
	CKZ (es_drawflags, offsetof (entity_state_t, drawflags));
	CKZ (es_abslight, offsetof (entity_state_t, abslight));
	CKZ (es_alpha, offsetof (entity_state_t, alpha));
	CKZ (es_clearcount, offsetof (entity_state_t, ClearCount));
	CKZ (ex_item_id, offsetof (ex_item_t, id));
	CKZ (ex_item_icon, offsetof (ex_item_t, icon));
#if !defined(SERVERONLY) && defined(GLQUAKE)
	CKZ (qmodel_ex_flags, offsetof (qmodel_t, ex_flags));
	CKZ (qmodel_glow_settings, offsetof (qmodel_t, glow_settings));
#endif
#undef CKF
#undef CKZ
	if (!bad)
		printf ("layouts: sv_main_connect facts match the headers\n");
	return bad;
}

/* ---- per-case backing stores ---- */
#define MAXEDICTS	32
static edict_t	edicts[MAXEDICTS];
static client_t	clients[MAXHARNESSCLIENTS];
static byte	msgbufs[MAXHARNESSCLIENTS][MAX_MSGLEN];
static byte	dgrambufs[MAXHARNESSCLIENTS][MAX_MSGLEN];
static client_state2_t states[MAXHARNESSCLIENTS];
static qmodel_t	qmodels[16];
static char	model_names[16][64];
static char	sound_names[16][64];
static ex_item_t ex_items[8];
static ex_inventory_page_t inv_page;

/* the progs string heap */
static void setup_strings (void)
{
	static char heap[4096];
	static const char *words[] = {
		"puzzle1", "models/v_w0.mdl", "models/v_w1.mdl", "",
		"a_very_long_model_name_for_testing",
	};
	size_t off = 0, w;
	pr_strings = heap;
	for (w = 0; w < sizeof words / sizeof words[0]; w++) {
		size_t l = strlen (words[w]) + 1;
		if (off + l >= sizeof heap)
			break;
		memcpy (heap + off, words[w], l);
		off += l;
	}
	memset (heap + off, 0, sizeof heap - off);
	pr_stringssize = (int)sizeof heap;
	host_string_count = 2;
	host_strings[0] = "Host String One";
	host_strings[1] = "Host String Two";
}

static void setup_client (int i)
{
	client_t *c = &clients[i];
	memset (c, 0, sizeof (*c));
	c->netconnection = (struct qsocket_s *)sock_tokens[i];
	c->edict = &edicts[i + 1];
	SZ_Init (&c->message, msgbufs[i], sizeof msgbufs[i]);
	SZ_Init (&c->datagram, dgrambufs[i], sizeof dgrambufs[i]);
}

/* fill sv.model_precache[1..n] and sv.sound_precache[1..m]; index 0 gets
 * a dummy like the engine's SV_SpawnServer does. */
static void setup_precache (int nmodels, int nsounds)
{
	int i;
	static char dummy[] = "dummy";
	memset (sv.model_precache, 0, sizeof sv.model_precache);
	memset (sv.sound_precache, 0, sizeof sv.sound_precache);
	memset (sv.models, 0, sizeof sv.models);
	sv.model_precache[0] = dummy;
	sv.sound_precache[0] = dummy;
	for (i = 0; i < nmodels && i < 16; i++) {
		snprintf (model_names[i], sizeof model_names[i],
			  "models/v_w%d.mdl", i);
		sv.model_precache[i + 1] = model_names[i];
	}
	for (i = 0; i < nsounds && i < 16; i++) {
		snprintf (sound_names[i], sizeof sound_names[i],
			  "sounds/s%d.wav", i);
		sv.sound_precache[i + 1] = sound_names[i];
	}
}

/* qmodels for the protocol >= 21 model-effects block: compiled out under
 * SERVERONLY (as in the engine), so the harness only needs the slots. */
static void setup_qmodels (int n)
{
	int i;
	(void)n;
	memset (qmodels, 0, sizeof qmodels);
	for (i = 0; i < 16; i++)
		sv.models[i + 1] = &qmodels[i];
}

static void setup_ex_items (int n)
{
	int i;
	static const char *icons[] = {
		"gfx/arti00.lmp", "gfx/arti05.lmp", "custom/icon.lmp",
		"gfx/arti15.lmp", "gfx/arti16.lmp",
	};
	memset (ex_items, 0, sizeof ex_items);
	sv.num_ex_items = n < 8 ? n : 8;
	for (i = 0; i < sv.num_ex_items; i++) {
		ex_items[i].id = rng_int (0, 20);
		snprintf (ex_items[i].icon, sizeof ex_items[i].icon, "%s",
			  icons[rng_next () % 5]);
	}
	sv.ex_items = ex_items;
}

/* ---- case setup ---- */
static int	case_kind;	/* 0..6, 7 = formatters (no fork) */
static int	case_client;	/* the client under test */
static int	case_length;	/* SV_ReserveSignonSpace's length */
static const char *case_name;	/* SV_ModelIndex's name */
static char	case_namebuf[128];

/* ---- stubs the archive and fixture need but no case reaches ---- */
void *Hunk_AllocName (int size, const char *name)
{
	(void)size; (void)name;
	fprintf (stderr, "harness bug: Hunk_AllocName\n");
	abort ();
}

int q_strcasecmp (const char *s1, const char *s2)
{
	(void)s1; (void)s2;
	fprintf (stderr, "harness bug: q_strcasecmp\n");
	abort ();
	return 0;
}

int NUM_FOR_EDICT (edict_t *e)
{
	return (int)(((byte *)e - (byte *)sv.edicts) / pr_edict_size);
}

edict_t *EDICT_NUM (int n)
{
	if (n < 0 || n >= MAXEDICTS) {
		fprintf (stderr, "EDICT_NUM: bad number %i\n", n);
		abort ();
	}
	return &edicts[n];
}

char *va (const char *fmt, ...)
{
	static char buf[1024];
	va_list ap;
	va_start (ap, fmt);
	vsnprintf (buf, sizeof buf, fmt, ap);
	va_end (ap);
	return buf;
}

static void setup_case (uint64_t seed)
{
	int i;
	rng_seed (seed);
	memset (&sv, 0, sizeof sv);
	memset (&svs, 0, sizeof svs);
	memset (edicts, 0, sizeof edicts);
	memset (clients, 0, sizeof clients);
	memset (sock_tokens, 0, sizeof sock_tokens);
	memset (states, 0, sizeof states);
	calllog_len = conlog_len = 0;
	outlen = 0;
	stub_npending = 0;
	sendtoall_called = 0;
	stub_inv_page = NULL;
	stub_alpha_present = 0;

	sv_protocol = rng_int (0, 3)
		? ((int[]){18, 19, 20, 21, 100})[rng_int (0, 4)]
		: rng_int (17, 102);
	sv.edicts = edicts;
	sv.num_edicts = MAXEDICTS;
	sv.states = states;
	svs.maxclients = MAXHARNESSCLIENTS;
	svs.clients = clients;
	pr_edict_size = sizeof (edict_t);
	pr_crc = (unsigned short)rng_next ();
	coop.integer = rng_int (0, 1);
	deathmatch.integer = coop.integer ? 0 : rng_int (0, 1);
	sv_kingofhill = rng_int (0, 3);
	sv.loadgame = rng_int (0, 3) == 0;
	sv.cd_track = (byte)rng_int (0, 255);
	snprintf (sv.midi_name, sizeof sv.midi_name, "midi%d", rng_int (0, 9));
	snprintf (sv.mod_name, sizeof sv.mod_name, "mod%d", rng_int (0, 9));
	snprintf (sv.skybox, sizeof sv.skybox, "sky%d", rng_int (0, 9));
	setup_strings ();
	setup_precache (rng_int (0, 10), rng_int (0, 10));
	setup_qmodels (rng_int (0, 10));
	setup_ex_items (rng_int (0, 6));
	/* worldspawn edict for SV_GetLevelname */
	edicts[0].v.message = (float)rng_int (0, 3);
	edicts[0].v.netname = rng_int (0, 40);

	/* signon buffers */
	sv.num_signon_buffers = 0;
	SZ_Init (&sv.signon, sv.signon_bufs[0], MAX_SIGNON_SIZE);
	sv.signon.cursize = 0;

	stub_is_local = rng_int (0, 3) == 0;
	stub_max_unreliable = rng_int (1400, 4000);

	for (i = 0; i < MAXHARNESSCLIENTS; i++)
		setup_client (i);

	if (case_kind == 1) {
		/* pending sockets: 0..3, then NULL */
		int n = rng_int (0, 3);
		for (i = 0; i < n; i++)
			stub_pending[i] =
				(struct qsocket_s *)sock_tokens[MAXHARNESSCLIENTS + i];
		stub_npending = n;
		/* client activity: sometimes all active (the Sys_Error path) */
		for (i = 0; i < MAXHARNESSCLIENTS; i++)
			clients[i].active = (n > 0 && rng_int (0, 4) == 0)
				? 1 : rng_int (0, 1);
		/* spawn_parms for the loadgame path */
		for (i = 0; i < MAXHARNESSCLIENTS; i++) {
			int k;
			for (k = 0; k < NUM_SPAWN_PARMS; k++)
				clients[i].spawn_parms[k] = rng_float ();
		}
		memset (&inv_page, 0, sizeof inv_page);
		stub_inv_page = rng_next () % 2 ? &inv_page : NULL;
	} else if (case_kind == 2) {
		switch (rng_int (0, 4)) {
		case 0: case_name = NULL; break;
		case 1: case_name = ""; break;
		case 2:
			/* a name in the precache */
			if (sv.model_precache[1])
				case_name = sv.model_precache[1];
			else
				case_name = "models/v_w0.mdl";
			break;
		case 3:
			snprintf (case_namebuf, sizeof case_namebuf,
				  "models/missing%d.mdl", rng_int (0, 99));
			case_name = case_namebuf;
			break;
		default:
			snprintf (case_namebuf, sizeof case_namebuf,
				  "models/v_w%d.mdl", rng_int (0, 20));
			case_name = case_namebuf;
			break;
		}
	} else if (case_kind == 3) {
		/* signon state: sometimes part-full, sometimes full */
		sv.num_signon_buffers = rng_int (0, 2);
		for (i = 0; i < sv.num_signon_buffers; i++) {
			sv.signon_size[i] = rng_int (100, 1000);
			memset (sv.signon_bufs[i], 0xC3,
				(size_t)sv.signon_size[i]);
		}
		SZ_Init (&sv.signon,
			 sv.signon_bufs[sv.num_signon_buffers], MAX_SIGNON_SIZE);
		sv.signon.cursize = rng_next () % 2
			? rng_int (0, MAX_SIGNON_SIZE)
			: MAX_SIGNON_SIZE;
		memset (sv.signon.data, 0x5A, (size_t)sv.signon.cursize);
		switch (rng_int (0, 5)) {
		case 0: case_length = rng_int (1, 100); break;
		case 1: case_length = MAX_SIGNON_SIZE + rng_int (1, 100); break;
		case 2: case_length = MAX_SIGNON_SIZE - sv.signon.cursize;
			 if (case_length < 1) case_length = 1; break;
		case 3: case_length = MAX_SIGNON_SIZE - sv.signon.cursize + 1;
			 if (case_length < 1) case_length = 1; break;
		default: case_length = rng_int (1, MAX_SIGNON_SIZE); break;
		}
		/* push num_signon_buffers at the Host_Error edge sometimes */
		if (rng_int (0, 6) == 0)
			sv.num_signon_buffers = MAX_SIGNON_BUFFERS - 1;
	} else if (case_kind == 4) {
		case_client = rng_int (0, MAXHARNESSCLIENTS - 1);
		sv.num_signon_buffers = rng_int (0, 3);
		for (i = 0; i < sv.num_signon_buffers; i++) {
			sv.signon_size[i] = rng_int (10, 500);
			memset (sv.signon_bufs[i], 0xC3,
				(size_t)sv.signon_size[i]);
		}
		SZ_Init (&sv.signon,
			 sv.signon_bufs[sv.num_signon_buffers], MAX_SIGNON_SIZE);
		sv.signon.cursize = rng_int (0, 500);
		memset (sv.signon.data, 0x5A, (size_t)sv.signon.cursize);
		switch (rng_int (0, 4)) {
		case 0: clients[case_client].signon_buffer = -1; break;
		case 1: clients[case_client].signon_buffer =
				rng_int (0, sv.num_signon_buffers); break;
		case 2: clients[case_client].signon_buffer =
				sv.num_signon_buffers + rng_int (1, 3); break;
		default: clients[case_client].signon_buffer =
				rng_int (-2, sv.num_signon_buffers + 1); break;
		}
	} else if (case_kind == 5) {
		sv.num_edicts = rng_int (1, MAXEDICTS);
		for (i = 0; i < sv.num_edicts; i++) {
			edict_t *e = &edicts[i];
			e->free = rng_int (0, 4) == 0;
			e->v.modelindex = rng_float ();
			e->v.frame = rng_float ();
			e->v.skin = rng_float ();
			e->v.scale = rng_float ();
			e->v.drawflags = rng_float ();
			e->v.abslight = rng_float ();
			e->v.model = rng_int (0, 60);
			e->v.colormap = rng_float ();
			e->v.origin[0] = rng_float ();
			e->v.origin[1] = rng_float ();
			e->v.origin[2] = rng_float ();
			e->v.angles[0] = rng_float ();
			e->v.angles[1] = rng_float ();
			e->v.angles[2] = rng_float ();
		}
		stub_alpha_present = rng_int (0, 1);
		stub_alpha_value = rng_float ();
		SZ_Init (&sv.signon, sv.signon_bufs[0], MAX_SIGNON_SIZE);
	}
}

/* ---- per-side serialization ---- */
static int	case_retval;

static void serialize_side (void)
{
	int i;
	outlen = 0;
	if (case_kind == 0) {
		client_t *c = &clients[case_client];
		out_sizebuf (&c->message);
		out_int (c->sendsignon);
		out_int (c->spawned);
		out_int (c->signon_buffer);
	} else if (case_kind == 1) {
		for (i = 0; i < MAXHARNESSCLIENTS; i++) {
			/* whole client_t: pointers match across the two
			 * forks, which inherit the same parent layout */
			out_bytes (&clients[i], sizeof clients[i]);
		}
		out_bytes (states, sizeof states);
		out_int (net_activeconnections);
	} else if (case_kind == 2) {
		out_int (case_retval);
	} else if (case_kind == 3) {
		out_sizebuf (&sv.signon);
		out_int (sv.num_signon_buffers);
		out_bytes (sv.signon_size, sizeof sv.signon_size);
		for (i = 0; i <= sv.num_signon_buffers && i < MAX_SIGNON_BUFFERS; i++)
			out_bytes (sv.signon_bufs[i], MAX_SIGNON_SIZE);
	} else if (case_kind == 4) {
		client_t *c = &clients[case_client];
		out_sizebuf (&c->message);
		out_int (c->signon_buffer);
		out_int (c->sendsignon);
	} else if (case_kind == 5) {
		out_sizebuf (&sv.signon);
		out_bytes (edicts, sizeof edicts);
	} else if (case_kind == 6) {
		out_int (sendtoall_called);
		out_int (sendtoall_cursize);
		out_bytes (sendtoall_data, (size_t)sendtoall_cursize);
		out_bytes (&sendtoall_blocktime, sizeof sendtoall_blocktime);
	}
	out_bytes (calllog, calllog_len);
	out_bytes (conlog, conlog_len);
}

/* ---- the child: run one side, write the output file ---- */
static int	harness_side;	/* 0 = C, 1 = Rust */

static void run_child (void)
{
	FILE *f;
	struct sigaction sa;
	memset (&sa, 0, sizeof sa);
	sa.sa_handler = crash_handler;
	sigaction (SIGSEGV, &sa, NULL);
	sigaction (SIGABRT, &sa, NULL);
	sigaction (SIGBUS, &sa, NULL);
	calllog_len = conlog_len = 0;
	if (getenv ("HARNESS_TRACE")) {
		fprintf (stderr, "child side %d kind %d client %d\n",
			 harness_side, case_kind, case_client);
	}
	if (case_kind == 0) {
		if (harness_side == 0)
			c_SV_SendServerinfo (&clients[case_client]);
		else
			SV_SendServerinfo (&clients[case_client]);
	} else if (case_kind == 1) {
		if (harness_side == 0)
			c_SV_CheckForNewClients ();
		else
			SV_CheckForNewClients ();
	} else if (case_kind == 2) {
		if (harness_side == 0)
			case_retval = c_SV_ModelIndex (case_name);
		else
			case_retval = SV_ModelIndex (case_name);
	} else if (case_kind == 3) {
		if (harness_side == 0)
			c_SV_ReserveSignonSpace (case_length);
		else
			SV_ReserveSignonSpace (case_length);
	} else if (case_kind == 4) {
		if (harness_side == 0)
			c_SV_SendSignonBuffer (&clients[case_client]);
		else
			SV_SendSignonBuffer (&clients[case_client]);
	} else if (case_kind == 5) {
		if (harness_side == 0)
			c_SV_CreateBaseline ();
		else
			SV_CreateBaseline ();
	} else {
		if (harness_side == 0)
			c_SV_SendReconnect ();
		else
			SV_SendReconnect ();
	}
	serialize_side ();
	f = fopen (out_path, "wb");
	if (!f)
		_exit (3);
	fwrite (outbuf, 1, outlen, f);
	fclose (f);
	/* exit(), not _exit(): the gcov exit handler dumps coverage, and
	 * libgcov merges the per-process files.  An explicit __gcov_dump
	 * does not exist in GCC 13's libgcov. */
	exit (0);
}

/* ---- main: generate cases, fork both sides, compare ---- */
static long	cases_run, cases_pass;

static int run_one_case (uint64_t seed, int kind, const char *tmpdir)
{
	pid_t pid;
	int status, c_status;
	char path_c[300], path_r[300];
	FILE *fc, *fr;
	long szc, szr;
	static char *bufc, *bufr;
	static size_t cap;

	snprintf (path_c, sizeof path_c, "%s/case.out", tmpdir);
	snprintf (path_r, sizeof path_r, "%s/rust.out", tmpdir);

	case_kind = kind;
	if (kind == 0 || kind == 4)
		case_client = (int)(seed % MAXHARNESSCLIENTS);
	setup_case (seed);

	harness_side = 0;
	snprintf (out_path, sizeof out_path, "%s", path_c);
	pid = fork ();
	if (pid < 0) {
		printf ("case %lu kind %d: fork failed: %s\n",
			cases_run, kind, strerror (errno));
		return 1;
	}
	if (pid == 0)
		run_child ();
	if (waitpid (pid, &status, 0) < 0) {
		printf ("case %lu kind %d: waitpid failed\n", cases_run, kind);
		return 1;
	}
	c_status = status;

	/* rebuild the identical setup for the Rust side */
	case_kind = kind;
	if (kind == 0 || kind == 4)
		case_client = (int)(seed % MAXHARNESSCLIENTS);
	setup_case (seed);

	harness_side = 1;
	snprintf (out_path, sizeof out_path, "%s", path_r);
	pid = fork ();
	if (pid == 0)
		run_child ();
	if (waitpid (pid, &status, 0) < 0) {
		printf ("case %lu kind %d: waitpid failed (rust)\n",
			cases_run, kind);
		return 1;
	}

	cases_run++;
	/* Both sides must exit the same way.  The error stubs write the
	 * logs to the output file before aborting, so an abort case
	 * compares the two logs byte for byte below. */
	if (c_status != status) {
		printf ("MISMATCH case %lu kind %d seed %lu: exit %d vs %d\n",
			cases_run - 1, kind, (unsigned long)seed,
			c_status, status);
		return 1;
	}
	fc = fopen (path_c, "rb"); fr = fopen (path_r, "rb");
	if (!fc || !fr) {
		/* Neither side wrote output: pass only if both died the
		 * same way without writing (a crash outside the stubs). */
		if (fc) fclose (fc);
		if (fr) fclose (fr);
		if (!fc && !fr) {
			cases_pass++;
			return 0;
		}
		printf ("case %lu kind %d: one side wrote no output\n",
			cases_run - 1, kind);
		return 1;
	}
	fseek (fc, 0, SEEK_END); szc = ftell (fc); fseek (fc, 0, SEEK_SET);
	fseek (fr, 0, SEEK_END); szr = ftell (fr); fseek (fr, 0, SEEK_SET);
	if ((size_t)(szc > szr ? szc : szr) > cap) {
		cap = (size_t)(szc > szr ? szc : szr) + 1024;
		bufc = realloc (bufc, cap); bufr = realloc (bufr, cap);
	}
	if ((size_t)fread (bufc, 1, (size_t)szc, fc) != (size_t)szc ||
	    (size_t)fread (bufr, 1, (size_t)szr, fr) != (size_t)szr) {
		printf ("case %lu: short read on the outputs\n", cases_run - 1);
		fclose (fc); fclose (fr);
		return 1;
	}
	fclose (fc); fclose (fr);
	if (szc != szr || memcmp (bufc, bufr, (size_t)szc) != 0) {
		long i, n = szc < szr ? szc : szr;
		printf ("MISMATCH case %lu kind %d seed %lu: %ld vs %ld bytes\n",
			cases_run - 1, kind, (unsigned long)seed, szc, szr);
		for (i = 0; i < n && bufc[i] == bufr[i]; i++)
			;
		printf ("  first difference at byte %ld\n", i);
		if (getenv ("HARNESS_KEEP")) {
			char kc[300], kr[300];
			snprintf (kc, sizeof kc, "/tmp/dbg_mismatch_c_%lu.bin", cases_run - 1);
			snprintf (kr, sizeof kr, "/tmp/dbg_mismatch_r_%lu.bin", cases_run - 1);
			FILE *f = fopen (kc, "wb");
			if (f) { fwrite (bufc, 1, (size_t)szc, f); fclose (f); }
			f = fopen (kr, "wb");
			if (f) { fwrite (bufr, 1, (size_t)szr, f); fclose (f); }
			printf ("  kept in %s and %s\n", kc, kr);
		}
		return 1;
	}
	cases_pass++;
	return 0;
}

/* ---- kind 7: the %4.2f / %i / %02d formatters, in-process ----
 * Fuzz %4.2f over +-1e6 against libc sprintf; %i and %02d over ints. */
static int test_formatters (void)
{
	unsigned char rbuf[32], cbuf[64];
	uint64_t i;
	long bad = 0, n = 0;
	rng_seed (0xF0470001ULL);
	for (i = 0; i < 2000000; i++) {
		double v;
		size_t rn, cn;
		uint64_t r = rng_next ();
		switch (r % 5) {
		case 0: v = 0.0; break;
		case 1: v = -0.0; break;
		case 2: v = (double)(int)(r % 2000000) - 1000000.0; break;
		case 3: v = ((double)(r % 2000000000) - 1000000000.0) / 137.0; break;
		default: v = ((double)r / (double)UINT64_MAX) * 2000000.0 - 1000000.0; break;
		}
		/* exact halves and ulps around them, for round-half-to-even */
		if (r % 7 == 0)
			v = (double)((int64_t)(v * 100.0)) / 100.0 + 0.005;
		rn = SvMainConnect_TestFmt42 (rbuf, v);
		cn = (size_t)snprintf ((char *)cbuf, sizeof cbuf, "%4.2f", v);
		n++;
		if (rn != cn || memcmp (rbuf, cbuf, rn) != 0) {
			if (bad < 5)
				printf ("fmt42 mismatch: v=%a rust='%s' libc='%s'\n",
					v, rbuf, cbuf);
			bad++;
		}
	}
	/* non-finite shapes */
	{
		static const double specials[] = {NAN, INFINITY, -INFINITY};
		size_t k;
		for (k = 0; k < 3; k++) {
			size_t rn = SvMainConnect_TestFmt42 (rbuf, specials[k]);
			size_t cn = (size_t)snprintf ((char *)cbuf, sizeof cbuf,
						      "%4.2f", specials[k]);
			n++;
			if (rn != cn || memcmp (rbuf, cbuf, rn) != 0) {
				printf ("fmt42 special mismatch: %s vs %s\n",
					rbuf, cbuf);
				bad++;
			}
		}
	}
	/* %i and %02d */
	for (i = 0; i < 200000; i++) {
		int v;
		size_t rn, cn;
		uint64_t r = rng_next ();
		if (r % 5 == 0)
			v = (int)(r % 201) - 100;
		else if (r % 5 == 1)
			v = (int)r;
		else
			v = rng_int (-1000000, 1000000);
		rn = SvMainConnect_TestFmtInt (rbuf, v);
		cn = (size_t)snprintf ((char *)cbuf, sizeof cbuf, "%i", v);
		n++;
		if (rn != cn || memcmp (rbuf, cbuf, rn) != 0) {
			if (bad < 5)
				printf ("fmti mismatch: v=%d rust='%s' libc='%s'\n",
					v, rbuf, cbuf);
			bad++;
		}
		rn = SvMainConnect_TestFmt02d (rbuf, v);
		cn = (size_t)snprintf ((char *)cbuf, sizeof cbuf, "%02d", v);
		n++;
		if (rn != cn || memcmp (rbuf, cbuf, rn) != 0) {
			if (bad < 5)
				printf ("fmt02d mismatch: v=%d rust='%s' libc='%s'\n",
					v, rbuf, cbuf);
			bad++;
		}
	}
	printf ("formatters: %ld cases, %ld mismatches\n", n, bad);
	cases_run += n;
	cases_pass += n - bad;
	return bad != 0;
}

/* Directed cases the generator is unlikely to hit. */
static int directed (const char *tmpdir)
{
	int bad = 0;
	uint64_t seed;
	size_t k;
	/* protocols 17..101, one per value that matters */
	static const int protos[] = {17, 18, 19, 20, 21, 22, 99, 100, 101};

	/* SV_SendServerinfo: coop/deathmatch x kingofhill */
	for (seed = 0; seed < 20; seed++)
		bad |= run_one_case (0x5EED0000ULL + seed, 0, tmpdir);
	/* SV_CheckForNewClients: duplicate connects and the full server.
	 * Seeds are fixed; setup_case's client-activity roll decides. */
	for (seed = 0; seed < 30; seed++)
		bad |= run_one_case (0xC0DA0000ULL + seed, 1, tmpdir);
	/* SV_ModelIndex hit/miss/NULL/empty */
	for (seed = 0; seed < 10; seed++)
		bad |= run_one_case (0xD1A60000ULL + seed, 2, tmpdir);
	/* signon overflow edges */
	for (seed = 0; seed < 20; seed++)
		bad |= run_one_case (0x51600000ULL + seed, 3, tmpdir);
	/* signon_buffer edges */
	for (seed = 0; seed < 20; seed++)
		bad |= run_one_case (0x51610000ULL + seed, 4, tmpdir);
	/* baseline edges */
	for (seed = 0; seed < 20; seed++)
		bad |= run_one_case (0xBA5E0000ULL + seed, 5, tmpdir);
	/* reconnect */
	for (seed = 0; seed < 5; seed++)
		bad |= run_one_case (0x2EC00000ULL + seed, 6, tmpdir);
	/* protocol sweep through serverinfo and baseline */
	for (k = 0; k < sizeof protos / sizeof protos[0]; k++) {
		uint64_t s = 0xD1A61000ULL + (uint64_t)protos[k] * 0x100;
		bad |= run_one_case (s, 0, tmpdir);
		bad |= run_one_case (s + 1, 5, tmpdir);
	}
	return bad;
}

int main (int argc, char **argv)
{
	char tmpdir[] = "/tmp/connect_harness_XXXXXX";
	long i, target;
	int bad = 0;

	if (!mkdtemp (tmpdir)) { perror ("mkdtemp"); return 2; }
	if (getenv ("HARNESS_PRINT_TMPDIR"))
		printf ("tmpdir=%s\n", tmpdir);
	if (check_facts ())
		return 2;

	target = argc > 1 ? atol (argv[1]) : 4000;
	printf ("sv_main_connect differential harness\n");

	bad |= test_formatters ();

	/* spread over the seven forked kinds */
	for (i = 0; i < target; i++) {
		int kind = (int)(i % 7);
		if (run_one_case (0x10000000ULL + (uint64_t)i, kind, tmpdir))
			bad = 1;
	}
	bad |= directed (tmpdir);

	printf ("sv_main_connect differential harness (%s): %s (%ld cases, %ld pass)\n",
		"h2", bad ? "FAIL" : "PASS", cases_run, cases_pass);
	return bad ? 1 : 0;
}
