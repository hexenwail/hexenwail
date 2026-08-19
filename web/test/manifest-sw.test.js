import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const manifest = JSON.parse(readFileSync(join(repoRoot, 'web/manifest.webmanifest'), 'utf8'));
const swText = readFileSync(join(repoRoot, 'web/sw.js'), 'utf8');

test('manifest uses relative project-pages-safe paths', () => {
  assert.match(manifest.start_url, /^\.\/?$/);
  assert.match(manifest.scope, /^\.\/?$/);
  for (const icon of manifest.icons) {
    assert.match(icon.src, /^\.\//);
    assert.equal(existsSync(join(repoRoot, 'web', icon.src.replace(/^\.\//, ''))), true, `missing icon ${icon.src}`);
  }
});

test('service worker precaches repo-managed launcher assets', () => {
  const matches = [...swText.matchAll(/'\.\/([^']+)'/g)].map((match) => match[1]);
  const repoManaged = matches.filter((asset) => !asset.startsWith('hexenwail.'));
  for (const asset of repoManaged) {
    const relativePath = asset === '' ? 'index.html' : asset;
    assert.equal(existsSync(join(repoRoot, 'web', relativePath)), true, `missing precache asset ${asset}`);
  }
  assert.ok(matches.includes('hexenwail.js'));
  assert.ok(matches.includes('hexenwail.wasm'));
  assert.ok(matches.includes('lib/phone-controls.js'));
});
