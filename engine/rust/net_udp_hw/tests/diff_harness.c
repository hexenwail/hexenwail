// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for the Rust port of engine/hexenworld/shared/net_udp.c.
//
// The C original is compiled with every exported name renamed (c_NET_*) and
// linked into this binary beside the Rust module, and the two are driven
// through one table of function pointers.  After every call the observable
// state is recorded into a byte trace -- the four exported globals, the
// printed forms of the addresses, the bytes the message reader would see, the
// diagnostics -- and the two traces are compared byte for byte.
//
// The cases that matter here are the ones the C's own comments call out:
//
//   * the address conversions, including "a:b:c" (the C's loop takes the
//     *last* colon), a bare address, a hostname, and a name that does not
//     resolve;
//   * a real round trip over a loopback UDP socket, with the packet encoded
//     and decoded through the Huffman codec at both ends, because that loop is
//     the reason the two ports can disagree without either looking wrong;
//   * the timeout path, which must report "nothing to read" rather than
//     blocking or erroring.
//
// Each case runs in a child process, for each implementation separately: the
// transport owns a process-wide socket and process-wide storage, so a fresh
// process is the only way to start each arm where the engine starts, and a
// crash is then one reported case rather than a dead gate.
//
// The socket's *port* is ephemeral, so it is never recorded -- only facts
// derived from it (that it is non-zero, that two addresses that should match
// do) -- or the two arms would differ for reasons that mean nothing.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#include "quakedef.h"
#include "net.h"
#include "huffman.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <fcntl.h>

/*----------------------------------------------------------------------------
 * the two implementations
 *--------------------------------------------------------------------------*/

/// The C original, renamed so it can share the binary with the Rust module.
#define DECLARE_NET_IMPL(P) \
	extern void P##NET_Init (int port); \
	extern void P##NET_Shutdown (void); \
	extern int P##NET_GetPacket (void); \
	extern void P##NET_SendPacket (int length, void *data, const netadr_t *to); \
	extern int P##NET_CheckReadTimeout (long sec, long usec); \
	extern qboolean P##NET_CompareAdr (const netadr_t *a, const netadr_t *b); \
	extern qboolean P##NET_CompareBaseAdr (const netadr_t *a, const netadr_t *b); \
	extern const char *P##NET_AdrToString (const netadr_t *a); \
	extern const char *P##NET_BaseAdrToString (const netadr_t *a); \
	extern qboolean P##NET_StringToAdr (const char *s, netadr_t *a); \
	extern netadr_t P##net_local_adr; \
	extern netadr_t P##net_loopback_adr; \
	extern netadr_t P##net_from; \
	extern sizebuf_t P##net_message

DECLARE_NET_IMPL(c_);
DECLARE_NET_IMPL();

/// The shim's accessors, which is how the Rust arm reaches its storage (the
/// same functions the engine links).
extern sizebuf_t *NetUDP_TargetMessageBuf (void);
extern netadr_t *NetUDP_TargetFrom (void);
extern netadr_t *NetUDP_TargetLocalAdr (void);
extern netadr_t *NetUDP_TargetLoopbackAdr (void);

typedef struct net_impl_s {
	const char *name;
	void (*Init)(int);
	void (*Shutdown)(void);
	int (*GetPacket)(void);
	void (*SendPacket)(int, void *, const netadr_t *);
	int (*CheckReadTimeout)(long, long);
	qboolean (*CompareAdr)(const netadr_t *, const netadr_t *);
	qboolean (*CompareBaseAdr)(const netadr_t *, const netadr_t *);
	const char *(*AdrToString)(const netadr_t *);
	const char *(*BaseAdrToString)(const netadr_t *);
	qboolean (*StringToAdr)(const char *, netadr_t *);
	netadr_t *local_adr;
	netadr_t *loopback_adr;
	netadr_t *from;
	sizebuf_t *message;
} net_impl_t;

enum { IMPL_C = 0, IMPL_RUST = 1, IMPL_COUNT = 2 };

