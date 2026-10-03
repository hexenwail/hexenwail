#!/usr/bin/env bash
#
# check-rust-net-h2-fixture.sh -- Hexen II's transport differential fixture on
# its own, for platforms where the engine itself does not build.
#
# scripts/check-rust-net-h2.sh is the whole gate: this fixture plus a build of
# glhexen2/h2ded/hwsv and the object- and symbol-ownership checks over them.
# That half needs a platform the engine builds on, and macOS is not one yet --
# glhexen2 there is behind ANGLE (uhexen2-6wj4, and docs/COMPILE still lists
# Linux and Windows).  This half needs only a C compiler, cargo and the two C
# originals, and it is the half that decides whether the port computes the same
# bytes as the C original does.  That question is platform-specific: under
# HAVE_SA_LEN -- macOS, the BSDs -- sockaddr_in carries an sa_len byte, the
# family moves to offset 1 and narrows to one byte, and the ioctl requests and
# errno values are different numbers.  So the macOS and FreeBSD jobs run this,
# and the Linux gate runs it as its first step.
#
# Requires cc and cargo on PATH; nm and cmake are not needed here.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,/^# SPDX/p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"

for tool in cc cargo; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH" >&2
		exit 2
	}
done

echo '== Hexen II client differential (C originals vs Rust, C UDP peer) =='
cargo build --release --offline --manifest-path "$crate/Cargo.toml" \
	--features net_loop_h2,net_bsd_h2,net_udp_h2 >"$work/cargo.log" 2>&1 || {
	tail -25 "$work/cargo.log" >&2
	exit 1
}
RUST_LIB="${RUST_LIB:-$crate/target/release/libengine_rs.a}" \
	WORKDIR="$work/fixture" \
	bash "$crate/net_h2/tests/run_diff_harness.sh" >"$work/harness.log" 2>&1 || {
	tail -65 "$work/harness.log" >&2
	exit 1
}
grep '^PASS:\|^checked\|^RESULT:' "$work/harness.log"
line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ cases, [0-9]+ trace bytes$' "$work/harness.log" || true)
checks=$(printf '%s\n' "$line" | grep -oE '[0-9]+' | sed -n 1p || true)
cases=$(printf '%s\n' "$line" | grep -oE '[0-9]+' | sed -n 2p || true)
bytes=$(printf '%s\n' "$line" | grep -oE '[0-9]+' | sed -n 3p || true)
# Observed on Linux: 128 expectations, 4 cases, 972 compared trace bytes.  The
# trace carries the C's diagnostics as well as its packets -- CON_Printf is
# stubbed but recorded -- so its exact length is host-dependent: whether
# gethostbyname() succeeds for this host's own name, and which strerror() text
# the platform returns for EBADF, both change it, and on macOS a hostname
# ending in ".local" makes the C skip the lookup entirely.  The byte floor is
# therefore "a substantial trace, not an empty one" rather than a target, and
# sits well under the smallest figure any of the three hosts is expected to
# produce.  The expectations and case floors are the tight ones: nothing about
# the host changes how many checks the fixture makes.
if [ "${checks:-0}" -lt 120 ] || [ "${cases:-0}" -lt 4 ] || [ "${bytes:-0}" -lt 600 ]; then
	echo "FAIL: Hexen II fixture below 120 expectations / 4 cases / 600 trace bytes" >&2
	exit 1
fi
echo "PASS: Hexen II transport differential fixture ($checks expectations, $cases cases, $bytes trace bytes)"
