#!/usr/bin/env bash
#
# headless-cfg.sh — Generate a config that plays a scripted run and dumps edicts.
#
# Produces a .cfg that starts a map, waits, throws an item, and dumps the edict
# list at several points, bracketing each dump with a `ZZZ<TAG>` marker line so
# tools/edict_pick.py can split the log back into sections.  Everything is
# driven by `wait` aliases rather than key events, so it needs no X server --
# use it with `-condebug` and a dedicated/headless run.
#
# Usage:
#   headless-cfg.sh <class> <map> <outfile> [impulse]
#
# Example:
#   ./tools/headless-cfg.sh 2 demo1 /tmp/run.cfg 108
#   glhexen2 -basedir ~/hexen2 -condebug +exec /tmp/run.cfg > /tmp/run.log
#   ./tools/edict_pick.py /tmp/run.log player,timebomb
#
# <class> is the playerclass number (1 paladin, 2 crusader, 3 necromancer,
# 4 assassin, 5 demoness).  [impulse] is the item to throw, default 108.
#
set -uo pipefail

[ $# -ge 3 ] || { sed -n '2,20p' "$0"; exit 2; }

CLASS=$1; MAP=$2; OUT=$3; IMP=${4:-108}

# `wait` is one frame, so the w5/w25/w125 ladder is how you get a multi-second
# delay out of a cfg -- there is no sleep command in the console.
cat > "$OUT" <<EOF
developer 1
skill 1
deathmatch 0
coop 0
playerclass $CLASS
map $MAP
alias w5 "wait;wait;wait;wait;wait"
alias w25 "w5;w5;w5;w5;w5"
alias w125 "w25;w25;w25;w25;w25"
alias d3 "echo ZZZDUMP3; edicts; echo ZZZEND; quit"
alias d2 "echo ZZZDUMP2; edicts; w25; d3"
alias d1 "echo ZZZDUMP1; edicts; w5; d2"
alias thr "echo ZZZTHROW; impulse $IMP; wait; wait; d1"
alias pre "echo ZZZPRE; edicts; w5; thr"
alias go "w125; w125; god; echo ZZZCHEAT; impulse 43; w25; pre"
go
EOF

echo "wrote $OUT (class $CLASS, map $MAP, impulse $IMP)"
