const HEADER = `#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;
precision highp sampler3D;
`;

const GEOMETRY_VERTEX = `${HEADER}
layout(location=0) in vec3 a_position;
layout(location=1) in vec2 a_texcoord;
layout(location=3) in vec4 a_color;
out vec2 v_uv;
out vec4 v_color;
void main() {
  v_uv = a_texcoord;
  v_color = a_color;
  gl_Position = vec4(a_position, 1.0);
}`;

const TEXTURED_FRAGMENT = `${HEADER}
uniform sampler2D u_texture0;
in vec2 v_uv;
in vec4 v_color;
out vec4 fragColor;
void main() {
  fragColor = texture(u_texture0, v_uv) * v_color;
}`;

const WORLD_FRAGMENT = `${HEADER}
uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
uniform sampler2D u_texture2;
in vec2 v_uv;
in vec4 v_color;
out vec4 fragColor;
void main() {
  vec2 size = vec2(textureSize(u_texture1, 0));
  vec3 albedo = texture(u_texture0, v_uv).rgb;
  vec3 lightmap = texture(u_texture1, v_uv + vec2(0.5) / size).rgb;
  vec3 fullbright = texture(u_texture2, v_uv).rgb;
  fragColor = vec4(albedo * lightmap * v_color.rgb + fullbright, 1.0);
}`;

const FULLSCREEN_VERTEX = `${HEADER}
out vec2 v_uv;
void main() {
  ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);
  vec2 position = vec2(v) * 4.0 - 1.0;
  v_uv = position * 0.5 + 0.5;
  gl_Position = vec4(position, 0.0, 1.0);
}`;

const POSTPROCESS_FRAGMENT = `${HEADER}
uniform sampler2D scene;
uniform sampler3D paletteLUT;
uniform float gamma;
uniform float contrast;
uniform int softemu;
uniform float dither;
uniform float scale;
in vec2 v_uv;
out vec4 fragColor;
uint reverse8(uint value) {
  value &= 0xffu;
  value = ((value & 0x55u) << 1) | ((value >> 1) & 0x55u);
  value = ((value & 0x33u) << 2) | ((value >> 2) & 0x33u);
  return ((value & 0x0fu) << 4) | (value >> 4);
}
float bayer16(ivec2 coordinates) {
  coordinates &= 15;
  coordinates.y ^= coordinates.x;
  uint value = uint(coordinates.y | (coordinates.x << 8));
  value = (value ^ (value << 2)) & 0x3333u;
  value = (value ^ (value << 1)) & 0x5555u;
  value |= value >> 7;
  return float(reverse8(value)) / 256.0 - 0.5;
}
void main() {
  vec4 color = texture(scene, v_uv);
  if (contrast != 1.0) color.rgb = (color.rgb - 0.5) * contrast + 0.5;
  if (gamma != 1.0) color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(gamma));
  color.rgb = clamp(color.rgb, 0.0, 1.0);
  if (softemu > 0) {
    vec3 adjusted = color.rgb;
    if (softemu == 1) adjusted += bayer16(ivec2(gl_FragCoord.xy * scale)) * dither / 16.0;
    ivec3 index = ivec3(clamp(adjusted, 0.0, 1.0) * 31.0 + 0.5);
    color.rgb = texelFetch(paletteLUT, index, 0).rgb;
  }
  fragColor = color;
}`;

const BLOOM_FRAGMENT = `${HEADER}
uniform sampler2D u_scene;
in vec2 v_uv;
layout(location=0) out vec4 fragColor;
void main() {
  vec3 color = texture(u_scene, v_uv).rgb;
  float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
  fragColor = vec4(color * max(luminance, 0.001), 1.0);
}`;

const SHADER_FAMILIES = [
  ['2d', GEOMETRY_VERTEX, TEXTURED_FRAGMENT],
  ['flat', GEOMETRY_VERTEX, TEXTURED_FRAGMENT],
  ['world', GEOMETRY_VERTEX, WORLD_FRAGMENT],
  ['world_opaque', GEOMETRY_VERTEX, WORLD_FRAGMENT],
  ['alias', GEOMETRY_VERTEX, TEXTURED_FRAGMENT],
  ['particle', GEOMETRY_VERTEX, TEXTURED_FRAGMENT],
  ['sky', GEOMETRY_VERTEX, TEXTURED_FRAGMENT],
  ['postprocess', FULLSCREEN_VERTEX, POSTPROCESS_FRAGMENT],
  ['bloom_bright', FULLSCREEN_VERTEX, BLOOM_FRAGMENT],
  ['bloom_down', FULLSCREEN_VERTEX, BLOOM_FRAGMENT],
  ['bloom_up', FULLSCREEN_VERTEX, BLOOM_FRAGMENT],
];

function compileShader(gl, type, source, family) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader) || 'unknown compile error';
    gl.deleteShader(shader);
    throw new Error(`${family} shader compilation failed: ${message}`);
  }
  return shader;
}

