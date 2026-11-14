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
# BROWSER=firefox runs the same page in Firefox instead (GitHub #112: the ES
# tier had only ever been checked in Chromium).  Headless Firefox finds no GL
# driver on a box without a GPU-backed EGL ("Exhausted GL driver options"), so
# it runs non-headless on a private Xvfb display, where GLX gives it llvmpipe.
# Needs firefox and Xvfb on PATH, e.g.
#   nix shell nixpkgs#firefox nixpkgs#xorg-server nixpkgs#python3 --command \
#     env BROWSER=firefox ./scripts/wasm-boot-smoke.sh BIN_DIR [DATA_DIR]
# The page is identical.  Only the launch differs, and the log format:
# Firefox prints page console calls to stdout as `console.log: "..."` (with a
# profile pref) and WebGL errors to stderr as `WebGL warning: <call>: ...`.
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

browser_kind=${BROWSER:-chrome}
browser=""
case "$browser_kind" in
	chrome)
		for candidate in "${CHROME_BIN:-}" google-chrome-stable google-chrome chromium chromium-browser; do
			if [ -n "$candidate" ] && command -v "$candidate" >/dev/null 2>&1; then
				browser="$(command -v "$candidate")"
				break
			fi
		done
		[ -n "$browser" ] || { echo "wasm boot smoke test requires Chrome or Chromium" >&2; exit 2; }
		;;
	firefox)
		browser=$(command -v firefox) || { echo "BROWSER=firefox requires firefox on PATH" >&2; exit 2; }
		command -v Xvfb >/dev/null || { echo "BROWSER=firefox requires Xvfb on PATH" >&2; exit 2; }
		;;
	*) echo "BROWSER must be chrome or firefox, not '$browser_kind'" >&2; exit 2 ;;
esac
command -v python3 >/dev/null || { echo "wasm boot smoke test requires python3" >&2; exit 2; }

