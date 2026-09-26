#!/usr/bin/env bash
#
# monster-walk-eval.sh -- differential monster-movement eval for two h2ded builds.
#
# WHAT IT IS FOR
#
# SV_CheckBottom and SV_movestep decide whether a monster takes a step.  A
# change there is not covered by any gate test that can link them (see
# engine/tests/sv_checkbottom_test.c for why), and it is exactly the kind of
# change that silently alters monster pathing on maps nobody thought about.
# This is the eval lane for it: run the same maps under two builds and diff the
# decisions.
#
# It exploits three properties of the dedicated server:
#   * ambient wanderers (village1's player_sheep, and friends) call walkmove
#     with no client connected, so monster movement runs without a player;
#   * nothing in this engine calls srand(), so rand() replays identically;
#   * sv_debugmovestep logs every refused step with the reason, the entity and
#     its position.
# So the Nth refused step is the same decision in both runs, and the two logs
# agree exactly for as long as both ran.  Wall-clock `wait` means the two runs
# execute a different NUMBER of ticks, so only the common prefix is compared --
# a difference inside it is a real behavioural difference, not scheduling.
#
# WHAT IT CANNOT SEE, because this trips people up
#
# sv_debugmovestep logs REFUSED steps only, so "0 steps" means no step was
# refused -- NOT that no monster moved and NOT that SV_CheckBottom was never
# reached.  Measured with a temporary probe on 2026-09-26: village1 logs 532
# refusals and calls SV_CheckBottom 271 times, while village3 logs zero
# refusals and still calls it 331 times.  A map reporting "no refused steps"
# under both builds is therefore weak evidence, not strong: it says the two
# builds refused the same nothing.  To see call-level behaviour you have to
# instrument SV_CheckBottom itself -- issue #282 proposes doing that behind the
# existing cvar so the next investigation does not need a throwaway build.
#
# It also needs an idle machine.  Map load has to finish inside SETTLE, and
# under a parallel nix build it does not: the same sweep that reports 532
# refusals on village1 idle reports 0 while two builds are running, which reads
# as "no refused steps" and is simply a lost run.
#
# USAGE
#   tools/monster-walk-eval.sh <outdir> <basedir> <h2dedA> <h2dedB> [map...]
#
# With no maps it reads every maps/*.bsp out of the basedir's paks.  Needs
# bwrap (via tools/serve.sh) and python3 for the pak directory.
#
#   nix build .#h2ded-bundled -o result-h2ded
#   tools/monster-walk-eval.sh /tmp/mw ~/hexen2 \
#       /nix/store/...-baseline/bin/h2ded ./result-h2ded/bin/h2ded
#
# OUTPUT
#   <outdir>/report.txt     per-map table, then the verdict
#   <outdir>/progress.log   timestamped progress, tail -f this
#   <outdir>/<map>.{a,b}    the sv_debugmovestep lines from each build
#   <outdir>/<map>.diff     present only where the common prefix differs
#
# Exit status is 0 whether or not the builds agree -- "they differ" is a result,
# not a failure.  1 means the eval itself could not run.  Read report.txt.
#
# Lives in tools/ rather than scripts/ on purpose: scripts/ is inside the
# flake's filteredSrc allowlist, so editing a file there invalidates every
# cached build, while tools/ is deliberately outside it.  This is a driver like
# serve.sh and headless-drive.sh, not something a build runs.
#
# Environment: SETTLE (seconds after map load, default 5), OBSERVE (seconds of
# logging, default 15), BUDGET (sv_debugmovestep line budget, default 8000).
#
# Do not shorten OBSERVE below about 12 seconds.  The wanderers this eval rides
# on take several seconds to get going, and village1 -- which reports 532 steps
# at the default -- reports zero at OBSERVE=8.  A too-short run looks exactly
# like "both builds agreed that nothing moved".
#
set -uo pipefail

