import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');

test('launcher chrome exposes bounded runtime diagnostics in both shells', () => {
  for (const path of ['web/index.html', 'engine/web/shell.html']) {
    const html = readFileSync(join(repoRoot, path), 'utf8');
    assert.match(html, /id="runtime-log"/, `${path} is missing the runtime log`);
    assert.match(html, /earlyLog/, `${path} does not buffer output emitted before app.js binds`);
  }

  assert.match(app, /MAX_RUNTIME_LOG_ENTRIES/);
  assert.match(app, /appendRuntimeLog\(prefix, text\)/);
  assert.match(app, /boot\.earlyLog \?\? \[\]/);
  assert.match(app, /runtimeLog: document\.getElementById\('runtime-log'\)/);
});

test('manual WASM launch keeps configured arguments and fails explicitly', () => {
  assert.match(app, /const ENGINE_ARGUMENTS = \['-basedir', BASE_DIR\]/);
  assert.match(app, /Module\.callMain\(\[\.\.\.ENGINE_ARGUMENTS\]\)/);
  assert.match(app, /Engine runtime did not expose callMain/);
  assert.match(app, /Engine exited during startup with status/);
  assert.doesNotMatch(app, /callMain\?\.\(\[\]\)/);
  assert.doesNotMatch(app, /String\(text\)\.includes\('Unable to find a proper Hexen II installation'\)/);
});
