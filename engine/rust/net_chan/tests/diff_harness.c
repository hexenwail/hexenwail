// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for the Rust port of engine/hexenworld/shared/net_chan.c.
//
// The C original is compiled with every exported name renamed (c_Netchan_*) and
// linked into this binary beside the Rust module, and the two are driven
// through one table of function pointers.  After every call the observable
// state is recorded into a byte trace -- the whole `netchan_t` field set, the
// bytes of each buffer's live prefix, the `net_drop` the receive path sets, the
// datagram the send path handed to the transport, the cvars the init registers,
// and the diagnostics -- and the two traces are compared byte for byte.
//
// **A peer on the other end, in both directions.**  Two Rust peers agreeing
// with each other would prove nothing about wire compatibility, and a golden
// packet on its own only shows the sender is self-consistent.  So every
// framing case records three things: the bytes the implementation under test
// put on the wire (which must be *identical* in the two arms -- this is what
// makes the traces comparable at all), the state it derived from those bytes,
// and what the *other* implementation derived from the same bytes when they
// were fed to it.  If the two ports disagree about the format, either the
// bytes differ or the other side's derived state does, and the trace shows it.
//
// What is deliberately not a case: `showpackets`/`showdrop` are `static cvars`
// in the C and private statics in the port, so no harness can set them.  The
// four printf format strings are compared against the C source in the gate
// instead, which is the same trade the transport gate makes for its address
// formats.
//
// Each case runs in a child process per implementation: the channel's buffers
// and the engine's clock are process-wide here, so a fresh process is how each
// arm starts where the engine starts, and a crash is one reported case rather
// than a dead gate.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "q_stdinc.h"
#include "arch_def.h"
#include "quakedef.h"
#include "net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

/*----------------------------------------------------------------------------
 * the two implementations
 *--------------------------------------------------------------------------*/

// The C original, renamed so it can share a binary with the Rust module.
#define DECLARE_NETCHAN_IMPL(P) \
	extern void P##Netchan_Init (void); \
	extern void P##Netchan_OutOfBand (const netadr_t *adr, int length, byte *data); \
	extern void P##Netchan_Setup (netchan_t *chan, const netadr_t *adr); \
	extern qboolean P##Netchan_CanPacket (const netchan_t *chan); \
	extern qboolean P##Netchan_CanReliable (const netchan_t *chan); \
	extern void P##Netchan_Transmit (netchan_t *chan, int length, byte *data); \
	extern qboolean P##Netchan_Process (netchan_t *chan); \
	extern int P##net_drop

DECLARE_NETCHAN_IMPL(c_);

// The Rust module exports the client's unambiguous spelling.  Its `net_drop`
// is whatever the per-target shim returns, not a symbol this harness names.
extern void HWNetchan_Init (void);
extern void HWNetchan_OutOfBand (const netadr_t *adr, int length, byte *data);
extern void HWNetchan_Setup (netchan_t *chan, const netadr_t *adr);
extern qboolean HWNetchan_CanPacket (const netchan_t *chan);
extern qboolean HWNetchan_CanReliable (const netchan_t *chan);
extern void HWNetchan_Transmit (netchan_t *chan, int length, byte *data);
extern qboolean HWNetchan_Process (netchan_t *chan);
extern int *NetChan_TargetDrop (void);

typedef struct netchan_impl_s {
	const char *name;
	void (*Init)(void);
	void (*OutOfBand)(const netadr_t *, int, byte *);
	void (*Setup)(netchan_t *, const netadr_t *);
	qboolean (*CanPacket)(const netchan_t *);
	qboolean (*CanReliable)(const netchan_t *);
	void (*Transmit)(netchan_t *, int, byte *);
	qboolean (*Process)(netchan_t *);
	int *drop;
} netchan_impl_t;

enum { IMPL_C = 0, IMPL_RUST = 1, IMPL_COUNT = 2 };

static netchan_impl_t impls[IMPL_COUNT] = {
	{
		"C",
		c_Netchan_Init, c_Netchan_OutOfBand, c_Netchan_Setup,
		c_Netchan_CanPacket, c_Netchan_CanReliable, c_Netchan_Transmit,
		c_Netchan_Process, &c_net_drop,
	},
	{
		"Rust",
		HWNetchan_Init, HWNetchan_OutOfBand, HWNetchan_Setup,
		HWNetchan_CanPacket, HWNetchan_CanReliable, HWNetchan_Transmit,
		HWNetchan_Process, 0 /* filled in by main: NetChan_TargetDrop() */,
	},
};

