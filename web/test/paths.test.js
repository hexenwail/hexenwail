import test from 'node:test';
import assert from 'node:assert/strict';
import {
  getBaseAssetStatus, isGamedirName, mapImportedPath, normalizeGamedirName, sanitizeGamedirName,
  sanitizeRelativePath,
} from '../lib/paths.js';

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

test('gamedir names are folded to what FS_Gamedir accepts as a single component', () => {
  assert.equal(sanitizeGamedirName('testmod'), 'testmod');
  assert.equal(sanitizeGamedirName('TestMod'), 'testmod');
  assert.equal(sanitizeGamedirName('Keep 2.0!'), 'keep-2-0');
  assert.equal(sanitizeGamedirName('my_mod-1'), 'my_mod-1');
  // A directory picker can hand over a trailing separator or a full path.
  assert.equal(sanitizeGamedirName('C:\\games\\Hexen II\\testmod\\'), 'testmod');
  assert.equal(sanitizeGamedirName('a'.repeat(64)), 'a'.repeat(32));
});

test('gamedir names reject traversal, emptiness and the base game directories', () => {
  for (const name of ['', '   ', '.', '..', '../..', '---', '///', null, undefined]) {
    assert.equal(sanitizeGamedirName(name), null, `should reject ${JSON.stringify(name)}`);
  }
  // FS_Gamedir silently ignores these three, so they can never be a mod.
  assert.equal(sanitizeGamedirName('data1'), null);
  assert.equal(sanitizeGamedirName('DATA1'), null);
  assert.equal(sanitizeGamedirName('portals'), null);
  assert.equal(sanitizeGamedirName('hw'), null);
  // The reserved names are still well-formed, which is how the launcher tells
  // "that is the base game" apart from "that is not a usable name at all".
  assert.equal(normalizeGamedirName('portals'), 'portals');
  assert.equal(normalizeGamedirName('..'), null);
});

test('isGamedirName only accepts an already-canonical mod directory', () => {
  assert.equal(isGamedirName('testmod'), true);
  assert.equal(isGamedirName('TestMod'), false);
  assert.equal(isGamedirName('data1'), false);
  assert.equal(isGamedirName('my mod'), false);
  assert.equal(isGamedirName(''), false);
  assert.equal(isGamedirName(42), false);
});

test('getBaseAssetStatus treats pak0.pak alone as launchable demo data', () => {
  assert.equal(getBaseAssetStatus([]), 'none');
  assert.equal(getBaseAssetStatus(['data1/pak1.pak']), 'none');
  assert.equal(getBaseAssetStatus(['data1/pak0.pak']), 'pak0-only');
  assert.equal(getBaseAssetStatus(['data1/pak0.pak', 'data1/progs.dat']), 'pak0-only');
  assert.equal(getBaseAssetStatus(['DATA1/PAK0.PAK']), 'pak0-only');
  assert.equal(getBaseAssetStatus(['data1/pak0.pak', 'data1/pak1.pak']), 'full');
});
