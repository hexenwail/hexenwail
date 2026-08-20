/*
 * web_canvas_gl2.c -- WebGL2 backend for the accelerated presentation canvas.
 *
 * Uploads the software renderer's palettized framebuffer as an R8UI texture
 * (one byte per pixel -- the smallest possible per-frame CPU->GPU transfer)
 * and expands it through a 256 entry RGBA8 palette LUT in the fragment
 * shader.  Palette shifts (damage, bonus, underwater) only re-upload the
 * 256x1 LUT, so they are free per pixel.
 *
 * Copyright (C) 2026  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "web_canvas.h"

#include <GLES3/gl3.h>

/* Unlike alextnewman/hexenwail 0b1906add, this backend does not create its
 * own WebGL2 context: vid_soft_web.c already owns an SDL3 window with an ES
 * 3.0 context, and a canvas can only ever hand out one.  Everything below
 * runs on whatever context is current when WebCanvas_Init() is called. */
static qboolean	canvas_ready;
static GLuint indexed_texture;
static GLuint palette_texture;
static GLuint blit_program;
static GLuint blit_vao;
static GLint u_indexed, u_palette, u_srcsize, u_smooth;
static int src_width, src_height;
static int filter_smooth;
static byte palette_rgba[256 * 4];

/* Fullscreen triangle generated from gl_VertexID: no vertex buffer, no
 * attribute fetch.  v_uv is flipped vertically because the software
 * framebuffer is stored top-down while GL clip space is bottom-up. */
static const char *blit_vertex_source =
	"#version 300 es\n"
	"out vec2 v_uv;\n"
	"void main(){\n"
	" vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
	" v_uv = vec2(p.x, 1.0 - p.y);\n"
	" gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char *blit_fragment_source =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform highp usampler2D u_indexed;\n"
	"uniform highp sampler2D u_palette;\n"
	"uniform vec2 u_srcsize;\n"
	"uniform int u_smooth;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag_color;\n"
	"vec3 lookup(ivec2 t){\n"
	" ivec2 c = clamp(t, ivec2(0), ivec2(u_srcsize) - 1);\n"
	" uint idx = texelFetch(u_indexed, c, 0).r;\n"
	" return texelFetch(u_palette, ivec2(int(idx), 0), 0).rgb;\n"
	"}\n"
	"void main(){\n"
	" vec2 pixel = v_uv * u_srcsize;\n"
	" if (u_smooth == 0){\n"
	"  frag_color = vec4(lookup(ivec2(floor(pixel))), 1.0);\n"
	"  return;\n"
	" }\n"
	/* Pixel-art antialiasing: a bilinear tap whose weights are sharpened by
	 * the on-screen size of one source texel.  Collapses to exact nearest
	 * neighbour at integer scale and only softens the seams otherwise. */
	"	vec2 base = floor(pixel - 0.5);\n"
	"	vec2 frac = pixel - 0.5 - base;\n"
	"	vec2 scale = max(vec2(1.0), 1.0 / max(fwidth(pixel), vec2(1e-5)));\n"
	"	frac = clamp(0.5 + (frac - 0.5) * scale, 0.0, 1.0);\n"
	"	ivec2 t = ivec2(base);\n"
	"	vec3 a = mix(lookup(t), lookup(t + ivec2(1, 0)), frac.x);\n"
	"	vec3 b = mix(lookup(t + ivec2(0, 1)), lookup(t + ivec2(1, 1)), frac.x);\n"
	"	frag_color = vec4(mix(a, b, frac.y), 1.0);\n"
	"}\n";

static GLuint WebCanvas_CompileShader (GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		Sys_Error("web canvas shader: %s", log);
	}
	return shader;
}

