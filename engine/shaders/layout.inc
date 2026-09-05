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
 *               (Samplers and images are unaffected: layout(binding=) on those
 *               is fine wherever compute exists at all.)
 *
 * VULKAN and GL_SPIRV are predefined by glslang itself -- do not pass -DVULKAN,
 * redefining it is an error.  Undefined identifiers evaluate to 0 in a GLSL
 * #if, so the plain-GLSL arm is what a driver sees.
 */

#if VULKAN >= 100
  #define TEXBIND(b)        layout(set = 0, binding = b)
  #define IMGBIND(b, fmt)   layout(set = 1, binding = b, fmt)
  #define UBO(b)            layout(std140, set = 2, binding = b)
#elif GL_SPIRV >= 100
  #define TEXBIND(b)        layout(binding = b)
  #define IMGBIND(b, fmt)   layout(binding = b, fmt)
  #define UBO(b)            layout(std140, binding = b)
#else
  #define TEXBIND(b)        layout(binding = b)
  #define IMGBIND(b, fmt)   layout(binding = b, fmt)
  #define UBO(b)            layout(std140)
#endif
