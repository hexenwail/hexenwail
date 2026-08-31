#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

# Wall-clock budget for one headless Chrome run.  Both launches together
# normally finish in about 20s, so this is not sized for the work -- it is
# sized for a starved runner.  Run 33402775034 failed with exit 124 after
# Chrome needed 29.8s just to reach its dbus setup, against the 4.3s the
# four runs before it took; the page itself was never the problem, and the
# same commit passed on a rerun.  Keep it far above the real cost so a
# genuine hang still trips it, but a slow cold start does not.
readonly BROWSER_TIMEOUT=120

browser=""
for candidate in "${CHROME_BIN:-}" google-chrome-stable google-chrome chromium chromium-browser; do
	if [ -n "$candidate" ] && command -v "$candidate" >/dev/null 2>&1; then
		browser="$(command -v "$candidate")"
		break
	fi
done

if [ -z "$browser" ]; then
	echo "WebGL2 smoke test requires Chrome or Chromium" >&2
	exit 1
fi

output="$(mktemp)"
engine_output="$(mktemp)"
engine_page="$(mktemp --suffix=.html)"
trap 'rm -f "$output" "$engine_output" "$engine_page"' EXIT

timeout "$BROWSER_TIMEOUT" "$browser" \
	--headless=new \
	--no-sandbox \
	--disable-dev-shm-usage \
	--allow-file-access-from-files \
	--use-gl=angle \
	--use-angle=swiftshader \
	--enable-unsafe-swiftshader \
	--virtual-time-budget=5000 \
	--dump-dom \
	"file://$PWD/web/test/webgl-smoke.html" >"$output"

if ! grep -q 'data-result="pass"' "$output"; then
	echo "WebGL2 smoke test failed:" >&2
	sed -n '/<pre id="result">/,/<\/pre>/p' "$output" >&2
	exit 1
fi

if grep -Eqi 'shader (compilation|link) failed|WebGL2 error|predominantly black' "$output"; then
	echo "WebGL2 smoke test reported a renderer regression:" >&2
	sed -n '/<pre id="result">/,/<\/pre>/p' "$output" >&2
	exit 1
fi

node scripts/webgl-engine-shader-smoke.mjs "$engine_page"

timeout "$BROWSER_TIMEOUT" "$browser" \
	--headless=new \
	--no-sandbox \
	--disable-dev-shm-usage \
	--allow-file-access-from-files \
	--use-gl=angle \
	--use-angle=swiftshader \
	--enable-unsafe-swiftshader \
	--virtual-time-budget=5000 \
	--dump-dom \
	"file://$engine_page" >"$engine_output"

if ! grep -q 'data-result="pass"' "$engine_output"; then
	echo "Engine WebGL2 shader test failed:" >&2
	sed -n '/<pre id="result">/,/<\/pre>/p' "$engine_output" >&2
	exit 1
fi

echo "WebGL2 engine shaders, RGBA8 framebuffer, post-process, and non-black-frame smoke test passed."
