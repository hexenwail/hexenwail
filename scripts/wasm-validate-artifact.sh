#!/usr/bin/env bash
# Checks that an assembled PWA "dist" directory holds everything an
# offline-installable client needs: the Emscripten runtime, the PWA shell
# (manifest + service worker), and the ES modules app.js imports.
#
# This is the part the PR check has that pages.yml never did.  A deploy proves
# the artifact assembled; it does not prove the artifact is complete, because
# a missing lib/ module is a blank screen at runtime, not a failed step.  Here
# it is a red check instead.
#
# Usage: scripts/wasm-validate-artifact.sh [dist-dir]
set -euo pipefail

cd "$(dirname "$0")/.."

DIST_DIR="${1:-dist}"
missing=0

require() {
	local path="$1"
	# -s, not -e: cp of a truncated or zero-length build output succeeds, and
	# an empty hexenwail.wasm is the failure mode worth catching.
	if [ ! -s "$DIST_DIR/$path" ]; then
		echo "MISSING (or empty): $DIST_DIR/$path" >&2
		missing=1
	else
		echo "OK: $DIST_DIR/$path"
	fi
}

# Emscripten output.
require "hexenwail.js"
require "hexenwail.wasm"
require "engine-shell-debug.html"

# PWA shell.
require "index.html"
require "manifest.webmanifest"
require "sw.js"
require "app.js"

# ES modules app.js imports; absent, the page loads and then does nothing.
require "lib/paths.js"
require "lib/zip.js"
require "lib/save-bundle.js"

# Icons the manifest names.  An install prompt with no icon is a soft failure
# browsers do not report.
require "icons/icon-192.png"
require "icons/icon-512.png"

# .data / .worker.js depend on build options (preloaded game data, pthreads),
# so their absence is normal for the default browser build -- report only.
for optional in hexenwail.data hexenwail.worker.js; do
	if [ -s "$DIST_DIR/$optional" ]; then
		echo "OK (optional present): $DIST_DIR/$optional"
	else
		echo "info: optional artifact not present: $DIST_DIR/$optional"
	fi
done

if [ "$missing" -ne 0 ]; then
	echo "PWA artifact validation FAILED: required files are missing." >&2
	exit 1
fi

echo "PWA artifact validation passed."
