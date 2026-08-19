#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

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

timeout 45 "$browser" \
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

timeout 45 "$browser" \
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
