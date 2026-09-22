#!/usr/bin/env bash
#
# wasm-boot-smoke.sh -- boot the real WebAssembly client in headless Chrome
# and check how far it gets.
#
# Usage: wasm-boot-smoke.sh BIN_DIR [DATA_DIR]
#
#   BIN_DIR   the build's bin/ directory (hexenwail.js + hexenwail.wasm), as
#             scripts/wasm-build.sh leaves it.
#   DATA_DIR  optional: a Hexen II basedir holding data1/pak0.pak (the demo is
#             enough).  Its files are written into the runtime's /persistent,
#             the directory the PWA gives the engine, and the engine is asked
#             to load demo1.
#
# Without data this is the check CI can run: no game data may go on a runner,
# so it asserts the engine dies at FS_Init's data gate with the exact
# diagnostic -- which means it got through Memory_Init, Cbuf_Init, Cmd_Init
# and COM_Init first, on the Rust sizebuf and strlcpy.  With data, a map load
# runs every port the engine links: crc (progs.dat), hashindex (pic and
# texture caches), mathlib and link_ops (the world), and sizebuf/msg_io (the
# local client/server loop).  Everything else under web/test and
# webgl-smoke-test.sh checks shaders and the PWA shell; this is the only
# lane that executes hexenwail.wasm.
#
# Requires Chrome or Chromium and python3.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

# Same reasoning as webgl-smoke-test.sh: sized for a starved runner, not for
# the work, which takes seconds.
readonly BROWSER_TIMEOUT=180

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
	sed -n '2,/^# SPDX/p' "$0" >&2
	exit 2
fi
bin_dir=$(cd "$1" && pwd)
data_dir=${2:+$(cd "$2" && pwd)}

for f in hexenwail.js hexenwail.wasm; do
	[ -f "$bin_dir/$f" ] || { echo "error: $bin_dir/$f not found" >&2; exit 2; }
done

browser=""
for candidate in "${CHROME_BIN:-}" google-chrome-stable google-chrome chromium chromium-browser; do
	if [ -n "$candidate" ] && command -v "$candidate" >/dev/null 2>&1; then
		browser="$(command -v "$candidate")"
		break
	fi
done
[ -n "$browser" ] || { echo "wasm boot smoke test requires Chrome or Chromium" >&2; exit 2; }
command -v python3 >/dev/null || { echo "wasm boot smoke test requires python3" >&2; exit 2; }

site=$(mktemp -d)
server_pid=""
chrome_pid=""
cleanup() {
	if [ -n "$chrome_pid" ]; then kill "$chrome_pid" 2>/dev/null || true; fi
	if [ -n "$server_pid" ]; then kill "$server_pid" 2>/dev/null || true; fi
	# Chrome's helper processes can still be writing the profile when the
	# main pid is gone; a cleanup hiccup must not overturn the verdict.
	rm -rf "$site" 2>/dev/null || true
}
trap cleanup EXIT

cp "$bin_dir/hexenwail.js" "$bin_dir/hexenwail.wasm" "$site/"

# The data files, as paths relative to the basedir, one JSON string each.
files_json="[]"
if [ -n "$data_dir" ]; then
	[ -f "$data_dir/data1/pak0.pak" ] || {
		echo "error: $data_dir/data1/pak0.pak not found" >&2
		exit 2
	}
	mkdir -p "$site/data"
	# --no-preserve=mode: a nix store basedir is read-only, and the trap
	# has to be able to delete the copy.
	cp -rL --no-preserve=mode "$data_dir/." "$site/data/"
	files_json=$(cd "$site/data" && find . -type f | sed 's|^\./||' | sort |
		python3 -c 'import json,sys; print(json.dumps([l.rstrip("\n") for l in sys.stdin]))')
	mode=map
	# Printed by the server once the local client has spawned in the level:
	# the map, progs.dat and the client/server message loop all worked.
	expect='player entered the game'
else
	mode=nodata
	expect='FATAL ERROR: Unable to find a proper Hexen II installation'
fi
expect_json=$(python3 -c 'import json, sys; print(json.dumps(sys.argv[1]))' "$expect")

