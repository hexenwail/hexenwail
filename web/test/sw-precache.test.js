import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname, relative, posix } from 'node:path';

const repoRoot = process.cwd();
const webRoot = join(repoRoot, 'web');

// The array literal is parsed rather than imported: sw.js is a worker script
// that touches `self` at load time.
export function parsePrecacheList(swSource) {
  const block = swSource.match(/const CORE_ASSETS = \[([\s\S]*?)\];/);
  assert.ok(block, 'sw.js no longer declares `const CORE_ASSETS = [...]`');
  return [...block[1].matchAll(/'\.\/([^']*)'/g)].map((m) => (m[1] === '' ? 'index.html' : m[1]));
}

export function importSpecifiers(source) {
  const specifiers = [];
  const patterns = [
    // import x from '...', import { a,\n b } from '...', export { a } from '...', export * from '...'
    /\b(?:import|export)\s[^'"`;]*?\bfrom\s*['"]([^'"]+)['"]/g,
    // import '...' (side-effect only)
    /\bimport\s*['"]([^'"]+)['"]/g,
    // import('...') with a literal specifier
    /\bimport\(\s*['"]([^'"]+)['"]\s*\)/g,
  ];
  for (const pattern of patterns) {
    for (const match of source.matchAll(pattern)) {
      specifiers.push(match[1]);
    }
  }
  return specifiers;
}

function isRelativeSpecifier(specifier) {
  return specifier.startsWith('./') || specifier.startsWith('../') || specifier.startsWith('/');
}

function htmlEntryPoints(html) {
  const moduleScripts = [];
  const assets = [];
  for (const match of html.matchAll(/<script\b([^>]*)>/g)) {
    const src = match[1].match(/\bsrc="([^"]+)"/);
    if (src) {
      (/\btype="module"/.test(match[1]) ? moduleScripts : assets).push(src[1]);
    }
  }
  for (const match of html.matchAll(/<link\b([^>]*)>/g)) {
    const rel = match[1].match(/\brel="([^"]+)"/)?.[1] ?? '';
    const href = match[1].match(/\bhref="([^"]+)"/)?.[1];
    if (href && /\b(manifest|icon|apple-touch-icon|stylesheet|modulepreload)\b/.test(rel)) {
      assets.push(href);
    }
  }
  return { moduleScripts, assets };
}

function toWebPath(fromFile, specifier) {
  const absolute = specifier.startsWith('/')
    ? join(webRoot, specifier)
    : join(dirname(fromFile), specifier);
  return relative(webRoot, absolute).split('\\').join(posix.sep);
}

// Everything the shell fetches from its own origin to boot: index.html, what
// it links, and the transitive static-import graph of its module scripts.
export function shellGraph() {
  const indexPath = join(webRoot, 'index.html');
  const { moduleScripts, assets } = htmlEntryPoints(readFileSync(indexPath, 'utf8'));
  const reached = new Set(['index.html']);
  for (const asset of assets) {
    reached.add(toWebPath(indexPath, asset));
  }

  const manifest = JSON.parse(readFileSync(join(webRoot, 'manifest.webmanifest'), 'utf8'));
  for (const icon of manifest.icons ?? []) {
    reached.add(toWebPath(join(webRoot, 'manifest.webmanifest'), icon.src));
  }

  const queue = moduleScripts.map((src) => toWebPath(indexPath, src));
  while (queue.length > 0) {
    const file = queue.shift();
    if (reached.has(file)) {
      continue;
    }
    reached.add(file);
    if (!file.endsWith('.js')) {
      continue;
    }
    const absolute = join(webRoot, file);
    assert.ok(existsSync(absolute), `import graph reaches missing file web/${file}`);
    for (const specifier of importSpecifiers(readFileSync(absolute, 'utf8'))) {
      // Bare and node: specifiers are Node-only fallbacks (zip.js's node:zlib)
      // that browsers never fetch.
      if (isRelativeSpecifier(specifier)) {
        queue.push(toWebPath(absolute, specifier));
      }
    }
  }
  return reached;
}

test('importSpecifiers finds every static and literal dynamic import form', () => {
  const source = [
    "import { a } from './a.js';",
    'import {',
    '  b, c,',
    "} from './b.js';",
    "import * as d from \"./d.js\";",
    "import './side-effect.js';",
    "export { e } from './e.js';",
    "export * from '../f.js';",
    "const { g } = await import('./g.js');",
    "const { inflateRawSync } = await import('node:zlib');",
    "const text = 'from here';",
  ].join('\n');
  assert.deepEqual(importSpecifiers(source).sort(), [
    '../f.js', './a.js', './b.js', './d.js', './e.js', './g.js', './side-effect.js', 'node:zlib',
  ]);
});

test('shell import graph reaches app.js and its lib modules', () => {
  const reached = shellGraph();
  // Guards against the walker silently finding nothing, which would make the
  // precache assertion below pass vacuously.
  for (const expected of ['app.js', 'lib/paths.js', 'lib/zip.js', 'manifest.webmanifest']) {
    assert.ok(reached.has(expected), `walker did not reach ${expected}`);
  }
});

test('service worker precaches every file the offline shell reaches', () => {
  const precached = new Set(parsePrecacheList(readFileSync(join(webRoot, 'sw.js'), 'utf8')));
  const missing = [...shellGraph()].filter((file) => !precached.has(file)).sort();
  assert.deepEqual(missing, [], `add these to CORE_ASSETS in web/sw.js: ${missing.join(', ')}`);
});

test('service worker precache list holds no test files', () => {
  const precached = parsePrecacheList(readFileSync(join(webRoot, 'sw.js'), 'utf8'));
  assert.deepEqual(precached.filter((file) => file.startsWith('test/')), []);
});
