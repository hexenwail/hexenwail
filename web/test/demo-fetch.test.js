import test from 'node:test';
import assert from 'node:assert/strict';
import { gzipSync, deflateRawSync } from 'node:zlib';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import {
  HEXEN2_DEMO_SOURCE, downloadHexen2Demo, isDemoDecompressionSupported, parseTarEntries, verifyDemoPayload,
} from '../lib/demo-fetch.js';
import { getBaseAssetStatus, mapImportedPath } from '../lib/paths.js';

const encoder = new TextEncoder();

function writeField(block, offset, text, length) {
  const bytes = encoder.encode(text);
  assert.ok(bytes.byteLength <= length, `field overflow: ${text}`);
  block.set(bytes, offset);
}

function tarHeader({ name, size, typeFlag = '0', prefix = '' }) {
  const block = new Uint8Array(512);
  writeField(block, 0, name, 100);
  writeField(block, 100, '0000644\0', 8);
  writeField(block, 108, '0000000\0', 8);
  writeField(block, 116, '0000000\0', 8);
  writeField(block, 124, `${size.toString(8).padStart(11, '0')}\0`, 12);
  writeField(block, 136, '00000000000\0', 12);
  block[156] = typeFlag.charCodeAt(0);
  writeField(block, 257, 'ustar  \0', 8);
  writeField(block, 345, prefix, 155);

  block.fill(0x20, 148, 156);
  let checksum = 0;
  for (const value of block) {
    checksum += value;
  }
  writeField(block, 148, `${checksum.toString(8).padStart(6, '0')}\0 `, 8);
  return block;
}

function buildTar(entries) {
  const blocks = [];
  for (const entry of entries) {
    const data = typeof entry.contents === 'string' ? encoder.encode(entry.contents) : (entry.contents ?? new Uint8Array(0));
    blocks.push(tarHeader({ ...entry, size: data.byteLength }));
    if (data.byteLength) {
      const padded = new Uint8Array(Math.ceil(data.byteLength / 512) * 512);
      padded.set(data);
      blocks.push(padded);
    }
  }
  blocks.push(new Uint8Array(1024));
  const total = blocks.reduce((sum, block) => sum + block.byteLength, 0);
  const tar = new Uint8Array(total);
  let offset = 0;
  for (const block of blocks) {
    tar.set(block, offset);
    offset += block.byteLength;
  }
  return tar;
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const value of bytes) {
    crc ^= value;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function buildZip(entries) {
  const locals = [];
  const centrals = [];
  let offset = 0;
  for (const entry of entries) {
    const nameBytes = encoder.encode(entry.name);
    const source = typeof entry.contents === 'string' ? encoder.encode(entry.contents) : entry.contents;
    const compressed = new Uint8Array(deflateRawSync(Buffer.from(source)));
    const crc = crc32(source);

    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(8, 8);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(compressed.byteLength, 18);
    local.writeUInt32LE(source.byteLength, 22);
    local.writeUInt16LE(nameBytes.byteLength, 26);
    locals.push(local, Buffer.from(nameBytes), Buffer.from(compressed));

    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(8, 10);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(compressed.byteLength, 20);
    central.writeUInt32LE(source.byteLength, 24);
    central.writeUInt16LE(nameBytes.byteLength, 28);
    central.writeUInt32LE(offset, 42);
    centrals.push(central, Buffer.from(nameBytes));

    offset += 30 + nameBytes.byteLength + compressed.byteLength;
  }

  const centralBytes = Buffer.concat(centrals);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(centralBytes.byteLength, 12);
  eocd.writeUInt32LE(offset, 16);
  return new Uint8Array(Buffer.concat([...locals, centralBytes, eocd]));
}

async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(digest)].map((value) => value.toString(16).padStart(2, '0')).join('');
}

// A stand-in manifest so the pipeline can be exercised end to end without any
// game data in the repository.
const PAK0 = encoder.encode('PACK fake pak0 payload for tests');
const PROGS = encoder.encode('fake progs.dat');
const RC = encoder.encode('exec default.cfg\n');

async function makeTestConfig() {
  return {
    ...HEXEN2_DEMO_SOURCE,
    local: { label: 'this site', url: './demo/hexen2demo.zip' },
    remote: { label: 'test mirror', url: 'https://example.invalid/demo.tgz', downloadBytes: 0 },
    files: {
      'data1/pak0.pak': { size: PAK0.byteLength, sha256: await sha256Hex(PAK0), required: true },
      'data1/progs.dat': { size: PROGS.byteLength, sha256: await sha256Hex(PROGS), required: true },
      'data1/hexen.rc': { size: RC.byteLength, sha256: await sha256Hex(RC) },
    },
  };
}

