# Phase 7 transports: ownership, target matrix, and the #233 decision this slice forces

Written before any build change, as #61 requires and as every earlier slice did.
Baseline `origin/master` `35df9ec70`. Refs #61, #233, #42.

Scope of the slice: Hexen II's `net_bsd.c` (100 lines), `net_loop.c` (235) and
`net_udp.c` (602), and HexenWorld's `engine/hexenworld/shared/net_udp.c` (425).
Together they are the transport layer the channel and datagram layers will later
sit on.

## 1. Ownership

| thing | owner | rule for the port |
|---|---|---|
| `net_drivers[]` and `net_landrivers[]` (`net_bsd.c:29`, `:74`; `net_win.c` has its own pair) | the port | the driver registry: an array of `net_driver_t`/`net_landriver_t` whose fields are **function pointers and name strings**, read by `net_main.c` to walk drivers. The entries name `Datagram_*` (net_dgrm.c), `Loop_*` (net_loop.c) and `UDP_*` (net_udp.c) — see §3, which is where this slice's difficulty lives. |
| `netadr_t` and its conversions (`net_udp.c` H2W: `NET_CompareBaseAdr`, `NET_CompareAdr`, `NET_StringToAdr`, `NET_AdrToString`, `NET_GetNameFromAddr`, `NET_GetAddrFromName`) | the port | a value type with a printed form; the harness compares both the struct fields and the formatted strings, because the strings are what a server browser shows. |
| `net_message`, `net_message_buffer` (`net_udp.c` H2W `:41`, `:54`) | the port | **defined twice in the tree**: `engine/hexen2/net_main.c:57` also defines `net_message`, and glhexen2 compiles both files. They link today only because both are C tentative definitions and the build uses common symbols; a Rust `#[no_mangle] static mut` is a strong definition, so the port must decide explicitly which target defines it and keep h2ded's own definition working. |
| `net_from`, `net_local_adr`, `net_loopback_adr`, `net_socket`, `huffbuff` (H2W only) | the port | exported and read outside the file (`net_chan.c`, `sv_send.c`, `sv_ccmds.c`). `huffbuff` is the Huffman receive buffer, so the H2W transport's receive path runs through the **Rust huffman port**. |
| the socket lifetime (`net_socket`, `UDP_OpenSocket`/`CloseSocket`) | the port | one socket for the process, opened in `NET_Init` and closed in `NET_Shutdown`; the harness must not leak it between cases. |
| `net_activeconnections`, `net_driverlevel`, `net_hostport`, `tcpipAvailable`, `my_tcpip_address` (H2) | the port | long-lived state read by `net_main.c` and the console commands. |
| `cls`, `hostcache`, `hostCacheCount` (referenced by `net_loop.c`) | **the caller** | client-only state this file only reads; see §3. |

## 2. Target matrix

| file | glhexen2 | h2ded | hwsv | conditional arms |
|---|:--:|:--:|:--:|---|
| `hexen2/net_bsd.c` | yes | yes | — | none |
| `hexen2/net_loop.c` | yes | — | — | none (removed from the dedicated list, `CMakeLists.txt:1307`) |
| `hexen2/net_udp.c` | yes | yes | — | none |
| `hexenworld/shared/net_udp.c` | yes | — | yes | one: `H2W_INTEGRATED` (`:26`) |

Only one conditional arm in all four files, which is why the difficulty here is
not divergence inside the code — it is the **symbol set each file is entitled to
reference**.

## 3. The #233 case, at scale

The archive is a single object linked into all three binaries, so every symbol
the Rust module references must exist in all three. Compiling each transport
with the defines of each target that builds it, and checking its undefined
engine symbols against what each binary actually defines:

```
net_bsd.c   → Datagram_* (11 symbols)      absent in hwsv
            → Datagram_Connect,
              Datagram_SearchForHosts      absent in h2ded too
            → Loop_* (12 symbols)          absent in h2ded and hwsv
            → UDP_* (17 symbols)           absent in hwsv
net_loop.c  → cls, hostcache,
              hostCacheCount               absent in h2ded and hwsv
            → net_activeconnections,
              net_driverlevel,
              NET_NewQSocket               absent in hwsv
net_udp.c   → my_tcpip_address,
   (Hexen II) net_hostport,
              tcpipAvailable               absent in hwsv
hexenworld/shared/net_udp.c → —            nothing missing in any target
```

