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
