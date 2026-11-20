/*
 * gl_buffer.c -- Ironwail-parity streaming buffer ring.
 *
 * Replaces the raw glBufferSubData → glBindBufferBase → draw pattern used
 * for per-frame dynamic SSBO/UBO uploads.  That pattern relied on driver
 * implicit sync between CPU writes and shader reads; driver quality on it
 * varies, leading to random visual corruption on a subset of GPUs.
 *
 * This module mirrors Ironwail's gl_rmisc.c FRAMES_IN_FLIGHT ring:
 *   - 3 frame slots, each owns a host buffer + GPU fence.
 *   - Host buffer is persistent-mapped (GL_ARB_buffer_storage) when
 *     available, falls back to GL_STREAM_DRAW + glBufferSubData otherwise.
 *   - Acquire-at-frame-begin waits on the slot's fence (CPU+GPU).
 *   - Release-at-frame-end inserts a new fence; ring advances.
 *   - Auto-grows host buffer when overflowed; old buffer goes to a
 *     per-slot garbage list, freed after its slot retires.
 *   - GL_BindBufferRange/GL_BindBuffersRange wrappers cache last
 *     bindings to drop redundant calls and provide a fallback if
 *     GL_ARB_multi_bind is missing.
 *
 * The whole module is GL 4.3 territory; the WebGL2/Emscripten build
 * stubs out at compile time via USE_GLES guards (the alias
 * instance SSBO shader doesn't compile on WebGL2 anyway).
 *
 * The end of the file, built on every tier, is the buffer and vertex-array
 * seam (#340 P2d): GL_BindBuffer and the R_* entry points below, each GL
 * arm the bare call it replaced, each SDL_GPU arm rg_buffer.c.
 * tools/test_buffer_routing.py keeps those calls out of every other file
 * but the ones its ledger names.
 */

#include "quakedef.h"

#ifdef USE_SDL_GPU
#include "rg_device.h"
#endif

#ifndef USE_GLES

#include "glheader.h"
#include "glquake.h"

/* Probed extension availability — set by gl_vidsdl.c at init. */
qboolean	gl_buffer_storage_able = false;
qboolean	gl_multi_bind_able = false;
qboolean	gl_sync_able = false;

GLint		gl_ssbo_align = 256;	/* conservative default */
GLint		gl_ubo_align = 256;

typedef struct {
	GLuint		buffer;
	GLintptr	offset;
	GLsizeiptr	size;
} bufferrange_t;

/* Per-binding-point cache of the last BindBufferRange call.  Sized to
 * cover the indices we actually use (alias inst 0/1/2, worldcull 0-6).  */
#define CACHED_BUFFER_RANGES	8
static bufferrange_t ssbo_ranges[CACHED_BUFFER_RANGES];
static bufferrange_t ubo_ranges[CACHED_BUFFER_RANGES];

/* ----------------------------------------------------------------------
 *  Buffer binding wrappers — drop redundant binds.
 * ---------------------------------------------------------------------- */

/* GL_BindBuffer is with the seams at the end of the file, shared with the
 * ES tier.  The ring below binds with the bare glBindBuffer_fp: it is GL 4.3
 * state -gpu leaves on the null table, and through the seam a GL_Upload to a
 * vertex target would write a null-table name into rg_buffer.c's array or
 * element binding (#340 P2d). */

/* Under -gpu the storage binding points are rg_buffer.c's (#340 P2e), ahead
 * of the cache, which shadows the GL context only. */
void GL_BindBufferRange (GLenum target, GLuint index,
			 GLuint buffer, GLintptr offset, GLsizeiptr size)
{
	bufferrange_t *cache = NULL;

#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_BindBufferRange (target, index, buffer, offset, size);
		return;
	}
#endif
	if (target == GL_SHADER_STORAGE_BUFFER && index < CACHED_BUFFER_RANGES)
		cache = &ssbo_ranges[index];
	else if (target == GL_UNIFORM_BUFFER && index < CACHED_BUFFER_RANGES)
		cache = &ubo_ranges[index];

	if (cache &&
	    cache->buffer == buffer &&
	    cache->offset == offset &&
	    cache->size   == size)
		return;

	if (cache)
	{
		cache->buffer = buffer;
		cache->offset = offset;
		cache->size   = size;
	}

	glBindBufferRange_fp (target, index, buffer, offset, size);
}