function compileProgram(gl, family, vertexSource, fragmentSource) {
  const vertex = compileShader(gl, gl.VERTEX_SHADER, vertexSource, family);
  const fragment = compileShader(gl, gl.FRAGMENT_SHADER, fragmentSource, family);
  const program = gl.createProgram();
  gl.attachShader(program, vertex);
  gl.attachShader(program, fragment);
  gl.linkProgram(program);
  gl.deleteShader(vertex);
  gl.deleteShader(fragment);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const message = gl.getProgramInfoLog(program) || 'unknown link error';
    gl.deleteProgram(program);
    throw new Error(`${family} shader link failed: ${message}`);
  }
  return program;
}

export function nonBlackPixelRatio(pixels, minimumLuminance = 8) {
  if (!pixels?.length) return 0;
  const pixelCount = Math.floor(pixels.length / 4);
  if (pixelCount === 0) return 0;
  let visible = 0;
  for (let offset = 0; offset < pixelCount * 4; offset += 4) {
    const luminance = pixels[offset] * 0.2126
      + pixels[offset + 1] * 0.7152
      + pixels[offset + 2] * 0.0722;
    if (luminance >= minimumLuminance) visible += 1;
  }
  return visible / pixelCount;
}

function createSolidTexture(gl, unit, color) {
  const texture = gl.createTexture();
  gl.activeTexture(gl.TEXTURE0 + unit);
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.texImage2D(
    gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
    new Uint8Array(color),
  );
  return texture;
}

function createPaletteTexture(gl, unit, color) {
  const texture = gl.createTexture();
  const pixels = new Uint8Array(32 * 32 * 32 * 3);
  for (let offset = 0; offset < pixels.length; offset += 3) {
    pixels.set(color, offset);
  }
  gl.activeTexture(gl.TEXTURE0 + unit);
  gl.bindTexture(gl.TEXTURE_3D, texture);
  gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);
  gl.texImage3D(
    gl.TEXTURE_3D, 0, gl.RGB8, 32, 32, 32, 0, gl.RGB, gl.UNSIGNED_BYTE,
    pixels,
  );
  return texture;
}

function framebufferSelfTest(gl, worldProgram) {
  const width = 32;
  const height = 32;
  const targetTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, targetTexture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);

  const framebuffer = gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, targetTexture, 0);
  const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
  if (status !== gl.FRAMEBUFFER_COMPLETE) {
    throw new Error(`RGBA8 framebuffer incomplete: 0x${status.toString(16)}`);
  }

  const sourceTextures = [
    createSolidTexture(gl, 0, [128, 96, 64, 255]),
    createSolidTexture(gl, 1, [192, 192, 192, 255]),
    createSolidTexture(gl, 2, [16, 8, 4, 255]),
  ];
  const vertices = new Float32Array([
    -1, -1, 0, 0, 0, 1, 1, 1, 1,
    1, -1, 0, 1, 0, 1, 1, 1, 1,
    -1, 1, 0, 0, 1, 1, 1, 1, 1,
    -1, 1, 0, 0, 1, 1, 1, 1, 1,
    1, -1, 0, 1, 0, 1, 1, 1, 1,
    1, 1, 0, 1, 1, 1, 1, 1, 1,
  ]);
  const vao = gl.createVertexArray();
  const vertexBuffer = gl.createBuffer();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, vertexBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 9 * 4, 0);
  gl.enableVertexAttribArray(1);
  gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 9 * 4, 3 * 4);
  gl.enableVertexAttribArray(3);
  gl.vertexAttribPointer(3, 4, gl.FLOAT, false, 9 * 4, 5 * 4);

  gl.useProgram(worldProgram);
  gl.uniform1i(gl.getUniformLocation(worldProgram, 'u_texture0'), 0);
  gl.uniform1i(gl.getUniformLocation(worldProgram, 'u_texture1'), 1);
  gl.uniform1i(gl.getUniformLocation(worldProgram, 'u_texture2'), 2);
  gl.viewport(0, 0, width, height);
  gl.clearColor(0, 0, 0, 1);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  const pixels = new Uint8Array(width * height * 4);
  gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
  const ratio = nonBlackPixelRatio(pixels);
  const sample = [...pixels.slice(0, 4)];

  gl.useProgram(null);
  gl.bindVertexArray(null);
  gl.deleteBuffer(vertexBuffer);
  gl.deleteVertexArray(vao);
  for (const sourceTexture of sourceTextures) gl.deleteTexture(sourceTexture);
  gl.activeTexture(gl.TEXTURE0);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.bindTexture(gl.TEXTURE_2D, null);
  gl.deleteFramebuffer(framebuffer);
  gl.deleteTexture(targetTexture);

  if (ratio < 0.9) {
    throw new Error(`generated world draw is predominantly black (${(ratio * 100).toFixed(1)}% visible)`);
  }
  const expected = [112, 80, 52, 255];
  if (sample.some((channel, index) => Math.abs(channel - expected[index]) > 2)) {
    throw new Error(`generated world draw returned ${sample.join(',')}, expected ${expected.join(',')}`);
  }
  return {
    width, height, draw: 'textured world triangle', nonBlackRatio: ratio, sample,
  };
}

