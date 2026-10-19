/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * datagram_harness.c -- engine/rust/src/sv_main_datagram.rs against the C
 * original of Hexen II's sv_main.c client data and datagrams, in one binary.
 *
 * run_datagram_harness.sh builds this against sv_main_datagram_h2.c
 * (-DSERVERONLY, as h2ded compiles it), with the real
 * engine/rust/world_target.c and engine/rust/sv_main_target.c compiled in.
 * The six ported functions go in renamed to c_*; the supporting functions
 * the slice calls but does not port (SV_ClientInventoryPage,
 * INV_UpdateExItem, SV_ModelIndex, the signon-buffer trio) keep their names:
 * both sides call the same C.
 *
 * Each case builds the state one of the three exported functions reads,
 * then forks twice: once to run the C, once to run the Rust, from the same
 * snapshot.  Forking (rather than snapshot/restore) keeps the two
 * function-local statics of SV_WriteClientdataToMessage (next_update,
 * next_count) in lockstep for free: each child inherits the parent's
 * values, and the parent never calls the functions itself.
 *
 * The child serializes everything the call can have written -- the sizebufs
 * (cursize, overflowed, every byte), the structs (old_v, the inventory
 * pages, the client fields), and the two logs (network/console) -- to a
 * file; the parent compares the two files byte for byte.
 *
 * The network (NET_*) and SV_DropClient are capture-and-record stubs both
 * sides call: SV_DropClient runs QuakeC in the real engine, so no harness
 * can run the real one.  SV_SetIdealPitch is sv_user.c's, out of this
 * slice; the stub records the call.  MSG_Write* and SZ_* are the
 * already-ported Rust, held to their C by their own harnesses.
 * SV_PrepareClientEntities and SV_CleanupEnts are slice B's Rust, called
 * by both sides -- the real engine does the same.
 *
 * Before the cases, the facts table sv_main_datagram.rs reads is checked
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
#include <string.h>

#ifdef __GNUC__
/* Weak: __gcov_dump exists only when this TU is built with --coverage (the
   cov build in run_datagram_harness.sh; GCC 13's libgcov no longer exports
   the old __gcov_flush name).  A single weak declaration -- a second,
   non-weak one makes GCC drop the null check and emit a direct call. */
__attribute__((weak)) void __gcov_dump (void);
#endif

static void crash_handler (int sig)
{
	void *bt[32];
	int n = backtrace (bt, 32);
	fprintf (stderr, "child crashed with signal %d:\n", sig);
	backtrace_symbols_fd (bt, n, 2);
	_exit (100 + sig);
}
#include <unistd.h>

void	c_SV_ClearDatagram (void);
void	c_SV_WriteClientdataToMessage (client_t *client, edict_t *ent, sizebuf_t *msg);
void	c_SV_SendClientMessages (void);
void	SV_ClearDatagram (void);
void	SV_WriteClientdataToMessage (client_t *client, edict_t *ent, sizebuf_t *msg);
void	SV_SendClientMessages (void);

/* the engine's storage */
server_t	sv;
server_static_t	svs;
int		sv_protocol;
cvar_t		sv_maxdatagram = {"sv_maxdatagram", "0", CVAR_NONE};
int		sv_max_edicts;
int		pr_edict_size = sizeof (edict_t);
sv_globals_t	sv_globals;
areanode_t	sv_areanodes[AREA_NODES];
sizebuf_t	net_message;
client_t	*host_client;
unsigned int	info_mask, info_mask2;
qboolean	sv_datagram_dropped;
double		realtime;
char		*pr_strings;
int		pr_stringssize;

/* ---- stub control: set by the parent per case, read by the stubs ---- */
static int	stub_max_unreliable = 1400;
static int	stub_is_local = 0;
static int	stub_can_send = 1;
static int	stub_send_unreliable_ret = 0;
static int	stub_send_ret = 0;

/* ---- the call log: every stubbed call, in order ---- */
#define LOGSIZE	(1 << 20)
static char	calllog[LOGSIZE];
static size_t	calllog_len;
static char	conlog[LOGSIZE];
static size_t	conlog_len;

static void log_call (const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start (ap, fmt);
	n = vsnprintf (calllog + calllog_len, LOGSIZE - calllog_len, fmt, ap);
	va_end (ap);
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
	/* normalize the harness's c_ rename: __thisfunc__ in the fixture
	 * expands to c_SV_*, but the Rust logs SV_*. */
	for (p = tmp; (p = strstr (p, "c_SV_")) != NULL; ) {
		memmove (p, p + 2, strlen (p + 2) + 1);
	}
	n = snprintf (conlog + conlog_len, LOGSIZE - conlog_len, "%s", tmp);
	if (n > 0)
		conlog_len += (size_t)n < LOGSIZE - conlog_len ? (size_t)n : 0;
}

/* qsocket_s tokens: the stubs only need distinct pointers, never the
 * struct's contents (net_defs.h is private to the network layer). */
