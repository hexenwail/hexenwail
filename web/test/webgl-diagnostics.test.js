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

test('pixel-quality failures warn, structural failures throw', async () => {
  const diagnostics = await readFile(new URL('../lib/webgl-diagnostics.js', import.meta.url), 'utf8');

  // Structural: nothing can render, so refuse to start.
  assert.match(diagnostics, /throw new Error\('WebGL2 context creation failed'\)/);
  assert.match(diagnostics, /throw new Error\(`\$\{family\} shader compilation failed/);
  assert.match(diagnostics, /throw new Error\(`\$\{family\} shader link failed/);
  assert.match(diagnostics, /throw new Error\(`RGBA8 framebuffer incomplete/);

  // Quality: the engine is probably playable, so do not lock the user out.
  // d2c46f078 threw for all four of these.
  for (const probe of [
    /warnings\.push\(`generated world draw is predominantly black/,
    /warnings\.push\(`generated world draw returned/,
    /warnings\.push\(`gamma post-process returned/,
    /warnings\.push\(`palette post-process returned/,
    /warnings\.push\(`WebGL2 error after self-test/,
  ]) {
    assert.match(diagnostics, probe);
  }
});

test('the launcher proceeds on warnings but CI does not', async () => {
  const [app, smoke] = await Promise.all([
    readFile(new URL('../app.js', import.meta.url), 'utf8'),
    readFile(new URL('./webgl-smoke.html', import.meta.url), 'utf8'),
  ]);

  // The launcher logs warnings and keeps going: rendererReady is only ever
  // cleared in the catch block, never on a non-empty warnings array.
  assert.match(app, /\[renderer:warn\]/);
  assert.doesNotMatch(app, /warnings\.length[\s\S]{0,80}rendererReady = false/);

  // CI runs a pinned software rasterizer, so there a moved pixel is a
  // regression and must fail the gate -- from either renderer's self-test.
  assert.match(smoke, /const warnings = \[\.\.\.report\.warnings, \.\.\.presenter\.warnings\]/);
  assert.match(smoke, /warnings\.length > 0/);
  assert.match(smoke, /dataset\.result = 'fail'/);
});

test('the launcher gates on the renderer its artifact was built with', async () => {
  const [app, assemble, cmake] = await Promise.all([
    readFile(new URL('../app.js', import.meta.url), 'utf8'),
    readFile(new URL('../../scripts/wasm-assemble-artifact.sh', import.meta.url), 'utf8'),
    readFile(new URL('../../engine/CMakeLists.txt', import.meta.url), 'utf8'),
  ]);

  // Build stamp, not sniffing: CMake writes the renderer name, the assembler
  // substitutes it, and the launcher reads the substituted constant.
  assert.match(cmake, /OUTPUT \$\{CMAKE_RUNTIME_OUTPUT_DIRECTORY\}\/hexenwail-renderer\.txt/);
  assert.match(assemble, /__HEXENWAIL_RENDERER__/);
  assert.match(app, /const BUILD_RENDERER_STAMP = '__HEXENWAIL_RENDERER__'/);

  // An unsubstituted placeholder is the development case and must behave
  // exactly as it did before the software renderer existed.
  assert.match(app, /BUILD_RENDERER_STAMP === 'software' \? 'software' : 'webgl2'/);

  // The software artifact never runs the GL renderer's self-test: it has no
  // world or post-process shaders to compile.
  assert.match(app, /ENGINE_RENDERER === 'software'\s*\n\s*\? runPresenterDiagnostics\(\)\s*\n\s*: runWebGLDiagnostics\(\)/);
});

test('the presenter self-test mirrors the engine palette blit', async () => {
  const [diagnostics, presenter] = await Promise.all([
    readFile(new URL('../lib/webgl-diagnostics.js', import.meta.url), 'utf8'),
    readFile(new URL('../../engine/h2shared/web_canvas_gl2.c', import.meta.url), 'utf8'),
  ]);

  // The C presenter is the thing being gated, so the probe has to sample the
  // same way it does: an R8UI source expanded through a 256x1 RGBA8 LUT.
  for (const shared of [
    /usampler2D u_indexed/,
    /texelFetch\(u_palette, ivec2\(int\(idx\), 0\), 0\)\.rgb/,
  ]) {
    assert.match(diagnostics, shared);
    assert.match(presenter, shared);
  }
  assert.match(presenter, /GL_R8UI/);
  assert.match(diagnostics, /gl\.R8UI/);

  // Structural for the software build: no context, or a blit program that
  // will not build, means nothing reaches the canvas.
  assert.match(diagnostics, /throw new Error\('WebGL2 context creation failed'\)/);
  assert.match(diagnostics, /warnings\.push\(`palette blit returned/);
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