void GL_BindBufferBase (GLenum target, GLuint index, GLuint buffer)
{
	bufferrange_t *cache = NULL;

#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_BindBufferBase (target, index, buffer);
		return;
	}
#endif
	if (target == GL_SHADER_STORAGE_BUFFER && index < CACHED_BUFFER_RANGES)
		cache = &ssbo_ranges[index];
	else if (target == GL_UNIFORM_BUFFER && index < CACHED_BUFFER_RANGES)
		cache = &ubo_ranges[index];

	/* Record the new binding rather than skip a redundant one: a raw
	 * glBindBufferBase_fp here would leave the range cache holding the
	 * previous (buffer, offset, size), and the next GL_BindBufferRange
	 * repeating that triple would short-circuit while the real binding
	 * still points wherever BindBufferBase left it.  size 0 is not a legal
	 * BindBufferRange size, so it can never be mistaken for a cache hit.
	 * uhexen2-lzex. */
	if (cache)
	{
		cache->buffer = buffer;
		cache->offset = 0;
		cache->size   = 0;
	}

	glBindBufferBase_fp (target, index, buffer);
}

void GL_BindBuffersRange (GLenum target, GLuint first, GLsizei count,
			  const GLuint *buffers,
			  const GLintptr *offsets,
			  const GLsizeiptr *sizes)
{
	GLsizei i;

#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		for (i = 0; i < count; i++)
			RG_BindBufferRange (target, first + i, buffers[i], offsets[i], sizes[i]);
		return;
	}
#endif
	if (gl_multi_bind_able)
	{
		bufferrange_t *cache = NULL;
		if (target == GL_SHADER_STORAGE_BUFFER)
			cache = ssbo_ranges;
		else if (target == GL_UNIFORM_BUFFER)
			cache = ubo_ranges;

		if (cache)
		{
			for (i = 0; i < count && first + i < CACHED_BUFFER_RANGES; i++)
			{
				bufferrange_t *slot = &cache[first + i];
				slot->buffer = buffers[i];
				slot->offset = offsets[i];
				slot->size   = sizes[i];
			}
		}
		glBindBuffersRange_fp (target, first, count, buffers, offsets, sizes);
		return;
	}

	/* Fallback: per-slot BindBufferRange. */
	for (i = 0; i < count; i++)
		GL_BindBufferRange (target, first + i, buffers[i], offsets[i], sizes[i]);
}

/* GPU particle resources (engine/hexen2/r_part.c, GitHub #453).
 *
 * r_part.c owns the particle SSBO/VAO names but may not call raw GL itself:
 * every draw-affecting GL call lives in engine/h2shared/gl_*.c or
 * engine/hexen2/gl_*.c, through the gl_func.h loader table, so a second
 * backend (SDL_GPU, #447) has one place that knows the whole draw state.
 * The SSBO is allocated empty -- the vertex shader reads particles from it
 * and there are no vertex attributes, hence the empty VAO. */
void GL_CreateParticleResources (GLuint *ssbo, GLuint *vao, GLsizeiptr size)
{
	glGenBuffers_fp (1, ssbo);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, *ssbo);
	glBufferData_fp (GL_SHADER_STORAGE_BUFFER, size, NULL, GL_DYNAMIC_DRAW);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, 0);

	/* Empty VAO -- vertex shader reads from SSBO, no vertex attributes */
	glGenVertexArrays_fp (1, vao);
}

void GL_DeleteParticleResources (GLuint ssbo, GLuint vao)
{
	/* Caller calls GL_ForgetBuffer (ssbo) first: the indexed-binding cache
	 * must not keep holding a name GL is about to hand back out. */
	if (ssbo)
		glDeleteBuffers_fp (1, &ssbo);
	if (vao)
		glDeleteVertexArrays_fp (1, &vao);
}

/* Drop any cached indexed binding that names `handle`.
 *
 * glDeleteBuffers resets every binding point the deleted buffer occupied back
 * to 0, and GL then hands the freed name straight back out of the next
 * glGenBuffers (names are allocated lowest-free-first).  So a cache entry left
 * holding a dead name is not merely stale, it actively matches the buffer that
 * inherits the name: GL_BindBufferRange sees buffer/offset/size all equal,
 * short-circuits, and the shader reads binding 0 instead of the new buffer.
 * Every SSBO/UBO delete site in the engine calls this first. */
