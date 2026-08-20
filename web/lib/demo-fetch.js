import { mapImportedPath } from './paths.js';
import { extractZipEntries } from './zip.js';

// Everything host-specific about the demo download lives in this one object so
// that pointing a deployment at a different copy is an edit here, not a code
// change.  Hexenwail hosts none of these bytes itself: the browser fetches them
// from a third party, exactly as scripts/get_demo.sh does from the shell.  See
// assets/demo/README.md for why the project does not mirror Raven's data.
export const HEXEN2_DEMO_SOURCE = Object.freeze({
  // Probed first.  Absent from the public Pages deploy; a self-hoster drops an
  // archive here to serve the demo from their own origin (docs/PWA.md).
  local: Object.freeze({
    label: 'this site',
    url: './demo/hexen2demo.zip',
  }),
  // Fallback.  archive.org's /cors/ endpoint echoes the requesting Origin, which
  // is what makes this fetchable from a page at all -- SourceForge, the upstream
  // home of the same tarball, sends no CORS headers and serves browser
  // user-agents an interstitial (see scripts/get_demo.sh).
  remote: Object.freeze({
    label: 'archive.org',
    url: 'https://archive.org/cors/hexen2demo_nov1997-linux-x86_64/hexen2demo_nov1997-linux-x86_64.tgz',
    downloadBytes: 13244368,
    // Advisory only: identifies the exact tarball this was tested against.  A
    // mismatch is logged, not fatal, because the per-file digests below are the
    // real gate and other packagings of the same demo (upstream's i586 tarball,
    // a self-hoster's ZIP) carry identical game files in a different container.
    archiveSha256: 'e26e0f2de6f8fba9577d6ffcf9e3548c291325443b626f46fd83f57709ce45be',
  }),

  maxDownloadBytes: 64 * 1024 * 1024,
  maxArchiveBytes: 256 * 1024 * 1024,

  // The Nov-1997 demo's entire data1 directory, the same subtree
  // scripts/get_demo.sh unpacks.  Keys are launcher install paths; archive
  // members reach them through mapImportedPath, so any container layout that a
  // hand import would accept works here too.  Nothing outside this table is
  // installed, and nothing whose digest disagrees is installed at all.
  files: Object.freeze({
    // pak0.pak is the demo.  Its MD5 is the engine's own genuine-demo
    // fingerprint (demo_pakdata in engine/h2shared/quakefs.c).
    'data1/pak0.pak': Object.freeze({ size: 27750257, sha256: '0d4aa01a9909771dfa8e5be27db5d6628dc92f1406998c1a89c27d4748aaf151', required: true }),
    // Required despite not being a pak: the demo pak0 contains no progs.dat, so
    // the gamecode only exists as this loose file.
    'data1/progs.dat': Object.freeze({ size: 886592, sha256: '6981e0076329c95fbf41ff2c62767d4ea02e277eee888323bbe0a1ece4a8f62a', required: true }),
    'data1/hexen.rc': Object.freeze({ size: 340, sha256: 'fd30dba85635d879f5d72043e8a914c5465ea77068950c38513d95cde21ce339' }),
    'data1/default.cfg': Object.freeze({ size: 1875, sha256: '8199a001d204cf6b0ac20627143b0febc317054ea28ccda548d45418ee250536' }),
    'data1/autoexec.cfg': Object.freeze({ size: 1, sha256: '01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b' }),
    'data1/maps/demo2.txt': Object.freeze({ size: 572, sha256: '5a07f04b94bce17625e200774d2e414553624bcd38c0971c8085ba957ae8af7a' }),
    'data1/maps/demo2.ent': Object.freeze({ size: 55132, sha256: '7823ecb49d00fde6101412975f8df2a91f4c37232696c3c0c8a3d78340aad35b' }),
  }),
});

const TAR_BLOCK_SIZE = 512;
const GZIP_MAGIC = [0x1f, 0x8b];
const ZIP_MAGIC = [0x50, 0x4b];

// gzip and deflate shipped with the first DecompressionStream implementations
// (Chrome 80, Safari 16.4, Firefox 113), so any browser that can run this
// launcher's ZIP import -- which needs the later 'deflate-raw' -- can also run
// this.  Constructing one is the honest check: the constructor throws on a
// format the engine does not implement, which presence of the global does not
// tell us.
export function isDemoDecompressionSupported() {
  try {
    new DecompressionStream('gzip');
    return true;
  } catch {
    return false;
  }
}

function startsWith(bytes, magic) {
  return magic.every((value, index) => bytes[index] === value);
}