static const netchan_impl_t *cur;
static const netchan_impl_t *peer;

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
static void rec_double(double v) { rec(&v, sizeof v); }
static void rec_str(const char *s) { rec(s, strlen(s) + 1); }

/*----------------------------------------------------------------------------
 * the engine surface the channel reaches
 *--------------------------------------------------------------------------*/

// net_chan.c reads the message the transport left in net_message, checks the
// sender against net_from, samples realtime for its bandwidth choke, prints
// through Con_Printf, registers two cvars, and sends through the transport.
// All of it is harness-owned here so both arms see the same substrate, which
// is what makes the trace comparison meaningful.

sizebuf_t	net_message;
static byte	net_message_buffer[8192];
netadr_t	net_from;
double		realtime;

// The datagrams the send path hands to the transport, one at a time.
#define SENT_MAX 4096
static byte	sent_buf[SENT_MAX];
static int	sent_len;
static int	sent_count;

// The cvars Netchan_Init registers, in order.
#define CVAR_MAX 8
static char	cvar_names[CVAR_MAX][64];
static int	cvar_count;

// The message reader and the sizebuf are Rust ports now, and they reach
// Sys_Error on their own fatal paths.  Nothing in these cases should reach it,
// so it is a labelled abort rather than a silent one.
void Sys_Error (const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "harness bug: Sys_Error called: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	abort();
}

// SZ_Init only reaches this when it is given no buffer of its own; every call
// here passes one, so a call is a harness bug rather than a fallback.
void *Hunk_AllocName (int size, const char *name)
{
	(void)size;
	(void)name;
	abort();
}

static char	print_buf[512];

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list ap;

	(void)flags;
	va_start(ap, fmt);
	vsnprintf(print_buf, sizeof print_buf, fmt, ap);
	va_end(ap);

	rec_int(0x9911);
	rec_str(print_buf);
}

void Cvar_RegisterVariable (cvar_t *var)
{
	int n = cvar_count;

	if (n < CVAR_MAX) {
		snprintf(cvar_names[n], sizeof cvar_names[n], "%s=%s",
			var->name ? var->name : "(null)",
			var->string ? var->string : "(null)");
		cvar_count++;
	}
}

// The transport's send/receive surface.  The Rust module names the HWNET_*
// spelling; the C original, compiled as hwsv does, names the plain one.  Both
// land in the same recorder so the two arms cannot diverge here.
void HWNET_SendPacket (int length, void *data, const netadr_t *to)
{
	(void)to;
	sent_count++;
	if (length > 0 && length <= SENT_MAX) {
		memcpy(sent_buf, data, (size_t)length);
		sent_len = length;
	} else {
		sent_len = -1;
	}
}

void NET_SendPacket (int length, void *data, const netadr_t *to)
{
	HWNET_SendPacket (length, data, to);
}

int HWNET_CompareAdr (const netadr_t *a, const netadr_t *b)
{
	int same = a->ip[0] == b->ip[0] && a->ip[1] == b->ip[1]
		&& a->ip[2] == b->ip[2] && a->ip[3] == b->ip[3]
		&& a->port == b->port;

	return same;
}

qboolean NET_CompareAdr (const netadr_t *a, const netadr_t *b)
{
	return HWNET_CompareAdr (a, b);
}

const char *HWNET_AdrToString (const netadr_t *a)
{
	static char s[64];

	snprintf(s, sizeof s, "%i.%i.%i.%i:%i",
		a->ip[0], a->ip[1], a->ip[2], a->ip[3], a->port);
	return s;
}

const char *NET_AdrToString (const netadr_t *a)
{
	return HWNET_AdrToString (a);
}

// The transport's storage accessors, which the Rust module reads net_message
// and net_from through rather than naming either target's spelling.
sizebuf_t *NetUDP_TargetMessageBuf (void)
{
	return &net_message;
}

netadr_t *NetUDP_TargetFrom (void)
{
	return &net_from;
}

/*----------------------------------------------------------------------------
 * state recording
 *--------------------------------------------------------------------------*/

