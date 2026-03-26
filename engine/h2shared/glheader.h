/* glheader.h: opengl system includes */

#ifndef __GLHEADER_H
#define __GLHEADER_H

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#include <emscripten.h>

#elif defined(PLATFORM_WINDOWS)
#include <windows.h>
#include <GL/gl.h>

#elif defined(PLATFORM_OSX)
#include <OpenGL/gl.h>

#elif defined(PLATFORM_MAC)
#include <gl.h>

#elif defined(__MORPHOS__)
#include <proto/tinygl.h>
#include <tgl/gl.h>

#elif defined(__AROS__) /* ABIv0, AROSMesa */
#include <GL/gl.h>

#elif defined(__amigaos4__)
#include <GL/gl.h>

#else	/* other unix */
#include <GL/gl.h>
#endif

#ifndef APIENTRY
#define	APIENTRY
#endif

/* GL types that may be missing from older/minimal GL headers (e.g. MinGW) */
#ifndef GL_VERSION_1_5
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#endif

/* include our function pointers */
#include "gl_func.h"

/* VBO/VAO constants (GL 1.5+ / GL 2.0+) */
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER				0x8892
#define GL_ELEMENT_ARRAY_BUFFER			0x8893
#define GL_STREAM_DRAW				0x88E0
#define GL_STATIC_DRAW				0x88E4
#define GL_DYNAMIC_DRAW				0x88E8
#endif

#ifndef	GL_TEXTURE0_ARB
#define	GL_TEXTURE0_ARB				0x84C0
#define	GL_TEXTURE1_ARB				0x84C1
#define	GL_TEXTURE2_ARB				0x84C2
#define	GL_TEXTURE3_ARB				0x84C3
#define	GL_TEXTURE4_ARB				0x84C4
#define	GL_TEXTURE5_ARB				0x84C5

#define	GL_ACTIVE_TEXTURE_ARB			0x84E0
#define	GL_CLIENT_ACTIVE_TEXTURE_ARB		0x84E1
#define	GL_MAX_TEXTURE_UNITS_ARB		0x84E2
#endif

#ifndef	GL_MULTISAMPLE_ARB
#define	GL_MULTISAMPLE_ARB			0x809D
#endif

#ifndef	GL_SAMPLE_ALPHA_TO_COVERAGE
#define	GL_SAMPLE_ALPHA_TO_COVERAGE		0x809E
#endif

#ifndef	GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define	GL_TEXTURE_MAX_ANISOTROPY_EXT		0x84FE
#define	GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT	0x84FF
#endif

/* FBO (framebuffer object) enums */
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER				0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER			0x8CA8
#define GL_DRAW_FRAMEBUFFER			0x8CA9
#endif
#ifndef GL_RGBA8
#define GL_RGBA8				0x8058
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER				0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0			0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT			0x8CE1
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE			0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24			0x81A6
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER			0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER			0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS			0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS				0x8B82
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE			0x812F
#endif
#ifndef GL_TEXTURE_3D
#define GL_TEXTURE_3D				0x806F
#endif
#ifndef GL_TEXTURE_WRAP_R
#define GL_TEXTURE_WRAP_R			0x8072
#endif
#ifndef GL_R8
#define GL_R8					0x8229
#endif
#ifndef GL_RED
#define GL_RED					0x1903
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH			0x8B84
#endif

/* Integer texture formats (OpenGL 3.0 / ES 3.0) */
#ifndef GL_R32UI
#define GL_R32UI				0x8236
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER				0x8D94
#endif

/* UBO (Uniform Buffer Object) — OpenGL 3.1 / ES 3.0 */
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER			0x8A11
#endif
#ifndef GL_INVALID_INDEX
#define GL_INVALID_INDEX			0xFFFFFFFFu
#endif

/* SSBO (Shader Storage Buffer Object) — OpenGL 4.3 */
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER		0x90D2
#endif

#endif	/* __GLHEADER_H */
