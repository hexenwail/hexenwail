#!/usr/bin/env bash
#
# check-rust-sv-main.sh -- gate for the Rust ports out of the server frame,
# engine/hexen2/sv_main.c (#469, step 6 of #455).  Slice A: the sound and
# particle messages and the per-protocol limits (SV_StartParticle..4,
# SV_StopSound, SV_UpdateSoundPos, SV_StartSound, SV_MaxSounds,
# SV_MaxDatagram, SV_MaxReliableMessage; sv_main_sound.rs).  Slice B: the
# entity updates (the fat PVS, SV_EntityUpdateSize, SV_EntityPriority,
# SV_PrepareClientEntities, SV_CleanupEnts; sv_main_ents.rs).  Slice C: the
# client data and datagrams (SV_ClearDatagram, SV_WriteClientdataToMessage,
# SV_SendClientDatagram, SV_UpdateToReliableMessages, SV_SendNop,
# SV_SendClientMessages; sv_main_datagram.rs).  Slice D: the connect and
# signon functions (SV_ProtocolName, SV_SendServerinfo, SV_ConnectClient,
# SV_CheckForNewClients, SV_ModelIndex, SV_SignonBufferCount/Size,
# SV_ReserveSignonSpace, SV_SendSignonBuffer, SV_CreateBaseline,
# SV_SendReconnect; sv_main_connect.rs).  Hexen II only:
# HexenWorld's server has its own sv_main.c, and its own C message functions
# of the same names in sv_send.c, which multicast; it has no SV_Max*.
#
# The differential half runs each slice's harness against its C original:
# each compares the datagram the call writes, byte for byte, with its
# cursize and overflow flag, every console line and fatal call, and what
# else the call writes (return values; the client's reference frames, frame
# bytes, ClearCounts, effects and fat PVS); requires its outcome floors; and
# requires every line of the C original to have run under gcov.
#
# The CMake half proves the shared engine build links each exported symbol
# exactly once in glhexen2 and h2ded; that hwsv defines the message functions
# once, in its own sv_send.c, and the SV_Max* not at all; and that no Hexen
# II sv_main.c object still defines any of them.
#
# USAGE
#
#   ./scripts/check-rust-sv-main.sh
#
# Requires cc, gcov, cargo/rustc, cmake and nm -- run inside `nix develop`.
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
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"

sound_symbols=(SV_StartParticle SV_StartParticle2 SV_StartParticle3 SV_StartParticle4
	SV_StopSound SV_UpdateSoundPos SV_StartSound SV_MaxSounds SV_MaxDatagram
	SV_MaxReliableMessage)
# exported for sv_main.c's SV_SendClientDatagram and SV_SendClientMessages;
# Hexen II's alone, hwsv's entity updates being its sv_ents.c
ents_symbols=(SV_PrepareClientEntities SV_CleanupEnts)
# slice D: the connect and signon functions; SV_ProtocolName, SV_SendServerinfo,
# SV_CreateBaseline and SV_SendReconnect stay C-visible for the still-C callers
connect_symbols=(SV_ProtocolName SV_SendServerinfo SV_CheckForNewClients SV_ModelIndex
	SV_ReserveSignonSpace SV_SendSignonBuffer SV_CreateBaseline SV_SendReconnect)

for tool in cc gcov cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

step=1
for port in sound ents datagram connect; do
	case "$port" in
		sound) feature=sv_main_sound; name=sv_main_sound; runner=run_diff_harness.sh; floor_digits=6 ;;
		ents) feature=sv_main_ents; name=sv_main_ents; runner=run_ents_harness.sh; floor_digits=5 ;;
		datagram) feature=sv_main_datagram; name=sv_main_datagram; runner=run_datagram_harness.sh; floor_digits=5 ;;
		connect) feature=sv_main_connect; name=sv_main_connect; runner=run_connect_harness.sh; floor_digits=6 ;;
	esac
	echo "== $step. build the crate with $feature =="
	step=$((step + 1))
	cargo build --release --offline \
		--manifest-path "$crate/Cargo.toml" \
		--target-dir "$work/target-$port" \
		--features "$feature"
	lib="$work/target-$port/release/libengine_rs.a"

	echo
	echo "== $step. $name differential harness, Rust vs the C original =="
	step=$((step + 1))
	log="$work/diff-$port.log"
	set +e
	VARIANT=h2 RUST_LIB="$lib" OUT="$work/diff-$port" \
		bash "$crate/sv_main/tests/$runner" >"$log" 2>&1
	status=$?
	set -e
	grep -E '^(layouts:|outcomes:|          [0-9A-Za-z-])|^sv_main_[a-z]+ differential harness|^coverage:|^RESULT:' "$log" || true
	if [ "$status" -ne 0 ] || ! grep -q '^RESULT: PASS$' "$log"; then
		echo "FAIL: $name differential harness (exit $status)" >&2
		grep -E 'MISMATCH|COVERAGE|FAIL|#####|error|harness bug' "$log" >&2 || tail -40 "$log" >&2
		exit 1
	fi
	# RESULT alone would still pass with the enumeration cut to nothing, so
	# floor the case count.  Observed: 120,000 for the sound messages
	# (six digits); 12,000 for the entity updates, whose cases are whole
	# frames of up to 8300 edicts (five).
	count_line=$(grep -E "^$name differential harness \([a-z0-9]+\): PASS \(" "$log" || true)
	count=$(printf '%s\n' "$count_line" | sed -n 's/.*PASS (\([0-9]*\) cases.*/\1/p')
	if [ "${#count}" -lt "$floor_digits" ]; then
		echo "FAIL: $name harness case count is below its $floor_digits-digit floor" >&2
		echo "      observed: ${count_line:-<no PASS line>}" >&2
		exit 1
	fi
	echo
