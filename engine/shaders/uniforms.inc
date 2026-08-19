// Descriptor layout.
//
// On the GL path these expand to nothing beyond the packing qualifier: GL 4.3
// and GLSL ES 3.00 take a block's binding point from C (glUniformBlockBinding)
// and a sampler's texture unit from glUniform1i, so the shader does not name
// either.  GLSL ES 3.00 could not name them anyway -- layout(binding=) on a
// uniform block is 4.20 / ES 3.10.
//
// Under VULKAN they expand to the numbering SDL_GPU fixes and documents in
// SDL_gpu.h: for a graphics pipeline the vertex stage uses set 0 for sampled
// textures and storage buffers and set 1 for uniform buffers, and the fragment
// stage uses set 2 and set 3.  FRAGMENT_STAGE picks the pair, and each
// fragment shader declares it itself rather than taking it from the command
// line, so a vertex and fragment stage compiled in one invocation still each
// get the right sets.
#ifdef VULKAN
  #ifdef FRAGMENT_STAGE
    #define UBO(b)  layout(std140, set = 3, binding = b)
    #define TEX(b)  layout(set = 2, binding = b)
    #define SSBO(b) layout(std430, set = 2, binding = b)
  #else
    #define UBO(b)  layout(std140, set = 1, binding = b)
    #define TEX(b)  layout(set = 0, binding = b)
    #define SSBO(b) layout(std430, set = 0, binding = b)
  #endif
#else
  #define UBO(b)  layout(std140)
  #define TEX(b)
  #define SSBO(b) layout(std430, binding = b)
#endif


// Stage interface locations.  Vulkan requires one on every user input and
// output; GL assigns vertex attributes from C (glBindAttribLocation, the ATTR_*
// values in gl_shader.h) and matches varyings by name, and GLSL ES 3.00 cannot
// put a location on a varying at all -- that is enhanced layouts, 4.40 / ES
// 3.20.  The numbers here match ATTR_* exactly, so the two agree.
// GL and Vulkan spell the two draw-index builtins differently.  The values
// agree for how this engine draws: gl_VertexIndex differs from gl_VertexID only
// by the base vertex of an indexed draw, and gl_InstanceIndex differs from
// gl_InstanceID only by firstInstance -- the instanced alias path carries its
// own base in u_inst_base and draws with firstInstance 0.  Revisit if either
// ever becomes non-zero.
#ifdef VULKAN
  #define gl_VertexID   gl_VertexIndex
  #define gl_InstanceID gl_InstanceIndex
#endif

#ifdef VULKAN
  #define ATTR(n)    layout(location = n)
  #define VARY(n)    layout(location = n)
  #define FRAGOUT(n) layout(location = n)
#else
  #define ATTR(n)
  #define VARY(n)
  #define FRAGOUT(n)
#endif
UBO(0) uniform PerFrame {
    highp vec4 u_fogv;     // xyz fog color, w fog density
    highp vec4 u_eyetime;  // xyz eye position, w cl.time
    highp vec4 u_worldp;   // x caustics intensity, y caustics time, z overbright, w bicubic
    highp vec4 u_debugp;   // x r_fullbright, y r_lightmap
    highp vec4 u_skyfogv;  // rgb sky fog color, a blend
    highp vec4 u_windv;    // xy sky wind uv offset
    highp vec4 u_ppup;     // xyz particle billboard up
    highp vec4 u_ppright;  // xyz particle billboard right
    highp vec4 u_pvpn;     // xyz view forward
    highp vec4 u_porigin;  // xyz camera origin, w cl.time for particles
    highp mat4 u_viewprojm;// instanced alias view-projection
};
UBO(1) uniform PerDraw {
    highp mat4 u_mvp;
    highp mat4 u_modelview;
    highp mat4 u_modelm;   // model-only matrix, no view
    highp vec4 u_draw0;    // x alpha threshold, y force-opaque alpha, zw alias caustics
    highp vec4 u_draw1;    // xyz soft-particle params
    highp ivec4 u_drawi;   // x pose base, y instance base, z pose vertex type
};
// Scalar aliases: shader bodies were written against these names and are
// left untouched.  Chained swizzles (u_worldp.xy.x) are legal GLSL.
#define u_fog_color u_fogv.xyz
#define u_fog_density u_fogv.w
#define u_eyepos u_eyetime.xyz
#define u_time u_eyetime.w
#define u_caustics u_worldp.xy
#define u_overbright u_worldp.z
#define u_lightmap_bicubic u_worldp.w
#define u_lightdebug u_debugp.xy
#define u_skyfog u_skyfogv
#define u_wind u_windv.xy
#define u_pup u_ppup.xyz
#define u_pright u_ppright.xyz
#define u_vpn u_pvpn.xyz
#define u_origin u_porigin.xyz
#define u_ctime u_porigin.w
#define u_viewproj u_viewprojm
#define u_alpha_threshold u_draw0.x
#define u_force_opaque_alpha u_draw0.y
#define u_alias_caustics u_draw0.zw
#define u_soft_params u_draw1.xyz
#define u_alias_model u_modelm
#define u_pose_base u_drawi.x
#define u_inst_base u_drawi.y
#define u_poseverttype u_drawi.z
