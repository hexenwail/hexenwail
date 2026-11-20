/* rg_buffer.c -- SDL_GPU backend: buffers and vertex arrays (#340 P2d)
 *
 * The engine keeps its GL-shaped buffer and vertex-array names (decision 1
 * on #340); this file maps them.  It shadows exactly the GL state an indexed
 * draw reads, with GL's binding rules:
 *
 *   GL_ARRAY_BUFFER          context state.  glVertexAttribPointer captures
 *                            it into the attribute, as GL does, so a later
 *                            rebind does not move an attribute already set.
 *   GL_ELEMENT_ARRAY_BUFFER  vertex-array state: binding it, and
 *                            glBufferData through it, act on the bound VAO.
 *   the VAO's attributes     enable, size, type, normalized / integer,
 *                            stride, offset, buffer.
 *   current attributes       glVertexAttrib4f, context state: what a shader
 *                            input with no enabled array reads.
 *
 * Buffers.  SDL_GPU sizes a buffer at creation and fills it through a copy
 * pass; GL respecifies one with glBufferData whenever it likes.  So
 * glBufferData (re)creates the SDL_GPUBuffer when the size changes, and
 * every upload is copied out of the caller's memory at once (glBufferData's
 * contract) and queued for rg_frame.c's next copy pass, like a texture
 * upload.  A whole-buffer respecify is uploaded with cycling on, so draws an
 * earlier segment or frame still has in flight keep the old contents;
 * glBufferSubData is not, because it keeps the rest of the buffer.  The copy
 * pass runs ahead of its segment's draws, so an upload into a buffer that a
 * draw already recorded in the open segment reads would change what that
 * draw sees: the upload first ends the segment (RG_Frame_Flush), as
 * rg_resource.c does for textures.  Every buffer is created with vertex and
 * index usage: GL does not say which a name will be until it is drawn from.
 *
 * Names.  Buffer and vertex-array names are tagged (RGB_NAME_TAG): the
 * calls P2d leaves raw (gl_mesh.c's alias meshes, the compute and storage
 * buffers; tools/test_buffer_routing.py's ledger) still take names from the
 * null table, and a tag means one of those reaching a draw is reported as
 * unknown rather than mistaken for a buffer of ours.
 *
 * The draw (RG_Buf_ForDraw, for rg_frame.c's RG_DrawIndexed).  The bound
 * VAO's enabled attributes become a vertex layout: attributes sharing a
 * buffer and a stride, and lying inside one element of it, share a vertex
 * buffer slot.  Locations 0-3 (ATTR_POSITION .. ATTR_COLOR, gl_shader.h)
 * that the VAO leaves disabled are fed their current value: SDL_GPU, like
 * Vulkan, has no current attribute, and a vertex input the pipeline does not
 * supply is an error.  They go in one more slot, instance-rate, whose bytes
 * rg_frame.c puts in the stream -- one record for one instance, so every
 * vertex reads it, and a copy per instance for an instanced draw.  A
 * zero-pitch vertex-rate slot would be smaller, but Metal (P7) has no
 * zero stride.  Locations past 3 have no such fallback: every routed path
 * feeds exactly 0-3, and the alias meshes that use more are P2e.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef USE_SDL_GPU
#error rg_buffer.c is the SDL_GPU backend; CMake compiles it only with USE_SDL_GPU
#endif

#include "quakedef.h"
#include "rg_internal.h"
#include "gl_shader.h"		/* ATTR_* */

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER			0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER		0x8893
#endif

#define RGB_NAME_TAG	0x40000000u
#define RGB_NAME_MASK	0x3FFFFFFFu

/* Locations fed a current value when their array is disabled. */
#define RGB_CONST_FIRST	ATTR_POSITION
#define RGB_CONST_LAST	ATTR_COLOR

/* Queued buffer data, a copy of the caller's bytes. */
typedef struct rgbupload_s {
	struct rgbupload_s	*next;
	Uint32		offset, size;
	Uint32		staged;		/* offset in the transfer buffer, once staged */
	qboolean	cycle;		/* a whole-buffer respecify */
	Uint8		data[1];
} rgbupload_t;

