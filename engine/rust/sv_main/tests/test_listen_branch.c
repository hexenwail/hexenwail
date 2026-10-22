/* Focused test for SV_SendReconnect's listen-server arm
 * (engine/rust/src/sv_main_connect.rs): the differential harness builds
 * its fixture -DSERVERONLY, so the arm is dead there by construction.
 * This links the Rust archive against the real sv_main_target.o and
 * stubs the rest, then checks both sides of the `SvMain_IsDedicated()`
 * gate: dedicated sends only the stufftext; a listen server also runs
 * Cmd_ExecuteString("reconnect\n") with the live cmd_source.
 *
 * Build (from the worktree root):
 *   cc -I engine/hexen2 -I engine/h2shared -I common -I engine/rust \
 *      -DSERVERONLY -c engine/rust/sv_main/tests/test_listen_branch.c \
 *      -o /tmp/lb/test_listen_branch.o
 *   cc -I ... -DSERVERONLY -c engine/rust/sv_main_target.c -o /tmp/lb/sv_main_target.o
 *   cc /tmp/lb/test_listen_branch.o /tmp/lb/sv_main_target.o \
 *      engine/rust/target/release/libengine_rs.a -o /tmp/lb/test_listen_branch -lm
 *   /tmp/lb/test_listen_branch
 * (libengine_rs.a built with --features sv_main_connect)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "quakedef.h"

/* ---- engine storage sv_main_target.o needs ---- */
server_t		sv;
server_static_t		svs;
int			sv_protocol;
cvar_t			coop, deathmatch, sv_netsort;
cvar_t			sv_update_player, sv_update_monsters;
cvar_t			sv_update_missiles, sv_update_misc;
int			sv_kingofhill;
unsigned short		pr_crc;
int			net_activeconnections;
int			pr_edict_size;
qboolean		isDedicated;
cmd_source_t		cmd_source;
/* slice C's accessors in sv_main_target.c, linked whole; never read here */
client_t		*host_client;
unsigned int		info_mask, info_mask2;
qboolean		sv_datagram_dropped;
cvar_t			sv_maxdatagram;
double			realtime;

/* ---- recorders ---- */
static int	ces_calls;
static char	ces_text[64];
static int	ces_src = -1;
void Cmd_ExecuteString (const char *text, cmd_source_t src)
{
	ces_calls++;
	snprintf (ces_text, sizeof ces_text, "%s", text);
	ces_src = (int)src;
}

static int	sta_calls;
static double	sta_blocktime;
int NET_SendToAll (sizebuf_t *msg, double blocktime)
{
	(void)msg;
	sta_calls++;
	sta_blocktime = blocktime;
	return 0;
}

void SV_SendReconnect (void);	/* the Rust port */

/* ---- link scaffolding: the archive's other CGUs reference these ---- */
#include <stdarg.h>
#include <stdlib.h>
sizebuf_t	net_message;
void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	(void)flags; (void)fmt;
}
void CON_DPrintf (unsigned int flags, const char *fmt, ...)
{
	(void)flags; (void)fmt;
}
void Sys_Error (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	vfprintf (stderr, fmt, ap);
	va_end (ap);
	fputc ('\n', stderr);
	abort ();
}
void Host_Error (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	vfprintf (stderr, fmt, ap);
	va_end (ap);
	fputc ('\n', stderr);
	abort ();
}
/* the rest of sv_main_connect.rs's externs: never called by this test */
struct qsocket_s *NET_CheckNewConnections (void) { return NULL; }
const char *NET_QSocketGetAddressString (const struct qsocket_s *sock)
{
	(void)sock;
	return "testaddr";
}
ex_inventory_page_t *SV_ClientInventoryPage (int clientnum)
{
	(void)clientnum;
	return NULL;
}
eval_t *GetEdictFieldValue (edict_t *ed, const char *field)
{
	(void)ed; (void)field;
	return NULL;
}
const char *PR_GetString (int num)
{
	(void)num;
	return "";
}
qboolean NET_IsLocalConnection (struct qsocket_s *sock)
{
	(void)sock;
	return false;
}
int NET_MaxUnreliableMessage (struct qsocket_s *sock)
{
	(void)sock;
	return 0;
}
const char *SV_GetLevelname (void) { return "testmap"; }
void *Hunk_AllocName (int size, const char *name)
{
	(void)size; (void)name;
	return NULL;
}
#include <strings.h>
int q_strcasecmp (const char *s1, const char *s2)
{
	return strcasecmp (s1, s2);
}
int NUM_FOR_EDICT (edict_t *e)
{
	(void)e;
	return 0;
}
/* world_hull's facts: never read on this test's path */
#include "world_target.h"
const world_target_facts_t World_TargetFacts;
struct qmodel_s **World_SvModels (void) { return NULL; }
struct edict_s *World_SvEdicts (void) { return NULL; }

int main (void)
{
	int fails = 0;

	/* dedicated: the stufftext goes out, no local command */
	isDedicated = 1;
	cmd_source = 1;
	SV_SendReconnect ();
	if (sta_calls != 1 || sta_blocktime != 5.0 || ces_calls != 0) {
		printf ("FAIL dedicated: sendtoall=%d (%.1f) cmd=%d\n",
			sta_calls, sta_blocktime, ces_calls);
		fails++;
	}

	/* listen server: the local client is re-armed synchronously,
	 * with the live cmd_source, as the C's src_command global */
	isDedicated = 0;
	cmd_source = 7;
	SV_SendReconnect ();
	if (sta_calls != 2) {
		printf ("FAIL listen: sendtoall=%d\n", sta_calls);
		fails++;
	}
	if (ces_calls != 1 || strcmp (ces_text, "reconnect\n") != 0 ||
	    ces_src != 7) {
		printf ("FAIL listen: cmd=%d text='%s' src=%d\n",
			ces_calls, ces_text, ces_src);
		fails++;
	}

	if (!fails)
		printf ("LISTEN-BRANCH TEST: PASS\n");
	return fails != 0;
}
