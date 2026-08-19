#include "uniforms.inc"

struct InstanceData {
    vec4 WorldMatrix0;
    vec4 WorldMatrix1;
    vec4 WorldMatrix2;
    vec4 LightAlpha;
    int Pose0;
    int Pose1;
    float Blend;
    int ShadedotRow;
};

layout(std430, binding=0) restrict readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(std430, binding=1) restrict readonly buffer PoseBuffer {
    uint pose_data[];
};

layout(std430, binding=2) restrict readonly buffer ShadeDots {
    float shadedots[4096];
};

layout(std430, binding=3) restrict readonly buffer MD3PoseBuffer {
    uvec2 md3_pose_data[];
};

in vec2 a_texcoord;


out vec2 v_texcoord;
out vec4 v_color;
out float v_fogdist;
out vec2 v_worldxy;

invariant gl_Position;

vec3 MD3_DecodePosition(uvec2 vdata) {
    int x = int((vdata.x) & 0xFFFFu);
    int y = int((vdata.x >> 16) & 0xFFFFu);
    int z = int((vdata.y) & 0xFFFFu);
    if (x > 32767) x -= 65536;
    if (y > 32767) y -= 65536;
    if (z > 32767) z -= 65536;
    return vec3(x, y, z) / 64.0;
}

vec3 MD3_DecodeNormal(uvec2 vdata) {
    uint latbyte = (vdata.y >> 16) & 0xFFu;
    uint lngbyte = (vdata.y >> 24) & 0xFFu;
    float lat = float(latbyte) * (3.14159265 / 128.0);
    float lng = float(lngbyte) * (3.14159265 / 128.0);
    float sinlng = sin(lng);
    return vec3(sinlng * cos(lat), sinlng * sin(lat), cos(lng));
}

void main() {
    InstanceData inst = instances[u_inst_base + gl_InstanceID];
    vec3 local_pos, normal;
    uint ni;

    if (u_poseverttype == 1) {
        uvec2 p0 = md3_pose_data[inst.Pose0 + gl_VertexID];
        uvec2 p1 = md3_pose_data[inst.Pose1 + gl_VertexID];
        vec3 v0 = MD3_DecodePosition(p0);
        vec3 v1 = MD3_DecodePosition(p1);
        vec3 n0 = MD3_DecodeNormal(p0);
        vec3 n1 = MD3_DecodeNormal(p1);
        local_pos = mix(v1, v0, inst.Blend);
        normal = normalize(mix(n1, n0, inst.Blend));
        ni = uint(0);
    } else {
        uint p0 = pose_data[inst.Pose0 + gl_VertexID];
        uint p1 = pose_data[inst.Pose1 + gl_VertexID];
        vec3 v0 = vec3(float(p0 & 0xFFu), float((p0>>8)&0xFFu), float((p0>>16)&0xFFu));
        vec3 v1 = vec3(float(p1 & 0xFFu), float((p1>>8)&0xFFu), float((p1>>16)&0xFFu));
        ni = (p0 >> 24) & 0xFFu;
        local_pos = mix(v1, v0, inst.Blend);
    }

    mat4x3 world = transpose(mat3x4(
        inst.WorldMatrix0, inst.WorldMatrix1, inst.WorldMatrix2));
    vec3 world_pos = world * vec4(local_pos, 1.0);

    float sdot;
    if (u_poseverttype == 1) {
        vec3 world_normal = normalize((mat3(inst.WorldMatrix0.xyz, inst.WorldMatrix1.xyz, inst.WorldMatrix2.xyz)) * normal);
        sdot = max(world_normal.z, 0.0);
    } else {
        int sdot_idx = inst.ShadedotRow * 256 + int(ni);
        sdot = shadedots[sdot_idx];
        sdot = max(sdot, 0.0);
    }
    v_color = vec4(inst.LightAlpha.rgb * sdot, inst.LightAlpha.a);

    v_texcoord = a_texcoord;
    v_worldxy = world_pos.xy;
    v_fogdist = distance(world_pos, u_eyepos);
    gl_Position = u_viewproj * vec4(world_pos, 1.0);
}
