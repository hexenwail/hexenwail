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
// The C is parsed, not preprocessed, so this understands exactly one shape:
// `static const char <name>[] = <string literals and object-like macros>;`
// with the tier-specific header macros living in an `#ifdef USE_GLES` block.
// Anything fancier should make this file fail loudly rather than guess.
//
// Ported from alextnewman/hexenwail d2c46f078, adapted to this tree's macro
// layout (USE_GLES rather than __EMSCRIPTEN__, and PP_* headers local to
// gl_postprocess.c).

import { readFile, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SHADER_SOURCE = resolve(ROOT, 'engine/h2shared/gl_shader.c');
const POSTPROCESS_SOURCE = resolve(ROOT, 'engine/h2shared/gl_postprocess.c');

const PROGRAM_SPECS = [
  ['2d', 'shader', 's2d_vert', 's2d_frag'],
  ['flat', 'shader', 'sflat_vert', 'sflat_frag'],
  ['world', 'shader', 'sworld_vert', 'sworld_frag'],
  ['world_opaque', 'shader', 'sworld_vert', 'sworld_frag_opaque'],
  ['alias', 'shader', 'salias_vert', 'salias_frag'],
  ['particle', 'shader', 'spart_vert', 'spart_frag'],
  ['sky', 'shader', 'ssky_vert', 'ssky_frag'],
  ['postprocess', 'postprocess', 'pp_vert_src', 'pp_frag_src'],
  ['bloom_bright', 'postprocess', 'bloom_vert_src', 'bloom_bright_frag_src'],
  ['bloom_down', 'postprocess', 'bloom_vert_src', 'bloom_down_frag_src'],
  ['bloom_up', 'postprocess', 'bloom_vert_src', 'bloom_up_frag_src'],
];

function tokenize(expression) {
  return expression.match(/"(?:\\.|[^"\\])*"|\/\*[\s\S]*?\*\/|\/\/[^\n]*|[A-Za-z_]\w*/g) || [];
}

function evaluateCStringExpression(expression, macros) {
  let value = '';
  for (const token of tokenize(expression)) {
    if (token.startsWith('"')) {
      value += JSON.parse(token);
    } else if (!token.startsWith('/*') && !token.startsWith('//')) {
      if (!macros.has(token)) throw new Error(`unknown C string macro ${token}`);
      value += macros.get(token);
    }
  }
  return value;
}

// Slice the `#ifdef USE_GLES` arm that defines `name`, so readDefine below picks
// up the ES spelling of a macro that also has a desktop one.
function glesBranchDefining(source, name) {
  const define = new RegExp(`^\\s*#define\\s+${name}\\b`, 'm');
  let from = 0;
  for (;;) {
    const start = source.indexOf('#ifdef USE_GLES', from);
    if (start < 0) throw new Error(`no #ifdef USE_GLES branch defines ${name}`);
    const elseAt = source.indexOf('\n#else', start);
    const endAt = source.indexOf('\n#endif', start);
    const stop = elseAt >= 0 && (endAt < 0 || elseAt < endAt) ? elseAt : endAt;
    const branch = source.slice(start, stop < 0 ? source.length : stop);
    if (define.test(branch)) return branch;
    from = start + 1;
  }
}

function readDefine(source, name, macros) {
  const lines = source.split('\n');
  const start = lines.findIndex((line) => new RegExp(`^\\s*#define\\s+${name}\\b`).test(line));
  if (start < 0) throw new Error(`missing shader macro ${name}`);

  const parts = [];
  for (let index = start; index < lines.length; index += 1) {
    let line = lines[index];
    if (index === start) line = line.replace(new RegExp(`^\\s*#define\\s+${name}\\b`), '');
    const continued = /\\\s*$/.test(line);
    parts.push(line.replace(/\\\s*$/, ''));
    if (!continued) break;
  }
  return evaluateCStringExpression(parts.join('\n'), macros);
}

function readArrayExpression(source, name) {
  const marker = new RegExp(`static\\s+const\\s+char\\s+${name}\\s*\\[\\s*\\]\\s*=`, 'm');
  const match = marker.exec(source);
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

export async function extractEngineWebGLPrograms() {
  const [shaderSource, postprocessSource] = await Promise.all([
    readFile(SHADER_SOURCE, 'utf8'),
    readFile(POSTPROCESS_SOURCE, 'utf8'),
  ]);

  const macros = new Map();
  // Tier-specific: read from the USE_GLES arm, not the desktop one.
  for (const name of ['GLSL_VERT_HEADER', 'GLSL_FRAG_HEADER', 'GLSL_EARLY_Z', 'GLSL_EARLY_Z_OPAQUE']) {
    macros.set(name, readDefine(glesBranchDefining(shaderSource, name), name, macros));
  }
  // Tier-independent GLSL helper functions.
  for (const name of ['GLSL_BICUBIC_LM_FN', 'GLSL_CAUSTICS_FN']) {
    macros.set(name, readDefine(shaderSource, name, macros));
  }
  // gl_postprocess.c keeps its own headers; PP_ES_PRECISION feeds the other two.
  for (const name of ['PP_ES_PRECISION', 'PP_VERT_HEADER', 'PP_FRAG_HEADER', 'PP_BITFIELD_REVERSE']) {
    macros.set(name, readDefine(glesBranchDefining(postprocessSource, name), name, macros));
  }

  const sources = { shader: shaderSource, postprocess: postprocessSource };
  return PROGRAM_SPECS.map(([name, family, vertexName, fragmentName]) => ({
    name,
    vertex: evaluateCStringExpression(readArrayExpression(sources[family], vertexName), macros),
    fragment: evaluateCStringExpression(readArrayExpression(sources[family], fragmentName), macros),
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
