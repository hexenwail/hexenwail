/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * inv_harness.c -- engine/rust/src/sv_main_inv.rs against the C original of
 * Hexen II's sv_main.c extended inventory, worldspawn sky, level name and
 * savegame helpers, in one binary (#469 slice E, unit E1).
 *
 * run_inv_harness.sh builds this against sv_main_inv_h2.c (-DSERVERONLY, as
 * h2ded compiles it), with the real engine/rust/world_target.c and
 * engine/rust/sv_main_target.c compiled in, COM_Parse extracted from
 * engine/h2shared/common.c and q_strlcpy from common/strlcpy.c.  The eight
 * ported functions go in renamed to c_*.
 *
 * Each case builds the state the function reads, once, in the parent; then
 * forks twice from that snapshot, once to run the C and once to run the Rust.
 * The child serializes everything the call can have written -- the pages
 * (every byte, changed_items/new_items and the ->next links included),
 * sv.next_page_id, sv.skybox and com_token, svs.serverflags, host_client and
 * the clients, the return value, and for the stdio functions the exact bytes
 * in the file and the stream's position, EOF and error flags and every byte
 * left unread -- plus the call log (COM_Parse, q_strlcpy, Host_GetString,
 * PR_GetString, Host_Error, with their arguments and results) to a file; the
 * parent compares the two files byte for byte, and the two exit statuses.
 *
 * How a child ends is part of what is compared:
 *
 *   0   returned.
 *   42  Host_Error (the stub serializes, then exits; Host_Error does not
 *       return).
 *   43  C side only: SV_LoadInventory's item loop was about to index
 *       item_id[MAX_INVENTORY_EX] -- undefined behaviour.  The fixture's
 *       fgetc is observed (c_ref_inv.c), and the observer stops the C there,
 *       before it writes past the page.
 *   44  Rust side only: abort(), the port's bounds check at the same point.
 *       43 and 44 are the same outcome; everything serialized must still
 *       match, so the two stopped at the same byte of the stream with the
 *       same pages.
 *   45  SIGALRM: the call did not return within SPIN_MS (150 ms).  INV_UpdateExItem
 *       spins forever when it links a full page back into its own chain (a
 *       C defect, kept); both sides must spin, with the same state.
 *
 * Before the cases, the facts table sv_main_inv.rs reads is checked against
 * this compile of the Hexen II headers, and its layout against the Rust's.
 */

#include "quakedef.h"
#include "sv_main_target.h"

#include <errno.h>
#include <execinfo.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* the ported functions: C side renamed, Rust side as exported */
void	c_SV_ParseWorldspawnSky (void);
ex_inventory_page_t *c_SV_ClientInventoryPage (int clientnum);
int	c_INV_UpdateExItem (ex_inventory_page_t *startPage, int inv_id, int inv_cnt, qboolean inc);
const char *c_SV_GetLevelname (void);
void	c_SV_SaveSpawnparms (void);
void	c_INV_WritePage (FILE *f, ex_inventory_page_t *page, int clientId);
void	c_SV_LoadInventory (FILE *FH);
void	c_INV_SavePages (FILE *FH);
/* the Rust's; server.h declares the other seven */
void	SV_ParseWorldspawnSky (void);
/* sv_main_inv.rs's layout accessors */
size_t	SvMainInvFacts_sizeof (void);
size_t	SvMainInvFacts_offsetof_max_clients (void);
size_t	SvMainInvFacts_offsetof_ex_size (void);
size_t	SvMainInvFacts_offsetof_ex_next (void);
size_t	SvMainInvFacts_offsetof_client_ex_inventory (void);
size_t	SvMainInvFacts_offsetof_skybox_size (void);
/* the engine's own C, renamed by the runner and wrapped below */
const char *real_COM_Parse (const char *data);
size_t	real_q_strlcpy (char *dst, const char *src, size_t size);

/* ---- the engine's storage ---- */
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
qboolean	isDedicated = true;
sv_globals_t	sv_globals;
areanode_t	sv_areanodes[AREA_NODES];
sizebuf_t	net_message;
client_t	*host_client;
double		realtime;
unsigned int	info_mask, info_mask2;
qboolean	sv_datagram_dropped;
cvar_t		sv_maxdatagram = {"sv_maxdatagram", "0", CVAR_NONE};
char		*pr_strings;
int		pr_stringssize;
int		host_string_count;

/* a call that has not returned after this long is spinning: every
 * function here returns in microseconds when it returns at all */
#define SPIN_MS	150

/* ---- per-case backing stores ---- */
#define NSTORE	((MAX_CLIENTS + 1) * MAX_INVENTORY_EX_PAGES)
static ex_inventory_page_t page_store[NSTORE];
static client_t	clients[MAX_CLIENTS + 1];
static edict_t	edicts[2];
static qmodel_t	world_model;
static char	lump[16384];
static float	progs_serverflags;
static char	*host_strings[16];
static char	host_string_store[16][32];
static char	prog_string_heap[256];

/* ---- the call log ---- */
#define LOGSIZE	(1 << 20)
static char	calllog[LOGSIZE];
static size_t	calllog_len;

static void log_call (const char *fmt, ...)
{
	va_list ap;
	char tmp[4096];
	int n;
	va_start (ap, fmt);
	n = vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	if (n <= 0)
		return;
	if ((size_t)n >= sizeof tmp)
		n = sizeof tmp - 1;
	if (calllog_len + (size_t)n < LOGSIZE) {
		memcpy (calllog + calllog_len, tmp, (size_t)n);
		calllog_len += (size_t)n;
	}
}

/* ---- case state, set by the parent, read by the child ---- */
enum {
	K_SKY, K_PAGE, K_UPDATE, K_LEVELNAME, K_SPAWNPARMS,
	K_WRITEPAGE, K_LOAD, K_SAVEPAGES, K_ROUNDTRIP, NKINDS
};
static const char *kind_names[NKINDS] = {
	"sky", "page", "update", "levelname", "spawnparms",
	"writepage", "load", "savepages", "roundtrip"
};
static int	case_kind;
static int	harness_side;		/* 0 = C, 1 = Rust */
static int	case_clientnum;		/* K_PAGE */
static int	case_start;		/* K_UPDATE / K_WRITEPAGE: page index, -1 = NULL */
static int	case_inv_id, case_inv_cnt, case_inc;
static int	case_client_id;		/* K_WRITEPAGE */
static int	case_preread;		/* K_LOAD: chars fgetc'd before the call */
static int	case_unget;		/* K_LOAD: then ungetc this char, or -1 */
static int	case_vbuf;		/* stdio buffering: 0 full, 1 line, 2 none */
static char	case_prefix[64];	/* writers: bytes written before the call */
static char	in_path[300], save_path[300], out_path[300];
static FILE	*case_fp;
static intptr_t	case_ret;		/* the return value, raw */
static int	case_ret_valid;

/* ---- output serialization ---- */
static char	*outbuf;
static size_t	outlen, outcap;

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
static void out_int (int v) { out_bytes (&v, sizeof v); }
static void out_long (long v) { out_bytes (&v, sizeof v); }

static void out_pages (void)
{
	out_bytes (page_store, sizeof page_store);
	out_int (sv.next_page_id);
	out_bytes (&sv.ex_inventory_pages, sizeof sv.ex_inventory_pages);
}

/* the stream after a read: position, flags, then every byte left */
static void out_read_stream (void)
{
	char buf[4096];
	size_t n;
	if (!case_fp) {
		out_int (-1);
		return;
	}
	out_long (ftell (case_fp));
	out_int (feof (case_fp) != 0);
	out_int (ferror (case_fp) != 0);
	while ((n = fread (buf, 1, sizeof buf, case_fp)) > 0)
		out_bytes (buf, n);
	fclose (case_fp);
	case_fp = NULL;
}

/* a written file: position and flags, then the whole file from disk */
static void out_written_file (void)
{
	char buf[4096];
	size_t n;
	FILE *f;
	if (!case_fp) {
		out_int (-1);
		return;
	}
	fflush (case_fp);
	out_long (ftell (case_fp));
	out_int (ferror (case_fp) != 0);
	fclose (case_fp);
	case_fp = NULL;
	f = fopen (save_path, "rb");
	if (!f) {
		out_int (-2);
		return;
	}
	while ((n = fread (buf, 1, sizeof buf, f)) > 0)
		out_bytes (buf, n);
	fclose (f);
}

static void serialize_side (void)
{
	outlen = 0;
	out_int (case_ret_valid);
	out_bytes (&case_ret, sizeof case_ret);
	switch (case_kind) {
	case K_SKY:
		out_bytes (sv.skybox, sizeof sv.skybox);
		out_bytes (com_token, sizeof com_token);
		break;
	case K_PAGE:
	case K_UPDATE:
		out_pages ();
		break;
	case K_LEVELNAME:
		if (case_ret_valid && case_ret)
			out_bytes ((const char *)case_ret, strlen ((const char *)case_ret) + 1);
		break;
	case K_SPAWNPARMS:
		out_int (svs.serverflags);
		out_bytes (&host_client, sizeof host_client);
		out_bytes (clients, sizeof clients);
		break;
	case K_WRITEPAGE:
	case K_SAVEPAGES:
	case K_ROUNDTRIP:
		out_written_file ();
		out_pages ();
		out_bytes (&host_client, sizeof host_client);
		out_bytes (clients, sizeof clients);
		break;
	case K_LOAD:
		out_read_stream ();
		out_pages ();
		break;
	}
	out_bytes (calllog, calllog_len);
}

/* serialize, write the output file, exit with `code' -- every way a child
 * ends goes through here.  exit(), not _exit(): libgcov's exit handler
 * dumps the coverage. */
static void finish_child (int code)
{
	static int finishing;
	FILE *f;
	if (finishing)
		_exit (99);
	finishing = 1;
	serialize_side ();
	f = fopen (out_path, "wb");
	if (!f)
		_exit (3);
	fwrite (outbuf, 1, outlen, f);
	fclose (f);
	exit (code);
}

static void crash_handler (int sig)
{
	void *bt[32];
	int n = backtrace (bt, 32);
	fprintf (stderr, "child (side %d, kind %s) crashed with signal %d:\n",
		 harness_side, kind_names[case_kind], sig);
	backtrace_symbols_fd (bt, n, 2);
	_exit (100 + sig);
}
static void abort_handler (int sig) { (void)sig; finish_child (44); }
static void alarm_handler (int sig) { (void)sig; finish_child (45); }

/* ---- the engine functions the ported code calls ---- */

/* COM_Parse is engine/h2shared/common.c's, extracted by the runner; this
 * logs each call: where in the lump it starts, where it stops, the token. */
static long lump_offset (const char *p)
{
	if (!p)
		return -1;
	if (p >= lump && p <= lump + sizeof lump)
		return (long)(p - lump);
	return -2;
}

const char *COM_Parse (const char *data)
{
	const char *r = real_COM_Parse (data);
	log_call ("COM_Parse(%ld) -> %ld [%s]\n", lump_offset (data),
		  lump_offset (r), com_token);
	return r;
}

/* q_strlcpy is common/strlcpy.c's (the engine's is its Rust port, held to
 * the same text by its frozen fixtures). */
size_t q_strlcpy (char *dst, const char *src, size_t size)
{
	size_t r = real_q_strlcpy (dst, src, size);
	log_call ("q_strlcpy(%s, [%s], %zu) -> %zu\n",
		  dst == sv.skybox ? "skybox" : "local", src, size, r);
	return r;
}

const char *Host_GetString (int idx)
{
	log_call ("Host_GetString(%d)\n", idx);
	if (idx >= 0 && idx < host_string_count && idx < 16)
		return host_strings[idx];
	return "<Host_GetString out of range>";
}

const char *PR_GetString (int num)
{
	log_call ("PR_GetString(%d)\n", num);
	if (num >= 0 && num < pr_stringssize)
		return pr_strings + num;
	return "<PR_GetString out of range>";
}

FUNC_NORETURN void Host_Error (const char *error, ...)
{
	va_list	ap;
	char	buf[1024], *p;
	va_start (ap, error);
	vsnprintf (buf, sizeof buf, error, ap);
	va_end (ap);
	/* __func__ in the fixture is c_SV_LoadInventory */
	if ((p = strstr (buf, "c_SV_")) != NULL)
		memmove (p, p + 2, strlen (p + 2) + 1);
	log_call ("Host_Error: %s\n", buf);
	finish_child (42);
	_exit (99);
}

FUNC_NORETURN void Sys_Error (const char *error, ...)
{
	va_list	ap;
	char	buf[1024];
	va_start (ap, error);
	vsnprintf (buf, sizeof buf, error, ap);
	va_end (ap);
	log_call ("Sys_Error: %s\n", buf);
	finish_child (41);
	_exit (99);
}

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list ap;
	char tmp[2048];
	va_start (ap, fmt);
	vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	log_call ("[%u] %s", flags, tmp);
}

