import test from 'node:test';
import assert from 'node:assert/strict';
import {
  createSaveBundle,
  createStoredZip,
  getPakCompatibilityWarnings,
  isPakCompatibilityPath,
  isSavePath,
  planSaveImport,
  validateSaveBundle,
} from '../lib/save-bundle.js';

const save = { path: 'data1/s0/clients.gip', bytes: new TextEncoder().encode('save state') };

test('classifies engine save slots while excluding commercial assets', () => {
  assert.equal(isSavePath('data1/s0/info.dat'), true);
  assert.equal(isSavePath('portals/ms9/level.gip'), true);
  assert.equal(isSavePath('hw/s1/clients.gip'), true);
  assert.equal(isSavePath('data1/pak0.pak'), false);
  assert.equal(isSavePath('data1/music/track01.ogg'), false);
  assert.equal(isSavePath('data1/progs.dat'), false);
});

test('an imported mod saves under its own gamedir, and only in that shape', () => {
  // -game puts fs_userdir at <basedir>/<gamedir>, so a mod's saves are never
  // under data1 and the allowlist cannot be three fixed names.
  assert.equal(isSavePath('testmod/s0/info.dat'), true);
  assert.equal(isSavePath('testmod/ms3/clients.gip'), true);
  // A mod's own content is not a save: only s<N>/ms<N> slots are exported.
  assert.equal(isSavePath('testmod/pak0.pak'), false);
  assert.equal(isSavePath('testmod/maps/e1m1.bsp'), false);
  assert.equal(isSavePath('testmod/progs.dat'), false);
  // Still an allowlist: the root has to have a gamedir's shape.
  assert.equal(isSavePath('../testmod/s0/info.dat'), false);
  assert.equal(isSavePath('My Mod/s0/info.dat'), false);
  assert.equal(isSavePath('testmod/notaslot/info.dat'), false);
  assert.equal(isPakCompatibilityPath('testmod/pak0.pak'), true);
  assert.equal(isPakCompatibilityPath('data1/pak0.pak'), true);
  assert.equal(isPakCompatibilityPath('My Mod/pak0.pak'), false);
  assert.equal(isPakCompatibilityPath('testmod/progs.dat'), false);
});

test('a save bundle round-trips an imported mod gamedir', async () => {
  const modSave = { path: 'testmod/s0/clients.gip', bytes: new TextEncoder().encode('mod save state') };
  const { bytes, manifest } = await createSaveBundle([modSave], {
    createdAt: '2026-08-20T00:00:00.000Z',
    requiredPaks: [{ path: 'testmod/pak0.pak', size: 12 }],
  });
  assert.deepEqual(manifest.gameDirectories, ['testmod']);
  const restored = await validateSaveBundle(bytes);
  assert.deepEqual(restored.files[0], modSave);
  assert.deepEqual(restored.manifest.gameDirectories, ['testmod']);
});

test('a save bundle cannot declare a game directory that is not a gamedir', async () => {
  const modSave = { path: 'testmod/s0/clients.gip', bytes: new TextEncoder().encode('mod save state') };
  const { manifest } = await createSaveBundle([modSave], { createdAt: '2026-08-20T00:00:00.000Z' });
  const escaped = structuredClone(manifest);
  escaped.gameDirectories = ['../etc'];
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(escaped)) },
    { name: 'saves/testmod/s0/clients.gip', bytes: modSave.bytes },
  ])), /invalid game directories/);
  const mismatched = structuredClone(manifest);
  mismatched.gameDirectories = ['othermod'];
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(mismatched)) },
    { name: 'saves/testmod/s0/clients.gip', bytes: modSave.bytes },
  ])), /do not match its files/);
});

test('creates and validates a versioned ZIP save bundle', async () => {
  const { bytes, manifest } = await createSaveBundle([save], { createdAt: '2026-08-07T00:00:00.000Z' });
  assert.equal(manifest.files[0].path, 'saves/data1/s0/clients.gip');
  const restored = await validateSaveBundle(bytes);
  assert.equal(restored.manifest.formatVersion, 1);
  assert.deepEqual(restored.files[0], save);
});

test('creates and validates a versioned ZIP save bundle with case-insensitive directories', async () => {
  const { bytes, manifest } = await createSaveBundle([save], { createdAt: '2026-08-07T00:00:00.000Z' });
  manifest.gameDirectories = ['DATA1'];
  manifest.files[0].path = 'saves/DATA1/s0/clients.gip';
  const modifiedZip = createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(manifest)) },
    { name: 'saves/DATA1/s0/clients.gip', bytes: save.bytes }
  ]);
  const restored = await validateSaveBundle(modifiedZip);
  assert.equal(restored.manifest.gameDirectories[0], 'data1');
});

test('rejects mismatched, duplicate, unlisted, and unsafe bundle files', async () => {
  const { manifest } = await createSaveBundle([save], { createdAt: '2026-08-07T00:00:00.000Z' });
  const changed = structuredClone(manifest);
  changed.files[0].size = 999;
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(changed)) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /size mismatch/);
  const badHash = structuredClone(manifest);
  badHash.files[0].sha256 = '0'.repeat(64);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(badHash)) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /hash mismatch/);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(manifest)) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
    { name: 'extra.txt', bytes: new Uint8Array() },
  ])), /undeclared|files not declared/);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(manifest)) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /Duplicate ZIP path/);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(manifest)) },
    { name: '../saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /unsafe/);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify({ ...manifest, formatVersion: 2 })) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /Unsupported save bundle version/);
});

test('plans merge and replace without deleting assets', () => {
  const existing = ['data1/pak0.pak', 'data1/s0/info.dat', 'data1/s1/clients.gip'];
  assert.deepEqual(planSaveImport(existing, [save], 'merge'), { writes: [], deletes: [] });
  assert.deepEqual(planSaveImport(existing, [save], 'replace'), { writes: ['data1/s0/clients.gip'], deletes: ['data1/s0/info.dat', 'data1/s1/clients.gip'] });

  const existingWithSave = ['data1/pak0.pak', 'data1/s0/clients.gip', 'data1/s1/clients.gip'];
  assert.deepEqual(planSaveImport(existingWithSave, [save], 'merge'), { writes: ['data1/s0/clients.gip'], deletes: [] });
});

test('reports missing and incompatible PAKs', () => {
  const required = [{ path: 'data1/pak0.pak', size: 10, sha256: 'a'.repeat(64) }, { path: 'portals/pak3.pak', size: 20 }];
  assert.deepEqual(getPakCompatibilityWarnings(required, [{ path: 'data1/pak0.pak', size: 9, sha256: 'b'.repeat(64) }]), [
    'Game data differs from this save bundle: data1/pak0.pak',
    'Missing required game data: portals/pak3.pak',
  ]);
});
