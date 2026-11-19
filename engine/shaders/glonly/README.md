# engine/shaders/glonly

GLSL that only the GL side compiles. `engine/cmake/EmbedShaders.cmake` embeds
each `*.glsl` here into `shaders_gen.h` as a string-literal **macro**, named
after the file in upper case (`tier_core.glsl` becomes `TIER_CORE`). These
files are never written to the Vulkan staging directory, so the SPIR-V and
cross-compile gates (`shaders_spirv`, `shaders_cross`) do not see them.

| file | macro | used by |
|---|---|---|
| `tier_core.glsl` | `TIER_CORE` | the desktop GL 4.3 header chunk of every row of `gl_shader.c`'s program table (`rprog_defs`, #340 P1b): through `GLSL_*_HEADER` and `PP_*_HEADER`, and directly for the SSAO, cull/Hi-Z, shadow-map, light-cluster and softemu LUT programs |
| `tier_es.glsl` | `TIER_ES` | the ES 3.00 header chunk of the draw programs (`GLSL_*_HEADER`) |
| `tier_es_pp.glsl` | `TIER_ES_PP` | the ES 3.00 header chunk of the post-process programs (`PP_*_HEADER`, in `gl_shader.c` beside their rows): highp int and sampler3D as well |
| `webcanvas_blit_vert.glsl`, `webcanvas_blit_frag.glsl` | `WEBCANVAS_BLIT_VERT`, `WEBCANVAS_BLIT_FRAG` | `web_canvas_gl2.c`, the WebGL2 presenter of the software renderer (WEBSOFT) |

Rules, all from how the bytes are used:

- **Every byte reaches the driver.** There is no `#include` expansion here and
  no comment stripping, so these files hold no comments of their own. Explain
  them in the C that uses them, or in this file.
- **Macros, not arrays.** The C pastes the tier preambles next to other string
  literals in the program table (`TIER_CORE "#define QUALITY 1\n"`), which
  only a literal allows.
- **No `%` in a `tier_*` file**: the headers used to be `q_snprintf` formats,
  and a `%` would be one again the day someone formats one. EmbedShaders
  fails the build if one appears.
- **No `#version` in C.** `tools/test_no_version_in_c.py` fails on any C
  string literal under `engine/h2shared` or `engine/hexen2` that contains one
  (#450). A new tier or GL-only shader goes here instead.
