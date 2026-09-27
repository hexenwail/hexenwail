#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# check-rust-quakefs.sh -- gate for the Rust port of engine/h2shared/quakefs.c.
#
# THE DIFFERENTIAL HALF is engine/rust/quakefs/tests/diff_harness.c, and it does
# not look like the other ports' harnesses: quakefs.c exports seventy-odd
# symbols, so instead of a vtable over both implementations in one binary, the
# same harness source is compiled twice -- once with -DIMPL=c_ against the
# renamed C original, once bare against the Rust module -- over the SAME Rust
# substrate, and the two traces are compared byte for byte.  See the harness
# header for the design and for what it deliberately does not cover.
#
# THE ARMS.  client and serveronly carry cases and are floored.  h2w is
# LINK-ONLY: it must build and link -- which is what proves the shim's H2W arm
# and the port's H2W references resolve, and is where the Host_Error/SV_Error
# answer is tested -- but its cases cannot run here because hwsv's FS_Init wants
# hw/pak4.pak and no HexenWorld data ships in this tree.  A variant that links
# and cannot run is evidence; a skipped variant is not, so the arm is built and
# its data gap named in the output, and a link failure fails the gate.
#
# THE STRUCTURAL COMPARISON is the check that caught an invented entry in the
# cmd port: every table quakefs.c carries -- the known-pak lists, the content
# marks, the CRC popcount table, the sky lists and the command registrations --
# is extracted from the C and from the Rust and compared entry for entry, in
# order.  Nothing is re-typed by hand here.
#
# THE BENCHMARK DECISION: none, and this is where it is recorded rather than
# omitted.  The port's own per-call work is list traversal and path formatting;
# the cost of the file layer is the syscalls underneath it, which the port does
# not change, so a two-implementation micro-benchmark would measure the
# filesystem rather than the port.  The zone port's allocator benchmark is the
# shape this would take if that ever stops being true.
#
# Requires cc, cargo, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,45p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"

# The substrate the harness links, so the comparison isolates quakefs.
HARNESS_FEATURES=quakefs,zone,hashindex,cvar,cmd,info_str,sizebuf,crc,strlcpy,strlcat

# Floors, recorded beside the numbers the arms actually produce.  The floor is
# one decimal place below the observation, so widening the enumeration does not
# mean editing the gate.
# Observed: client 7 cases / 7893 trace bytes; serveronly 7 cases / 9167.
# (The numbers are re-recorded whenever the harness changes shape; the floors
# below are what the gate enforces, and they are one step under these.)
FLOOR_CASES=6
FLOOR_BYTES=5000

for tool in cc cargo cmake nm python3; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the crate with the harness's substrate =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features "$HARNESS_FEATURES"

echo
echo "== 2. differential harness: the C original and the Rust module, per arm =="
for arm in client serveronly; do
	log="$work/arm-$arm.log"
	set +e
	RUST_LIB="$crate/target/release/libengine_rs.a" OUT="$work/arm-$arm" \
		bash "$crate/quakefs/tests/run_diff_harness.sh" "$arm" >"$log" 2>&1
	status=$?
	set -e
	grep -E 'case |RESULT:|agree|differential harness' "$log" | sed 's/^/  /' || true
	if [ "$status" -ne 0 ]; then
		echo "FAIL: the $arm arm exited $status" >&2
		cat "$log" >&2
		exit 1
	fi
	cases=$(grep -oE '^case [a-z-]+' "$log" | sort -u | wc -l)
	trace_bytes=$(stat -c%s "$work/arm-$arm/rust.trace" 2>/dev/null || echo 0)
	if [ "$cases" -lt "$FLOOR_CASES" ]; then
		echo "FAIL: $arm ran $cases cases, below the floor of $FLOOR_CASES" >&2
		exit 1
	fi
	if [ "$trace_bytes" -lt "$FLOOR_BYTES" ]; then
		echo "FAIL: $arm compared $trace_bytes trace bytes, below the floor of $FLOOR_BYTES" >&2
		exit 1
	fi
	echo "  $arm: $cases cases, $trace_bytes trace bytes compared (floors $FLOOR_CASES/$FLOOR_BYTES)"
done

echo
echo "== 3. the h2w arm: linked, not skipped =="
log="$work/arm-h2w.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" OUT="$work/arm-h2w" \
	bash "$crate/quakefs/tests/run_diff_harness.sh" h2w --link-only >"$log" 2>&1