/* the C side's stdio observers (c_ref_inv.c): the C library's vfscanf and
 * fgetc, plus the one undefined-behaviour stop described in the header */
static int	c_item_fgetcs;	/* fgetc calls since the last "Page:" scan */

int inv_harness_c_fscanf (FILE *f, const char *fmt, ...)
{
	va_list ap;
	int r;
	if (strncmp (fmt, "Page:", 5) == 0)
		c_item_fgetcs = 0;
	va_start (ap, fmt);
	r = vfscanf (f, fmt, ap);
	va_end (ap);
	return r;
}

int inv_harness_c_fgetc (FILE *f)
{
	int c = fgetc (f);
	/* fgetc number n after the page scan starts the body with
	 * item_idx = n - 1; item_idx == MAX_INVENTORY_EX is the overrun */
	if (++c_item_fgetcs > MAX_INVENTORY_EX && c != '\n' && c != '\r')
		finish_child (43);
	return c;
}

/* ---- stubs the archive needs but no case reaches ---- */
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
}

int NUM_FOR_EDICT (edict_t *e)
{
	return (int)(((byte *)e - (byte *)sv.edicts) / pr_edict_size);
}

edict_t *EDICT_NUM (int n)
{
	if (n < 0 || n >= 2) {
		fprintf (stderr, "harness bug: EDICT_NUM %d\n", n);
		abort ();
	}
	return &edicts[n];
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
	return lo + (int)(rng_next () % (uint64_t)((int64_t)hi - lo + 1));
}
static int rng_pct (int pct) { return (int)(rng_next () % 100) < pct; }
static int rng_extreme_int (void)
{
	static const int v[] = {INT_MIN, INT_MIN + 1, -1, 0, 1, INT_MAX - 1, INT_MAX};
	return v[rng_next () % (sizeof v / sizeof v[0])];
}
static float rng_float (void)
{
	static const float v[] = {
		0.0f, -0.0f, 0.5f, 0.99999994f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f,
		5.9999995f, 6.0f, -1.0f, -0.99999994f, 255.0f, 256.0f, 1e9f,
		2147483520.0f, 2147483648.0f, -2147483648.0f, -2147483904.0f,
		1e30f, -1e30f, 1e-30f, NAN, -NAN, INFINITY, -INFINITY
	};
	uint64_t r = rng_next ();
	if (r % 4 == 0) {
		union { uint32_t u; float f; } b;
		b.u = (uint32_t)(r >> 8);
		return b.f;
	}
	if (r % 4 == 1)
		return (float)rng_int (-8, 24) + (float)(r % 3) * 0.25f;
	return v[(r >> 8) % (sizeof v / sizeof v[0])];
}

