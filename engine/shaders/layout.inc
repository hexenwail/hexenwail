/* layout.inc -- resource-qualifier macros so one shader source can be fed to
 * the GL driver, to glslang's OpenGL-SPIR-V frontend and to its Vulkan
 * frontend without forking the file.
 *
 * The three targets genuinely disagree about who owns a binding point:
 *
 *   Vulkan      every resource needs an explicit set AND binding.  The set
 *               numbers below are the ones SDL_GPU fixes for a COMPUTE
 *               pipeline -- set 0 sampled textures, set 1 read-write storage
 *               textures/buffers, set 2 uniform buffers.  A graphics pipeline
 *               numbers its sets differently, so a vertex/fragment shader will
 *               need its own macros rather than reusing these.
 *
 *   GL_SPIRV    glslang -G rejects a uniform block with no layout(binding=X),
 *               but has no notion of descriptor sets.
 *
 *   plain GLSL  layout(binding=) on a UNIFORM BLOCK is GL 4.2 / GLSL ES 3.10,
 *               and the ES tier this engine also targets is ES 3.00, which
 *               rejects it outright.  So the block deliberately carries no
 *               binding here and the C side assigns one with
 *               glUniformBlockBinding.  Keep the two in agreement by hand.
 *               (Samplers and images are unaffected in a COMPUTE shader:
 *               compute does not exist below GL 4.3 / ES 3.1, and both of
 *               those have layout(binding=) on an opaque uniform.  A graphics
 *               shader has no such floor -- see FRAG_TEX below.)
 *
 * VULKAN and GL_SPIRV are predefined by glslang itself -- do not pass -DVULKAN,
 * redefining it is an error.  The arms are selected with #ifdef and not with
 * `#if VULKAN >= 100`, because an undefined identifier in a #if expression is
 * a hard error in the ES profile ("undefined macro in expression not allowed
 * in es profile") even though desktop GLSL quietly evaluates it as 0.  The
 * compute shaders never noticed: there is no compute stage on the ES tier at
 * all, so nothing had ever fed this file to an ES compiler until the 2D
 * program did.
 */

#ifndef LAYOUT_INC
#define LAYOUT_INC

#ifdef VULKAN
  #define TEXBIND(b)        layout(set = 0, binding = b)
  #define IMGBIND(b, fmt)   layout(set = 1, binding = b, fmt)
  #define UBO(b)            layout(std140, set = 2, binding = b)
#elif defined(GL_SPIRV)
  #define TEXBIND(b)        layout(binding = b)
  #define IMGBIND(b, fmt)   layout(binding = b, fmt)
  #define UBO(b)            layout(std140, binding = b)
#else
  #define TEXBIND(b)        layout(binding = b)
  #define IMGBIND(b, fmt)   layout(binding = b, fmt)
  #define UBO(b)            layout(std140)
#endif

/* Compute resources whose GL binding point and SDL_GPU slot differ.
 *
 * TEXBIND/IMGBIND/UBO above take one number because their callers so far
 * happen to use the same index on both sides.  GL binding points are global
 * and fixed by the C side (glBindBufferBase, glActiveTexture), while SDL_GPU
 * renumbers each compute pipeline's resources from 0 within each set: set 0
 * holds sampled textures, then read-only storage textures, then read-only
 * storage buffers; set 1 holds read-write storage textures, then read-write
 * storage buffers.  So a shader with eight SSBOs at GL points 0-7 and a
 * sampler on unit 1 (gl_worldcull.c's cull_mark) needs both numbers spelled.
 *
 *   TEXBIND2(gl, vk)  sampled texture; vk is its index in set 0
 *   SSBO_RO(gl, vk)   readonly storage buffer; vk follows set 0's textures
 *   SSBO_RW(gl, vk)   storage buffer that is written (read-write or
 *                     writeonly -- SDL_GPU has no write-only kind); set 1
 *
 * TEXBIND(b) is only right when b is also the dense set-0 index: the SSAO
 * stages' TEXBIND(8..10) gave SDL_GPU bindings 8.. in a set that holds two or
 * three textures (#340).  tools/test_sdlgpu_bindings.py checks every stage's
 * Vulkan SPIR-V against this numbering, in the shaders-spirv CI job.
 *
 * The GL arms use `gl` only, so the driver sees exactly the qualifiers it
 * would from a hand-written layout(std430, binding = gl).  Storage blocks
 * take layout(binding=) wherever compute exists (GL 4.3 / ES 3.1), unlike
 * uniform blocks, so no arm needs C-side binding for them. */
