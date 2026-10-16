import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');

test('launcher chrome exposes bounded runtime diagnostics', () => {
  // web/index.html is also the emcc shell (engine/CMakeLists.txt, #155), so
  // this covers engine-shell-debug.html too.
  const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');
  assert.match(html, /id="runtime-log"/, 'web/index.html is missing the runtime log');
  assert.match(html, /earlyLog/, 'web/index.html does not buffer output emitted before app.js binds');

  assert.match(app, /MAX_RUNTIME_LOG_ENTRIES/);
  assert.match(app, /appendRuntimeLog\(prefix, text\)/);
  assert.match(app, /boot\.earlyLog \?\? \[\]/);
  assert.match(app, /runtimeLog: document\.getElementById\('runtime-log'\)/);
});

test('manual WASM launch keeps configured arguments and fails explicitly', () => {
  assert.match(app, /const ENGINE_ARGUMENTS = \['-basedir', BASE_DIR\]/);
  // The mod selector may append a -game pair; ENGINE_ARGUMENTS stays the base.
  assert.match(app, /Module\.callMain\(engineArguments\)/);
  assert.match(app, /buildEngineArguments\(ENGINE_ARGUMENTS, getActiveMod\(\)\)/);
  assert.match(app, /engine arguments: \$\{engineArguments\.join\(' '\)\}/);
  assert.match(app, /Engine runtime did not expose callMain/);
  assert.match(app, /Engine exited during startup with status/);
  assert.doesNotMatch(app, /callMain\?\.\(\[\]\)/);
  assert.doesNotMatch(app, /String\(text\)\.includes\('Unable to find a proper Hexen II installation'\)/);
});