function demoTar() {
  return gzipSync(Buffer.from(buildTar([
    { name: 'hexen2demo_nov1997/', typeFlag: '5' },
    { name: 'hexen2demo_nov1997/glhexen2', contents: 'ELF engine binary, must be ignored' },
    { name: 'hexen2demo_nov1997/data1/', typeFlag: '5' },
    { name: 'hexen2demo_nov1997/data1/pak0.pak', contents: PAK0 },
    { name: 'hexen2demo_nov1997/data1/progs.dat', contents: PROGS },
    { name: 'hexen2demo_nov1997/data1/hexen.rc', contents: RC },
    { name: 'hexen2demo_nov1997/docs/README', contents: 'engine docs, must be ignored' },
  ])));
}

function stubFetch(routes) {
  return async (url, options = {}) => {
    const handler = routes[url];
    if (!handler) {
      return new Response(null, { status: 404 });
    }
    if (options.method === 'HEAD') {
      return new Response(null, { status: 200 });
    }
    const bytes = handler();
    return new Response(bytes, { status: 200, headers: { 'content-length': String(bytes.byteLength) } });
  };
}

test('tar parser reads regular files and skips directories', () => {
  const tar = buildTar([
    { name: 'demo/', typeFlag: '5' },
    { name: 'demo/a.txt', contents: 'alpha' },
    { name: 'demo/b.bin', contents: new Uint8Array([1, 2, 3, 4, 5]) },
  ]);
  const entries = parseTarEntries(tar);
  assert.deepEqual(entries.map((entry) => entry.name), ['demo/a.txt', 'demo/b.bin']);
  assert.equal(entries[0].size, 5);
  assert.equal(new TextDecoder().decode(entries[0].data), 'alpha');
  assert.deepEqual([...entries[1].data], [1, 2, 3, 4, 5]);
});

test('tar parser honours the ustar prefix and GNU long names', () => {
  const longName = `demo/${'nested/'.repeat(16)}deep.txt`;
  const tar = buildTar([
    { name: 'split.txt', prefix: 'demo/data1', contents: 'prefixed' },
    { name: '././@LongLink', typeFlag: 'L', contents: longName },
    { name: 'demo/nested/nested/nes', contents: 'long' },
  ]);
  const entries = parseTarEntries(tar);
  assert.deepEqual(entries.map((entry) => entry.name), ['demo/data1/split.txt', longName]);
  assert.equal(new TextDecoder().decode(entries[1].data), 'long');
});

test('tar parser rejects a corrupt header instead of guessing', () => {
  const tar = buildTar([{ name: 'demo/a.txt', contents: 'alpha' }]);
  tar[10] ^= 0xff;
  assert.throws(() => parseTarEntries(tar), /checksum mismatch/);
});

test('tar parser skips copying data for filtered-out members', () => {
  const tar = buildTar([
    { name: 'demo/data1/pak0.pak', contents: 'keep' },
    { name: 'demo/glhexen2', contents: 'drop' },
  ]);
  const entries = parseTarEntries(tar, { filter: (name) => name.endsWith('.pak') });
  assert.equal(entries.length, 2);
  assert.equal(new TextDecoder().decode(entries[0].data), 'keep');
  assert.equal(entries[1].data, null);
});

test('the pinned demo manifest describes a launchable pak0-only install', () => {
  const paths = Object.keys(HEXEN2_DEMO_SOURCE.files);
  assert.equal(getBaseAssetStatus(paths), 'pak0-only');
  for (const path of paths) {
    // Members reach their install path through the ordinary import mapping, so
    // a manifest key that mapImportedPath would rewrite could never be matched.
    assert.equal(mapImportedPath(path), path, `${path} is not a stable import path`);
    const entry = HEXEN2_DEMO_SOURCE.files[path];
    assert.match(entry.sha256, /^[0-9a-f]{64}$/, `${path} needs a sha256`);
    assert.ok(Number.isInteger(entry.size) && entry.size > 0, `${path} needs a size`);
  }
  // pak0.pak is the demo and the demo pak0 carries no progs.dat, so the loose
  // gamecode is required too; everything else is optional.
  assert.deepEqual(paths.filter((path) => HEXEN2_DEMO_SOURCE.files[path].required),
    ['data1/pak0.pak', 'data1/progs.dat']);
});