typedef struct {
	qboolean		alive;
	SDL_GPUBuffer		*buf;		/* NULL until glBufferData gives it a size */
	Uint32			size;
	rgbupload_t		*uploads, **uploads_tail;
	unsigned int		read_segment;	/* RG_Frame_Segment () of the last draw */
	qboolean		read_ever;
} rgbuf_t;

typedef struct {
	Uint8		enabled, integer, normalized;
	GLint		size;
	GLenum		type;
	GLsizei		stride;
	GLintptr	offset;
	GLuint		buffer;		/* GL_ARRAY_BUFFER when the pointer was set */
} rgattrib_t;

typedef struct {
	qboolean	alive;
	rgattrib_t	attr[RG_MAX_VATTRS];
	GLuint		ebo;
} rgvao_t;

/* Name tables: entry 0 unused, name n is (RGB_NAME_TAG | n). */
typedef struct {
	void		*items;
	int		size, count, used;	/* item size, table size, highest index */
	int		*free;
	int		numfree, maxfree;
} rgnames_t;

static rgnames_t	rgb_bufnames = { NULL, sizeof(rgbuf_t), 0, 0, NULL, 0, 0 };
static rgnames_t	rgb_vaonames = { NULL, sizeof(rgvao_t), 0, 0, NULL, 0, 0 };

#define RGB_BUFS	((rgbuf_t *) rgb_bufnames.items)
#define RGB_VAOS	((rgvao_t *) rgb_vaonames.items)

static GLuint		rgb_array_buffer;	/* GL_ARRAY_BUFFER */
static GLuint		rgb_vao;		/* GL_VERTEX_ARRAY_BINDING */
/* VAO 0: GL core has none to draw from, but binding an element buffer or
 * setting a pointer with nothing bound still has to land somewhere. */
static rgvao_t		rgb_vao0;
static float		rgb_current[RG_MAX_VATTRS][4];

/* Released only after the frame's command buffer is submitted: a recorded
 * draw can still name one when the delete or re-creation happens. */
static SDL_GPUBuffer	**rgb_retired;
static int		rgb_numretired, rgb_maxretired;

static Uint32		rgb_pending_bytes;
static unsigned long	rgb_created, rgb_uploads, rgb_upload_bytes;

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

static void *RGB_Item (rgnames_t *t, int i)
{
	return (Uint8 *) t->items + (size_t) i * t->size;
}

static void RGB_GenNames (rgnames_t *t, GLsizei n, GLuint *names)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
	{
		int	idx;

		if (t->numfree > 0)
			idx = t->free[--t->numfree];
		else
		{
			if (t->used + 1 >= t->count)
			{
				int	newcount = t->count ? t->count * 2 : 256;

				if ((Uint32) newcount > RGB_NAME_MASK)
					Sys_Error ("SDL_GPU: out of buffer / vertex-array names");
				t->items = realloc (t->items, (size_t) newcount * t->size);
				if (!t->items)
					Sys_Error ("SDL_GPU: out of memory for %d buffer / vertex-array names", newcount);
				memset (RGB_Item (t, t->count), 0, (size_t)(newcount - t->count) * t->size);
				t->count = newcount;
			}
			idx = ++t->used;
		}
		memset (RGB_Item (t, idx), 0, t->size);
		names[i] = RGB_NAME_TAG | (GLuint) idx;
	}
}

static int RGB_Index (const rgnames_t *t, GLuint name)
{
	int	idx;

	if (!(name & RGB_NAME_TAG))
		return 0;
	idx = (int)(name & RGB_NAME_MASK);
	return (idx > 0 && idx <= t->used) ? idx : 0;
}

