/* salias_vert -- the alias (model) vertex stage: vertex-coloured, textured,
 * fogged.  Also drives the sprite, mesh, warp and brush-poly batches that
 * share gl_shader_alias.  Moved from gl_shader.c's salias_vert (#450, slice S1
 * of the alias-family plan on that issue).
 *
 * No #version line; gl_shader.c passes GLSL_VERT_HEADER, with
 * "#define NOPERSP 1" appended for the affine (r_softemu) twin.
 *
 * invariant gl_Position pins the position math across draws so the additive
 * fullbright re-draw lands on the exact depth the base pass wrote.  Without
 * it the GLSL compiler may reorder the matrix multiply and produce slightly
 * different Z, so fullbright fragments fail the GL_LEQUAL/GL_GEQUAL test
 * polygon-by-polygon and the model develops blocky chunks (uhexen2-iir3).
 * Mesa sometimes ignores the qualifier; the fullbright pass also enables
 * polygon offset as a backstop.
 *
 * u_mvp, u_modelview, u_alias_model and u_alias_dlight were loose uniforms;
 * they are VertParams members now (uniforms.inc), fed per draw from the same
 * imm_* globals by GL_ImmPushBlocks.  The varyings must match salias_frag.glsl
 * by name (the plain arm of VERT_OUT emits no location) and, for SPIR-V, by
 * location.
 */
#include "layout.inc"
#define UNIFORMS_VERT
#include "uniforms.inc"

/* Affine texture mapping, the way the software rasteriser did it
 * (uhexen2-ktjv).  An interpolation qualifier is not something a uniform can
 * switch, so this is a compile-time variant -- the only stage of r_softemu
 * that needs one.  Empty object-like macro when NOPERSP is not defined, so
 * the qualifier simply is not there.  Desktop only: `noperspective` is a
 * reserved word in GLSL ES 3.00, and the ES tier never defines NOPERSP. */
#ifdef NOPERSP
#define TCQUAL noperspective
#else
#define TCQUAL
#endif

VERT_IN(0) in vec3 a_position;
VERT_IN(1) in vec2 a_texcoord;
VERT_IN(3) in vec4 a_color;
/* u_alias_model: model-only matrix (entity rotation + scale_origin + scale,
 * no view).  u_modelview is view*model, so it lands vertices in EYE space --
 * sampling caustics there would make the pattern swim with the camera.  C
 * uploads the same transform chain R_DrawAliasModel pushes onto the matrix
 * stack, rebuilt on an identity base.  Identity at rest, so the sprite / warp
 * / brush-poly batches that share this program (which submit world-space
 * positions already) stay correct.  uhexen2-0gn3. */
VERT_OUT(0) TCQUAL out vec2 v_texcoord;
VERT_OUT(1) out vec4 v_color;
VERT_OUT(2) out float v_fogdist;
VERT_OUT(3) out vec2 v_worldxy;
/* EYE space -- u_modelview is view*model, so whatever entity transform
 * preceded it, this lands camera-relative with the camera at the origin.
 * u_alias_lightdir is uploaded in the matching frame.  uhexen2-4kcb. */
VERT_OUT(4) out highp vec3 v_matpos;
#ifndef GL_ES
/* Clustered dynamic lighting on alias models (uhexen2-waum), desktop only:
 * the ES tier has no SSBO and no compute pass to fill the grid.  This was
 * the GLSL_ALIAS_DLIGHT_VS_OUT / _VS_CALC splice; GL_ES, which the ES
 * compiler predefines, now makes the same choice. */
VERT_OUT(5) out highp vec3 v_eyepos;
VERT_OUT(6) flat out float v_dlightscale;
#endif
invariant gl_Position;
void main() {
    v_texcoord = a_texcoord;
    v_color = a_color;
    v_worldxy = (u_alias_model * vec4(a_position, 1.0)).xy;
    vec4 eyepos = u_modelview * vec4(a_position, 1.0);
    v_fogdist = length(eyepos.xyz);
    v_matpos = eyepos.xyz;
#ifndef GL_ES
    v_eyepos = eyepos.xyz;
    v_dlightscale = u_alias_dlight;
#endif
    gl_Position = u_mvp * vec4(a_position, 1.0);
    VK_FLIP_Y()
}