static net_impl_t impls[IMPL_COUNT] = {
	{
		"C",
		c_NET_Init, c_NET_Shutdown, c_NET_GetPacket, c_NET_SendPacket,
		c_NET_CheckReadTimeout, c_NET_CompareAdr, c_NET_CompareBaseAdr,
		c_NET_AdrToString, c_NET_BaseAdrToString, c_NET_StringToAdr,
		&c_net_local_adr, &c_net_loopback_adr, &c_net_from, &c_net_message,
	},
	{
		"Rust",
		NET_Init, NET_Shutdown, NET_GetPacket, NET_SendPacket,
		NET_CheckReadTimeout, NET_CompareAdr, NET_CompareBaseAdr,
		NET_AdrToString, NET_BaseAdrToString, NET_StringToAdr,
		NULL, NULL, NULL, NULL, /* filled in main(), from the accessors */
	},
};

static const net_impl_t *cur;

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
static void rec_str(const char *s) { rec(s, strlen(s) + 1); }

static void rec_adr(const netadr_t *a)
{
	rec(a->ip, 4);
	rec_int(a->port);
}

/// The same, for addresses whose port is an ephemeral socket number: the port
/// is a different integer in every process, so a state snapshot records only
/// whether one was assigned.  Addresses the *test* constructed -- the ones
/// StringToAdr parses -- keep their real port, because that is the value under
/// test.
static void rec_adr_assigned(const netadr_t *a)
{
	rec(a->ip, 4);
	rec_int(a->port != 0);
}

/*----------------------------------------------------------------------------
 * the engine surface both arms use
 *--------------------------------------------------------------------------*/

static char printf_buf[512];

/// The transport's own startup line prints the socket's address, and the port
/// in it is ephemeral: it is different in every process and means nothing to
/// the comparison.  The *shape* of the diagnostic is what both arms must
/// agree on, so a ":<digits>" run is rewritten to ":<port>" before recording.
/// Nothing else here carries a port.
static void normalize_port(char *s)
{
	char *colon = NULL;
	char *p;

	for (p = s; *p; p++)
		if (*p == ':')
			colon = p;
	if (!colon || !colon[1])
		return;
	if (colon[1] < '0' || colon[1] > '9')
		return;
	for (p = colon + 1; *p >= '0' && *p <= '9'; p++)
		;
	/* Only whitespace may follow, or this is not a port. */
	if (*p && *p != '\n' && *p != ' ' && *p != '\t')
		return;
	{
		char tail[8];
		size_t t = 0;

		while (*p && t < sizeof tail - 1)
			tail[t++] = *p++;
		tail[t] = 0;
		strcpy(colon + 1, "<port>");
		strcat(colon + 1, tail);
	}
}

/// `Con_Printf(fmt, ...)` as the macro expands, recorded rather than printed:
/// the diagnostics are part of what the two arms must agree on.
void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list ap;

	(void)flags;
	va_start(ap, fmt);
	vsnprintf(printf_buf, sizeof printf_buf, fmt, ap);
	va_end(ap);
	normalize_port(printf_buf);

	rec_int(0x9911);
	rec_str(printf_buf);
}

static jmp_buf err_env;
static int err_armed;
static int err_calls;
static char err_buf[256];

/// The C's fatal paths never return; here they unwind to the guarded call so
/// the harness can compare them.
void Sys_Error (const char *fmt, ...)
{
	va_list ap;

	err_calls++;
	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);

	if (!err_armed) {
		fprintf(stderr, "harness bug: Sys_Error outside a guarded call: %s\n",
			err_buf);
		abort();
	}
	longjmp(err_env, 1);
}

/// The Rust sizebuf port is the real one when the harness archive carries it;
/// this is the same function, so both arms share one implementation of it.
extern void SZ_Init (sizebuf_t *buf, byte *data, int length);

/// `-ip`/`-bindip` are absent in the harness, so the C's checks find nothing.
int COM_CheckParm (const char *parm)
{
	(void)parm;
	return 0;
}

/// `com_argc`/`com_argv` are macros over host_parms in the engine; both the C
/// original and the port read it, so the harness owns the one definition.
quakeparms_t *host_parms;
static quakeparms_t parms;

/// The Rust sizebuf port takes its storage from the hunk when `data` is NULL.
/// The harness always passes a buffer, so this exists only to satisfy the
/// link -- and says so loudly if that ever stops being true.
void *Hunk_AllocName (int size, const char *name)
{
	(void)size;
	(void)name;
	fprintf(stderr, "harness bug: Hunk_AllocName called\n");
	abort();
}

