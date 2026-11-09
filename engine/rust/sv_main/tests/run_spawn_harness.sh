#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_main spawn differential harness (E2 / slice E of #469) and run
# it: SV_SpawnServer and the map_checks readout, Rust against the C original.
#
#   VARIANT=h2  sv_main_spawn_h2.c (hexen2/sv_main.c's), against an archive
#               built with sv_main_spawn.  Hexen II only: hwsv's
#               SV_SpawnServer is HexenWorld's own sv_init.c.
#
# The Rust is one archive linked into every Hexen II target, and the C it
# replaces has `#if !defined(SERVERONLY)` and `#ifdef GLQUAKE` arms, and a
# texture_t whose layout is each client renderer's own.  So the harness is
# built three times, once per way the engine compiles sv_main.c --
#
#   serveronly  -DSERVERONLY             h2ded
#   glquake     -DGLQUAKE -DSDLQUAKE     hexenwail, the GLES and WebGL2 clients
#   websoft     -DWEBQUAKE -DWEBSOFT     the web software renderer
#
# -- each compiling the C original, engine/rust/sv_main_target.c and
# engine/rust/sv_main_variadic.c that way, and each linked against the same
# RUST_LIB, so the one archive is held to all three.  The C goes in as
# plain IEEE C at -O2, and the glquake build is repeated with --coverage:
# it compiles every line of the fixture (the serveronly build compiles a
# subset, the websoft build the same lines as glquake), and every one of them
# must run.  All runs are under a timeout.
#
# --gc-sections: sv_main_target.c is linked whole and carries the other
# slices' accessors too; the harness defines only the globals this slice's
# accessors read, and the linker drops the rest unreferenced.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

VARIANT="${VARIANT:-h2}"
RUST_LIB="${RUST_LIB:?set RUST_LIB to the archive built with sv_main_spawn}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
[ -f "$RUST_LIB" ] || { echo "error: Rust staticlib not found: $RUST_LIB" >&2; exit 2; }
[ "$VARIANT" = h2 ] || { echo "error: VARIANT must be h2 (this slice is Hexen II's alone)" >&2; exit 2; }

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
fixture=sv_main_spawn_h2.c
WARN=(-Wall -Wno-unused-function -Wno-comment)
SECT=(-ffunction-sections -fdata-sections)
LIMIT=${HARNESS_TIMEOUT:-900}

config_defs() { # config
	case "$1" in
		serveronly) echo "-DSERVERONLY" ;;
		glquake) echo "-DGLQUAKE -DSDLQUAKE" ;;
		websoft) echo "-DWEBQUAKE -DWEBSOFT -DSDLQUAKE" ;;
	esac
}

build() { # dir config ref-cflags...
	local dir=$1 config=$2; shift 2
	local defs
	read -r -a defs <<<"$(config_defs "$config")"
	mkdir -p "$OUT/$dir"
	# HARNESS_REUSE_OBJECTS=1 (spawn_mutants.sh): the C objects do not
	# depend on the Rust, so a mutant only needs the link
	if [ "${HARNESS_REUSE_OBJECTS:-0}" != 1 ] || [ ! -f "$OUT/$dir/qsnprint.o" ]; then
		cc "${INCS[@]}" "${defs[@]}" "${WARN[@]}" "${SECT[@]}" "$@" \
			-DSV_SpawnServer=c_SV_SpawnServer \
			-DSV_FIXTURE="\"$here/$fixture\"" \
			-c "$here/c_ref_spawn.c" -o "$OUT/$dir/c_ref.o"
		local src
		for src in "$rust_root/sv_main_target.c" "$rust_root/sv_main_variadic.c" \
			"$here/spawn_harness.c" "$root/common/strlcpy.c" "$root/common/qsnprint.c"; do
			cc "${INCS[@]}" "${defs[@]}" "${WARN[@]}" "${SECT[@]}" -O2 -g \
				-c "$src" -o "$OUT/$dir/$(basename "$src" .c).o"
		done
	fi
	cc "$@" -Wl,--gc-sections -o "$OUT/$dir/spawn_harness" \
		"$OUT/$dir/spawn_harness.o" "$OUT/$dir/c_ref.o" \
		"$OUT/$dir/sv_main_target.o" "$OUT/$dir/sv_main_variadic.o" \
		"$OUT/$dir/strlcpy.o" "$OUT/$dir/qsnprint.o" "$RUST_LIB" -lm
}

