import test from 'node:test';
import assert from 'node:assert/strict';
import {
  buildEngineArguments, detectModGamedirs, hasMissionPack, isModAssetPath, mapModImportPath, MISSION_PACK,
  modImportRoot, modOwnedPath, modPanelNote, parseLaunchChoice,
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
  // ("portals" is the mission pack choice, never -game portals; see below.)
  assert.deepEqual(buildEngineArguments(BASE, 'data1'), BASE);
  assert.deepEqual(buildEngineArguments(BASE, 'hw'), BASE);
  assert.ok(!buildEngineArguments(BASE, 'portals').includes('-game'));
  // Never let a stored preference smuggle a path or a second flag through.
  assert.deepEqual(buildEngineArguments(BASE, '../data1'), BASE);
  assert.deepEqual(buildEngineArguments(BASE, '-nosound'), ['-basedir', '/persistent', '-game', 'nosound']);
  assert.equal(buildEngineArguments(BASE, 'testmod').length, 4);
});

test('buildEngineArguments does not mutate the base arguments', () => {
  const base = [...BASE];
  buildEngineArguments(base, 'testmod');
  buildEngineArguments(base, MISSION_PACK);
  assert.deepEqual(base, BASE);
});

// GitHub #100: the mission pack launches from its own flag, because FS_Gamedir
// ignores -game portals.
test('the mission pack choice launches with -portals and nothing else', () => {
  assert.equal(MISSION_PACK, 'portals');
  assert.deepEqual(buildEngineArguments(BASE, MISSION_PACK), ['-basedir', '/persistent', '-portals']);
  // Only the exact sentinel: a near miss is not a quiet way to get -portals.
  assert.deepEqual(buildEngineArguments(BASE, 'Portals'), BASE);
  assert.deepEqual(buildEngineArguments(BASE, ' portals'), BASE);
});

test('hasMissionPack wants portals/pak3.pak exactly where the engine opens it', () => {
  assert.equal(hasMissionPack(['data1/pak0.pak', 'data1/pak1.pak', 'portals/pak3.pak']), true);
  assert.equal(hasMissionPack(['portals/pak3.pak']), true);
  assert.equal(hasMissionPack([]), false);
  assert.equal(hasMissionPack(['data1/pak0.pak', 'data1/pak1.pak']), false);
  // Loose portals files alone are not an installation: no pak3, no GAME_PORTALS.
  assert.equal(hasMissionPack(['portals/progs.dat', 'portals/strings.txt']), false);
  // FS_AddGameDirectory opens "portals/pak3.pak" by that exact name on a
  // case-sensitive filesystem; an older import stored differently is not
  // offered, since the engine would not load it.
  assert.equal(hasMissionPack(['PORTALS/pak3.pak']), false);
  assert.equal(hasMissionPack(['portals/PAK3.PAK']), false);
  assert.equal(hasMissionPack(['mymod/portals/pak3.pak']), false);
  assert.equal(hasMissionPack(['data1/pak3.pak']), false);
  // A mission pack converted by tools/pak2pk3.py (#129) is a portals pk3.
  assert.equal(hasMissionPack(['portals/000-pak3.pk3']), true);
  assert.equal(hasMissionPack(['portals/sub/000-pak3.pk3']), false);
  assert.equal(hasMissionPack(['portals/000-pak3.PK3']), false);
});

test('a mod\'s top-level paks are stored by the name the engine mounts', () => {
  assert.equal(mapModImportPath('testmod', 'testmod/PAK0.PAK'), 'testmod/pak0.pak');
  assert.equal(mapModImportPath('testmod', 'testmod/Extra.PK3'), 'testmod/Extra.pk3');
  assert.equal(mapModImportPath('testmod', 'testmod/maps/PAK0.PAK'), 'testmod/maps/PAK0.PAK');
});

test('modPanelNote says what the selection actually does', () => {
  const note = (o) => modPanelNote({ mods: [], missionPack: false, selected: '', launchable: true, ...o });
  assert.match(note({}), /^No mods imported yet\./);
  assert.doesNotMatch(note({}), /Portal of Praevus/);
  assert.match(note({ missionPack: true }), /Portal of Praevus is installed: pick it above/);
  const mp = note({ mods: ['mymod'], missionPack: true, selected: MISSION_PACK });
  assert.match(mp, /Portal of Praevus is selected: the engine starts the mission pack \(-portals\)/);
  // The first cut said the selection went out as -game here.  It does not.
  assert.doesNotMatch(mp, /-game/);
  const mod = note({ mods: ['mymod'], missionPack: true, selected: 'mymod' });
  assert.match(mod, /“mymod” is selected and is passed to the engine as -game/);
  assert.doesNotMatch(mod, /-portals/);
  assert.match(note({ mods: ['mymod'] }), /Pick one above to pass it to the engine as -game/);
  assert.match(note({ selected: MISSION_PACK, launchable: false }), /^Portal of Praevus is selected but will not load/);
  assert.match(note({ mods: ['mymod'], selected: 'mymod', launchable: false }), /^“mymod” is selected but will not load/);
});

test('parseLaunchChoice keeps the mission pack and sanitizes everything else', () => {
  assert.equal(parseLaunchChoice('portals'), MISSION_PACK);
  assert.equal(parseLaunchChoice('testmod'), 'testmod');
  assert.equal(parseLaunchChoice(''), '');
  assert.equal(parseLaunchChoice(undefined), '');
  assert.equal(parseLaunchChoice(null), '');
  assert.equal(parseLaunchChoice(42), '');
  assert.equal(parseLaunchChoice('data1'), '');
  assert.equal(parseLaunchChoice('hw'), '');
  assert.equal(parseLaunchChoice('../portals'), '');
});
