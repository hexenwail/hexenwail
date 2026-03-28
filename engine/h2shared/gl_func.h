/*
 * gl_func.h -- opengl function pointers
 * make sure NOT to protect this file against multiple inclusions!
 *
 * Copyright (C) 2001 contributors of the Anvil of Thyrion project
 * Copyright (C) 2005-2016  O.Sezer <sezero@users.sourceforge.net>
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

/* whether to dlsym gl function calls:
 * the define GL_DLSYM is decided in the Makefile */

#ifndef __GL_FUNC_EXTERN
#define __GL_FUNC_EXTERN extern
#endif

/* core gl functions
 */
#if defined(GL_DLSYM)

#ifndef GL_FUNCTION
#define GL_FUNCTION(ret, func, params) \
typedef ret (APIENTRY *func##_f) params; \
__GL_FUNC_EXTERN func##_f func##_fp;
#endif

GL_FUNCTION(void, glBindTexture, (GLenum,GLuint))
GL_FUNCTION(void, glDeleteTextures, (GLsizei,const GLuint *))
GL_FUNCTION(void, glGenTextures, (GLsizei,GLuint *))
GL_FUNCTION(void, glTexParameterf, (GLenum,GLenum,GLfloat))
GL_FUNCTION(void, glTexEnvf, (GLenum,GLenum,GLfloat))
GL_FUNCTION(void, glScalef, (GLfloat,GLfloat,GLfloat))
GL_FUNCTION(void, glTexImage2D, (GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const GLvoid*))
GL_FUNCTION(void, glTexSubImage2D, (GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const GLvoid *))

GL_FUNCTION(void, glBegin, (GLenum))
GL_FUNCTION(void, glEnd, (void))
GL_FUNCTION(void, glEnable, (GLenum))
GL_FUNCTION(void, glDisable, (GLenum))
GL_FUNCTION(GLboolean, glIsEnabled, (GLenum))

GL_FUNCTION(void, glFinish, (void))
GL_FUNCTION(void, glFlush, (void))
GL_FUNCTION(void, glClear, (GLbitfield))

GL_FUNCTION(void, glVertex2f, (GLfloat,GLfloat))
GL_FUNCTION(void, glVertex3f, (GLfloat,GLfloat,GLfloat))
GL_FUNCTION(void, glVertex3fv, (const GLfloat *))
GL_FUNCTION(void, glTexCoord2f, (GLfloat,GLfloat))
GL_FUNCTION(void, glTexCoord2fv, (const GLfloat *))
GL_FUNCTION(void, glColor4f, (GLfloat,GLfloat,GLfloat,GLfloat))
GL_FUNCTION(void, glColor4fv, (const GLfloat *))
GL_FUNCTION(void, glColor4ub, (GLubyte,GLubyte,GLubyte,GLubyte))
GL_FUNCTION(void, glColor4ubv, (const GLubyte *))
GL_FUNCTION(void, glColor3ubv, (const GLubyte *))
GL_FUNCTION(void, glColor3f, (GLfloat,GLfloat,GLfloat))
GL_FUNCTION(void, glColor3fv, (const GLfloat *))
GL_FUNCTION(void, glClearColor, (GLclampf,GLclampf,GLclampf,GLclampf))
GL_FUNCTION(void, glFogf, (GLenum,GLfloat))
GL_FUNCTION(void, glFogfv, (GLenum,const GLfloat *))
GL_FUNCTION(void, glFogi, (GLenum,GLint))

GL_FUNCTION(void, glAlphaFunc, (GLenum,GLclampf))
GL_FUNCTION(void, glBlendFunc, (GLenum,GLenum))
GL_FUNCTION(void, glShadeModel, (GLenum))
GL_FUNCTION(void, glPolygonMode, (GLenum,GLenum))
GL_FUNCTION(void, glDepthMask, (GLboolean))
GL_FUNCTION(void, glDepthRange, (GLclampd,GLclampd))
GL_FUNCTION(void, glDepthFunc, (GLenum))

#if defined(DRAW_PROGRESSBARS) /* D_ShowLoadingSize() */
GL_FUNCTION(void, glDrawBuffer, (GLenum))
#endif
GL_FUNCTION(void, glReadPixels, (GLint,GLint,GLsizei,GLsizei,GLenum,GLenum, GLvoid *))
GL_FUNCTION(void, glPixelStorei, (GLenum,GLint))
GL_FUNCTION(void, glHint, (GLenum,GLenum))
GL_FUNCTION(void, glCullFace, (GLenum))

GL_FUNCTION(void, glRotatef, (GLfloat,GLfloat,GLfloat,GLfloat))
GL_FUNCTION(void, glTranslatef, (GLfloat,GLfloat,GLfloat))

GL_FUNCTION(void, glOrtho, (GLdouble,GLdouble,GLdouble,GLdouble,GLdouble,GLdouble))
GL_FUNCTION(void, glFrustum, (GLdouble,GLdouble,GLdouble,GLdouble,GLdouble,GLdouble))
GL_FUNCTION(void, glViewport, (GLint,GLint,GLsizei,GLsizei))
GL_FUNCTION(void, glPushMatrix, (void))
GL_FUNCTION(void, glPopMatrix, (void))
GL_FUNCTION(void, glLoadIdentity, (void))
GL_FUNCTION(void, glMatrixMode, (GLenum))
GL_FUNCTION(void, glLoadMatrixf, (const GLfloat *))
/*
GL_FUNCTION(void, glPolygonOffset, (GLfloat,GLfloat))
*/

GL_FUNCTION(const GLubyte*, glGetString, (GLenum))
GL_FUNCTION(void, glGetFloatv, (GLenum,GLfloat *))
GL_FUNCTION(void, glGetIntegerv, (GLenum,GLint *))

GL_FUNCTION(void, glStencilFunc, (GLenum,GLint,GLuint))
GL_FUNCTION(void, glStencilOp, (GLenum,GLenum,GLenum))
GL_FUNCTION(void, glClearStencil, (GLint))

#elif defined(__EMSCRIPTEN__)

/* Emscripten / WebGL2: GLES3 functions linked statically.
 * Legacy GL1 functions (glBegin, glVertex, glMatrixMode, etc.)
 * do not exist — the engine uses VBO/shader paths instead.
 * We define stubs for the _fp names that are still referenced. */
#ifndef GL_FUNC_H
#define GL_FUNC_H

/* Core GLES3 functions */
#define glBindTexture_fp	glBindTexture
#define glDeleteTextures_fp	glDeleteTextures
#define glGenTextures_fp	glGenTextures
#define glTexParameterf_fp	glTexParameterf
#define glTexImage2D_fp		glTexImage2D
#define glTexSubImage2D_fp	glTexSubImage2D
#define glCopyTexSubImage2D_fp	glCopyTexSubImage2D
#define glEnable_fp		glEnable
#define glDisable_fp		glDisable
#define glIsEnabled_fp		glIsEnabled
#define glFinish_fp		glFinish
#define glFlush_fp		glFlush
#define glClear_fp		glClear
#define glClearColor_fp		glClearColor
#define glBlendFunc_fp		glBlendFunc
#define glDepthMask_fp		glDepthMask
#define glDepthFunc_fp		glDepthFunc
#define glDepthRange_fp		glDepthRangef	/* ES3 uses float version */
#define glReadPixels_fp		glReadPixels
#define glPixelStorei_fp	glPixelStorei
#define glHint_fp		glHint
#define glCullFace_fp		glCullFace
#define glViewport_fp		glViewport
#define glPolygonOffset_fp	glPolygonOffset
#define glGetString_fp		glGetString
#define glGetFloatv_fp		glGetFloatv
#define glGetIntegerv_fp	glGetIntegerv
#define glStencilFunc_fp	glStencilFunc
#define glStencilOp_fp		glStencilOp
#define glClearStencil_fp	glClearStencil

/* Legacy GL1 functions — not available in GLES3, stub as no-ops.
 * The engine's VBO/shader pipeline doesn't call these at runtime,
 * but some code paths still reference the _fp names. */
#define glBegin_fp(m)		((void)0)
#define glEnd_fp()		((void)0)
#define glVertex2f_fp(x,y)	((void)0)
#define glVertex3f_fp(x,y,z)	((void)0)
#define glVertex3fv_fp(v)	((void)0)
#define glTexCoord2f_fp(s,t)	((void)0)
#define glTexCoord2fv_fp(v)	((void)0)
#define glColor4f_fp(r,g,b,a)	((void)0)
#define glColor4fv_fp(v)	((void)0)
#define glColor4ub_fp(r,g,b,a)	((void)0)
#define glColor4ubv_fp(v)	((void)0)
#define glColor3ubv_fp(v)	((void)0)
#define glColor3f_fp(r,g,b)	((void)0)
#define glColor3fv_fp(v)	((void)0)
#define glTexEnvf_fp(a,b,c)	((void)0)
#define glScalef_fp(x,y,z)	((void)0)
#define glFogf_fp(p,v)		((void)0)
#define glFogfv_fp(p,v)		((void)0)
#define glFogi_fp(p,v)		((void)0)
#define glAlphaFunc_fp(f,r)	((void)0)
#define glShadeModel_fp(m)	((void)0)
#define glPolygonMode_fp(f,m)	((void)0)
#define glDrawBuffer_fp(m)	((void)0)
#define glRotatef_fp(a,x,y,z)	((void)0)
#define glTranslatef_fp(x,y,z)	((void)0)
#define glOrtho_fp(l,r,b,t,n,f)		((void)0)
#define glFrustum_fp(l,r,b,t,n,f)	((void)0)
#define glPushMatrix_fp()	((void)0)
#define glPopMatrix_fp()	((void)0)
#define glLoadIdentity_fp()	((void)0)
#define glMatrixMode_fp(m)	((void)0)
#define glLoadMatrixf_fp(m)	((void)0)
#define glPointSize_fp(s)	((void)0)
#define glPointSize(s)		((void)0)	/* Direct function call, not _fp variant */

#endif	/* GL_FUNC_H */

#else	/* desktop GL without dlsym */

#ifndef GL_FUNC_H
#define GL_FUNC_H

#define glBindTexture_fp	glBindTexture
#define glDeleteTextures_fp	glDeleteTextures
#define glGenTextures_fp	glGenTextures
#define glTexParameterf_fp	glTexParameterf
#define glTexEnvf_fp		glTexEnvf
#define glScalef_fp		glScalef
#define glTexImage2D_fp		glTexImage2D
#define glTexSubImage2D_fp	glTexSubImage2D
#define glCopyTexSubImage2D_fp	glCopyTexSubImage2D

#define glBegin_fp		glBegin
#define glEnd_fp		glEnd
#define glEnable_fp		glEnable
#define glDisable_fp		glDisable
#define glIsEnabled_fp		glIsEnabled
#define glFinish_fp		glFinish
#define glFlush_fp		glFlush
#define glClear_fp		glClear

#define glVertex2f_fp		glVertex2f
#define glVertex3f_fp		glVertex3f
#define glVertex3fv_fp		glVertex3fv
#define glTexCoord2f_fp		glTexCoord2f
#define glTexCoord2fv_fp	glTexCoord2fv
#define glColor4f_fp		glColor4f
#define glColor4fv_fp		glColor4fv
#define glColor4ub_fp		glColor4ub
#define glColor4ubv_fp		glColor4ubv
#define glColor3ubv_fp		glColor3ubv
#define glColor3f_fp		glColor3f
#define glColor3fv_fp		glColor3fv
#define glClearColor_fp		glClearColor
#define glFogf_fp		glFogf
#define glFogfv_fp		glFogfv
#define glFogi_fp		glFogi

#define glAlphaFunc_fp		glAlphaFunc
#define glBlendFunc_fp		glBlendFunc
#define glShadeModel_fp		glShadeModel
#define glPolygonMode_fp	glPolygonMode
#define glDepthMask_fp		glDepthMask
#define glDepthRange_fp		glDepthRange
#define glDepthFunc_fp		glDepthFunc

#define glDrawBuffer_fp		glDrawBuffer
#define glReadPixels_fp		glReadPixels
#define glPixelStorei_fp	glPixelStorei
#define glHint_fp		glHint
#define glCullFace_fp		glCullFace

#define glRotatef_fp		glRotatef
#define glTranslatef_fp		glTranslatef

#define glOrtho_fp		glOrtho
#define glFrustum_fp		glFrustum
#define glViewport_fp		glViewport
#define glPushMatrix_fp		glPushMatrix
#define glPopMatrix_fp		glPopMatrix
#define glLoadIdentity_fp	glLoadIdentity
#define glMatrixMode_fp		glMatrixMode
#define glLoadMatrixf_fp	glLoadMatrixf
#define glPolygonOffset_fp	glPolygonOffset

#define glGetString_fp		glGetString
#define glGetFloatv_fp		glGetFloatv
#define glGetIntegerv_fp	glGetIntegerv

#define glStencilFunc_fp	glStencilFunc
#define glStencilOp_fp		glStencilOp
#define glClearStencil_fp	glClearStencil

#endif	/* GL_FUNC_H */

#endif	/* !defined(GL_DLSYM) */

#undef GL_FUNCTION


/* global gl functions link to at runtime
 */
#if defined(__EMSCRIPTEN__)
/* Emscripten: all GL functions are statically linked */
#ifndef GL_FUNCTION_OPT
#define GL_FUNCTION_OPT(ret, func, params)	/* nothing — use direct defines below */
#endif
#else
#ifndef GL_FUNCTION_OPT
#define GL_FUNCTION_OPT(ret, func, params) \
typedef ret (APIENTRY *func##_f) params; \
__GL_FUNC_EXTERN func##_f func##_fp;
#endif
#endif

/* GL_ARB_multitexture */
GL_FUNCTION_OPT(void, glActiveTextureARB, (GLenum))
GL_FUNCTION_OPT(void, glMultiTexCoord2fARB, (GLenum,GLfloat,GLfloat))

/* FBO functions (OpenGL 3.0 / GL_ARB_framebuffer_object) */
GL_FUNCTION_OPT(void, glGenFramebuffers, (GLsizei, GLuint *))
GL_FUNCTION_OPT(void, glDeleteFramebuffers, (GLsizei, const GLuint *))
GL_FUNCTION_OPT(void, glBindFramebuffer, (GLenum, GLuint))
GL_FUNCTION_OPT(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GL_FUNCTION_OPT(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
GL_FUNCTION_OPT(GLenum, glCheckFramebufferStatus, (GLenum))
GL_FUNCTION_OPT(void, glGenRenderbuffers, (GLsizei, GLuint *))
GL_FUNCTION_OPT(void, glDeleteRenderbuffers, (GLsizei, const GLuint *))
GL_FUNCTION_OPT(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))
GL_FUNCTION_OPT(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
GL_FUNCTION_OPT(void, glBindRenderbuffer, (GLenum, GLuint))
GL_FUNCTION_OPT(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))

/* GLSL shader functions (OpenGL 2.0+) */
GL_FUNCTION_OPT(GLuint, glCreateShader, (GLenum))
GL_FUNCTION_OPT(void, glDeleteShader, (GLuint))
GL_FUNCTION_OPT(void, glShaderSource, (GLuint, GLsizei, const char **, const GLint *))
GL_FUNCTION_OPT(void, glCompileShader, (GLuint))
GL_FUNCTION_OPT(void, glGetShaderiv, (GLuint, GLenum, GLint *))
GL_FUNCTION_OPT(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *))
GL_FUNCTION_OPT(GLuint, glCreateProgram, (void))
GL_FUNCTION_OPT(void, glDeleteProgram, (GLuint))
GL_FUNCTION_OPT(void, glAttachShader, (GLuint, GLuint))
GL_FUNCTION_OPT(void, glLinkProgram, (GLuint))
GL_FUNCTION_OPT(void, glUseProgram, (GLuint))
GL_FUNCTION_OPT(void, glGetProgramiv, (GLuint, GLenum, GLint *))
GL_FUNCTION_OPT(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *))
GL_FUNCTION_OPT(GLint, glGetUniformLocation, (GLuint, const char *))
GL_FUNCTION_OPT(void, glUniform1i, (GLint, GLint))
GL_FUNCTION_OPT(void, glUniform1f, (GLint, GLfloat))
GL_FUNCTION_OPT(void, glUniform2f, (GLint, GLfloat, GLfloat))
GL_FUNCTION_OPT(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GL_FUNCTION_OPT(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GL_FUNCTION_OPT(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))
GL_FUNCTION_OPT(void, glBindAttribLocation, (GLuint, GLuint, const char *))

/* VBO/VAO functions (OpenGL 2.0+ / 3.0+) */
GL_FUNCTION_OPT(void, glGenBuffers, (GLsizei, GLuint *))
GL_FUNCTION_OPT(void, glDeleteBuffers, (GLsizei, const GLuint *))
GL_FUNCTION_OPT(void, glBindBuffer, (GLenum, GLuint))
GL_FUNCTION_OPT(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))
GL_FUNCTION_OPT(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *))
GL_FUNCTION_OPT(void, glGenVertexArrays, (GLsizei, GLuint *))
GL_FUNCTION_OPT(void, glDeleteVertexArrays, (GLsizei, const GLuint *))
GL_FUNCTION_OPT(void, glBindVertexArray, (GLuint))
GL_FUNCTION_OPT(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
GL_FUNCTION_OPT(void, glEnableVertexAttribArray, (GLuint))
GL_FUNCTION_OPT(void, glDisableVertexAttribArray, (GLuint))
GL_FUNCTION_OPT(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat))
GL_FUNCTION_OPT(void, glDrawArrays, (GLenum, GLint, GLsizei))
GL_FUNCTION_OPT(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))
GL_FUNCTION_OPT(void, glBindBufferBase, (GLenum, GLuint, GLuint))

