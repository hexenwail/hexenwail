import { KNOWN_GAME_ROOTS, canonicalPakName, isGamedirName, normalizeGamedirName, sanitizeGamedirName, sanitizeRelativePath } from './paths.js';

/*
 * The browser equivalent of `hexen2 -game mymod`.
 *
 * A mod import is rooted at its own gamedir, so it deliberately does NOT go
 * through mapImportedPath: that mapper exists to flatten a retail install into
 * data1/, and flattening is exactly the wrong thing to do to mod content whose
 * whole meaning is "these files sit above data1 in the search path".
 */

/*
 * Accepted by extension rather than by name: a mod's payload is whatever its
 * progs.dat asks for, so there is no filename list to match against.  The
 * entries are the extensions the engine itself opens (grep the sources for
 * .lmp/.mdl/.wav/.spr/.bsp/... and snd_codec's format table), plus the
 * replacement-texture formats the GL renderer loads.  Everything else --
 * archives, executables, scripts, .gip/.hsv savegames -- is ignored, so an
 * imported folder can only ever contain data the engine would read.
 */
export const MOD_FILE_EXTENSIONS = Object.freeze([
  'pak', 'pk3',
  'dat', 'cfg', 'rc', 'lst', 'txt',
  'bsp', 'lit', 'ent',
  'mdl', 'spr',
  'lmp', 'wad', 'pcx', 'tga', 'png', 'jpg', 'jpeg', 'dds', 'ktx',
  'wav', 'ogg', 'mp3', 'flac', 'opus', 'mid', 'it', 's3m', 'umx',
  'dem',
]);

const MOD_FILE_EXTENSION_SET = new Set(MOD_FILE_EXTENSIONS);

export function isModAssetPath(inputPath) {
  const safe = sanitizeRelativePath(inputPath);
  if (!safe) {
    return false;
  }
  const basename = safe.split('/').at(-1);
  const dot = basename.lastIndexOf('.');
  return dot > 0 && MOD_FILE_EXTENSION_SET.has(basename.slice(dot + 1).toLowerCase());
}

// The picked folder's own name is the gamedir, so every entry of a directory
// import shares a first segment; anything that does not is not part of this
// mod and is dropped rather than quietly re-rooted.
export function modImportRoot(sourcePaths) {
  let root = null;
  for (const value of sourcePaths) {
    const safe = sanitizeRelativePath(value);
    if (!safe || !safe.includes('/')) {
      return null;
    }
    const segment = safe.split('/')[0];
    if (root === null) {
      root = segment;
    } else if (root !== segment) {
      return null;
    }
  }
  return root;
}

export function mapModImportPath(gamedir, sourcePath) {
  const dir = sanitizeGamedirName(gamedir);
  const safe = sanitizeRelativePath(sourcePath);
  if (!dir || !safe) {
    return null;
  }
  const segments = safe.split('/');
  // Interior segments keep their case: a mod's own files reference each other
  // by the paths its author shipped, and folding them would break the lookups
  // it does over a case-sensitive filesystem.
  if (segments.length < 2 || sanitizeGamedirName(segments[0]) !== dir || !isModAssetPath(safe)) {
    return null;
  }
  const rest = segments.slice(1);
  // A pak or pk3 at the top of the mod is one FS_AddGameDirectory mounts, by
  // its exact lowercase name or extension (see canonicalPakName).
  if (rest.length === 1) {
    rest[0] = canonicalPakName(rest[0]);
  }
  return [dir, ...rest].join('/');
}

/*
 * Where a mod .zip's mod lives inside it, and what to call it (GitHub #143).
 *
 * The mod is found by its anchors, not by the archive's folder shape: a
 * progs.dat, a pak or pk3, or a folder the engine reads content from (maps/,
 * sound/, models/, ...).  The mod root is the shallowest directory an anchor
 * sits in, compared without case, and the gamedir is that directory's name --
 * or the archive's, when the anchors are at the top:
 *
 *   mymod/progs.dat, mymod/maps/a.bsp          -> mymod
 *   README.txt, mymod/progs.dat, mymod/pak0.pak -> mymod (README left out)
 *   Download/MyMod/progs.dat                   -> mymod (wrappers fall away)
 *   progs.dat, maps/a.bsp                      -> named after the archive
 *   maps/a.bsp                                 -> named after the archive
 *
 * A shape rule ("one top folder is the mod, unless...") got these wrong: a
 * README beside the mod folder made the archive the mod and stored
 * mymod-v1/mymod/pak0.pak, which the engine never mounts.
 *
 * Refused: a root that is, or sits inside, data1/portals/hw (a base-game
 * overlay, which "Choose files" imports); anchors in two different roots (two
 * mods, or a mod plus an overlay); and an archive with no anchor at all, whose
 * layout says nothing about where the mod starts.
 *
 * Archive junk -- __MACOSX/ resource forks, .DS_Store, Thumbs.db,
 * desktop.ini -- is dropped before anything is read.
 *
 * names are the zip's file entry names (directories already dropped).
 * Returns { gamedir, root, paths, outside } -- root the anchor directory's
 * segments, paths mapping each entry under it to its gamedir-rooted path,
 * outside the entries left out -- or { error, ... }.
 */