#define MAXHARNESSCLIENTS	4
/* edicts per harness case; must cover every dmg_inflictor offset the
 * generator emits (0..MAXEDICTS*sizeof(edict_t)) */
#define MAXEDICTS	16
static unsigned char	sock_tokens[MAXHARNESSCLIENTS][16];

static int sock_index (struct qsocket_s *s)
{
	int i;
	for (i = 0; i < MAXHARNESSCLIENTS; i++)
		if (s == (struct qsocket_s *)sock_tokens[i])
			return i;
	return -1;
}

int NET_MaxUnreliableMessage (struct qsocket_s *sock)
{
	log_call ("max_unreliable(%d) -> %d\n", sock_index (sock),
		  stub_max_unreliable);
	return stub_max_unreliable;
}

qboolean NET_IsLocalConnection (struct qsocket_s *sock)
{
	log_call ("is_local(%d) -> %d\n", sock_index (sock), stub_is_local);
	return stub_is_local;
}

qboolean NET_CanSendMessage (struct qsocket_s *sock)
{
	log_call ("can_send(%d) -> %d\n", sock_index (sock), stub_can_send);
	return stub_can_send;
}

int NET_SendUnreliableMessage (struct qsocket_s *sock, sizebuf_t *msg)
{
	log_call ("send_unreliable(%d, %d bytes) -> %d\n", sock_index (sock),
		  msg->cursize, stub_send_unreliable_ret);
	return stub_send_unreliable_ret;
}

int NET_SendMessage (struct qsocket_s *sock, sizebuf_t *data)
{
	log_call ("send(%d, %d bytes) -> %d\n", sock_index (sock),
		  data->cursize, stub_send_ret);
	return stub_send_ret;
}

/* ---- shared by both sides: SV_PrepareClientEntities (slice B, reused by
 * the fixture's SV_SendClientDatagram) needs these ---- */
FUNC_NORETURN void Host_Error (const char *error, ...)
{
	va_list	ap;
	char	buf[1024];
	va_start (ap, error);
	vsnprintf (buf, sizeof buf, error, ap);
	va_end (ap);
	log_call ("Host_Error", "%s", buf);
	abort ();
}

FUNC_NORETURN void Sys_Error (const char *error, ...)
{
	va_list	ap;
	char	buf[1024];
	va_start (ap, error);
	vsnprintf (buf, sizeof buf, error, ap);
	va_end (ap);
	fprintf (stderr, "Sys_Error: %s\n", buf);
	log_call ("Sys_Error", "%s", buf);
	abort ();
}

edict_t *EDICT_NUM (int n)
{
	fprintf (stderr, "EDICT_NUM(%d) max=%d\n", n, sv_max_edicts);
	if (n < 0 || n >= sv_max_edicts) {
		fprintf (stderr, "EDICT_NUM: bad number %i (max %i)\n", n, sv_max_edicts);
		Host_Error ("EDICT_NUM: bad number %i", n);
	}
	return (edict_t *)((byte *)sv.edicts + (n) * pr_edict_size);
}

/* no alpha field in the harness's edicts */
eval_t *GetEdictFieldValue (edict_t *ed, const char *field)
{
	(void)ed; (void)field;
	return NULL;
}

/* every leaf sees every other leaf: worst case for the PVS walk */
static byte	allpvs[(MAXEDICTS + 7) / 8];
byte *Mod_LeafPVS (mleaf_t *leaf, qmodel_t *model)
{
	(void)leaf; (void)model;
	memset (allpvs, 0xFF, sizeof allpvs);
	return allpvs;
}

cvar_t	sv_update_missiles = {"sv_update_missiles", "1", CVAR_NONE};
cvar_t	sv_update_misc = {"sv_update_misc", "1", CVAR_NONE};
cvar_t	sv_update_player = {"sv_update_player", "1", CVAR_NONE};
cvar_t	sv_update_monsters = {"sv_update_monsters", "1", CVAR_NONE};
cvar_t	sv_netsort = {"sv_netsort", "1", CVAR_NONE};

/* sv_main_sound.rs, in the same archive, calls these; nothing here reaches them */
int q_strcasecmp (const char *s1, const char *s2)	{ (void)s1; (void)s2; abort (); }
int NUM_FOR_EDICT (edict_t *e)
{
	return ((byte *)e - (byte *)sv.edicts) / pr_edict_size;
}

/* SZ_Init's overflow path; unreachable here (buffers are sized) */
void *Hunk_AllocName (int size, const char *name)
{
	(void)size; (void)name;
	fprintf (stderr, "harness bug: Hunk_AllocName\n");
	abort ();
}

void SV_DropClient (qboolean crash)
{
	int idx = (int)(host_client - svs.clients);
	log_call ("drop_client(%d, crash=%d)\n", idx, crash);
	/* record-only: the real one runs QuakeC and tears the client down;
	 * both sides see the same stub. */
}

void SV_SetIdealPitch (void)
{
	log_call ("set_ideal_pitch()\n");
}

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	log_con ((int)flags, fmt, ap);
	va_end (ap);
}