static void WebCanvas_InitProgram (void)
{
	GLuint vertex = WebCanvas_CompileShader(GL_VERTEX_SHADER, blit_vertex_source);
	GLuint fragment = WebCanvas_CompileShader(GL_FRAGMENT_SHADER, blit_fragment_source);
	GLint ok;

	blit_program = glCreateProgram();
	glAttachShader(blit_program, vertex);
	glAttachShader(blit_program, fragment);
	glLinkProgram(blit_program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	glGetProgramiv(blit_program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetProgramInfoLog(blit_program, sizeof(log), NULL, log);
		Sys_Error("web canvas program: %s", log);
	}

	u_indexed = glGetUniformLocation(blit_program, "u_indexed");
	u_palette = glGetUniformLocation(blit_program, "u_palette");
	u_srcsize = glGetUniformLocation(blit_program, "u_srcsize");
	u_smooth = glGetUniformLocation(blit_program, "u_smooth");

	glGenVertexArrays(1, &blit_vao);
}

void WebCanvas_Init (void)
{
	canvas_ready = true;

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	WebCanvas_InitProgram();

	glGenTextures(1, &indexed_texture);
	glBindTexture(GL_TEXTURE_2D, indexed_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &palette_texture);
	glBindTexture(GL_TEXTURE_2D, palette_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, palette_rgba);
}

void WebCanvas_Shutdown (void)
{
	if (!canvas_ready)
		return;
	glDeleteTextures(1, &indexed_texture);
	glDeleteTextures(1, &palette_texture);
	glDeleteProgram(blit_program);
	glDeleteVertexArrays(1, &blit_vao);
	indexed_texture = palette_texture = blit_program = blit_vao = 0;
	canvas_ready = false;
	src_width = src_height = 0;
}

const char *WebCanvas_BackendName (void)
{
	return "webgl2";
}

void WebCanvas_SetSource (int width, int height)
{
	if (width == src_width && height == src_height)
		return;
	src_width = width;
	src_height = height;
	glBindTexture(GL_TEXTURE_2D, indexed_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, width, height, 0,
		GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
}

void WebCanvas_SetPalette (const byte *palette)
{
	int i;

	for (i = 0; i < 256; ++i)
	{
		palette_rgba[i * 4 + 0] = palette[i * 3 + 0];
		palette_rgba[i * 4 + 1] = palette[i * 3 + 1];
		palette_rgba[i * 4 + 2] = palette[i * 3 + 2];
		palette_rgba[i * 4 + 3] = 255;
	}
	if (!palette_texture)
		return;
	glBindTexture(GL_TEXTURE_2D, palette_texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA,
		GL_UNSIGNED_BYTE, palette_rgba);
}

void WebCanvas_SetFilter (qboolean smooth)
{
	filter_smooth = smooth ? 1 : 0;
}

void WebCanvas_Present (const byte *pixels, int rowbytes,
			int canvas_w, int canvas_h,
			int dst_x, int dst_y, int dst_w, int dst_h)
{
	if (!blit_program || src_width <= 0 || src_height <= 0)
		return;

	glBindTexture(GL_TEXTURE_2D, indexed_texture);
	if (rowbytes == src_width)
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_width, src_height,
			GL_RED_INTEGER, GL_UNSIGNED_BYTE, pixels);
	}
	else
	{
		int y;
		for (y = 0; y < src_height; ++y)
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, src_width, 1,
				GL_RED_INTEGER, GL_UNSIGNED_BYTE, pixels + (size_t)y * rowbytes);
	}

	/* Letterbox bars: only cleared when the image does not cover the canvas. */
	if (dst_x > 0 || dst_y > 0 || dst_w < canvas_w || dst_h < canvas_h)
	{
		glViewport(0, 0, canvas_w, canvas_h);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	glViewport(dst_x, dst_y, dst_w, dst_h);
	glUseProgram(blit_program);
	glUniform1i(u_indexed, 0);
	glUniform1i(u_palette, 1);
	glUniform2f(u_srcsize, (float)src_width, (float)src_height);
	glUniform1i(u_smooth, filter_smooth);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, indexed_texture);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, palette_texture);
	glActiveTexture(GL_TEXTURE0);

	glBindVertexArray(blit_vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
}
