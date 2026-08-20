import { isGamedirName, sanitizeGamedirName, sanitizeRelativePath } from './paths.js';

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
  'pak',
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
  return [dir, ...segments.slice(1)].join('/');
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
 * -game is what FS_Init looks for first (-mod is its alias, checked only when
 * -game is absent), and passing it also folds portals/ into the search path
 * below the mod, which is what mods that share mission pack assets expect.
 *
 * Passing it at all requires a registered install -- the engine fatals with
 * "You must have the full version of Hexen II to play modified games" on a
 * demo one -- so callers gate on the base assets before selecting a mod.
 */
export function buildEngineArguments(baseArguments, mod) {
  const gamedir = mod ? sanitizeGamedirName(mod) : null;
  return gamedir ? [...baseArguments, '-game', gamedir] : [...baseArguments];
}
