import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { guardPointerLock, isBenignPointerLockRefusal } from '../lib/pointer-lock.js';

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

// The rest is web/lib/pointer-lock.js: the callers tryCaptureInput is not.
// SDL3 asks for the lock through library_html5.js's helper, quoted here as
// emscripten 4.0.23 ships it in hexenwail.js.  It drops the promise, so a
// rejection from it can only be handled inside requestPointerLock itself.
const emscriptenRequestPointerLock = (target) => {
  if (target.requestPointerLock) {
    target.requestPointerLock();
  } else {
    return -1;
  }
  return 0;
};

async function unhandledDuring(fn) {
  const unhandled = [];
  const listener = (reason) => unhandled.push(reason);
  process.on('unhandledRejection', listener);
  try {
    await fn();
    await new Promise((resolve) => setTimeout(resolve, 20));
  } finally {
    process.off('unhandledRejection', listener);
  }
  return unhandled;
}

// Blink's rejection when the browser process still holds a lock the page has
// already released (PointerLockController::ConvertResultToException).
const alreadyLocked = () => new DOMException('Pointer is already locked.', 'InUseAttributeError');

function chromeCanvas(rejectWith) {
  const canvas = {
    requests: 0,
    focus() {},
    requestPointerLock() {
      canvas.requests += 1;
      return rejectWith ? Promise.reject(rejectWith()) : Promise.resolve();
    },
  };
  return canvas;
}

// Node's test runner fails a test on any unhandled rejection, which is what
// holds the "must not leak" cases below.  The cases that must leak one watch
// the promise the helper drops instead: a promise that rejects with nobody
// holding it is exactly what reaches the page's unhandledrejection handler.
function watchDropped(canvas) {
  const inner = canvas.requestPointerLock;
  const outcomes = [];
  canvas.requestPointerLock = function requestPointerLock(...args) {
    const pending = inner.apply(this, args);
    outcomes.push(Promise.resolve(pending).then(() => 'resolved', (error) => error));
    return pending;
  };
  return outcomes;
}

test('unguarded, the SDL path drops a promise that rejects "Pointer is already locked."', async () => {
  // The control: the bug as it shipped.  If this stops failing the way it
  // should, the tests below prove nothing.
  const canvas = chromeCanvas(alreadyLocked);
  const dropped = watchDropped(canvas);
  assert.equal(emscriptenRequestPointerLock(canvas), 0);
  assert.equal(dropped.length, 1);
  const outcome = await dropped[0];
  assert.equal(outcome.name, 'InUseAttributeError');
  assert.equal(outcome.message, 'Pointer is already locked.');
});

test('guarded, the SDL path handles "Pointer is already locked."', async () => {
  const canvas = chromeCanvas(alreadyLocked);
  const debug = [];
  assert.equal(guardPointerLock(canvas, { pointerLockElement: null }, { debug: (...a) => debug.push(a) }), true);
  const dropped = watchDropped(canvas);
  const unhandled = await unhandledDuring(() => emscriptenRequestPointerLock(canvas));
  assert.equal(canvas.requests, 1);
  assert.deepEqual(unhandled, []);
  assert.equal(await dropped[0], 'resolved');
  assert.deepEqual(debug, [['Pointer lock not granted:', 'Pointer is already locked.']]);
});

test('guarded, a canvas that already holds the lock is not asked again', async () => {
  const canvas = chromeCanvas(alreadyLocked);
  const doc = { pointerLockElement: null };
  guardPointerLock(canvas, doc, { debug() {} });
  doc.pointerLockElement = canvas;
  const unhandled = await unhandledDuring(async () => {
    assert.equal(emscriptenRequestPointerLock(canvas), 0);
    await canvas.requestPointerLock();
  });
  assert.equal(canvas.requests, 0);
  assert.deepEqual(unhandled, []);
});

test('guarded, a request with options still reaches the browser while locked', async () => {
  const canvas = chromeCanvas();
  const doc = { pointerLockElement: null };
  guardPointerLock(canvas, doc, { debug() {} });
  doc.pointerLockElement = canvas;
  await canvas.requestPointerLock({ unadjustedMovement: true });
  assert.equal(canvas.requests, 1);
});

