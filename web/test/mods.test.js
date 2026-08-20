import test from 'node:test';
import assert from 'node:assert/strict';
import {
  buildEngineArguments, detectModGamedirs, isModAssetPath, mapModImportPath, modImportRoot, modOwnedPath,
} from '../lib/mods.js';

const BASE = ['-basedir', '/persistent'];

test('mapModImportPath roots a picked folder at its own gamedir', () => {
  assert.equal(mapModImportPath('testmod', 'testmod/progs.dat'), 'testmod/progs.dat');
  assert.equal(mapModImportPath('testmod', 'testmod/maps/start.bsp'), 'testmod/maps/start.bsp');
  // Interior case is the mod author's business; only the gamedir is folded.
  assert.equal(mapModImportPath('testmod', 'TestMod/Models/Hero.mdl'), 'testmod/Models/Hero.mdl');
  // A mod's pak0 stays in the mod, where -game puts it above data1's.
  assert.equal(mapModImportPath('testmod', 'testmod/pak0.pak'), 'testmod/pak0.pak');
});

test('mapModImportPath refuses anything outside the picked folder', () => {
  assert.equal(mapModImportPath('testmod', 'othermod/progs.dat'), null);
  assert.equal(mapModImportPath('testmod', '../testmod/progs.dat'), null);
  assert.equal(mapModImportPath('testmod', '/testmod/progs.dat'), null);
  assert.equal(mapModImportPath('testmod', 'testmod'), null);
  assert.equal(mapModImportPath('data1', 'data1/progs.dat'), null);
  assert.equal(mapModImportPath('portals', 'portals/pak3.pak'), null);
});

test('mod imports accept engine-readable file types and drop the rest', () => {
  for (const name of ['progs.dat', 'autoexec.cfg', 'hexen.rc', 'pak0.pak', 'maps/e1m1.bsp',
    'maps/e1m1.lit', 'models/hero.mdl', 'gfx/menu.lmp', 'sound/hit.wav', 'music/track02.ogg',
    'textures/wall.tga', 'readme.txt']) {
    assert.equal(isModAssetPath(name), true, `should accept ${name}`);
  }
  for (const name of ['setup.exe', 'installer.zip', 'payload.js', 'engine.wasm', 'data1/s0/clients.gip',
    '.DS_Store', 'Thumbs.db', 'noextension']) {
    assert.equal(isModAssetPath(name), false, `should reject ${name}`);
  }
});

test('modImportRoot demands one shared folder name', () => {
  assert.equal(modImportRoot(['testmod/progs.dat', 'testmod/maps/a.bsp']), 'testmod');
  assert.equal(modImportRoot(['testmod/progs.dat', 'other/progs.dat']), null);
  // A hand-picked loose file has no folder to name a gamedir after.
  assert.equal(modImportRoot(['progs.dat']), null);
});

test('detectModGamedirs finds stored gamedirs and ignores the base game', () => {
  assert.deepEqual(detectModGamedirs([
    'data1/pak0.pak', 'data1/s0/info.dat', 'portals/pak3.pak', 'hw/pak4.pak',
    'testmod/progs.dat', 'keep/maps/a.bsp', 'keep/s1/info.dat', 'loose.pak',
  ]), ['keep', 'testmod']);
  assert.deepEqual(detectModGamedirs([]), []);
  // Never written by the launcher, so never trusted as a gamedir either.
  assert.deepEqual(detectModGamedirs(['MixedCase/progs.dat', 'has space/progs.dat']), []);
});

test('modOwnedPath scopes deletion to one gamedir', () => {
  assert.equal(modOwnedPath('testmod', 'testmod/progs.dat'), true);
  assert.equal(modOwnedPath('testmod', 'testmod/s0/info.dat'), true);
  assert.equal(modOwnedPath('testmod', 'testmod2/progs.dat'), false);
  assert.equal(modOwnedPath('testmod', 'data1/pak0.pak'), false);
  assert.equal(modOwnedPath('data1', 'data1/pak0.pak'), false);
});

test('buildEngineArguments appends -game only for a real mod', () => {
  assert.deepEqual(buildEngineArguments(BASE, ''), BASE);
  assert.deepEqual(buildEngineArguments(BASE, null), BASE);
  assert.deepEqual(buildEngineArguments(BASE, undefined), BASE);
  assert.deepEqual(buildEngineArguments(BASE, 'testmod'), ['-basedir', '/persistent', '-game', 'testmod']);
  // FS_Gamedir ignores these, so an argument pair for them would be a lie.
  assert.deepEqual(buildEngineArguments(BASE, 'data1'), BASE);
  assert.deepEqual(buildEngineArguments(BASE, 'portals'), BASE);
  assert.deepEqual(buildEngineArguments(BASE, 'hw'), BASE);
  // Never let a stored preference smuggle a path or a second flag through.
  assert.deepEqual(buildEngineArguments(BASE, '../data1'), BASE);
  assert.deepEqual(buildEngineArguments(BASE, '-nosound'), ['-basedir', '/persistent', '-game', 'nosound']);
  assert.equal(buildEngineArguments(BASE, 'testmod').length, 4);
});

test('buildEngineArguments does not mutate the base arguments', () => {
  const base = [...BASE];
  buildEngineArguments(base, 'testmod');
  assert.deepEqual(base, BASE);
});