function postprocessSelfTest(gl, postprocessProgram) {
  const width = 8;
  const height = 8;
  const sceneTexture = createSolidTexture(gl, 0, [128, 96, 64, 255]);
  const paletteColor = [40, 80, 120];
  const paletteTexture = createPaletteTexture(gl, 1, paletteColor);
  const targetTexture = gl.createTexture();
  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, targetTexture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);

  const framebuffer = gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, targetTexture, 0);
  const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
  if (status !== gl.FRAMEBUFFER_COMPLETE) {
    throw new Error(`post-process framebuffer incomplete: 0x${status.toString(16)}`);
  }
  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, sceneTexture);
  gl.activeTexture(gl.TEXTURE1);
  gl.bindTexture(gl.TEXTURE_3D, paletteTexture);

  const vao = gl.createVertexArray();
  gl.bindVertexArray(vao);
  gl.useProgram(postprocessProgram);
  gl.uniform1i(gl.getUniformLocation(postprocessProgram, 'scene'), 0);
  gl.uniform1i(gl.getUniformLocation(postprocessProgram, 'paletteLUT'), 1);
  gl.uniform1f(gl.getUniformLocation(postprocessProgram, 'contrast'), 1);
  gl.uniform1f(gl.getUniformLocation(postprocessProgram, 'dither'), 0);
  gl.uniform1f(gl.getUniformLocation(postprocessProgram, 'scale'), 1);
  gl.viewport(0, 0, width, height);

  const drawAndSample = (gamma, softemu) => {
    gl.uniform1f(gl.getUniformLocation(postprocessProgram, 'gamma'), gamma);
    gl.uniform1i(gl.getUniformLocation(postprocessProgram, 'softemu'), softemu);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    const sample = new Uint8Array(4);
    gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, sample);
    return [...sample];
  };

  const gammaSample = drawAndSample(2, 0);
  const paletteSample = drawAndSample(1, 2);

  gl.useProgram(null);
  gl.bindVertexArray(null);
  gl.deleteVertexArray(vao);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.deleteFramebuffer(framebuffer);
  gl.deleteTexture(targetTexture);
  gl.deleteTexture(sceneTexture);
  gl.activeTexture(gl.TEXTURE1);
  gl.bindTexture(gl.TEXTURE_3D, null);
  gl.deleteTexture(paletteTexture);
  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, null);

  const expectedGamma = [64, 36, 16, 255];
  if (gammaSample.some((channel, index) => Math.abs(channel - expectedGamma[index]) > 2)) {
    throw new Error(`gamma post-process returned ${gammaSample.join(',')}, expected ${expectedGamma.join(',')}`);
  }
  const expectedPalette = [...paletteColor, 255];
  if (paletteSample.some((channel, index) => Math.abs(channel - expectedPalette[index]) > 2)) {
    throw new Error(`palette post-process returned ${paletteSample.join(',')}, expected ${expectedPalette.join(',')}`);
  }

  return { gammaSample, paletteSample };
}

export function runWebGLDiagnostics({ canvas = null } = {}) {
  const target = canvas || document.createElement('canvas');
  target.width = 32;
  target.height = 32;
  const gl = target.getContext('webgl2', {
    alpha: false,
    antialias: false,
    depth: true,
    stencil: true,
    preserveDrawingBuffer: true,
  });
  if (!gl) throw new Error('WebGL2 context creation failed');

  const programs = [];
  try {
    for (const [family, vertexSource, fragmentSource] of SHADER_FAMILIES) {
      programs.push([family, compileProgram(gl, family, vertexSource, fragmentSource)]);
    }
    const worldProgram = programs.find(([family]) => family === 'world')?.[1];
    const postprocessProgram = programs.find(([family]) => family === 'postprocess')?.[1];
    const framebuffer = framebufferSelfTest(gl, worldProgram);
    const postprocess = postprocessSelfTest(gl, postprocessProgram);
    const error = gl.getError();
    if (error !== gl.NO_ERROR) {
      throw new Error(`WebGL2 error after self-test: 0x${error.toString(16)}`);
    }
    return {
      ok: true,
      profile: gl.getParameter(gl.VERSION),
      renderer: gl.getParameter(gl.RENDERER),
      shadingLanguage: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
      extensions: {
        colorBufferFloat: Boolean(gl.getExtension('EXT_color_buffer_float')),
        anisotropy: Boolean(
          gl.getExtension('EXT_texture_filter_anisotropic')
          || gl.getExtension('WEBKIT_EXT_texture_filter_anisotropic'),
        ),
        indexedBlend: Boolean(gl.getExtension('OES_draw_buffers_indexed')),
      },
      shaders: SHADER_FAMILIES.map(([family]) => family),
      framebuffer,
      postprocess,
    };
  } finally {
    for (const [, program] of programs) gl.deleteProgram(program);
    gl.getExtension('WEBGL_lose_context')?.loseContext();
  }
}