static void rec_state (netchan_t *chan)
{
	int i;

	rec_int(chan->fatal_error);
	rec_double((double)chan->last_received);
	rec_double((double)chan->frame_latency);
	rec_double((double)chan->frame_rate);
	rec_int(chan->drop_count);
	rec_int(chan->good_count);
	rec_int(chan->remote_address.ip[0]);
	rec_int(chan->remote_address.ip[1]);
	rec_int(chan->remote_address.ip[2]);
	rec_int(chan->remote_address.ip[3]);
	rec_int(chan->remote_address.port);
	rec_double(chan->cleartime);
	rec_double(chan->rate);
	rec_int(chan->incoming_sequence);
	rec_int(chan->incoming_acknowledged);
	rec_int(chan->incoming_reliable_acknowledged);
	rec_int(chan->incoming_reliable_sequence);
	rec_int(chan->outgoing_sequence);
	rec_int(chan->reliable_sequence);
	rec_int(chan->last_reliable_sequence);
	rec_int(chan->reliable_length);
	rec_int(chan->message.cursize);
	rec(chan->message_buf, (size_t)(chan->message.cursize > 0 ? chan->message.cursize : 0));
	rec(chan->reliable_buf, (size_t)(chan->reliable_length > 0 ? chan->reliable_length : 0));

	for (i = 0; i < MAX_LATENT; ++i) {
		rec_int(chan->outgoing_size[i]);
		rec_double(chan->outgoing_time[i]);
	}

	rec_int(*cur->drop);
	rec_int(*peer->drop);

	// the datagram the send path produced, and what the other side read out
	// of it: bytes first, then the state the peer derived from them
	rec_int(sent_count);
	rec_int(sent_len);
	if (sent_len > 0)
		rec(sent_buf, (size_t)sent_len);

	rec_int(cvar_count);
	for (i = 0; i < cvar_count; ++i)
		rec_str(cvar_names[i]);
}

// The peer's derived state, for the cross-implementation half of a case.  It
// is recorded separately because it is the other implementation's channel, not
// the one under test.
static void rec_peer_state (netchan_t *peerchan)
{
	rec_int(peerchan->incoming_sequence);
	rec_int(peerchan->incoming_acknowledged);
	rec_int(peerchan->incoming_reliable_acknowledged);
	rec_int(peerchan->incoming_reliable_sequence);
	rec_int(peerchan->drop_count);
	rec_int(peerchan->good_count);
	rec_int(peerchan->reliable_length);
	rec_int(peerchan->fatal_error);
	rec_double(peerchan->frame_latency);
	rec_double(peerchan->frame_rate);
	rec_int(*peer->drop);
}

/*----------------------------------------------------------------------------
 * helpers the cases speak in
 *--------------------------------------------------------------------------*/

static netadr_t make_adr (int a, int b, int c, int d, int port)
{
	netadr_t adr;

	adr.ip[0] = (byte)a;
	adr.ip[1] = (byte)b;
	adr.ip[2] = (byte)c;
	adr.ip[3] = (byte)d;
	adr.port = (unsigned short)port;
	adr.pad = 0;
	return adr;
}

// Puts the bytes the implementation under test just sent into net_message,
// with net_from set to the address it considers remote, so the other
// implementation can be asked what it reads out of them.
static void deliver_sent_to_peer (netchan_t *chan)
{
	net_from = chan->remote_address;
	SZ_Init(&net_message, net_message_buffer, sizeof net_message_buffer);
	if (sent_len > 0)
		SZ_Write(&net_message, sent_buf, sent_len);
}

static void op_tx (const char *what, netchan_t *chan, const char *payload)
{
	rec_int(0x7A11);
	rec_str(what);
	sent_count = 0;
	sent_len = 0;
	cur->Transmit(chan, (int)strlen(payload), (byte *)payload);
	rec_state(chan);
}

static void op_rx (const char *what, netchan_t *chan)
{
	qboolean ok;

	rec_int(0x7A22);
	rec_str(what);
	ok = cur->Process(chan);
	rec_int(ok);
	rec_state(chan);
}

// The cross-implementation half: feed the bytes the implementation under test
// just sent to the *other* implementation's channel and record what it derives.
// In the C arm the other side is Rust and in the Rust arm it is the C, so a
// format disagreement shows up as a trace difference on one side or the other.
static void peer_reads_sent (const char *what, netchan_t *peerchan)
{
	qboolean ok;

	rec_int(0x7A33);
	rec_str(what);
	deliver_sent_to_peer(peerchan);
	ok = peer->Process(peerchan);
	rec_int(ok);
	rec_peer_state(peerchan);
}

static void scenario_reset (void)
{
	sent_count = 0;
	sent_len = 0;
	cvar_count = 0;
	print_buf[0] = 0;
	realtime = 100.0;
	net_from = make_adr(0, 0, 0, 0, 0);
	memset(net_message_buffer, 0, sizeof net_message_buffer);
	SZ_Init(&net_message, net_message_buffer, sizeof net_message_buffer);
}

/*----------------------------------------------------------------------------
 * scenarios
 *--------------------------------------------------------------------------*/

