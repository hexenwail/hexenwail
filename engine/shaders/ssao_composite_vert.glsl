/* ssao_composite (vertex) -- one full-screen triangle from gl_VertexID, no
 * vertex buffer.  No #version: gl_ssao.c supplies it. */
void main ()
{
	vec2 p = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);
	gl_Position = vec4 (p * 2.0 - 1.0, 0.0, 1.0);
}
