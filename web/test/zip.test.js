import test from 'node:test';
import assert from 'node:assert/strict';
import { deflateRawSync } from 'node:zlib';
import { extractZipEntries, parseZipCentralDirectory } from '../lib/zip.js';

function makeCrcTable() {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i += 1) {
    let c = i;
    for (let j = 0; j < 8; j += 1) {
      c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    table[i] = c >>> 0;
  }
  return table;
}
const CRC_TABLE = makeCrcTable();
function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc = CRC_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function buildZip(entries) {
  const encoder = new TextEncoder();
  const localParts = [];
  const centralParts = [];
  let offset = 0;

  for (const entry of entries) {
    const nameBytes = encoder.encode(entry.name);
    const sourceBytes = Buffer.from(entry.contents);
    const compressed = entry.method === 8 ? deflateRawSync(sourceBytes) : sourceBytes;
    const crc = crc32(sourceBytes);

    const localHeader = Buffer.alloc(30);
    localHeader.writeUInt32LE(0x04034b50, 0);
    localHeader.writeUInt16LE(20, 4);
    localHeader.writeUInt16LE(0, 6);
    localHeader.writeUInt16LE(entry.method, 8);
    localHeader.writeUInt16LE(0, 10);
    localHeader.writeUInt16LE(0, 12);
    localHeader.writeUInt32LE(crc, 14);
    localHeader.writeUInt32LE(compressed.length, 18);
    localHeader.writeUInt32LE(sourceBytes.length, 22);
    localHeader.writeUInt16LE(nameBytes.length, 26);
    localHeader.writeUInt16LE(0, 28);

    localParts.push(localHeader, Buffer.from(nameBytes), compressed);

    const centralHeader = Buffer.alloc(46);
    centralHeader.writeUInt32LE(0x02014b50, 0);
    centralHeader.writeUInt16LE(20, 4);
    centralHeader.writeUInt16LE(20, 6);
    centralHeader.writeUInt16LE(0, 8);
    centralHeader.writeUInt16LE(entry.method, 10);
    centralHeader.writeUInt16LE(0, 12);
    centralHeader.writeUInt16LE(0, 14);
    centralHeader.writeUInt32LE(crc, 16);
    centralHeader.writeUInt32LE(compressed.length, 20);
    centralHeader.writeUInt32LE(sourceBytes.length, 24);
    centralHeader.writeUInt16LE(nameBytes.length, 28);
    centralHeader.writeUInt16LE(0, 30);
    centralHeader.writeUInt16LE(0, 32);
    centralHeader.writeUInt16LE(0, 34);
    centralHeader.writeUInt16LE(0, 36);
    centralHeader.writeUInt32LE(0, 38);
    centralHeader.writeUInt32LE(offset, 42);

    centralParts.push(centralHeader, Buffer.from(nameBytes));
    offset += localHeader.length + nameBytes.length + compressed.length;
  }

  const centralDirectory = Buffer.concat(centralParts);
  const endOfCentralDirectory = Buffer.alloc(22);
  endOfCentralDirectory.writeUInt32LE(0x06054b50, 0);
  endOfCentralDirectory.writeUInt16LE(0, 4);
  endOfCentralDirectory.writeUInt16LE(0, 6);
  endOfCentralDirectory.writeUInt16LE(entries.length, 8);
  endOfCentralDirectory.writeUInt16LE(entries.length, 10);
  endOfCentralDirectory.writeUInt32LE(centralDirectory.length, 12);
  endOfCentralDirectory.writeUInt32LE(offset, 16);
  endOfCentralDirectory.writeUInt16LE(0, 20);

  return new Uint8Array(Buffer.concat([...localParts, centralDirectory, endOfCentralDirectory]));
}

test('parseZipCentralDirectory reads stored and deflated entries', async () => {
  const archive = buildZip([
    { name: 'data1/pak0.pak', contents: 'registered-data', method: 8 },
    { name: 'music/track02.ogg', contents: 'music-data', method: 0 },
  ]);

  const entries = parseZipCentralDirectory(archive);
  assert.equal(entries.length, 2);
  assert.equal(entries[0].name, 'data1/pak0.pak');
  assert.equal(entries[1].name, 'music/track02.ogg');

  const extracted = await extractZipEntries(archive);
  assert.equal(extracted.length, 2);
  assert.equal(Buffer.from(extracted[0].data).toString('utf8'), 'registered-data');
  assert.equal(Buffer.from(extracted[1].data).toString('utf8'), 'music-data');
});

test('extractZipEntries rejects traversal paths', async () => {
  const archive = buildZip([{ name: '../evil.txt', contents: 'nope', method: 0 }]);
  await assert.rejects(() => extractZipEntries(archive), /Rejected unsafe ZIP path/);
});

test('parseZipCentralDirectory enforces resource limits', () => {
  const archive = buildZip([{ name: 'data1/pak0.pak', contents: '1234567890', method: 0 }]);
  assert.throws(() => parseZipCentralDirectory(archive, { maxTotalUncompressedSize: 4 }), /allowed total size/);
});