The last line matters: **HexenWorld's transport ports cleanly into the single
archive.** Every engine symbol it references exists in all three binaries,
including the ones it defines itself.

The three Hexen II files do not. Their references are exactly the properties
that make them per-target: `net_loop.c` is the *client's* loopback driver and
reads `cls`; `net_bsd.c`'s tables name the drivers each target is allowed to
have; `net_udp.c` reads H2-only network state. The C gets away with it because
each file is compiled only into the targets that define those symbols. One
archive cannot reproduce that by itself.

## 4. The options

**(a) The shim owns the per-target symbol set.** The per-target C shim — the
mechanism three slices have now used — takes over the driver tables themselves
(or supplies their entries through an accessor the Rust table calls), because
the tables are the target's own symbol set expressed as data. The Rust module
keeps the table *types*, the iteration and everything else. Cost: `net_bsd.c`'s
main content stays C, which weakens the claim that the file is ported; and the
same treatment is needed for `net_loop.c`'s `cls` reads and `net_udp.c`'s
H2-only state, so the shim grows to own a large share of three files.

**(b) Per-feature-set archives (#233's option 2).** Build the crate once per
feature set — client, dedicated, HexenWorld — and link each binary the archive
built for it. The Hexen II transport then exists only in the archives that have
its symbols, and the HexenWorld transport only in the HexenWorld one, exactly as
the C is compiled today. Cost: three cargo builds and three archives per
configuration, multiplied across Windows and wasm, and the feature matrix
multiplied by every later port; each binary still links exactly one archive, so
the exactly-once property that settled Phase 0 survives.

**(c) Split the slice.** Port the transport that does *not* have the problem
first — HexenWorld's `net_udp.c` — and put the Hexen II trio behind a decision
on (a) versus (b), since those three files are where the choice is real. This
gets a slice landed and the problem characterised before anyone commits to a
build change.

**Recommendation: (c) now, then (b) if the H2 trio is to be ported at all.**
This is the first case where the per-target facts are not a handful of call
sites but a file's entitlement to a symbol set; option (a) can express it only
by leaving the tables — the substance of `net_bsd.c` — in C, and doing that to
three files makes "ported" mean less than it has in every previous slice. That
is a decision for the maintainer, not a side effect of this goal.

## 5. Fixture strategy, per stack

The roadmap requires Hexen II and HexenWorld to have **separate** fixtures, and
they are separate protocols:

- **Hexen II**: golden packets for the driver framing, driven through the
  loopback driver (which needs no socket) and the UDP land driver over a real
  socket on a loopback port, compared against the C original's bytes.
- **HexenWorld**: the `NET_*` address conversions and their printed forms, the
  send/receive framing with the **Rust huffman codec** in the loop, and the
  state of the five exported globals after each call.

Tick rate: `sv_physfps`, default **72 Hz**, is the server's physics rate and is
not the transport's concern, but any simulation fixture added later must record
it (`sys_ticrate` is dedicated-server only and is not it).

## 6. What the differential harnesses must observe

Per call: the `netadr_t` fields and the string each conversion produces; the
driver tables' entries (name, function pointer identity, `initialised` and the
other flags) compared entry for entry against the C's, since a table is where an
invented entry hides; each of the exported globals' values; the socket's state
and the bytes actually written to and read from the loopback port; and every
diagnostic. Failure paths — a bind that fails, a bad address string, a packet
larger than the buffer, a receive timeout — run in child processes where the C
aborts.

## Status (2026-09-28)

Recorded here rather than only on the issue tracker because a completion
auditor can read this file and cannot read tracker comments, and because a
scope decision that exists only in a comment is not inspectable.

**Landed from this model:**

