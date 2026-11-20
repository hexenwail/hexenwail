# tools/

Developer analysis tooling. `tools/` is not an input to the engine's derivation
in `flake.nix`, so editing anything here does not rebuild the engine.

**Two exceptions:** `tools/qcdis.py` and `tools/check_progs_fields.py` are
build inputs to `.#gamecode`, which runs them in its check phase. Editing
either one rebuilds `.#gamecode`. A new build-time tool must be added to the
`gamecodeSrc` filter in `flake.nix`, or belongs in `scripts/` instead.

Some scripts are CI gates, run from `.github/workflows/lint.yml`:
`menu_soft_parity.py`, `test_software_model_flags.py`,
`test_multiplayer_join.py` and `ironwail_scorecard.py --check`.
`test_hcc_tail_slack.py` is a gate in `ci.yml`'s `cross` job instead,
because it needs the `hcc` that job builds.

Run the scripts directly. The Python ones need only stock `python3`, except
`pak_extract.py`, which needs Pillow.

| script | what it does |
| --- | --- |
| `qcdis.py` | disassemble `progs.dat` bytecode |
| `edict_pick.py` | pull entities of interest out of an `edicts` console dump |
| `headless-cfg.sh` | generate a scripted-run config that emits those dumps |
| `headless-drive.sh` | drive the engine's menus under Xvfb with real key events |
| `serve.sh` | drive the dedicated server (`h2ded`) on stdin, with no X at all |
| `hw-drive.sh` | run `hwsv` and a client connected to it, and screenshot the HexenWorld session |
| `web-drive.mjs` | drive the web player page in headless Chromium (boot, start prompt, running, drawer, files menu; desktop or emulated phone) and screenshot each phase |
| `hw-master-check.sh` | `hwmaster` plus two `hwsv`: both must appear in `hwmquery`'s list, and one that quits over `hwrcon` must leave it (heartbeat and shutdown, #38); retail data, local only |
| `bsp_dlight_leak.py` | the oracle for dynamic light through walls (#97): reads the BSPs out of the retail paks, puts a light at every `light_torch*` / `light_flame*` (8 units up, radius `--radius`, default 200), traces hull 0 to 2n+1 samples on every face in reach, and reports per map the faces that leak (lit by vanilla, every sample blocked) and how many of them PVS, the one-sided test (`--eps`) or either removes; `--scenes N` lists candidate repro scenes, `--out` writes the JSON report (`--faces` adds every reachable face per light, to compare with the engine's `r_dlightvis_dump ... faces`), nothing else is written |
| `dlight-leak-check.sh` | **local, needs the retail paks**: live check that a dynamic light does not light the far side of a wall (#97) -- loads `demo2` through an external `.ent` twice (control, then with a torch given a `targetname` so it keeps its dlight), screenshots five scenes offscreen and compares a box's luminance; scene 2 (front-facing but occluded) and the alias scene (a barrel the `.ent` adds behind a wall) pass on the CPU light path since P2, FAIL on `r_lightclusters 1` under `r_dlight_occlusion 1`, and pass there under 2, the cube shadow maps (P3/P4); `SHADOWS=1` runs that configuration and fails a dark scene as proving nothing unless `r_shadowinfo` shows the torch's map in use, and adds the `cast` scene (#97 P6): a free-standing torch the `.ent` adds and a barrel between it and a wall, whose shadow must darken the wall under `r_shadow_casters 2` and leave it as the barrel-less view under 1 |
| `pimp-nodlight-check.sh` | **local, needs Storm over Thyrion in `sot/`**: live end-to-end check that `misc_modelpimp` spawnflag 64 turns a model's dynamic lights off -- loads a SoT map twice through an external `.ent` (control, then with a flag-64 pimp added) and compares `r_dlightinfo` / `mcache` (#140) |
| `pak_extract.py` | extract textures/skins/GFX from a PAK to PNG |
| `test_demobar_copies.py` | fail if the software renderer's `SCR_DrawDemoBar` (`screen.c`) drifts from the GL one (`gl_screen.c`) beyond the two allowed lines (#123) |
| `pak2pk3.py` | convert a retail v1.11 `data1` or `portals`, the Nov. 1997 demo's `data1`, or HexenWorld 0.14 - 0.15's `hw` from `pakN.pak` to `000-pakN.pk3` (DEFLATE, same members, order, duplicates and bytes; verified before it is renamed into place). Refuses content the engine would not identify as a pk3, such as the OEM release and pre-1.11 installs, and the Aug. 1997 (0.42) demo, which the engine identifies but does not support (#130). `--loose` packs the loose game files into `zz-loose.pk3`. Retail data1: paks 98 MB to 46 MB (#129) |
| `upscale-pak.sh` | extract and AI-upscale PAK textures to TGA overrides |
| `artifact-save-check.sh` | check on a live engine that the selected artifact survives save, relaunch and load, including old-format saves and a pre-r11 engine (#159); needs retail data and a GPU (offscreen) |
| `progs_crc.py` | print the two CRCs `PR_ClassifyGamecode()` identifies a `progs.dat` by, and the Raven release when it is one |
| `test_progs_release_ident.py` | compile `pr_raven_images[]` and its lookup out of `pr_edict.c` and check every Raven release is named, only on both CRCs, in step with `progs_crc.py` (#160) |
| `test_wateralpha_reupload.py` | compile the real `GL_WaterAlphaByte`, the `GL_LoadTextureEx` cache test and `Mod_WaterAlphaChanged`, and check a change to `r_wateralpha` re-uploads exactly the loaded EF_TRANSPARENT alias skins (#124) |
| `test_menu_mouse_grab.py` | compile `IN_ReactivateMouse` and check that focus, unpause and `_enable_mouse` take the mouse back through it, and that no menu draw function grabs it, so menu hover keeps working (#91) |
| `test_demo_mods_gate.py` | check that the `game` command (C and Rust) refuses mods without the full game, that the main menu dims MODS and says why, and that the GL glyph fade can fade (#171) |
| `test_menu_row_notes.py` | check that the Options submenus' row notes fit `M_ROWNOTE_MAX` columns and name the cvar their row actually sets (#98) |
| `test_spawn_walk.py` | check that `SV_Physics` leaves an edict created during its walk for the next frame and that every `ED_Alloc` path stamps it (#138) |
| `sdl_gpu_feasibility.py` | check an `SDL_gpu.h` (default: the vendored Windows SDL3) for every capability the SDL_GPU backend plan rests on, report any of #77's recorded ceiling items (timer queries, bindless, mesh shaders, ray tracing, WebGPU) that have appeared, and with two headers diff the declared functions; run it on every SDL bump (#77, #452) |
| `test_sdl_gpu_feasibility.py` | run `sdl_gpu_feasibility.py` on the vendored SDL3 and check that it catches a removed entry point and ignores comment-only mentions (#452) |
| `test_lcparams_block.py` | check that `engine/shaders/lightcluster_comp.glsl`'s LcParams block and `lc_params_t` in `gl_lightcluster.c` agree member for member (std140 offsets, 160 bytes), that the old loose uniform names are only aliases into it, and that the C side binds it at point 5 with no loose uniform queries or uploads left (#450) |
| `test_bloomparams_block.py` | check the post-process blocks (BloomParams, OitParams, PpVert/PpParams) against their C mirrors and C-side writes; for bloom, that `engine/shaders/bloom.inc`'s BloomParams block and `bloom_params_t` in `gl_postprocess.c` agree, that every bloom stage includes it and the inline literals are gone, that binding 6 is registered, and that `PP_PushBloomParams` does not use the `GL_Upload` ring, which is a stub on the ES tier (#450) |
| `test_no_version_in_c.py` | check that no C string literal under `engine/h2shared` or `engine/hexen2` contains `#version`: every literal after comments are blanked (plain, prefixed or raw, escapes decoded), adjacent literals joined as the compiler joins them, and a `#version` in code that a stringizing macro would turn into one; the tier preambles and the WEBSOFT canvas blit live in `engine/shaders/glonly` (#450) |
| `test_embed_include_bound.py` | check that `EmbedShaders.cmake` and `scripts/webgl-engine-shader-smoke.mjs` allow the same number of `#include` passes, and that no `engine/shaders` stage needs more distinct includes than that (#450) |
| `test_sdlgpu_bindings.py` | **needs the `shaders_spirv` output**, so it runs in `shaders-spirv.yml`, not `lint.yml`: parse every `<build>/engine/generated/spirv/*.vk.spv` (stdlib only, no spirv-cross) and check its resources against SDL_GPU's SPIR-V layout -- compute set 0 sampled textures, read-only storage textures, read-only storage buffers; set 1 read-write storage textures, read-write storage buffers; set 2 uniform buffers; vertex sets 0/1 and fragment sets 2/3 the same without the read-write classes -- dense from 0 and in that order within each set; also fails separate samplers, resource arrays, push constants and writable graphics storage, and lists compute images that need `SIMULTANEOUS_READ_WRITE`. `--sources <build>/engine/generated/vk` fails a stage with no `.vk.spv` (#340); and holds every graphics stage's read-only storage blocks (their fixed part, from the SPIR-V's `Offset` / `ArrayStride` / `MatrixStride`) to the zeroed stand-in the backend binds until P5, `RG_SSBO_STANDIN_BYTES` in `engine/h2shared/rg_internal.h` (#340 P2a) |
| `gen_program_reflection.py` | **needs the `shaders_spirv` output**: decode every `<build>/engine/generated/spirv/*.vk.spv` with `test_sdlgpu_bindings.py`'s parser and write `engine/h2shared/gl_progid_refl.h`, one row per offline blob with the counts `SDL_CreateGPUShader` / `SDL_CreateGPUComputePipeline` need (samplers, read-only and read-write storage textures and buffers, uniform buffers) and a compute stage's `LocalSize` threadcount; checked in so the GLES, web and server builds need no glslang (#340 decision 6). `--check` writes nothing and fails on a stale header, and `--cross <build>/engine/generated/cross` also fails a row that disagrees with shadercross's reflection JSON of the same blob; both run in `shaders-spirv.yml` (#340 P1b) |
| `test_shader_variants.py` | check that `engine/shaders/variants.txt` enumerates every stage and `#define` set the desktop GL client can build at run time, so the offline SPIR-V gate (and with it SDL_GPU, which cannot compile GLSL at run time) has a blob for each: reads every row of `gl_shader.c`'s program table (`rprog_defs`, one per `rprog_id_t`, #340 P1b) and evaluates each stage's header chunk as the compiler would (literals and macros concatenated, `GLSL_STR` stringized, integer `+ - *` folded), failing on a run-time value; each stage and define set must be the plain stage, its OIT twin or a `variant` line, and each `const` (froxel-grid sizes, instanced-alias record layout, LUT size) must match both the C and every shader `#ifndef` default; fails a `#define` literal in the C that no row consumes, a stage symbol named anywhere but a row's body, `glShaderSource` named anywhere but a call in `RP_CompileStage` (or `RP_CompileStage` anywhere but a call in `R_Program_Create`, a macro, a function pointer, an unparsed file), a header line other than `#version`/`precision`/`#define`, an auto OIT twin or manifest line no program requests, and any `--invert-y` in `CompileSpirv.cmake` (glslang implements it for HLSL only; on GLSL it is a silent no-op). Runs in `lint.yml`; `--vk-dir <build>/engine/generated/vk` (in `shaders-spirv.yml`) also checks EmbedShaders wrote exactly those copies, each with its manifest `#define`s (#340) |
| `test_vk_flip_y.py` | check SDL_GPU's Y flip (#340): `engine/shaders/layout.inc` defines `VK_FLIP_Y()` as `gl_Position.y = -gl_Position.y;` in the `VULKAN` arm and as nothing in the `GL_SPIRV` and plain-GL arms (each arm evaluated as the preprocessor picks it); every `engine/shaders/*_vert.glsl` reaches `layout.inc`, ends `main()` with a bare `VK_FLIP_Y()` (no `;`, so GL sees no token) outside any `#if`, and puts one before every `return` in `main()`; the macro appears nowhere else (`glonly/` is GL-only text and out of scope). Runs in `lint.yml`; `--spirv <build>/engine/generated/spirv [--sources <build>/engine/generated/vk]` (in `shaders-spirv.yml`) checks every `*.vert.vk.spv` stores `OpFNegate` of `gl_Position.y` back to it as the last `gl_Position` store before every `OpReturn`, that no `*.vert.gl.spv` does, and that every staged `.vert` has both blobs |
| `test_rg_sources.py` | check that the SDL_GPU backend (`engine/h2shared/rg_*.c`, #340) is built into the desktop GL client alone: `engine/CMakeLists.txt` names `rg_*.c` only inside `if(USE_SDL_GPU)`, into `RG_SOURCES`, which only the client's `ALL_SOURCES` carries (never `SERVER_SOURCES` / `HWSV_SOURCES`); `USE_SDL_GPU` is forced off for `USE_GLES`, `EMSCRIPTEN`, any build without `GL_DLSYM` and SDL older than 3.4 before anything tests it, and reaches the compiler only through `CLIENT_DEFINITIONS`, never `add_definitions`; every `RG_*` / `rg_*` identifier, `R_GPU()` and `#include "rg_device.h"` in any other engine source sits under `#ifdef USE_SDL_GPU` (comments excluded), so the GLES, WEBSOFT and dedicated builds never name a backend symbol; each `rg_*` file `#error`s without `USE_SDL_GPU`; and no `SDL_GL_GetProcAddress` load can run under `-gpu` (every one outside `gl_vidsdl.c`'s `GL_Init_Functions` / `GL_LoadFunctionPointers` / `GL_Init` sits in an `if (!R_GPU())` block, `GL_Init` returns into `GL_Init_NullGL` before its first load, and the two loaders are called from `GL_Init` alone), since under `-gpu` there is no GL library and a load writes NULL over a stub; and (#340 decision 6) `glslangValidator` is required only under `USE_SDL_GPU`, whose client embeds the SPIR-V through `EmbedSpirv.cmake` (never in `ALL`, never outside that arm), the flake gives `glslang` to `.#nixos` and `.#win64` alone while the server builds configure `-DUSE_SDL_GPU=OFF`, and `rg_shader.c` reads `-gpushaders <dir>` first, then the embedded table, and no other directory; and (#340 P2b, the 3D world under `-gpu`) `GL_Init_NullGL` turns `gl_clipcontrol_able` on with the `GL_GEQUAL` test and a 0.0 clear depth, `rg_pipeline.c` clips depth exactly when the draw tests it and keeps the clockwise front face, `rg_frame.c` clears depth to what `RG_Clear` was handed; `glClear`, `glClearColor`, `glClearDepth` and `glClearStencil` appear only in `gl_pipeline.c` (bar `gl_vidsdl.c`'s GL-only FBO self-test), where each GL arm is the bare call; `R_BuildWorldCull` returns under `R_GPU()` before creating anything; and alias instancing and `GL_DrawAliasSkeletal` need `gl_renderer_caps.shader_storage`, which `GL_DrawAliasSkeletal` tests with no `-gpu` note left behind; and (#340 P2c, stencil under `-gpu`) `GL_Init_NullGL` sets `have_stencil = RG_DepthHasStencil ()`, the scene's depth target tries `D24_UNORM_S8_UINT` then `D32_FLOAT_S8_UINT` first, the stencil seam's GL arms are the bare `glEnable` / `glDisable (GL_STENCIL_TEST)`, `glStencilFunc (func, ref, mask)` and `glStencilOp (sfail, dpfail, dppass)` with no `glStencilMask` or `*Separate` in `gl_pipeline.c`, `rg_pipeline.c` maps each compare func and stencil op to its own SDL_GPU value and keys them (never the reference) only on a format with stencil, one op state for both faces, and `rg_frame.c` sets each draw's clamped reference with `SDL_SetGPUStencilReference` and turns a stencil clear into the pass's stencil load op; and (#340 P2d, buffers under `-gpu`) every buffer, vertex-array and indexed-draw seam (`GL_BindBuffer`, `R_GenBuffers`, `R_DeleteBuffers`, `R_BufferData`, `R_BufferSubData`, `R_GenVertexArrays`, `R_DeleteVertexArrays`, `R_BindVertexArray`, `R_Enable`/`DisableVertexAttribArray`, `R_VertexAttribPointer`, `R_VertexAttribIPointer`, `R_VertexAttrib4f` in `gl_buffer.c`, `R_DrawElements`, `R_DrawElementsInstanced` in `gl_vbo.c`) is exactly its `-gpu` arm and then the one bare GL call with the seam's own arguments, `R_BuildWorldVBO` and `R_BuildSkyStencilVBO` build through them with no `R_GPU()` test and no raw GL, `rg_buffer.c` queues a respecify with cycling on and a sub-update without, ends a segment that reads the buffer first, and feeds the disabled `ATTR_POSITION`..`ATTR_COLOR` inputs their current values in an instance-rate FLOAT4 slot, `rg_frame.c` takes the index width from GL's type and divides the byte offset by it, records and replays both, and `rg_pipeline.c` keys the pipeline on the interned vertex layout and builds its vertex input from it; and (#340 P2e, storage under `-gpu`) `GL_BindBufferBase`, `GL_BindBufferRange`, `GL_BindBuffersRange` and `GL_Upload` have a `-gpu` arm ahead of every GL call, `rg_shader.c`'s block-name table (`RG_StoragePoint`) is exactly the GL binding point every graphics stage in `engine/shaders` declares for each storage block (`VERT_SSBO` / `FRAG_SSBO`'s `gl`, or a plain `layout(std430, binding = N)`), one point per name, no entry undeclared, and `RG_CheckStage` numbers a block's slot past the stage's textures; `rg_buffer.c` makes every buffer with graphics storage-read usage, refuses a storage range at a non-zero offset and hands `GL_Upload`'s arm back at offset 0 queued with cycling on; `rg_frame.c` binds each storage slot to `RG_Buf_Storage`'s buffer or the stand-in and replays the draw's own slots; `GL_Init_NullGL` turns on `shader_storage` and `skeletal_animation` and no other capability. The frames themselves are `sweep-world-gpu.steps`', `sweep-skybox-gpu.steps`' and `sweep-alias-gpu.steps`' |
| `test_rg_sampler_units.py` | **needs cc**: check the sampler-unit seam (#340 P2a). `R_ProgramSamplerLoc` / `R_ProgramSamplerUnit` (`gl_shader.c`) are the `glGetUniformLocation` / `glUniform1i` pair call for call in GL, and `RG_ProgramSampler` / `RG_ProgramSamplerUnit` under `-gpu`; no other engine caller of either GL entry point but `gl_postprocess.c` and `gl_ssao.c` (P4 / P5); each call names the sampler its location was looked up for, with the unit that sampler is bound at (`LIGHT_GRID_TMU`, `LMSTYLE_TMU + k`, `MBOIT_TMU_*`, `DLSHADOW_TMU`, `u_textureN` N); every graphics program's samplers, read from `engine/shaders` through the GL preprocessor with its `rprog_defs` row's defines and checked against `gl_progid_refl.h`, each get a unit from the function that sets the program up, and no two share one; and the backend lifted and run on SPIR-V assembled in the script: `RG_Reflect`'s dimensionality, sampled class, shadow and multisample flags and unmapped units, `RG_ProgramSamplerUnit` across both stages, `RG_Res_ForDraw`'s typed placeholders, the draw-time unmapped note and every storage-buffer slot bound at replay (the stand-in where its GL point holds nothing, #340 P2e) |
| `test_embed_spirv.py` | **needs cmake and cc**, so it runs under `nix develop` in `ci.yml`: run `engine/cmake/EmbedSpirv.cmake` on fixture blobs and check the `spirv_gen.c` it writes has every `gl_progid_refl.h` row, sorted, word-exact for little- and big-endian files, compiles with `rg_spirv.h` under `-Werror`, is byte-identical across runs with no build path in it, and that the script refuses a row with no blob, a blob with no row, a partial word and a missing magic number (#340 decision 6).  `--generated FILE --spirv-dir DIR` checks a real build's table against its `.vk.spv` (`shaders-spirv.yml`) |
| `gen_rg_null.py` | generate `engine/h2shared/rg_null.c`, SDL_GPU mode's null GL table (#340 P0), from every `GL_FUNCTION` / `GL_FUNCTION_OPT` line of `gl_func.h`'s desktop loader table: a counting no-op per entry point with gl_func.h's exact signature (assigned to its `_fp`, so a drifted type is a compile error), conditional entries kept under their `#if`, and defined results wherever the engine reads one (fake nonzero names from `glGen*` / `glCreate*`, `glGet*` limits and zeroed outputs, compile/link success, `GL_FRAMEBUFFER_COMPLETE`, zeroed `glReadPixels` output at the tightly packed size); also `RG_NullGL_Verify`, which `GL_Init_NullGL` runs last and which names any entry a later load overwrote; fails generation on a `glGet*` / `glGen*` / non-void entry with no hand-written body, or a body for an entry gl_func.h no longer has. `gpu_unrouted` prints the counts. `--check` (in `lint.yml`) fails a stale committed copy |
| `test_alias_vert_block.py` | check that `engine/shaders/uniforms.inc`'s VertParams and `r_vertparams_t` agree member for member (sizes and int/float kinds, 368 bytes), that `salias_vert.glsl` reads only block members with its dlight varyings desktop-only, that all four alias builds compile the file, that `GL_ImmPushBlocks` feeds `u_alias_model` / `u_alias_dlight`, and that MSL targets 2.1 (#450 alias slice S1); since slice S2 also that all three vertex stages' varyings match `salias_frag.glsl`'s inputs, and since S3 that the file-backed ones match its Vulkan locations too (all three since S4) |
| `test_alias_frag_block.py` | check that `engine/shaders/alias.inc`'s AliasFrag and `r_aliasfrag_t` agree member for member (144 bytes, int/float kinds) at a binding no other block uses, uploaded without the `GL_Upload` ring; that `salias_frag.glsl` has no loose non-opaque uniform and keeps the clustered dlight desktop-only; that the helpers it shares with the world stages (`caustics`, `turb_uv`, `span_uv`, `material`, `lightgrid` .inc) are one copy both families include, with no `GLSL_*` macro copy left (#450 W2); that all ten alias-family programs compile the file; and that every draw path using them (`GL_ImmEnd`, skeletal, instanced and its fullbright pass, `R_SetSpanCell`, the light cluster) writes the block members it reads before drawing (#450 alias slice S2) |
| `test_alias_inst_vert_block.py` | check that `alias.inc`'s AliasVert and `r_aliasvert_t`, and VertParams and `r_vertparams_t`, agree member for member by name (96 and 368 bytes; 64 until slice S4 added the skeletal lighting) and AliasVert sits at a binding no other block uses; that `salias_inst_vert.glsl` has no loose non-opaque uniform, puts its four storage buffers on `VERT_SSBO`, aliases the Vulkan vertex/instance builtins and keeps its ShadedotRow constants equal to `gl_shader.h`'s; that both instanced programs compile the file and bind AliasVert; and that `R_DrawAliasInstanced` writes every VertParams member the stage reads and sets and flushes the instance base before each batch's draw in both passes (#450 alias slice S3); that both passes draw MD3 batches from `ssbo_pose_md3` at binding 3 (GitHub #485); that the glow pass marks its records `ALIAS_INST_FULLBRIGHT_BIT` and the shader draws them unshaded (#491); and that `R_CollectAliasInstance` admits an .md3 on its own pose buffer and packs the exact yaw its MD3 arm shades by (#492) |
| `test_skeletal_vert_block.py` | check that `alias.inc`'s AliasVert and `r_aliasvert_t`, and VertParams and `r_vertparams_t`, agree member for member by name (96 and 368 bytes); that every name `sskeletal_vert.glsl` reads aliases the lane its setter writes and that each lane has one setter; that the file has no `#version` or loose non-opaque uniform, puts its bones on `VERT_SSBO(4, 0)`, and gives every input the slot, width and integer-ness of the IQM VAO in `gl_mesh.c`; that the literal, `GL_SpliceDefines` and the `GLSL_ALIAS_DLIGHT_VS_*` splices are gone and all four skeletal programs compile the file desktop-only and bind AliasVert; and that `GL_DrawAliasSkeletal` makes no loose write and writes every member the stage reads, after the pose clamp, before one unconditional flush and its draw (#450 alias slice S4) |
| `test_alias_no_loose.py` | check that no alias- or world-family program regains loose uniform plumbing: derives the names the four alias stages and three world stages read through `uniforms.inc` / `alias.inc` / `world.inc` block macros, minus every name a live shader (any `engine/shaders` file or `gl_shader.c` literal, both tiers) still declares loose, pinned by a floor of all thirty-five (u_mvp and u_modelview joined when `sworld_vert` moved onto VertParams, #450 W1, and the seventeen world fragment names when `sworld_frag` / `sworld_opaque_frag` moved onto FragParams + WorldFrag, W2), and that no live shader declares any non-opaque loose uniform (the OIT preamble's two went onto OitFrag in #450 W3, joining the floor); fails on any C identifier spelling one (struct field, local, stringized macro argument) and any string literal, adjacent literals joined, equal to one; that the stages (includes expanded) and the header chunks they compile with (resolved from the compile calls through macros, `q_snprintf` buffers and parameters) declare no loose non-opaque uniform, with multi-declarator, initialised and macro-built forms counted; that nothing `glUniform*`s a non-sampler field through an alias program, a local assigned one or `R_AliasProgram()`; that `GL_InitProgramUniforms` looks every field up under its own name; that every `glprogram_t` location names a live loose uniform or a listed non-alias leftover; and that every `GL_ImmEnd` cache sits under a live location's `if` (#450 alias slice S5) |
| `test_world_vert_block.py` | check that VertParams and `r_vertparams_t` agree member for member by name (368 bytes); that `engine/shaders/sworld_vert.glsl` reads exactly `u_mvp` / `u_modelview`, aliased to the members `R_SetMVP` / `R_SetModelView` write, has no `#version` or loose uniform, keeps `invariant gl_Position` and a pinned `main`, puts every input on its `ATTR_*` slot at the width the imm and world VAOs feed, and matches `sworld_frag.glsl`'s and `sworld_opaque_frag.glsl`'s inputs by name, type, interpolation, precision and location; that the three world programs compile it with `GLSL_WORLD_FRAG_HEADER` fragment files and no `u_mvp` / `u_modelview` location, lookup or C identifier is left; and that every world draw path (`GL_ImmEnd`, `DrawTextureChains_FlushBatch`, `DrawTextureChains_DrawDeferred`, `R_DrawBrushModel`, `R_DispatchBrushInstancedPass`, `R_DrawWorldCulled`) writes both matrices and flushes unconditionally before its draws with no block writer in between, and that any other function naming a world program and drawing fails it; plus a flow pass over every function the h2 client builds (macros expanded, calls followed through parameters, locals, return values and pointers) that fails any function drawing while a world program may be bound unless it is one of those paths, any block writer reached through a chain of calls between a path's flush and its draw, and any path to `DrawTextureChains_FlushBatch` without `world_state_set` (which FlushBatch now also refuses at run time), and fails loudly on anything it cannot parse or follow (#450 world slice W1); plus a member pass that follows every block slot a world stage reads (VertParams' matrices, FragParams' and WorldFrag's, derived from the shader files and the setters' bodies) through each of those paths, and fails any draw a path reaches without its own `R_SetMVP` / `R_SetModelView` / `R_SetWorldFrag` write of that slot, flushed, with no other writer since, on every path through the code (#450 W2) |
| `test_world_frag_block.py` | check that `engine/shaders/world.inc`'s WorldFrag and `r_worldfrag_t` agree member for member by name (160 bytes) at a binding of their own, uploaded, invalidated, seeded and deleted like the other blocks; that each old world uniform name aliases its WorldFrag or FragParams member; that `sworld_frag.glsl` and `sworld_opaque_frag.glsl` declare the same blocks, samplers and varyings, no loose non-opaque uniform, `FRAG_TEX` 0..n-1, `sworld_frag`'s colour output the `#else` arm of its `oit.inc` include (#450 W3), early-Z in the opaque twin only and only outside GL_ES, and their preprocessed function bodies pinned by hash on both tiers; that the ES arm reads no desktop-only member; that `R_SetWorldFrag` writes every member from its arguments and the world frame record, that record's setters are called from where each value is decided, and each of the six world draw paths passes the values it uploaded by hand (#450 world slice W2), except the overbright: every `R_SetWorldFrag` call passes `R_WorldOverbright ()`, defined once in `gl_rsurf.c` as 2 with `gl_overbright` and 1 without it or under `r_fullbright` (#489) |
| `test_oit_frag_block.py` | check that the OIT twins' output stage is `engine/shaders/oit.inc`, taken by `salias_frag` / `sworld_frag` / `spart_frag` (and only them) as the `#ifdef OIT` arm whose `#else` is the colour output, with `OIT_TEX0` / `OIT_UBO` equal to the stage's own sampler and block counts and `lightgrid.inc` moving the light buffer past the four moment samplers; that OitFrag and `r_oitfrag_t` agree (one ivec4, 16 bytes) at a binding of their own, uploaded, invalidated, seeded and deleted like the other blocks; that `R_SetOITMode` is the block's only writer, writes every lane `oit.inc` reads, and is called at exactly the four pass transitions in `gl_postprocess.c` with the arguments `GL_SetOITMode` had; that every function picking an OIT program reaches `GL_ImmEnd` or `R_FlushUniforms` after it; and that `GL_InitOITProgramSplit` appends exactly `#define OIT 1`, refuses a truncated header or a twin without OitFrag, and builds the six twins desktop-only, with no `oit_preamble`, splice or `GL_SetOITMode` left (#450 W3) |
| `test_alias_fullbright.py` | check that `r_fullbright` flattens alias model light on every alias path (#490): `R_AliasFinishLight`, defined once in `gl_rmain.c`, sets `lightcolor` to 1.0 under `r_fullbright` (the unlit skin at 1x; no alias shader doubles its light, where Ironwail's 0.5 is doubled), clears `model_dlight_gpu` and returns before `R_AliasLightScale`'s scale; that both setups (`R_DrawAliasModel`, which MDL, MD3, skeletal and the viewmodel draw through, and `R_CollectAliasInstance` for `r_alias_gpu 1`) call it once, write no light after it, and feed their consumers only after it; that `GL_DrawAliasFrame`, `GL_DrawAliasFrameMD3`, `GL_DrawAliasSkeletal` and the InstanceData fill take every channel from `lightcolor` and are reached only from those setups; that `R_AliasLightScale` has no other caller; that every client function reading `lightcolor` is classified; and that `R_LightCluster_AliasActive` stays off under `r_fullbright` |
| `test_alias_fullbright_lerp.py` | check that the CPU alias glow (`r_alias_gpu 0`, and every MD5 model) draws at the base pass's pose blend, as the instanced glow does (#493): `R_AliasResolveLerp` does not read `model_fullbright_pass`; `R_DrawAliasModel` resolves once, unconditionally, into locals nothing else writes, before its first draw and with no return before the glow pass, and hands that pair and blend to both `R_SetupAliasFrame` calls (the second inside the `model_fullbright_pass` bracket); `R_SetupAliasFrame` has no other caller, never resolves or rewrites what it is handed, and passes it to `GL_DrawAliasSkeletal` and `GL_DrawAliasFrame`; that `model_fullbright_pass` is read only where `GL_DrawAliasFrame`, `GL_DrawAliasFrameMD3` and `GL_DrawAliasSkeletal` pick the unshaded colour, never a pose; and that `R_CollectAliasInstance` resolves once per entity |
| `test_delta_held_lerp.py` | check that a model the delta protocol is merely holding keeps its pose and move lerps (#494): in `CL_RelinkEntities` no `LERP_RESETANIM` / `LERP_RESETMOVE` write sits under a stale-`msgtime` condition (`msgtime != cl.mtime[0]`, or the else side of `==`) except the drop block, which also sets `forcelink`; the first branch after `oldorg` is exactly `if (ent->forcelink)` and resets both lerps; a held branch puts the entity at `msg_origins[0]` before the frac lerp and touches no lerp flag; the step-mover teleport test reads only a fresh pair. In `CL_ParseUpdate`, no `msgtime != cl.mtime[1]`, no msgtime-keyed reset or `forcelink`, `forcelink` is `(ent->model == NULL \|\| ent->forcelink)` taken after the `U_CLEAR_ENT` memset and before `msgtime` / `->model` are overwritten, and a model change resets the pose lerp; that `svc_clear_edicts` sets `forcelink` on what it removes and `svc_reference` resets the pose lerp when it restores a different `->model`; that `HWCL_CopyEntity` stamps `msgtime` unconditionally and sets `forcelink` only for `!was_on \|\| model_changed` (why HexenWorld needed no change); and that `R_AliasResolveLerp` keeps uhexen2-43f8's stale-threshold reset |
| `test_dlight_vis.py` | check that a dynamic light does not light what it cannot see on the CPU light path (#97 P2): compiles `r_dlightvis.c` whole against a stub `quakedef.h` and runs it on a synthetic BSP (two rooms, a wall with a doorway under a lintel, a slanted corner) -- the segment walk against `bsp_dlight_leak.py`'s own `Bsp.blocked` on 600+ segments, every face's 2n+1 samples against its `samples()`, the PVS / segment cascade, liquid and `SURF_PLANEBACK` lifts, what is not judged, the per-light cache and its slop, models, and `r_dlightvis_dump`; lifts and runs `R_MarkLights`' surface loop (GL, software) and the three alias dlight loops; pins `BeginFrame` / `NewMap` / `Init`, both CMake lists and the view model left alone |
| `test_brush_dlight_gates.py` | check that `R_DrawBrushModel` (`gl_rsurf.c`) and its instanced twin `R_CollectBrushInstances` (`gl_rmain.c`) guard their dlight mark loops with the same pure `&&` gate: `firstmodelsurface != 0`, `!gl_flashblend.integer` (dlights drawn as blobs) and `!R_LightCluster_ShadesWorld()` (the GPU path lights them, uhexen2-26bm) |
| `test_dlight_shared_submodel.py` | check that entities sharing a brush submodel are each lit by their own light on the CPU light path (#496): compiles `r_dlightmark.c` whole, twice (GL and software `SURF_*`), with each tier's `R_MarkLights` surface loop and `R_AddDynamicLights` prologue lifted and `R_DlightModelOrigin`, and runs two copies of one face apart, stacked (each light behind the other copy), overlapping (one light within reach of both: measured in the nearest copy) and interleaved, in both marking orders: every light lit, radius left and foot in its own copy's model space, the records exactly the (entity, light) pairs that marked, none for one entity; the pool restarting per frame and an overflow misplacing but never dropping (the one-sided test skipped); pins the reset, the call order, the gate and both CMake lists |
| `test_shadow_cube_faces.py` | check the dynamic-light cube shadow maps' faces (#97 P3): `gl_shadow.c`'s `sh_faceaxes` against GL's cube-map major-axis table, every face drawing each direction it owns into the texel the sampler reads, clip z inside both clip conventions, and the winding -- a triangle facing the light projected through each face and through a model of `R_SetupGL`'s own view and frustum, so `SH_CULL_FACE` (`GL_BACK`) is shown to keep exactly the faces turned away from the light; pins the matrix layout, the layer-face order and the light-to-point direction on both sides |
| `test_dlshadow_block.py` | check the shadow maps' two blocks (#97 P3/P4): `ShadowPass` and `DlShadow` against `r_shadowpass_t` / `r_dlshadow_t` (members, std140 offsets, sizes, `SHADOW_MAX_MAPS`), their binding points unique, `GL_Upload` / `glBufferData` and the binds, `R_Shadow_InitProgram` at all three program-link paths; runs the mask / 4-bit layer packing through `DlShadowFactor`'s decode on random light sets; pins the three stages' sampler, slot and include order, the scaled light term after the radius rejects, an ES arm that names none of it, and `R_SetupGL`'s call outside the mirror pass between the `RPROF_SHADOW` timestamps; and the alias casters (#97 P6): `sshadow_alias_vert`'s buffers, attribute slots and pose blend against `salias_inst_vert` / `sskeletal_vert`, the push along the light ray and its reset for the world's pass, the program pinned and failing soft, `R_AliasShadowCaster` posing a copy of the entity, `SH_AliasEligible`'s viewmodel and translucency refusals, the hash and the collection asking `SH_AliasCasts` with the light's key, and `SH_AliasCasts` lifted, compiled and run (owner key, light inside the model's box, reach) |
| `test_shadow_presets.py` | check the Display presets' shadow-map tiers (#97 P5): `PRESET_SHADOWS` per preset against the table (64/8/nearest/2/world for Crunchy, Retro and Authentic, 128/16/PCF/4/world for Faithful and Clean, 256/16/PCF/8/+alias for Modern, 512/16/PCF/16/+alias for Ultra), the macro's GL and software arms and `M_DLOCCLUSION_MAX` (2 desktop, 1 ES and software), every value inside `gl_shadow.c`'s clamps; runs each preset's writes through `M_Display_DetectPreset`'s own conditions, parsed out of the source, and fails a preset detected as anything else or one that leaves a cvar detection reads unset; pins the Misc menu's `DLight Occlusion` row (soft-ok, 0..`M_DlightOcclusionMax ()` with wrap, Off / Walls / Shadows; the ceiling drops to Walls on desktop while `R_LightCluster_ShadesWorld ()` is false, since the cube maps light only the clustered path, and the row's note then says Shadows need `r_lightclusters 1`) and the `r_dlight_occlusion` / `r_shadow_casters` defaults |
| `test_dlight_onesided.py` | check that a dynamic light behind a face does not light it, on every tier (#97): lifts `R_AddDynamicLights`' prologue and `R_MarkLights`' surface loop out of the GL and software renderers, runs them against stubs (24 behind dark, 1 behind lit, `SURF_PLANEBACK`, liquids exempt, `r_dlight_occlusion 0` vanilla), and runs the same cases on moved and rotated brush entities through `R_DlightModelOrigin` and the three brush-entity marking loops it lifts, checking the light lands in model space (#495), and pins `ClusteredDlight`'s eye-facing normal and one-sided test, the `u_wfgridz.w` lane, `R_SetWorldDlight`, `R_SetWorldLiquid` and lit water's bracket, and the cvar in both builds |
| `test_no_loose_uniforms.py` | check that no engine source calls a `glUniform*` entry point other than `glUniform1i` (sampler to texture unit, made only through `R_ProgramSamplerUnit`'s GL arm and the two raw-GL files `test_rg_sampler_units.py` allows) and `glUniformBlockBinding`, and that `gl_func.h` and `gl_vidsdl.c` declare, load and gate on no other, so every value a shader reads comes through a std140 block (#19); the WEBSOFT canvas blit (`web_canvas_gl2.c`) calls GLES directly and is exempt |
| `test_flatpak_sf3.py` | check that the Flatpak's FluidSynth module, which disables libsndfile, still applies `patches/fluidsynth-sf3-vorbisfile.patch` with its define, both link drivers' vorbisfile libraries and its post-install check, and that `flake.nix`'s `fluidsynthPortable` applies the same patch (#168) |
| `test_soundfont_pin.py` | check that the TimGM6mb tarball hash agrees in `scripts/fetch-soundfont.sh`, `flake.nix`, the Flatpak manifest and `web/soundfont-NOTICE.txt` (#114) |
| `test_csqc_tail_slack.py` | compile `PR_ValidateBytecode` and check that a wide operand may run into the CSQC loader's zeroed globals tail but never start there, that builtin function records are not bounds-checked, and that the loader and `PR_ExecuteProgram` keep builtins from being entered (#120) |
| `test_render_disconnected.py` | check that `V_RenderView` draws nothing while `cls.state != ca_connected`, so the loading plaque no longer redraws the map just left after a disconnect, and that the plaque's backdrop still covers that frame (#204) |
| `test_alias_mesh_key.py` | check that an alias model finds its GPU mesh through slot and generation fields in its header, never by header address (which the loader frees before the first draw), that refused meshes are marked so the draw path does not rebuild them (#134), that the builder's scratch comes from the C heap (it runs from the draw path), and that `vid_restart` drops the table without deleting dead-context names (#387) |
| `test_dedicated_heap.py` | check that h2ded, hwsv and the client's `-dedicated` path default to a 512 MB heap, that `-heapsize` still overrides it in KB (and the usage says so), and that running out names `-heapsize` in both zone.c and its Rust port (#79) |
| `test_headless_drive_binds.py` | check that `headless-drive.sh`'s engine sandbox re-binds the userdir, basedir and engine after the throwaway `$HOME` bind, so an outdir under `$HOME` still gets its `qconsole.log` back |
| `test_lerp_pair_snaps.py` | check that `r_lerp_autodetect` records every measured pose pair and snaps only those over `r_lerp_autodetect_threshold` (either direction, read live) instead of setting `MOD_NOLERP` for the whole model; compiles the real `R_AliasPairSnaps` against a mock model (#94) |
| `test_showtris_models.py` | check that `r_showtris` outlines alias models, sprites, particles and the viewmodel through the helpers their own draws use (`R_AliasModelTransform`, `R_SpriteAxes`, `R_DrawParticles_Real`, the viewmodel helpers), and stays off on GLES (#81) |
| `test_vid_floor_200.py` | check that the video mode floor is 320x200 with one constant, and that the main menu's renderer label and version watermark yield to the menu where its canvas reaches their line (#135) |
| `test_gfx_malloc.py` | check that `W_LoadWadFile` (C and Rust) and the software renderer's conchars, backtile and loading disc load with `FS_LoadMallocFile` and release with `free`, never the 256 KB secondary zone, and that the wad differential harness still checks the release (#126) |
| `test_sbar_alpha.py` | check that `scr_sbaralpha` (default 1) fades only the bottom bar on GL -- edge indicators, inventory, overlays and the minimal HUD stay opaque -- through the pic alpha in `gl_draw.c`, and that the HUD menu row drives it on GL and `sbtrans` on software (#83) |
| `test_brush_abslight.py` | check that a brush entity's `abslight` is lit at 2A/255 like a lightmap sample of the same value (the software renderer's mapping), on both GL brush paths -- the romeric6 dark pillars (#326) |
| `test_offscreen_windowed.py` | check that `VID_SetMode` keeps SDL's offscreen driver windowed, so a headless `screenshot` reads back the frame that was drawn and not a fullscreen size the EGL surface never had (#401) |
| `test_particles_pause.py` | check that `R_UpdateParticles` advances particles by the render frame time but not while `cl.paused` (snow kept falling under the PAUSED plaque) |
| `test_webgl2_es_fixes.py` | check the WebGL2 fixes: texture swizzle gated on a cap that is false in the browser (opaque alpha written instead), no multisample OIT targets on the ES tier, web screenshots taken before the frame is presented, and the boot smoke failing on WebGL/GL errors (#112) |
| `test_contrast_parity.py` | check that `contrast` multiplies the frame, clamped to Ironwail's 1..2, in the shader, the uniform, the post-process trigger and the Display menu |
| `test_max_edicts.py` | check `max_edicts` defaults to 16384 under a 32000 `MAX_EDICTS`, that sounds on entities past 8192 go out whole on protocol 100 and are dropped on 18..21, that the client reads packed sound shorts unsigned, and that `EDICT_NUM` bounds by `sv_max_edicts` |
| `test_sv_autoload.py` | check `sv_autoload` (Ironwail's 0/1/2/3) is tried by `restart` and by a same-map `changelevel` / `changelevel2`, Hexen II's two death paths, checks the save exists after waiting for the save thread, and that saves and loads record `sv.lastsave` |
| `test_gl_call_sites.py` | check no `glFoo_fp` or raw `glFoo` call sits outside `engine/h2shared/gl_*.c` and `engine/hexen2/gl_*.c`, except the software renderer's WebGL2 presentation canvas (`web_canvas_gl2.c`, raw GLES, no loader table) -- one place knows the whole draw state, which a second backend needs (#453) |
| `test_draw_state_routing.py` | check no draw-state GL call (`glEnable`/`glDisable`, blend, depth, cull/front face, polygon offset/mode, stencil, colour mask, logic op, scissor, line width, point size, sample shading/coverage/mask, clip control, provoking vertex, primitive restart, `glUseProgram`; raw, `_fp`, extension-suffixed, called through a pointer, or taken as a value by a `#define` or assignment) sits outside `gl_pipeline.c`, whose state shadow is only right while nothing writes behind it -- except an allowlist of context-time switches matched by file, GL function and whole argument list with an exact count, the loader table `gl_func.h` and `web_canvas_gl2.c`; an allowlist entry matching nothing fails too (#337) |
| `test_texture_bind_routing.py` | check no texture-binding GL call (`glActiveTexture`, `glBindTexture`, `glBindTextures`, `glBindTextureUnit`, `glBindMultiTexture`, `glBindSampler(s)`, `glBindImageTexture(s)`, `glDeleteTextures`; raw, `_fp`, extension-suffixed, called through a pointer, or taken as a value by a `#define` or assignment) sits outside `gl_texbind.c`, whose per-unit binding shadow is only right while nothing binds, switches units or deletes behind it -- sampling binds go through `R_BindTextureSlot` / `R_BindTextures` / `R_BindTextureTarget`, upload and readback binds through `R_BindTextureForEdit`, compute storage images through `R_BindImageTexture`, deletes through `R_DeleteTextures`; allowlist (empty) matched by file, GL function and whole argument list with an exact count, an entry matching nothing fails; the loader table `gl_func.h` and `web_canvas_gl2.c` are exempt (#338) |
| `test_texbind_shadow.py` | compile the real `gl_texbind.c` against a fake GL that tracks every unit's per-target binding, the active unit and live names (a delete unbinds everywhere and the name is recycled lowest-first) and run 200000 seeded random slot / target / array / edit binds, deletes, gens and resets: unit 0 is active after every call, every bind really lands, an edit bind leaves the texture on unit 0, and a repeated bind issues no GL call -- plus fixed edit-between-binds, delete-then-recycle and 2D-vs-3D-on-one-unit sequences; and after every op `R_EditBinding`, the record `gl_texcreate.c` checks each call's texture against, is either unknown or unit 0's real binding, cube faces included (#338, #340) |
| `test_texture_create_routing.py` | check no texture-creation, upload or sampler-state GL call (`glGenTextures`, `glTexImage*`, `glTexSubImage*`, `glTexStorage*`, `glCompressedTex(Sub)Image*`, `glCopyTex(Sub)Image*`, `glTexParameter*`, `glGenerateMipmap`, `glClearTex*`, `glTexBuffer`, `glTextureView`, the DSA `glTexture*` spellings, sampler objects, and `glPixelStore` unless its first argument is a `GL_PACK_*` name; raw, `_fp`, extension-suffixed, called through a pointer, or taken as a value by a `#define`, an assignment, an argument, a return, an initializer or a ternary; EXT_direct_state_access and memory-object spellings included) sits outside `gl_texcreate.c`, whose entry points (`R_GenTextures`, `R_TexImage2D`/`3D`, `R_TexStorage2D`, `R_TexSubImage2D(Rows)`, `R_TexSetFilter`, `R_TexSetWrap`, `R_TexParameteri`/`f`, ...) name the texture they act on, which the SDL_GPU backend needs and GL does not; deletes stay with `test_texture_bind_routing.py`; allowlist (empty since P1a routed the last family) matched by file and GL function with an exact count, an entry matching nothing fails; the loader table `gl_func.h`, the generated null-GL table `rg_null.c` and `web_canvas_gl2.c` are exempt (#340 P1a) |
| `test_buffer_routing.py` | check no vertex/index/storage buffer, vertex-array or draw GL call sits outside `gl_buffer.c` and `gl_vbo.c`, whose seams (`GL_BindBuffer`, `R_GenBuffers`, `R_DeleteBuffers`, `R_BufferData`, `R_BufferSubData`, `R_GenVertexArrays`, `R_DeleteVertexArrays`, `R_BindVertexArray`, `R_Enable`/`DisableVertexAttribArray`, `R_VertexAttrib(I)Pointer`, `R_VertexAttrib4f`, `R_DrawElements(Instanced)`, `GL_ImmEnd`, and since P2e `GL_BindBufferBase` / `GL_BindBufferRange` / `GL_BindBuffersRange`) the SDL_GPU backend draws from: buffer names, storage and mapping (`glGenBuffers`, `glDeleteBuffers`, `glBufferStorage`, `glMapBuffer*`, `glCopyBufferSubData`, the `glNamedBuffer*` DSA spellings, ...), `glBindBuffer` / `glBufferData` / `glBufferSubData` unless the first argument is a literal target other than `GL_ARRAY_BUFFER` / `GL_ELEMENT_ARRAY_BUFFER` / `GL_SHADER_STORAGE_BUFFER`, every indexed binding (`glBindBufferBase`, `glBindBufferRange`, `glBindBuffers*`) whatever its target, every vertex-array and attribute call (pointers, formats, bindings, divisors, current values, the `glVertexArray*` DSA spellings), and every `glDrawArrays` / `glDrawElements` / instanced / base-vertex / range / indirect / multi-draw variant; every spelling and value hand-off, as `test_texture_create_routing.py`; allowlist matched by file and GL function with an exact count and the phase that routes it (world culling and the light grid, compute, P5; post-process and SSAO P4; shadows P6; the uniform-block buffers -gpu mirrors instead; the alias meshes and the ShadeDots buffer are routed since P2e and have none), an entry matching nothing fails; `gl_func.h`, `rg_null.c` and `web_canvas_gl2.c` are exempt (#340 P2d, P2e) |
| `test_rg_buffer_shadow.py` | compile the real `engine/h2shared/rg_buffer.c` against a fake SDL_GPU (buffers holding real bytes) and frame recorder, drive the `RG_*` entry points `gl_buffer.c`'s seams call, and check GL's binding rules (a pointer captures `GL_ARRAY_BUFFER`, the element buffer is the VAO's, current attribute values are context state, deleting the bound VAO binds 0), the layout an indexed draw gets (shared slots, the disabled `ATTR_POSITION`..`ATTR_COLOR` inputs as an instance-rate FLOAT4 slot of `glVertexAttrib4f`'s values, GL's default `(0,0,0,1)`), uploads (respecify cycled, sub-update not, a resize retired until the frame ends), the segment hazard (an upload into a buffer the open segment's recorded draw reads flushes first; a dropped draw stamps nothing), a sub-update into a buffer named before the name table grew, and a reused name never drawn from by a VAO that named the deleted one (#340 P2d); and (#340 P2e) the skeletal VAO's formats (`BYTE4_NORM` normals, `UBYTE4_NORM` weights, integer `UBYTE4` bone indices, one slot, no constants) and storage: storage-read usage on every buffer, `glBufferData` through `GL_SHADER_STORAGE_BUFFER`, a binding point's buffer for a draw's slot (a range only at offset 0 and inside the buffer, nothing for a uniform-target bind or once deleted), `GL_Upload`'s arm a buffer per upload at offset 0 reused cycled the next frame and refusing uniform data, and the hazard on a storage buffer a recorded draw reads |
| `test_program_routing.py` | check every GPU program is built by id (`rprog_id_t`, `gl_progid.h`) through `R_Program_Create` from its row of `gl_shader.c`'s `rprog_defs`: no `glCreateShader`, `glShaderSource`, `glCompileShader`, `glCreateProgram`, `glAttachShader`, `glDetachShader`, `glLinkProgram`, `glBindAttribLocation`, `glBindFragDataLocation`, `glCreateShaderProgramv`, `glProgramBinary`, `glShaderBinary`, `glSpecializeShader`, `glProgramParameteri` or ARB shader-object call outside `gl_shader.c` (every spelling and value hand-off, as `test_texture_create_routing.py`), and inside it the stage calls only in `RP_CompileStage` and the program calls only in `R_Program_Create`; `RP_CompileStage` hands `glShaderSource` exactly its row's header and body; every `R_Program_Create` argument is an `RPROG_*` enumerator, an element of a `static const rprog_id_t` array of them, or an `rprog_id_t` parameter whose callers pass one; one row per enumerator, named after it, with an `RPROG_LINK_*` mode and `_vert`+`_frag` or one `_comp` stage; each stage's blob is the offline copy EmbedShaders writes for its body and `#define` set; `gl_progid_refl.h` has exactly one row per offline copy, of the right stage kind, included by `gl_shader.c` only; and no id reaches no `R_Program_Create`. Exempt: `gl_func.h`, `rg_null.c`, `web_canvas_gl2.c` (#340 P1b) |
| `test_maps_resume.py` | check that picking a map in the Maps browser opens Ironwail's skill step (without leaving the menu first), which offers "Resume last game" when `autosave/<map>` exists and loads it |
| `test_joy_always_active.py` | check `joy_always_active` (default 0) gates rumble and gyro steering, and only those, on the gamepad being the input last used, tracked as upstream |
| `test_demo_rewind.py` | check demo rewind: the left arrow gives a negative speed that restores a keyframe and silently replays to the target (sounds muted, prints/stufftext/music held back), keyframe save and restore cover the same state in the same order, keyframes are dropped per level and bounded, and the demo bar shows `<<` |
| `test_cl_startmenu.py` | check `cl_startmenu` (default 1) gates both the `menu_main` that `startdemos` queues and the one Hexen II's `hexen.rc` runs during startup |
| `test_sv_maxdatagram.py` | check `sv_maxdatagram` (default 0 = off) caps the per-frame unreliable datagram for every connection after the protocol clamp, floored at 1024 |
| `test_audit_tail_174.py` | check the 2026-08-18 audit tail stays fixed: `r_entdist` culls instanced models, alias batches cannot run out, mesh buffers are sized from `MAXALIASTRIS`, both-bits scale flags read as 0, dead GL `R_LightPoint` stays gone |
| `test_appimage_apprun.py` | run `appimage/AppRun` against a stub engine: data beside the AppImage becomes `-basedir`, `~/.hexen2` data runs as-is, no data explains and exits, `-basedir`/`-help` pass through, `--get-demo` fetches; and the runtime is pinned to a tagged release |
| `test_softemu_lut.py` | compile the real softemu metric functions and `PP_FillPaletteGrid` out of `gl_postprocess.c` and check the 128³ table cell for cell against a full scan, for all three metrics, on a tie-heavy palette (and the Hexen II palette with `HEXEN2_PALETTE=`) |
| `test_mboit.py` | check `r_oit 2` (moment-based OIT): `engine/shaders/oit.inc`'s three `u_oit_mode` outputs with vkQuake's constants, the translucency block redrawn while `OIT_NextPass()`, water chains kept on the moment pass, fp32 moment targets, and the resolve's exp(-b0) |
| `test_vid_restart_brush_textures.py` | compile `Mod_ReloadTextures`' model selection against stubs: every loaded brush model is re-uploaded on `vid_restart`, with `cl.worldmodel` NULL too, `*N` submodels skipped; load and reload share `Mod_UploadBrushTexture`, so replacements and material maps survive a restart (#430) |
| `test_hwsv_bundled_progs.py` | check `hwsv` reaches the `hwprogs.dat` shipped beside it: bundle lookup compiled for H2W, tried only after the gamedir lookup fails, for the server's own gamedir, `-vanillaprogs` honoured, a bad file refused out loud (#37) |
| `test_gpu_lightstyles.py` | check `r_gpulightstyles`: the per-frame style checks gated off in that mode, `R_BuildLightMap`'s raw layer 0 with the style index in alpha, the layer atlases cleared to no-style, the world shaders' style sum right before the dlight add, desktop only (`#ifndef GL_ES` in `sworld_frag.glsl` / `sworld_opaque_frag.glsl`), the per-frame setup after the cluster update, recorded for the world draws (#439, #450 W2) |
| `test_softemu_spans.py` | check that `r_softemu_spans` routes the world, brush-poly and liquid texture coordinates through `SpanUV` (highp, guarded), at 8/320 of the render width (16 for liquids), and resets around each override (#176) |
| `test_skyfog_layers.py` | check that `Fog_SetupFrame` writes the sky fog every frame and unconditionally (fog colour, `r_skyfog`/worldspawn `skyfog` clamped to 0..1, 0 with fog off) as the only `R_SetSkyFog` caller (and `R_SetSkyFog` the only toucher of `fragparams.skyfog`), from a `Fog_GetColor` clamped to 0..1 as in Ironwail, before the world and `Sky_DrawSky` draw; that both two-pass scrolling-sky paths (`R_DrawSkyChain`, `EmitSkyPolysMulti`) blend a flat fog pass over the front layer under `Sky_DrawSkyBox`'s condition, `Sky_LayerFogColor` pinned whole; that only `ssky_layers_frag.glsl` reads `u_skyfog`, so skybox faces are not fogged twice; and that `Sky_DrawSky` gates the skybox on `r_fastsky` alone, never on `skyfog` (#487); `--root DIR` checks another tree (#486) |
| `test_multiplayer_join.py` | compile extracted production join/filesystem functions and test protocol fallback, gamedir validation, HW/Siege precedence, and restoration |
| `test_hcc_tail_slack.py` | compile the `hcc_tail_slack/` fixtures with a given `hcc` and apply the engine's operand and function-record bounds to them and to any `progs.dat` named |

---

## qcdis.py — progs.dat disassembler

Checks whether the HexenC in `gamecode/hc` matches the bytecode players run,
without building anything. To read gamecode as source, use `utils/dcc`
(`dhcc`) instead; `qcdis.py` is for proving two builds are semantically
identical.

```
qcdis.py <progs.dat> [function ...]     disassemble named functions
qcdis.py <progs.dat> --pseudo <func>    collapse temporaries into expressions
qcdis.py <progs.dat> --list [PATTERN]   list functions (regex, case-insensitive)
qcdis.py --check-opcodes                verify the opcode table vs common/pr_comp.h
```

Cross-build diff:

```sh
nix build .#gamecode
diff <(tools/qcdis.py ~/hexen2/data1/PROGS.DAT --pseudo Use_TimeBomb) \
     <(tools/qcdis.py result/share/hexenwail/data1/progs.dat --pseudo Use_TimeBomb)
```

Does a symbol exist at all:

```sh
$ tools/qcdis.py ~/hexen2/sot/progs.dat --list '^TimeBomb'
TimeBombBoom                             invntory.hc:21159
TimeBombTouch                            invntory.hc:21173
```

Both progs versions (v6 and v7) are supported; struct layouts come from
`common/pr_comp.h`. v6 operands are read unsigned except jump offsets, matching
the engine (`engine/h2shared/pr_exec.c`) and `hcc`. Reading them all as signed
corrupts about a third of the functions in retail `data1/PROGS.DAT`.

Reading the output:

- **Braced constants are file values, not runtime values.** `v_forward{0 0 0}`
  is the initialiser in the globals block, not what the code multiplies by.
- **Indirect calls** through an entity field usually render as `()`. The
  preceding `LOAD_FNC` shows what is actually called.
- **`--pseudo` is lossy.** It is for diffing, not for reading control flow.
- **Function extent is inferred.** A function is capped at `--max-statements`
  (default 400) and prints a truncation line when it hits it; raise the limit
  rather than assuming the function ended.
- **`t<N>`** is an unnamed global, usually a compiler temporary. `I+` is an
  `IMMEDIATE_NAME` pooled constant.

### Verification

Run `--check-opcodes` after any upstream merge that touches `pr_comp.h`:

```sh
$ tools/qcdis.py --check-opcodes
opcode table matches .../common/pr_comp.h (105 opcodes)
```

End-to-end check: build the same source as v7 and v6, and the disassembly of
both must be identical.

```sh
nix build .#utils -o /tmp/u
cp -r gamecode /tmp/gc && chmod -R u+w /tmp/gc
(cd /tmp/gc && /tmp/u/bin/hcc -src hc/h2 -os -v7)      # v7 alongside the v6 from .#gamecode
```

---

## edict_pick.py + headless-cfg.sh — scripted runs and entity dumps

`headless-cfg.sh` writes a config that starts a map, waits, throws an item and
calls `edicts` at several points, marking each dump with a `ZZZ<TAG>` line.
`edict_pick.py` splits the log on those markers and prints only the classnames
and fields you ask for. No X server is needed.

```sh
tools/headless-cfg.sh 2 demo1 /tmp/run.cfg 108     # class 2 (crusader), throw impulse 108
hexenwail -basedir ~/hexen2 -condebug +exec /tmp/run.cfg > /tmp/run.log
tools/edict_pick.py /tmp/run.log player,timebomb
```

Pass `all` as the classname list to keep every entity, and `--fields` to choose
which fields are printed and in what order.

---

## serve.sh — the dedicated server on stdin

Try this first for anything below the renderer: gamecode, physics, savegames,
protocol, filesystem, cvars. `h2ded` boots in about a second and needs nothing
but the engine: every run gets `-userdir <outdir>/userdir`, and `bwrap` adds a
throwaway `$HOME` and read-only data when it is installed (`NO_BWRAP=1` skips
it). It cannot show you what anything looks like; use `headless-drive.sh` for
that.

```sh
nix build .#h2ded-bundled -o result-h2ded
nix build .#demodata      -o result-demodata
printf 'map demo1\nwait 3\nstatus\nedicts\n' | \
  tools/serve.sh /tmp/out result-demodata/share/hexenwail
```

One console command per line. `wait N` sleeps N seconds (the engine's own
`wait` is one frame). A final `quit` is appended for you.

Read `<outdir>/qconsole.log`: **h2ded prints nothing to stdout when stdout is
not a tty**, so `engine.stdout` is normally empty. `-condebug` is passed for
you, and the run uses a throwaway `$HOME`.

---

## headless-drive.sh — menu testing under Xvfb

Runs the engine under Xvfb and sends real X key events with `xdotool`, so menu
code runs exactly as it would from a keyboard. Each step is screenshotted with
ImageMagick `import`.

```sh
nix build .#default
ENGINE=result/bin/hexenwail tools/headless-drive.sh noportals /tmp/out ~/hexen2
```

Requires `Xvfb`, `xdotool`, `bwrap` and `import`. Without them:

```sh
nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick nixpkgs#bubblewrap --command \
  env ENGINE=result/bin/hexenwail tools/headless-drive.sh noportals /tmp/out ~/hexen2
```

(`Xvfb` ships in `xorg-server`; there is no `nixpkgs#xvfb`.)

The run uses a throwaway `$HOME` with the basedir bound read-only, so it cannot
touch your real config or savegames. Output in `<outdir>`: numbered PNGs,
`engine.stdout`, `xvfb.log`, `qconsole.log`, and the sandbox HOME under `work/`.

**`script` is the general-purpose scenario.** It reads steps from `$STEPS`, one
verb per line:

```sh
printf 'shot 01-main\nkeyn Down 3\nkey Return\nshot 02-mods\n' > /tmp/steps.txt
STEPS=/tmp/steps.txt tools/headless-drive.sh script /tmp/out ~/hexen2
```

Verbs: `shot` / `shotf` / `key` / `keyn` / `type` / `enter` / `console` / `cmd`
/ `hold` / `mouse` / `click` / `wipe` / `sleep`, plus `#` comments. `type`
types without submitting, `enter` submits, and `cmd` wraps one console command
in the grave-key toggle. An unrecognised verb aborts the run.

The other `case` arms are worked examples. Add one only when a run needs real
logic (a loop, a control condition). Timings are generous: the engine gets
25 s to load a map.

---

## hw-drive.sh — a live HexenWorld session

The client's HexenWorld reader (`engine/hexen2/cl_hw.c`) runs only after
`connect hw://`, so no other driver here reaches it. `hw-drive.sh` starts
`hwsv` on loopback, boots the client through `headless-drive.sh`, connects it,
and then runs your step file with the client already in the game.

```sh
nix build .#default      -o result
nix build .#hwsv-bundled -o result-hwsv
nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick nixpkgs#bubblewrap --command \
  env STEPS=tools/hw-bluestep.steps ./tools/hw-drive.sh /tmp/out-hw
```

About 90 s. It ends with `client_rc=0  server saw joins: 1  client signons: 1`
and exits non-zero if the client never signed on. Output: `server.log` (the
server console), `client/` (PNGs and `qconsole.log`), `steps.txt` (your steps
with the connect preamble in front).

**It needs retail data.** `hwsv` refuses the demo: `BASEDIR` (default
`~/hexen2`) must hold `data1/pak0.pak`, `data1/pak1.pak`, `hw/pak4.pak` and
`hw/hwprogs.dat`.

The server starts with `+deathmatch 0 +coop 0 +skill 0` because the gamecode's
cheat impulses return early otherwise; set `SVARGS=` to change that. `MAP=`,
`PORT=`, `SERVER=`, `ENGINE=`, `LOAD=`, `JOIN=` and `TIMEOUT=` are described at
the top of the script.

`hw-bluestep.steps` is the worked example and the live check for issue #309:
an invisible player walks, and the first `walk-*.png` frames must show pale
blue specks around a translucent footprint. Red specks mean wire particle type
28 is being drawn as red fire again.

### console_tab — TAB completion

```
tools/headless-drive.sh console_tab /tmp/out ~/hexen2 -condebug +"map demo1"
```

Presses TAB in a live console across eight cases. Keep both arguments:
without `+map demo1` the console cannot be opened from the main menu and the
run screenshots the untouched menu, which looks like a pass; without
`-condebug` the ambiguous-match listing is lost. Read results off the PNGs: the
line under `]` is the assertion.

### Demo playback — `scripts/demo-playback-smoke.sh`

```
nix build .#default -o result && nix build .#demodata -o result-demodata
nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick \
          nixpkgs#bubblewrap --command ./scripts/demo-playback-smoke.sh
```

Records a demo of `demo1`, then boots a **second, fresh** engine straight into
`+playdemo`. The second boot is the whole point: a demo played in the session
that recorded it still has the listen server's `cls.netcon`, so it survives a
client that refuses to read without one. Only a cold boot has no qsocket — the
state `startdemos` puts a mod's front end in. ~2 minutes; needs no retail data.

The verdict is `couldn't exec <name>end.cfg` in `qconsole.log`, not a
screenshot: a frozen client still prints `Playing demo from …` and the
matching `start.cfg` line.

`tools/test_demo_playback_gate.py` is the free CI half of the same invariant.

### Quitting — `scripts/quit-smoke.sh`

```
nix build .#default -o result && nix build .#demodata -o result-demodata
./scripts/quit-smoke.sh
```

Loads `demo1`, issues `quit` from a cfg the way every headless harness does,
and asserts the process exits 0 inside `LIMIT` seconds. Needs no X and no
retail data — SDL's offscreen video driver is enough — so it is the cheapest
check here at ~60 s.

The assertion is the **exit status**, never a log line: the bug it guards
(GitHub #204) let the engine print everything up to and including the `quit`
and then keep rendering, because `quit` raised the confirmation prompt and a
headless run has no key to answer it with. The two lines that hang was always
reported with, `Sending clc_disconnect` and `Client player removed`, are
printed by the harness timeout's SIGTERM on the way out, *after* the hang.

---

## progs_crc.py — which gamecode is this file?

```
progs_crc.py <progs.dat> [progs.dat ...]
```

Prints the two numbers `PR_ClassifyGamecode()` (`engine/h2shared/pr_edict.c`)
works from, and names the Raven release when the pair is one it knows:

- **file crc**: CRC over the whole file. Identifies one exact build.
- **progdefs crc**: the header's `crc` field. Identifies the interface
  generation only (`PROGS_V103_CRC` / `PROGS_V111_CRC` / `PROGS_V112_CRC`).
  Releases share these (1.08 to 1.11 all declare 38488) and so do mods, so
  only the file CRC tells them apart.

`pr_raven_images[]` matches on both and covers 1.03, 1.08, 1.09, 1.10, 1.11,
portals 1.12 and 1.12a, and the November 1997 demo. A Raven image it does not know
(a localised release, say) is reported as `Third-party`: run this on it and
add the pair to `pr_raven_images[]` and `KNOWN_RAVEN` together.
`test_progs_release_ident.py` fails if the two tables drift, and with
`HCODE_ARCHIVE=<uhexen2-hcode_archive checkout>` it re-reads the archive rows
from the binaries.
