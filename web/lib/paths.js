export const KNOWN_GAME_ROOTS = ['data1', 'portals', 'hw'];
export const MAX_GAMEDIR_LENGTH = 32;
const WINDOWS_DRIVE_RE = /^[a-zA-Z]:/;
const GAMEDIR_RE = /^[a-z0-9_-]+$/;

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

/*
 * A gamedir reaches the engine twice: as a bare argv token after -game, and as
 * a single path component (FS_Gamedir refuses any name holding '/', '\\', ':'
 * or '..', and fatals when it is handed one at startup).  A folder name picked
 * out of a file dialog satisfies neither by construction, so it is folded down
 * to the one shape both accept before it is ever stored or launched with.
 */
export function normalizeGamedirName(rawName) {
  const raw = normalizeSlashes(rawName).split('/').filter(Boolean).at(-1) ?? '';
  const folded = raw.toLowerCase().replace(/[^a-z0-9_-]+/g, '-').replace(/-{2,}/g, '-');
  const trimmed = folded.replace(/^-+/, '').slice(0, MAX_GAMEDIR_LENGTH).replace(/-+$/, '');
  return GAMEDIR_RE.test(trimmed) ? trimmed : null;
}

// The base game's own directories are not mods: FS_Gamedir silently ignores
// "data1" and "portals" and warns that "hw" belongs to HexenWorld, so letting
// one be imported or selected would offer a control that cannot do anything.
export function sanitizeGamedirName(rawName) {
  const name = normalizeGamedirName(rawName);
  return name && !KNOWN_GAME_ROOTS.includes(name) ? name : null;
}

export function isGamedirName(value) {
  return typeof value === 'string' && sanitizeGamedirName(value) === value;
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
  // The demo ships its gamecode loose next to pak0 instead of inside a pak, so
  // a file-by-file import of a demo install has to be able to take it.
  if (/^progs2?\.dat$/.test(lowerBase)) {
    return `data1/${basename}`;
  }

  return null;
}

export function isRecognizedImportPath(inputPath) {
  return mapImportedPath(inputPath) !== null;
}

// 'full' and 'pak0-only' both launch: pak0 alone is the entire Nov-1997 demo,
// and the engine does its own version detection (and fatals with a clear
// message when a registered pak0 has no pak1), so the launcher reports what it
// can see rather than guessing which edition the file is.
export function getBaseAssetStatus(paths) {
  const set = new Set(paths.map((value) => sanitizeRelativePath(value)?.toLowerCase()).filter(Boolean));
  if (!set.has('data1/pak0.pak')) {
    return 'none';
  }
  return set.has('data1/pak1.pak') ? 'full' : 'pak0-only';
}
