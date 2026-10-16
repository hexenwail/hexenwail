import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { canvasViewportVerdict, readViewport } from '../lib/canvas-viewport.js';

const view = { left: 0, top: 0, width: 1280, height: 720 };

test('a canvas that fills the viewport passes', () => {
  const v = canvasViewportVerdict({ left: 0, top: 0, width: 1280, height: 720 }, view);
  assert.equal(v.ok, true);
  assert.equal(v.visibleFraction, 1);
  assert.match(v.message, /is inside the visual viewport 1280x720/);
});

test('the o6js shape: an 832x2371 canvas in a 1280x720 window fails on the bottom edge', () => {
  // 721c218b2: the drawable was 832x2371 in a 1280x720 window, and the
  // offscreen self-test passed throughout.
  const v = canvasViewportVerdict({ left: 224, top: 60, width: 832, height: 2371 }, view);
  assert.equal(v.ok, false);
  assert.ok(v.overflow.bottom > 1700);
  assert.equal(v.overflow.left, 0);
  assert.equal(v.overflow.right, 0);
  assert.match(v.message, /sticks out of the visual viewport.*bottom by 1711px/);
  assert.match(v.message, /28% visible/);
});

test('a canvas pushed fully below the fold is 0% visible', () => {
  const v = canvasViewportVerdict({ left: 0, top: 900, width: 1280, height: 720 }, view);
  assert.equal(v.ok, false);
  assert.equal(v.visibleFraction, 0);
});

test('every edge is reported', () => {
  const v = canvasViewportVerdict({ left: -10, top: -20, width: 1320, height: 780 }, view);
  assert.equal(v.ok, false);
  assert.deepEqual(v.overflow, { left: 10, top: 20, right: 30, bottom: 40 });
  assert.match(v.message, /left by 10px, top by 20px, right by 30px, bottom by 40px/);
});

test('sub-pixel overflow within the tolerance is not a layout bug', () => {
  const v = canvasViewportVerdict({ left: -0.4, top: 0, width: 1280.8, height: 720.5 }, view);
  assert.equal(v.ok, true);
  const strict = canvasViewportVerdict({ left: -0.4, top: 0, width: 1280.8, height: 720.5 }, view, 0);
  assert.equal(strict.ok, false);
});

test('a zero-size canvas fails rather than counting as inside', () => {
  const v = canvasViewportVerdict({ left: 10, top: 10, width: 0, height: 0 }, view);
  assert.equal(v.ok, false);
  assert.match(v.message, /has no area/);
});

test('the visual viewport offset is honoured (pinch-zoom, on-screen keyboard)', () => {
  // Zoomed in and panned: only (300,200)-(940,560) of the layout viewport is visible.
  const zoomed = { left: 300, top: 200, width: 640, height: 360 };
  const inside = canvasViewportVerdict({ left: 300, top: 200, width: 640, height: 360 }, zoomed);
  assert.equal(inside.ok, true);
  const atOrigin = canvasViewportVerdict({ left: 0, top: 0, width: 640, height: 360 }, zoomed);
  assert.equal(atOrigin.ok, false);
  assert.deepEqual(atOrigin.overflow, { left: 300, top: 200, right: 0, bottom: 0 });
  // Visible part is (300,200)-(640,360): 340 x 160 of 640 x 360.
  assert.equal(atOrigin.visibleFraction, (340 * 160) / (640 * 360));
});

test('visibleFraction clips on the left and top edges too', () => {
  const v = canvasViewportVerdict({ left: -640, top: -360, width: 1280, height: 720 }, view);
  assert.equal(v.visibleFraction, 0.25);
});

test('the tolerance is inclusive: exactly 1px over passes, just past it fails', () => {
  assert.equal(canvasViewportVerdict({ left: 0, top: 0, width: 1281, height: 720 }, view).ok, true);
  assert.equal(canvasViewportVerdict({ left: 0, top: 0, width: 1281.1, height: 720 }, view).ok, false);
});

test('a zoomed page is named as the likely cause', () => {
  const v = canvasViewportVerdict({ left: 0, top: 0, width: 1280, height: 720 },
    { left: 100, top: 50, width: 640, height: 360, scale: 2 });
  assert.equal(v.ok, false);
  assert.match(v.message, /page is zoomed 2x/);
  const unzoomed = canvasViewportVerdict({ left: 0, top: 900, width: 1280, height: 720 }, { ...view, scale: 1 });
  assert.doesNotMatch(unzoomed.message, /zoomed/);
});

test('readViewport prefers visualViewport and falls back to inner size', () => {
  assert.deepEqual(
    readViewport({ visualViewport: { offsetLeft: 5, offsetTop: 7, width: 400, height: 300, scale: 1.5 }, innerWidth: 999, innerHeight: 999 }),
    { left: 5, top: 7, width: 400, height: 300, scale: 1.5 });
  assert.deepEqual(readViewport({ innerWidth: 800, innerHeight: 600 }), { left: 0, top: 0, width: 800, height: 600, scale: 1 });
  // A visualViewport that reports nothing yet (before first layout) is ignored.
  assert.deepEqual(
    readViewport({ visualViewport: { offsetLeft: 0, offsetTop: 0, width: 0, height: 0 }, innerWidth: 800, innerHeight: 600 }),
    { left: 0, top: 0, width: 800, height: 600, scale: 1 });
});

test('app.js runs the check at launch and logs it', () => {
  const app = readFileSync(join(process.cwd(), 'web/app.js'), 'utf8');
  assert.match(app, /from '\.\/lib\/canvas-viewport\.js'/);
  const start = app.slice(app.indexOf('async function startEngineFromUserAction'));
  const body = start.slice(0, start.indexOf('\n}\n'));
  // Scheduled before callMain, which may never return.
  assert.ok(body.indexOf('scheduleCanvasViewportCheck(') !== -1, 'startEngineFromUserAction does not schedule the check');
  assert.ok(body.indexOf('scheduleCanvasViewportCheck(') < body.indexOf('callMain('), 'the check must be scheduled before callMain');
  // It measures the canvas itself (not the viewport element it is sized
  // from), against the visual viewport, and writes the result to the log.
  assert.match(app, /canvasViewportVerdict\(ui\.canvas\.getBoundingClientRect\(\), readViewport\(\)\)/);
  assert.match(app, /logToConsole\('\[layout\]', verdict\.message, !verdict\.ok\)/);
});

test('the verdict survives the phone-mode reload back to the launcher', () => {
  const app = readFileSync(join(process.cwd(), 'web/app.js'), 'utf8');
  assert.match(app, /sessionStorage\.setItem\(LAYOUT_VERDICT_KEY/);
  const init = app.slice(app.indexOf('async function init()'));
  assert.ok(init.slice(0, 400).includes('replayLastLayoutVerdict()'), 'init() does not replay the last verdict early');
  // Replayed once, not on every later reload.
  assert.match(app, /sessionStorage\.removeItem\(LAYOUT_VERDICT_KEY\)/);
});