const MOD_CONTENT_DIRS = new Set([
  'maps', 'models', 'progs', 'sound', 'sounds', 'music', 'midi', 'gfx', 'sprites',
  'textures', 'skins', 'particles', 'demos', 'locs', 'env', 'puzzles',
]);

const ARCHIVE_JUNK_RE = /(^|\/)(__MACOSX\/|\.DS_Store$|Thumbs\.db$|desktop\.ini$)/i;

function anchorRoot(segments) {
  for (let i = 0; i < segments.length - 1; i += 1) {
    if (MOD_CONTENT_DIRS.has(segments[i].toLowerCase())) {
      return segments.slice(0, i);
    }
  }
  const base = segments.at(-1).toLowerCase();
  if (/^progs2?\.dat$/.test(base) || base.endsWith('.pak') || base.endsWith('.pk3')) {
    return segments.slice(0, -1);
  }
  return null;
}

export function planModZipImport(names, archiveName) {
  const kept = [];
  for (const value of names) {
    const safe = sanitizeRelativePath(value);
    if (safe && !ARCHIVE_JUNK_RE.test(safe)) {
      kept.push({ name: value, segments: safe.split('/') });
    }
  }
  if (!kept.length) {
    return { error: 'empty' };
  }

  // Every anchor's root, keyed without case: MyMod/ and mymod/ are one folder
  // to the engine's lookups, so they are one root here.
  const roots = new Map();
  for (const { segments } of kept) {
    const root = anchorRoot(segments);
    if (root) {
      const key = root.join('/').toLowerCase();
      if (!roots.has(key)) {
        roots.set(key, root);
      }
    }
  }
  if (!roots.size) {
    return { error: 'no-anchor' };
  }
  const depth = Math.min(...[...roots.values()].map((root) => root.length));
  const shallowest = [...roots.values()].filter((root) => root.length === depth);

  for (const root of roots.values()) {
    const base = root.find((segment) => KNOWN_GAME_ROOTS.includes(segment.toLowerCase()));
    if (base) {
      return { error: 'base-game', base: base.toLowerCase() };
    }
  }
  if (shallowest.length > 1) {
    return { error: 'several', names: shallowest.map((root) => root.at(-1) || archiveName) };
  }

  const root = shallowest[0];
  const rawName = root.length ? root.at(-1) : String(archiveName ?? '').replace(/\.zip$/i, '');
  const gamedir = sanitizeGamedirName(rawName);
  if (!gamedir) {
    return normalizeGamedirName(rawName)
      ? { error: 'reserved', name: rawName }
      : { error: 'unnamed', name: rawName || archiveName };
  }

  const prefix = root.map((segment) => segment.toLowerCase());
  const paths = new Map();
  const outside = [];
  for (const { name, segments } of kept) {
    const inside = segments.length > prefix.length
      && prefix.every((segment, i) => segments[i].toLowerCase() === segment);
    if (inside) {
      paths.set(name, [gamedir, ...segments.slice(prefix.length)].join('/'));
    } else {
      outside.push(name);
    }
  }
  return { gamedir, root, paths, outside };
}

// Detection is by storage layout, not by a registry: a gamedir is any stored
// top-level directory that holds a file and is not part of the base game.
export function detectModGamedirs(paths) {
  const found = new Set();
  for (const value of paths) {
    const safe = sanitizeRelativePath(value);
    if (!safe) {
      continue;
    }
    const segments = safe.split('/');
    if (segments.length >= 2 && isGamedirName(segments[0])) {
      found.add(segments[0]);
    }
  }
  return [...found].sort();
}