done

echo "== $step. the engine build: glhexen2, h2ded and hwsv =="
step=$((step + 1))
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== $step. one definition in each binary, none in sv_main.c =="
for bin in glhexen2 h2ded hwsv; do
	path="$engine_build/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${sound_symbols[@]}"; do
		want=1
		# hwsv: its own sv_send.c message functions, and no SV_Max*
		[ "$bin" = hwsv ] && case "$sym" in SV_Max*) want=0 ;; esac
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne "$want" ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected $want" >&2
			exit 1
		fi
	done
	for sym in "${ents_symbols[@]}"; do
		if [ "$bin" = hwsv ]; then want=0; else want=1; fi
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne "$want" ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected $want" >&2
			exit 1
		fi
	done
	for sym in "${connect_symbols[@]}"; do
		if [ "$bin" = hwsv ]; then want=0; else want=1; fi
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne "$want" ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected $want" >&2
			exit 1
		fi
	done
	echo "  $bin: each defined once$([ "$bin" = hwsv ] && echo ' (HexenWorld'"'"'s sv_send.c), SV_Max* and the entity updates absent')"
done
# hwsv's message functions must be its sv_send.c's, not the Rust
objects=$(find "$engine_build" -path '*hexenworld*' \( -name 'sv_send.c.o' -o -name 'sv_send.c.obj' \) | sort)
[ -n "$objects" ] || { echo "FAIL: no HexenWorld sv_send.c object in the build" >&2; exit 1; }
while IFS= read -r obj; do
	nm "$obj" | grep -qE " T SV_StartParticle4\$" || {
		echo "FAIL: ${obj#"$engine_build"/} does not define SV_StartParticle4; hwsv's message functions moved" >&2
		exit 1
	}
done <<<"$objects"
# Every Hexen II sv_main.c object -- the client's and h2ded's -- must define
# none of them now.
objects=$(find "$engine_build" -path '*hexen2*' \( -name 'sv_main.c.o' -o -name 'sv_main.c.obj' \) | sort)
nobj=$(printf '%s\n' "$objects" | grep -c . || true)
if [ "$nobj" -lt 2 ]; then
	echo "FAIL: expected a Hexen II sv_main.c object per target (2), found $nobj" >&2
	exit 1
fi
while IFS= read -r obj; do
	for sym in "${sound_symbols[@]}" SV_SoundEntityFits "${ents_symbols[@]}" \
		SV_AddToFatPVS SV_FatPVS SV_EntityUpdateSize SV_EntityPriority \
		"${connect_symbols[@]}"; do
		if nm "$obj" | grep -qE " [Tt] $sym\$"; then
			echo "FAIL: ${obj#"$engine_build"/} still defines $sym" >&2
			exit 1
		fi
	done
done <<<"$objects"
echo "  $nobj Hexen II sv_main.c object(s): none defines the ported functions"
stray=$(find "$engine_build" -name 'sv_main_sound_*.c.o' -o -name 'sv_main_ents_*.c.o' -o -name 'sv_main_datagram_*.c.o' -o -name 'sv_main_connect_*.c.o' -o -name 'c_ref_ents.c.o' -o -name 'c_ref_connect.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray harness fixture object(s) in the engine build" >&2
	exit 1
}

echo "PASS: Rust sv_main.c ports (the sound and particle messages and protocol"
echo "      limits, the entity updates) -- each harness agrees with its C original,"
echo "      every line of it ran, each Hexen II binary links each symbol once, hwsv"
echo "      only its own, and sv_main.c defines none of them."
