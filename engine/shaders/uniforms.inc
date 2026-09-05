/* uniforms.inc -- the two std140 uniform blocks every file-backed draw program
 * reads, plus the scalar aliases that let shader BODIES keep the old loose
 * uniform names.
 *
 * A shader includes layout.inc, then defines exactly one of UNIFORMS_VERT or
 * UNIFORMS_FRAG, then includes this file.  (Spelling either include line out
 * in full here would be a build failure: EmbedShaders.cmake resolves includes
 * by plain text substitution and does not know what a comment is, so a literal
 * include directive inside one recurses until it hits the pass limit.)
 *
 * SPLIT BY STAGE, not by update frequency.  SDL_GPU pushes vertex and fragment
 * uniforms through separate calls into separate descriptor sets
 * (SDL_PushGPUVertexUniformData / SDL_PushGPUFragmentUniformData, sets 1 and
 * 3), so the stage boundary is a hard constraint on the second backend where
 * frequency is only an optimization.  That is also why a stage must declare
 * only its own block: a fragment shader carrying a set-1 block would be a
 * fragment stage reading the vertex stage's descriptor set, which SDL_GPU has
 * no way to bind.  A frequency split can still be added later by halving
 * either block, and shader bodies would not notice.
 *
 * EVERY MEMBER IS vec4, mat4 OR ivec4.  Not tidiness: std140's padding rules
 * are where a change like this goes silently wrong -- a lone float followed by
 * a vec3 does not land where a naive C struct puts it, and the failure mode is
 * garbage uniforms with a clean compile, which no gate here can see.  A layout
 * built only from 16-byte-aligned, 16-byte-multiple types has no padding to
 * get wrong, so the C mirrors in gl_uniforms.h are exact by construction.
 * gl_uniforms.c still asks the driver for GL_UNIFORM_BLOCK_DATA_SIZE once and
 * says so loudly if it disagrees.
 *
 * The aliases at the end of each block are the reason this is tractable across
 * 35 shaders: only the declarations at the top of a stage change, the logic is
 * untouched.  Chained swizzles (u_fcaustics.xy.x) are legal GLSL, so an alias
 * naming a vec2 or vec3 can still be swizzled at the use site.
 *
 * Eye position and time appear in BOTH blocks.  They are the only values both
 * stages want, and a stage cannot reach across to the other's set; C writes
 * both copies from one shadow, so they cannot drift.
 */

#ifdef UNIFORMS_VERT
VERT_UBO(0) uniform VertParams {
    highp mat4  u_mvpm;         /*   0  projection * view * model */
    highp mat4  u_modelviewm;   /*  64  view * model */
    highp mat4  u_modelm;       /* 128  model only, no view */
    highp mat4  u_viewprojm;    /* 192  instanced alias view-projection */
    highp vec4  u_vscene;       /* 256  xyz eye position, w cl.time */
    highp vec4  u_vpup;         /* 272  xyz particle billboard up */
    highp vec4  u_vpright;      /* 288  xyz particle billboard right */
    highp vec4  u_vvpn;         /* 304  xyz view forward */
    highp vec4  u_vporigin;     /* 320  xyz camera origin, w particle time */
    highp ivec4 u_vposei;       /* 336  x pose base, y instance base, z pose vertex type */
};                              /* 352 bytes */

#define u_mvp           u_mvpm
#define u_modelview     u_modelviewm
#define u_alias_model   u_modelm
#define u_viewproj      u_viewprojm
#define u_eyepos        u_vscene.xyz
#define u_time          u_vscene.w
#define u_pup           u_vpup.xyz
#define u_pright        u_vpright.xyz
#define u_vpn           u_vvpn.xyz
#define u_origin        u_vporigin.xyz
#define u_ctime         u_vporigin.w
#define u_pose_base     u_vposei.x
#define u_inst_base     u_vposei.y
#define u_poseverttype  u_vposei.z
#endif  /* UNIFORMS_VERT */

#ifdef UNIFORMS_FRAG
FRAG_UBO(0) uniform FragParams {
    highp vec4 u_fscene;        /*  0  xyz eye position, w cl.time */
    highp vec4 u_ffogv;         /* 16  xyz fog color, w fog density */
    highp vec4 u_fmask;         /* 32  x alpha threshold, y force-opaque alpha,
                                 *     z lightmap overbright, w bicubic lightmap */
    highp vec4 u_fcaustics;     /* 48  xy world caustics, zw alias caustics */
    highp vec4 u_fsoft;         /* 64  xyz soft-particle params */
    highp vec4 u_fskyfog;       /* 80  rgb sky fog color, a blend */
    highp vec4 u_fsky;          /* 96  xy sky wind uv offset */
    highp vec4 u_fdebug;        /* 112 x r_fullbright, y r_lightmap */
};                              /* 128 bytes */

#define u_eyepos              u_fscene.xyz
#define u_time                u_fscene.w
#define u_fog_color           u_ffogv.xyz
#define u_fog_density         u_ffogv.w
#define u_alpha_threshold     u_fmask.x
#define u_force_opaque_alpha  u_fmask.y
#define u_overbright          u_fmask.z
#define u_lightmap_bicubic    u_fmask.w
#define u_caustics            u_fcaustics.xy
#define u_alias_caustics      u_fcaustics.zw
#define u_soft_params         u_fsoft.xyz
#define u_skyfog              u_fskyfog
#define u_wind                u_fsky.xy
#define u_lightdebug          u_fdebug.xy
#endif  /* UNIFORMS_FRAG */
