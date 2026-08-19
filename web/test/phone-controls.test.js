import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { PhoneControls, PHONE_CONTROL_KEYCODES } from '../lib/phone-controls.js';

function makeElement(action, rect = { left: 0, top: 0, width: 120, height: 120 }) {
  const listeners = new Map();
  const style = new Map();
  return {
    dataset: action ? { phoneAction: action } : {},
    style: { setProperty: (name, value) => style.set(name, value), get: (name) => style.get(name) },
    closest(selector) {
      return selector.includes(this.dataset.phoneAction) || selector === '[data-phone-action]' ? this : null;
    },
    getBoundingClientRect: () => rect,
    addEventListener(name, callback) { listeners.set(name, callback); },
    removeEventListener(name) { listeners.delete(name); },
    setPointerCapture() {},
    releasePointerCapture() {},
    dispatch(name, event) { listeners.get(name)?.(event); },
  };
}

function pointer(target, pointerId, x, y) {
  return {
    target,
    pointerId,
    clientX: x,
    clientY: y,
    preventDefault() { this.defaultPrevented = true; },
    stopPropagation() { this.stopped = true; },
  };
}

test('stick owns one pointer and transitions movement keys without sticking', () => {
  const root = makeElement(null);
  const stick = makeElement('stick');
  const events = [];
  const controls = new PhoneControls(root, { key: (key, down) => events.push([key, down]), look() {} });
  controls.attach();

  root.dispatch('pointerdown', pointer(stick, 1, 60, 60));
  root.dispatch('pointermove', pointer(stick, 1, 60, 0));
  root.dispatch('pointermove', pointer(stick, 1, 120, 60));
  root.dispatch('pointerup', pointer(stick, 1, 120, 60));

  assert.deepEqual(events, [
    [PHONE_CONTROL_KEYCODES.forward, true],
    [PHONE_CONTROL_KEYCODES.forward, false],
    [PHONE_CONTROL_KEYCODES.right, true],
    [PHONE_CONTROL_KEYCODES.right, false],
  ]);
});

test('multi-touch buttons and look region keep independent pointer ownership', () => {
  const root = makeElement(null);
  const attack = makeElement('attack');
  const look = makeElement('look');
  const keys = [];
  const looks = [];
  const controls = new PhoneControls(root, {
    key: (key, down) => keys.push([key, down]),
    look: (dx, dy) => looks.push([dx, dy]),
  }, { lookSensitivity: 2, maxLookDelta: 10 });
  controls.attach();

  root.dispatch('pointerdown', pointer(attack, 7, 10, 10));
  root.dispatch('pointerdown', pointer(look, 8, 100, 100));
  root.dispatch('pointermove', pointer(look, 8, 140, 90));
  root.dispatch('pointerup', pointer(attack, 7, 10, 10));

  assert.deepEqual(keys, [[PHONE_CONTROL_KEYCODES.attack, true], [PHONE_CONTROL_KEYCODES.attack, false]]);
  assert.deepEqual(looks, [[20, -20]], 'look deltas are clamped before sensitivity scaling');
});

test('each touch action rejects a second pointer', () => {
  const root = makeElement(null);
  const attack = makeElement('attack');
  const stick = makeElement('stick');
  const events = [];
  const controls = new PhoneControls(root, { key: (key, down) => events.push([key, down]), look() {} });
  controls.attach();

  root.dispatch('pointerdown', pointer(attack, 1, 10, 10));
  root.dispatch('pointerdown', pointer(attack, 2, 10, 10));
  root.dispatch('pointerup', pointer(attack, 1, 10, 10));
  root.dispatch('pointerdown', pointer(stick, 3, 60, 60));
  root.dispatch('pointerdown', pointer(stick, 4, 60, 60));
  root.dispatch('pointermove', pointer(stick, 4, 60, 0));

  assert.deepEqual(events, [
    [PHONE_CONTROL_KEYCODES.attack, true],
    [PHONE_CONTROL_KEYCODES.attack, false],
  ]);
});

test('releaseAll clears button and stick keys after cancellation or backgrounding', () => {
  const root = makeElement(null);
  const stick = makeElement('stick');
  const jump = makeElement('jump');
  const events = [];
  const controls = new PhoneControls(root, { key: (key, down) => events.push([key, down]), look() {} });
  controls.attach();

  root.dispatch('pointerdown', pointer(stick, 1, 60, 60));
  root.dispatch('pointermove', pointer(stick, 1, 60, 0));
  root.dispatch('pointerdown', pointer(jump, 2, 0, 0));
  controls.releaseAll();

  assert.deepEqual(events, [
    [PHONE_CONTROL_KEYCODES.forward, true],
    [PHONE_CONTROL_KEYCODES.jump, true],
    [PHONE_CONTROL_KEYCODES.forward, false],
    [PHONE_CONTROL_KEYCODES.jump, false],
  ]);
});

test('phone mode DOM includes playing layout, touch visibility rules, and quit hook', () => {
  const repoRoot = process.cwd();
  const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');
  const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
  assert.match(html, /body\[data-engine-state="running"\]/);
  assert.match(html, /id="phone-controls"/);
  assert.match(html, /data-touch-only="true"/);
  assert.match(html, /data-phone-mode="true"/);
  assert.match(html, /@media \(pointer: coarse\) and \(hover: none\) and \(max-width: 820px\), \(pointer: coarse\) and \(hover: none\) and \(max-height: 820px\)/);
  assert.match(app, /isLikelyTouchOnlyEnvironment/);
  assert.match(app, /isPhoneModeEnvironment/);
  assert.match(app, /PHONE_VIEWPORT_QUERY/);
  assert.match(app, /gamepadconnected/);
  assert.match(app, /hexenwailquit/);
  assert.match(app, /Hexenwail_ResizeCanvas/);
});
