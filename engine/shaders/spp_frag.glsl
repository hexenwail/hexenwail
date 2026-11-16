/* spp_frag -- the post-process chain's final pass: water warp, FXAA, motion
 * blur, bloom composite, HDR tonemap, contrast and gamma, then softemu
 * palettization with bayer16 dither.  Moved from gl_postprocess.c's
 * pp_frag_src (#450).
 *
 * No #version line; gl_postprocess.c passes PP_FRAG_HEADER (desktop 430 core,
 * or ES 3.00 with its precision lines).
 *
 * The fourteen loose uniforms are now the PpParams block (GL binding 9,
 * PP_FRAG_UBO_BINDING), mirrored by pp_params_t in gl_postprocess.c and
 * filled from its shadow before each draw.  The #defines below keep the old
 * names, so the body is unchanged.
 */
#include "layout.inc"

FRAG_UBO(0) uniform PpParams {
    vec4  pp_p0;	/* gamma, contrast, dither, lutmax */
    vec4  pp_p1;	/* scale, waterwarp, time, fxaa_on */
    vec4  pp_p2;	/* rcpFrame.xy, viewdelta.xy */
    vec4  pp_p3;	/* motionblur, hdr_exposure, u_bloom_strength, unused */
    ivec4 pp_i0;	/* softemu */
};
#define gamma		pp_p0.x
#define contrast	pp_p0.y
#define dither		pp_p0.z
#define lutmax		pp_p0.w	/* grid size - 1 of the bound LUT (#80) */
#define scale		pp_p1.x
#define waterwarp	pp_p1.y
#define time		pp_p1.z
#define fxaa_on		pp_p1.w
#define rcpFrame	pp_p2.xy
#define viewdelta	pp_p2.zw
#define motionblur	pp_p3.x
#define hdr_exposure	pp_p3.y
#define u_bloom_strength pp_p3.z
#define softemu		pp_i0.x

FRAG_TEX(0) uniform sampler2D scene;
FRAG_TEX(1) uniform sampler3D paletteLUT;
FRAG_TEX(2) uniform sampler2D u_bloom;
FRAG_IN(0) in vec2 v_texcoord;
FRAG_OUT(0) out vec4 fragColor;

vec4 fxaa(sampler2D tex, vec2 uv, vec2 rcp) {
    #define LM(c) ((c).g)
    vec3 rN=texture(tex,uv+vec2(0,-1)*rcp).rgb,rS=texture(tex,uv+vec2(0,1)*rcp).rgb;
    vec3 rE=texture(tex,uv+vec2(1,0)*rcp).rgb,rW=texture(tex,uv+vec2(-1,0)*rcp).rgb;
    vec3 rM=texture(tex,uv).rgb;
    float lN=LM(rN),lS=LM(rS),lE=LM(rE),lW=LM(rW),lM=LM(rM);
    float mn=min(lM,min(min(lN,lS),min(lW,lE)));
    float mx=max(lM,max(max(lN,lS),max(lW,lE)));
    float rng=mx-mn;
    if(rng<max(0.0625,mx*0.166)) return vec4(rM,1.0);
    float lNW=LM(texture(tex,uv+vec2(-1,-1)*rcp).rgb);
    float lNE=LM(texture(tex,uv+vec2(1,-1)*rcp).rgb);
    float lSW=LM(texture(tex,uv+vec2(-1,1)*rcp).rgb);
    float lSE=LM(texture(tex,uv+vec2(1,1)*rcp).rgb);
    float eH=abs(lNW+lNE-2.0*lN)+abs(lW+lE-2.0*lM)*2.0+abs(lSW+lSE-2.0*lS);
    float eV=abs(lNW+lSW-2.0*lW)+abs(lN+lS-2.0*lM)*2.0+abs(lNE+lSE-2.0*lE);
    bool hz=(eH>=eV);
    float l1=hz?lS:lE,l2=hz?lN:lW;
    float st=hz?rcp.y:rcp.x;
    if(abs(l1-lM)<abs(l2-lM))st=-st;
    float avg=(lN+lS+lE+lW)*0.25;
    float sp=clamp(abs(avg-lM)/rng,0.0,0.75);
    vec2 p=uv;if(hz)p.y+=st*0.5;else p.x+=st*0.5;
    return vec4(mix(texture(tex,p).rgb,rM,1.0-sp),1.0);
    #undef LM
}

#ifdef GL_ES
/* bitfieldReverse is GLSL 4.00 / ES 3.10, so the ES 3.00 tier gets this.
 * Only bayer16 uses it, on a value whose meaningful bits are the low 16, but
 * a full 32-bit reversal is what the desktop builtin does and what the
 * following ">> 24" expects.  Selected by the compiler's own GL_ES rather
 * than spliced in from C (it was PP_BITFIELD_REVERSE). */
uint bitfieldReverse(uint v) {
    v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);
    v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v >> 4) & 0x0F0F0F0Fu);
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    return (v << 16) | (v >> 16);
}
#endif
float bayer16(ivec2 c) {
    c &= 15;
    c.y ^= c.x;
    uint v = uint(c.y | (c.x << 8));
    v = (v ^ (v << 2)) & 0x3333u;
    v = (v ^ (v << 1)) & 0x5555u;
    v |= v >> 7;
    v = bitfieldReverse(v) >> 24;
    return float(v) / 256.0 - 0.5;
}

void main() {
    vec2 uv = v_texcoord;
    if (waterwarp > 0.0) {
        uv.x += sin(uv.y * 10.0 + time * 1.5) * 0.015 * waterwarp;
        uv.y += sin(uv.x * 10.0 + time * 2.0) * 0.015 * waterwarp;
    }
    vec4 color = (fxaa_on > 0.0) ? fxaa(scene, uv, rcpFrame) : texture(scene, uv);
    if (motionblur > 0.0) {
        vec2 vel = viewdelta * motionblur;
        color.rgb = color.rgb * 0.4;
        color.rgb += texture(scene, uv + vel * 0.25).rgb * 0.2;
        color.rgb += texture(scene, uv + vel * 0.50).rgb * 0.15;
        color.rgb += texture(scene, uv + vel * 0.75).rgb * 0.125;
        color.rgb += texture(scene, uv + vel * 1.00).rgb * 0.125;
    }
    if (u_bloom_strength > 0.0) {
        color.rgb += texture(u_bloom, uv).rgb * u_bloom_strength;
    }
    /* HDR tonemapping (ACES filmic) */
    if (hdr_exposure > 0.0) {
        color.rgb *= hdr_exposure;
        vec3 x = color.rgb;
        color.rgb = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
    }
    /* Ironwail's (and QuakeSpasm's, MarkV's) contrast: a multiplier, 1..2,
     * that brightens.  This was a stretch about mid-grey, which a dark
     * scene -- most of Hexen II -- read as darkening: contrast 1.5 took
     * demo1's mean from 0.107 to 0.018.  PP_Contrast clamps.  GitHub #112
     * follow-up. */
    color.rgb *= contrast;
    if (gamma != 1.0)
        color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(gamma));
    color.rgb = clamp(color.rgb, 0.0, 1.0);

    if (softemu > 0) {
        vec3 c = color.rgb;
        if (softemu == 1) {
            float d = bayer16(ivec2(gl_FragCoord.xy * scale));
            c += d * dither / 16.0;
        }
        ivec3 idx = ivec3(clamp(c, 0.0, 1.0) * lutmax + 0.5);
        color.rgb = texelFetch(paletteLUT, idx, 0).rgb;
    }

    fragColor = color;
}