void GL_ForgetBuffer (GLuint handle)
{
	int i;

	if (!handle)
		return;

	for (i = 0; i < CACHED_BUFFER_RANGES; i++)
	{
		if (ssbo_ranges[i].buffer == handle)
		{
			ssbo_ranges[i].buffer = 0;
			ssbo_ranges[i].offset = 0;
			ssbo_ranges[i].size   = 0;
		}
		if (ubo_ranges[i].buffer == handle)
		{
			ubo_ranges[i].buffer = 0;
			ubo_ranges[i].offset = 0;
			ubo_ranges[i].size   = 0;
		}
	}
}

void GL_ClearBufferBindings (void)
{
	int i;

	for (i = 0; i < CACHED_BUFFER_RANGES; i++)
	{
		ssbo_ranges[i].buffer = 0;
		ssbo_ranges[i].offset = 0;
		ssbo_ranges[i].size   = 0;
		ubo_ranges[i].buffer = 0;
		ubo_ranges[i].offset = 0;
		ubo_ranges[i].size   = 0;
	}
}

/* ----------------------------------------------------------------------
 *  Frame resources: ring of host buffers with fence sync.
 * ---------------------------------------------------------------------- */

#define FRAMES_IN_FLIGHT	3
#define MAX_GARBAGE_PER_FRAME	8

typedef struct frameres_t {
	GLsync		fence;
	GLuint		host_buffer;
	GLubyte		*host_ptr;	/* non-NULL when persistent-mapped */
	GLuint		garbage[MAX_GARBAGE_PER_FRAME];
	int		num_garbage;
} frameres_t;

static frameres_t	frameres[FRAMES_IN_FLIGHT];
static int		frameres_idx = 0;
static size_t		frameres_host_offset = 0;
static size_t		frameres_host_buffer_size = 1 * 1024 * 1024;	/* 1 MB initial */

static void GL_AddGarbageBufferTo (frameres_t *frame, GLuint handle)
{
	if (!handle)
		return;
	if (frame->num_garbage >= MAX_GARBAGE_PER_FRAME)
	{
		/* Should never happen — we resize at most once per frame.
		 * Fall back to immediate delete (the only risk: the buffer
		 * is still in flight, but the only path here is buggy). */
		glDeleteBuffers_fp (1, &handle);
		return;
	}
	frame->garbage[frame->num_garbage++] = handle;
}

void GL_AddGarbageBuffer (GLuint handle)
{
	GL_AddGarbageBufferTo (&frameres[frameres_idx], handle);
}

static void GL_DrainGarbage (frameres_t *frame)
{
	int i;
	for (i = 0; i < frame->num_garbage; i++)
	{
		GL_ForgetBuffer (frame->garbage[i]);
		glDeleteBuffers_fp (1, &frame->garbage[i]);
	}
	frame->num_garbage = 0;
}

static void GL_AllocFrameHostBuffers (void)
{
	int i;
	const GLbitfield map_flags =
		GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

	for (i = 0; i < FRAMES_IN_FLIGHT; i++)
	{
		frameres_t *frame = &frameres[i];

		if (frame->host_buffer)
		{
			if (frame->host_ptr)
			{
				glBindBuffer_fp (GL_SHADER_STORAGE_BUFFER,
						    frame->host_buffer);
				glUnmapBuffer_fp (GL_SHADER_STORAGE_BUFFER);
				frame->host_ptr = NULL;
			}
			/* Defer delete: this slot's host buffer might still
			 * be in flight on the GPU. */
			GL_AddGarbageBufferTo (frame, frame->host_buffer);
			frame->host_buffer = 0;
		}

		glGenBuffers_fp (1, &frame->host_buffer);
		glBindBuffer_fp (GL_SHADER_STORAGE_BUFFER, frame->host_buffer);

		if (gl_buffer_storage_able)
		{
			glBufferStorage_fp (GL_SHADER_STORAGE_BUFFER,
					    frameres_host_buffer_size,
					    NULL, map_flags);
			frame->host_ptr = (GLubyte *) glMapBufferRange_fp (
				GL_SHADER_STORAGE_BUFFER, 0,
				frameres_host_buffer_size, map_flags);
			if (!frame->host_ptr)
				Sys_Error ("GL_AllocFrameHostBuffers: "
					   "MapBufferRange failed on %lu bytes",
					   (unsigned long) frameres_host_buffer_size);
		}
		else
		{
			glBufferData_fp (GL_SHADER_STORAGE_BUFFER,
					 frameres_host_buffer_size,
					 NULL, GL_STREAM_DRAW);
		}
	}

	frameres_host_offset = 0;
}

