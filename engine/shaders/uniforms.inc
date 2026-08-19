layout(std140) uniform PerFrame {
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
layout(std140) uniform PerDraw {
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
