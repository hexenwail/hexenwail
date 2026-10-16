import { canonicalPakName, isGamedirName, sanitizeGamedirName, sanitizeRelativePath } from './paths.js';

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
    : 'No mods imported yet. “Import mod folder” stores a folder under its own name and launches the engine with -game <folder>.'];
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