static void RGB_FreeName (rgnames_t *t, int idx)
{
	if (t->numfree == t->maxfree)
	{
		t->maxfree = t->maxfree ? t->maxfree * 2 : 64;
		t->free = (int *) realloc (t->free, t->maxfree * sizeof(int));
		if (!t->free)
			Sys_Error ("SDL_GPU: out of memory freeing names");
	}
	t->free[t->numfree++] = idx;
}

static rgbuf_t *RGB_Buf (GLuint name)
{
	int	i = RGB_Index (&rgb_bufnames, name);

	return (i && RGB_BUFS[i].alive) ? &RGB_BUFS[i] : NULL;
}

static rgvao_t *RGB_Vao (GLuint name)
{
	int	i;

	if (name == 0)
		return &rgb_vao0;
	i = RGB_Index (&rgb_vaonames, name);
	return (i && RGB_VAOS[i].alive) ? &RGB_VAOS[i] : NULL;
}

/* ------------------------------------------------------------------ */
/* Buffers                                                             */
/* ------------------------------------------------------------------ */

static void RGB_Retire (SDL_GPUBuffer *buf)
{
	if (!buf)
		return;
	if (rgb_numretired == rgb_maxretired)
	{
		rgb_maxretired = rgb_maxretired ? rgb_maxretired * 2 : 32;
		rgb_retired = (SDL_GPUBuffer **) realloc (rgb_retired, rgb_maxretired * sizeof(*rgb_retired));
		if (!rgb_retired)
			Sys_Error ("SDL_GPU: out of memory retiring buffers");
	}
	rgb_retired[rgb_numretired++] = buf;
}

static void RGB_DropUploads (rgbuf_t *b)
{
	rgbupload_t	*u, *next;

	for (u = b->uploads; u; u = next)
	{
		next = u->next;
		rgb_pending_bytes -= u->size;
		free (u);
	}
	b->uploads = NULL;
	b->uploads_tail = &b->uploads;
}

/* A draw recorded in the open segment reads b, so its copy pass has not run:
 * anything that changes what that pass would put in b -- a new upload, or
 * dropping or re-targeting the queued ones -- has to end the segment first. */
static void RGB_FlushIfRead (const rgbuf_t *b)
{
	if (b->read_ever && b->read_segment == RG_Frame_Segment () && RG_Frame_SegmentHasDraws ())
		RG_Frame_Flush ();
}

static void RGB_Queue (rgbuf_t *b, Uint32 offset, Uint32 size, const void *data, qboolean cycle)
{
	rgbupload_t	*u = (rgbupload_t *) malloc (sizeof(rgbupload_t) + size);

	if (!u)
		Sys_Error ("SDL_GPU: out of memory queueing a %u-byte buffer upload", (unsigned int)size);
	memcpy (u->data, data, size);
	u->next = NULL;
	u->offset = offset;
	u->size = size;
	u->staged = 0;
	u->cycle = cycle;
	/* An empty list's tail points into the name table, which RGB_GenNames
	 * may have moved since: re-derive it. */
	if (!b->uploads)
		b->uploads_tail = &b->uploads;
	*b->uploads_tail = u;
	b->uploads_tail = &u->next;
	rgb_pending_bytes += size;
	rgb_uploads++;
	rgb_upload_bytes += size;
	/* A level load is not held in client memory until the next frame. */
	if (rgb_pending_bytes > 64u * 1024u * 1024u)
		RG_Frame_Flush ();
}

void RG_GenBuffers (GLsizei n, GLuint *buffers)
{
	GLsizei	i;

	RGB_GenNames (&rgb_bufnames, n, buffers);
	for (i = 0; i < n; i++)
	{
		rgbuf_t	*b = &RGB_BUFS[buffers[i] & RGB_NAME_MASK];

		b->alive = true;
		b->uploads_tail = &b->uploads;
	}
}

static void RGB_Detach (rgvao_t *v, GLuint name)
{
	int	i;

	if (v->ebo == name)
		v->ebo = 0;
	for (i = 0; i < RG_MAX_VATTRS; i++)
	{
		if (v->attr[i].buffer == name)
			v->attr[i].buffer = 0;
	}
}

