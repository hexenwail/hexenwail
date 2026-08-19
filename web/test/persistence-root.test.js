import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { isSavePath } from '../lib/save-bundle.js';

// The browser build has exactly one durable directory: the IDBFS mount the
// launcher passes as -basedir.  Everything the engine writes -- config.cfg,
// savegames -- has to land inside it, and that only holds while the engine's
// userdir follows -basedir instead of $HOME.  These are source assertions
// because the failure they guard against is a C-side one: re-enabling
// DO_USERDIRS for Emscripten silently moves every save to a MEMFS path that
// dies with the tab, and no amount of JS-level testing can see that.
const repoRoot = process.cwd();
const appText = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
const sysHeader = readFileSync(join(repoRoot, 'engine/h2shared/sys.h'), 'utf8');
const quakefsText = readFileSync(join(repoRoot, 'engine/h2shared/quakefs.c'), 'utf8');

test('the launcher syncs the same root it hands the engine as -basedir', () => {
  assert.match(appText, /const BASE_DIR = '\/persistent';/);
  assert.match(appText, /const ENGINE_ARGUMENTS = \['-basedir', BASE_DIR\];/);
  // Persistence walks BASE_DIR, so anything written outside it is never stored.
  assert.match(appText, /function walkRuntimeFiles\(path = BASE_DIR/);
  assert.match(appText, /Module\.callMain\(\[\.\.\.ENGINE_ARGUMENTS\]\)/);
});

test('the Emscripten build keeps one persistence root by disabling user directories', () => {
  const guard = sysHeader.match(/#if defined\(PLATFORM_WINDOWS\)[^\n]*\n#undef\s+DO_USERDIRS\n#define\s+DO_USERDIRS\s+0/);
  assert.ok(guard, 'sys.h no longer has the DO_USERDIRS platform-exclusion guard');
  assert.match(guard[0], /defined\(__EMSCRIPTEN__\)/,
    'Emscripten must disable DO_USERDIRS, or saves go to $HOME on a MEMFS mount and are lost on reload');
});

test('the engine points its userdir at -basedir when user directories are off', () => {
  assert.match(quakefsText, /#if !DO_USERDIRS\n\s*host_parms->userdir = com_argv\[i\+1\];\n#endif/);
});

test('the save layout the engine produces is the one the bundle allowlist accepts', () => {
  // FS_USERDIR resolves to <basedir>/<gamedir>, so a "save s0" writes
  // /persistent/data1/s0/... -- BASE_DIR-relative data1/s0/...
  assert.equal(isSavePath('data1/s0/info.dat'), true);
  assert.equal(isSavePath('data1/s0/demo1.gip'), true);
  // A separate userdir would have prefixed these and silently exported nothing.
  assert.equal(isSavePath('userdata/data1/s0/info.dat'), false);
  assert.equal(isSavePath('.hexen2/data1/s0/info.dat'), false);
});