// Setup, the bandwidth choke's two boundaries, and the init's cvar
// registrations.
static void scenario_setup (void)
{
	static netchan_t chan;
	netadr_t remote = make_adr(10, 0, 0, 1, 26000);

	rec_int(0x1001);
	cur->Init();
	rec_int(cvar_count);
	{
		int i;
		for (i = 0; i < cvar_count; ++i)
			rec_str(cvar_names[i]);
	}

	memset(&chan, 0, sizeof chan);
	cur->Setup(&chan, &remote);
	rec_state(&chan);

	// rate is 1/2500 s per byte; MAX_BACKUP is 200, so the choke opens when
	// cleartime < realtime + 200*rate
	chan.cleartime = realtime + 200.0 * chan.rate - 0.001;
	rec_int(cur->CanPacket(&chan));
	rec_int(cur->CanReliable(&chan));

	chan.cleartime = realtime + 200.0 * chan.rate + 0.001;
	rec_int(cur->CanPacket(&chan));
	rec_int(cur->CanReliable(&chan));

	rec_state(&chan);
}

// The out-of-band path: a -1 sequence header and the payload after it, with
// the demo-playback guard deciding whether the transport is reached at all.
static void scenario_out_of_band (void)
{
	netadr_t remote = make_adr(10, 0, 0, 2, 26001);
	byte payload[8] = { 'h', 'e', 'l', 'l', 'o', 0, 1, 2 };

	rec_int(0x1002);
	sent_count = 0;
	sent_len = 0;
	cur->OutOfBand(&remote, 8, payload);
	rec_int(sent_count);
	rec_int(sent_len);
	if (sent_len > 0)
		rec(sent_buf, (size_t)sent_len);
}

// The framing: an unreliable message, then a reliable one, then the same
// reliable message again once the peer's ack has cleared the buffer.  The
// packet bytes are the evidence; the peer reading them is the proof.
static void scenario_framing (void)
{
	static netchan_t chan, peerchan;
	netadr_t remote = make_adr(10, 0, 0, 3, 26002);

	rec_int(0x1003);
	memset(&chan, 0, sizeof chan);
	memset(&peerchan, 0, sizeof peerchan);
	cur->Setup(&chan, &remote);
	peer->Setup(&peerchan, &remote);

	op_tx("unreliable only", &chan, "first");
	peer_reads_sent("peer reads the unreliable packet", &peerchan);

	// a reliable payload: written into the channel's message buffer, then
	// flushed by the next Transmit
	SZ_Write(&chan.message, "reliable!", 9);
	op_tx("reliable first send", &chan, "");
	peer_reads_sent("peer reads the reliable packet", &peerchan);

	// the peer's ack travels back in its own next packet: the peer sends it,
	// the implementation under test reads it
	rec_int(0x1004);
	peer->Transmit(&peerchan, 0, (byte *)"");
	peer_reads_sent("peer reads its own ack packet", &peerchan);
	deliver_sent_to_peer(&peerchan);
	rec_int(cur->Process(&chan));
	rec_state(&chan);

	// once acknowledged, the reliable buffer is free and a new message can be
	// staged and sent
	SZ_Write(&chan.message, "second", 6);
	op_tx("reliable after ack", &chan, "unreliable too");
	peer_reads_sent("peer reads the second reliable packet", &peerchan);
}

// Loss, duplication and reorder, against the sequence the peer's own packets
// established: the receive path must ignore stale and duplicate packets,
// count the gap when one is missing, and never abort.
static void scenario_loss_reorder (void)
{
	static netchan_t chan;
	netadr_t remote = make_adr(10, 0, 0, 4, 26003);
	byte packets[4][64];
	int lens[4];
	int i;

	rec_int(0x1005);
	memset(&chan, 0, sizeof chan);
	cur->Setup(&chan, &remote);

	// build four packets by transmitting four times, capturing each
	for (i = 0; i < 4; ++i) {
		cur->Transmit(&chan, 0, (byte *)"");
		lens[i] = sent_len;
		if (sent_len > 0)
			memcpy(packets[i], sent_buf, (size_t)sent_len);
	}

	// deliver them out of order: 1, 2, then 3 and finally the stale 2 again
	for (i = 0; i < 4; ++i) {
		int k = (i == 2) ? 3 : (i == 3 ? 1 : i);
		net_from = remote;
		SZ_Init(&net_message, net_message_buffer, sizeof net_message_buffer);
		SZ_Write(&net_message, packets[k], lens[k]);
		rec_int(0x1006);
		rec_int(k);
		rec_int(cur->Process(&chan));
		rec_state(&chan);
	}

	// skip one, so the gap is counted
	i = 6;
	net_from = remote;
	SZ_Init(&net_message, net_message_buffer, sizeof net_message_buffer);
	{
		byte pkt[8];
		unsigned int w1 = (unsigned int)i;
		unsigned int w2 = (unsigned int)(chan.incoming_sequence | (chan.incoming_reliable_sequence << 31));
		pkt[0] = (byte)w1; pkt[1] = (byte)(w1 >> 8); pkt[2] = (byte)(w1 >> 16); pkt[3] = (byte)(w1 >> 24);
		pkt[4] = (byte)w2; pkt[5] = (byte)(w2 >> 8); pkt[6] = (byte)(w2 >> 16); pkt[7] = (byte)(w2 >> 24);
		SZ_Write(&net_message, pkt, 8);
	}
	rec_int(0x1007);
	rec_int(cur->Process(&chan));
	rec_state(&chan);
}

