import test from 'node:test';
import assert from 'node:assert/strict';
import {
  canonicalPakName, canonicalStoredPath, getBaseAssetStatus, isGamedirName, mapImportedPath, normalizeGamedirName,
  planCanonicalRenames, sanitizeGamedirName, sanitizeRelativePath,
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
  // Game roots and top-level paks are stored by the names the engine opens:
  // the gamedir by its canonical name, pakN.pak lowercase (FS_AddGameDirectory
  // has no case fallback for either).  GitHub #100.
  assert.equal(mapImportedPath('HEXEN2/PORTALS/PAK3.PAK'), 'portals/pak3.pak');
  assert.equal(mapImportedPath('Data1/PAK0.PAK'), 'data1/pak0.pak');
  assert.equal(mapImportedPath('PAK1.PAK'), 'data1/pak1.pak');
  assert.equal(mapImportedPath('PAK3.PAK'), 'portals/pak3.pak');
  assert.equal(mapImportedPath('Pak5.Pak'), 'data1/pak5.pak');
  // Loose files keep their case: the engine finds those case-insensitively,
  // and a name like PROGS.DAT is how a retail install ships it.
  assert.equal(mapImportedPath('DATA1/PROGS.DAT'), 'data1/PROGS.DAT');
  assert.equal(mapImportedPath('portals/Maps/Keep1.bsp'), 'portals/Maps/Keep1.bsp');
  // Only a pak at the top of the gamedir is a pak the engine mounts.
  assert.equal(mapImportedPath('data1/sub/PAK0.PAK'), 'data1/sub/PAK0.PAK');
  // Not a pakN name: kept as shipped.
  assert.equal(mapImportedPath('Extra.PAK'), 'data1/Extra.PAK');

  // A SoundFont lands where SF_FindSoundFont looks on the web, whatever it was
  // called and wherever it sat (GitHub #114); SF3 is not taken.
  assert.equal(mapImportedPath('TimGM6mb.sf2'), 'data1/soundfont.sf2');
  assert.equal(mapImportedPath('Sounds/GeneralMIDI.SF2'), 'data1/soundfont.sf2');
  assert.equal(mapImportedPath('data1/GeneralUser.sf2'), 'data1/soundfont.sf2');
  assert.equal(mapImportedPath('portals/x.sf2'), 'data1/soundfont.sf2');
  assert.equal(mapImportedPath('data1/soundfont.sf2'), 'data1/soundfont.sf2');
  assert.equal(mapImportedPath('FluidR3.sf3'), null);
  assert.equal(mapImportedPath('../evil.sf2'), null);
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
  // Exact names: the engine cannot open DATA1/PAK0.PAK, so it is not
  // installed.  The launcher renames such legacy files (planCanonicalRenames)
  // before it asks.  GitHub #100.
  assert.equal(getBaseAssetStatus(['DATA1/PAK0.PAK']), 'none');
  assert.equal(getBaseAssetStatus(['data1/pak0.pak', 'DATA1/PAK1.PAK']), 'pak0-only');
  assert.equal(getBaseAssetStatus(['data1/pak0.pak', 'data1/pak1.pak']), 'full');
});

// GitHub #100: the names a stored file must have for the engine to open it.
test('canonicalPakName folds what the engine matches exactly and nothing else', () => {
  assert.equal(canonicalPakName('PAK0.PAK'), 'pak0.pak');
  assert.equal(canonicalPakName('Pak12.Pak'), 'pak12.pak');
  assert.equal(canonicalPakName('pak0.pak'), 'pak0.pak');
  // pk3: only the extension, which the mount scan matches case-sensitively;
  // the name orders the mounts and keeps its case.
  assert.equal(canonicalPakName('000-Pak3.PK3'), '000-Pak3.pk3');
  assert.equal(canonicalPakName('zz-loose.pk3'), 'zz-loose.pk3');
  assert.equal(canonicalPakName('PROGS.DAT'), 'PROGS.DAT');
  assert.equal(canonicalPakName('Extra.PAK'), 'Extra.PAK');
  assert.equal(canonicalPakName('pak.pak'), 'pak.pak');
});

test('canonicalStoredPath fixes the root and top-level paks, keeps the rest', () => {
  assert.equal(canonicalStoredPath('DATA1/PAK0.PAK'), 'data1/pak0.pak');
  assert.equal(canonicalStoredPath('Portals/PAK3.PAK'), 'portals/pak3.pak');
  assert.equal(canonicalStoredPath('HW/Pak4.pak'), 'hw/pak4.pak');
  assert.equal(canonicalStoredPath('DATA1/PROGS.DAT'), 'data1/PROGS.DAT');
  assert.equal(canonicalStoredPath('DATA1/s0/INFO.DAT'), 'data1/s0/INFO.DAT');
  assert.equal(canonicalStoredPath('data1/sub/PAK0.PAK'), 'data1/sub/PAK0.PAK');
  // A mod's root is already its sanitized gamedir; its top-level paks are not.
  assert.equal(canonicalStoredPath('mymod/PAK0.PAK'), 'mymod/pak0.pak');
  assert.equal(canonicalStoredPath('mymod/Maps/E1.bsp'), 'mymod/Maps/E1.bsp');
  assert.equal(canonicalStoredPath('data1/pak0.pak'), 'data1/pak0.pak');
  assert.equal(canonicalStoredPath('../data1/pak0.pak'), null);
});

test('planCanonicalRenames moves legacy names and never picks between two files', () => {
  assert.deepEqual(planCanonicalRenames(['data1/pak0.pak', 'data1/pak1.pak', 'portals/pak3.pak']), []);
  assert.deepEqual(planCanonicalRenames(['DATA1/PAK0.PAK', 'DATA1/PAK1.PAK', 'DATA1/PROGS.DAT']), [
    ['DATA1/PAK0.PAK', 'data1/pak0.pak'],
    ['DATA1/PAK1.PAK', 'data1/pak1.pak'],
    ['DATA1/PROGS.DAT', 'data1/PROGS.DAT'],
  ]);
  // The canonical copy already exists: it is what the engine loads, so the
  // legacy one is left alone, not moved over it and not deleted.
  assert.deepEqual(planCanonicalRenames(['portals/pak3.pak', 'PORTALS/PAK3.PAK']), []);
  // Two legacy spellings of one target: only the first moves.
  assert.deepEqual(planCanonicalRenames(['DATA1/PAK0.PAK', 'Data1/pak0.PAK']), [
    ['DATA1/PAK0.PAK', 'data1/pak0.pak'],
  ]);
  assert.deepEqual(planCanonicalRenames([]), []);
});