test('the demo source config keeps both fetch URLs in one place', () => {
  assert.match(HEXEN2_DEMO_SOURCE.local.url, /^\.\//, 'the local override must be same-origin relative');
  assert.match(HEXEN2_DEMO_SOURCE.remote.url, /^https:\/\//);
  assert.match(HEXEN2_DEMO_SOURCE.remote.archiveSha256, /^[0-9a-f]{64}$/);
  assert.ok(HEXEN2_DEMO_SOURCE.remote.downloadBytes > 0);
  assert.ok(HEXEN2_DEMO_SOURCE.maxDownloadBytes > HEXEN2_DEMO_SOURCE.remote.downloadBytes);
  assert.ok(Object.isFrozen(HEXEN2_DEMO_SOURCE));
});

test('the service worker never precaches the optional demo archive', () => {
  const swText = readFileSync(join(process.cwd(), 'web/sw.js'), 'utf8');
  assert.doesNotMatch(swText, /['"]\.\/demo\//, 'demo/ must stay out of the precache list');
  assert.equal(swText.includes(HEXEN2_DEMO_SOURCE.remote.url), false);
  // Cross-origin requests fall through to the network untouched, which is what
  // keeps a 13 MB download out of the offline shell cache.
  assert.match(swText, /url\.origin !== self\.location\.origin/);
});

test('verifyDemoPayload installs only manifest files and ignores the rest', async () => {
  const config = await makeTestConfig();
  const { files, ignored } = await verifyDemoPayload([
    { name: 'hexen2demo_nov1997/data1/progs.dat', data: PROGS },
    { name: 'hexen2demo_nov1997/data1/pak0.pak', data: PAK0 },
    { name: 'hexen2demo_nov1997/glhexen2', data: encoder.encode('binary') },
    { name: 'hexen2demo_nov1997/docs/README', data: encoder.encode('docs') },
  ], config);

  assert.deepEqual(files.map((file) => file.path), ['data1/pak0.pak', 'data1/progs.dat']);
  assert.deepEqual(ignored, ['hexen2demo_nov1997/glhexen2', 'hexen2demo_nov1997/docs/README']);
});

test('verifyDemoPayload refuses a tampered file and a short archive', async () => {
  const config = await makeTestConfig();
  await assert.rejects(() => verifyDemoPayload([
    { name: 'data1/pak0.pak', data: encoder.encode('PACK fake pak0 payload for TESTS') },
    { name: 'data1/progs.dat', data: PROGS },
  ], config), /pak0\.pak failed verification/);

  await assert.rejects(() => verifyDemoPayload([
    { name: 'data1/pak0.pak', data: PAK0 },
  ], config), /missing data1\/progs\.dat/);

  await assert.rejects(() => verifyDemoPayload([
    { name: 'data1/pak0.pak', data: PAK0.subarray(0, 8) },
    { name: 'data1/progs.dat', data: PROGS },
  ], config), /expected \d+/);
});

test('downloadHexen2Demo falls back to the remote tarball when no local copy exists', async (t) => {
  if (!isDemoDecompressionSupported()) {
    t.skip('DecompressionStream is unavailable');
    return;
  }
  const config = await makeTestConfig();
  const stages = [];
  const result = await downloadHexen2Demo({
    config,
    fetchImpl: stubFetch({ [config.remote.url]: demoTar }),
    onStage: (event) => stages.push(event.stage),
  });

  assert.equal(result.source.label, 'test mirror');
  assert.deepEqual(result.files.map((file) => file.path), ['data1/pak0.pak', 'data1/progs.dat', 'data1/hexen.rc']);
  assert.equal(new TextDecoder().decode(result.files[0].bytes), 'PACK fake pak0 payload for tests');
  assert.ok(stages.includes('downloading') && stages.includes('extracting') && stages.includes('verifying'));
  assert.equal(getBaseAssetStatus(result.files.map((file) => file.path)), 'pak0-only');
});

test('downloadHexen2Demo prefers a same-origin ZIP when one is published', async (t) => {
  if (!isDemoDecompressionSupported()) {
    t.skip('DecompressionStream is unavailable');
    return;
  }
  const config = await makeTestConfig();
  const zip = () => buildZip([
    { name: 'data1/pak0.pak', contents: PAK0 },
    { name: 'data1/progs.dat', contents: PROGS },
  ]);
  const result = await downloadHexen2Demo({
    config,
    fetchImpl: stubFetch({ [config.local.url]: zip, [config.remote.url]: demoTar }),
  });

  assert.equal(result.source.label, 'this site');
  assert.deepEqual(result.files.map((file) => file.path), ['data1/pak0.pak', 'data1/progs.dat']);
});

test('downloadHexen2Demo reports a failed download instead of installing anything', async (t) => {
  if (!isDemoDecompressionSupported()) {
    t.skip('DecompressionStream is unavailable');
    return;
  }
  const config = await makeTestConfig();
  await assert.rejects(() => downloadHexen2Demo({ config, fetchImpl: stubFetch({}) }), /responded 404/);

  await assert.rejects(() => downloadHexen2Demo({
    config,
    fetchImpl: stubFetch({ [config.remote.url]: () => encoder.encode('<html>not an archive</html>') }),
  }), /neither a gzip nor a ZIP archive/);

  await assert.rejects(() => downloadHexen2Demo({
    config: { ...config, maxDownloadBytes: 16 },
    fetchImpl: stubFetch({ [config.remote.url]: demoTar }),
  }), /limit/);
});

test('downloadHexen2Demo warns but proceeds when an equivalent repack is served', async (t) => {
  if (!isDemoDecompressionSupported()) {
    t.skip('DecompressionStream is unavailable');
    return;
  }
  const base = await makeTestConfig();
  const config = { ...base, remote: { ...base.remote, archiveSha256: 'f'.repeat(64) } };
  const result = await downloadHexen2Demo({
    config,
    fetchImpl: stubFetch({ [config.remote.url]: demoTar }),
  });

  assert.equal(result.files.length, 3);
  assert.match(result.warnings[0], /not the pinned/);
});
