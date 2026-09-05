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
 * ------------------------------------------------------------------------ */

#ifdef VULKAN
  #define VERT_IN(loc)      layout(location = loc)
  #define VERT_OUT(loc)     layout(location = loc)
  #define VERT_UBO(b)       layout(std140, set = 1, binding = b)
  #define FRAG_IN(loc)      layout(location = loc)
  #define FRAG_OUT(loc)     layout(location = loc)
  #define FRAG_TEX(b)       layout(set = 2, binding = b)
  #define FRAG_UBO(b)       layout(std140, set = 3, binding = b)
#elif defined(GL_SPIRV)
  #define VERT_IN(loc)      layout(location = loc)
  #define VERT_OUT(loc)     layout(location = loc)
  #define VERT_UBO(b)       layout(std140, binding = b)
  #define FRAG_IN(loc)      layout(location = loc)
  #define FRAG_OUT(loc)     layout(location = loc)
  #define FRAG_TEX(b)       layout(binding = b)
  #define FRAG_UBO(b)       layout(std140, binding = b)
#else
  #define VERT_IN(loc)      layout(location = loc)
  #define VERT_OUT(loc)
  #define VERT_UBO(b)       layout(std140)
  #define FRAG_IN(loc)
  #define FRAG_OUT(loc)     layout(location = loc)
  #define FRAG_TEX(b)
  #define FRAG_UBO(b)       layout(std140)
#endif
