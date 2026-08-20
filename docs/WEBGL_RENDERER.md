# WebGL2 renderer

The browser build is one instance of the engine's **GL ES 3.0 tier**, selected
by `USE_GLES` — Emscripten implies it, and a desktop `-DUSE_GLES=ON` build
reaches the same code paths, which is how ES regressions get caught without a
browser. Startup prints the selected profile, drawable dimensions, capability
summary, shader status, lightmap state, and the result of an RGBA8 texture/FBO
self-test.

## Browser baseline

| Feature | ES tier behavior |
| --- | --- |
| World, sky, HUD, liquids | GLSL ES 3.00, RGBA8 lightmaps |
| Brush entities | Legacy per-entity walk (`R_BrushInst_Available` is false here) |
| Alias models | CPU submission path |
| Skeletal SSBO path | Not compiled; shader storage blocks are ES 3.10 |
| Particles | CPU-built triangles/points; the SSBO particle path is not compiled |
| Transparency | Sorted blending |
| OIT | Off: `glBlendFunci` is GL 4.0 and WebGL2 exposes no per-draw-buffer blending |
| Gamma, contrast, render scale, water warp, FXAA, bloom, motion blur | GLSL ES 3.00 post-process |
| Palette effects (`r_softemu`) | GLSL ES 3.00 post-process, RGB8 palette LUT |
| HDR targets (`r_hdr`) | Only with `EXT_color_buffer_float`; reset to 0 otherwise |
| Anisotropic filtering | Only with `EXT_texture_filter_anisotropic`; menu item hidden otherwise |
| Compute, indirect drawing | Not compiled |

`gl_lightmapfmt GL_LUMINANCE` and `-lm_1` are ignored on **every** tier, not
just in the browser — `GL_LUMINANCE` is not a core GL 4.3 format either, so the
engine warns and uses `GL_RGBA` rather than refusing to start (uhexen2-q3j7).

The palette LUT stores resolved RGB rather than an 8-bit index into a
`uniform vec3 palette[256]`. That array alone is 256 uniform vectors, over the
224 a WebGL2 implementation is only required to offer a fragment shader, so the
entire post-process program could fail to link on a conforming browser.

## Diagnostics

- `renderer_status` prints the profile, GL/GLSL versions, drawable and viewport
  dimensions, capabilities, shader program status, and lightmap/atlas state.
- `renderer_safe` resets the optional effects (`r_hdr`, `r_oit`, `r_bloom`,
  `r_scale`, `r_softemu`, `gl_fxaa`, `r_motionblur`) and restores the RGBA8
  atlas baseline.
- `[RENDERER] First world draw completed` separates "the renderer never got
  that far" from "the renderer drew something you cannot see".
- `[RENDERER] WARNING: lightmap atlas contains no lit texels` distinguishes an
  all-black atlas from one that was never uploaded — they look identical.

Shader and framebuffer failures are reported but are **not** fatal. Bricking
startup on a self-test false negative would be worse than the degraded
rendering it warns about, and the PWA launcher runs its own gate (below) that
can put a reason in front of the user, which a `Sys_Error` cannot.

## Regression gates

`scripts/webgl-smoke-test.sh` runs Chrome/Chromium with software WebGL2
(ANGLE + SwiftShader) and:

1. Compiles and links a representative set of shader families, draws generated
   albedo/lightmap/fullbright textures through a world-style contract into an
   RGBA8 FBO, and validates gamma and RGB palette-LUT post-processing —
   rejecting GL errors, unexpected pixels, and predominantly black output.
2. Extracts the engine's *actual* shader strings from `gl_shader.c` and
   `gl_postprocess.c` via `scripts/webgl-engine-shader-smoke.mjs` and compiles
   and links every browser-profile program in WebGL2.

Step 2 is the one that matters: a hand-maintained copy of the shaders only
tells you the copy is valid, which is not the question.

`node --test web/test/*.test.js` covers the black-frame detector, the launcher
failure gate, and the source-level invariants (ES headers present, no palette
uniform array, brush instancing off on the tier). It does not need a browser.

**Known limit:** SwiftShader is lenient about some ES 3.00 conformance rules —
it accepts `bitfieldReverse`, which is GLSL 4.00 / ES 3.10, so the gate would
not by itself have caught uhexen2-7wdv's polyfill need. Treat it as a guard
against structural regressions (link failures, uniform budgets, missing
declarations), not as a conformance validator.

Blackmarsh visual acceptance still requires legally supplied game data: verify
world lighting, brush and alias models, HUD, liquids, particles, sky,
gamma/contrast, and supported post-processing with clean and legacy configs.
