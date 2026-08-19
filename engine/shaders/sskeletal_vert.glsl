#include "uniforms.inc"
layout(location=0) in vec3 a_position;
layout(location=1) in vec4 a_normal;
layout(location=2) in vec2 a_texcoord;
layout(location=3) in vec4 a_weights;
layout(location=4) in uvec4 a_indices;

layout(std430, binding=4) restrict readonly buffer BoneBuffer {
    mat3x4 bones[];
};


out vec2 v_texcoord;
out vec4 v_color;
out float v_fogdist;
out vec2 v_worldxy;
invariant gl_Position;

void main() {
    mat3x4 blend = bones[u_pose_base + int(a_indices.x)] * a_weights.x;
    if (a_weights.y > 0.01) blend += bones[u_pose_base + int(a_indices.y)] * a_weights.y;
    if (a_weights.z > 0.01) blend += bones[u_pose_base + int(a_indices.z)] * a_weights.z;
    if (a_weights.w > 0.01) blend += bones[u_pose_base + int(a_indices.w)] * a_weights.w;

    mat4x3 mat_3x4 = transpose(blend);
    vec3 skinned_pos = mat_3x4 * vec4(a_position, 1.0);
    vec3 skinned_normal = normalize(mat_3x4 * vec4(a_normal.xyz, 0.0));

    v_texcoord = a_texcoord;
    v_color = vec4(skinned_normal * 0.5 + 0.5, 1.0);
    v_worldxy = (u_alias_model * vec4(skinned_pos, 1.0)).xy;
    vec4 eyepos = u_modelview * vec4(skinned_pos, 1.0);
    v_fogdist = length(eyepos.xyz);
    gl_Position = u_mvp * vec4(skinned_pos, 1.0);
}