/* Instancing (OpenGL 3.1 / ES 3.0) */
GL_FUNCTION_OPT(void, glDrawElementsInstanced, (GLenum, GLsizei, GLenum, const void *, GLsizei))
GL_FUNCTION_OPT(void, glVertexAttribDivisor, (GLuint, GLuint))
GL_FUNCTION_OPT(void, glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void *))

/* UBO (OpenGL 3.1 / ES 3.0) */
GL_FUNCTION_OPT(GLuint, glGetUniformBlockIndex, (GLuint, const char *))
GL_FUNCTION_OPT(void, glUniformBlockBinding, (GLuint, GLuint, GLuint))
GL_FUNCTION_OPT(void, glBindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))

/* Compute shader (OpenGL 4.3) */
GL_FUNCTION_OPT(void, glDispatchCompute, (GLuint, GLuint, GLuint))
GL_FUNCTION_OPT(void, glMemoryBarrier, (GLbitfield))

/* Timer queries (GL 3.3 / GL_ARB_timer_query) */
GL_FUNCTION_OPT(void, glGenQueries, (GLsizei, GLuint *))
GL_FUNCTION_OPT(void, glDeleteQueries, (GLsizei, const GLuint *))
GL_FUNCTION_OPT(void, glQueryCounter, (GLuint, GLenum))
GL_FUNCTION_OPT(void, glGetQueryObjectui64v, (GLuint, GLenum, GLuint64 *))
GL_FUNCTION_OPT(void, glGetQueryObjectiv, (GLuint, GLenum, GLint *))