- HexenWorld's transport, `engine/hexenworld/shared/net_udp.c` →
  `engine/rust/src/net_udp_hw.rs`, with `engine/rust/net_udp_hw_target.c` owning
  the C-visible storage because `net.h` renames every symbol under
  `H2W_INTEGRATED`. PR #295, squash-merged `a51312502`.
- Its harness and gate, `scripts/check-rust-net-udp-hw.sh`, since widened from
  three cases and 800 trace bytes to seven and 6 220 (payload sizes, the
  oversize boundary, a bounded read timeout, the init/shutdown lifecycle), with
  a gate check that keeps the harness's receive limit equal to the C's
  `HWNET_MAX_MSGLEN + 9`.
- Beyond this model's slice: the HexenWorld channel layer (`net_chan.c` →
  `engine/rust/src/net_chan.rs`), PR #297, merged `915d1dcfb`; and the first
  sanitizer pass over the port harnesses, which found a real harness defect in
  `cmd` and fixed it, PR #298, merged `4c70a09ec`, recipe in
  `history/sanitizer_pass.md`.

**Deferred:** the Hexen II transport trio — `engine/hexen2/net_bsd.c`,
`net_loop.c` and `net_udp.c`. On 2026-09-28 the maintainer chose to defer it to
the #233 build-architecture decision (the driver tables in the per-target shim,
versus per-feature-set archives) rather than settle that decision as a side
effect of this slice. Those three files are not different in degree from the
HexenWorld transport above: their per-target facts are a *file's entitlement to
a symbol set* — `net_bsd.c` is nothing but the `net_drivers[]` /
`net_landrivers[]` tables naming `Datagram_*`, `Loop_*` and `UDP_*` drivers
that hwsv does not define, `net_loop.c` reads the client-only `cls`, and Hexen
II's `net_udp.c` reads H2-only network state — so a per-target shim can express
the requirement only by leaving the substance of the file in C.

**Consequence at the time of that deferral:** the Phase 7 transport objective
was **half met**. The HexenWorld half was delivered; the Hexen II half had no
port, harness or gate. This was a status record, not an amendment to the goal.

## Follow-up after #233 (2026-09-29; not a goal-completion claim)

