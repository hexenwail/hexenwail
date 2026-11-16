#version 300 es
precision highp float;
precision highp int;
uniform highp usampler2D u_indexed;
uniform highp sampler2D u_palette;
uniform vec2 u_srcsize;
uniform int u_smooth;
in vec2 v_uv;
out vec4 frag_color;
vec3 lookup(ivec2 t){
 ivec2 c = clamp(t, ivec2(0), ivec2(u_srcsize) - 1);
 uint idx = texelFetch(u_indexed, c, 0).r;
 return texelFetch(u_palette, ivec2(int(idx), 0), 0).rgb;
}
void main(){
 vec2 pixel = v_uv * u_srcsize;
 if (u_smooth == 0){
  frag_color = vec4(lookup(ivec2(floor(pixel))), 1.0);
  return;
 }
	vec2 base = floor(pixel - 0.5);
	vec2 frac = pixel - 0.5 - base;
	vec2 scale = max(vec2(1.0), 1.0 / max(fwidth(pixel), vec2(1e-5)));
	frac = clamp(0.5 + (frac - 0.5) * scale, 0.0, 1.0);
	ivec2 t = ivec2(base);
	vec3 a = mix(lookup(t), lookup(t + ivec2(1, 0)), frac.x);
	vec3 b = mix(lookup(t + ivec2(0, 1)), lookup(t + ivec2(1, 1)), frac.x);
	frag_color = vec4(mix(a, b, frac.y), 1.0);
}
