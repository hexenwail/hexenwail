#!/usr/bin/env node
//
// Extract the engine's *actual* GLSL sources for the ES tier and hand them to a
// browser to compile and link.
//
// The point is that this reads engine/h2shared/gl_shader.c and gl_postprocess.c
// rather than keeping a parallel copy of the shaders: a hand-maintained copy
// tells you the copy is valid GLSL ES 3.00, which is not the question.  Two
// real regressions motivated it — `bitfieldReverse` (GLSL 4.00, absent from ES
// 3.00) and a `uniform vec3 palette[256]` that overran the 224 fragment uniform
// vectors WebGL2 is only required to offer.  Both compiled fine on the desktop
// and both silently disabled the entire post-process chain in a browser.
//
// The C is parsed, not preprocessed, so this understands exactly two shapes.
// The original one is `static const char <name>[] = <string literals and
// object-like macros>;` with the tier-specific header macros living in an
// `#ifdef USE_GLES` block.  The second is a stage that has moved out to
// engine/shaders/<name>.glsl and reaches the engine through shaders_gen.h --
// see readFileBackedShaders() below.  Anything fancier should make this file
// fail loudly rather than guess.
//
// Ported from alextnewman/hexenwail d2c46f078, adapted to this tree's macro
// layout (USE_GLES rather than __EMSCRIPTEN__, and PP_* headers local to
// gl_postprocess.c).

import { readdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SHADER_SOURCE = resolve(ROOT, 'engine/h2shared/gl_shader.c');
const POSTPROCESS_SOURCE = resolve(ROOT, 'engine/h2shared/gl_postprocess.c');
const SHADER_FILE_DIR = resolve(ROOT, 'engine/shaders');

const PROGRAM_SPECS = [
  ['2d', 'shader', 's2d_vert', 's2d_frag'],
  ['flat', 'shader', 'sflat_vert', 'sflat_frag'],
  ['world', 'shader', 'sworld_vert', 'sworld_frag'],
  ['world_opaque', 'shader', 'sworld_vert', 'sworld_frag_opaque'],
  ['alias', 'shader', 'salias_vert', 'salias_frag'],
  ['particle', 'shader', 'spart_vert', 'spart_frag'],
  // Two programs since uhexen2-a5nn.4 split the one that branched on
  // u_alpha_threshold; both must still compile on the ES tier.
  ['sky_layers', 'shader', 'ssky_layers_vert', 'ssky_layers_frag'],
  ['sky_boxside', 'shader', 'ssky_side_vert', 'ssky_side_frag'],
  ['sky_cubemap', 'shader', 'ssky_cube_vert', 'ssky_cube_frag'],
  ['postprocess', 'postprocess', 'pp_vert_src', 'pp_frag_src'],
  ['bloom_bright', 'postprocess', 'bloom_vert_src', 'bloom_bright_frag_src'],
  ['bloom_down', 'postprocess', 'bloom_vert_src', 'bloom_down_frag_src'],
  ['bloom_up', 'postprocess', 'bloom_vert_src', 'bloom_up_frag_src'],
];

function tokenize(expression) {
  return expression.match(/"(?:\\.|[^"\\])*"|\/\*[\s\S]*?\*\/|\/\/[^\n]*|[A-Za-z_]\w*/g) || [];
}

// How many times `name` is #defined in `source`.  One means the macro has a
// single spelling and can be resolved without anyone deciding anything; more
// than one means it has a USE_GLES arm and a desktop arm, and which one this
// extractor wants is a judgement it must not make on its own.
function countDefines(source, name) {
  const matches = source.match(new RegExp(`^\\s*#define\\s+${name}\\b`, 'gm'));
  return matches ? matches.length : 0;
}