test('guarded, a genuine failure still reaches the global handler', async () => {
  const wrongDocument = () => new DOMException(
    'The root document of this element is not valid for pointer lock.', 'WrongDocumentError');
  const canvas = chromeCanvas(wrongDocument);
  guardPointerLock(canvas, { pointerLockElement: null }, { debug() {} });
  const dropped = watchDropped(canvas);
  assert.equal(emscriptenRequestPointerLock(canvas), 0);
  const outcome = await dropped[0];
  assert.equal(outcome.name, 'WrongDocumentError');
});

test('guarded, a caller holding the promise still sees a genuine failure', async () => {
  const canvas = chromeCanvas(() => new DOMException('unsupported', 'NotSupportedError'));
  guardPointerLock(canvas, { pointerLockElement: null }, { debug() {} });
  await assert.rejects(canvas.requestPointerLock(), { name: 'NotSupportedError' });
});

test('guarded, Safari\'s promise-less requestPointerLock is passed through', () => {
  let requests = 0;
  const canvas = { requestPointerLock() { requests += 1; } };
  guardPointerLock(canvas, { pointerLockElement: null }, { debug() {} });
  assert.equal(canvas.requestPointerLock(), undefined);
  assert.equal(emscriptenRequestPointerLock(canvas), 0);
  assert.equal(requests, 2);
});

test('guarding twice wraps once; a canvas without the API is left alone', async () => {
  const canvas = chromeCanvas(alreadyLocked);
  const doc = { pointerLockElement: null };
  assert.equal(guardPointerLock(canvas, doc, { debug() {} }), true);
  const first = canvas.requestPointerLock;
  assert.equal(guardPointerLock(canvas, doc, { debug() {} }), false);
  assert.equal(canvas.requestPointerLock, first);
  await canvas.requestPointerLock();
  assert.equal(canvas.requests, 1);
  assert.equal(guardPointerLock({}, doc), false);
  assert.equal(guardPointerLock(null, doc), false);
});

test('tryCaptureInput still works through the guard', async () => {
  const canvas = chromeCanvas(alreadyLocked);
  const doc = { pointerLockElement: null };
  guardPointerLock(canvas, doc, { debug() {} });
  const unhandled = await unhandledDuring(() => load(canvas, doc)());
  assert.equal(canvas.requests, 1);
  assert.deepEqual(unhandled, []);
});

test('refusals the next click repairs are benign; real faults are not', () => {
  // Chrome's own names and messages, from ConvertResultToException and
  // PointerLockController::RequestPointerLock.
  const refusal = (message, name) => isBenignPointerLockRefusal(new DOMException(message, name));
  assert.equal(refusal('Pointer is already locked.', 'InUseAttributeError'), true);
  assert.equal(refusal('Pointer lock pending.', 'InUseAttributeError'), true);
  assert.equal(refusal('A user gesture is required to request Pointer Lock.', 'NotAllowedError'), true);
  assert.equal(refusal('Too many pointer lock requests in a short window of time.', 'NotAllowedError'), true);
  assert.equal(refusal('Pointer lock cannot be acquired immediately after the user has exited the lock.', 'SecurityError'), true);
  assert.equal(refusal('The user has exited the lock before this request was completed.', 'SecurityError'), true);
  assert.equal(refusal('The root document of this element is not valid for pointer lock.', 'SecurityError'), false);
  assert.equal(refusal('The root document of this element is not valid for pointer lock.', 'WrongDocumentError'), false);
  assert.equal(refusal('The element has been destroyed while making this request.', 'WrongDocumentError'), false);
  assert.equal(refusal('The options asked for in this request are not supported on this platform.', 'NotSupportedError'), false);
  assert.equal(refusal('If you see this error we have a bug.', 'UnknownError'), false);
  assert.equal(isBenignPointerLockRefusal(undefined), false);
  assert.equal(isBenignPointerLockRefusal('Pointer is already locked.'), false);
});

test('app.js guards the canvas before the engine script can ask for the lock', () => {
  assert.match(app, /^import \{ guardPointerLock \} from '\.\/lib\/pointer-lock\.js';$/m);
  const init = app.slice(app.indexOf('async function init() {'));
  const guard = init.indexOf('guardPointerLock(ui.canvas);');
  assert.ok(guard > init.indexOf('bindUi();'), 'guardPointerLock must run after bindUi() fills ui.canvas');
  const engine = init.indexOf('ensureEngineScriptLoaded(');
  assert.ok(engine === -1 || guard < engine, 'guardPointerLock must run before hexenwail.js loads');
});
