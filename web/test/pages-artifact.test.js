import test from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, unlinkSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const repoRoot = process.cwd();

// Runs the real assemble + validate scripts that pages.yml uses, against stub
// Emscripten outputs, so the artifact's shape is tested without an emsdk.
function assemble() {
  const work = mkdtempSync(join(tmpdir(), 'hexenwail-pages-'));
  const bin = join(work, 'bin');
  const dist = join(work, 'dist');
  mkdirSync(bin);
  for (const name of ['hexenwail.js', 'hexenwail.wasm', 'hexenwail.html']) {
    writeFileSync(join(bin, name), `stub ${name}\n`);
  }
  execFileSync('bash', [join(repoRoot, 'scripts/wasm-assemble-artifact.sh'), dist, 'test-build', bin], { stdio: 'pipe' });
  return { work, dist };
}

function validate(dist) {
  try {
    execFileSync('bash', [join(repoRoot, 'scripts/wasm-validate-artifact.sh'), dist], { stdio: 'pipe' });
    return { ok: true, stderr: '' };
  } catch (error) {
    return { ok: false, stderr: String(error.stderr) };
  }
}

test('assembled Pages artifact excludes web/test and passes validation', () => {
  const { work, dist } = assemble();
  try {
    assert.equal(existsSync(join(dist, 'test')), false, 'web/test/ was copied into the artifact');
    assert.equal(existsSync(join(dist, 'lib', 'zip.js')), true);
    const result = validate(dist);
    assert.equal(result.ok, true, result.stderr);
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
});

test('artifact validation rejects a published test directory', () => {
  const { work, dist } = assemble();
  try {
    mkdirSync(join(dist, 'test'));
    writeFileSync(join(dist, 'test', 'leak.test.js'), '');
    const result = validate(dist);
    assert.equal(result.ok, false);
    assert.match(result.stderr, /must not be published/);
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
});

test('artifact validation rejects a missing precached module', () => {
  const { work, dist } = assemble();
  try {
    unlinkSync(join(dist, 'lib', 'webgl-diagnostics.js'));
    const result = validate(dist);
    assert.equal(result.ok, false);
    assert.match(result.stderr, /lib\/webgl-diagnostics\.js/);
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
});

test('ci.yml and pages.yml pin the same Emscripten version', () => {
  const pinned = (file) => readFileSync(join(repoRoot, '.github/workflows', file), 'utf8')
    .match(/^\s*EMSDK_VERSION:\s*'([^']+)'/m)?.[1];
  const ci = pinned('ci.yml');
  assert.ok(ci, 'ci.yml no longer pins EMSDK_VERSION');
  assert.equal(pinned('pages.yml'), ci, 'the PR check would build with a different toolchain than the deploy');
});
