import test from 'node:test';
import assert from 'node:assert/strict';
import { getBaseAssetStatus, mapImportedPath, sanitizeRelativePath } from '../lib/paths.js';

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

test('mapImportedPath takes the loose gamecode a demo install ships', () => {
  assert.equal(mapImportedPath('progs.dat'), 'data1/progs.dat');
  assert.equal(mapImportedPath('PROGS.DAT'), 'data1/PROGS.DAT');
  assert.equal(mapImportedPath('progs2.dat'), 'data1/progs2.dat');
  assert.equal(mapImportedPath('hexen2demo/data1/progs.dat'), 'data1/progs.dat');
  assert.equal(mapImportedPath('csprogs.dat'), null);
});

test('getBaseAssetStatus treats pak0.pak alone as launchable demo data', () => {
  assert.equal(getBaseAssetStatus([]), 'none');
  assert.equal(getBaseAssetStatus(['data1/pak1.pak']), 'none');
  assert.equal(getBaseAssetStatus(['data1/pak0.pak']), 'pak0-only');
  assert.equal(getBaseAssetStatus(['data1/pak0.pak', 'data1/progs.dat']), 'pak0-only');
  assert.equal(getBaseAssetStatus(['DATA1/PAK0.PAK']), 'pak0-only');
  assert.equal(getBaseAssetStatus(['data1/pak0.pak', 'data1/pak1.pak']), 'full');
});
