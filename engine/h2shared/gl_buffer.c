/*
 * gl_buffer.c -- Ironwail-parity streaming buffer ring.
 *
 * Replaces the raw glBufferSubData → glBindBufferBase → draw pattern used
 * for per-frame dynamic SSBO/UBO uploads.  That pattern relied on driver
 * implicit sync between CPU writes and shader reads; driver quality on it
 * varies, leading to random visual corruption on a subset of GPUs
 * (uhexen2-c5xe).
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
 * stubs out at compile time via __EMSCRIPTEN__ guards (the alias
 * instance SSBO shader doesn't compile on WebGL2 anyway).
 */

#include "quakedef.h"

#ifndef __EMSCRIPTEN__

#include "glheader.h"
#include "glquake.h"

/* Probed extension availability — set by gl_vidsdl.c at init. */
qboolean	gl_buffer_storage_able = false;
qboolean	gl_multi_bind_able = false;
qboolean	gl_sync_able = false;

GLint		gl_ssbo_align = 256;	/* conservative default */
GLint		gl_ubo_align = 256;

/* Last-seen single buffer bindings — drop redundant glBindBuffer calls. */
static GLuint	current_array_buffer;
static GLuint	current_element_array_buffer;
static GLuint	current_shader_storage_buffer;
static GLuint	current_uniform_buffer;
static GLuint	current_draw_indirect_buffer;

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

void GL_BindBuffer (GLenum target, GLuint buffer)
{
	GLuint *cache;

	switch (target)
	{
	case GL_ARRAY_BUFFER:		cache = &current_array_buffer;		break;
	case GL_ELEMENT_ARRAY_BUFFER:	cache = &current_element_array_buffer;	break;
	case GL_SHADER_STORAGE_BUFFER:	cache = &current_shader_storage_buffer;	break;
	case GL_UNIFORM_BUFFER:		cache = &current_uniform_buffer;	break;
	case GL_DRAW_INDIRECT_BUFFER:	cache = &current_draw_indirect_buffer;	break;
	default:
		glBindBuffer_fp (target, buffer);
		return;
	}

	if (*cache == buffer)
		return;
	*cache = buffer;
	glBindBuffer_fp (target, buffer);
}
#define GL_BindBufferCached GL_BindBuffer

void GL_BindBufferRange (GLenum target, GLuint index,
			 GLuint buffer, GLintptr offset, GLsizeiptr size)
{
	bufferrange_t *cache = NULL;

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

	/* BindBufferRange also implicitly binds to the target binding,
	 * so update the single-binding cache for that target too. */
	if (target == GL_SHADER_STORAGE_BUFFER)
		current_shader_storage_buffer = buffer;
	else if (target == GL_UNIFORM_BUFFER)
		current_uniform_buffer = buffer;

	glBindBufferRange_fp (target, index, buffer, offset, size);
}

void GL_BindBuffersRange (GLenum target, GLuint first, GLsizei count,
			  const GLuint *buffers,
			  const GLintptr *offsets,
			  const GLsizeiptr *sizes)
{
	GLsizei i;

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

void GL_ClearBufferBindings (void)
{
	int i;

	current_array_buffer = 0;
	current_element_array_buffer = 0;
	current_shader_storage_buffer = 0;
	current_uniform_buffer = 0;
	current_draw_indirect_buffer = 0;

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
		glDeleteBuffers_fp (1, &frame->garbage[i]);
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
				GL_BindBufferCached (GL_SHADER_STORAGE_BUFFER,
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
		GL_BindBufferCached (GL_SHADER_STORAGE_BUFFER, frame->host_buffer);

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
			GL_BindBufferCached (GL_SHADER_STORAGE_BUFFER,
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
		GL_BindBufferCached (target, frame->host_buffer);
		glBufferSubData_fp (target, frameres_host_offset, numbytes, data);
	}

	*outbuf = frame->host_buffer;
	*outofs = (GLintptr) frameres_host_offset;

	frameres_host_offset += numbytes;
}

#else	/* __EMSCRIPTEN__ — WebGL2 has no buffer_storage, sync, multi_bind.
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

void GL_BindBuffersRange (GLenum target, GLuint first, GLsizei count,
			  const GLuint *buffers,
			  const GLintptr *offsets,
			  const GLsizeiptr *sizes)
{
	(void)target; (void)first; (void)count;
	(void)buffers; (void)offsets; (void)sizes;
}

#endif	/* __EMSCRIPTEN__ */