site=$(mktemp -d)
server_pid=""
chrome_pid=""
xvfb_pid=""
cleanup() {
	if [ -n "$chrome_pid" ]; then kill "$chrome_pid" 2>/dev/null || true; fi
	if [ -n "$server_pid" ]; then kill "$server_pid" 2>/dev/null || true; fi
	if [ -n "$xvfb_pid" ]; then kill "$xvfb_pid" 2>/dev/null || true; fi
	# SMOKE_LOG=<path> keeps the raw browser log, which is otherwise deleted
	# with the site; it is the only place filtered WebGL lines can be audited.
	if [ -n "${SMOKE_LOG:-}" ] && [ -f "$site/chrome.log" ]; then cp "$site/chrome.log" "$SMOKE_LOG" || true; fi
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
# SMOKE_ARGS: extra engine arguments for the map run, split on whitespace and
# placed before +map, e.g. "+r_softemu 1 +r_dither 1 +r_bloom 1" to compile and
# run the post-process programs a plain load never builds.
extra_json=$(python3 -c 'import json, sys; print(json.dumps(sys.argv[1].split()))' "${SMOKE_ARGS:-}")

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
const EXTRA_ARGS = $extra_json;
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
// A shader the browser's GLSL ES validator rejects is an ES-tier bug even
// when the engine carries on without it (post-processing just switches off),
// which is how #112's bugs hid.  Every compile/link failure the engine prints.
const SHADER_FAIL = /COMPILE ERROR|LINK ERROR|shader compile error|shader link error|link failed/;
function engine(s) {
  s = String(s);
  engineLines.push(s);
  note(s);
  if (TRAP.test(s)) finish(false, 'runtime trap: ' + s);
  else if (MODE === 'nodata' && s.includes(EXPECT)) finish(true, 'engine reached the FS_Init data gate');
  else if (SHADER_FAIL.test(s)) finish(false, 'shader rejected: ' + s);
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
      if (MODE === 'map') args.push(...EXTRA_ARGS, '+map', 'demo1');
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
if [ "$browser_kind" = firefox ]; then
	mkdir -p "$site/profile"
	# stdout.content puts page console calls on stdout; the rest keeps a
	# fresh profile from opening first-run pages over the test.
	cat >"$site/profile/user.js" <<'PREFS'
user_pref("devtools.console.stdout.content", true);
user_pref("webgl.force-enabled", true);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("browser.newtabpage.activity-stream.feeds.topsites", false);
PREFS
	# -displayfd: Xvfb picks a free display and reports it, so concurrent
	# runs never share one.
	exec 9<> <(:)
	Xvfb -displayfd 9 -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 &
	xvfb_pid=$!
	read -r -t 20 display_n <&9 || { echo "error: Xvfb did not start" >&2; exit 2; }
	exec 9<&-
	DISPLAY=":$display_n" "$browser" --no-remote --profile "$site/profile" \
		"http://127.0.0.1:$port/index.html" >"$console" 2>&1 &
	chrome_pid=$!
else
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
fi

verdict=""
for _ in $(seq $((BROWSER_TIMEOUT * 2))); do
	verdict=$(grep -o '"HWSMOKE [^"]*' "$console" | head -1 | sed 's/^"HWSMOKE //') || true
	[ -n "$verdict" ] && break
	kill -0 "$chrome_pid" 2>/dev/null || break
	sleep 0.5
done
# SMOKE_SHOT=<file.png>, Firefox only: grab the Xvfb screen while the page is
# still up, i.e. the frame as the browser composited it, not the engine's own
# readback.  Needs ImageMagick's `import`.
if [ -n "${SMOKE_SHOT:-}" ] && [ "$browser_kind" = firefox ]; then
	if command -v import >/dev/null; then
		import -display ":$display_n" -window root "$SMOKE_SHOT" 2>/dev/null ||
			echo "warning: SMOKE_SHOT capture failed" >&2
	else
		echo "warning: SMOKE_SHOT needs ImageMagick's import on PATH" >&2
	fi
fi
kill "$chrome_pid" 2>/dev/null || true
wait "$chrome_pid" 2>/dev/null || true

echo "---- engine output ----"
# Both browsers escape the message as a JS string literal: \" and \\ at most.
# Chrome: ... "HWLOG <msg>", source: ...   Firefox: console.log: "HWLOG <msg>"
sed -n -e 's/.*"HWLOG \(.*\)", source: .*/\1/p' \
       -e 's/^console\.log: "HWLOG \(.*\)"$/\1/p' "$console" | sed 's/\\"/"/g; s/\\\\/\\/g'
echo "-----------------------"
# A WebGL error is a failure, not noise.  These lines were once read as
# SwiftShader chatter; they were the engine: texture swizzle WebGL2 dropped
# (254 per demo1 load), and multisample OIT targets the ES tier cannot create
# (GitHub #112).  Chrome reports each as "WebGL: INVALID_..."; the engine's ES
# error poll as "GL error ... detected at end of frame".  Firefox reports a
# failed call as "WebGL warning: <call>: <why>" on stderr; it uses the same
# prefix for a few performance notes, which are not errors and are listed
# in FF_WEBGL_NOTES rather than counted.
FF_WEBGL_NOTES='lazy initialization|Tex image .* is incurring|is deprecated|Alpha-premult|performance'
gl_errors=$(grep -aE 'WebGL: (INVALID|OUT_OF_MEMORY|CONTEXT_LOST)|GL error GL_|WebGL warning: ' "$console" |
	grep -avcE "$FF_WEBGL_NOTES" || true)
if [ "$gl_errors" -gt 0 ] && [ "${verdict%% *}" = pass ]; then
	echo "GL errors in the browser's log:"
	grep -aoE 'WebGL: [A-Z_]+: [^"]*|GL error GL_[^"]*|WebGL warning: [^"]*' "$console" |
		grep -avE "$FF_WEBGL_NOTES" | sort | uniq -c | head -20
	verdict="fail $gl_errors WebGL/GL error line(s)"
fi
case "$verdict" in
	pass\ *) echo "result: $verdict"; echo "PASS: hexenwail.wasm boot smoke ($mode)" ;;
	fail\ *) echo "result: $verdict"; echo "FAIL: hexenwail.wasm boot smoke ($mode)"; exit 1 ;;
	*)
		echo "FAIL: no verdict within ${BROWSER_TIMEOUT}s; the browser's log follows"
		grep -v HWLOG "$console" | tail -40
		exit 1
		;;
esac