function concatChunks(chunks, totalBytes) {
  const merged = new Uint8Array(totalBytes);
  let offset = 0;
  for (const chunk of chunks) {
    merged.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return merged;
}

async function drainStream(stream, maxBytes, onBytes) {
  const reader = stream.getReader();
  const chunks = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) {
        break;
      }
      total += value.byteLength;
      if (total > maxBytes) {
        throw new Error(`Archive is larger than the ${maxBytes} byte limit`);
      }
      chunks.push(value);
      onBytes?.(total);
    }
  } finally {
    reader.cancel?.().catch(() => {});
  }
  return concatChunks(chunks, total);
}

export async function gunzip(bytes, maxBytes = HEXEN2_DEMO_SOURCE.maxArchiveBytes) {
  if (!isDemoDecompressionSupported()) {
    throw new Error('This browser cannot decompress gzip archives (DecompressionStream is unavailable).');
  }
  const stream = new Response(bytes).body.pipeThrough(new DecompressionStream('gzip'));
  return drainStream(stream, maxBytes);
}

export async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(digest)].map((value) => value.toString(16).padStart(2, '0')).join('');
}

function readTarNumber(bytes, offset, length) {
  const field = bytes.subarray(offset, offset + length);
  // GNU's base-256 encoding for values that do not fit the octal field.  Only
  // the size field can realistically need it, but reading it costs nothing and
  // an unhandled one would silently truncate a member.
  if (field[0] & 0x80) {
    let value = 0;
    for (let index = 1; index < field.length; index += 1) {
      value = (value * 256) + field[index];
    }
    return value;
  }
  let text = '';
  for (const value of field) {
    if (value === 0 || value === 0x20) {
      break;
    }
    text += String.fromCharCode(value);
  }
  return text.length ? Number.parseInt(text, 8) : 0;
}

function readTarString(bytes, offset, length) {
  const field = bytes.subarray(offset, offset + length);
  const end = field.indexOf(0);
  return new TextDecoder('utf-8').decode(end === -1 ? field : field.subarray(0, end));
}

function isTarHeaderValid(block) {
  const stored = readTarNumber(block, 148, 8);
  let unsigned = 0;
  let signed = 0;
  for (let index = 0; index < TAR_BLOCK_SIZE; index += 1) {
    // The checksum field itself is summed as if it were spaces.
    const value = (index >= 148 && index < 156) ? 0x20 : block[index];
    unsigned += value;
    signed += (value > 127) ? value - 256 : value;
  }
  return stored === unsigned || stored === signed;
}

/*
 * Enough POSIX/GNU tar to read the demo tarball: 512-byte headers, regular
 * files and directories, the ustar name prefix, and GNU long names.  Members
 * rejected by `filter` are skipped without copying their data, which is what
 * keeps a 30 MB archive from being held twice.
 */