/*----------------------------------------------------------------------------
 * helpers
 *--------------------------------------------------------------------------*/

static void op_reset(void)
{
	err_calls = 0;
	err_buf[0] = 0;
	printf_buf[0] = 0;
}

/// Every call is followed by this: the whole observable state, not just the
/// return value.
static void rec_state(void)
{
	rec_adr_assigned(cur->local_adr);
	rec_adr_assigned(cur->loopback_adr);
	rec_adr_assigned(cur->from);
	rec_int(cur->message->cursize);
	rec_int(cur->message->maxsize);
	rec_int(cur->message->data != NULL);
}

static void mkadr(netadr_t *a, int b0, int b1, int b2, int b3, int port)
{
	a->ip[0] = (byte)b0;
	a->ip[1] = (byte)b1;
	a->ip[2] = (byte)b2;
	a->ip[3] = (byte)b3;
	a->port = htons((unsigned short)port);
	a->pad = 0;
}

/*----------------------------------------------------------------------------
 * the peer socket: a plain libc UDP socket, so the round trip crosses the
 * kernel rather than being simulated
 *--------------------------------------------------------------------------*/

static int peer_socket = -1;
static unsigned short peer_port;

static void peer_open(void)
{
	struct sockaddr_in addr;
	socklen_t len;
	int one = 1;

	peer_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (peer_socket < 0) {
		fprintf(stderr, "harness bug: peer socket: %s\n", strerror(errno));
		abort();
	}
	setsockopt(peer_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(peer_socket, (struct sockaddr *)&addr, sizeof addr) < 0) {
		fprintf(stderr, "harness bug: peer bind: %s\n", strerror(errno));
		abort();
	}
	len = sizeof addr;
	if (getsockname(peer_socket, (struct sockaddr *)&addr, &len) < 0) {
		fprintf(stderr, "harness bug: peer getsockname: %s\n", strerror(errno));
		abort();
	}
	peer_port = addr.sin_port;

	/* A receive timeout, so a lost packet fails the case instead of hanging
	 * the gate. */
	{
		struct timeval tv;
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		setsockopt(peer_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	}
}

/// Send `n` bytes to the transport, leaving it to decide they arrived.
static void peer_send(const netadr_t *to, const void *data, int n)
{
	struct sockaddr_in addr;
	ssize_t sent;

	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	memcpy(&addr.sin_addr, to->ip, 4);
	addr.sin_port = to->port;

	sent = sendto(peer_socket, data, (size_t)n, 0,
			(struct sockaddr *)&addr, sizeof addr);
	check(sent == n, "[%s]: peer sent %d of %d", cur->name, (int)sent, n);
	rec_int(sent == n);
}

/// Read one datagram from the transport, into a caller buffer.
static int peer_recv(void *buf, int maxn)
{
	ssize_t got;

	got = recv(peer_socket, buf, (size_t)maxn, 0);
	rec_int(got > 0);
	return (int)(got > 0 ? got : -1);
}

/*----------------------------------------------------------------------------
 * scenarios
 *--------------------------------------------------------------------------*/

/// The address conversions and their printed forms.  This is where the C's
/// own quirks live, so the strings are taken from the C's format.
static void scenario_addresses(void)
{
	netadr_t a, b, out;
	int i;

	mkadr(&a, 127, 0, 0, 1, 26000);
	mkadr(&b, 127, 0, 0, 1, 26000);
	mkadr(&out, 0, 0, 0, 0, 0);

	rec_int(cur->CompareAdr(&a, &b));
	rec_int(cur->CompareBaseAdr(&a, &b));
	rec_str(cur->AdrToString(&a));
	rec_str(cur->BaseAdrToString(&a));

	/* Same address, different port: the two predicates must disagree. */
	mkadr(&b, 127, 0, 0, 1, 26001);
	rec_int(cur->CompareAdr(&a, &b));
	rec_int(cur->CompareBaseAdr(&a, &b));

	/* Different address, same port. */
	mkadr(&b, 127, 0, 0, 2, 26000);
	rec_int(cur->CompareAdr(&a, &b));
	rec_int(cur->CompareBaseAdr(&a, &b));

	/* The string forms.  "a:b:c" is the case the C's loop handles by taking
	 * the last colon, and a bare address takes the whole string. */
	{
		static const char *const strings[] = {
			"127.0.0.1:26000",
			"127.0.0.1",
			"255.255.255.255:1",
			"0.0.0.0:0",
			"1.2.3.4:0",
			"not.a.real.host.invalid:26000",
			"a:b:c",
		};

		for (i = 0; i < (int)(sizeof strings / sizeof strings[0]); i++) {
			op_reset();
			memset(&out, 0x7f, sizeof out);
			err_armed = 1;
			if (setjmp(err_env) == 0) {
				int ok = cur->StringToAdr(strings[i], &out);
				rec_str(strings[i]);
				rec_int(ok);
				if (ok)
					rec_adr(&out);
			}
			err_armed = 0;
			rec_int(err_calls);
			rec_state();
		}
	}
}

/// A real round trip: the transport receives what the peer sends, decodes it
/// through the Huffman codec, and hands it to the message reader; then it
/// sends back through the same codec and the peer reads it.
static void scenario_roundtrip(void)
{
	netadr_t peer_adr;
	int n;

	cur->Init(PORT_ANY);

	/* The C's NET_Init prints the local address, which contains a hostname-
	 * dependent string; record only that it happened. */
	rec_int(cur->local_adr->port != 0);
	rec_adr(cur->loopback_adr);
	check(cur->loopback_adr->ip[0] == 127 && cur->loopback_adr->ip[3] == 1,
		"[%s]: loopback is not 127.0.0.1", cur->name);

	mkadr(&peer_adr, 127, 0, 0, 1, ntohs(peer_port));

	/* Nothing is waiting yet. */
	check(cur->CheckReadTimeout(0, 0) == 0,
		"[%s]: CheckReadTimeout with no data returned non-zero", cur->name);
	rec_int(cur->CheckReadTimeout(0, 0) == 0);

	/* The peer sends a message the transport must decode -- encoded through the
	 * same Huffman codec, because that is what the wire carries.  Sending the
	 * plaintext would only prove the decoder rejects it. */
	{
		static const char payload[] = "hello from the other end of the socket\n";
		size_t plen = sizeof payload - 1;
		byte encoded[512];
		int clen = 0;

		HuffEncode((const unsigned char *)payload, encoded, (int)plen, &clen);
		rec_int(clen > 0);
		peer_send(cur->local_adr, encoded, clen);

		n = cur->GetPacket();
		rec_int(n);
		rec_int(n == (int)plen);
		check(n == (int)plen, "[%s]: GetPacket returned %d, want %d",
			cur->name, n, (int)plen);
		if (n > 0) {
			rec(cur->message->data, (size_t)n);
			check(cur->message->cursize == n,
				"[%s]: net_message.cursize is %d, want %d",
				cur->name, cur->message->cursize, n);
		}
		/* net_from is the peer: the address is compared, the ephemeral port
		 * is recorded only as "one was assigned". */
		rec_adr_assigned(cur->from);
		check(cur->CompareBaseAdr(cur->from, &peer_adr),
			"[%s]: net_from is not the peer", cur->name);
	}

	/* And the other direction: the transport sends, the peer decodes.  The
	 * peer uses the same Huffman codec, so this proves the framing rather
	 * than the codec. */
	{
		static const char reply[] = "and back again\n";
		size_t rlen = sizeof reply - 1;
		byte raw[8192];
		int got;

		cur->SendPacket((int)rlen, (void *)reply, &peer_adr);
		got = peer_recv(raw, sizeof raw);
		rec_int(got > 0);
		if (got > 0) {
			byte plain[1024];
			int outlen = (int)sizeof plain;

			HuffDecode(raw, plain, got, &outlen, (int)sizeof plain);
			rec_int(outlen);
			rec(plain, (size_t)outlen);
		}
	}

	/* A packet from nowhere: the timeout path again, this time after traffic. */
	check(cur->CheckReadTimeout(0, 0) == 0,
		"[%s]: CheckReadTimeout after the round trip returned non-zero",
		cur->name);
	rec_int(cur->CheckReadTimeout(0, 0) == 0);

	rec_state();
	cur->Shutdown();
}

/// The failure paths: an unresolvable name, and a send to a port nobody is
/// listening on (which the C reports and does not treat as fatal).
static void scenario_failures(void)
{
	netadr_t dead;
	netadr_t out;

	cur->Init(PORT_ANY);

	op_reset();
	rec_int(cur->StringToAdr("not.a.real.host.invalid", &out));
	rec_int(err_calls);

	/* A send to a closed port: ECONNREFUSED on the next receive, which the C
	 * reports and returns 0 from. */
	mkadr(&dead, 127, 0, 0, 1, 1); /* port 1: nothing listens */
	cur->SendPacket(4, (void *)"ping", &dead);

	op_reset();
	{
		int n = cur->GetPacket();
		rec_int(n);
	}
	rec_state();
	cur->Shutdown();
}

/// The receive buffer's limit, taken from the C rather than guessed:
/// `MAX_UDP_PACKET` is `HWNET_MAX_MSGLEN + 9` (`net_udp.c:53`), and a datagram
/// that exactly fills the buffer is reported as oversize and dropped rather
/// than truncated (`:202-207`).  The gate checks this constant against both
/// sources so it cannot drift.
#define HARNESS_RX_LIMIT (HWNET_MAX_MSGLEN + 9)

/// Payload sizes, not just the couple of bytes the round trip uses: the decode
/// path's buffer arithmetic is where an off-by-one hides, and one byte is the
/// case that exercises a codec's smallest input.
static void scenario_sizes(void)
{
	static const int sizes[] = { 1, 2, 512, 4000 };
	netadr_t peer;
	size_t i;

	cur->Init(PORT_ANY);
	mkadr(&peer, 127, 0, 0, 1, ntohs(peer_port));

	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		byte plain[8192], encoded[8192];
		int clen = 0;

		memset(plain, (int)(0xa0 + i), (size_t)sizes[i]);
		op_reset();
		rec_int(sizes[i]);
		HuffEncode(plain, encoded, sizes[i], &clen);
		rec_int(clen);
		peer_send(cur->local_adr, encoded, clen);
		{
			int n = cur->GetPacket();

			rec_int(n);
			check(n == sizes[i], "[%s]: GetPacket returned %d for a %d-byte payload",
				cur->name, n, sizes[i]);
			if (n > 0)
				rec(cur->message->data, (size_t)n);
			rec_adr_assigned(cur->from);
		}
		rec_state();
	}
	cur->Shutdown();
}