void GL_CreateFrameResources (void)
{
	memset (frameres, 0, sizeof(frameres));
	frameres_idx = 0;
	GL_AllocFrameHostBuffers ();
}

void GL_DeleteFrameResources (void)
{
	int i;

	if (glFinish_fp)
		glFinish_fp ();

	for (i = 0; i < FRAMES_IN_FLIGHT; i++)
	{
		frameres_t *frame = &frameres[i];

		if (frame->fence)
		{
			glDeleteSync_fp (frame->fence);
			frame->fence = NULL;
		}

		GL_DrainGarbage (frame);

		if (frame->host_ptr)
		{
			glBindBuffer_fp (GL_SHADER_STORAGE_BUFFER,
					    frame->host_buffer);
			glUnmapBuffer_fp (GL_SHADER_STORAGE_BUFFER);
			frame->host_ptr = NULL;
		}

		if (frame->host_buffer)
		{
			glDeleteBuffers_fp (1, &frame->host_buffer);
			frame->host_buffer = 0;
		}
	}

	frameres_idx = 0;
	frameres_host_offset = 0;

	GL_ClearBufferBindings ();
}

void GL_AcquireFrameResources (void)
{
	frameres_t *frame = &frameres[frameres_idx];

	if (frame->fence && gl_sync_able)
	{
		GLuint64 timeout = 1ull * 1000 * 1000 * 1000;	/* 1 second */
		GLenum result = glClientWaitSync_fp (frame->fence,
						     GL_SYNC_FLUSH_COMMANDS_BIT,
						     timeout);
		if (result == GL_TIMEOUT_EXPIRED)
			glFinish_fp ();
		else if (result == GL_WAIT_FAILED)
			Sys_Error ("GL_AcquireFrameResources: wait failed");

		glDeleteSync_fp (frame->fence);
		frame->fence = NULL;
	}

	/* Slot has retired; its garbage can now be freed safely. */
	GL_DrainGarbage (frame);
}

void GL_ReleaseFrameResources (void)
{
	frameres_t *frame = &frameres[frameres_idx];

	if (gl_sync_able && glFenceSync_fp)
	{
		if (frame->fence)
			glDeleteSync_fp (frame->fence);
		frame->fence = glFenceSync_fp (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	}

	frameres_idx = (frameres_idx + 1) % FRAMES_IN_FLIGHT;
	frameres_host_offset = 0;
}

/* ----------------------------------------------------------------------
 *  GL_Upload — streaming write into the current frame's host buffer.
 *  Returns the (buffer, offset) tuple to bind via GL_BindBufferRange.
 * ---------------------------------------------------------------------- */

static size_t GL_AlignUp (size_t value, size_t align)
{
	size_t mask = align - 1;
	return (value + mask) & ~mask;
}

void GL_Upload (GLenum target, const void *data, size_t numbytes,
		GLuint *outbuf, GLintptr *outofs)
{
	size_t align;
	frameres_t *frame;

	/* -gpu streams storage data through rg_buffer.c, a buffer per upload
	 * (#340 P2e); the ring below is GL's. */
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_Upload (target, data, numbytes, outbuf, outofs);
		return;
	}
#endif
	align = (target == GL_UNIFORM_BUFFER) ? gl_ubo_align : gl_ssbo_align;
	if (align < 1)
		align = 1;
	frameres_host_offset = GL_AlignUp (frameres_host_offset, align);

	if (frameres_host_offset + numbytes > frameres_host_buffer_size)
	{
		/* Grow.  All slots are reallocated; old buffers go to each
		 * slot's garbage list and get freed after that slot retires
		 * (at the latest, FRAMES_IN_FLIGHT frames from now).
		 *
		 * Growth strategy: enough for this allocation plus 50%
		 * headroom, matching Ironwail. */
		size_t needed = GL_AlignUp (frameres_host_offset + numbytes, align);
		frameres_host_buffer_size = needed + (needed >> 1);
		GL_AllocFrameHostBuffers ();
		/* AllocFrameHostBuffers reset offset to 0, but we still need
		 * room for the alignment slack at the start.  In practice
		 * offset==0 already aligned, so re-apply: */
		frameres_host_offset = GL_AlignUp (0, align);
	}

	frame = &frameres[frameres_idx];

	if (frame->host_ptr)
	{
		memcpy (frame->host_ptr + frameres_host_offset, data, numbytes);
	}
	else
	{
		glBindBuffer_fp (target, frame->host_buffer);
		glBufferSubData_fp (target, frameres_host_offset, numbytes, data);
	}

	*outbuf = frame->host_buffer;
	*outofs = (GLintptr) frameres_host_offset;

	frameres_host_offset += numbytes;
}