usage() { sed -n '2,42p' "$0"; exit 2; }
[ $# -ge 4 ] || usage

OUT="$1"; BASEDIR="$2"; ENGINE_A="$3"; ENGINE_B="$4"; shift 4

SETTLE=${SETTLE:-5}
OBSERVE=${OBSERVE:-15}
BUDGET=${BUDGET:-8000}

HERE=$(cd "$(dirname "$0")/.." && pwd)
SERVE="$HERE/tools/serve.sh"

[ -x "$SERVE" ]     || { echo "eval: no tools/serve.sh at $SERVE" >&2; exit 1; }
[ -x "$ENGINE_A" ]  || { echo "eval: no engine A at $ENGINE_A" >&2; exit 1; }
[ -x "$ENGINE_B" ]  || { echo "eval: no engine B at $ENGINE_B" >&2; exit 1; }
[ -d "$BASEDIR" ]   || { echo "eval: no basedir at $BASEDIR" >&2; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT"
PROGRESS="$OUT/progress.log"
REPORT="$OUT/report.txt"

say() {
	printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "$PROGRESS"
}

# Map list: the arguments, or every maps/*.bsp in the basedir's paks.  Reading
# the pak directory beats a hardcoded list, which would go stale against the
# mission pack, a mod, or the free demo data.
if [ $# -gt 0 ]; then
	MAPS=("$@")
else
	mapfile -t MAPS < <(python3 - "$BASEDIR" <<'PY'
import glob, os, struct, sys

names = set()
for pak in sorted(glob.glob(os.path.join(sys.argv[1], '*', '*.pak'))):
    try:
        with open(pak, 'rb') as f:
            magic, off, length = struct.unpack('<4sii', f.read(12))
            if magic != b'PACK':
                continue
            f.seek(off)
            for _ in range(length // 64):
                entry = f.read(64)
                if len(entry) < 64:
                    break
                name = entry[:56].split(b'\0')[0].decode('latin1')
                if name.startswith('maps/') and name.endswith('.bsp'):
                    names.add(os.path.basename(name)[:-4])
    except (OSError, struct.error):
        continue
for name in sorted(names):
    print(name)
PY
)
fi

[ ${#MAPS[@]} -gt 0 ] || { echo "eval: no maps found in $BASEDIR" >&2; exit 1; }

say "monster-walk eval: ${#MAPS[@]} maps, settle=${SETTLE}s observe=${OBSERVE}s"
say "  A = $ENGINE_A"
say "  B = $ENGINE_B"
say "follow with: tail -f $PROGRESS"

# One map under one engine -> the sv_debugmovestep lines, in order.
run_one() {
	local engine="$1" map="$2" dest="$3" work
	work="$OUT/.work"
	printf 'map %s\nwait %s\nsv_debugmovestep %s\nwait %s\n' \
		"$map" "$SETTLE" "$BUDGET" "$OBSERVE" |
		ENGINE="$engine" TIMEOUT=$((SETTLE + OBSERVE + 60)) \
		"$SERVE" "$work" "$BASEDIR" >/dev/null 2>&1
	grep '^sv_debugmovestep' "$work/qconsole.log" 2>/dev/null > "$dest"
	# Did the map actually load?  A misspelled or mission-pack-only name
	# otherwise reads as "both builds agreed on zero movement", which is a pass
	# that proves nothing.  h2ded says "Couldn't spawn server maps/<name>.bsp".
	! grep -qi "couldn't spawn server\|couldn't load" "$work/qconsole.log" 2>/dev/null
}

printf '%-12s %8s %8s %8s %8s  %s\n' \
	map A_steps B_steps A_cbot B_cbot verdict > "$REPORT"

differ=0; missing=0; agreed=0; done_n=0
for map in "${MAPS[@]}"; do
	done_n=$((done_n + 1))
	a="$OUT/$map.a"; b="$OUT/$map.b"
	ok=0
	run_one "$ENGINE_A" "$map" "$a" || ok=1
	run_one "$ENGINE_B" "$map" "$b" || ok=1

	na=$(wc -l < "$a"); nb=$(wc -l < "$b")
	ca=$(grep -c 'CheckBottom' "$a"); cb=$(grep -c 'CheckBottom' "$b")

	if [ "$ok" != 0 ]; then
		verdict="MAP NOT LOADED"
		missing=$((missing + 1))
	elif [ "$na" = 0 ] && [ "$nb" = 0 ]; then
		# Not "nothing moved" -- nothing was REFUSED.  See the header.
		verdict="no refused steps (weak)"
		agreed=$((agreed + 1))
	else
		# Common prefix only; see the header for why the totals may differ.
		n=$(( na < nb ? na : nb ))
		if diff -u <(head -n "$n" "$a") <(head -n "$n" "$b") > "$OUT/$map.diff"; then
			rm -f "$OUT/$map.diff"
			verdict="identical (${n} steps compared)"
			agreed=$((agreed + 1))
		else
			verdict="DIFFERS -- see $map.diff"
			differ=$((differ + 1))
		fi
	fi

	printf '%-12s %8s %8s %8s %8s  %s\n' \
		"$map" "$na" "$nb" "$ca" "$cb" "$verdict" >> "$REPORT"
	say "[$done_n/${#MAPS[@]}] $map: A=$na B=$nb cbot=$ca/$cb -- $verdict"
done

rm -rf "$OUT/.work"

{
	echo
	echo "maps compared:        ${#MAPS[@]}"
	echo "agreed:               $agreed"
	echo "differed:             $differ"
	echo "could not be loaded:  $missing"
} >> "$REPORT"

say "done: $agreed agreed, $differ differed, $missing unloadable -- $REPORT"
exit 0
