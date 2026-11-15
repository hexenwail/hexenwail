/* sworld_vert -- the world vertex stage: textured + lightmapped, fogged.
 * Shared by gl_shader_world, gl_shader_world_opaque and gl_shader_world_oit,
 * which draw world surfaces, brush entities (R_RotateForEntity multiplies the
 * entity transform into u_modelview / u_mvp, never into a_position) and lit
 * water.  Moved from gl_shader.c's sworld_vert (#450, world slice W1).
 *
 * No #version line; gl_shader.c passes GLSL_VERT_HEADER.
 *
 * u_mvp and u_modelview were loose uniforms; they are VertParams members now
 * (uniforms.inc).  A block member is global where a loose uniform was this
 * program's own, so every path that draws with a world program writes both
 * and flushes before its draw: GL_ImmPushBlocks for the GL_ImmEnd batches,
 * and each glDrawElements path in gl_rsurf.c / gl_rmain.c / gl_worldcull.c
 * by hand.  tools/test_world_vert_block.py pins that list.
 *
 * The varyings must match sworld_frag and sworld_frag_opaque, still C
 * literals, by name (the plain arm of VERT_OUT emits no location).  The
 * locations are for the SPIR-V gates, and for the FRAG_IN side once the
 * fragment stages move.
 */
#include "layout.inc"
#define UNIFORMS_VERT
#include "uniforms.inc"

VERT_IN(0) in vec3 a_position;
VERT_IN(1) in vec2 a_texcoord;
VERT_IN(2) in vec2 a_lmcoord;
VERT_IN(3) in vec4 a_color;
/* highp: the material tangent frame differentiates this; see v_eyepos
 * (uhexen2-mfql). */
VERT_OUT(0) out highp vec2 v_texcoord;
/* highp: SpanUV extrapolates it across a cell (#176). */
VERT_OUT(1) out highp vec2 v_lmcoord;
VERT_OUT(2) out vec4 v_color;
VERT_OUT(3) out float v_fogdist;
/* World-space XY for caustics sampling.  For world surfaces a_position IS
 * world space.  For brush ents R_RotateForEntity multiplies into
 * u_modelview, not into a_position, so a_position here is still model-local
 * for brush ents -- caustics will tile with the brush ent's local frame,
 * which is acceptable for moving func_* ents (the effect is subtle and
 * rarely visible mid-motion).  uhexen2-6bfm. */
VERT_OUT(4) out vec2 v_worldxy;
/* Eye-space position, for the material-map tangent frame (uhexen2-mfql).
 * EYE space and not world: the frame, the view vector and the geometric
 * normal all have to live in one space, and eye space is the only one this
 * shader can produce for BOTH world surfaces and brush ents.  For a brush
 * ent the entity transform is inside u_modelview and never reaches
 * a_position -- which is exactly why v_worldxy above is documented as wrong
 * for brush ents -- so anything derived from a_position directly would put
 * a rotating platform's relief in its own local frame.  In eye space the
 * camera sits at the origin, so the view vector is just -normalize(v_eyepos),
 * and no eye-position uniform is needed.
 *
 * highp explicitly.  The ES tier's fragment stage defaults to highp
 * (GLSL_FRAG_HEADER, #151 (a)), but the explicit qualifier stays: it
 * documents that this quantity needs highp, and protects it against any
 * future default change.  mediump carries about 10 bits of mantissa -- fine
 * for a colour, useless for a position that can be thousands of world units
 * from the camera, whose screen-space derivative is then the difference of
 * two badly-rounded large numbers.  That is precisely the quantity the
 * tangent frame is built from, so without this the WebGL2 build gets a frame
 * made of noise.  Desktop GLSL accepts and ignores the qualifier.
 * uhexen2-mfql. */
VERT_OUT(5) out highp vec3 v_eyepos;
/* invariant gl_Position: pin position math so within-shader vertex
 * transforms produce stable depth across draw calls.  Brush ents share this
 * same compiled program (uhexen2-mf45), so the within-shader guarantee covers
 * coplanar joins between brush ents and world surfaces -- no cross-shader
 * 1-ULP drift. */
invariant gl_Position;
void main() {
    v_texcoord = a_texcoord;
    v_lmcoord = a_lmcoord;
    v_color = a_color;
    v_worldxy = a_position.xy;
    vec4 eyepos = u_modelview * vec4(a_position, 1.0);
    v_eyepos = eyepos.xyz;
    v_fogdist = length(eyepos.xyz);
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
