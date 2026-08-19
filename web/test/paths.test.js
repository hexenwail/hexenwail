import test from 'node:test';
import assert from 'node:assert/strict';
import { hasRequiredBaseAssets, mapImportedPath, sanitizeRelativePath } from '../lib/paths.js';

test('sanitizeRelativePath rejects traversal and absolute paths', () => {
  assert.equal(sanitizeRelativePath('../data1/pak0.pak'), null);
  assert.equal(sanitizeRelativePath('/data1/pak0.pak'), null);
  assert.equal(sanitizeRelativePath('C:/data1/pak0.pak'), null);
  assert.equal(sanitizeRelativePath('data1/./pak0.pak'), 'data1/pak0.pak');
});

test('mapImportedPath recognizes common Hexen II asset layouts', () => {
  assert.equal(mapImportedPath('pak0.pak'), 'data1/pak0.pak');
  assert.equal(mapImportedPath('pak1.pak'), 'data1/pak1.pak');
  assert.equal(mapImportedPath('pak3.pak'), 'portals/pak3.pak');
  assert.equal(mapImportedPath('Hexen II/data1/pak1.pak'), 'data1/pak1.pak');
  assert.equal(mapImportedPath('music/track02.ogg'), 'data1/music/track02.ogg');
  assert.equal(mapImportedPath('Mods/portals/progs.dat'), 'portals/progs.dat');
  assert.equal(mapImportedPath('README.txt'), null);
});

test('hasRequiredBaseAssets detects the mandatory retail pak set', () => {
  assert.equal(hasRequiredBaseAssets(['data1/pak0.pak']), false);
  assert.equal(hasRequiredBaseAssets(['data1/pak0.pak', 'data1/pak1.pak']), true);
});
