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

// FS_AddGameDirectory opens a gamedir's paks as "pak%i.pak", lowercase, with
// no case fallback, so PAK0.PAK from a CD copy has to be stored as pak0.pak or
// the engine never sees it.  Its pk3 scan matches "*.pk3" case-sensitively
// (Sys_FindFirstFile's fnmatch), so a .PK3 extension is folded too; the rest
// of a pk3's name keeps its case, since the engine mounts pk3s in name order.
// Any other name keeps its case: loose files are found case-insensitively.
export function canonicalPakName(name) {
  if (/^pak\d+\.pak$/i.test(name)) {
    return name.toLowerCase();
  }
  if (/\.pk3$/i.test(name)) {
    return name.slice(0, -4) + '.pk3';
  }
  return name;
}

/*
 * The name a stored file should have so the engine opens it: a game root
 * (data1, portals, hw) by its canonical lowercase name -- FS_AddGameDirectory
 * adds the directory by exactly that name -- and a pak or pk3 directly inside
 * any gamedir by canonicalPakName.  Interior paths keep their case.
 *
 * Imports before GitHub #100 stored the root and paks as the source folder
 * spelled them, so a CD-style DATA1/PAK0.PAK copy sat in storage under a name
 * the engine never opens.  planCanonicalRenames moves those.
 */
export function canonicalStoredPath(inputPath) {
  const safe = sanitizeRelativePath(inputPath);
  if (!safe) {
    return null;
  }
  const segments = safe.split('/');
  if (KNOWN_GAME_ROOTS.includes(segments[0].toLowerCase())) {
    segments[0] = segments[0].toLowerCase();
  }
  if (segments.length === 2) {
    segments[1] = canonicalPakName(segments[1]);
  }
  return segments.join('/');
}

/*
 * [from, to] moves that bring stored paths to their canonical names.  A move
 * whose target is already stored, or is claimed by an earlier move, is skipped:
 * the canonical copy is the one the engine loads, and a launcher must not
 * pick between two different files on its own, or delete one.
 */
export function planCanonicalRenames(paths) {
  const stored = new Set(paths);
  const claimed = new Set();
  const moves = [];
  for (const from of paths) {
    const to = canonicalStoredPath(from);
    if (!to || to === from || stored.has(to) || claimed.has(to)) {
      continue;
    }
    claimed.add(to);
    moves.push([from, to]);
  }
  return moves;
}

export function mapImportedPath(inputPath) {
  const safe = sanitizeRelativePath(inputPath);
  if (!safe) {
    return null;
  }

  const segments = safe.split('/');
  const lowered = segments.map((segment) => segment.toLowerCase());

  // A General MIDI SoundFont for the MIDI music (GitHub #114).  The web build
  // has no executable directory, so of SF_FindSoundFont's search
  // (engine/h2shared/soundfont.c) the reachable spot is
  // <basedir>/data1/soundfont.sf2 -- whatever the file was called and wherever
  // it sat in the picked folder.  The launcher writes stored files before the
  // engine starts, so the codec finds it at init.  SF3 is not taken: the web
  // synth is libTiMidity, which cannot decode it (the engine skips one).
  if (lowered.at(-1).endsWith('.sf2')) {
    return 'data1/soundfont.sf2';
  }

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
    // The root is stored by its canonical name ("PORTALS/" from a CD copy
    // becomes "portals/"), because the engine adds the gamedir by exactly that
    // name and the browser filesystem is case-sensitive.  Interior names keep
    // their case -- loose files are found through the engine's case-insensitive
    // fallback -- except a top-level pakN.pak, see canonicalPakName.
    const rest = segments.slice(knownRootIndex + 1);
    if (rest.length === 1) {
      rest[0] = canonicalPakName(rest[0]);
    }
    return [root, ...rest].join('/');
  }

  const basename = segments.at(-1);
  const lowerBase = basename.toLowerCase();
  if (/^pak[0-2]\.pak$/.test(lowerBase)) {
    return `data1/${lowerBase}`;
  }
  if (lowerBase === 'pak3.pak') {
    return `portals/${lowerBase}`;
  }
  if (lowerBase.endsWith('.ogg')) {
    return `data1/music/${basename}`;
  }
  if (lowerBase.endsWith('.pak')) {
    return `data1/${canonicalPakName(basename)}`;
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
//
// Exact names, not case-folded: the engine opens data1/pak0.pak by exactly that
// name, so a DATA1/PAK0.PAK it cannot open must not read as installed.  Stored
// legacy spellings are renamed by planCanonicalRenames before this is asked.
export function getBaseAssetStatus(paths) {
  const set = new Set(paths.map((value) => sanitizeRelativePath(value)).filter(Boolean));
  if (!set.has('data1/pak0.pak')) {
    return 'none';
  }
  return set.has('data1/pak1.pak') ? 'full' : 'pak0-only';
}