#ifdef VULKAN
  #define TEXBIND2(gl, vk)  layout(set = 0, binding = vk)
  #define SSBO_RO(gl, vk)   layout(std430, set = 0, binding = vk)
  #define SSBO_RW(gl, vk)   layout(std430, set = 1, binding = vk)
#else
  #define TEXBIND2(gl, vk)  layout(binding = gl)
  #define SSBO_RO(gl, vk)   layout(std430, binding = gl)
  #define SSBO_RW(gl, vk)   layout(std430, binding = gl)
#endif

/* ------------------------------------------------------------------------
 * Graphics pipeline.  Separate macros rather than reuse of the compute ones
 * above, because SDL_GPU numbers a graphics pipeline's descriptor sets
 * differently and the two must not be allowed to drift into each other:
 *
 *   vertex stage    set 0 sampled textures / storage, set 1 uniform buffers
 *   fragment stage  set 2 sampled textures / storage, set 3 uniform buffers
 *
 * The `b` argument is therefore the binding WITHIN that set -- stage-local,
 * so a vertex UBO and a fragment UBO are both binding 0.  GL has one binding
 * space per resource type for the whole program instead of one per set, so
 * the GL_SPIRV arm's numbers collide by construction between stages.  That is
 * survivable only because nothing links -G modules: CompileSpirv.cmake runs
 * the two frontends per stage as a check that every resource carries the
 * qualifiers Vulkan will demand.  A future GL SPIR-V *loader* would have to
 * renumber here, not at the call site.
 *
 * What each arm can spell, verified against glslangValidator rather than
 * assumed, because the three disagree differently than they do for compute:
 *
 *   vertex input     layout(location=) is legal in ES 3.00.  Kept in the
 *                    plain arm so the file states its own attribute slots;
 *                    they must equal the ATTR_* constants in gl_shader.h,
 *                    since a layout qualifier overrides the C side's
 *                    glBindAttribLocation rather than conflicting with it.
 *   varying          layout(location=) on a vertex OUTPUT is rejected by
 *                    ES 3.00 ("not supported in this stage"); it is ES 3.10 /
 *                    ARB_separate_shader_objects.  Empty in the plain arm,
 *                    where the linker matches varyings by name.
 *   fragment output  layout(location=) is legal in ES 3.00.
 *   sampler          layout(binding=) is NOT legal in ES 3.00 -- unlike the
 *                    compute case above, which is why FRAG_TEX cannot just be
 *                    TEXBIND.  Empty in the plain arm; the C side assigns the
 *                    texture unit with glUniform1i, as it always has.
 *   uniform block    same three-way split as UBO above.
 *   storage buffer   FRAG_SSBO(gl, vk): `gl` the GL binding point, which the
 *                    plain arm spells because desktop GL 4.3 allows
 *                    layout(binding=) on a storage block; `vk` its index in
 *                    set 2, after the stage's sampled textures, which is
 *                    where SDL_GPU puts a fragment stage's read-only storage
 *                    buffers.  Storage blocks do not exist in ES 3.00 at all,
 *                    so a caller keeps them under #ifndef GL_ES.
 *                    VERT_SSBO(gl, vk) is the vertex-stage twin: set 0,
 *                    after that stage's sampled textures (#450, the instanced
 *                    alias stage, which has none, so its four buffers are
 *                    0..3 in both numberings).  Desktop-only for the same
 *                    reason.
 * ------------------------------------------------------------------------ */