status=$?
set -e
grep -E 'LINK-ONLY|RESULT:' "$log" | sed 's/^/  /' || true
if [ "$status" -ne 0 ]; then
	echo "FAIL: the h2w arm did not link -- this is where the H2W references" >&2
	echo "      (Host_Error/SV_Error and the shim's H2W arm) are tested" >&2
	cat "$log" >&2
	exit 1
fi

echo
echo "== 4. the tables match the C's, entry for entry =="
python3 - "$root" <<'PY'
import re, sys
root = sys.argv[1]
c = open(f"{root}/engine/h2shared/quakefs.c").read()
r = open(f"{root}/engine/rust/src/quakefs.rs").read()

def block(text, start_pat, end_pat=r'\n[ \t]*\};|\n[ \t]*\];'):
    m = re.search(start_pat, text)
    if not m:
        return None
    rest = text[m.start():]
    e = re.search(end_pat, rest)
    return rest[:e.end()] if e else rest

def c_strings(seg):
    return re.findall(r'"([^"]*)"', seg) if seg else []

def r_strings(seg):
    # The Rust spellings are CStr/byte-string literals: b"rt\0" IS the C string
    # "rt", so the trailing NUL is stripped before comparing.
    out = []
    for lit in re.findall(r'(?:c|b)?"([^"]*)"', seg) if seg else []:
        out.append(lit[:-2] if lit.endswith('\\0') else lit)
    return out

def c_numbers(seg):
    # The C's pop[] is written in hex (0x6600), so decimal-only matching sees
    # nothing -- normalise both sides through int(x, 0).
    return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', seg)] if seg else []

def r_numbers(seg):
    return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', seg)] if seg else []

checks = [
    ("pakdata[]",          r'static pakdata_t pakdata\[',        r'static PAKDATA:'),
    ("demo_pakdata[]",     r'static pakdata_t demo_pakdata\[\]',  r'static DEMO_PAKDATA:'),
    ("oem0_pakdata[]",     r'static pakdata_t oem0_pakdata\[\]',  r'static OEM0_PAKDATA:'),
    ("old_pakdata[]",      r'static pakdata_t old_pakdata\[\]',   r'static OLD_PAKDATA:'),
    ("mark_data1_pak0[]",  r'static const contentmark_t mark_data1_pak0\[\]', r'static MARK_DATA1_PAK0:'),
    ("mark_data1_pak1[]",  r'static const contentmark_t mark_data1_pak1\[\]', r'static MARK_DATA1_PAK1:'),
    ("mark_portals[]",     r'static const contentmark_t mark_portals\[\]',    r'static MARK_PORTALS:'),
]
bad = 0
for name, cpat, rpat in checks:
    cs = c_strings(block(c, cpat))
    rs = r_strings(block(r, rpat))
    if cs != rs:
        bad += 1
        print(f"FAIL: {name} differs (C {len(cs)} entries, Rust {len(rs)})", file=sys.stderr)
        for i, (a, b) in enumerate(zip(cs, rs)):
            if a != b:
                print(f"      first difference at entry {i}: C {a!r} vs Rust {b!r}", file=sys.stderr)
                break
        if len(cs) != len(rs):
            print(f"      C: {cs}\n      Rust: {rs}", file=sys.stderr)
    else:
        print(f"  {name}: {len(cs)} entries match")

# pop[] is numbers, not strings.
cp = c_numbers(block(c, r'static const unsigned short\s+pop\[\]'))
# From the '= [' onward: the declaration's own `[u16; 128]` would otherwise
# contribute a 129th number.
_pop = block(r, r'static POP:')
rp = r_numbers(_pop[_pop.index('= [') + 3:] if '= [' in _pop else _pop)
if cp != rp:
    bad += 1
    print(f"FAIL: pop[] differs (C {len(cp)} values, Rust {len(rp)})", file=sys.stderr)
else:
    print(f"  pop[]: {len(cp)} values match")

# The sky lists: single-line arrays on both sides, so they are read from their
# own line rather than through block(), whose close pattern assumes the brace
# starts a line.
def one_line(text, pat):
    m = re.search(pat + r'[^\n]*', text)
    return m.group(0) if m else ''

