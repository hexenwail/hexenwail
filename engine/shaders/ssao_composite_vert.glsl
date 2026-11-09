/* ssao_composite (vertex) -- one full-screen triangle from gl_VertexID, no
 * vertex buffer.  No #version: gl_ssao.c supplies it.
 *
 * Vulkan GLSL has no gl_VertexID; its gl_VertexIndex counts from the draw's
 * firstVertex instead of from 0.  The two agree here because this triangle is
 * always drawn with firstVertex 0 (glDrawArrays(GL_TRIANGLES, 0, 3) today, and
 * the same 0 under SDL_GPU), so aliasing the name is exact rather than close.
 * Without it the -V compile, and with it shaders_spirv, fails (#449). */
#ifdef VULKAN
#define gl_VertexID gl_VertexIndex
#endif

void main ()
{
	vec2 p = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);
	gl_Position = vec4 (p * 2.0 - 1.0, 0.0, 1.0);
}