// Resolve a macro nobody registered.  uhexen2-rnzi: the registered set used to
// be a hardcoded list, so every new GLSL helper macro broke the PWA/WASM CI
// job with "unknown C string macro" until someone added its name -- and it did
// break twice, GLSL_MATERIAL_FN on 0.8.0-beta.r21 being the second, found
// after the tag because native and wasm builds both compile such a macro
// fine and only this test sees it.  Discovering it here turns that class of
// break into a no-op.  The tier-specific macros stay explicitly registered:
// picking the ES arm over the desktop one is exactly the judgement above, so a
// multiply-defined macro is still an error, just an actionable one.
function resolveMacro(name, macros, ctx) {
  if (macros.has(name)) return macros.get(name);
  if (ctx.resolving.has(name)) throw new Error(`shader macro ${name} is defined in terms of itself`);

  const found = countDefines(ctx.source, name);
  if (found === 0) throw new Error(`unknown C string macro ${name}: no #define in ${ctx.name}`);
  if (found > 1) {
    throw new Error(
      `shader macro ${name} is #defined ${found} times in ${ctx.name}, so it has tier-specific `
      + 'arms; register it in the tier-specific list in extractEngineWebGLPrograms() so the '
      + 'USE_GLES spelling is the one that gets used');
  }

  ctx.resolving.add(name);
  try {
    const value = readDefine(ctx.source, name, macros, ctx);
    macros.set(name, value);
    return value;
  } finally {
    ctx.resolving.delete(name);
  }
}

function evaluateCStringExpression(expression, macros, ctx) {
  let value = '';
  for (const token of tokenize(expression)) {
    if (token.startsWith('"')) {
      value += JSON.parse(token);
    } else if (!token.startsWith('/*') && !token.startsWith('//')) {
      value += resolveMacro(token, macros, ctx);
    }
  }
  return value;
}

// Slice the arm that a USE_GLES build takes, out of the conditional that
// defines `name`, so readDefine below picks up the ES spelling of a macro that
// also has a desktop one.
//
// BOTH SPELLINGS OF THE CONDITIONAL, because gl_shader.c uses both and which
// one a given macro got is a matter of what read better where it was written:
//
//   #ifdef USE_GLES  <- ES arm    #ifndef USE_GLES  <- desktop arm
//   #else            <- desktop   #else             <- ES arm
//   #endif                        #endif
//
// This used to understand only the first, which is why it could not resolve
// GLSL_DLIGHT_DECL and friends: it walked past their `#ifndef` looking for an
// `#ifdef` that was not there, ran off the end of the file and reported "no
// #ifdef USE_GLES branch defines ...".  That surfaced as the registration
// error instead, because a caller only reaches here after countDefines has
// already found the macro twice.  uhexen2-waum.
//
// Deliberately naive about nesting -- it takes the first #else/#endif after
// the opener, so a conditional nested inside one of these arms would slice
// wrong.  None of the tier-specific macro blocks in gl_shader.c or
// gl_postprocess.c have one, and a wrong slice fails loudly (the #define is
// not in it) rather than silently picking the desktop text.
function glesBranchDefining(source, name) {
  const define = new RegExp(`^\\s*#define\\s+${name}\\b`, 'm');
  const opener = /^#(ifdef|ifndef)\s+USE_GLES\b/gm;
  for (let match = opener.exec(source); match; match = opener.exec(source)) {
    const start = match.index;
    const elseAt = source.indexOf('\n#else', start);
    const endAt = source.indexOf('\n#endif', start);
    const hasElse = elseAt >= 0 && (endAt < 0 || elseAt < endAt);
    // `#ifdef` puts the ES text first, `#ifndef` puts it in the #else arm --
    // and a bare `#ifndef USE_GLES` with no #else contributes nothing at all
    // to an ES build, which is a real shape here (whole blocks of desktop-only
    // shaders) but never one that defines a macro the ES tier then references.
    const branch = match[1] === 'ifdef'
      ? source.slice(start, hasElse ? elseAt : (endAt < 0 ? source.length : endAt))
      : (hasElse ? source.slice(elseAt, endAt < 0 ? source.length : endAt) : '');
    if (define.test(branch)) return branch;
  }
  throw new Error(`no USE_GLES branch defines ${name}`);
}

// Is a block comment still open at the end of `line`?  `/*` inside a string
// literal does not open one, which is why this walks the line rather than
// running a regex over it.
function blockCommentOpenAfter(line, inComment) {
  let inString = false;
  for (let i = 0; i < line.length; i += 1) {
    if (inComment) {
      if (line[i] === '*' && line[i + 1] === '/') { inComment = false; i += 1; }
      continue;
    }
    if (inString) {
      if (line[i] === '\\') { i += 1; continue; }
      if (line[i] === '"') inString = false;
      continue;
    }
    if (line[i] === '"') { inString = true; continue; }
    if (line[i] === '/' && line[i + 1] === '*') { inComment = true; i += 1; continue; }
    if (line[i] === '/' && line[i + 1] === '/') break;
  }
  return inComment;
}