void RG_DeleteBuffers (GLsizei n, const GLuint *buffers)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
	{
		rgbuf_t	*b = RGB_Buf (buffers[i]);
		int	k;

		if (!b)
			continue;
		if (b->uploads)
			RGB_FlushIfRead (b);
		RGB_DropUploads (b);
		RGB_Retire (b->buf);
		b->buf = NULL;
		b->alive = false;
		/* GL unbinds a deleted buffer from the context and the bound VAO;
		 * another VAO keeps the object alive.  The name is reused here, so
		 * every VAO lets go of it, or a stale one would draw from whatever
		 * buffer is handed that name next: a draw from it is dropped
		 * (counted) instead.  The engine deletes its VAOs with their
		 * buffers, so the GL difference is never seen. */
		if (rgb_array_buffer == buffers[i])
			rgb_array_buffer = 0;
		RGB_Detach (&rgb_vao0, buffers[i]);
		for (k = 1; k <= rgb_vaonames.used; k++)
		{
			if (RGB_VAOS[k].alive)
				RGB_Detach (&RGB_VAOS[k], buffers[i]);
		}
		RGB_FreeName (&rgb_bufnames, (int)(buffers[i] & RGB_NAME_MASK));
	}
}

void RG_BindBuffer (GLenum target, GLuint buffer)
{
	rgvao_t	*v;

	/* Uniform and storage targets are not routed (gl_uniforms.c mirrors
	 * the blocks; storage is P5): nothing here reads them. */
	switch (target)
	{
	case GL_ARRAY_BUFFER:
		rgb_array_buffer = buffer;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		v = RGB_Vao (rgb_vao);
		if (v)
			v->ebo = buffer;
		else
			RG_NoteUnhandled ("element buffer bound with a VAO SDL_GPU never made bound (dropped)");
		break;
	}
}

static rgbuf_t *RGB_Bound (GLenum target, const char *what)
{
	GLuint	name = 0;
	rgbuf_t	*b;
	rgvao_t	*v;

	if (target == GL_ARRAY_BUFFER)
		name = rgb_array_buffer;
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
	{
		v = RGB_Vao (rgb_vao);
		name = v ? v->ebo : 0;
	}
	else
	{
		RG_NoteUnhandled (what);
		return NULL;
	}
	b = RGB_Buf (name);
	if (!b)
		RG_NoteUnhandled ("buffer data with no SDL_GPU buffer bound to the target (dropped)");
	return b;
}

void RG_BufferData (GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	rgbuf_t			*b = RGB_Bound (target, "glBufferData on a target other than "
						"GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER (dropped)");
	SDL_GPUBufferCreateInfo	ci;

	(void) usage;	/* SDL_GPU has no usage hint; the cycling below is the stream case */
	if (!b)
		return;
	if (size < 0 || (Uint64) size > 0x7FFFFFFFu)
	{
		RG_NoteUnhandled ("glBufferData size out of range (dropped)");
		return;
	}
	/* Respecified whole: what is queued and what the open segment's draws
	 * would read are both replaced. */
	RGB_FlushIfRead (b);
	RGB_DropUploads (b);
	if (!b->buf || b->size != (Uint32) size)
	{
		RGB_Retire (b->buf);
		b->buf = NULL;
		b->size = 0;
		if (size > 0)
		{
			SDL_zero (ci);
			ci.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_INDEX;
			ci.size = (Uint32) size;
			b->buf = SDL_CreateGPUBuffer (rg_dev, &ci);
			if (!b->buf)
			{
				Con_Printf ("SDL_GPU: cannot create a %u-byte buffer (%s)\n",
					    (unsigned int) size, SDL_GetError ());
				return;
			}
			b->size = (Uint32) size;
			rgb_created++;
		}
	}
	if (data && size > 0)
		RGB_Queue (b, 0, (Uint32) size, data, true);
}

