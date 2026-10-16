import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

// web/app.js's tryCaptureInput, run as written against a fake canvas (#150).
// Chrome's requestPointerLock returns a promise that rejects when the pointer
// is already locked; unhandled, that reached the page's unhandledrejection
// handler and replaced the status line on every canvas click.
const app = readFileSync(join(process.cwd(), 'web/app.js'), 'utf8');
const start = app.indexOf('function tryCaptureInput() {');
let depth = 0;
let end = app.indexOf('{', start);
do {
  depth += app[end] === '{' ? 1 : app[end] === '}' ? -1 : 0;
  end += 1;
} while (depth);
const source = app.slice(start, end);

function load(canvas, documentStub) {
  // eslint-disable-next-line no-new-func
  return new Function('ui', 'document', 'console', `${source}; return tryCaptureInput;`)(
    { canvas, pointerLockHint: null }, documentStub, { debug() {} });
}

test('a refused pointer lock is handled, not left to the global handler', async () => {
  const unhandled = [];
  const listener = (reason) => unhandled.push(reason);
  process.on('unhandledRejection', listener);
  try {
    let requests = 0;
    const canvas = {
      focus() {},
      requestPointerLock() {
        requests += 1;
        return Promise.reject(new Error('Pointer is already locked.'));
      },
    };
    load(canvas, { pointerLockElement: null })();
    await new Promise((resolve) => setTimeout(resolve, 20));
    assert.equal(requests, 1);
    assert.deepEqual(unhandled, []);
  } finally {
    process.off('unhandledRejection', listener);
  }
});

test('an already locked canvas is not asked again', () => {
  let requests = 0;
  const canvas = { focus() {}, requestPointerLock() { requests += 1; return Promise.resolve(); } };
  load(canvas, { pointerLockElement: canvas })();
  assert.equal(requests, 0);
});

test('Safari, whose requestPointerLock returns nothing, still works', () => {
  let requests = 0;
  const canvas = { focus() {}, requestPointerLock() { requests += 1; } };
  assert.doesNotThrow(() => load(canvas, { pointerLockElement: null })());
  assert.equal(requests, 1);
});