export function parseTarEntries(bytes, options = {}) {
  const filter = options.filter ?? (() => true);
  const entries = [];
  let offset = 0;
  let pendingLongName = null;

  while (offset + TAR_BLOCK_SIZE <= bytes.byteLength) {
    const block = bytes.subarray(offset, offset + TAR_BLOCK_SIZE);
    if (block.every((value) => value === 0)) {
      break;
    }
    if (!isTarHeaderValid(block)) {
      throw new Error(`Tar header checksum mismatch at offset ${offset}`);
    }

    const prefix = readTarString(block, 345, 155);
    const rawName = readTarString(block, 0, 100);
    const name = pendingLongName ?? (prefix ? `${prefix}/${rawName}` : rawName);
    const size = readTarNumber(block, 124, 12);
    const typeFlag = block[156] === 0 ? '0' : String.fromCharCode(block[156]);
    const dataStart = offset + TAR_BLOCK_SIZE;
    const dataEnd = dataStart + size;
    if (dataEnd > bytes.byteLength) {
      throw new Error(`Tar member ${name} is truncated`);
    }
    pendingLongName = null;

    if (typeFlag === 'L') {
      // GNU long name: this member's data is the next member's path.
      pendingLongName = readTarString(bytes, dataStart, size);
    } else if (typeFlag === '0' || typeFlag === '7') {
      const wanted = filter(name);
      entries.push({
        name,
        size,
        typeFlag,
        data: wanted ? bytes.slice(dataStart, dataEnd) : null,
      });
    }

    offset = dataStart + (Math.ceil(size / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE);
  }

  return entries;
}

/*
 * Turns archive members into the (path, bytes) pairs the importer takes, and is
 * the only gate on what gets installed: a member has to map onto a manifest
 * entry and match its recorded size and digest.  Anything else in the archive
 * -- the 1997 engine binaries, the docs tree -- is ignored, and a manifest file
 * whose digest disagrees fails the whole install rather than landing a file the
 * launcher cannot vouch for.
 */
export async function verifyDemoPayload(members, config = HEXEN2_DEMO_SOURCE) {
  const manifest = config.files;
  const found = new Map();
  const ignored = [];

  for (const member of members) {
    const path = mapImportedPath(member.name);
    const expected = path ? manifest[path] : null;
    if (!expected) {
      ignored.push(member.name);
      continue;
    }
    if (found.has(path)) {
      throw new Error(`Archive contains ${path} more than once`);
    }
    if (member.data.byteLength !== expected.size) {
      throw new Error(`${path} is ${member.data.byteLength} bytes, expected ${expected.size}`);
    }
    const digest = await sha256Hex(member.data);
    if (digest !== expected.sha256) {
      throw new Error(`${path} failed verification (sha256 ${digest})`);
    }
    found.set(path, member.data);
  }

  for (const [path, entry] of Object.entries(manifest)) {
    if (entry.required && !found.has(path)) {
      throw new Error(`Archive is missing ${path}`);
    }
  }

  // Manifest order, so pak0.pak is written first and a storage failure part way
  // through cannot leave the launcher thinking it has a playable install.
  const files = Object.keys(manifest)
    .filter((path) => found.has(path))
    .map((path) => ({ path, bytes: found.get(path) }));
  return { files, ignored };
}

async function fetchArchive(url, options) {
  const { fetchImpl, onProgress, expectedBytes, maxBytes, signal } = options;
  // no-store keeps a one-shot 13 MB download out of the HTTP cache; the bytes
  // are headed for OPFS, and the service worker never sees this request (it is
  // cross-origin, or same-origin but outside the precache list).
  const response = await fetchImpl(url, { cache: 'no-store', signal });
  if (!response.ok) {
    throw new Error(`${url} responded ${response.status}`);
  }

  const declared = Number(response.headers?.get?.('content-length'));
  const totalBytes = Number.isFinite(declared) && declared > 0 ? declared : expectedBytes ?? 0;
  if (totalBytes && totalBytes > maxBytes) {
    throw new Error(`Download is ${totalBytes} bytes, over the ${maxBytes} byte limit`);
  }

  if (!response.body) {
    const bytes = new Uint8Array(await response.arrayBuffer());
    onProgress?.({ receivedBytes: bytes.byteLength, totalBytes: totalBytes || bytes.byteLength });
    return bytes;
  }
  return drainStream(response.body, maxBytes, (receivedBytes) => {
    onProgress?.({ receivedBytes, totalBytes });
  });
}

async function probeLocalSource(url, fetchImpl, signal) {
  try {
    const response = await fetchImpl(url, { method: 'HEAD', cache: 'no-store', signal });
    return response.ok;
  } catch {
    return false;
  }
}

/*
 * Downloads and verifies the demo, returning importable (path, bytes) pairs.
 * Storage is the caller's job: this resolves only once every byte it hands back
 * has matched the pinned manifest.
 */
export async function downloadHexen2Demo(options = {}) {
  const config = options.config ?? HEXEN2_DEMO_SOURCE;
  const fetchImpl = options.fetchImpl ?? globalThis.fetch.bind(globalThis);
  const onStage = options.onStage ?? (() => {});
  const { signal } = options;
  const warnings = [];

  if (!isDemoDecompressionSupported()) {
    throw new Error('This browser cannot decompress the demo archive (DecompressionStream is unavailable).');
  }

  const hasLocalCopy = await probeLocalSource(config.local.url, fetchImpl, signal);
  const source = hasLocalCopy ? config.local : config.remote;

  onStage({ stage: 'downloading', source });
  const archive = await fetchArchive(source.url, {
    fetchImpl,
    signal,
    expectedBytes: source.downloadBytes,
    maxBytes: config.maxDownloadBytes,
    onProgress: (progress) => onStage({ stage: 'downloading', source, ...progress }),
  });

  onStage({ stage: 'verifying', source });
  if (source.archiveSha256) {
    const digest = await sha256Hex(archive);
    if (digest !== source.archiveSha256) {
      warnings.push(`Archive sha256 ${digest} is not the pinned ${source.archiveSha256}; falling back to per-file verification.`);
    }
  }

  let members;
  if (startsWith(archive, GZIP_MAGIC)) {
    onStage({ stage: 'extracting', source });
    const tar = await gunzip(archive, config.maxArchiveBytes);
    members = parseTarEntries(tar, { filter: (name) => Boolean(mapImportedPath(name)) })
      .filter((entry) => entry.data);
  } else if (startsWith(archive, ZIP_MAGIC)) {
    onStage({ stage: 'extracting', source });
    members = (await extractZipEntries(archive)).map((entry) => ({ name: entry.name, data: entry.data }));
  } else {
    throw new Error(`${source.url} is neither a gzip nor a ZIP archive`);
  }

  onStage({ stage: 'verifying', source });
  const { files, ignored } = await verifyDemoPayload(members, config);
  return { source, files, ignored, warnings };
}
