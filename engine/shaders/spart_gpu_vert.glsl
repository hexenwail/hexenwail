#include "uniforms.inc"

struct GpuParticle {
    vec4 pos_die;  /* xyz=position, w=die_time */
    vec4 color;    /* rgba, pre-converted from palette */
};

layout(std430, binding = 0) readonly buffer GpuParticleBuffer {
    GpuParticle particles[];
};


out vec2 v_texcoord;
out vec4 v_color;
out float v_fogdist;

/* Default particle texcoords (ptex_coord[0] from r_part.c) */
const vec2 ptc[3] = vec2[3](
    vec2(1.0, 0.0),
    vec2(1.0, 0.5),
    vec2(0.5, 0.0)
);

void main() {
    int pidx  = gl_VertexID / 3;
    int corner = gl_VertexID % 3;
    GpuParticle p = particles[pidx];

    /* Dead particle -> degenerate (zero-area) triangle */
    if (p.pos_die.w < u_ctime) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        v_color     = vec4(0.0);
        v_texcoord  = vec2(0.0);
        v_fogdist   = 0.0;
        return;
    }

    vec3 base = p.pos_die.xyz;

    /* Distance-based scale (mirrors the CPU hack in r_part.c) */
    float depth = dot(base - u_origin, u_vpn);
    float scale = (depth < 20.0) ? 1.0 : 1.0 + depth * 0.004;

    vec3 pos;
    if (corner == 0)      pos = base;
    else if (corner == 1) pos = base + u_pup    * scale;
    else                  pos = base + u_pright * scale;

    v_texcoord = ptc[corner];
    v_color    = p.color;

    vec4 eyepos = u_modelview * vec4(pos, 1.0);
    v_fogdist   = length(eyepos.xyz);
    gl_Position = u_mvp * vec4(pos, 1.0);
}