c_sky = (c_strings(one_line(c, r'static const char \*const skyfaces\[6\]')) +
         c_strings(one_line(c, r'static const char \*const skyexts\[3\]')) +
         c_strings(one_line(c, r'static const char \*const skydirs\[2\]')))
r_sky = (r_strings(one_line(r, r'static SKYFACES:')) +
         r_strings(one_line(r, r'static SKYEXTS:')) +
         r_strings(one_line(r, r'static SKYDIRS:')))
if c_sky != r_sky:
    bad += 1
    print(f"FAIL: the sky lists differ\n      C: {c_sky}\n      Rust: {r_sky}", file=sys.stderr)
else:
    print(f"  sky lists: {len(c_sky)} entries match")

# The commands FS_Init registers, in order.
c_reg = re.findall(r'Cmd_AddCommand \("([^"]+)"', c)
r_reg = re.findall(r'Cmd_AddCommand\(c"([^"]+)"', r)
if c_reg != r_reg:
    bad += 1
    print(f"FAIL: the registered commands differ\n      C: {c_reg}\n      Rust: {r_reg}", file=sys.stderr)
else:
    print(f"  registered commands: {len(c_reg)} names match, in order")

sys.exit(1 if bad else 0)
PY

echo
echo "== 5. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 6. the archive exports the whole ABI exactly once =="
archive="$engine_build/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
symbols=()
while read -r sym; do
	[ -n "$sym" ] && symbols+=("$sym")
done < <(grep -oE '^(const char|char|void|int|long|qboolean|size_t|byte|unsigned int|fshandle_t|FILE) +\**([A-Za-z_][A-Za-z0-9_]*) *\(' \
	"$root/engine/h2shared/quakefs.h" | grep -oE '[A-Za-z_][A-Za-z0-9_]* *\($' | tr -d ' (' | sort -u \
	| grep -vE '^(FS_MakePath_VA|FS_MakePath_VABUF|FS_ResolveCasePath)$')
# The variadic and dirent entry points come from the C shim
# (engine/rust/quakefs_variadic.c), which is compiled per target, so they are
# checked in the binaries below and not in the archive.
shim_binary_symbols=(FS_MakePath_VA FS_MakePath_VABUF FS_ResolveCasePath QuakeFS_TargetHostError)
globals=(fs_gamedir_nopath gameflags fs_filesize file_from_pak oem registered)
for sym in "${symbols[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T ${sym}\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
for sym in "${globals[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " [BbDd] ${sym}\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#symbols[@]}/${#symbols[@]} FS functions and ${#globals[@]}/${#globals[@]} globals once"

echo
echo "== 7. exactly one quakefs definition, and the shim symbols, in every target =="
targets=(glhexen2 h2ded hwsv)
predicates=(QuakeFS_TargetIsH2W QuakeFS_TargetIsH2WIntegrated QuakeFS_TargetHasClientCommands QuakeFS_TargetClientReset QuakeFS_TargetClientClearState QuakeFS_TargetClientReinit QuakeFS_TargetVidLock QuakeFS_TargetShowList QuakeFS_TargetSetHwServerinfo QuakeFS_TargetHostError Zone_TargetDefSize Zone_TargetSecSize Zone_TargetHasCache Zone_TargetDedicated Cmd_TargetHasClientLists Cmd_TargetIsH2W Cmd_TargetHasBuiltinStartupScript)
for bin in "${targets[@]}"; do
	path="$engine_build/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] ${sym}\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	for sym in "${globals[@]}"; do
		n=$(nm "$path" | grep -cE " [BbDd] ${sym}\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	for sym in "${predicates[@]}" "${shim_binary_symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] ${sym}\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	echo "  $bin: ${#symbols[@]}/${#symbols[@]} FS functions, ${#globals[@]}/${#globals[@]} globals, ${#predicates[@]}/${#predicates[@]} shim predicates and ${#shim_binary_symbols[@]}/${#shim_binary_symbols[@]} variadic/dirent entries exactly once"
done
stray=$(find "$engine_build" -name 'quakefs.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray quakefs.c object(s) in the engine build" >&2
	exit 1
}

echo
echo "PASS: Rust quakefs -- the C original and the Rust module agree byte for byte"
echo "      on the client and SERVERONLY arms, the H2W arm links, every table"
echo "      matches the C entry for entry, all three targets link exactly one"
echo "      definition of every symbol, and no quakefs.c object is left."