#else	/* USE_GLES — WebGL2 has no buffer_storage, sync, multi_bind.
	 * The alias instance SSBO shader does not compile on WebGL2, so the
	 * only consumer of this module no-ops anyway.  Provide stubs so
	 * vid_setup paths can link without ifdefs at the call sites. */

#include "glheader.h"
#include "glquake.h"

qboolean	gl_buffer_storage_able = false;
qboolean	gl_multi_bind_able = false;
qboolean	gl_sync_able = false;

GLint		gl_ssbo_align = 256;
GLint		gl_ubo_align = 256;

void GL_CreateFrameResources (void) {}
void GL_DeleteFrameResources (void) {}
void GL_AcquireFrameResources (void) {}
void GL_ReleaseFrameResources (void) {}
void GL_ClearBufferBindings (void) {}
void GL_ForgetBuffer (GLuint handle) { (void)handle; }
void GL_AddGarbageBuffer (GLuint handle) { (void)handle; }

void GL_Upload (GLenum target, const void *data, size_t numbytes,
		GLuint *outbuf, GLintptr *outofs)
{
	(void)target; (void)data; (void)numbytes;
	*outbuf = 0;
	*outofs = 0;
}

void GL_BindBufferRange (GLenum target, GLuint index,
			 GLuint buffer, GLintptr offset, GLsizeiptr size)
{
	(void)target; (void)index; (void)buffer; (void)offset; (void)size;
}

void GL_BindBufferBase (GLenum target, GLuint index, GLuint buffer)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_BindBufferBase (target, index, buffer);
		return;
	}
#endif
	glBindBufferBase_fp (target, index, buffer);
}

void GL_BindBuffersRange (GLenum target, GLuint first, GLsizei count,
			  const GLuint *buffers,
			  const GLintptr *offsets,
			  const GLsizeiptr *sizes)
{
	(void)target; (void)first; (void)count;
	(void)buffers; (void)offsets; (void)sizes;
}

/* No SSBO/VAO backing on the ES tier: the GPU particle path is compiled out
 * there (r_part.c keeps stub init/shutdown symbols), so these are never
 * called -- but the symbols must exist for the link.  GitHub #453. */
void GL_CreateParticleResources (GLuint *ssbo, GLuint *vao, GLsizeiptr size)
{
	(void)ssbo; (void)vao; (void)size;
}

void GL_DeleteParticleResources (GLuint ssbo, GLuint vao)
{
	(void)ssbo; (void)vao;
}

#endif	/* USE_GLES */

/* ----------------------------------------------------------------------
 *  Buffer and vertex-array seams (#340 P2d), every tier.
 *
 *  GL names buffers and vertex arrays and edits whatever is bound; so do
 *  these, with GL's signatures, and each GL arm is the one bare call the
 *  call site used to make -- GL is call-for-call unchanged.  The SDL_GPU
 *  arm (rg_buffer.c) shadows the same bindings by GL's rules: the array
 *  buffer is context state a pointer captures, the element buffer belongs
 *  to the bound VAO, the current attribute values are context state.
 *  The draws that read them are gl_vbo.c's R_DrawElements pair.
 * ---------------------------------------------------------------------- */

