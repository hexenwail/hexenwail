import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import { nonBlackPixelRatio } from '../lib/webgl-diagnostics.js';
import { extractEngineWebGLPrograms } from '../../scripts/webgl-engine-shader-smoke.mjs';

test('black-frame detector distinguishes visible and dark frames', () => {
  // Guard cases: null, undefined, empty, and a buffer shorter than one pixel.
  assert.equal(nonBlackPixelRatio(null), 0);
  assert.equal(nonBlackPixelRatio(undefined), 0);
  assert.equal(nonBlackPixelRatio(new Uint8Array(0)), 0);
  assert.equal(nonBlackPixelRatio(new Uint8Array(3)), 0);

  // Four all-zero pixels.
  assert.equal(nonBlackPixelRatio(new Uint8Array(16)), 0);

  assert.equal(nonBlackPixelRatio(new Uint8Array([
    64, 128, 192, 255,
    64, 128, 192, 255,
  ])), 1);
  assert.equal(nonBlackPixelRatio(new Uint8Array([
    0, 0, 0, 255,
    64, 128, 192, 255,
  ])), 0.5);
});

test('launcher blocks startup on a failed renderer self-test', async () => {
  const app = await readFile(new URL('../app.js', import.meta.url), 'utf8');
  assert.match(app, /runWebGLDiagnostics\(\)/);
  assert.match(app, /!state\.rendererReady/);
  assert.match(app, /\[renderer:error\]/);
});

test('framebuffer self-test validates a generated world draw', async () => {
  const diagnostics = await readFile(new URL('../lib/webgl-diagnostics.js', import.meta.url), 'utf8');
  assert.match(diagnostics, /gl\.clearColor\(0, 0, 0, 1\)/);
  assert.match(diagnostics, /gl\.drawArrays\(gl\.TRIANGLES, 0, 6\)/);
  assert.match(diagnostics, /generated world draw returned/);
  assert.match(diagnostics, /gamma post-process returned/);
  assert.match(diagnostics, /palette post-process returned/);
});

test('every engine shader family carries an ES-tier header', async () => {
  const [shader, postprocess] = await Promise.all([
    readFile(new URL('../../engine/h2shared/gl_shader.c', import.meta.url), 'utf8'),
    readFile(new URL('../../engine/h2shared/gl_postprocess.c', import.meta.url), 'utf8'),
  ]);

  // The tier is USE_GLES, not __EMSCRIPTEN__: a desktop -DUSE_GLES=ON build
  // reaches the same code paths and is how ES regressions get caught locally.
  assert.match(shader, /#ifdef USE_GLES\n#define GLSL_VERT_HEADER\s+"#version 300 es/);
  assert.match(postprocess, /#ifdef USE_GLES\n#define PP_ES_PRECISION/);

  // Every post-process shader goes through the header macros.  Hardcoding
  // "#version 430 core" here is what took the whole chain -- gamma, contrast,
  // FXAA, bloom, render scale -- out on the ES tier in uhexen2-7wdv.  The
  // browser-profile shaders in gl_shader.c are covered by the extraction test
  // below, which compiles what the engine actually emits; gl_shader.c's
  // remaining literal 430s are the SSBO and instanced-alias programs, which
  // are built under #ifndef USE_GLES and can only ever be desktop.
  const hardcoded = postprocess.match(/^\s*"#version 430 core\\n"/gm) || [];
  assert.deepEqual(hardcoded, [], 'gl_postprocess.c hardcodes a desktop #version in a shader source');
});

test('post-process palette stays inside the WebGL2 fragment uniform budget', async () => {
  const postprocess = await readFile(new URL('../../engine/h2shared/gl_postprocess.c', import.meta.url), 'utf8');

  // A vec3[256] is 256 uniform vectors on its own, over the 224 a WebGL2
  // implementation is only required to offer a fragment shader -- the whole
  // post-process program can fail to link.  The palette LUT carries resolved
  // RGB instead.  d2c46f078.
  assert.doesNotMatch(postprocess, /uniform vec3 palette\[/);
  assert.match(postprocess, /texelFetch\(paletteLUT, idx, 0\)\.rgb/);
  assert.match(postprocess, /GL_RGB8, 32, 32, 32, 0,\s+GL_RGB,/);
});

test('brush-entity instancing stays off on the ES tier', async () => {
  const renderer = await readFile(new URL('../../engine/hexen2/gl_rmain.c', import.meta.url), 'utf8');
  assert.match(renderer, /qboolean R_BrushInst_Available \(void\)\n\{\n#ifdef USE_GLES/);
});

test('headless smoke gate compiles the actual engine WebGL shader sources', async () => {
  const programs = await extractEngineWebGLPrograms();
  const byName = Object.fromEntries(programs.map((program) => [program.name, program]));

  assert.deepEqual(programs.map(({ name }) => name), [
    '2d', 'flat', 'world', 'world_opaque', 'alias', 'particle', 'sky',
    'postprocess', 'bloom_bright', 'bloom_down', 'bloom_up',
  ]);
  // Spot-check that macro expansion actually happened rather than silently
  // emitting the macro name -- these live behind GLSL_BICUBIC_LM_FN etc.
  assert.match(byName.world.fragment, /vec4 BicubicLightmap/);
  assert.match(byName.world.fragment, /float Caustics/);
  assert.match(byName.postprocess.fragment, /vec4 fxaa/);
  assert.match(byName.postprocess.fragment, /HDR tonemapping/);
  for (const program of programs) {
    assert.match(program.vertex, /^#version 300 es\n/);
    assert.match(program.fragment, /^#version 300 es\n/);
    assert.doesNotMatch(program.vertex, /#version 430/);
    assert.doesNotMatch(program.fragment, /#version 430/);
  }
});