/* ---- the facts table, checked against this compile's headers ---- */
static int check_facts (void)
{
	const sv_main_inv_facts_t *f = &SvMainInv_TargetFacts;
	int bad = 0;
#define CKF(name, expect) \
	if (f->name != (expect)) { \
		printf ("facts mismatch: %s = %d, header says %d\n", \
			#name, (int)f->name, (int)(expect)); bad = 1; }
#define CKZ(name, expect) \
	if ((size_t)f->name != (size_t)(expect)) { \
		printf ("facts mismatch: %s = %zu, header says %zu\n", \
			#name, (size_t)f->name, (size_t)(expect)); bad = 1; }
	CKF (max_inventory_ex, MAX_INVENTORY_EX);
	CKF (max_inventory_ex_pages, MAX_INVENTORY_EX_PAGES);
	CKF (max_clients, MAX_CLIENTS);
	CKZ (ex_size, sizeof (ex_inventory_page_t));
	CKZ (ex_id, offsetof (ex_inventory_page_t, id));
	CKZ (ex_client_id, offsetof (ex_inventory_page_t, client_id));
	CKZ (ex_changed_items, offsetof (ex_inventory_page_t, changed_items));
	CKZ (ex_new_items, offsetof (ex_inventory_page_t, new_items));
	CKZ (ex_item_id, offsetof (ex_inventory_page_t, item_id));
	CKZ (ex_item_cnt, offsetof (ex_inventory_page_t, item_cnt));
	CKZ (ex_next, offsetof (ex_inventory_page_t, next));
	CKZ (client_size, sizeof (client_t));
	CKZ (client_active, offsetof (client_t, active));
	CKZ (client_ex_inventory, offsetof (client_t, ex_inventory));
	CKZ (edict_v, offsetof (edict_t, v));
	CKZ (v_message, offsetof (entvars_t, message));
	CKZ (v_netname, offsetof (entvars_t, netname));
	CKZ (qmodel_entities, offsetof (qmodel_t, entities));
	CKZ (skybox_size, sizeof (sv.skybox));
	/* the struct itself, against the Rust's mirror of it */
	CKZ (ex_size, sizeof (ex_inventory_page_t));
	if (SvMainInvFacts_sizeof () != sizeof (sv_main_inv_facts_t) ||
	    SvMainInvFacts_offsetof_max_clients () != offsetof (sv_main_inv_facts_t, max_clients) ||
	    SvMainInvFacts_offsetof_ex_size () != offsetof (sv_main_inv_facts_t, ex_size) ||
	    SvMainInvFacts_offsetof_ex_next () != offsetof (sv_main_inv_facts_t, ex_next) ||
	    SvMainInvFacts_offsetof_client_ex_inventory () != offsetof (sv_main_inv_facts_t, client_ex_inventory) ||
	    SvMainInvFacts_offsetof_skybox_size () != offsetof (sv_main_inv_facts_t, skybox_size)) {
		printf ("facts mismatch: sv_main_inv_facts_t's layout differs from SvMainInvFacts\n");
		bad = 1;
	}
#undef CKF
#undef CKZ
	if (!bad)
		printf ("layouts: sv_main_inv facts match the headers\n");
	return bad;
}

/* ---- generators ---- */

static void gen_pages (int maxcl)
{
	int n = maxcl * MAX_INVENTORY_EX_PAGES, i, j, k;
	int perm[NSTORE];
	int style = (int)(rng_next () % 6);
	if (n < 1)
		n = 1;
	if (n > NSTORE)
		n = NSTORE;
	memset (page_store, 0, sizeof page_store);
	for (i = 0; i < n; i++) {
		ex_inventory_page_t *p = &page_store[i];
		int used = style == 0 ? 0 : style == 1 ? rng_pct (90) : rng_pct (50);
		if (!used) {
			if (rng_pct (10)) {	/* a free page that is not clean */
				p->item_id[rng_int (0, 31)] = rng_int (1, 40);
				p->item_cnt[rng_int (0, 31)] = rng_int (-3, 9);
			}
			continue;
		}
		p->id = rng_pct (10) ? rng_int (-3, 3) : rng_pct (5) ? INT_MAX - rng_int (0, 2) : rng_int (1, 200);
		p->client_id = rng_pct (12) ? rng_int (-2, 20) : rng_int (0, maxcl > 1 ? maxcl - 1 : 0);
		switch (rng_next () % 12) {
		case 0: case 1: case 2:
			break;				/* empty */
		case 3:					/* full */
			for (k = 0; k < MAX_INVENTORY_EX; k++) {
				p->item_id[k] = rng_pct (90) ? 100 + k + 32 * i : rng_extreme_int ();
				if (!p->item_id[k])
					p->item_id[k] = 7;
				p->item_cnt[k] = rng_int (-2, 300);
			}
			break;
		default:
			for (j = rng_int (1, 12); j > 0; j--) {
				k = rng_int (0, 31);
				p->item_id[k] = rng_pct (8) ? rng_extreme_int () : rng_int (0, 40);
				p->item_cnt[k] = rng_pct (8) ? rng_extreme_int () : rng_int (-5, 300);
			}
			break;
		}
		p->changed_items = (int)rng_next ();
		p->new_items = rng_pct (50) ? 0 : (int)rng_next ();
		for (k = 0; k < MAX_INVENTORY_EX; k++) {
			p->inv_order[k] = (int)(rng_next () % 64);
			p->item_gettime[k] = (float)(rng_next () % 1000) * 0.125f;
		}
	}
	/* the ->next links: forward along a permutation, so never a cycle;
	 * half the time the permutation groups each owner's pages */
	for (i = 0; i < n; i++)
		perm[i] = i;
	for (i = n - 1; i > 0; i--) {
		int t;
		j = rng_int (0, i);
		t = perm[i]; perm[i] = perm[j]; perm[j] = t;
	}
	if (rng_pct (50)) {
		for (i = 1; i < n; i++) {	/* insertion sort by owner */
			int t = perm[i];
			for (j = i - 1; j >= 0 &&
			     page_store[perm[j]].client_id > page_store[t].client_id; j--)
				perm[j + 1] = perm[j];
			perm[j + 1] = t;
		}
	}
	for (i = 0; i + 1 < n; i++)
		if (rng_pct (35))
			page_store[perm[i]].next = &page_store[perm[i + 1]];
	sv.ex_inventory_pages = page_store;
	sv.next_page_id = rng_pct (5) ? INT_MAX - rng_int (0, 1) : rng_pct (5) ? rng_int (-5, 0) : rng_int (0, 210);
}

static void gen_clients (int with_inventory)
{
	int i;
	memset (clients, 0, sizeof clients);
	for (i = 0; i <= MAX_CLIENTS; i++) {
		clients[i].active = rng_pct (60) ? 1 : rng_pct (50) ? 0 : rng_int (-1, 3);
		if (with_inventory && rng_pct (70))
			clients[i].ex_inventory = &page_store[rng_int (0, NSTORE - 1)];
		clients[i].spawned = rng_int (0, 1);
		clients[i].name[0] = 'a' + i;
	}
	svs.clients = clients;
	host_client = rng_pct (50) ? &clients[rng_int (0, MAX_CLIENTS)] : (client_t *)(uintptr_t)0x5a5a5a50;
}

/* SV_ParseWorldspawnSky's lump */
static size_t lumpcat (size_t len, const char *s)
{
	size_t n = strlen (s);
	if (len + n + 1 >= sizeof lump)
		return len;
	memcpy (lump + len, s, n + 1);
	return len + n;
}

static void long_key (char *out, size_t total, const char *head, char fill, const char *tail)
{
	size_t h = strlen (head), t = strlen (tail), i;
	memcpy (out, head, h);
	for (i = h; i + t < total; i++)
		out[i] = fill;
	memcpy (out + i, tail, t);
	out[total] = 0;
}

static const char *rand_key (char *buf)
{
	static const char *keys[] = {
		"sky", "_sky", "sky ", "sky   ", "_sky  ", "skyname", "_skyname",
		"skyname ", "qlsky", "_qlsky", "qlsky ", "Sky", "SKY", "skyy", "sk",
		"", "_", " ", "  ", " sky", "__sky", "_ sky", "sky\t", "sky_",
		"classname", "message", "wad", "worldtype", "light", "}", "{",
		"}sky", "sky}", "//", "/*x*/", "sk y",
	};
	uint64_t r = rng_next ();
	switch (r % 9) {
	case 0: {
		/* the 127/128-byte key edges: key[128] holds 127 */
		static const size_t lens[] = {126, 127, 128, 129, 200, 1023, 1100};
		size_t L = lens[(r >> 8) % 7];
		switch ((r >> 16) % 5) {
		case 0: long_key (buf, L, "sky", ' ', ""); break;
		case 1: long_key (buf, L, "sky", ' ', "x"); break;
		case 2: long_key (buf, L, "_sky", ' ', "x"); break;
		case 3: long_key (buf, L, "", 'k', "sky"); break;
		default: long_key (buf, L, "_skyname", ' ', ""); break;
		}
		return buf;
	}
	default:
		return keys[(r >> 8) % (sizeof keys / sizeof keys[0])];
	}
}

static const char *rand_value (char *buf)
{
	static const char *vals[] = {
		"", "sky1", "storm", "night", "a b c", "}", "{", "\"", "x",
		"   ", "skybox/foo_", "\xe9\xe8t\xe9", "//c", "\t",
	};
	uint64_t r = rng_next ();
	if (r % 5 == 0) {
		static const size_t lens[] = {62, 63, 64, 65, 200, 1022, 1023, 1024, 1500};
		size_t L = lens[(r >> 8) % 9], i;
		for (i = 0; i < L; i++)
			buf[i] = (char)('a' + (i % 26));
		buf[L] = 0;
		return buf;
	}
	return vals[(r >> 8) % (sizeof vals / sizeof vals[0])];
}

static void append_token (size_t *len, const char *tok, int quote)
{
	static const char *seps[] = {" ", "\n", "\t", "  \n ", "\r\n", " // note\n", " /* c */ ", "\x80", ""};
	int need_quote = quote || !*tok || strpbrk (tok, " \t\r\n\"") != NULL;
	if (need_quote && strchr (tok, '"')) {
		/* a quote cannot be inside a quoted token; write it bare */
		*len = lumpcat (*len, tok);
	} else if (need_quote) {
		*len = lumpcat (*len, "\"");
		*len = lumpcat (*len, tok);
		*len = lumpcat (*len, "\"");
	} else {
		*len = lumpcat (*len, tok);
	}
	*len = lumpcat (*len, seps[rng_next () % (sizeof seps / sizeof seps[0] - (quote ? 0 : 1))]);
}

static void gen_lump (void)
{
	size_t len = 0;
	int pairs, p, ents, e;
	char kb[1200], vb[1600];
	lump[0] = 0;
	if (rng_pct (8)) {
		sv.worldmodel = NULL;
		return;
	}
	sv.worldmodel = &world_model;
	if (rng_pct (6)) {
		world_model.entities = NULL;
		return;
	}
	world_model.entities = lump;
	switch (rng_next () % 14) {
	case 0: len = lumpcat (len, ""); break;
	case 1: len = lumpcat (len, " \n\t "); break;
	case 2: len = lumpcat (len, "// comment\n"); break;
	case 3: len = lumpcat (len, "/* c */ "); break;
	case 4: len = lumpcat (len, "/* unterminated"); break;
	default: break;
	}
	switch (rng_next () % 12) {
	case 0: len = lumpcat (len, "x\n"); break;
	case 1: len = lumpcat (len, "{x "); break;
	case 2: len = lumpcat (len, "\"{\"\n"); break;
	case 3: len = lumpcat (len, "}\n"); break;
	default: len = lumpcat (len, "{\n"); break;
	}
	ents = rng_pct (70) ? 1 : rng_int (2, 3);
	for (e = 0; e < ents; e++) {
		pairs = rng_pct (10) ? 0 : rng_int (1, 8);
		for (p = 0; p < pairs; p++) {
			append_token (&len, rand_key (kb), rng_pct (85));
			if (rng_pct (3) && p == pairs - 1 && e == ents - 1)
				return;		/* a key with no value */
			append_token (&len, rand_value (vb), rng_pct (85));
		}
		if (rng_pct (5) && e == ents - 1) {
			len = lumpcat (len, "\"sky\" \"trunc");	/* unterminated quote */
			return;
		}
		if (rng_pct (90))
			len = lumpcat (len, "}\n");
		if (e + 1 < ents)
			len = lumpcat (len, "{\n\"sky\" \"later\"\n\"classname\" \"light\"\n");
	}
}

/* SV_LoadInventory's input text */
static char	textbuf[16384];
static size_t	textlen;

static void textcat (const char *s)
{
	size_t n = strlen (s);
	if (textlen + n + 1 >= sizeof textbuf)
		return;
	memcpy (textbuf + textlen, s, n + 1);
	textlen += n;
}

static void textf (const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	va_start (ap, fmt);
	vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	textcat (tmp);
}

static const char *rand_eol (int allow_eof)
{
	static const char *eols[] = {"\n", "\n", "\n", "\n", "\n", "\n", "\r\n", "\r\n", "\r", " \n", "\t\n", "\n\n", "  \r\n", ""};
	size_t n = sizeof eols / sizeof eols[0] - (allow_eof ? 0 : 1);
	return eols[rng_next () % n];
}

static int rand_total (void)
{
	static const int t[] = {-1, 0, 1, 2, 3, 4, 8, 15, MAX_CLIENTS, MAX_CLIENTS + 1, 100, INT_MIN, INT_MAX};
	if (rng_pct (40))
		return rng_int (0, 4);
	return t[rng_next () % (sizeof t / sizeof t[0])];
}

static void gen_load_text (int maxcl)
{
	int total = rand_total (), lines, l, k, pairs;
	textlen = 0;
	textbuf[0] = 0;
	/* bytes the caller has already read past (case_preread) */
	for (k = 0; k < case_preread; k++)
		textcat (rng_pct (50) ? "m" : "\n");
	switch (rng_next () % 16) {
	case 0: textf ("Pages:%d", total); break;
	case 1: textf ("Pages:   %d", total); break;
	case 2: textf ("pages: %d", total); break;
	case 3: textcat ("Pages: x"); break;
	case 4: textcat (""); break;
	case 5: textf ("Pages: +%d", total); break;
	case 6: textcat ("Pages: 99999999999"); break;
	case 7: textcat ("Pages: -0"); break;
	default: textf ("Pages: %d", total); break;
	}
	textcat (rand_eol (1));
	lines = rng_pct (25) ? rng_int (0, 18) : (total < 0 ? 0 : total > 18 ? 18 : total);
	for (l = 0; l < lines; l++) {
		int id = rng_pct (10) ? rng_extreme_int () : rng_int (-2, 60);
		int nxt = rng_pct (50) ? -1 : rng_int (-2, 60);
		int cl = rng_pct (10) ? rng_int (-3, 40) : rng_int (0, maxcl > 0 ? maxcl : 0);
		switch (rng_next () % 20) {
		case 0: textf ("Page:%d %d %d", id, nxt, cl); break;
		case 1: textf ("Page: %d %d", id, nxt); break;
		case 2: textcat ("Page: x"); break;
		case 3: textcat ("Page:"); break;
		case 4: textf ("page: %d %d %d", id, nxt, cl); break;
		case 5: textf ("Page: %d\t%d\n%d", id, nxt, cl); break;
		default: textf ("Page: %d %d %d", id, nxt, cl); break;
		}
		pairs = rng_pct (3) ? rng_int (33, 40) : rng_pct (6) ? MAX_INVENTORY_EX : rng_int (0, 8);
		for (k = 0; k < pairs; k++) {
			switch (rng_next () % 30) {
			case 0: textf (" %d", rng_int (1, 40)); break;
			case 1: textcat (" x"); break;
			case 2: textf ("  %d  %d", rng_int (1, 40), rng_int (-5, 300)); break;
			case 3: textf (" %d %d", rng_extreme_int (), rng_extreme_int ()); break;
			default: textf (" %d %d", rng_int (1, 40), rng_int (-5, 300)); break;
			}
		}
		textcat (rand_eol (l == lines - 1));
	}
	switch (rng_next () % 5) {
	case 0: break;
	case 1: textcat ("-1\n"); break;
	default: textcat ("-1\n{\n\"serverflags\" \"0\"\n}\n"); break;
	}
	/* sometimes, mutate the text */
	if (rng_pct (20)) {
		static const char alpha[] = " \t\r\n0123456789-+Pagesx:";
		int m;
		for (m = rng_int (1, 6); m > 0 && textlen > 0; m--) {
			size_t at = (size_t)rng_int (0, (int)textlen - 1);
			switch (rng_next () % 3) {
			case 0:
				textbuf[at] = alpha[rng_next () % (sizeof alpha - 1)];
				break;
			case 1:
				if (textlen + 2 < sizeof textbuf) {
					memmove (textbuf + at + 1, textbuf + at, textlen - at + 1);
					textbuf[at] = alpha[rng_next () % (sizeof alpha - 1)];
					textlen++;
				}
				break;
			default:
				memmove (textbuf + at, textbuf + at + 1, textlen - at);
				textlen--;
				break;
			}
		}
	}
}

static void write_input (void)
{
	FILE *f = fopen (in_path, "wb");
	if (!f) { perror ("fopen input"); exit (2); }
	fwrite (textbuf, 1, textlen, f);
	fclose (f);
}

static void setup_strings (void)
{
	static const char *words[] = {"", "Village", "Castle of the Dark", "x"};
	size_t off = 0, w;
	int i;
	for (w = 0; w < sizeof words / sizeof words[0]; w++) {
		size_t l = strlen (words[w]) + 1;
		memcpy (prog_string_heap + off, words[w], l);
		off += l;
	}
	pr_strings = prog_string_heap;
	pr_stringssize = (int)off;
	for (i = 0; i < 16; i++) {
		snprintf (host_string_store[i], sizeof host_string_store[i], "level string %d", i);
		host_strings[i] = host_string_store[i];
	}
}

static void setup_case (uint64_t seed, int kind)
{
	int maxcl;
	rng_seed (seed * 0x9E3779B97F4A7C15ULL + (uint64_t)kind + 1);
	case_kind = kind;
	case_ret = 0;
	case_ret_valid = 0;
	case_prefix[0] = 0;
	case_vbuf = (int)(rng_next () % 3);
	memset (sv.skybox, 0, sizeof sv.skybox);
	if (rng_pct (30))
		snprintf (sv.skybox, sizeof sv.skybox, "stale");
	memset (com_token, 0, sizeof com_token);
	svs.serverflags = rng_int (-3, 15);
	sv.edicts = edicts;
	switch (kind) {
	case K_SKY:
		gen_lump ();
		break;
	case K_PAGE:
		maxcl = rng_pct (8) ? rng_int (-1, 0) : rng_int (1, MAX_CLIENTS + 1);
		gen_pages (maxcl);
		svs.maxclients = maxcl;
		if (rng_pct (5))
			sv.ex_inventory_pages = NULL;
		case_clientnum = rng_pct (10) ? rng_int (-2, 20) : rng_int (0, maxcl > 0 ? maxcl - 1 : 0);
		break;
	case K_UPDATE: {
		int owner;
		maxcl = rng_int (1, MAX_CLIENTS);
		gen_pages (maxcl);
		svs.maxclients = rng_pct (90) ? maxcl : rng_int (0, maxcl);
		if (rng_pct (3))
			sv.ex_inventory_pages = NULL;
		owner = rng_int (0, maxcl - 1);
		if (rng_pct (8)) {
			case_start = -1;
		} else if (rng_pct (55)) {
			/* the chain start a client would have: SV_ClientInventoryPage's
			 * choice, found without claiming */
			int i;
			case_start = 0;
			for (i = 0; i < maxcl * MAX_INVENTORY_EX_PAGES; i++)
				if (page_store[i].id == 0 || page_store[i].client_id == owner) {
					case_start = i;
					break;
				}
		} else {
			case_start = rng_int (0, maxcl * MAX_INVENTORY_EX_PAGES - 1);
		}
		case_inv_id = rng_pct (10) ? 0 : rng_pct (8) ? rng_extreme_int () : rng_int (1, 40);
		if (rng_pct (10) && case_start >= 0)	/* an id already in a slot */
			case_inv_id = page_store[case_start].item_id[rng_int (0, 31)];
		switch (rng_next () % 8) {
		case 0: case_inv_cnt = 0; break;
		case 1: case_inv_cnt = rng_int (-10, -1); break;
		case 2: case_inv_cnt = rng_extreme_int (); break;
		case 3: case_inv_cnt = (int)rng_next (); break;
		default: case_inv_cnt = rng_int (1, 255); break;
		}
		{
			static const int incs[] = {0, 0, 1, 1, 2, -1};
			case_inc = incs[rng_next () % 6];
		}
		break;
	}
	case K_LEVELNAME:
		memset (edicts, 0, sizeof edicts);
		host_string_count = rng_pct (10) ? rng_int (-2, 0) : rng_int (0, 16);
		edicts[0].v.message = rng_pct (50) ? (float)rng_int (-2, host_string_count + 2) : rng_float ();
		edicts[0].v.netname = rng_pct (90) ? rng_int (0, pr_stringssize - 1) : rng_int (-3, pr_stringssize + 3);
		break;
	case K_SPAWNPARMS:
		gen_clients (0);
		svs.maxclients = rng_pct (10) ? rng_int (-3, 0) : rng_int (1, MAX_CLIENTS);
		progs_serverflags = rng_float ();
		sv_globals.serverflags = &progs_serverflags;
		break;
	case K_WRITEPAGE:
		maxcl = rng_int (1, MAX_CLIENTS);
		gen_pages (maxcl);
		gen_clients (0);
		case_start = rng_int (0, maxcl * MAX_INVENTORY_EX_PAGES - 1);
		case_client_id = rng_pct (20) ? rng_extreme_int () : rng_int (-1, MAX_CLIENTS);
		if (rng_pct (30))
			snprintf (case_prefix, sizeof case_prefix, "prefix %d\n", rng_int (0, 99));
		break;
	case K_SAVEPAGES:
	case K_ROUNDTRIP:
		maxcl = rng_int (1, MAX_CLIENTS);
		gen_pages (maxcl);
		gen_clients (1);
		svs.maxclients = rng_pct (10) ? rng_int (-2, 0) : rng_pct (10) ? MAX_CLIENTS + 1 : maxcl;
		if (kind == K_ROUNDTRIP && svs.maxclients < 1)
			svs.maxclients = maxcl;
		if (kind == K_ROUNDTRIP) {
			/* a start page must be a page the load can rebuild: inside
			 * the array of this maxclients */
			int i;
			for (i = 0; i <= MAX_CLIENTS; i++)
				if (clients[i].ex_inventory)
					clients[i].ex_inventory = &page_store[rng_int (0, svs.maxclients * MAX_INVENTORY_EX_PAGES - 1)];
		}
		if (rng_pct (30))
			snprintf (case_prefix, sizeof case_prefix, "%d\nskill\n", rng_int (0, 9));
		break;
	case K_LOAD:
		maxcl = rng_int (1, MAX_CLIENTS);
		gen_pages (maxcl);
		svs.maxclients = maxcl;
		if (rng_pct (50)) {	/* the fresh array SV_SpawnServer leaves */
			memset (page_store, 0, sizeof page_store);
			sv.next_page_id = 0;
		}
		case_preread = rng_pct (20) ? rng_int (1, 5) : 0;
		case_unget = case_preread && rng_pct (40) ? (rng_pct (50) ? 'P' : ' ') : -1;
		gen_load_text (maxcl);
		write_input ();
		break;
	}
}

/* ---- the child: run one side ---- */
static void run_child (void)
{
	struct sigaction sa;
	memset (&sa, 0, sizeof sa);
	sa.sa_handler = crash_handler;
	sigaction (SIGSEGV, &sa, NULL);
	sigaction (SIGBUS, &sa, NULL);
	sigaction (SIGFPE, &sa, NULL);
	sa.sa_handler = abort_handler;
	sigaction (SIGABRT, &sa, NULL);
	sa.sa_handler = alarm_handler;
	sigaction (SIGALRM, &sa, NULL);
	calllog_len = 0;
	c_item_fgetcs = 0;
	case_fp = NULL;
	/* every kind, not only INV_UpdateExItem's: a port that loops where the
	 * C returns must end as a spin the parent sees, not hang the run */
	{
		struct itimerval it;
		memset (&it, 0, sizeof it);
		it.it_value.tv_usec = SPIN_MS * 1000;
		setitimer (ITIMER_REAL, &it, NULL);
	}

	switch (case_kind) {
	case K_SKY:
		if (harness_side == 0)
			c_SV_ParseWorldspawnSky ();
		else
			SV_ParseWorldspawnSky ();
		break;
	case K_PAGE: {
		ex_inventory_page_t *p = harness_side == 0 ?
			c_SV_ClientInventoryPage (case_clientnum) :
			SV_ClientInventoryPage (case_clientnum);
		case_ret = p ? (intptr_t)(p - page_store) : -1;
		case_ret_valid = 1;
		break;
	}
	case K_UPDATE: {
		ex_inventory_page_t *start = case_start < 0 ? NULL : &page_store[case_start];
		case_ret = harness_side == 0 ?
			c_INV_UpdateExItem (start, case_inv_id, case_inv_cnt, case_inc) :
			INV_UpdateExItem (start, case_inv_id, case_inv_cnt, case_inc);
		case_ret_valid = 1;
		break;
	}
	case K_LEVELNAME:
		case_ret = (intptr_t)(harness_side == 0 ? c_SV_GetLevelname () : SV_GetLevelname ());
		case_ret_valid = 1;
		break;
	case K_SPAWNPARMS:
		if (harness_side == 0)
			c_SV_SaveSpawnparms ();
		else
			SV_SaveSpawnparms ();
		break;
	case K_WRITEPAGE:
	case K_SAVEPAGES:
	case K_ROUNDTRIP:
		case_fp = fopen (save_path, case_kind == K_ROUNDTRIP ? "w+" : "w");
		if (!case_fp)
			_exit (3);
		setvbuf (case_fp, NULL, case_vbuf == 0 ? _IOFBF : case_vbuf == 1 ? _IOLBF : _IONBF, 0);
		fputs (case_prefix, case_fp);
		if (case_kind == K_WRITEPAGE) {
			if (harness_side == 0)
				c_INV_WritePage (case_fp, &page_store[case_start], case_client_id);
			else
				INV_WritePage (case_fp, &page_store[case_start], case_client_id);
		} else if (harness_side == 0) {
			c_INV_SavePages (case_fp);
		} else {
			INV_SavePages (case_fp);
		}
		if (case_kind == K_ROUNDTRIP) {
			/* read it back into a fresh array, as a load would */
			fflush (case_fp);
			fseek (case_fp, (long)strlen (case_prefix), SEEK_SET);
			memset (page_store, 0, sizeof page_store);
			sv.next_page_id = 0;
			if (harness_side == 0)
				c_SV_LoadInventory (case_fp);
			else
				SV_LoadInventory (case_fp);
			fseek (case_fp, 0, SEEK_END);
		}
		break;
	case K_LOAD: {
		int k;
		case_fp = fopen (in_path, "r");
		if (!case_fp)
			_exit (3);
		setvbuf (case_fp, NULL, case_vbuf == 0 ? _IOFBF : case_vbuf == 1 ? _IOLBF : _IONBF, 0);
		for (k = 0; k < case_preread; k++)
			fgetc (case_fp);
		if (case_unget >= 0)
			ungetc (case_unget, case_fp);
		if (harness_side == 0)
			c_SV_LoadInventory (case_fp);
		else
			SV_LoadInventory (case_fp);
		break;
	}
	}
	finish_child (0);
}

/* ---- the parent: fork both sides, compare ---- */
static const char *tmpdir_g;
static long	cases_run, cases_pass;
static long	outcomes[NKINDS][5];	/* returned, Host_Error, UB stop, spin, crash */

static int outcome_of (int status)
{
	int code;
	if (!WIFEXITED (status))
		return 4;
	code = WEXITSTATUS (status);
	switch (code) {
	case 0: return 0;
	case 42: return 1;
	case 43: case 44: return 2;
	case 45: return 3;
	default: return 4;
	}
}

static int read_file (const char *path, char **buf, size_t *cap, long *len)
{
	FILE *f = fopen (path, "rb");
	if (!f)
		return -1;
	fseek (f, 0, SEEK_END);
	*len = ftell (f);
	fseek (f, 0, SEEK_SET);
	if ((size_t)*len + 1 > *cap) {
		*cap = (size_t)*len + 1024;
		*buf = realloc (*buf, *cap);
	}
	if (fread (*buf, 1, (size_t)*len, f) != (size_t)*len) {
		fclose (f);
		return -1;
	}
	fclose (f);
	return 0;
}

static pid_t fork_side (int side)
{
	pid_t pid;
	fflush (stdout);
	harness_side = side;
	snprintf (out_path, sizeof out_path, "%s/%s.out", tmpdir_g, side ? "rust" : "c");
	snprintf (save_path, sizeof save_path, "%s/%s.sav", tmpdir_g, side ? "rust" : "c");
	unlink (out_path);
	pid = fork ();
	if (pid == 0)
		run_child ();
	return pid;
}

/* run the prepared case (state already built in this process) */
static int run_prepared (const char *label)
{
	static char *bufc, *bufr;
	static size_t capc, capr;
	long szc, szr;
	int sc, sr, oc, orr, ec, er;
	pid_t pid;

	pid = fork_side (0);
	if (pid < 0 || waitpid (pid, &sc, 0) < 0) {
		printf ("case %ld (%s): fork/wait failed: %s\n", cases_run, label, strerror (errno));
		return 1;
	}
	pid = fork_side (1);
	if (pid < 0 || waitpid (pid, &sr, 0) < 0) {
		printf ("case %ld (%s): fork/wait failed (rust): %s\n", cases_run, label, strerror (errno));
		return 1;
	}
	cases_run++;
	oc = outcome_of (sc);
	orr = outcome_of (sr);
	ec = WIFEXITED (sc) ? WEXITSTATUS (sc) : -1;
	er = WIFEXITED (sr) ? WEXITSTATUS (sr) : -1;
	/* 43 is the C's undefined-behaviour stop and 44 the Rust's abort; each
	 * belongs to its own side only */
	if (oc != orr || (oc == 2 && (ec != 43 || er != 44)) || (oc != 2 && ec != er) || oc == 4) {
		printf ("MISMATCH case %ld (%s, kind %s): C exit %d/status %d, Rust exit %d/status %d\n",
			cases_run - 1, label, kind_names[case_kind], ec, sc, er, sr);
		return 1;
	}
	if (read_file (out_path, &bufr, &capr, &szr) ||
	    (snprintf (out_path, sizeof out_path, "%s/c.out", tmpdir_g),
	     read_file (out_path, &bufc, &capc, &szc))) {
		printf ("case %ld (%s): a side wrote no output\n", cases_run - 1, label);
		return 1;
	}
	if (szc != szr || memcmp (bufc, bufr, (size_t)szc) != 0) {
		long i, n = szc < szr ? szc : szr;
		for (i = 0; i < n && bufc[i] == bufr[i]; i++)
			;
		printf ("MISMATCH case %ld (%s, kind %s): %ld vs %ld bytes, first difference at %ld\n",
			cases_run - 1, label, kind_names[case_kind], szc, szr, i);
		if (getenv ("HARNESS_KEEP")) {
			char kp[400];
			FILE *f;
			snprintf (kp, sizeof kp, "%s/mismatch_%ld_c.bin", tmpdir_g, cases_run - 1);
			if ((f = fopen (kp, "wb"))) { fwrite (bufc, 1, (size_t)szc, f); fclose (f); }
			snprintf (kp, sizeof kp, "%s/mismatch_%ld_rust.bin", tmpdir_g, cases_run - 1);
			if ((f = fopen (kp, "wb"))) { fwrite (bufr, 1, (size_t)szr, f); fclose (f); }
			printf ("  kept in %s\n", tmpdir_g);
		}
		return 1;
	}
	outcomes[case_kind][oc]++;
	cases_pass++;
	return 0;
}

static int run_seeded (uint64_t seed, int kind)
{
	char label[64];
	setup_case (seed, kind);
	snprintf (label, sizeof label, "seed %llu", (unsigned long long)seed);
	return run_prepared (label);
}

/* ---- directed cases ---- */

static void fill_page (int idx, int id, int client_id, int nitems, int base_item)
{
	ex_inventory_page_t *p = &page_store[idx];
	int k;
	memset (p, 0, sizeof *p);
	p->id = id;
	p->client_id = client_id;
	for (k = 0; k < nitems && k < MAX_INVENTORY_EX; k++) {
		p->item_id[k] = base_item + k;
		p->item_cnt[k] = 1 + k;
	}
}

static int directed_load (const char *text, int maxcl, int fresh, const char *label)
{
	rng_seed (0xD0AD);
	case_kind = K_LOAD;
	case_ret_valid = 0;
	case_ret = 0;
	case_vbuf = 0;
	case_preread = 0;
	case_unget = -1;
	gen_pages (maxcl);
	svs.maxclients = maxcl;
	if (fresh) {
		memset (page_store, 0, sizeof page_store);
		sv.next_page_id = 0;
	}
	textlen = strlen (text);
	memcpy (textbuf, text, textlen + 1);
	write_input ();
	return run_prepared (label);
}

static int directed_sky (const char *text, const char *label)
{
	case_kind = K_SKY;
	case_ret_valid = 0;
	case_ret = 0;
	memset (sv.skybox, 0, sizeof sv.skybox);
	memset (com_token, 0, sizeof com_token);
	sv.worldmodel = &world_model;
	world_model.entities = lump;
	snprintf (lump, sizeof lump, "%s", text);
	return run_prepared (label);
}

static int directed_update (int start, int inv_id, int inv_cnt, int inc, const char *label)
{
	case_kind = K_UPDATE;
	case_ret_valid = 0;
	case_start = start;
	case_inv_id = inv_id;
	case_inv_cnt = inv_cnt;
	case_inc = inc;
	return run_prepared (label);
}

static int directed (void)
{
	int bad = 0, t, i, n;
	char text[8192], key[256];
	size_t len;

	/* ---- SV_LoadInventory: the "Pages:" totals, LF and CRLF ---- */
	{
		static const int totals[] = {-1, 0, 1, MAX_CLIENTS, MAX_CLIENTS + 1};
		static const char *eols[] = {"\n", "\r\n", "\r"};
		size_t e;
		for (t = 0; t < 5; t++)
			for (e = 0; e < 3; e++) {
				len = (size_t)snprintf (text, sizeof text, "Pages: %d%s", totals[t], eols[e]);
				for (i = 0; i < (totals[t] > 0 ? totals[t] : 0); i++)
					len += (size_t)snprintf (text + len, sizeof text - len,
						"Page: %d -1 %d %d %d 9 1%s", i + 1, i, i + 5, i + 1, eols[e]);
				snprintf (text + len, sizeof text - len, "-1%s", eols[e]);
				bad |= directed_load (text, MAX_CLIENTS, 1, "load totals");
				bad |= directed_load (text, 1, 0, "load totals, maxclients 1");
			}
	}
	/* exactly MAX_INVENTORY_EX pairs (defined), then one more (the UB stop) */
	for (n = MAX_INVENTORY_EX - 1; n <= MAX_INVENTORY_EX + 1; n++) {
		len = (size_t)snprintf (text, sizeof text, "Pages: 1\nPage: 3 -1 0");
		for (i = 0; i < n; i++)
			len += (size_t)snprintf (text + len, sizeof text - len, " %d %d", i + 1, 100 + i);
		snprintf (text + len, sizeof text - len, "\n-1\n");
		bad |= directed_load (text, 1, 1, "load 31/32/33 pairs");
	}
	/* EOF in the item loop: the C loops on EOF until it overruns */
	bad |= directed_load ("Pages: 1\nPage: 3 -1 0 5 6", 2, 1, "load EOF mid-line");
	bad |= directed_load ("Pages: 1\nPage: 3 -1 0", 2, 1, "load EOF after header");
	bad |= directed_load ("Pages: 2\nPage: 3 -1 0\n", 2, 1, "load EOF before a page");
	/* the first scans fail: the C's uninitialised ints (read as 0) */
	bad |= directed_load ("Pages: 1\nx\n", 2, 1, "load page scan fails");
	bad |= directed_load ("Pages: 1\nPage: 1 -1 0 x\n", 2, 1, "load item scan fails");
	bad |= directed_load ("Pages: 2\nPage: 1 -1 0 4 4\nPage: x\n", 2, 0, "load stale ints");
	/* trailing space before the newline: the next line is read as items */
	bad |= directed_load ("Pages: 2\nPage: 1 -1 0 4 4 \nPage: 2 -1 1 5 5\n-1\n", 2, 1, "load trailing space");
	/* the search is bounded by maxclients: every page in it taken */
	rng_seed (1);
	for (i = 0; i < 3; i++) {
		gen_pages (2);
		svs.maxclients = 2;
		fill_page (0, 1, 5, 2, 1);
		fill_page (1, 2, 6, 2, 1);
		case_kind = K_LOAD;
		case_ret_valid = 0;
		case_vbuf = i;
		case_preread = 0;
		case_unget = -1;
		snprintf (textbuf, sizeof textbuf, "Pages: 1\nPage: %d -1 %d 1 1\n", 7 + i, i == 2 ? 6 : 9);
		textlen = strlen (textbuf);
		write_input ();
		bad |= run_prepared ("load search exhausted");
	}
	/* next_page_id: lower, equal, higher */
	for (i = 0; i < 3; i++) {
		char tx[64];
		snprintf (tx, sizeof tx, "Pages: 1\nPage: %d -1 0\n", 9 + i);
		rng_seed (2);
		gen_pages (1);
		svs.maxclients = 1;
		memset (page_store, 0, sizeof page_store);
		sv.next_page_id = 10;
		case_kind = K_LOAD;
		case_ret_valid = 0;
		case_preread = 0;
		case_unget = -1;
		textlen = strlen (tx);
		memcpy (textbuf, tx, textlen + 1);
		write_input ();
		bad |= run_prepared ("load next_page_id");
	}

	/* ---- SV_ParseWorldspawnSky: the key edges ---- */
	{
		static const char *texts[] = {
			"{\n\"sky\" \"one\"\n}",
			"{\n\"_sky\" \"one\"\n}",
			"{\n\"sky   \" \"one\"\n}",
			"{\n\"_sky \" \"one\"\n}",
			"{\n\"skyname\" \"hl\"\n\"qlsky\" \"ql\"\n}",
			"{\n\"sky\" \"a\" \"sky\" \"b\"\n}",
			"{\n\"sky\" \"\"\n}",
			"{\n\"sky\"\n}",
			"{\n\"sky\" \"one\"",
			"{\n}\n{\"sky\" \"late\"}",
			"\"{\" \"sky\" \"q\" }",
			"{x \"sky\" \"q\" }",
			"x \"sky\" \"q\" }",
			"",
			"   ",
			"// c\n{ /* c */ \"sky\" /* */ \"cm\" }",
			"{ \"\" \"v\" \"sky\" \"w\" }",
			"{ \"_\" \"v\" }",
			"{ \" sky\" \"lead\" }",
			"{ \"sky\t\" \"tab\" }",
			"{ \"}\" \"sky\" \"x\" }",
		};
		size_t k;
		for (k = 0; k < sizeof texts / sizeof texts[0]; k++)
			bad |= directed_sky (texts[k], "sky text");
		/* keys of 126..129 bytes: key[128] holds 127, and the trim
		 * runs on what survived the cut */
		for (n = 126; n <= 129; n++) {
			long_key (key, (size_t)n, "sky", ' ', "x");
			snprintf (text, sizeof text, "{ \"%s\" \"cut\" }", key);
			bad |= directed_sky (text, "sky 126..129, x at the end");
			long_key (key, (size_t)n, "sky", ' ', "");
			snprintf (text, sizeof text, "{ \"%s\" \"cut\" }", key);
			bad |= directed_sky (text, "sky 126..129, spaces");
			long_key (key, (size_t)n, "_sky", ' ', "x");
			snprintf (text, sizeof text, "{ \"%s\" \"cut\" }", key);
			bad |= directed_sky (text, "_sky 126..129");
		}
		/* values of 62..65 bytes: sv.skybox holds 63 */
		for (n = 62; n <= 65; n++) {
			memset (key, 'v', (size_t)n);
			key[n] = 0;
			snprintf (text, sizeof text, "{ \"sky\" \"%s\" }", key);
			bad |= directed_sky (text, "sky value 62..65");
		}
		case_kind = K_SKY;
		sv.worldmodel = NULL;
		bad |= run_prepared ("sky, no world");
		sv.worldmodel = &world_model;
		world_model.entities = NULL;
		bad |= run_prepared ("sky, no entities");
	}

	/* ---- SV_GetLevelname: message 0, == host_string_count, past it ---- */
	{
		static const float msgs[] = {-1.0f, 0.0f, 0.5f, 1.0f, 2.99f, 3.0f, 4.0f, NAN, INFINITY, 2147483648.0f};
		size_t k;
		for (k = 0; k < sizeof msgs / sizeof msgs[0]; k++)
			for (t = 0; t <= 3; t += 3) {
				case_kind = K_LEVELNAME;
				memset (edicts, 0, sizeof edicts);
				sv.edicts = edicts;
				host_string_count = t;
				edicts[0].v.message = msgs[k];
				edicts[0].v.netname = 1;
				bad |= run_prepared ("levelname");
			}
		/* past int range, (int) is INT_MIN on x86: with Rust's
		 * saturating `as` it would be INT_MAX, which only a
		 * host_string_count of INT_MAX can tell apart */
		for (k = 0; k < 2; k++) {
			case_kind = K_LEVELNAME;
			memset (edicts, 0, sizeof edicts);
			sv.edicts = edicts;
			host_string_count = INT_MAX;
			edicts[0].v.message = k ? 3e9f : INFINITY;
			edicts[0].v.netname = 1;
			bad |= run_prepared ("levelname, count INT_MAX");
		}
	}

	/* ---- SV_SaveSpawnparms ---- */
	{
		static const float flags[] = {0.0f, 7.0f, 7.9f, -1.5f, NAN, 3e9f, -3e9f};
		static const int maxcls[] = {-1, 0, 1, MAX_CLIENTS};
		size_t k, m;
		for (k = 0; k < sizeof flags / sizeof flags[0]; k++)
			for (m = 0; m < 4; m++) {
				rng_seed (k * 4 + m + 1);
				case_kind = K_SPAWNPARMS;
				gen_clients (0);
				svs.maxclients = maxcls[m];
				progs_serverflags = flags[k];
				sv_globals.serverflags = &progs_serverflags;
				bad |= run_prepared ("spawnparms");
			}
	}

	/* ---- SV_ClientInventoryPage: exhaustion, claim, next_page_id wrap ---- */
	for (i = 0; i < 4; i++) {
		rng_seed (0x9A9E + (uint64_t)i);
		gen_pages (2);
		svs.maxclients = 2;
		for (n = 0; n < 2 * MAX_INVENTORY_EX_PAGES; n++)
			fill_page (n, n + 1, 9, 0, 0);	/* every page someone else's */
		if (i == 1)
			page_store[2 * MAX_INVENTORY_EX_PAGES - 1].id = 0;	/* the last one free */
		if (i == 2)
			page_store[5].client_id = 1;	/* one of ours, mid-array */
		sv.next_page_id = i == 3 ? INT_MAX : 40;
		if (i == 3)
			page_store[3].id = 0;
		case_kind = K_PAGE;
		case_clientnum = 1;
		bad |= run_prepared ("page exhaustion");
	}

	/* ---- INV_UpdateExItem ---- */
	/* a full start page, the chain extended from the next free page */
	for (i = 0; i < 4; i++) {
		rng_seed (0x0FF + (uint64_t)i);
		gen_pages (2);
		svs.maxclients = 2;
		for (n = 0; n < 2 * MAX_INVENTORY_EX_PAGES; n++)
			fill_page (n, 0, 0, 0, 0);
		fill_page (0, 1, 1, 0, 0);		/* free-or-ours comes first: not ours */
		page_store[0].client_id = 0;
		fill_page (3, 2, 1, MAX_INVENTORY_EX, 50);	/* client 1's, full */
		page_store[1].id = 0;			/* page 1 free: before the start */
		if (i == 1) {				/* no free page: exhaustion */
			for (n = 1; n < 2 * MAX_INVENTORY_EX_PAGES; n++)
				if (n != 3)
					fill_page (n, 10 + n, 0, 0, 0);
		}
		if (i == 2) {				/* a full two-page chain */
			fill_page (4, 4, 1, MAX_INVENTORY_EX, 90);
			page_store[3].next = &page_store[4];
		}
		bad |= directed_update (3, i == 3 ? 55 : 7, 3, i & 1, "update full page");
	}
	/* a chain whose last page is someone else's: the extension is
	 * claimed for the start page's owner (1), not the last page's (2) --
	 * client 2 owns page 1, before the free page 2 */
	for (i = 0; i < 2; i++) {
		rng_seed (0xC4A1 + (uint64_t)i);
		gen_pages (2);
		svs.maxclients = 2;
		for (n = 0; n < 2 * MAX_INVENTORY_EX_PAGES; n++)
			fill_page (n, 0, 0, 0, 0);
		fill_page (0, 1, 0, 0, 0);
		fill_page (1, 2, 2, i ? 3 : 0, 70);
		fill_page (3, 3, 1, MAX_INVENTORY_EX, 50);
		fill_page (4, 4, 2, MAX_INVENTORY_EX, 90);
		page_store[3].next = &page_store[4];
		bad |= directed_update (3, 500, 2, i, "update mixed-owner chain");
	}
	/* the C's spin: the extension finds the start page itself */
	for (i = 0; i < 3; i++) {
		rng_seed (0x5B1 + (uint64_t)i);
		gen_pages (1);
		svs.maxclients = 1;
		for (n = 0; n < MAX_INVENTORY_EX_PAGES; n++)
			fill_page (n, n + 1, 3, 0, 0);
		fill_page (0, 1, 0, MAX_INVENTORY_EX, 1 + i);	/* client 0, full */
		bad |= directed_update (0, 500, 1 + i, i == 1, "update self-link spin");
	}
	/* slot 31, inv_cnt 0/negative, inc on/off, overflow */
	{
		static const int cnts[] = {0, -1, 1, INT_MAX, INT_MIN};
		size_t k;
		for (k = 0; k < 5; k++)
			for (t = 0; t < 2; t++) {
				rng_seed (0x31 + k);
				gen_pages (1);
				svs.maxclients = 1;
				fill_page (0, 1, 0, MAX_INVENTORY_EX - 1, 1);
				page_store[0].item_cnt[5] = INT_MAX;
				/* new item into slot 31, then an existing one */
				bad |= directed_update (0, 777, cnts[k], t, "update slot 31");
				bad |= directed_update (0, 6, cnts[k], t, "update overflow");
				bad |= directed_update (0, 0, cnts[k], t, "update id 0");
				bad |= directed_update (-1, 6, cnts[k], t, "update NULL start");
			}
	}
	return bad;
}

/* the Rust's abort where the C dereferences a NULL page array: not compared
 * with the C (whose behaviour is a NULL dereference); checked on its own */
static int rust_null_pages (void)
{
	pid_t pid;
	int status;
	case_kind = K_LOAD;
	case_preread = 0;
	case_unget = -1;
	svs.maxclients = 1;
	sv.ex_inventory_pages = NULL;
	snprintf (textbuf, sizeof textbuf, "Pages: 1\nPage: 1 -1 0\n");
	textlen = strlen (textbuf);
	write_input ();
	pid = fork_side (1);
	if (pid < 0 || waitpid (pid, &status, 0) < 0)
		return 1;
	if (!WIFEXITED (status) || WEXITSTATUS (status) != 44) {
		printf ("FAIL: Rust SV_LoadInventory with no page array did not abort (status %d)\n", status);
		return 1;
	}
	printf ("null pages: the Rust aborts where the C dereferences NULL\n");
	return 0;
}

int main (int argc, char **argv)
{
	char tmpdir[] = "/tmp/inv_harness_XXXXXX";
	long i, target;
	int bad = 0, k, fail_fast;

	setvbuf (stdout, NULL, _IOLBF, 0);
	if (!mkdtemp (tmpdir)) { perror ("mkdtemp"); return 2; }
	tmpdir_g = tmpdir;
	snprintf (in_path, sizeof in_path, "%s/in.sav", tmpdir);
	if (check_facts ())
		return 2;
	setup_strings ();

	target = argc > 1 ? atol (argv[1]) : 36000;
	printf ("sv_main_inv differential harness\n");

	/* HARNESS_FAIL_FAST (the mutation runs): stop at the first mismatch.
	 * The directed cases go first because they sit on the boundaries. */
	fail_fast = getenv ("HARNESS_FAIL_FAST") != NULL;
	bad |= directed ();
	bad |= rust_null_pages ();
	for (i = 0; i < target && !(bad && fail_fast); i++)
		if (run_seeded (0x1E1E0000ULL + (uint64_t)i, (int)(i % NKINDS)))
			bad = 1;

	printf ("outcomes: kind        returned host_error ub_stop  spin\n");
	for (k = 0; k < NKINDS; k++)
		printf ("          %-11s %8ld %10ld %7ld %5ld\n", kind_names[k],
			outcomes[k][0], outcomes[k][1], outcomes[k][2], outcomes[k][3]);
	printf ("sv_main_inv differential harness (%s): %s (%ld cases, %ld pass)\n",
		"h2", bad ? "FAIL" : "PASS", cases_run, cases_pass);
	if (!getenv ("HARNESS_KEEP_TMP")) {
		char cmd[400];
		snprintf (cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
		if (system (cmd) != 0)
			fprintf (stderr, "warning: could not remove %s\n", tmpdir);
	}
	return bad ? 1 : 0;
}
