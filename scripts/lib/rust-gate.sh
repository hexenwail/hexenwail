# shellcheck shell=bash
#
# scripts/lib/rust-gate.sh -- shared by the scripts/check-rust-*.sh gates.
#
# Every gate checks symbols in the same three binaries, and before the Rust
# archive was mandatory each one built them twice, once per flag setting.
# Eleven gates made that twenty-two full engine builds per CI run for one tree.
# There is one configuration now, so there is one build, and these helpers
# are how a gate gets it.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# rust_gate_engine_build WORKDIR
#
# Prints the path of a CMake build of glhexen2, h2ded and hwsv.  With
# ENGINE_BUILD set (scripts/check-rust.sh sets it) that build is reused as is;
# otherwise one is configured and built under WORKDIR/build.  Build output
# goes to WORKDIR/build.log, so stdout carries the path alone.
rust_gate_engine_build() {
	local work=$1
	if [ -n "${ENGINE_BUILD:-}" ]; then
		[ -x "$ENGINE_BUILD/bin/glhexen2" ] || {
			echo "error: ENGINE_BUILD=$ENGINE_BUILD has no bin/glhexen2" >&2
			return 1
		}
		printf '%s\n' "$ENGINE_BUILD"
		return 0
	fi
	local root
	root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
	if ! cmake -B "$work/build" -S "$root/engine" \
			-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >"$work/build.log" 2>&1 ||
		! cmake --build "$work/build" -j"$(nproc)" >>"$work/build.log" 2>&1; then
		echo "error: engine build failed; see $work/build.log" >&2
		tail -30 "$work/build.log" >&2
		return 1
	fi
	printf '%s\n' "$work/build"
}

# rust_gate_reference_bin
#
# The --engine smoke compares the engine against a C-only reference binary.
# Rust is linked into every build now, so this tree cannot produce one.  Build
# it from a revision that still had the switches -- cc86c3ea9 is the last --
# with every one of them off (its default build already linked Rust):
#   git worktree add /tmp/c-ref cc86c3ea9
#   cmake -B /tmp/c-ref/build -S /tmp/c-ref/engine -DBUILD_DEDICATED=ON \
#     -DUSE_RUST_HASHINDEX=OFF -DUSE_MATHLIB_RS=OFF -DUSE_SIZEBUF_RS=OFF \
#     -DUSE_CRC_RS=OFF -DUSE_LINK_OPS_RS=OFF -DUSE_MSG_IO_RS=OFF \
#     -DUSE_INFO_STR_RS=OFF -DUSE_STRLCPY_RS=OFF -DUSE_STRLCAT_RS=OFF \
#     -DUSE_HUFFMAN_RS=OFF -DUSE_WAD_RS=OFF
#   cmake --build /tmp/c-ref/build -j"$(nproc)" --target glhexen2 h2ded
#   OFF_BIN=/tmp/c-ref/build/bin/glhexen2 ./scripts/check-rust-hashindex.sh --engine
rust_gate_reference_bin() {
	if [ -z "${OFF_BIN:-}" ] || [ ! -x "$OFF_BIN" ]; then
		echo "error: --engine needs OFF_BIN=<a C-only glhexen2> to compare against;" >&2
		echo "       see rust_gate_reference_bin in scripts/lib/rust-gate.sh" >&2
		return 1
	fi
	# run_engine_smoke.sh also runs the h2ded next to each glhexen2.
	[ -x "$(dirname "$OFF_BIN")/h2ded" ] || {
		echo "error: no h2ded next to OFF_BIN; build the reference with" >&2
		echo "       -DBUILD_DEDICATED=ON (see scripts/lib/rust-gate.sh)" >&2
		return 1
	}
	printf '%s\n' "$OFF_BIN"
}
