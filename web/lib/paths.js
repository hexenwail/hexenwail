export const KNOWN_GAME_ROOTS = ['data1', 'portals', 'hw'];
const WINDOWS_DRIVE_RE = /^[a-zA-Z]:/;

function normalizeSlashes(input) {
  return String(input ?? '').replace(/\\+/g, '/').replace(/\/+/g, '/');
}

export function sanitizeRelativePath(inputPath) {
  const raw = normalizeSlashes(inputPath).trim();
  if (!raw || raw === '.' || raw.includes('\0')) {
    return null;
  }
  if (raw.startsWith('/') || raw.startsWith('\\') || WINDOWS_DRIVE_RE.test(raw)) {
    return null;
  }

  const cleaned = [];
  for (const segment of raw.split('/')) {
    if (!segment || segment === '.') {
      continue;
    }
    if (segment === '..') {
      return null;
    }
    cleaned.push(segment);
  }

  return cleaned.length ? cleaned.join('/') : null;
}

export function mapImportedPath(inputPath) {
  const safe = sanitizeRelativePath(inputPath);
  if (!safe) {
    return null;
  }

  const segments = safe.split('/');
  const lowered = segments.map((segment) => segment.toLowerCase());
  const knownRootIndex = lowered.findIndex((segment) =>
    KNOWN_GAME_ROOTS.includes(segment) || segment === 'music'
  );

  if (knownRootIndex >= 0) {
    const root = lowered[knownRootIndex];
    if (root === 'music') {
      const suffix = segments.slice(knownRootIndex + 1);
      if (!suffix.length) {
        return null;
      }
      return ['data1', 'music', ...suffix].join('/');
    }
    return segments.slice(knownRootIndex).join('/');
  }

  const basename = segments.at(-1);
  const lowerBase = basename.toLowerCase();
  if (/^pak[0-2]\.pak$/.test(lowerBase)) {
    return `data1/${basename}`;
  }
  if (lowerBase === 'pak3.pak') {
    return `portals/${basename}`;
  }
  if (lowerBase.endsWith('.ogg')) {
    return `data1/music/${basename}`;
  }
  if (lowerBase.endsWith('.pak')) {
    return `data1/${basename}`;
  }

  return null;
}

export function isRecognizedImportPath(inputPath) {
  return mapImportedPath(inputPath) !== null;
}

export function hasRequiredBaseAssets(paths) {
  const set = new Set(paths.map((value) => sanitizeRelativePath(value)?.toLowerCase()).filter(Boolean));
  return set.has('data1/pak0.pak') && set.has('data1/pak1.pak');
}