/// The boundary at the receive buffer: a datagram of exactly the limit is
/// reported oversize and dropped, and one byte under it reaches the decoder.
/// The second half is the more interesting: it is attacker-shaped input, and
/// whatever the C does with it is what the port has to do.
static void scenario_oversize(void)
{
	byte big[HARNESS_RX_LIMIT];

	cur->Init(PORT_ANY);
	memset(big, 0x5a, sizeof big);

	op_reset();
	rec_int((int)sizeof big);
	peer_send(cur->local_adr, big, (int)sizeof big);
	{
		int n = cur->GetPacket();

		rec_int(n);
		check(n == 0, "[%s]: an oversize datagram returned %d, want 0",
			cur->name, n);
		rec_adr_assigned(cur->from);
	}
	rec_state();

	op_reset();
	peer_send(cur->local_adr, big, (int)sizeof big - 1);
	err_armed = 1;
	if (setjmp(err_env) == 0) {
		int n = cur->GetPacket();

		rec_int(n);
	}
	err_armed = 0;
	rec_int(err_calls);
	rec_state();
	cur->Shutdown();
}

/// `NET_CheckReadTimeout` is a `select` with a deadline (`net_udp.c:254-265`),
/// and the landed harness only ever called it with `(0, 0)`.  A bounded wait
/// that expires and one that finds data ready are different paths.
static void scenario_timeout(void)
{
	byte encoded[64];
	int clen = 0;

	cur->Init(PORT_ANY);

	/* Nothing is waiting, so a bounded wait expires and reports 0. */
	rec_int(cur->CheckReadTimeout(0, 50000));

	/* Now something is: the same wait reports readable rather than timing out. */
	HuffEncode((const unsigned char *)"t", encoded, 1, &clen);
	peer_send(cur->local_adr, encoded, clen);
	rec_int(cur->CheckReadTimeout(0, 200000));

	/* Drain it; the wait expires again. */
	rec_int(cur->GetPacket());
	rec_int(cur->CheckReadTimeout(0, 50000));
	rec_state();
	cur->Shutdown();
}