/* The generic (non-indexed) binding point is deliberately NOT cached.
 *
 * It used to be, and skipping the "redundant" bind is what produced the
 * glBufferSubData errors in issue #203 and its GL_INVALID_OPERATION twin
 * ("no buffer bound").  The cache can only be trusted if every binder in the
 * engine goes through it, and roughly forty raw glBindBuffer_fp calls do not
 * — gl_mesh.c, gl_shader.c, r_part.c and gl_worldcull.c all bind
 * GL_SHADER_STORAGE_BUFFER directly and then unbind to 0.  Sequence that bit:
 *
 *   frame N   LC_SetupFrame        GL_BindBufferBase(SSBO, 6, lc_ssbo)
 *                                    -> cache says lc_ssbo, real binding lc_ssbo
 *   map load  GL_MakeAliasGPUMesh  glBindBuffer_fp(SSBO, 0)
 *                                    -> cache still says lc_ssbo, real binding 0
 *   frame N+1 LC_UploadAndDispatch GL_BindBuffer(SSBO, lc_ssbo)  [skipped!]
 *                                  glBufferSubData(SSBO, 0, 1024, styles)
 *                                    -> GL_INVALID_OPERATION, no buffer bound,
 *                                       or GL_INVALID_VALUE into whatever a
 *                                       sibling path left bound instead.
 *
 * glBindBuffer is a cheap state set and this wrapper has under a dozen call
 * sites, a handful of which run per frame, so binding unconditionally costs
 * nothing measurable and removes the entire failure class.  The indexed
 * binding points above ARE still cached, which is safe because no raw
 * glBindBufferBase_fp / glBindBufferRange_fp call exists outside this file;
 * the one remaining way to stale that cache is deleting a buffer and having
 * GL hand the name back out, which GL_ForgetBuffer covers.
 * (gl_mesh.c and gl_shader.c bind through this wrapper since #340 P2e;
 * the compute files still bind raw, so the argument stands.)
 *
 * Under -gpu the element and array bindings are what a VAO and a pointer
 * capture (rg_buffer.c), and the storage target is what glBufferData fills
 * a storage buffer through (#340 P2e); the uniform target reaches nothing
 * there (gl_uniforms.c mirrors the blocks). */
void GL_BindBuffer (GLenum target, GLuint buffer)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_BindBuffer (target, buffer);
		return;
	}
#endif
	glBindBuffer_fp (target, buffer);
}

void R_GenBuffers (GLsizei n, GLuint *buffers)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_GenBuffers (n, buffers);
		return;
	}
#endif
	glGenBuffers_fp (n, buffers);
}

void R_DeleteBuffers (GLsizei n, const GLuint *buffers)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_DeleteBuffers (n, buffers);
		return;
	}
#endif
	glDeleteBuffers_fp (n, buffers);
}

void R_BufferData (GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_BufferData (target, size, data, usage);
		return;
	}
#endif
	glBufferData_fp (target, size, data, usage);
}

void R_BufferSubData (GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_BufferSubData (target, offset, size, data);
		return;
	}
#endif
	glBufferSubData_fp (target, offset, size, data);
}

void R_GenVertexArrays (GLsizei n, GLuint *arrays)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_GenVertexArrays (n, arrays);
		return;
	}
#endif
	glGenVertexArrays_fp (n, arrays);
}

void R_DeleteVertexArrays (GLsizei n, const GLuint *arrays)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_DeleteVertexArrays (n, arrays);
		return;
	}
#endif
	glDeleteVertexArrays_fp (n, arrays);
}

void R_BindVertexArray (GLuint array)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_BindVertexArray (array);
		return;
	}
#endif
	glBindVertexArray_fp (array);
}

void R_EnableVertexAttribArray (GLuint index)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_EnableVertexAttribArray (index, true);
		return;
	}
#endif
	glEnableVertexAttribArray_fp (index);
}

void R_DisableVertexAttribArray (GLuint index)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_EnableVertexAttribArray (index, false);
		return;
	}
#endif
	glDisableVertexAttribArray_fp (index);
}

void R_VertexAttribPointer (GLuint index, GLint size, GLenum type, GLboolean normalized,
			    GLsizei stride, const void *pointer)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_VertexAttribPointer (index, size, type, normalized, false, stride, pointer);
		return;
	}
#endif
	glVertexAttribPointer_fp (index, size, type, normalized, stride, pointer);
}

void R_VertexAttribIPointer (GLuint index, GLint size, GLenum type, GLsizei stride,
			     const void *pointer)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_VertexAttribPointer (index, size, type, GL_FALSE, true, stride, pointer);
		return;
	}
#endif
	glVertexAttribIPointer_fp (index, size, type, stride, pointer);
}

void R_VertexAttrib4f (GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_VertexAttrib4f (index, x, y, z, w);
		return;
	}
#endif
	glVertexAttrib4f_fp (index, x, y, z, w);
}