// The overflow path: the message buffer is filled past its limit, which makes
// Transmit set fatal_error and print instead of sending.  The C does not
// abort here, and neither may the port.
static void scenario_overflow (void)
{
	static netchan_t chan;
	netadr_t remote = make_adr(10, 0, 0, 5, 26004);
	static byte big[7000];

	rec_int(0x1008);
	memset(&chan, 0, sizeof chan);
	cur->Setup(&chan, &remote);

	// SZ_Write into a buffer with allowoverflow set raises overflowed rather
	// than aborting, which is exactly what the C relies on here
	// Two writes that each fit but together do not: that is the path
	// allowoverflow exists for -- and the single-write guard (a chunk larger
	// than the whole buffer) aborts in the C too, so it is not this case.
	memset(big, 'x', sizeof big);
	chan.message.allowoverflow = true;
	SZ_Write(&chan.message, big, (int)sizeof big);
	rec_int(chan.message.overflowed);
	SZ_Write(&chan.message, big, (int)sizeof big);
	rec_int(chan.message.overflowed);

	sent_count = 0;
	sent_len = 0;
	cur->Transmit(&chan, 0, (byte *)"");
	rec_int(sent_count);
	rec_state(&chan);
}

// The bandwidth choke as time advances: consecutive sends accumulate
// cleartime, and CanPacket closes and reopens as realtime catches up.
static void scenario_rate (void)
{
	static netchan_t chan;
	netadr_t remote = make_adr(10, 0, 0, 6, 26005);
	int i;

	rec_int(0x1009);
	memset(&chan, 0, sizeof chan);
	cur->Setup(&chan, &remote);

	for (i = 0; i < 4; ++i) {
		rec_int(0x100A);
		rec_int(i);
		rec_int(cur->CanPacket(&chan));
		cur->Transmit(&chan, 0, (byte *)"");
		rec_state(&chan);
		realtime += 0.001;
	}
	realtime += 1000.0;
	rec_int(cur->CanPacket(&chan));
	rec_int(cur->CanReliable(&chan));
	rec_state(&chan);
}

/*----------------------------------------------------------------------------
 * case runner
 *--------------------------------------------------------------------------*/

struct child_result {
	size_t len;
	int checks;
	int failures;
};

static int run_impl (int idx, void (*scenario)(void), unsigned char *out,
		size_t *outlen, int *out_checks, int *out_failures)
{
	char path[] = "/tmp/netchan-harness-XXXXXX";
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
		peer = &impls[idx == IMPL_C ? IMPL_RUST : IMPL_C];
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

static void run_case (const char *what, void (*scenario)(void))
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
		/* A child that died is a failed case: without this, two arms that
		 * both crashed would "agree" by both producing no trace. */
		failures++;
		fprintf(stderr, "FAIL: %s: an implementation did not produce a trace\n",
			what);
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
	}
}

int main (void)
{
	printf("net_chan differential harness: the C original as hwsv compiles it "
		"and the Rust module, with each feeding the other\n");

	// The Rust module reaches `net_drop` through the shim, so its address is
	// only known at runtime.
	impls[IMPL_RUST].drop = NetChan_TargetDrop();

	run_case("setup, choke and init", scenario_setup);
	run_case("out-of-band", scenario_out_of_band);
	run_case("framing both ways", scenario_framing);
	run_case("loss and reorder", scenario_loss_reorder);
	run_case("overflow", scenario_overflow);
	run_case("rate limit", scenario_rate);

	printf("checked %d expectations, %d failures, %d cases, %zu trace bytes\n",
		checks, failures, cases_run, trace_bytes);
	if (failures) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