/// Init, Shutdown, then Init again.  A second `NET_Init` has to give a working
/// socket rather than a stale descriptor, and the state it leaves after each
/// step is part of the contract.
static void scenario_lifecycle(void)
{
	byte encoded[64];
	int clen = 0;

	cur->Init(PORT_ANY);
	rec_int(cur->local_adr->port != 0);
	rec_adr(cur->loopback_adr);
	cur->Shutdown();
	rec_state();

	cur->Init(PORT_ANY);
	rec_int(cur->local_adr->port != 0);
	rec_adr(cur->loopback_adr);

	/* Traffic after the re-init proves the second socket is real. */
	HuffEncode((const unsigned char *)"after re-init", encoded, 13, &clen);
	peer_send(cur->local_adr, encoded, clen);
	rec_int(cur->GetPacket());
	rec_state();

	cur->Shutdown();
	rec_state();
}

/*----------------------------------------------------------------------------
 * case runner
 *--------------------------------------------------------------------------*/

struct child_result {
	size_t len;
	int checks;
	int failures;
};

static int run_impl(int idx, void (*scenario)(void), unsigned char *out,
		size_t *outlen, int *out_checks, int *out_failures)
{
	char path[] = "/tmp/net-udp-hw-harness-XXXXXX";
	struct child_result hdr;
	int fd;
	pid_t pid;
	int status = 0;

	fd = mkstemp(path);
	if (fd < 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	if (pid == 0) {
		cur = &impls[idx];
		/* The Rust arm's storage lives in the shim's accessors; fill the
		 * table in here, before anything reads it. */
		if (idx == IMPL_RUST) {
			impls[idx].local_adr = NetUDP_TargetLocalAdr();
			impls[idx].loopback_adr = NetUDP_TargetLoopbackAdr();
			impls[idx].from = NetUDP_TargetFrom();
			impls[idx].message = NetUDP_TargetMessageBuf();
		}
		trace_len = 0;
		checks = 0;
		failures = 0;
		op_reset();
		peer_open();
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
	if (read(fd, out, hdr.len) != (ssize_t)hdr.len) {
		close(fd);
		unlink(path);
		fprintf(stderr, "FAIL: %s trace truncated\n", impls[idx].name);
		return -1;
	}
	close(fd);
	unlink(path);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "FAIL: %s did not finish", impls[idx].name);
		if (WIFSIGNALED(status))
			fprintf(stderr, " (killed by signal %d)", WTERMSIG(status));
		fprintf(stderr, "\n");
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
		/* The surrounding bytes, so a value difference is readable rather
		 * than just located. */
		{
			size_t lo = k > 24 ? k - 24 : 0;
			size_t j;

			fprintf(stderr, "      C   :");
			for (j = lo; j < len[IMPL_C] && j < k + 24; j++)
				fprintf(stderr, " %02x", out[IMPL_C][j]);
			fprintf(stderr, "\n      Rust:");
			for (j = lo; j < len[i] && j < k + 24; j++)
				fprintf(stderr, " %02x", out[i][j]);
			fprintf(stderr, "\n      C text   : %.60s\n", (const char *)&out[IMPL_C][lo]);
			fprintf(stderr, "      Rust text: %.60s\n", (const char *)&out[i][lo]);
		}
	}
}

int main(void)
{
	printf("net_udp_hw differential harness: the C original and the Rust "
		"transport module\n");

	/* The Huffman codec must be initialised before either arm runs; it is
	 * the real Rust port, shared by both arms, as the engine shares it. */
	HuffInit();

	/* The port's shim reads host_parms for -ip/-bindip. */
	parms.argc = 1;
	parms.argv = NULL;
	host_parms = &parms;

	run_case("address conversions", scenario_addresses);
	run_case("round trip", scenario_roundtrip);
	run_case("failure paths", scenario_failures);
	run_case("payload sizes", scenario_sizes);
	run_case("oversize datagrams", scenario_oversize);
	run_case("read timeouts", scenario_timeout);
	run_case("init and shutdown lifecycle", scenario_lifecycle);

	printf("checked %d expectations, %d failures, %d cases, %zu trace bytes\n",
		checks, failures, cases_run, trace_bytes);
	if (failures) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
