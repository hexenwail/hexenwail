import { sanitizeRelativePath, KNOWN_GAME_ROOTS } from './paths.js';
import { extractZipEntries, parseZipCentralDirectory } from './zip.js';

export const SAVE_BUNDLE_FORMAT = 'hexenwail-save';
export const SAVE_BUNDLE_VERSION = 1;
export const SAVE_MANIFEST_PATH = 'hexenwail-save.json';
export const SAVE_BUNDLE_LIMITS = Object.freeze({
  maxArchiveBytes: 256 * 1024 * 1024,
  maxFiles: 512,
  maxFileBytes: 64 * 1024 * 1024,
  maxTotalBytes: 256 * 1024 * 1024,
});

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });
const SAVE_SLOT = /^(?:s|ms)\d+$/i;
const EXCLUDED_SAVE_FILE = /\.(?:pak|ogg|wasm|js|data|cache|log)$/i;

function assertBundle(condition, message) {
  if (!condition) throw new Error(message);
}

function hex(bytes) {
  return [...bytes].map((value) => value.toString(16).padStart(2, '0')).join('');
}

export async function sha256(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return hex(new Uint8Array(digest));
}

export function isSavePath(path) {
  const safe = sanitizeRelativePath(path);
  if (!safe || EXCLUDED_SAVE_FILE.test(safe)) return false;
  const parts = safe.split('/');
  return KNOWN_GAME_ROOTS.includes(parts[0].toLowerCase()) && parts.length >= 3 && SAVE_SLOT.test(parts[1]);
}

export function saveBundlePath(path) {
  return isSavePath(path) ? `saves/${sanitizeRelativePath(path)}` : null;
}

export async function createSaveManifest(files, { createdAt = new Date().toISOString(), build, requiredPaks = [] } = {}) {
  const unique = new Set();
  const listed = [];
  for (const file of files) {
    const path = sanitizeRelativePath(file.path);
    assertBundle(path && isSavePath(path), `Not a supported save path: ${file.path}`);
    assertBundle(!unique.has(path), `Duplicate save path: ${path}`);
    unique.add(path);
    assertBundle(file.bytes instanceof Uint8Array, `Missing save bytes: ${path}`);
    assertBundle(file.bytes.byteLength <= SAVE_BUNDLE_LIMITS.maxFileBytes, `Save file is too large: ${path}`);
    listed.push({ path: saveBundlePath(path), size: file.bytes.byteLength, sha256: await sha256(file.bytes) });
  }
  listed.sort((a, b) => a.path.localeCompare(b.path));
  const gameDirectories = [...new Set(listed.map((file) => file.path.split('/')[1]))].sort();
  return {
    format: SAVE_BUNDLE_FORMAT,
    formatVersion: SAVE_BUNDLE_VERSION,
    createdAt,
    ...(build ? { build } : {}),
    gameDirectories,
    requiredPaks: requiredPaks.map(({ path, size, sha256: digest }) => ({ path, size, ...(digest ? { sha256: digest } : {}) })),
    files: listed,
  };
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const value of bytes) {
    crc ^= value;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function u16(value) {
  return Uint8Array.of(value & 0xff, (value >>> 8) & 0xff);
}

function u32(value) {
  return Uint8Array.of(value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff);
}

function join(parts) {
  const size = parts.reduce((total, part) => total + part.byteLength, 0);
  const result = new Uint8Array(size);
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.byteLength;
  }
  return result;
}

export function createStoredZip(entries) {
  assertBundle(entries.length < 0xffff, 'Too many ZIP entries');
  const local = [];
  const central = [];
  let offset = 0;
  for (const { name, bytes } of entries) {
    const nameBytes = encoder.encode(name);
    assertBundle(nameBytes.byteLength && bytes.byteLength <= 0xffffffff, 'ZIP entry is too large');
    const crc = crc32(bytes);
    const header = join([u32(0x04034b50), u16(20), u16(0), u16(0), u16(0), u16(0), u32(crc), u32(bytes.byteLength), u32(bytes.byteLength), u16(nameBytes.byteLength), u16(0)]);
    local.push(header, nameBytes, bytes);
    central.push(join([u32(0x02014b50), u16(20), u16(20), u16(0), u16(0), u16(0), u16(0), u32(crc), u32(bytes.byteLength), u32(bytes.byteLength), u16(nameBytes.byteLength), u16(0), u16(0), u16(0), u16(0), u32(0), u32(offset)]), nameBytes);
    offset += header.byteLength + nameBytes.byteLength + bytes.byteLength;
  }
  const directory = join(central);
  return join([...local, directory, u32(0x06054b50), u16(0), u16(0), u16(entries.length), u16(entries.length), u32(directory.byteLength), u32(offset), u16(0)]);
}

export async function createSaveBundle(files, options) {
  const manifest = await createSaveManifest(files, options);
  const entries = [{ name: SAVE_MANIFEST_PATH, bytes: encoder.encode(JSON.stringify(manifest, null, 2)) }];
  for (const file of [...files].sort((a, b) => a.path.localeCompare(b.path))) entries.push({ name: saveBundlePath(file.path), bytes: file.bytes });
  return { manifest, bytes: createStoredZip(entries) };
}

function isSymlink(entry) {
  return (entry.versionMadeBy >>> 8) === 3 && ((entry.externalAttributes >>> 16) & 0xf000) === 0xa000;
}

function validDigest(value) {
  return typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
}

