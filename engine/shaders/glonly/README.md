# engine/shaders/glonly

GLSL that only the GL side compiles. `engine/cmake/EmbedShaders.cmake` embeds
each `*.glsl` here into `shaders_gen.h` as a string-literal **macro**, named
after the file in upper case (`tier_core.glsl` becomes `TIER_CORE`). These
files are never written to the Vulkan staging directory, so the SPIR-V and
cross-compile gates (`shaders_spirv`, `shaders_cross`) do not see them.

| file | macro | used by |
|---|---|---|
| `tier_core.glsl` | `TIER_CORE` | the desktop GL 4.3 header chunk: `GLSL_*_HEADER`, `PP_*_HEADER`, `HIZ_COMPUTE_VERSION`, `SSAO_VERSION`, the shadow-map, light-cluster and softemu LUT programs |
| `tier_es.glsl` | `TIER_ES` | the ES 3.00 header chunk for `gl_shader.c` (`GLSL_*_HEADER`) |
| `tier_es_pp.glsl` | `TIER_ES_PP` | the ES 3.00 header chunk for `gl_postprocess.c` (`PP_*_HEADER`): highp int and sampler3D as well |
| `webcanvas_blit_vert.glsl`, `webcanvas_blit_frag.glsl` | `WEBCANVAS_BLIT_VERT`, `WEBCANVAS_BLIT_FRAG` | `web_canvas_gl2.c`, the WebGL2 presenter of the software renderer (WEBSOFT) |

Rules, all from how the bytes are used:

- **Every byte reaches the driver.** There is no `#include` expansion here and
  no comment stripping, so these files hold no comments of their own. Explain
  them in the C that uses them, or in this file.
- **Macros, not arrays.** The C pastes the tier preambles next to other string
  literals (`GLSL_VERT_HEADER "#define NOPERSP 1\n"`) and into `q_snprintf`
  formats, which only a literal allows.
- **No `%` in a `tier_*` file**, because of those formats. EmbedShaders fails
  the build if one appears.
- **No `#version` in C.** `tools/test_no_version_in_c.py` fails on any C
  string literal under `engine/h2shared` or `engine/hexen2` that contains one
  (#450). A new tier or GL-only shader goes here instead.
