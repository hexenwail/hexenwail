import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

// Every engine function the launcher calls through ccall must be exported by
// the Emscripten link, or the call fails at runtime and the fallback (or
// nothing) runs instead -- touch input, look and canvas resize.  The alextnewman
// fork renamed these to Web_* on one side only; the rename-only hunks were
// reviewed for GitHub #150 (a), and this holds the two sides together.
test('every callEngine() name is in the link\'s EXPORTED_FUNCTIONS', () => {
  const app = readFileSync(join(import.meta.dirname, '../app.js'), 'utf8');
  const cmake = readFileSync(join(import.meta.dirname, '../../engine/CMakeLists.txt'), 'utf8');
  const called = new Set([...app.matchAll(/callEngine\('([A-Za-z_][A-Za-z0-9_]*)'/g)].map((m) => m[1]));
  const exportsLine = cmake.match(/-sEXPORTED_FUNCTIONS=\[([^\]]*)\]/);
  assert.ok(exportsLine, 'no -sEXPORTED_FUNCTIONS in engine/CMakeLists.txt');
  const exported = new Set([...exportsLine[1].matchAll(/'_([A-Za-z0-9_]+)'/g)].map((m) => m[1]));
  assert.ok(called.size >= 3, `expected the launcher's engine calls, found ${[...called]}`);
  for (const name of called) {
    assert.ok(exported.has(name), `app.js calls ${name}, which the wasm link does not export`);
  }
});
