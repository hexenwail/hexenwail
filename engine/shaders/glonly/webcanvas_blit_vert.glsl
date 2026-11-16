#version 300 es
out vec2 v_uv;
void main(){
 vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
 v_uv = vec2(p.x, 1.0 - p.y);
 gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