run() { # dir
	local status=0
	(cd "$OUT/$1" && timeout -k 10 "$LIMIT" ./spawn_harness) || status=$?
	if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
		echo "FAIL: the harness hung (timeout ${LIMIT}s)" >&2
	fi
	return "$status"
}

# The total case count is mostly checklist, title and thresh cases, which
# would meet the gate's floor with no SV_SpawnServer case at all, and gcov
# runs on one config and sees lines, not the false arm of a bare `if`.  So
# every config floors its own spawn cases, each outcome the generator aims
# at, and both arms of each branch the harness counts.
SPAWN_FLOOR=${SPAWN_FLOOR:-1000}
floors() { # config log
	local config=$1 log=$2 line spawn k v arms name t f bad=0
	line=$(grep -m1 '^outcomes: spawn ' "$log" || true)
	spawn=$(sed -n 's/^outcomes: spawn \([0-9]*\) .*/\1/p' <<<"$line")
	if [ -z "$spawn" ] || [ "$spawn" -lt "$SPAWN_FLOOR" ]; then
		echo "FAIL: $config ran ${spawn:-no} SV_SpawnServer cases (floor $SPAWN_FLOOR)" >&2
		bad=1
	fi
	for k in fatal "world missing" changelevel "checklist printed" "upgraded to 21" "to 100"; do
		v=$(sed -n "s/.*[(,] *$k \([0-9]*\)[,)].*/\1/p" <<<"$line")
		if [ -z "$v" ] || [ "$v" -lt 1 ]; then
			echo "FAIL: $config outcome '$k' is ${v:-missing}; the generator must reach it" >&2
			bad=1
		fi
	done
	arms=$(grep -m1 '^ *arms (taken/not):' "$log" | sed 's/^ *arms (taken\/not)://' || true)
	[ -n "$arms" ] || { echo "FAIL: $config printed no arm counts" >&2; bad=1; }
	# shellcheck disable=SC2086  # word pairs: name taken/not
	set -- $arms
	while [ $# -ge 2 ]; do
		name=$1; t=${2%/*}; f=${2#*/}; shift 2
		if [ "$t" -lt 1 ] || [ "$f" -lt 1 ]; then
			echo "FAIL: $config branch '$name' ran $t taken / $f not taken; both must run" >&2
			bad=1
		fi
	done
	[ "$bad" -eq 0 ] || exit 1
	echo "floors: $config -- $spawn spawn cases, every outcome and both arms of every counted branch"
}

# SPAWN_CONFIGS and HARNESS_COVERAGE=0 are for spawn_mutants.sh, which
# needs a quick kill and not a coverage proof; the gate sets neither.
configs="${SPAWN_CONFIGS:-serveronly glquake websoft}"
total=0
for config in $configs; do
	echo "== $VARIANT/$config: the C original against the Rust archive =="
	build "$config" "$config" -O2 -g
	log="$OUT/$config.log"
	status=0
	run "$config" | tee "$log" || status=$?
	if [ "$status" -ne 0 ] || ! grep -q '^RESULT: PASS$' "$log"; then
		echo "FAIL: $config differential harness (exit $status)" >&2
		exit 1
	fi
	n=$(sed -n 's/^sv_main_spawn [a-z]* config: PASS (\([0-9]*\) cases)$/\1/p' "$log")
	total=$((total + n))
	floors "$config" "$log"
done

if [ "${HARNESS_COVERAGE:-1}" = 0 ]; then
	echo "coverage: not run (HARNESS_COVERAGE=0)"
	echo "RESULT: PASS"
	exit 0
fi

echo "== $VARIANT/glquake: the same run with C line coverage =="
build cov glquake -O0 -g --coverage
# The children use exit(), not _exit(), so libgcov's exit handler dumps and
# merges each child's coverage.
(cd "$OUT/cov" && HARNESS_SCALE=1 timeout -k 10 "$LIMIT" ./spawn_harness >/dev/null 2>&1) || {
	echo "FAIL: the coverage run did not pass" >&2
	exit 1
}
(cd "$OUT/cov" && gcov -o . c_ref.o >/dev/null 2>&1) || true
gcov_file="$OUT/cov/$fixture.gcov"
[ -f "$gcov_file" ] || { echo "FAIL: gcov produced no $fixture.gcov" >&2; ls "$OUT/cov" >&2; exit 1; }
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
echo "coverage: every executable line of $fixture ran ($lines)"

echo "sv_main_spawn differential harness (h2): PASS ($total cases)"
echo "RESULT: PASS"