void RG_BufferSubData (GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	rgbuf_t	*b = RGB_Bound (target, "glBufferSubData on a target other than "
				"GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER (dropped)");

	if (!b || size <= 0 || !data)
		return;
	if (!b->buf || offset < 0 || (Uint64) offset + (Uint64) size > b->size)
	{
		/* GL_INVALID_VALUE in GL: nothing is written. */
		RG_NoteUnhandled ("glBufferSubData outside the buffer's data store (dropped)");
		return;
	}
	RGB_FlushIfRead (b);
	RGB_Queue (b, (Uint32) offset, (Uint32) size, data, false);
}

/* ------------------------------------------------------------------ */
/* Vertex arrays                                                       */
/* ------------------------------------------------------------------ */

void RG_GenVertexArrays (GLsizei n, GLuint *arrays)
{
	GLsizei	i;

	RGB_GenNames (&rgb_vaonames, n, arrays);
	for (i = 0; i < n; i++)
		RGB_VAOS[arrays[i] & RGB_NAME_MASK].alive = true;
}

void RG_DeleteVertexArrays (GLsizei n, const GLuint *arrays)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
	{
		int	idx = RGB_Index (&rgb_vaonames, arrays[i]);

		if (!idx || !RGB_VAOS[idx].alive)
			continue;
		RGB_VAOS[idx].alive = false;
		if (rgb_vao == arrays[i])
			rgb_vao = 0;	/* GL: deleting the bound VAO binds 0 */
		RGB_FreeName (&rgb_vaonames, idx);
	}
}

void RG_BindVertexArray (GLuint array)
{
	if (array && !RGB_Vao (array))
		RG_NoteUnhandled ("glBindVertexArray of a name SDL_GPU never made (draws from it dropped)");
	rgb_vao = array;
}

void RG_EnableVertexAttribArray (GLuint index, qboolean enable)
{
	rgvao_t	*v = RGB_Vao (rgb_vao);

	if (!v || index >= RG_MAX_VATTRS)
	{
		RG_NoteUnhandled ("glEnable/DisableVertexAttribArray with no SDL_GPU VAO or index >= 16");
		return;
	}
	v->attr[index].enabled = enable ? 1 : 0;
}

void RG_VertexAttribPointer (GLuint index, GLint size, GLenum type, GLboolean normalized,
			     qboolean integer, GLsizei stride, const void *pointer)
{
	rgvao_t		*v = RGB_Vao (rgb_vao);
	rgattrib_t	*a;

	if (!v || index >= RG_MAX_VATTRS)
	{
		RG_NoteUnhandled ("glVertexAttrib*Pointer with no SDL_GPU VAO or index >= 16");
		return;
	}
	a = &v->attr[index];
	a->size = size;
	a->type = type;
	a->normalized = (normalized && !integer) ? 1 : 0;
	a->integer = integer ? 1 : 0;
	a->stride = stride;
	a->offset = (GLintptr) pointer;
	a->buffer = rgb_array_buffer;
}

void RG_VertexAttrib4f (GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
	if (index >= RG_MAX_VATTRS)
	{
		RG_NoteUnhandled ("glVertexAttrib4f index >= 16");
		return;
	}
	rgb_current[index][0] = x;
	rgb_current[index][1] = y;
	rgb_current[index][2] = z;
	rgb_current[index][3] = w;
}

/* ------------------------------------------------------------------ */
/* Draws                                                               */
/* ------------------------------------------------------------------ */

static int RGB_TypeBytes (GLenum type)
{
	switch (type)
	{
	case GL_BYTE: case GL_UNSIGNED_BYTE:		return 1;
	case GL_SHORT: case GL_UNSIGNED_SHORT:
	case GL_HALF_FLOAT:				return 2;
	case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
	}
	return 0;
}

/* GL's (size, type, normalized, integer) as an SDL_GPU vertex format;
 * INVALID for a combination SDL_GPU has no format for (a non-normalized
 * byte or short read as float, three-component bytes and shorts). */