// `source` is the text to find the #define in -- for a tier-specific macro that
// is one preprocessor arm, not the whole file.  `ctx` is what any macro NESTED
// inside the body resolves against, and that must stay the whole file.
function readDefine(source, name, macros, ctx) {
  const lines = source.split('\n');
  const start = lines.findIndex((line) => new RegExp(`^\\s*#define\\s+${name}\\b`).test(line));
  if (start < 0) throw new Error(`missing shader macro ${name}`);

  const parts = [];
  let inComment = false;
  for (let index = start; index < lines.length; index += 1) {
    let line = lines[index];
    if (index === start) line = line.replace(new RegExp(`^\\s*#define\\s+${name}\\b`), '');
    inComment = blockCommentOpenAfter(line, inComment);
    parts.push(line.replace(/\\\s*$/, ''));
    // A block comment carries the definition across newlines by itself: the C
    // preprocessor replaces the whole comment with a single space in phase 3,
    // so the interior newlines never terminate the directive and the interior
    // lines carry no backslashes.  GLSL_MATERIAL_FN is written that way.
    if (inComment) continue;
    if (!/\\\s*$/.test(line)) break;
  }
  return evaluateCStringExpression(parts.join('\n'), macros, ctx);
}

function arrayMarker(name) {
  return new RegExp(`static\\s+const\\s+char\\s+${name}\\s*\\[\\s*\\]\\s*=`, 'm');
}

function readArrayExpression(source, name) {
  const match = arrayMarker(name).exec(source);
  if (!match) throw new Error(`missing shader source ${name}`);

  let inString = false;
  let inLineComment = false;
  let inBlockComment = false;
  let escaped = false;
  const start = match.index + match[0].length;

  for (let index = start; index < source.length; index += 1) {
    const char = source[index];
    const next = source[index + 1];

    if (inString) {
      if (escaped) escaped = false;
      else if (char === '\\') escaped = true;
      else if (char === '"') inString = false;
      continue;
    }
    if (inLineComment) {
      if (char === '\n') inLineComment = false;
      continue;
    }
    if (inBlockComment) {
      if (char === '*' && next === '/') {
        inBlockComment = false;
        index += 1;
      }
      continue;
    }
    if (char === '"') inString = true;
    else if (char === '/' && next === '/') {
      inLineComment = true;
      index += 1;
    } else if (char === '/' && next === '*') {
      inBlockComment = true;
      index += 1;
    } else if (char === ';') {
      return source.slice(start, index);
    }
  }
  throw new Error(`unterminated shader source ${name}`);
}

// Exported for the tests only.  extractEngineWebGLPrograms() reads the real
// engine files at fixed paths, so proving that a NEW helper macro resolves
// without being registered needs a seam that takes synthetic source.
export function expandShaderExpression(expression, source, sourceName = 'test source') {
  return evaluateCStringExpression(expression, new Map(),
    { name: sourceName, source, resolving: new Set() });
}

// --- the file route ----------------------------------------------------
//
// A stage's GLSL used to live only in the C, as a string-literal array.  Since
// `render: prove the shader-to-SPIR-V route on one 12-line compute shader` it
// may instead live in engine/shaders/<name>.glsl and reach the engine as a
// <name>[] symbol in shaders_gen.h, generated at build time by
// engine/cmake/EmbedShaders.cmake.  This gate has to follow, or it stops
// covering every shader that moves -- silently, since a spec whose symbol has
// gone reads here exactly like one that was never right.  It went red that way
// on s2d_vert the first time two programs moved.

