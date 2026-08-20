import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');

// uhexen2-o6js: with an auto-height body the launcher column's ~2400px of
// cards sized the shell row, so the canvas grew with it and the software
// renderer's centred 4:3 image landed below the fold -- a black window at
// 1280x720 while the engine ran normally.  Both declarations below are what
// keep the canvas inside the visible viewport; neither is cosmetic.
test('the app shell is bounded by the viewport, not by the launcher column', () => {
  assert.match(html, /height:\s*100dvh;/,
    'body needs a definite height, not just min-height');
  assert.match(html, /grid-template-rows:\s*minmax\(0,\s*1fr\);/,
    '.shell needs a bounded row so #control-panel scrolls instead of stretching the canvas');
});