static SDL_GPUVertexElementFormat RGB_Format (const rgattrib_t *a)
{
	static const SDL_GPUVertexElementFormat f32[4] = {
		SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
		SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4 };
	static const SDL_GPUVertexElementFormat i32[4] = {
		SDL_GPU_VERTEXELEMENTFORMAT_INT, SDL_GPU_VERTEXELEMENTFORMAT_INT2,
		SDL_GPU_VERTEXELEMENTFORMAT_INT3, SDL_GPU_VERTEXELEMENTFORMAT_INT4 };
	static const SDL_GPUVertexElementFormat u32[4] = {
		SDL_GPU_VERTEXELEMENTFORMAT_UINT, SDL_GPU_VERTEXELEMENTFORMAT_UINT2,
		SDL_GPU_VERTEXELEMENTFORMAT_UINT3, SDL_GPU_VERTEXELEMENTFORMAT_UINT4 };

	if (a->size < 1 || a->size > 4)
		return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
	if (a->integer)
	{
		switch (a->type)
		{
		case GL_INT:		return i32[a->size - 1];
		case GL_UNSIGNED_INT:	return u32[a->size - 1];
		case GL_BYTE:		return a->size == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_BYTE2 :
					       a->size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_BYTE4 :
					       SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
		case GL_UNSIGNED_BYTE:	return a->size == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2 :
					       a->size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4 :
					       SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
		case GL_SHORT:		return a->size == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_SHORT2 :
					       a->size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_SHORT4 :
					       SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
		case GL_UNSIGNED_SHORT:	return a->size == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_USHORT2 :
					       a->size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_USHORT4 :
					       SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
		}
		return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
	}
	switch (a->type)
	{
	case GL_FLOAT:
		return f32[a->size - 1];
	case GL_HALF_FLOAT:
		return a->size == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_HALF2 :
		       a->size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_HALF4 :
		       SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
	case GL_UNSIGNED_BYTE:
		if (!a->normalized)
			break;
		return a->size == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM :
		       a->size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM :
		       SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
	case GL_BYTE:
		if (!a->normalized)
			break;
		return a->size == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_BYTE2_NORM :
		       a->size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM :
		       SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
	case GL_UNSIGNED_SHORT:
		if (!a->normalized)
			break;
		return a->size == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM :
		       a->size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM :
		       SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
	case GL_SHORT:
		if (!a->normalized)
			break;
		return a->size == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM :
		       a->size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM :
		       SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
	}
	return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
}

static void RGB_Stamp (rgbuf_t *b)
{
	b->read_ever = true;
	b->read_segment = RG_Frame_Segment ();
}

/* Called once the draw is recorded, so a dropped one stamps nothing. */
void RG_Buf_MarkRead (const rgvinput_t *in)
{
	int	i;

	for (i = 0; i < in->nreads; i++)
		RGB_Stamp ((rgbuf_t *) in->reads[i]);
}