# The page reports through the console, one line per event, because the
# engine's main loop never goes idle once a map is running: Chrome's
# --dump-dom/--virtual-time-budget pair waits for idle and would never
# produce a result.  --enable-logging=stderr puts every console.log on
# Chrome's stderr, and the loop below reads the verdict from there on a
# wall clock and then stops the browser.
#   HWLOG <line>             engine stdout/stderr, and harness notes
#   HWSMOKE pass|fail <why>  the verdict, printed once
cat >"$site/index.html" <<EOF
<!doctype html>
<html><head><meta charset="utf-8"><title>wasm boot smoke</title></head>
<body>
<canvas id="canvas" width="640" height="480"></canvas>
<script>
const MODE = "$mode";
const FILES = $files_json;
const EXPECT = $expect_json;
const engineLines = [];
let done = false;
function note(s) { console.log('HWLOG ' + s); }
function finish(ok, why) {
  if (done) return;
  done = true;
  console.log('HWSMOKE ' + (ok ? 'pass' : 'fail') + ' ' + why);
}
// A wasm trap is what a Rust/C layout or linkage mismatch looks like at run
// time, so it fails either mode as soon as it shows up.
const TRAP = /RuntimeError|memory access out of bounds|unreachable|\[abort\]/;
function engine(s) {
  s = String(s);
  engineLines.push(s);
  note(s);
  if (TRAP.test(s)) finish(false, 'runtime trap: ' + s);
  else if (MODE === 'nodata' && s.includes(EXPECT)) finish(true, 'engine reached the FS_Init data gate');
  else if (MODE === 'map' && /FATAL ERROR/.test(s)) finish(false, 'engine fatal: ' + s);
}
window.onerror = (m) => { engine('[window.onerror] ' + m); };
window.Module = {
  canvas: document.getElementById('canvas'),
  noInitialRun: true,
  print: (t) => engine(t),
  printErr: (t) => engine('[err] ' + t),
  onAbort: (w) => engine('[abort] ' + w),
  async onRuntimeInitialized() {
    try {
      const FS = Module.FS || window.FS;
      FS.mkdirTree('/persistent/data1');
      for (const path of FILES) {
        const bytes = new Uint8Array(await (await fetch('data/' + path)).arrayBuffer());
        FS.mkdirTree(('/persistent/' + path).replace(/\/[^/]*$/, ''));
        FS.writeFile('/persistent/' + path, bytes);
      }
      const args = ['-basedir', '/persistent'];
      if (MODE === 'map') args.push('+map', 'demo1');
      note('[smoke] callMain ' + args.join(' '));
      Module.callMain(args);
    } catch (e) {
      // emscripten reports exit() and a Sys_Error unwind as a thrown
      // ExitStatus; that is the expected end of the no-data run.
      note('[smoke] callMain threw: ' + (e && (e.message || e.status || e)));
    }
  },
};
// Map mode: the engine has to have loaded the level and kept rendering it
// for a while with no fatal and no trap.  EXPECT is matched against engine
// output only, never this harness's own notes.
if (MODE === 'map') {
  setTimeout(() => {
    const text = engineLines.join('\n');
    finish(text.includes(EXPECT), text.includes(EXPECT)
      ? 'demo1 loaded, the client spawned, no fatal or trap'
      : 'the local client never spawned in demo1');
  }, 15000);
}
</script>
<script src="hexenwail.js"></script>
</body></html>
EOF

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
python3 -m http.server "$port" --bind 127.0.0.1 --directory "$site" >/dev/null 2>&1 &
server_pid=$!
for _ in $(seq 50); do
	python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:$port/index.html')" 2>/dev/null && break
	sleep 0.1
done

console="$site/chrome.log"
"$browser" \
	--headless=new \
	--no-sandbox \
	--disable-dev-shm-usage \
	--use-gl=angle \
	--use-angle=swiftshader \
	--enable-unsafe-swiftshader \
	--enable-logging=stderr --v=0 \
	--user-data-dir="$site/profile" \
	"http://127.0.0.1:$port/index.html" >/dev/null 2>"$console" &
chrome_pid=$!

verdict=""
for _ in $(seq $((BROWSER_TIMEOUT * 2))); do
	verdict=$(grep -o '"HWSMOKE [^"]*' "$console" | head -1 | sed 's/^"HWSMOKE //') || true
	[ -n "$verdict" ] && break
	kill -0 "$chrome_pid" 2>/dev/null || break
	sleep 0.5
done
kill "$chrome_pid" 2>/dev/null || true
wait "$chrome_pid" 2>/dev/null || true

echo "---- engine output ----"
# Chrome escapes the message as a JS string literal: \" and \\ at most.
sed -n 's/.*"HWLOG \(.*\)", source: .*/\1/p' "$console" | sed 's/\\"/"/g; s/\\\\/\\/g'
echo "-----------------------"
case "$verdict" in
	pass\ *) echo "result: $verdict"; echo "PASS: hexenwail.wasm boot smoke ($mode)" ;;
	fail\ *) echo "result: $verdict"; echo "FAIL: hexenwail.wasm boot smoke ($mode)"; exit 1 ;;
	*)
		echo "FAIL: no verdict within ${BROWSER_TIMEOUT}s; Chrome's log follows"
		grep -v HWLOG "$console" | tail -40
		exit 1
		;;
esac
