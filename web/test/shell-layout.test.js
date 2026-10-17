import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');
const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');

// uhexen2-o6js: with an auto-height body, 2400px of launcher content sized the
// row the canvas lived in, and the software renderer's 4:3 image landed below
// the fold -- a black window at 1280x720 while the engine ran normally.  The
// page is now the game (the quake-srp layout): the view is sized from the
// WINDOW, never from content, and everything else lives under it.
test('the view is sized by the window, not by the page content', () => {
  assert.match(html, /height:\s*100dvh;/, 'body needs a definite height, not just min-height');
  assert.match(html, /width:\s*max\(640px,\s*min\(100vw - var\(--chrome-h\),\s*\(100dvh - var\(--chrome-v\)\) \* 4 \/ 3\)\);/,
    'the 4:3 view box must come from the window size, leaving room for the bar');
  assert.match(html, /aspect-ratio:\s*4 \/ 3;/);
});

test('the reference and settings live in a drawer under the view, closed by default', () => {
  const stage = html.slice(html.indexOf('<main id="stage">'), html.indexOf('</main>'));
  assert.ok(stage.includes('id="canvas"'), 'the canvas is in the stage');
  assert.ok(!stage.includes('id="drawer"'), 'the drawer is outside the stage, so it can never resize the view');
  assert.match(html, /<div id="drawer" hidden>/);
  assert.ok(stage.includes('id="bar"'), 'the bar sits directly under the view');
});

test('the start overlay is the one way in, and gets out of the way while playing', () => {
  assert.match(html, /body\[data-engine-state="running"\] #overlay \{ display: none; \}/);
  // Overlay click and Enter both go through the launch button's handler.
  assert.match(app, /ui\.overlay\?\.addEventListener\('click'[\s\S]*?ui\.launchButton\.click\(\)/);
  assert.match(app, /event\.key !== 'Enter'[\s\S]*?ui\.launchButton\.click\(\)/);
});

test('a first visit with no game data fetches the free demo by itself', () => {
  assert.match(app, /async function maybeAutoInstallDemo\(\)/);
  assert.match(app, /getBaseAssetStatus\(\[\.\.\.state\.storedPaths\]\) !== 'none'\) return;\s*state\.autoDemoTried = true;/,
    'never over files the player has stored, and once per load');
  assert.equal([...app.matchAll(/await maybeAutoInstallDemo\(\);/g)].length, 3,
    'every path that finishes restoring stored files then tries the demo');
});

test('fullscreen takes the stage, so the chips and touch controls come along', () => {
  assert.match(app, /const canvas = ui\.viewport \?\? ui\.canvas;/);
  assert.match(html, /#wrap\.viewport:fullscreen/);
});