qboolean RG_Buf_ForDraw (int index_bytes, rgvinput_t *in)
{
	rgvao_t		*v = (rgb_vao != 0) ? RGB_Vao (rgb_vao) : NULL;
	rgbuf_t		*b, *used[RG_MAX_VBUFS];
	Uint32		base[RG_MAX_VBUFS];
	GLuint		slotbuf[RG_MAX_VBUFS];
	int		i, s, nslots = 0;

	memset (in, 0, sizeof(*in));
	in->const_slot = -1;
	if (!v)
	{
		RG_NoteUnhandled (rgb_vao ? "indexed draw from a VAO SDL_GPU never made (dropped; P2e for "
					    "the alias meshes)"
					  : "indexed draw with no VAO bound (dropped)");
		return false;
	}
	b = RGB_Buf (v->ebo);
	if (!b || !b->buf)
	{
		RG_NoteUnhandled ("indexed draw with no SDL_GPU element buffer in the VAO (dropped)");
		return false;
	}
	in->ib = b->buf;
	in->ib_count = b->size / (Uint32) index_bytes;
	in->reads[in->nreads++] = b;

	for (i = 0; i < RG_MAX_VATTRS; i++)
	{
		const rgattrib_t	*a = &v->attr[i];
		SDL_GPUVertexElementFormat fmt;
		Uint32			stride, ofs, width;
		rgvattr_t		*la;

		if (!a->enabled)
			continue;
		fmt = RGB_Format (a);
		width = (Uint32)(RGB_TypeBytes (a->type) * a->size);
		if (fmt == SDL_GPU_VERTEXELEMENTFORMAT_INVALID || !width)
		{
			RG_NoteUnhandled ("vertex attribute format with no SDL_GPU equivalent (draw dropped)");
			return false;
		}
		b = RGB_Buf (a->buffer);
		if (!b || !b->buf)
		{
			RG_NoteUnhandled ("enabled vertex attribute with no SDL_GPU buffer (draw dropped)");
			return false;
		}
		stride = a->stride ? (Uint32) a->stride : width;
		ofs = (Uint32) a->offset;
		/* A slot is one buffer, one stride, and attributes inside one
		 * element of it measured from the slot's first. */
		for (s = 0; s < nslots; s++)
		{
			if (slotbuf[s] == a->buffer && in->layout.slot[s].pitch == stride &&
			    ofs >= base[s] && ofs - base[s] + width <= stride)
				break;
		}
		if (s == nslots)
		{
			if (nslots == RG_MAX_VBUFS - 1)	/* one kept for the constants */
			{
				RG_NoteUnhandled ("more vertex buffer slots than SDL_GPU is given (draw dropped)");
				return false;
			}
			slotbuf[s] = a->buffer;
			base[s] = ofs;
			used[s] = b;
			in->layout.slot[s].pitch = stride;
			in->layout.slot[s].instance = 0;
			in->vb[s] = b->buf;
			in->vb_ofs[s] = ofs;
			nslots++;
		}
		la = &in->layout.attr[in->layout.num_attrs++];
		la->location = i;
		la->slot = (Uint32) s;
		la->format = (Uint32) fmt;
		la->offset = ofs - base[s];
	}
	for (s = 0; s < nslots; s++)
		in->reads[in->nreads++] = used[s];

	/* The current values of the standard inputs the VAO leaves off. */
	for (i = RGB_CONST_FIRST; i <= RGB_CONST_LAST; i++)
	{
		rgvattr_t	*la;

		if (v->attr[i].enabled)
			continue;
		if (in->const_slot < 0)
		{
			in->const_slot = nslots;
			in->layout.slot[nslots].instance = 1;
			in->vb[nslots] = NULL;		/* rg_frame.c's stream */
			nslots++;
		}
		la = &in->layout.attr[in->layout.num_attrs++];
		la->location = i;
		la->slot = (Uint32) in->const_slot;
		la->format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
		la->offset = (Uint32)(in->nconst * 4 * sizeof(float));
		memcpy (in->consts[in->nconst], rgb_current[i], sizeof(in->consts[0]));
		in->nconst++;
	}
	if (in->const_slot >= 0)
		in->layout.slot[in->const_slot].pitch = (Uint32)(in->nconst * 4 * sizeof(float));
	in->layout.num_slots = (Uint32) nslots;
	return true;
}

/* ------------------------------------------------------------------ */
/* The copy pass                                                       */
/* ------------------------------------------------------------------ */

Uint32 RG_Buf_PendingBytes (void)
{
	Uint32	n = 0;
	int	i;

	if (!rgb_pending_bytes)
		return 0;
	for (i = 1; i <= rgb_bufnames.used; i++)
	{
		rgbupload_t	*u;

		if (!RGB_BUFS[i].alive)
			continue;
		for (u = RGB_BUFS[i].uploads; u; u = u->next)
			n += (u->size + 15) & ~15u;
	}
	return n;
}