export function modOwnedPath(gamedir, path) {
  const dir = sanitizeGamedirName(gamedir);
  const safe = sanitizeRelativePath(path);
  return Boolean(dir && safe && safe.split('/')[0].toLowerCase() === dir && safe.includes('/'));
}

/*
 * The mission pack is a launch choice beside the mods, not a mod: "portals" is
 * reserved (sanitizeGamedirName refuses it, and FS_Gamedir ignores -game
 * portals), and the engine starts Portal of Praevus from its own flag.  This
 * sentinel is the value the launcher stores and the dropdown carries for it.
 * GitHub #100.
 */
export const MISSION_PACK = 'portals';

/*
 * The mission pack is offered when its pak is stored where FS_Init opens it:
 * FS_AddGameDirectory reads portals/pak3.pak by that exact, lowercase name, and
 * mounts any portals/*.pk3 -- which is how tools/pak2pk3.py ships a converted
 * one (000-pak3.pk3).  Content identification (GAME_PORTALS) then happens in
 * the engine, which backs out with "Missing or invalid mission pack
 * installation" when the file is not the mission pack.
 */
export function hasMissionPack(paths) {
  for (const value of paths) {
    const safe = sanitizeRelativePath(value);
    if (safe === 'portals/pak3.pak' || /^portals\/[^/]+\.pk3$/.test(safe ?? '')) {
      return true;
    }
  }
  return false;
}

// A stored or picked launch choice: the mission pack, a gamedir, or '' for
// the base game.  Anything else -- a reserved name, junk from an old
// preferences blob -- reads as the base game.
export function parseLaunchChoice(value) {
  if (typeof value !== 'string') {
    return '';
  }
  if (value === MISSION_PACK) {
    return MISSION_PACK;
  }
  return sanitizeGamedirName(value) ?? '';
}

/*
 * -game is what FS_Init looks for first (-mod is its alias, checked only when
 * -game is absent), and passing it also folds portals/ into the search path
 * below the mod, which is what mods that share mission pack assets expect.
 *
 * The mission pack itself is -portals (-missionpack and -h2mp are aliases).
 *
 * Passing either at all requires a registered install -- the engine fatals
 * with "You must have the full version of Hexen II to play modified games", or
 * "Portal of Praevus requires registered version of Hexen II", on a demo one --
 * so callers gate on the base assets before selecting one.
 */
export function buildEngineArguments(baseArguments, choice) {
  if (choice === MISSION_PACK) {
    return [...baseArguments, '-portals'];
  }
  const gamedir = choice ? sanitizeGamedirName(choice) : null;
  return gamedir ? [...baseArguments, '-game', gamedir] : [...baseArguments];
}

/*
 * The mod panel's explanatory line, from the state it describes.  Pure so the
 * copy can be tested: the first cut said "The selected mod is passed to the
 * engine as -game" while the mission pack, which is not, was selected.
 *
 *   mods        imported gamedirs, sorted
 *   missionPack whether hasMissionPack() holds
 *   selected    the dropdown's choice: '', a gamedir, or MISSION_PACK
 *   launchable  whether the install can launch a choice at all (full game)
 */
export function modPanelNote({ mods, missionPack, selected, launchable }) {
  if (selected && !launchable) {
    return selected === MISSION_PACK
      ? 'Portal of Praevus is selected but will not load: the mission pack needs the full game\'s data1/pak1.pak, so this install starts the base game.'
      : `“${selected}” is selected but will not load: the engine refuses -game without the full game's data1/pak1.pak, so this install starts unmodded.`;
  }
  const parts = [mods.length
    ? `Imported mods: ${mods.join(', ')}.`
    : 'No mods imported yet. “Import mod folder” stores a folder under its own name and launches the engine with -game <folder>; “Import mod .zip” does the same for a mod as it was downloaded.'];
  if (selected === MISSION_PACK) {
    parts.push('Portal of Praevus is selected: the engine starts the mission pack (-portals).');
  } else if (selected) {
    parts.push(`“${selected}” is selected and is passed to the engine as -game.`);
  } else {
    if (mods.length) {
      parts.push('Pick one above to pass it to the engine as -game.');
    }
    if (missionPack) {
      parts.push('Portal of Praevus is installed: pick it above to launch the mission pack (-portals).');
    }
  }
  return parts.join(' ');
}