export async function validateSaveBundle(bytes) {
  assertBundle(bytes.byteLength <= SAVE_BUNDLE_LIMITS.maxArchiveBytes, 'Save bundle is too large');
  const entries = parseZipCentralDirectory(bytes, {
    maxEntries: SAVE_BUNDLE_LIMITS.maxFiles + 1,
    maxSingleEntrySize: SAVE_BUNDLE_LIMITS.maxFileBytes,
    maxTotalUncompressedSize: SAVE_BUNDLE_LIMITS.maxTotalBytes,
  });
  const names = new Set();
  for (const entry of entries) {
    assertBundle(!entry.isDirectory && entry.name === entry.rawName && !entry.flags && !isSymlink(entry), `Unsupported or unsafe ZIP entry: ${entry.rawName}`);
    assertBundle(!names.has(entry.name), `Duplicate ZIP path: ${entry.name}`);
    names.add(entry.name);
  }
  const files = await extractZipEntries(bytes, { limits: { maxEntries: SAVE_BUNDLE_LIMITS.maxFiles + 1, maxSingleEntrySize: SAVE_BUNDLE_LIMITS.maxFileBytes, maxTotalUncompressedSize: SAVE_BUNDLE_LIMITS.maxTotalBytes } });
  const manifestFile = files.find((file) => file.name === SAVE_MANIFEST_PATH);
  assertBundle(manifestFile, 'Not a Hexenwail save bundle: manifest is missing');
  let manifest;
  try { manifest = JSON.parse(decoder.decode(manifestFile.data)); } catch { throw new Error('Save bundle manifest is not valid JSON'); }
  assertBundle(manifest?.format === SAVE_BUNDLE_FORMAT, 'Unsupported save bundle format');
  assertBundle(Number.isInteger(manifest.formatVersion) && manifest.formatVersion === SAVE_BUNDLE_VERSION, `Unsupported save bundle version: ${manifest?.formatVersion}`);
  assertBundle(typeof manifest.createdAt === 'string' && !Number.isNaN(Date.parse(manifest.createdAt)), 'Save bundle manifest has an invalid creation date');
  assertBundle(Array.isArray(manifest.gameDirectories) && manifest.gameDirectories.every((dir) => typeof dir === 'string' && KNOWN_GAME_ROOTS.includes(dir.toLowerCase())) && new Set(manifest.gameDirectories.map((dir) => dir.toLowerCase())).size === manifest.gameDirectories.length, 'Save bundle manifest has invalid game directories');
  manifest.gameDirectories = manifest.gameDirectories.map((dir) => dir.toLowerCase());
  assertBundle(Array.isArray(manifest.requiredPaks) && Array.isArray(manifest.files) && manifest.files.length > 0 && manifest.files.length <= SAVE_BUNDLE_LIMITS.maxFiles, 'Save bundle manifest is invalid');
  const pakPaths = new Set();
  for (const pak of manifest.requiredPaks) {
    const path = sanitizeRelativePath(pak?.path);
    assertBundle(path === pak?.path && /^(?:data1|portals|hw)\/.+\.pak$/i.test(path) && Number.isInteger(pak.size) && pak.size >= 0 && (!('sha256' in pak) || validDigest(pak.sha256)), 'Save bundle manifest contains invalid PAK compatibility data');
    assertBundle(!pakPaths.has(path), `Duplicate PAK compatibility path: ${path}`);
    pakPaths.add(path);
  }
  const declared = new Map();
  for (const file of manifest.files) {
    assertBundle(file && typeof file.path === 'string' && file.path.startsWith('saves/') && isSavePath(file.path.slice(6)) && Number.isInteger(file.size) && file.size >= 0 && file.size <= SAVE_BUNDLE_LIMITS.maxFileBytes && validDigest(file.sha256), 'Save bundle manifest contains an invalid file');
    assertBundle(!declared.has(file.path), `Duplicate manifest path: ${file.path}`);
    declared.set(file.path, file);
  }
  const declaredDirectories = [...new Set([...declared.keys()].map((path) => path.split('/')[1].toLowerCase()))].sort();
  assertBundle(JSON.stringify(declaredDirectories) === JSON.stringify([...manifest.gameDirectories].sort()), 'Save bundle manifest game directories do not match its files');
  assertBundle(files.length === declared.size + 1, 'Save bundle contains files not declared by its manifest');
  const output = [];
  for (const file of files) {
    if (file.name === SAVE_MANIFEST_PATH) continue;
    const declaration = declared.get(file.name);
    assertBundle(declaration, `Save bundle contains undeclared file: ${file.name}`);
    assertBundle(file.data.byteLength === declaration.size, `Save file size mismatch: ${file.name}`);
    assertBundle(await sha256(file.data) === declaration.sha256, `Save file hash mismatch: ${file.name}`);
    output.push({ path: file.name.slice(6), bytes: file.data });
  }
  return { manifest, files: output };
}

export function getPakCompatibilityWarnings(requiredPaks, installedPaks) {
  const installed = new Map(installedPaks.map((pak) => [pak.path.toLowerCase(), pak]));
  return requiredPaks.flatMap((pak) => {
    const local = installed.get(pak.path.toLowerCase());
    if (!local) return [`Missing required game data: ${pak.path}`];
    if (local.size !== pak.size || (pak.sha256 && local.sha256 && pak.sha256 !== local.sha256)) return [`Game data differs from this save bundle: ${pak.path}`];
    return [];
  });
}

export function planSaveImport(existingPaths, bundleFiles, mode) {
  assertBundle(mode === 'merge' || mode === 'replace', 'Invalid save import mode');
  const writes = mode === 'merge'
    ? bundleFiles.map((file) => file.path).filter((path) => existingPaths.includes(path))
    : bundleFiles.map((file) => file.path);
  const deletes = mode === 'replace' ? existingPaths.filter((path) => isSavePath(path) && !writes.includes(path)) : [];
  return { writes, deletes };
}
