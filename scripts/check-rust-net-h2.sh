#!/usr/bin/env bash
# Hexen II driver tables, loopback and UDP: independent of HexenWorld's gate.
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
crate="$root/engine/rust"
echo '== Hexen II client differential (C originals vs Rust, C UDP peer) =='
# The fixture decides whether the port computes the C's bytes, so it is the one
# half of this gate a platform can run without the engine building there; see
# scripts/check-rust-net-h2-fixture.sh.  It is shared rather than copied so the
# floors stay in one place.
WORKDIR="$work/fixture" RUST_LIB="$crate/target/release/libengine_rs.a" \
  bash "$root/scripts/check-rust-net-h2-fixture.sh" >"$work/harness.log" 2>&1 || {
  tail -65 "$work/harness.log" >&2; exit 1;
}
grep '^PASS:\|^checked\|^RESULT:' "$work/harness.log"

echo '== Hexen II dedicated driver table variant =='
cargo build --release --offline --manifest-path "$crate/Cargo.toml" \
  --target-dir "$work/dedicated-target" \
  --features net_bsd_h2,net_udp_h2,h2ded_target >"$work/dedicated-cargo.log" 2>&1 || {
  tail -25 "$work/dedicated-cargo.log" >&2; exit 1;
}
includes=(-I"$root/engine/hexen2/server" -I"$root/engine/hexen2" \
  -I"$root/engine/h2shared" -I"$root/common")
cc "${includes[@]}" -DSERVERONLY -D_GNU_SOURCE=1 -std=gnu11 \
  -Dnet_drivers=c_net_drivers -Dnet_numdrivers=c_net_numdrivers \
  -Dnet_landrivers=c_net_landrivers -Dnet_numlandrivers=c_net_numlandrivers \
  -c "$root/engine/hexen2/net_bsd.c" -o "$work/bsd-dedicated.o"
cc "${includes[@]}" -DSERVERONLY -D_GNU_SOURCE=1 -std=gnu11 \
  -c "$crate/net_udp_h2_target.c" -o "$work/udp-target-dedicated.o"
cc "${includes[@]}" -DSERVERONLY -D_GNU_SOURCE=1 -std=gnu11 \
  "$crate/net_h2/tests/dedicated_tables.c" "$work/bsd-dedicated.o" \
  "$work/udp-target-dedicated.o" \
  "$work/dedicated-target/release/libengine_rs.a" -lm -o "$work/dedicated-tables"
"$work/dedicated-tables" | tee "$work/dedicated.log"
grep -q '^dedicated driver tables: 40 entries/layout/function-pointer checks passed$' "$work/dedicated.log" || {
  echo 'FAIL: dedicated table expectation floor changed' >&2; exit 1;
}
build=$(rust_gate_engine_build "$work")
for tgt in glhexen2 h2ded; do
  if find "$build/CMakeFiles/$tgt.dir" -type f \
      \( -name 'net_bsd.c.o' -o -name 'net_udp.c.o' -o -name 'net_loop.c.o' \) | grep -q .; then
    echo "FAIL: $tgt still compiles a Hexen II transport C original" >&2
    exit 1
  fi
  for sym in net_drivers net_landrivers UDP_Init; do
    n=$(nm -g --defined-only "$build/bin/$tgt" | grep -cE " [A-Z] $sym\$" || true)
    [ "$n" -eq 1 ] || { echo "FAIL: $tgt defines $sym $n times" >&2; exit 1; }
  done
done
for sym in Loop_Init Loop_Connect; do
  n=$(nm -g --defined-only "$build/bin/glhexen2" | grep -cE " [A-Z] $sym\$" || true)
  [ "$n" -eq 1 ] || { echo "FAIL: glhexen2 defines $sym $n times" >&2; exit 1; }
done
if nm -g --defined-only "$build/bin/h2ded" | grep -qE ' [A-Z] Loop_Init$'; then
  echo 'FAIL: h2ded unexpectedly exports loopback' >&2; exit 1;
fi
if nm -g --defined-only "$build/bin/hwsv" | grep -qE ' [A-Z] (Loop_Init|UDP_Init|net_drivers)$'; then
  echo 'FAIL: hwsv unexpectedly exports Hexen II transport' >&2; exit 1;
fi
echo 'PASS: Hexen II differential fixture, archive symbol ownership and no C transport objects'