/* Indirect draw (OpenGL 4.0 / GL_ARB_draw_indirect) */
GL_FUNCTION_OPT(void, glMultiDrawElementsIndirect, (GLenum, GLenum, const void *, GLsizei, GLsizei))
GL_FUNCTION_OPT(void, glDrawElementsIndirect, (GLenum, GLenum, const void *))

/* 3D texture (OpenGL 1.2+) */
GL_FUNCTION_OPT(void, glTexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))

/* Additional uniform functions */
GL_FUNCTION_OPT(void, glUniform3fv, (GLint, GLsizei, const GLfloat *))
GL_FUNCTION_OPT(void, glUniform4fv, (GLint, GLsizei, const GLfloat *))

#undef GL_FUNCTION_OPT

#if defined(__EMSCRIPTEN__)
/* Direct-call defines for GLES3 functions on Emscripten */
#define glActiveTextureARB_fp		glActiveTexture
#define glMultiTexCoord2fARB_fp(t,s,u)	((void)0)	/* unused */
#define glGenFramebuffers_fp		glGenFramebuffers
#define glDeleteFramebuffers_fp		glDeleteFramebuffers
#define glBindFramebuffer_fp		glBindFramebuffer
#define glFramebufferTexture2D_fp	glFramebufferTexture2D
#define glFramebufferRenderbuffer_fp	glFramebufferRenderbuffer
#define glCheckFramebufferStatus_fp	glCheckFramebufferStatus
#define glGenRenderbuffers_fp		glGenRenderbuffers
#define glDeleteRenderbuffers_fp	glDeleteRenderbuffers
#define glRenderbufferStorageMultisample_fp glRenderbufferStorageMultisample
#define glBlitFramebuffer_fp		glBlitFramebuffer
#define glBindRenderbuffer_fp		glBindRenderbuffer
#define glRenderbufferStorage_fp	glRenderbufferStorage
#define glCreateShader_fp		glCreateShader
#define glDeleteShader_fp		glDeleteShader
#define glShaderSource_fp		glShaderSource
#define glCompileShader_fp		glCompileShader
#define glGetShaderiv_fp		glGetShaderiv
#define glGetShaderInfoLog_fp		glGetShaderInfoLog
#define glCreateProgram_fp		glCreateProgram
#define glDeleteProgram_fp		glDeleteProgram
#define glAttachShader_fp		glAttachShader
#define glLinkProgram_fp		glLinkProgram
#define glUseProgram_fp			glUseProgram
#define glGetProgramiv_fp		glGetProgramiv
#define glGetProgramInfoLog_fp		glGetProgramInfoLog
#define glGetUniformLocation_fp		glGetUniformLocation
#define glUniform1i_fp			glUniform1i
#define glUniform1f_fp			glUniform1f
#define glUniform2f_fp			glUniform2f
#define glUniform3f_fp			glUniform3f
#define glUniform4f_fp			glUniform4f
#define glUniformMatrix4fv_fp		glUniformMatrix4fv
#define glBindAttribLocation_fp		glBindAttribLocation
#define glGenBuffers_fp			glGenBuffers
#define glDeleteBuffers_fp		glDeleteBuffers
#define glBindBuffer_fp			glBindBuffer
#define glBufferData_fp			glBufferData
#define glBufferSubData_fp		glBufferSubData
#define glGenVertexArrays_fp		glGenVertexArrays
#define glDeleteVertexArrays_fp		glDeleteVertexArrays
#define glBindVertexArray_fp		glBindVertexArray
#define glVertexAttribPointer_fp	glVertexAttribPointer
#define glEnableVertexAttribArray_fp	glEnableVertexAttribArray
#define glDisableVertexAttribArray_fp	glDisableVertexAttribArray
#define glVertexAttrib4f_fp		glVertexAttrib4f
#define glDrawArrays_fp			glDrawArrays
#define glDrawElements_fp		glDrawElements
#define glBindBufferBase_fp		glBindBufferBase
#define glDispatchCompute_fp(x,y,z)	((void)0)
#define glMemoryBarrier_fp(b)		((void)0)
#define glGenQueries_fp(n,ids)		((void)0)
#define glDeleteQueries_fp(n,ids)	((void)0)
#define glQueryCounter_fp(id,t)		((void)0)
#define glGetQueryObjectui64v_fp(id,p,v) ((void)0)
#define glGetQueryObjectiv_fp(id,p,v)	((void)0)
#define glMultiDrawElementsIndirect_fp(m,t,i,c,s) ((void)0)
#define glDrawElementsIndirect_fp(m,t,i)	((void)0)
#define glDrawElementsInstanced_fp	glDrawElementsInstanced
#define glVertexAttribDivisor_fp	glVertexAttribDivisor
#define glVertexAttribIPointer_fp	glVertexAttribIPointer
#define glGetUniformBlockIndex_fp	glGetUniformBlockIndex
#define glUniformBlockBinding_fp	glUniformBlockBinding
#define glBindBufferRange_fp		glBindBufferRange
#define glTexImage3D_fp			glTexImage3D
#define glUniform3fv_fp			glUniform3fv
#define glUniform4fv_fp			glUniform4fv
#endif /* __EMSCRIPTEN__ */


/* typedefs for functions linked to locally at runtime
 */
#ifndef GL_FUNC_TYPEDEFS
#define GL_FUNC_TYPEDEFS

/* this one doesn't seem to in amiga opengl libs :( */
typedef void (APIENTRY *glGetTexParameterfv_f) (GLenum,GLenum,GLfloat *);

/* GL_EXT_shared_texture_palette */
typedef void (APIENTRY *glColorTableEXT_f) (GLenum, GLenum, GLsizei, GLenum, GLenum, const GLvoid *);

/* 3DFX_set_global_palette (NOTE: the PowerVR equivalent is
   POWERVR_set_global_palette / glSetGlobalPalettePOWERVR) */
typedef void (APIENTRY *gl3DfxSetPaletteEXT_f) (GLuint *);

#endif /* GL_FUNC_TYPEDEFS */