#ifdef VULKAN
  #define VERT_IN(loc)      layout(location = loc)
  #define VERT_OUT(loc)     layout(location = loc)
  #define VERT_UBO(b)       layout(std140, set = 1, binding = b)
  #define FRAG_IN(loc)      layout(location = loc)
  #define FRAG_OUT(loc)     layout(location = loc)
  #define FRAG_TEX(b)       layout(set = 2, binding = b)
  #define FRAG_UBO(b)       layout(std140, set = 3, binding = b)
  #define FRAG_SSBO(gl, vk) layout(std430, set = 2, binding = vk)
  #define VERT_SSBO(gl, vk) layout(std430, set = 0, binding = vk)
#elif defined(GL_SPIRV)
  #define VERT_IN(loc)      layout(location = loc)
  #define VERT_OUT(loc)     layout(location = loc)
  #define VERT_UBO(b)       layout(std140, binding = b)
  #define FRAG_IN(loc)      layout(location = loc)
  #define FRAG_OUT(loc)     layout(location = loc)
  #define FRAG_TEX(b)       layout(binding = b)
  #define FRAG_UBO(b)       layout(std140, binding = b)
  #define FRAG_SSBO(gl, vk) layout(std430, binding = gl)
  #define VERT_SSBO(gl, vk) layout(std430, binding = gl)
#else
  #define VERT_IN(loc)      layout(location = loc)
  #define VERT_OUT(loc)
  #define VERT_UBO(b)       layout(std140)
  #define FRAG_IN(loc)
  #define FRAG_OUT(loc)     layout(location = loc)
  #define FRAG_TEX(b)
  #define FRAG_UBO(b)       layout(std140)
  #define FRAG_SSBO(gl, vk) layout(std430, binding = gl)
  #define VERT_SSBO(gl, vk) layout(std430, binding = gl)
#endif

/* VK_FLIP_Y(): the last statement of every vertex stage's main(), and of
 * every early return from it.
 *
 * SDL_GPU's clip space is +Y up with framebuffer row 0 at the TOP (it gives
 * Vulkan a negative-height viewport to match D3D and Metal), while GL puts
 * NDC y = -1 on row 0.  Negating y in the Vulkan SPIR-V makes every render
 * target hold the same rows in the same memory order as under GL, so
 * gl_FragCoord, dFdy, texture coordinates into render targets and readbacks
 * all keep their GL meaning.  Two consequences for the backend:
 *   - nothing may be drawn straight into the swapchain texture, which SDL_GPU
 *     shows row 0 at the top: every pass renders offscreen, and the final
 *     present blit is the one place that flips;
 *   - GL viewport and scissor rectangles (bottom-left origin, y counted up
 *     in rows) go to SDL_GPU UNCHANGED -- with the negative viewport they
 *     cover the same memory rows GL does.  Converting them to a top-left
 *     origin would break this.
 * The negation reverses winding: GL's counter-clockwise front face is
 * SDL_GPU's CLOCKWISE.  DXIL and MSL are cross-compiled from the Vulkan
 * SPIR-V, so they inherit it.
 *
 * glslang's --invert-y would have done this, but it is implemented only in
 * the HLSL front end; on GLSL it is accepted and ignored (#340).
 *
 * Written without a trailing semicolon at the call site, so the GL arms see
 * no token at all -- not even an empty statement -- and the preprocessed GL
 * program is token-identical (the embedded source string does carry the
 * call).  tools/test_vk_flip_y.py checks the call sites and, on the
 * compiled SPIR-V, that every Vulkan vertex stage stores the negated y. */
#ifdef VULKAN
  #define VK_FLIP_Y()       gl_Position.y = -gl_Position.y;
#else
  #define VK_FLIP_Y()
#endif

#endif /* LAYOUT_INC */