void RG_Buf_StageUploads (Uint8 *dst, Uint32 base)
{
	Uint32	off = 0;
	int	i;

	if (!rgb_pending_bytes)
		return;
	for (i = 1; i <= rgb_bufnames.used; i++)
	{
		rgbupload_t	*u;

		if (!RGB_BUFS[i].alive)
			continue;
		for (u = RGB_BUFS[i].uploads; u; u = u->next)
		{
			memcpy (dst + off, u->data, u->size);
			u->staged = base + off;
			off += (u->size + 15) & ~15u;
		}
	}
}

void RG_Buf_IssueUploads (SDL_GPUCopyPass *copy, SDL_GPUTransferBuffer *tb)
{
	SDL_GPUTransferBufferLocation	src;
	SDL_GPUBufferRegion		dst;
	int				i;

	if (!rgb_pending_bytes)
		return;
	for (i = 1; i <= rgb_bufnames.used; i++)
	{
		rgbuf_t		*b = &RGB_BUFS[i];
		rgbupload_t	*u;

		if (!b->alive || !b->uploads)
			continue;
		for (u = b->uploads; b->buf && u; u = u->next)
		{
			src.transfer_buffer = tb;
			src.offset = u->staged;
			dst.buffer = b->buf;
			dst.offset = u->offset;
			dst.size = u->size;
			/* cycle on a respecify: an earlier segment's or frame's draws
			 * still read the old contents */
			SDL_UploadToGPUBuffer (copy, &src, &dst, u->cycle);
		}
		RGB_DropUploads (b);
	}
}

void RG_Buf_EndFrame (void)
{
	int	i;

	for (i = 0; i < rgb_numretired; i++)
		SDL_ReleaseGPUBuffer (rg_dev, rgb_retired[i]);
	rgb_numretired = 0;
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

void RG_Buf_Init (void)
{
	int	i;

	rgb_array_buffer = 0;
	rgb_vao = 0;
	memset (&rgb_vao0, 0, sizeof(rgb_vao0));
	rgb_vao0.alive = true;
	/* GL's initial current attribute: (0, 0, 0, 1). */
	memset (rgb_current, 0, sizeof(rgb_current));
	for (i = 0; i < RG_MAX_VATTRS; i++)
		rgb_current[i][3] = 1.0f;
}

static void RGB_FreeTable (rgnames_t *t)
{
	free (t->items); t->items = NULL;
	free (t->free); t->free = NULL;
	t->count = t->used = t->numfree = t->maxfree = 0;
}

void RG_Buf_Shutdown (void)
{
	int	i;

	if (!rg_dev)
		return;
	for (i = 1; i <= rgb_bufnames.used; i++)
	{
		if (!RGB_BUFS[i].alive)
			continue;
		RGB_DropUploads (&RGB_BUFS[i]);
		if (RGB_BUFS[i].buf)
			SDL_ReleaseGPUBuffer (rg_dev, RGB_BUFS[i].buf);
	}
	RG_Buf_EndFrame ();
	RGB_FreeTable (&rgb_bufnames);
	RGB_FreeTable (&rgb_vaonames);
	free (rgb_retired); rgb_retired = NULL;
	rgb_numretired = rgb_maxretired = 0;
	rgb_pending_bytes = 0;
}

void RG_Buf_Status (void)
{
	int	i, alive = 0, made = 0, vaos = 0;
	Uint64	bytes = 0;

	for (i = 1; i <= rgb_bufnames.used; i++)
	{
		if (!RGB_BUFS[i].alive)
			continue;
		alive++;
		if (RGB_BUFS[i].buf)
		{
			made++;
			bytes += RGB_BUFS[i].size;
		}
	}
	for (i = 1; i <= rgb_vaonames.used; i++)
		vaos += RGB_VAOS[i].alive ? 1 : 0;
	Con_Printf ("[RENDERER] SDL_GPU buffers: %d names, %d created (%lu KB; %lu creates), "
		    "%d vertex arrays, %lu uploads / %lu KB, %u bytes queued\n",
		    alive, made, (unsigned long)(bytes / 1024), rgb_created, vaos,
		    rgb_uploads, rgb_upload_bytes / 1024, (unsigned int) rgb_pending_bytes);
}
