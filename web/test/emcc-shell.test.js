import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

// engine/CMakeLists.txt generates the page emcc links (engine-shell-debug.html
// in the artifact) from web/index.html, so the launcher has one copy (#155).
// It anchors on two strings; this holds index.html and the CMake to the same
// two, and applies the same edit the CMake does.
const repoRoot = process.cwd();
const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');
const cmake = readFileSync(join(repoRoot, 'engine/CMakeLists.txt'), 'utf8');

const EMBED_OFF = 'window.__HEXENWAIL_EMSCRIPTEN_SCRIPT_EMBEDDED = false;';
const APP_TAG = '<script type="module" src="./app.js"></script>';

const count = (haystack, needle) => haystack.split(needle).length - 1;

test('web/index.html carries each anchor the shell generator replaces exactly once', () => {
  assert.equal(count(html, EMBED_OFF), 1);
  assert.equal(count(html, APP_TAG), 1);
});

test('engine/CMakeLists.txt replaces those same anchors and links the result', () => {
  assert.ok(cmake.includes(`set(_embed_off "${EMBED_OFF}")`));
  assert.ok(cmake.includes('set(_app_tag "<script type=\\"module\\" src=\\"./app.js\\"></script>")'));
  assert.match(cmake, /--shell-file=\$\{CMAKE_CURRENT_BINARY_DIR\}\/shell\.html/);
  assert.doesNotMatch(cmake, /--shell-file=\$\{CMAKE_CURRENT_SOURCE_DIR\}/, 'a hand-kept shell copy is linked again');
  assert.ok(!existsSync(join(repoRoot, 'engine/web/shell.html')), 'engine/web/shell.html is back');
});

test('the generated shell embeds the engine once, ahead of app.js', () => {
  const shell = html
    .replace(EMBED_OFF, 'window.__HEXENWAIL_EMSCRIPTEN_SCRIPT_EMBEDDED = true;')
    .replace(APP_TAG, `{{{ SCRIPT }}}\n  ${APP_TAG}`);
  assert.equal(count(shell, '{{{ SCRIPT }}}'), 1);
  assert.ok(shell.indexOf('{{{ SCRIPT }}}') < shell.indexOf(APP_TAG));
  assert.ok(shell.includes('__HEXENWAIL_EMSCRIPTEN_SCRIPT_EMBEDDED = true;'));
  // app.js skips its own <script src="./hexenwail.js"> on exactly this flag.
  const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
  assert.match(app, /state\.runtimeLoaded \|\| globalThis\.__HEXENWAIL_EMSCRIPTEN_SCRIPT_EMBEDDED/);
});