// Resolve #include the way EmbedShaders.cmake resolves it: plain text
// substitution, bounded passes so a cycle fails instead of hanging.  Matching
// its behaviour matters more than doing includes properly -- a difference
// between the two would make this a gate on text that no build ever sees.
// (GLSL has no #include of its own: ARB_shading_language_include is not core
// and does not exist in GLSL ES at all, which is why the expansion is offline.)
async function expandShaderIncludes(name, body) {
  for (let pass = 0; pass <= 8; pass += 1) {
    const match = /#include "([^"]+)"/.exec(body);
    if (!match) break;
    let included;
    try {
      included = await readFile(resolve(SHADER_FILE_DIR, match[1]), 'utf8');
    } catch {
      throw new Error(`shader ${name} includes missing ${match[1]}`);
    }
    body = body.replaceAll(`#include "${match[1]}"\n`, included);
  }
  if (/#include "/.test(body))
    throw new Error(`shader ${name} has unresolved or cyclic includes`);
  return body;
}

// Every stage that lives in engine/shaders, keyed by the C symbol it becomes.
// That key is just the basename: EmbedShaders.cmake names the symbol after the
// file, so s2d_vert.glsl is the s2d_vert[] the C compiles.
async function readFileBackedShaders() {
  const files = (await readdir(SHADER_FILE_DIR)).filter((f) => f.endsWith('.glsl')).sort();
  const sources = new Map();
  for (const file of files) {
    const name = file.slice(0, -'.glsl'.length);
    sources.set(name, await expandShaderIncludes(
      name, await readFile(resolve(SHADER_FILE_DIR, file), 'utf8')));
  }
  return sources;
}

// Which #version header the engine compiles a file-backed stage with.
//
// The .glsl files carry no #version line on purpose -- the GL side passes one
// as a separate glShaderSource chunk because the desktop and ES tiers want
// different ones, and EmbedShaders.cmake prepends its own for the offline
// SPIR-V compile -- so this has to supply the ES one or hand the browser a
// shader with no version at all.
//
// READ OUT OF THE CALL SITE, not inferred from the _vert/_frag suffix.  The
// pairing is a fact about the engine, stated by the argument order of the
// GL_InitProgramSplit() call; inferring it would be this file quietly deciding
// something it is the engine's business to say -- the same judgement
// resolveMacro() already refuses to make for a tier-specific macro.
function splitHeaderFor(ctx, symbol) {
  const call = /GL_InitProgramSplit\s*\(([^;]*)\)\s*;/g;
  for (let match = call.exec(ctx.source); match; match = call.exec(ctx.source)) {
    const args = match[1]
      .replace(/\/\*[\s\S]*?\*\//g, '')
      .replace(/\/\/[^\n]*/g, '')
      .split(',')
      .map((arg) => arg.trim());
    /* (program, name, vert_header, vert_src, frag_header, frag_src) */
    if (args[3] === symbol) return args[2];
    if (args[5] === symbol) return args[4];
  }
  throw new Error(
    `engine/shaders/${symbol}.glsl exists but no GL_InitProgramSplit() call in `
    + `${ctx.name} names ${symbol}, so there is no way to know which #version `
    + 'header the engine compiles it with');
}

export async function extractEngineWebGLPrograms() {
  const [shaderSource, postprocessSource, fileBacked] = await Promise.all([
    readFile(SHADER_SOURCE, 'utf8'),
    readFile(POSTPROCESS_SOURCE, 'utf8'),
    readFileBackedShaders(),
  ]);

  // One shared macro table, as before: the two files' macro namespaces are
  // disjoint in practice (GLSL_* in gl_shader.c, PP_* in gl_postprocess.c) and
  // a name already in the table is never looked up again.
  const macros = new Map();
  const shaderCtx = { name: 'gl_shader.c', source: shaderSource, resolving: new Set() };
  const postprocessCtx = { name: 'gl_postprocess.c', source: postprocessSource, resolving: new Set() };

  // THE ONLY MACROS THAT STILL HAVE TO BE NAMED HERE are the tier-specific
  // ones -- those with both a USE_GLES arm and a desktop arm, where this
  // extractor has to say which spelling it wants.  Everything else is found on
  // demand by resolveMacro(), so a new GLSL helper needs no edit to this file.
  // Clustered dynamic lighting (uhexen2-26bm world, uhexen2-waum alias) is
  // desktop-only -- it needs a compute shader and an SSBO, and the ES tier has
  // neither -- so every one of its macros has an empty ES arm and has to be
  // named here.  The alias ones include the varying declarations, because an
  // `in` with no matching `out` is a link error and the ES tier has nothing to
  // fill them with.
  for (const name of ['GLSL_VERT_HEADER', 'GLSL_FRAG_HEADER', 'GLSL_EARLY_Z', 'GLSL_EARLY_Z_OPAQUE',
                      'GLSL_BITFIELD_REVERSE',
                      'GLSL_DLIGHT_DECL', 'GLSL_DLIGHT_FN', 'GLSL_DLIGHT_APPLY',
                      'GLSL_ALIAS_DLIGHT_DECL', 'GLSL_ALIAS_DLIGHT_FN', 'GLSL_ALIAS_DLIGHT_APPLY',
                      'GLSL_ALIAS_DLIGHT_VS_OUT', 'GLSL_ALIAS_DLIGHT_FS_IN',
                      'GLSL_ALIAS_DLIGHT_VS_CALC']) {
    macros.set(name, readDefine(glesBranchDefining(shaderSource, name), name, macros, shaderCtx));
  }
  // gl_postprocess.c keeps its own headers; PP_ES_PRECISION feeds the other two.
  for (const name of ['PP_VERT_HEADER', 'PP_FRAG_HEADER', 'PP_BITFIELD_REVERSE']) {
    macros.set(name, readDefine(glesBranchDefining(postprocessSource, name), name, macros, postprocessCtx));
  }

  const contexts = { shader: shaderCtx, postprocess: postprocessCtx };

  // A stage is a C array or a file, and the two routes have to stay exclusive:
  // both at once means a moved shader left its old copy behind, and the C one
  // is what the engine would still compile while this gate read the new file.
  const stage = (family, symbol) => {
    const ctx = contexts[family];
    const inC = arrayMarker(symbol).test(ctx.source);
    const inFile = fileBacked.has(symbol);
    if (inC && inFile) {
      throw new Error(
        `${symbol} is both a string-literal array in ${ctx.name} and `
        + `engine/shaders/${symbol}.glsl; delete whichever one the engine no longer builds`);
    }
    if (inC)
      return evaluateCStringExpression(readArrayExpression(ctx.source, symbol), macros, ctx);
    if (inFile)
      return resolveMacro(splitHeaderFor(ctx, symbol), macros, ctx) + fileBacked.get(symbol);
    throw new Error(
      `missing shader source ${symbol}: no array in ${ctx.name} and no `
      + `engine/shaders/${symbol}.glsl`);
  };

  return PROGRAM_SPECS.map(([name, family, vertexName, fragmentName]) => ({
    name,
    vertex: stage(family, vertexName),
    fragment: stage(family, fragmentName),
  }));
}

export function renderEngineShaderSmokePage(programs) {
  const serialized = JSON.stringify(programs).replaceAll('<', '\\u003c');
  return `<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><title>Engine WebGL2 shader smoke test</title></head>
<body data-result="pending"><pre id="result">pending</pre>
<script>
const sources = ${serialized};
const output = document.getElementById('result');
function compile(gl, type, source, family, stage) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader) || 'unknown compile error';
    gl.deleteShader(shader);
    throw new Error(family + ' ' + stage + ' shader compilation failed: ' + message);
  }
  return shader;
}
try {
  const canvas = document.createElement('canvas');
  const gl = canvas.getContext('webgl2', { antialias: false });
  if (!gl) throw new Error('WebGL2 context creation failed');
  for (const source of sources) {
    const vertex = compile(gl, gl.VERTEX_SHADER, source.vertex, source.name, 'vertex');
    const fragment = compile(gl, gl.FRAGMENT_SHADER, source.fragment, source.name, 'fragment');
    const program = gl.createProgram();
    gl.attachShader(program, vertex);
    gl.attachShader(program, fragment);
    gl.bindAttribLocation(program, 0, 'a_position');
    gl.bindAttribLocation(program, 1, 'a_texcoord');
    gl.bindAttribLocation(program, 2, 'a_lmcoord');
    gl.bindAttribLocation(program, 3, 'a_color');
    gl.linkProgram(program);
    gl.deleteShader(vertex);
    gl.deleteShader(fragment);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      const message = gl.getProgramInfoLog(program) || 'unknown link error';
      gl.deleteProgram(program);
      throw new Error(source.name + ' shader link failed: ' + message);
    }
    gl.deleteProgram(program);
  }
  const error = gl.getError();
  if (error !== gl.NO_ERROR) throw new Error('WebGL2 error after engine shader test: 0x' + error.toString(16));
  document.body.dataset.result = 'pass';
  output.textContent = JSON.stringify({ profile: gl.getParameter(gl.VERSION), shaders: sources.map(({ name }) => name) });
} catch (error) {
  document.body.dataset.result = 'fail';
  output.textContent = error.stack || error.message;
}
</script></body></html>
`;
}

async function main() {
  const output = process.argv[2];
  if (!output) throw new Error('usage: webgl-engine-shader-smoke.mjs <output.html>');
  const programs = await extractEngineWebGLPrograms();
  await writeFile(output, renderEngineShaderSmokePage(programs));
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