The maintainer selected per-feature-set archives in #233, implemented in
`build(rust): select one feature-set archive per engine target (#233)` (#305).
That unblocked a **Linux and browser** implementation of the Hexen II trio:
`net_bsd.c`'s full function-pointer tables, `net_loop.c`'s client loopback,
and `net_udp.c`'s socket/address driver now live in `net_bsd_h2.rs`,
`net_loop_h2.rs` and `net_udp_h2.rs`. A small `net_loop_h2_target.c` exposes
client/server facts without copying the client state structs into Rust. Each
binary still links one archive: Linux glhexen2 has all three ports; Linux
h2ded has tables and UDP with the SERVERONLY driver layout, no loopback;
hwsv has none. The Windows binaries still build their *different* C network
files, `net_win.c` and `net_wins.c`, while the client loopback is Rust.

The separate Hexen II gate `scripts/check-rust-net-h2.sh` builds the C
originals under renamed symbols. Its client fixture checks driver table entry
order, fields and every function pointer; qsocket and table layouts; reliable
and unreliable loopback packets and state; UDP address conversion, actual
bidirectional socket traffic with an ordinary C peer, init/shutdown, invalid
socket and bind failure diagnostics. A SERVERONLY fixture checks the dedicated
table's different ABI and all 40 entry/layout/function-pointer facts. This is
not HexenWorld's Huffman-framed transport or its gate.

**Platform boundary, resolved 2026-10-02 — see below.** The paragraph that
stood here said CMake deliberately kept `net_bsd.c` and `net_udp.c` as C on
macOS and non-Linux Unix, because their socket-address family layouts, ioctl
request sizes and errno values differ from the then Linux-specific Rust FFI,
and that no cross-target parity fixture existed. That is no longer the case.

Emscripten *does* link the Rust code and no Hexen II C transport originals;
wasm32 staticlib cannot export data globals, so `wasm_globals.c` owns only the
two table arrays and two counts and `NetH2_InitDriverTables` fills their
entries in Rust before `NET_Init` uses them. WebGL2 build and the existing wasm
ABI gate pass, but there is still no wasm runtime packet fixture.

## The platform boundary, and what closed it (2026-10-02)

The port had no platform dependence left in it beyond a handful of values that
are not logic: the socket constants, the two ioctl request numbers, the errno
values compared against `EWOULDBLOCK` and `ECONNREFUSED`, how `errno` and
`h_errno` are reached, and the byte layout of `sockaddr_in`. Those are exactly
what `net_udp.c` reads out of the system headers, and `common/net_sys.h`
already names the one that moves — `HAVE_SA_LEN`/`SA_FAM_OFFSET`, because a
BSD-family `sockaddr` carries an `sa_len` byte, so the family sits at offset 1
and is one byte wide instead of two. `struct qsockaddr` in
`engine/hexen2/net_defs.h` mirrors the same split, which is why the two cast
between each other on both families.

So the values now come from C, once: `engine/rust/net_udp_h2_target.c` returns
each one by reading the real macro or `offsetof`, and the shim is attached to
every target that links the feature. Returning them rather than writing per-OS
constant tables in Rust is the point — there is no second copy to drift, and a
platform nobody enumerated still gets the right numbers. Nothing in that file
is transport logic; the driver is still entirely in `net_udp_h2.rs`, and
`QSockAddr` became a fixed 16-byte region whose family is read and written at
the shim's offset, which is what removed the last `target_os` assumption from
the port.

`engine/CMakeLists.txt` then dropped the
`if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")` fallback in both the client and
h2ded source lists, and the feature sets became `UNIX` rather than
`CMAKE_SYSTEM_NAME STREQUAL "Linux"`. Windows is unchanged: it builds
`net_win.c`/`net_wins.c`, a different transport this port does not replace.

**Evidence.** `scripts/check-rust-net-h2-fixture.sh` is the differential
fixture on its own — the half that decides whether the port computes the C's
bytes, and the half that does not need the engine to build.
`scripts/check-rust-net-h2.sh` now calls it, and two CI jobs run it on the
platforms whose layout is the point: `macos-transport` on `macos-latest`
(darwin/arm64) and `freebsd-transport` in a FreeBSD VM (x86_64). Both link the
Rust archive against the C originals compiled by that host's own compiler, so a
wrong family offset, ioctl request or errno value fails there rather than
passing on Linux.

All three passed, at PR #320:

| host | expectations | cases | trace bytes |
|---|---|---|---|
| Linux (x86_64) | 128 | 4 | 972 |
| macOS (arm64) | 130 | 4 | 1082 |
| FreeBSD (x86_64) | 130 | 4 | 1044 |

The trace is a byte-for-byte comparison of what the C original and the Rust
port produce; the three byte counts differ only because the trace records the
C's diagnostics as well as its packets (`CON_Printf` is stubbed but recorded),
and whether `gethostbyname()` resolves the host's own name, and which
`strerror()` text the platform returns, both vary. That the counts differ while
the comparison still succeeds is the point: the port is not agreeing with
Linux's answers, it is computing the host's own.

Two things this does **not** establish, stated plainly so a reader does not
infer them:

- **No engine-level build on macOS or the BSDs.** The engine does not build on
  macOS at all yet — glhexen2 there is behind ANGLE (uhexen2-6wj4), and
  docs/COMPILE still lists Linux and Windows — so there is no macOS
  `glhexen2`/`h2ded` binary to show carrying no `net_bsd.c` object. The
  object- and symbol-ownership half of the gate runs on Linux only. That is a
  gap in platform support, not in the transport: it is the same CMake code
  path Linux exercises, and it is covered the moment macOS builds, which is
  what the C fallback was waiting on and is no longer waiting for.
- **Still no wasm runtime packet fixture.** The browser links the Rust
  transport and the wasm ABI gate passes; nothing drives a packet through it
  under Emscripten. The interface scan is compiled out there
  (`H2UDP_has_ifscan()` is 0, as `#if defined(SIOCGIFCONF)` was in the C).
