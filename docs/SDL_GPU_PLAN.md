# SDL_GPU backend: plan and state

GitHub #340, under the decision in #447: SDL_GPU is the route to Vulkan, D3D12 and Metal. It is a fourth renderer pathway. GL 4.3, GLES3/WebGL2 and WEBSOFT all stay. The issue thread holds the full design notes. This file is the short version, kept next to the code.

## Done

| Phase | What | Where |
|---|---|---|
| Prereqs | Draw state routed (#337), texture binds routed (#338), uniforms in std140 blocks (#19), every stage in `engine/shaders` with an offline SPIR-V gate (#339/#450) | master |
| P−1a | Dense SDL_GPU bindings; `tools/test_sdlgpu_bindings.py` | #499 |
| P−1b | Every runtime shader variant compiled offline from `engine/shaders/variants.txt` | #499 |
| Y flip | `VK_FLIP_Y()` in every vertex stage, Vulkan arm only | #499 |
| P0 | `-gpu`: device, swapchain, clear colour; generated null-GL table; `gpu_unrouted` | #500 |
| P1a | Texture creation, upload and sampler state through `gl_texcreate.c` | #501 |
| P1b | Every program created by id through `R_Program_Create`; reflection rows in `gl_progid_refl.h` | #502 |
| P1c | Console, menus, HUD and plaque drawn through SDL_GPU. Files: `rg_frame.c`, `rg_resource.c`, `rg_pipeline.c`, `rg_shader.c` | #504 |
| Decision 6 | SPIR-V embedded: `EmbedSpirv.cmake` writes `generated/spirv_gen.c` (60 blobs, 638 KiB) under `USE_SDL_GPU`; `-gpu` boots with no flag. Checks: `tools/test_embed_spirv.py`, `tools/test_rg_sources.py` | #504 |
| P2a | Plumbing the world draws need, nothing new drawn. Sampler units through `R_ProgramSamplerLoc` / `R_ProgramSamplerUnit` (GL arm: the same `glGetUniformLocation` / `glUniform1i` calls); reflection records cube arrays, sampled class (float / int / uint), shadow and multisample, and leaves every non-`u_textureN` sampler unmapped until told; typed placeholders (float, int, uint per dimensionality, a depth cube array with a compare sampler); a zeroed 16 KiB stand-in for every read-only storage-buffer slot; the `DlShadow` mirror. Checks: `tools/test_rg_sampler_units.py`, the stand-in size in `tools/test_sdlgpu_bindings.py`. Known gaps: every OIT twin declares `sampler2DMS` (`oit.inc`), which SDL_GPU cannot sample, so their draws are dropped and counted until P6; and `sh_tex` now binds as a real cube array, so a shadow pass that ran under `-gpu` (gated off today by `R_LightCluster_Available`) would sample a map nothing rendered | branch `bobberb/sdlgpu-p2a-e34a6394` |
| P2b | The 3D world: demo1's world, brush entities, alias models and classic sky drawn through SDL_GPU. Depth reuses reversed-Z: `GL_Init_NullGL` turns `gl_clipcontrol_able` on, so `GL_Frustum` emits [0,1] and every call site takes its reversed-Z branch; depth clip follows the depth test (3D clips, 2D clamps). A clear seam in `gl_pipeline.c` (`R_ClearBuffers`, `R_SetClearColor` / `Depth` / `Stencil`; each GL arm the bare call) whose `-gpu` arm, `RG_Clear`, becomes the next render pass's load ops; a clear after draws ends the segment. `R_BuildWorldVBO`, `R_BuildSkyStencilVBO` and `R_BuildWorldCull` return under `-gpu`, so the world, brush entities and sky take their `GL_ImmEnd` fallbacks; alias instancing and `GL_DrawAliasSkeletal` need `shader_storage`; `GL_UnbindVertexArray` for the entity loops' tidy-up. Front face checked with culling on (clockwise is right: SDL's Vulkan viewport has negative height). Checks: `tools/sweep-world-gpu.steps` (GL vs `-gpu` at 40 dB PSNR, measured 49.1-52.5 on llvmpipe/lavapipe), `tools/shot-diff.sh`'s `PSNR_MIN` mode, `tools/test_rg_sources.py`. Known gaps: skeletal (IQM) models are not drawn, and are counted, until P2e; no stencil (P2c), so a skybox fills wherever depth is still at the far plane and geometry hidden behind sky brushes shows; the 3D draws that turn the depth test off (bounding boxes, pointfile) clamp where GL clips | branch `bobberb/sdlgpu-p2b-e34a6394` |
| P2c | Stencil under `-gpu`, so the sky depth+stencil pre-pass and the stencilled skybox run as on GL. The scene's depth target tries `D24_UNORM_S8_UINT` then `D32_FLOAT_S8_UINT` (SDL guarantees one), and `GL_Init_NullGL` sets `have_stencil = RG_DepthHasStencil ()`; a device with neither falls back to a depth-only format and runs without the stencil, as GL does on a visual with no stencil bits. The stencil seam (`R_SetStencilTest` / `Func` / `Op`) was already in `gl_pipeline.c` with bare GL arms; its `-gpu` arm is the shadow, which `rg_pipeline.c` keys (func, ops, compare mask, one op state for both faces, write mask `glStencilMask`'s default) only on a format with stencil, refusing a func or op with no SDL_GPU value through `RG_NoteUnhandled`. The reference is dynamic state, clamped to 8 bits and set per draw with `SDL_SetGPUStencilReference`, so it costs no pipelines: demo1 builds 9, egypt 14. The stencil clear is P2b's clear seam. Checks: `tools/sweep-skybox-gpu.steps` (the Shadow of Tremor mod's egypt, cameras that look through sky brushes at open space found by ray-marching the BSP; 51.2-58.7 dB against GL, and the P2b binary fails one camera at 38.6 dB, where a wall behind the sky shows), an `r_shadows` camera in `tools/sweep-world-gpu.steps`, `tools/test_rg_sources.py`. Classic sky gains nothing visible: its chain writes sky depth itself, and no retail map but village5 has open space behind a sky brush (village5's views are pixel-identical with and without the stencil). Known gaps: the Tesla P4 logs 10 `vkCmdCopyImage` validation errors on egypt, from a 3D texture's mips, the same with the P2b binary | branch `bobberb/sdlgpu-p2c-e34a6394` |
| P2d | Vertex and index buffers, vertex arrays and indexed draws through SDL_GPU, so the world, brush models (instanced too: `R_DrawBrushInstanced` reads uniform blocks, no storage) and the sky pre-pass draw from their static VBOs under `-gpu`, as on GL, instead of rebuilding them through `GL_ImmEnd` every frame. Seams with GL's signatures, each GL arm the one bare call: `gl_buffer.c`'s `GL_BindBuffer`, `R_GenBuffers`, `R_DeleteBuffers`, `R_BufferData`, `R_BufferSubData`, `R_GenVertexArrays`, `R_DeleteVertexArrays`, `R_BindVertexArray`, `R_Enable`/`DisableVertexAttribArray`, `R_VertexAttribPointer`, `R_VertexAttribIPointer`, `R_VertexAttrib4f`, and `gl_vbo.c`'s `R_DrawElements` / `R_DrawElementsInstanced`; `gl_rsurf.c` and `gl_rmain.c` make no raw buffer, vertex or draw call. `rg_buffer.c` shadows GL's rules (the array buffer is context state a pointer captures, the element buffer the VAO's, current attribute values context state), maps tagged names to `SDL_GPUBuffer`s (vertex + index usage), copies each upload out at once and queues it for the segment's copy pass (a respecify cycled, a sub-update not), and ends the open segment first when one of its recorded draws reads the buffer. An indexed draw's layout is interned (`rg_pipeline.c`, the immediate `immvert_t` is layout 0) and keys the pipeline; disabled standard inputs (`ATTR_POSITION`..`ATTR_COLOR`, the world's white `ATTR_COLOR`) are an instance-rate FLOAT4 slot in the stream, one record per segment while the values hold (Metal has no zero stride). Index width from GL's type, first index the byte offset divided by it. demo1, last camera: P2c 70 draws, 632,400 stream bytes per frame; P2d 278 draws (240 from buffers), 478,080 bytes. egypt: 290 draws, 4,136,208 bytes against 3,588 (3,530) and 448,192. Draws rise because the immediate fallback built a texture chain into one batch and the VBO path draws one contiguous index run at a time, which is GL's draw for draw. Checks: `tools/test_buffer_routing.py` (no raw buffer, vertex-array or draw call outside the seams, bar a counted ledger of what later phases route), `tools/test_rg_buffer_shadow.py` (compiles `rg_buffer.c` against a fake SDL_GPU), `tools/test_rg_sources.py` (each seam's arms and the backend lines the sweeps rest on), both sweeps: GL vs `-gpu` 48.2-52.7 dB on demo1, 52.0-56.9 on egypt, and P2d `-gpu` frames pixel-identical to P2c's on demo1's static cameras, 85.9-94.2 dB on egypt's (the VBO's fans come from the BSP edges, the fallback's from the polys). Known gaps: the alias meshes (`gl_mesh.c`) and every compute, storage, post-process and shadow buffer stay raw (the ledger names each with its phase) | branch `bobberb/sdlgpu-p2d-e34a6394` |

## Decisions

The first five are provisional: the code is built on them, and any of them can still be reversed.

1. **Handles:** GL-shaped handles and enums are the backend's vocabulary.
2. **Null-GL safety net:** a generated null-GL stub table, which feeds `gpu_unrouted`.
3. **Software Vulkan:** `gpu_allow_software` is test-only, default 0.
5. **Switching:** `-gpu` is restart-only.
9. **Y orientation:** handled by negating `gl_Position.y` under `VULKAN` (`--invert-y` does nothing for GLSL).

Settled:

6. **Shader packaging, decided 2026-11-19:**
   - Builds with `USE_SDL_GPU` embed the SPIR-V in the binary, SPIR-V only for now.
   - `glslangValidator` becomes a build input only where `USE_SDL_GPU` is on.
   - DXIL and Metal come at P7.
   - Lookup order: `-gpushaders <dir>` (the developer override, no relink), then the embedded table. P1c's implicit `<exe>/spirv`, `<basedir>/spirv` and build-tree search is gone: ahead of the table a stale directory could replace the blobs the binary was built with, and behind it nothing could reach them.
   - `.#h2ded`, `.#hwsv` and `.#h2ded-win64` configure with `-DUSE_SDL_GPU=OFF`, so their configure needs no `glslang`.

Still open: 4 (first-release scope), 7 (pipeline pre-warm), 8 (D3D12 Tier-1 hardware).

## Next

1. ~~Embed the SPIR-V (decision 6), land P1c.~~ Done, see the table above.
2. ~~**P2a:** sampler units, typed placeholders, storage-buffer stand-in, DlShadow mirror.~~ Done, see the table above.
3. **P2:** the opaque world, brush and alias models, and the sky; P2b, P2c and P2d are done (see the table above), one slice remains:

   | Slice | What | Gate |
   |---|---|---|
   | P2e | Skeletal and instanced alias storage buffers (optional, else P5); the alias meshes' VAOs through P2d's seams (`gl_mesh.c`'s rows in `tools/test_buffer_routing.py`'s ledger) | the ledger rows gone |

4. **Later phases:**

   | Phase | What |
   |---|---|
   | P3 | Translucents, particles, warp, fog |
   | P4 | Post-process |
   | P5 | Compute and storage |
   | P6 | Shadows, OIT, MSAA |
   | P7 | Readbacks, #451 timing, the #103 menu switch, D3D12/MSL, Windows |

## Verification habits that held

- **Pixel identity:** GL without `-gpu` must stay pixel-identical. Gates: `tools/sweep-2d.steps`, `gl_hiz_stats` via `tools/sweep-worldcull.steps`, and `tools/dlight-leak-check.sh`.
- **Comparing `-gpu` frames:** GL/llvmpipe against SDL_GPU/lavapipe, with a fuzz or PSNR threshold (`PSNR_MIN=<dB> tools/shot-diff.sh`), taken from three runs of each side. Turn off on both sides whatever `-gpu` does not draw yet, so the frames come from the same draw paths. Run the Vulkan validation layers, and require 0 errors.
- **Every phase checks:**
  - `.#win64` (mingw's `gl.h` is GL 1.1, so new enums need `glheader.h` fallbacks);
  - the WEBSOFT wasm link;
  - `node --test web/test/*.test.js` (some tests pin C source text);
  - the shaders gate, with `shaders_cross` run as `env -u LD_LIBRARY_PATH`.