void I_Printf (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	log_con (-1, fmt, ap);
	va_end (ap);
}

const char *PR_GetString (int num)
{
	if (num >= 0 && num < pr_stringssize)
		return pr_strings + num;
	return "";
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
/* odd floats throughout, like the ents harness: normals, NaN, infinities,
 * values past a byte/short/int */
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
	const sv_main_datagram_facts_t *f = &SvMainDatagram_TargetFacts;
	int bad = 0;
#define CKF(name, expect) \
	if (f->name != (expect)) { \
		printf ("facts mismatch: %s = %d, header says %d\n", \
			#name, (int)f->name, (int)(expect)); bad = 1; }
#define CKZ(name, expect) \
	if (f->name != (size_t)(expect)) { \
		printf ("facts mismatch: %s = %zu, header says %zu\n", \
			#name, f->name, (size_t)(expect)); bad = 1; }
	CKF (msg_damage, svc_damage); CKF (msg_setangle, svc_setangle);
	CKF (msg_clientdata, svc_clientdata); CKF (msg_update_inv, svc_update_inv);
	CKF (msg_time, svc_time); CKF (msg_nop, svc_nop);
	CKF (msg_updatefrags, svc_updatefrags); CKF (msg_signonnum, svc_signonnum);
	CKF (su_viewheight, SU_VIEWHEIGHT); CKF (su_idealpitch, SU_IDEALPITCH);
	CKF (su_idealroll, SU_IDEALROLL); CKF (su_punch1, SU_PUNCH1);
	CKF (su_velocity1, SU_VELOCITY1); CKF (su_onground, SU_ONGROUND);
	CKF (su_weaponframe, SU_WEAPONFRAME); CKF (su_armor, SU_ARMOR);
	CKF (su_weapon, SU_WEAPON);
	CKF (sc1_health, SC1_HEALTH); CKF (sc1_rings_active, SC1_RINGS_ACTIVE);
	CKF (sc1_cnt_torch, SC1_CNT_TORCH);
	CKF (sc1_cameramode, SC1_CAMERAMODE);
	CKF (sc2_rings_low, SC2_RINGS_LOW); CKF (sc2_obj2, SC2_OBJ2);
	CKF (sc2_puzzle1, SC2_PUZZLE1); CKF (sc2_maxhealth, SC2_MAXHEALTH);
	CKF (max_inventory_ex, MAX_INVENTORY_EX); CKF (fl_onground, FL_ONGROUND);
	CKZ (edict_v, offsetof (edict_t, v));
	CKZ (entvars_size, sizeof (entvars_t));
	CKZ (v_dmg_take, offsetof (entvars_t, dmg_take));
	CKZ (v_dmg_inflictor, offsetof (entvars_t, dmg_inflictor));
	CKZ (v_cameramode, offsetof (entvars_t, cameramode));
	CKZ (v_puzzle_inv8, offsetof (entvars_t, puzzle_inv8));
	CKZ (v_frags, offsetof (entvars_t, frags));
	CKZ (client_size, sizeof (client_t));
	CKZ (client_old_v, offsetof (client_t, old_v));
	CKZ (client_send_all_v, offsetof (client_t, send_all_v));
	CKZ (client_message, offsetof (client_t, message));
	CKZ (client_datagram, offsetof (client_t, datagram));
	CKZ (client_netconnection, offsetof (client_t, netconnection));
	CKZ (client_signon_buffer, offsetof (client_t, signon_buffer));
	CKZ (client_last_message, offsetof (client_t, last_message));
	CKZ (client_ex_inventory, offsetof (client_t, ex_inventory));
	CKZ (ex_changed_items, offsetof (ex_inventory_page_t, changed_items));
	CKZ (ex_item_cnt, offsetof (ex_inventory_page_t, item_cnt));
	CKZ (ex_next, offsetof (ex_inventory_page_t, next));
#undef CKF
#undef CKZ
	if (!bad)
		printf ("layouts: sv_main_datagram facts match the headers\n");
	return bad;
}

/* ---- output serialization: one malloc'd buffer per child ---- */
static char	*outbuf;
static size_t	outlen, outcap;
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

/* a one-leaf world: the PVS walk in SV_PrepareClientEntities just needs
 * world->nodes to point at a leaf (contents < 0, not solid) */
static qmodel_t	worldmodel;
static mleaf_t	one_leaf;

/* ---- per-case backing stores ---- */
static edict_t	edicts[MAXEDICTS];
static client_t	clients[MAXHARNESSCLIENTS];
static byte	msgbufs[MAXHARNESSCLIENTS][MAX_MSGLEN];
static byte	dgrambufs[MAXHARNESSCLIENTS][NET_MAXMESSAGE];
static byte	sv_dgram_buf[NET_MAXMESSAGE];
static byte	sv_rel_buf[NET_MAXMESSAGE];
static byte	case_msg_buf[NET_MAXMESSAGE];
static ex_inventory_page_t pages[MAXHARNESSCLIENTS][3];

static void setup_client (int i, int active)
{
	client_t *c = &clients[i];
	memset (c, 0, sizeof (*c));
	c->active = active;
	c->netconnection = (struct qsocket_s *)sock_tokens[i];
	c->edict = &edicts[i];
	SZ_Init (&c->message, msgbufs[i], sizeof msgbufs[i]);
	SZ_Init (&c->datagram, dgrambufs[i], sizeof dgrambufs[i]);
}

static int pr_w3_off = -1;	/* heap offset of "models/v_w3.mdl", for SV_ModelIndex's found path */

static void randomize_edict_v (edict_t *e)
{
	float *v = &e->v.origin[0] - offsetof (entvars_t, origin) / sizeof (float);
	/* walk the whole entvars_t as floats except the known int fields */
	size_t n = sizeof (entvars_t) / sizeof (float), k;
	for (k = 0; k < n; k++)
		v[k] = rng_float ();
	/* int fields, as ints; dmg_inflictor is a byte offset from sv.edicts,
	 * kept inside the harness's edict array */
	e->v.dmg_inflictor = rng_int (0, MAXEDICTS - 1) * (int)sizeof (edict_t);
	e->v.weaponmodel = rng_int (0, 64);
	/* sometimes a precached model name, so SV_ModelIndex's found path
	 * (return i) runs; the precache holds models/v_w0..7.mdl */
	if (pr_w3_off >= 0 && rng_next () % 4 == 0)
		e->v.weaponmodel = pr_w3_off;
	e->v.cameramode = rng_int (0, 1);
	e->v.puzzle_inv1 = rng_int (0, 200); e->v.puzzle_inv2 = rng_int (0, 200);
	e->v.puzzle_inv3 = rng_int (0, 200); e->v.puzzle_inv4 = rng_int (0, 200);
	e->v.puzzle_inv5 = rng_int (0, 200); e->v.puzzle_inv6 = rng_int (0, 200);
	e->v.puzzle_inv7 = rng_int (0, 200); e->v.puzzle_inv8 = rng_int (0, 200);
}

static void randomize_client_v (client_t *c)
{
	float *v = (float *)&c->old_v;
	size_t n = sizeof (c->old_v) / sizeof (float), k;
	for (k = 0; k < n; k++)
		v[k] = rng_float ();
	c->old_v.dmg_inflictor = rng_int (-4, MAXEDICTS * (int)sizeof (edict_t));
	c->old_v.weaponmodel = rng_int (0, 64);
	c->old_v.cameramode = rng_int (0, 1);
}

/* an inventory page chain: 1-3 pages, random changed/new flags and counts */
static void setup_pages (int ci, int npages)
{
	int p, k;
	for (p = 0; p < 3; p++) {
		ex_inventory_page_t *pg = &pages[ci][p];
		memset (pg, 0, sizeof (*pg));
		pg->id = p + 1;
		pg->client_id = ci;
		pg->next = (p + 1 < npages) ? &pages[ci][p + 1] : NULL;
		if (rng_next () % 2) {
			pg->changed_items = (int)rng_next ();
			pg->new_items = (int)rng_next () & pg->changed_items;
			for (k = 0; k < 32; k++) {
				pg->item_id[k] = rng_int (0, 20);
				pg->item_cnt[k] = rng_int (0, 300);
			}
		}
	}
	clients[ci].ex_inventory = npages ? &pages[ci][0] : NULL;
}

/* the progs string heap: puzzle names and weapon models */
static void setup_strings (void)
{
	static char heap[4096];
	static const char *words[] = {
		"puzzle1", "puzzle2", "key", "models/v_crowbar.mdl",
		"models/v_shot.mdl", "", "a_very_long_puzzle_name_for_overflow",
		"models/v_w3.mdl",
	};
	size_t off = 0, w;
	pr_strings = heap;
	for (w = 0; w < sizeof words / sizeof words[0]; w++) {
		size_t l = strlen (words[w]) + 1;
		if (off + l >= sizeof heap)
			break;
		if (!strcmp (words[w], "models/v_w3.mdl"))
			pr_w3_off = (int)off;
		memcpy (heap + off, words[w], l);
		off += l;
	}
	/* NUL-fill past the words, so every offset PR_GetString can return
	 * is a valid C string (the C original would never see these
	 * offsets; the harness just needs defined behavior) */
	memset (heap + off, 0, sizeof heap - off);
	pr_stringssize = (int)sizeof heap;
}

/* ---- case setup ---- */
static sizebuf_t	case_msg;
static int		case_kind;	/* 0 = clear, 1 = clientdata, 2 = messages */
static int		case_client;	/* clientdata's client index */

static void setup_case (uint64_t seed)
{
	int i;
	rng_seed (seed);
	memset (&sv, 0, sizeof sv);
	memset (&svs, 0, sizeof svs);
	memset (edicts, 0, sizeof edicts);
	memset (clients, 0, sizeof clients);
	memset (sock_tokens, 0, sizeof sock_tokens);
	calllog_len = conlog_len = 0;

	sv_protocol = rng_int (0, 3)
		? ((int[]){18, 21, 100, 101})[rng_int (0, 3)]
		: rng_int (-5, 120);
	sv.edicts = (edict_t *)edicts;
	sv.num_edicts = MAXEDICTS;
	sv_max_edicts = MAXEDICTS;
	/* SV_PrepareClientEntities (slice B) needs the per-client states */
	if (!sv.states)
		sv.states = calloc (MAXHARNESSCLIENTS, sizeof (client_state2_t));
	/* and a world whose nodes are a single empty leaf */
	memset (&worldmodel, 0, sizeof worldmodel);
	memset (&one_leaf, 0, sizeof one_leaf);
	one_leaf.contents = -1;	/* CONTENTS_EMPTY */
	worldmodel.numleafs = 1;
	worldmodel.nodes = (mnode_t *)&one_leaf;
	sv.worldmodel = &worldmodel;
	svs.maxclients = rng_int (1, MAXHARNESSCLIENTS);
	svs.clients = clients;
	sv.time = (double)rng_float ();
	realtime = (double)rng_float ();
	info_mask = (unsigned)rng_next ();
	info_mask2 = (unsigned)rng_next ();
	sv_datagram_dropped = rng_int (0, 1);
	sv_maxdatagram.integer = rng_next () % 3 ? 0 : rng_int (-100, 9000);
	setup_strings ();

	SZ_Init (&sv.datagram, sv_dgram_buf, sizeof sv_dgram_buf);
	SZ_Init (&sv.reliable_datagram, sv_rel_buf, sizeof sv_rel_buf);
	if (rng_next () % 2) {
		int n = rng_int (0, 2000);
		SZ_Write (&sv.datagram, sv_dgram_buf, 0); /* noop, keeps the pattern */
		sv.datagram.cursize = n % (int)sizeof sv_dgram_buf;
		memset (sv.datagram.data, 0x5A, (size_t)sv.datagram.cursize);
	}
	if (rng_next () % 2) {
		int n = rng_int (0, 2000);
		sv.reliable_datagram.cursize = n % (int)sizeof sv_rel_buf;
		memset (sv.reliable_datagram.data, 0xA5,
			(size_t)sv.reliable_datagram.cursize);
	}

	/* signon buffers, for the not-yet-spawned */
	sv.num_signon_buffers = rng_int (0, 3);
	for (i = 0; i < sv.num_signon_buffers; i++) {
		sv.signon_size[i] = rng_int (10, 200);
		memset (sv.signon_bufs[i], 0xC3, (size_t)sv.signon_size[i]);
	}
	SZ_Init (&sv.signon, sv.signon_bufs[sv.num_signon_buffers],
		 MAX_SIGNON_SIZE);
	sv.signon.cursize = rng_int (0, 300);

	/* model precache, so SV_ModelIndex's found path runs sometimes */
	if (rng_next () % 2) {
		static char *models[8];
		static char mbuf[8][64];
		int k;
		for (k = 0; k < 8; k++) {
			snprintf (mbuf[k], sizeof mbuf[k], "models/v_w%d.mdl", k);
			models[k] = mbuf[k];
			sv.model_precache[k] = mbuf[k];
		}
		(void)models;
	}

	/* ex_inventory_pages backing, sometimes NULL (fresh map, no pages) */
	if (rng_next () % 2) {
		static ex_inventory_page_t backing[32];
		memset (backing, 0, sizeof backing);
		sv.ex_inventory_pages = backing;
	} else {
		sv.ex_inventory_pages = NULL;
	}

	for (i = 0; i < svs.maxclients; i++) {
		client_t *c = &clients[i];
		setup_client (i, rng_int (0, 3) != 0);
		c->spawned = rng_int (0, 1);
		c->sendsignon = rng_int (0, 1);
		c->signon_buffer = rng_int (-1, 2);
		c->dropasap = rng_int (0, 4) == 0;
		c->last_message = (double)rng_float ();
		c->old_frags = rng_int (-2, 4);
		c->info_mask = rng_next () % 2 ? info_mask : (unsigned)rng_next ();
		c->info_mask2 = rng_next () % 2 ? info_mask2 : (unsigned)rng_next ();
		c->send_all_v = rng_int (0, 1);
		snprintf (c->name, sizeof c->name, "client%d", i);
		randomize_edict_v (c->edict);
		c->edict->v.frags = (float)rng_int (-2, 4);
		randomize_client_v (c);
		/* the C dereferences ex_inventory unconditionally for
		 * protocol >= 21, so those cases always get a page */
		{
			int np = rng_int (0, 3);
			if (sv_protocol >= PROTOCOL_UH2_114 && np == 0)
				np = 1;
			setup_pages (i, np);
		}
		/* sometimes old_v == v exactly and no page flags, so the
		 * inventory block's `goto end' path runs.  Scrub NaNs: the
		 * generator emits them, and NaN != NaN would set sc bits
		 * despite the memcpy, keeping the path unreachable. */
		if (rng_next () % 8 == 0) {
			int p;
			size_t k, n = sizeof c->old_v / sizeof (float);
			float *a = (float *)&c->old_v, *b = (float *)&c->edict->v;
			memcpy (&c->old_v, &c->edict->v, sizeof c->old_v);
			for (k = 0; k < n; k++)
				if (a[k] != a[k])
					a[k] = b[k] = 0.0f;
			for (p = 0; p < 3; p++) {
				pages[i][p].changed_items = 0;
				pages[i][p].new_items = 0;
			}
		}
		/* DEBUG: force inventory flags to zero to isolate sc2 */
		if (getenv ("HARNESS_NOINV")) {
			int p;
			for (p = 0; p < 3; p++) {
				pages[i][p].changed_items = 0;
				pages[i][p].new_items = 0;
			}
		}
		if (rng_next () % 3 == 0) {
			/* up to and past the reliable limits, so the
			 * too-big-for-the-protocol drop runs; capped so the
			 * reliable_datagram append in SV_SendClientMessages
			 * cannot overflow MAX_MSGLEN */
			int n = rng_next () % 2 ? rng_int (0, 3000)
						: rng_int (15000, 30000);
			c->message.cursize = n % (int)sizeof msgbufs[i];
			memset (c->message.data, 0x77, (size_t)c->message.cursize);
			if (rng_next () % 5 == 0)
				c->message.overflowed = 1;
		}
		if (rng_next () % 3 == 0) {
			int n = rng_int (0, 2000);
			c->datagram.cursize = n % (int)sizeof dgrambufs[i];
			memset (c->datagram.data, 0x99, (size_t)c->datagram.cursize);
		}
	}

	/* stub behaviour for this case: wire must fit the fixed header
	 * (svc_time + float) plus whatever SV_WriteClientdataToMessage
	 * writes (it has no overflow check; the caller sizes the buffer).
	 * The real NET_MaxUnreliableMessage never returns less than the
	 * path MTU allows. */
	stub_max_unreliable = rng_int (2048, 4000);
	stub_is_local = rng_int (0, 3) == 0;
	stub_can_send = rng_int (0, 4) != 0;
	stub_send_unreliable_ret = rng_int (0, 5) == 0 ? -1 : 0;
	stub_send_ret = rng_int (0, 5) == 0 ? -1 : 0;

	if (case_kind == 1 || case_kind == 3) {
		case_client = rng_int (0, svs.maxclients - 1);
		host_client = &clients[case_client];
		SZ_Init (&case_msg, case_msg_buf, sizeof case_msg_buf);
		if (rng_next () % 2) {
			/* small enough to overflow, but never 0: the C's
			 * maxsize==0 branch writes unchecked and SZ_GetSpace
			 * aborts a zero-size buffer on the first byte */
			int n = rng_int (100, 30000);
			case_msg.maxsize = n;
			case_msg.cursize = rng_int (0, n);
			memset (case_msg.data, 0x3C, (size_t)case_msg.cursize);
		}
	}
}

/* ---- per-side serialization ---- */
static void serialize_side (void)
{
	int i, p;
	outlen = 0;
	if (case_kind == 0) {
		out_sizebuf (&sv.datagram);
		out_int (sv_datagram_dropped);
	} else if (case_kind == 1 || case_kind == 3) {
		client_t *c = &clients[case_client];
		out_sizebuf (&case_msg);
		out_sizebuf (&c->message);
		out_bytes (&c->old_v, sizeof c->old_v);
		out_bytes (&c->edict->v.dmg_take, sizeof (float));
		out_bytes (&c->edict->v.dmg_save, sizeof (float));
		out_bytes (&c->edict->v.fixangle, sizeof (float));
		out_int (c->send_all_v);
		out_bytes (&c->info_mask, sizeof c->info_mask);
		out_bytes (&c->info_mask2, sizeof c->info_mask2);
		/* the inventory page chain as the call left it */
		for (p = 0; p < 3; p++) {
			ex_inventory_page_t *pg = &pages[case_client][p];
			out_int (pg->changed_items);
			out_int (pg->new_items);
			out_bytes (pg->item_cnt, sizeof pg->item_cnt);
			out_bytes (pg->item_id, sizeof pg->item_id);
		}
		/* pages INV_UpdateExItem may have allocated via
		 * SV_ClientInventoryPage come from sv.ex_inventory_pages;
		 * serialize the whole backing when present */
		if (sv.ex_inventory_pages) {
			for (i = 0; i < 32; i++) {
				ex_inventory_page_t *pg = &sv.ex_inventory_pages[i];
				out_int (pg->id); out_int (pg->client_id);
				out_int (pg->changed_items); out_int (pg->new_items);
				out_bytes (pg->item_cnt, sizeof pg->item_cnt);
				out_bytes (pg->item_id, sizeof pg->item_id);
			}
		} else {
			out_int (-1);
		}
	} else {
		for (i = 0; i < svs.maxclients; i++) {
			client_t *c = &clients[i];
			out_sizebuf (&c->message);
			out_sizebuf (&c->datagram);
			out_int (c->signon_buffer);
			out_int (c->sendsignon);
			out_bytes (&c->last_message, sizeof c->last_message);
			out_int (c->active);
			out_bytes (&c->old_v, sizeof c->old_v);
			out_int (c->old_frags);
		}
		out_int (sv.reliable_datagram.cursize);
		out_int ((int)(host_client - svs.clients));
		/* SV_CleanupEnts clears EF_MUZZLEFLASH on every edict at the end
		 * of SV_SendClientMessages; capture it */
		for (i = 0; i < MAXEDICTS; i++)
			out_bytes (&edicts[i].v.effects, sizeof (float));
	}
	out_bytes (calllog, calllog_len);
	out_bytes (conlog, conlog_len);
}

/* ---- the child: run one side, write the output file ---- */
static int	harness_side;	/* 0 = C, 1 = Rust */
static char	out_path[256];

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
			 harness_side, case_kind, case_kind == 1 ? case_client : -1);
	}
	if (case_kind == 0) {
		if (harness_side == 0)
			c_SV_ClearDatagram ();
		else
			SV_ClearDatagram ();
	} else if (case_kind == 1) {
		client_t *c = &clients[case_client];
		if (harness_side == 0)
			c_SV_WriteClientdataToMessage (c, c->edict, &case_msg);
		else
			SV_WriteClientdataToMessage (c, c->edict, &case_msg);
	} else if (case_kind == 3) {
		/* forty back-to-back calls in one child: the only way to
		 * advance next_update/next_count through the whole 0..11
		 * forced-refresh cycle (a single frame never gets past 1) */
		client_t *c = &clients[case_client];
		int k;
		for (k = 0; k < 40; k++) {
			if (harness_side == 0)
				c_SV_WriteClientdataToMessage (c, c->edict, &case_msg);
			else
				SV_WriteClientdataToMessage (c, c->edict, &case_msg);
		}
	} else {
		if (harness_side == 0)
			c_SV_SendClientMessages ();
		else
			SV_SendClientMessages ();
	}
	serialize_side ();
	f = fopen (out_path, "wb");
	if (!f)
		_exit (3);
	fwrite (outbuf, 1, outlen, f);
	fclose (f);
#ifdef __GNUC__
	if (__gcov_dump)
		__gcov_dump ();
#endif
	_exit (0);
}

/* ---- main: generate cases, fork both sides, compare ---- */
static long	cases_run, cases_pass;

static int run_one_case (uint64_t seed, int kind, const char *tmpdir)
{
	pid_t pid;
	int status;
	char path_c[300], path_r[300];
	FILE *fc, *fr;
	long szc, szr;
	static char *bufc, *bufr;
	static size_t cap;

	snprintf (path_c, sizeof path_c, "%s/case.out", tmpdir);
	snprintf (path_r, sizeof path_r, "%s/rust.out", tmpdir);

	case_kind = kind;
	setup_case (seed);
	{
		uint32_t sum = 0, sum2 = 0;
		int i, cc = (case_kind == 1 || case_kind == 3) ? case_client : 0;
		for (i = 0; i < (int)sizeof(clients[cc].old_v); i++)
			sum += ((unsigned char *)&clients[cc].old_v)[i];
		for (i = 0; i < (int)sizeof(edicts[cc].v); i++)
			sum2 += ((unsigned char *)&edicts[cc].v)[i];
	}

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
	if (waitpid (pid, &status, 0) < 0 || !WIFEXITED (status) ||
	    WEXITSTATUS (status) != 0) {
		printf ("case %lu kind %d: C child failed (raw status %d, exited %d, code %d, sig %d)\n",
			cases_run, kind, status, WIFEXITED (status),
			WIFEXITED (status) ? WEXITSTATUS (status) : -1,
			WIFSIGNALED (status) ? WTERMSIG (status) : -1);
		return 1;
	}

	/* rebuild the identical setup: the C child may have mutated the
	 * parent's copy-on-write pages, but the parent's own memory is
	 * untouched by fork; re-running setup is belt and braces. */
	case_kind = kind;
	setup_case (seed);
	{
		uint32_t sum = 0, sum2 = 0;
		int i, cc = (case_kind == 1 || case_kind == 3) ? case_client : 0;
		for (i = 0; i < (int)sizeof(clients[cc].old_v); i++)
			sum += ((unsigned char *)&clients[cc].old_v)[i];
		for (i = 0; i < (int)sizeof(edicts[cc].v); i++)
			sum2 += ((unsigned char *)&edicts[cc].v)[i];
	}

	harness_side = 1;
	snprintf (out_path, sizeof out_path, "%s", path_r);
	pid = fork ();
	if (pid == 0)
		run_child ();
	if (waitpid (pid, &status, 0) < 0 || !WIFEXITED (status) ||
	    WEXITSTATUS (status) != 0) {
		printf ("case %lu kind %d: Rust child failed (status %d)\n",
			cases_run, kind, status);
		return 1;
	}

	fc = fopen (path_c, "rb"); fr = fopen (path_r, "rb");
	if (!fc || !fr) { printf ("case %lu: output open failed\n", cases_run); return 1; }
	fseek (fc, 0, SEEK_END); szc = ftell (fc); fseek (fc, 0, SEEK_SET);
	fseek (fr, 0, SEEK_END); szr = ftell (fr); fseek (fr, 0, SEEK_SET);
	if ((size_t)(szc > szr ? szc : szr) > cap) {
		cap = (size_t)(szc > szr ? szc : szr) + 1024;
		bufc = realloc (bufc, cap); bufr = realloc (bufr, cap);
	}
	if ((size_t)fread (bufc, 1, (size_t)szc, fc) != (size_t)szc ||
	    (size_t)fread (bufr, 1, (size_t)szr, fr) != (size_t)szr) {
		printf ("case %lu: short read on the outputs\n", cases_run);
		fclose (fc); fclose (fr);
		return 1;
	}
	fclose (fc); fclose (fr);
	cases_run++;
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

/* Directed cases the generator is unlikely to hit: values on the exact
 * edges the code tests.  Each returns nonzero on mismatch. */
static int directed (const char *tmpdir)
{
	int bad = 0;
	uint64_t seed;

	/* every next_update/next_count phase: 12 forced-refresh cycles */
	for (seed = 1; seed <= 24; seed++) {
		bad |= run_one_case (0xC0DA0000ULL + seed, 1, tmpdir);
		bad |= run_one_case (0xC0DA1000ULL + seed, 2, tmpdir);
	}
	/* protocol edges, one per supported version and around them */
	{
		static const int protos[] = {17, 18, 19, 20, 21, 22, 99, 100, 101};
		size_t k;
		for (k = 0; k < sizeof protos / sizeof protos[0]; k++) {
			uint64_t s = 0xD1A61000ULL + (uint64_t)protos[k];
			bad |= run_one_case (s, 1, tmpdir);
			bad |= run_one_case (s + 0x1000, 2, tmpdir);
		}
	}
	/* send paths: unreliable refused, reliable refused, local vs not */
	for (seed = 0; seed < 40; seed++)
		bad |= run_one_case (0x5EED0000ULL + seed, 2, tmpdir);
	/* the next_update/next_count forced-refresh cycle, forty calls per
	 * child so all twelve phases (and the wrap) actually run */
	for (seed = 0; seed < 4; seed++)
		bad |= run_one_case (0xC7C10000ULL + seed, 3, tmpdir);
	/* clear-datagram with every flag state */
	for (seed = 0; seed < 8; seed++)
		bad |= run_one_case (0xC1EA0000ULL + seed, 0, tmpdir);
	return bad;
}

int main (int argc, char **argv)
{
	char tmpdir[] = "/tmp/datagram_harness_XXXXXX";
	long i, target, case_offset = 0;
	int bad = 0;

	if (!mkdtemp (tmpdir)) { perror ("mkdtemp"); return 2; }
	if (check_facts ())
		return 2;

	target = argc > 1 ? atol (argv[1]) : 12000;
	/* Chunked runs: HARNESS_CASE_OFFSET shifts the enumerated seeds, so a
	 * full enumeration too fork-heavy for one process (fork() fails with
	 * ENOMEM after ~900 forks on small machines) can be covered in
	 * several processes with disjoint seeds.  HARNESS_SKIP_DIRECTED
	 * skips the directed edges, which are identical in every chunk. */
	{
		const char *off = getenv ("HARNESS_CASE_OFFSET");
		if (off)
			case_offset = atol (off);
	}
	printf ("sv_main_datagram differential harness\n");

	/* two thirds messages-frames, the rest split over the small ones.
	 * HARNESS_KIND=0/1/2 runs only one loop, for chunked full
	 * enumerations with per-kind offsets. */
	{
		const char *kf = getenv ("HARNESS_KIND");
		int only = kf ? atoi (kf) : -1;
		if (only < 0 || only == 2)
	for (i = 0; i < target * 2 / 3; i++) {
		if (run_one_case (0x10000000ULL + (uint64_t)(i + case_offset), 2, tmpdir))
			bad = 1;
	}
		if (only < 0 || only == 1)
	for (i = 0; i < target / 6; i++) {
		if (run_one_case (0x20000000ULL + (uint64_t)(i + case_offset), 1, tmpdir))
			bad = 1;
	}
		if (only < 0 || only == 0)
	for (i = 0; i < target / 6; i++) {
		if (run_one_case (0x30000000ULL + (uint64_t)(i + case_offset), 0, tmpdir))
			bad = 1;
	}
	}
	if (!getenv ("HARNESS_SKIP_DIRECTED"))
		bad |= directed (tmpdir);

	printf ("sv_main_datagram differential harness (%s): %s (%ld cases, %ld pass)\n",
		"h2", bad ? "FAIL" : "PASS", cases_run, cases_pass);
	return bad ? 1 : 0;
}
