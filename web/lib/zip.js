import { sanitizeRelativePath } from './paths.js';

export const ZIP_LIMITS = Object.freeze({
  maxEntries: 4096,
  maxSingleEntrySize: 512 * 1024 * 1024,
  maxTotalUncompressedSize: 2 * 1024 * 1024 * 1024,
});

const EOCD_SIGNATURE = 0x06054b50;
const CENTRAL_DIR_SIGNATURE = 0x02014b50;
const LOCAL_FILE_SIGNATURE = 0x04034b50;

function readUInt16(view, offset) {
  return view.getUint16(offset, true);
}

function readUInt32(view, offset) {
  return view.getUint32(offset, true);
}

function assertZip(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function mergeLimits(overrides = {}) {
  return { ...ZIP_LIMITS, ...overrides };
}

function findEndOfCentralDirectory(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const minOffset = Math.max(0, bytes.byteLength - 0xffff - 22);
  for (let offset = bytes.byteLength - 22; offset >= minOffset; offset -= 1) {
    if (readUInt32(view, offset) === EOCD_SIGNATURE) {
      return { view, offset };
    }
  }
  throw new Error('ZIP end-of-central-directory record not found');
}

export function parseZipCentralDirectory(bytes, limitOverrides) {
  const limits = mergeLimits(limitOverrides);
  const { view, offset: eocdOffset } = findEndOfCentralDirectory(bytes);
  const entryCount = readUInt16(view, eocdOffset + 10);
  const centralDirectorySize = readUInt32(view, eocdOffset + 12);
  const centralDirectoryOffset = readUInt32(view, eocdOffset + 16);

  assertZip(entryCount < 0xffff, 'ZIP64 archives are not supported');
  assertZip(centralDirectoryOffset + centralDirectorySize <= bytes.byteLength, 'ZIP central directory exceeds archive size');
  assertZip(entryCount <= limits.maxEntries, `ZIP contains too many entries (${entryCount} > ${limits.maxEntries})`);

  const decoder = new TextDecoder('utf-8');
  let totalUncompressedSize = 0;
  let cursor = centralDirectoryOffset;
  const entries = [];

  for (let index = 0; index < entryCount; index += 1) {
    assertZip(cursor + 46 <= bytes.byteLength, 'ZIP central directory entry truncated');
    assertZip(readUInt32(view, cursor) === CENTRAL_DIR_SIGNATURE, 'ZIP central directory entry signature mismatch');

    const compressionMethod = readUInt16(view, cursor + 10);
    const compressedSize = readUInt32(view, cursor + 20);
    const uncompressedSize = readUInt32(view, cursor + 24);
    const nameLength = readUInt16(view, cursor + 28);
    const extraLength = readUInt16(view, cursor + 30);
    const commentLength = readUInt16(view, cursor + 32);
    const localHeaderOffset = readUInt32(view, cursor + 42);
    const crc32 = readUInt32(view, cursor + 16);

    assertZip(compressedSize !== 0xffffffff && uncompressedSize !== 0xffffffff, 'ZIP64 entries are not supported');
    assertZip(uncompressedSize <= limits.maxSingleEntrySize, `ZIP entry is too large (${uncompressedSize} bytes)`);
    totalUncompressedSize += uncompressedSize;
    assertZip(totalUncompressedSize <= limits.maxTotalUncompressedSize, 'ZIP expands beyond the allowed total size');

    const nameStart = cursor + 46;
    const nameEnd = nameStart + nameLength;
    assertZip(nameEnd <= bytes.byteLength, 'ZIP entry name is truncated');
    const rawName = decoder.decode(bytes.subarray(nameStart, nameEnd));

    entries.push({
      rawName,
      name: sanitizeRelativePath(rawName),
      compressionMethod,
      compressedSize,
      uncompressedSize,
      localHeaderOffset,
      crc32,
      isDirectory: rawName.endsWith('/'),
    });

    cursor = nameEnd + extraLength + commentLength;
  }

  Object.defineProperty(entries, 'totalUncompressedSize', {
    value: totalUncompressedSize,
    enumerable: false,
  });
  return entries;
}

async function inflateRaw(bytes) {
  if (typeof DecompressionStream !== 'undefined') {
    const stream = new Response(bytes).body.pipeThrough(new DecompressionStream('deflate-raw'));
    const result = await new Response(stream).arrayBuffer();
    return new Uint8Array(result);
  }

  if (typeof process !== 'undefined' && process.versions?.node) {
    const { inflateRawSync } = await import('node:zlib');
    return new Uint8Array(inflateRawSync(Buffer.from(bytes)));
  }

  throw new Error('No deflate decompressor available in this environment');
}

async function extractEntryData(bytes, entry) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const headerOffset = entry.localHeaderOffset;
  assertZip(headerOffset + 30 <= bytes.byteLength, 'ZIP local file header truncated');
  assertZip(readUInt32(view, headerOffset) === LOCAL_FILE_SIGNATURE, 'ZIP local file header signature mismatch');

  const nameLength = readUInt16(view, headerOffset + 26);
  const extraLength = readUInt16(view, headerOffset + 28);
  const dataStart = headerOffset + 30 + nameLength + extraLength;
  const dataEnd = dataStart + entry.compressedSize;
  assertZip(dataEnd <= bytes.byteLength, 'ZIP file data truncated');

  const compressed = bytes.subarray(dataStart, dataEnd);
  if (entry.compressionMethod === 0) {
    return compressed.slice();
  }
  if (entry.compressionMethod === 8) {
    const inflated = await inflateRaw(compressed);
    assertZip(inflated.byteLength === entry.uncompressedSize, 'ZIP entry size mismatch after inflate');
    return inflated;
  }

  throw new Error(`Unsupported ZIP compression method: ${entry.compressionMethod}`);
}

export async function extractZipEntries(bytes, options = {}) {
  const entries = parseZipCentralDirectory(bytes, options.limits);
  const totalBytes = entries.totalUncompressedSize;
  const files = [];
  let processed = 0;

  for (const entry of entries) {
    if (entry.isDirectory) {
      continue;
    }
    if (!entry.name) {
      throw new Error(`Rejected unsafe ZIP path: ${entry.rawName}`);
    }
    const data = await extractEntryData(bytes, entry);
    files.push({
      name: entry.name,
      rawName: entry.rawName,
      data,
      uncompressedSize: entry.uncompressedSize,
      compressionMethod: entry.compressionMethod,
      crc32: entry.crc32,
    });
    processed += entry.uncompressedSize;
    options.onProgress?.({ processedBytes: processed, totalBytes, entry });
  }

  return files;
}
