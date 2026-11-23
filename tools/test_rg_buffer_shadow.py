#!/usr/bin/env python3
"""rg_buffer.c's shadow of GL's buffer and vertex-array state is GL's
(GitHub #340, P2d).

tools/test_buffer_routing.py proves nothing reaches buffers or vertex state
behind the seams' back; tools/test_rg_sources.py pins the lines the -gpu
sweeps rest on.  This proves the shadow itself: it compiles the real
engine/h2shared/rg_buffer.c against a fake SDL_GPU whose buffers hold real
bytes and a fake frame recorder (segments, draws, flushes), then drives
the RG_* entry points the gl_buffer.c seams call and checks, case by case:

  * GL's binding rules: glVertexAttribPointer captures GL_ARRAY_BUFFER, a
    later rebind does not move it; GL_ELEMENT_ARRAY_BUFFER belongs to the
    bound VAO; the current attribute values are context state; deleting the
    bound VAO binds 0;
  * the layout an indexed draw gets: the world VAO's three attributes in one
    slot, the disabled standard inputs (ATTR_POSITION .. ATTR_COLOR) as an
    instance-rate FLOAT4 slot holding glVertexAttrib4f's values, GL's
    default (0,0,0,1) where none was set; the element buffer's index count;
  * uploads: glBufferData and glBufferSubData land in the SDL_GPU buffer
    once the copy pass runs, a respecify with cycling on, a sub-update
    without; a size change makes a new buffer and retires the old one
    until the frame ends;
  * the hazard: an upload into a buffer a draw recorded in the open segment
    reads ends the segment first (and one into a buffer nothing read, or a
    draw that was dropped, does not);
  * names past the first table growth (300 buffers): a sub-update into an
    early buffer still lands (an empty upload list's tail must not point
    into the moved table);
  * a deleted buffer's name, handed out again, is not drawn from by a VAO
    that still named the old one;
  * names are tagged: a name the null GL table made is not one of ours;
  * the skeletal mesh's VAO (#340 P2e): iqmvert_t's five attributes in one
    slot, the normal GL_BYTE normalized (BYTE4_NORM), the weights
    GL_UNSIGNED_BYTE normalized (UBYTE4_NORM), the bone indices integer
    (UBYTE4, glVertexAttribIPointer), and no constant slot;
  * storage (#340 P2e): every buffer is made with graphics storage-read
    usage; glBufferData fills one through GL_SHADER_STORAGE_BUFFER; a
    binding point's buffer is what a draw's storage slot gets, a range
    only at offset 0 and inside the buffer (SDL_GPU binds whole buffers),
    nothing for a uniform-target bind, nothing once the buffer is deleted;
    GL_Upload's arm hands each upload of a frame its own buffer at offset
    0, reuses them in order the next frame with cycling on, refuses a
    uniform target; an upload into a storage buffer a recorded draw of the
    open segment reads ends the segment.  Nothing bound is counted unless
    the caller excuses it (unbound_ok, #520), and the excuse covers that
    case alone: a refused range is counted either way.

Stock python3 and a C compiler (cc / gcc / clang); lint.yml.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "engine/h2shared/rg_buffer.c"
INTERNAL = ROOT / "engine/h2shared/rg_internal.h"
SHADER_H = ROOT / "engine/h2shared/gl_shader.h"

FORMATS = ("INVALID INT INT2 INT3 INT4 UINT UINT2 UINT3 UINT4 FLOAT FLOAT2 FLOAT3 FLOAT4 "
           "BYTE2 BYTE4 UBYTE2 UBYTE4 BYTE2_NORM BYTE4_NORM UBYTE2_NORM UBYTE4_NORM SHORT2 SHORT4 "
           "USHORT2 USHORT4 SHORT2_NORM SHORT4_NORM USHORT2_NORM USHORT4_NORM HALF2 HALF4").split()

QUAKEDEF = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef int qboolean;
#define true 1
#define false 0
typedef uint8_t Uint8;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int32_t Sint32;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_SHADER_STORAGE_BUFFER 0x90D2
void Sys_Error (const char *fmt, ...);
void Con_Printf (const char *fmt, ...);
'''

RG_INTERNAL = r'''
#include "quakedef.h"
#define SDL_zero(x) memset (&(x), 0, sizeof(x))
typedef enum { %(formats)s } SDL_GPUVertexElementFormat;
typedef Uint32 SDL_GPUBufferUsageFlags;
#define SDL_GPU_BUFFERUSAGE_VERTEX 1u
#define SDL_GPU_BUFFERUSAGE_INDEX 2u
#define SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ 4u
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUBuffer { Uint32 size, usage; Uint8 *data; int released, cycles; } SDL_GPUBuffer;
typedef struct SDL_GPUTransferBuffer { Uint8 *data; } SDL_GPUTransferBuffer;
typedef struct SDL_GPUCopyPass SDL_GPUCopyPass;
typedef struct { SDL_GPUBufferUsageFlags usage; Uint32 size; Uint32 props; } SDL_GPUBufferCreateInfo;
typedef struct { SDL_GPUTransferBuffer *transfer_buffer; Uint32 offset; } SDL_GPUTransferBufferLocation;
typedef struct { SDL_GPUBuffer *buffer; Uint32 offset, size; } SDL_GPUBufferRegion;
SDL_GPUBuffer *SDL_CreateGPUBuffer (SDL_GPUDevice *dev, const SDL_GPUBufferCreateInfo *ci);
void SDL_ReleaseGPUBuffer (SDL_GPUDevice *dev, SDL_GPUBuffer *b);
void SDL_UploadToGPUBuffer (SDL_GPUCopyPass *c, const SDL_GPUTransferBufferLocation *s,
			    const SDL_GPUBufferRegion *d, qboolean cycle);
const char *SDL_GetError (void);
extern SDL_GPUDevice *rg_dev;
void RG_NoteUnhandled (const char *what);
unsigned int RG_Frame_Segment (void);
qboolean RG_Frame_SegmentHasDraws (void);
void RG_Frame_Flush (void);
/* the backend's own entry points, from rg_device.h */
void RG_GenBuffers (GLsizei n, GLuint *buffers);
void RG_DeleteBuffers (GLsizei n, const GLuint *buffers);
void RG_BindBuffer (GLenum target, GLuint buffer);
void RG_BufferData (GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void RG_BufferSubData (GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
void RG_GenVertexArrays (GLsizei n, GLuint *arrays);
void RG_DeleteVertexArrays (GLsizei n, const GLuint *arrays);
void RG_BindVertexArray (GLuint array);
void RG_EnableVertexAttribArray (GLuint index, qboolean enable);
void RG_VertexAttribPointer (GLuint index, GLint size, GLenum type, GLboolean normalized,
			     qboolean integer, GLsizei stride, const void *pointer);
void RG_VertexAttrib4f (GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void RG_BindBufferBase (GLenum target, GLuint index, GLuint buffer);
void RG_BindBufferRange (GLenum target, GLuint index, GLuint buffer, GLintptr offset,
			 GLsizeiptr size);
void RG_Upload (GLenum target, const void *data, size_t numbytes, GLuint *outbuf,
		GLintptr *outofs);
/* the real header's layout and draw-input types, copied verbatim */
%(types)s
'''

HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include "rg_internal.h"
#include "gl_shader.h"
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893

SDL_GPUDevice *rg_dev = (SDL_GPUDevice *) 1;
static unsigned int seg = 1;
static int draws, flushes, unhandled, failures;
static SDL_GPUBuffer *made[4096];
static int nmade;

void Sys_Error (const char *fmt, ...) { va_list a; va_start (a, fmt); vprintf (fmt, a); va_end (a); exit (2); }
void Con_Printf (const char *fmt, ...) { (void) fmt; }
const char *SDL_GetError (void) { return "fake"; }
void RG_NoteUnhandled (const char *what) { (void) what; unhandled++; }
unsigned int RG_Frame_Segment (void) { return seg; }
qboolean RG_Frame_SegmentHasDraws (void) { return draws > 0; }
SDL_GPUBuffer *SDL_CreateGPUBuffer (SDL_GPUDevice *dev, const SDL_GPUBufferCreateInfo *ci)
{
	SDL_GPUBuffer *b = calloc (1, sizeof(*b));
	(void) dev;
	b->size = ci->size;
	b->usage = ci->usage;
	b->data = calloc (1, ci->size);
	made[nmade++] = b;
	return b;
}
void SDL_ReleaseGPUBuffer (SDL_GPUDevice *dev, SDL_GPUBuffer *b) { (void) dev; b->released++; }
static int cycled, plain;
void SDL_UploadToGPUBuffer (SDL_GPUCopyPass *c, const SDL_GPUTransferBufferLocation *s,
			    const SDL_GPUBufferRegion *d, qboolean cycle)
{
	(void) c;
	assert (d->offset + d->size <= d->buffer->size);
	memcpy (d->buffer->data + d->offset, s->transfer_buffer->data + s->offset, d->size);
	if (cycle) { cycled++; d->buffer->cycles++; } else plain++;
}
/* rg_frame.c's copy pass, then the segment's render pass and submit. */
static void CopyPass (void)
{
	Uint32 n = RG_Buf_PendingBytes ();
	SDL_GPUTransferBuffer tb;
	tb.data = calloc (1, n + 16);
	RG_Buf_StageUploads (tb.data, 0);
	RG_Buf_IssueUploads ((SDL_GPUCopyPass *) 1, &tb);
	free (tb.data);
}
void RG_Frame_Flush (void) { flushes++; CopyPass (); draws = 0; seg++; }
static void EndFrame (void) { RG_Frame_Flush (); RG_Buf_EndFrame (); }

#define CHECK(c, what) do { if (!(c)) { printf ("FAIL %s:%d: %s\n", __func__, __LINE__, what); failures++; } } while (0)

/* A draw recorded from the bound VAO, as RG_DrawIndexed does. */
static qboolean Draw (int isize, rgvinput_t *in)
{
	if (!RG_Buf_ForDraw (isize, in))
		return false;
	RG_Buf_MarkRead (in);
	draws++;
	return true;
}

typedef struct { float pos[3], st[2], lm[2]; } wv_t;

static void binding_rules (void)
{
	GLuint vao[2], buf[3];
	rgvinput_t in;
	wv_t v[4] = {{{0}}};
	unsigned int ix[6] = { 0, 1, 2, 0, 2, 3 };

	RG_GenVertexArrays (2, vao);
	RG_GenBuffers (3, buf);
	CHECK (vao[0] & 0x40000000u && buf[0] & 0x40000000u, "names are tagged");
	RG_BindVertexArray (vao[0]);
	RG_BindBuffer (GL_ARRAY_BUFFER, buf[0]);
	RG_BufferData (GL_ARRAY_BUFFER, sizeof(v), v, 0);
	RG_EnableVertexAttribArray (ATTR_POSITION, true);
	RG_VertexAttribPointer (ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, false, sizeof(wv_t), (void *) 0);
	RG_EnableVertexAttribArray (ATTR_TEXCOORD, true);
	RG_VertexAttribPointer (ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, false, sizeof(wv_t), (void *) 12);
	/* A rebind after the pointer does not move it: lmcoord reads buf[1]. */
	RG_BindBuffer (GL_ARRAY_BUFFER, buf[1]);
	RG_BufferData (GL_ARRAY_BUFFER, 64, NULL, 0);
	RG_EnableVertexAttribArray (ATTR_LMCOORD, true);
	RG_VertexAttribPointer (ATTR_LMCOORD, 2, GL_FLOAT, GL_FALSE, false, 8, (void *) 0);
	RG_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf[2]);
	RG_BufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof(ix), ix, 0);
	RG_VertexAttrib4f (ATTR_COLOR, 0.25f, 0.5f, 0.75f, 1.0f);

	CHECK (Draw (4, &in), "the VAO draws");
	CHECK (in.ib_count == 6, "element count is size / index width");
	CHECK (in.layout.num_slots == 3, "pos+tc share a slot, lm its own, the constants one");
	CHECK (in.layout.slot[0].pitch == sizeof(wv_t) && !in.layout.slot[0].instance, "slot 0: the world stride");
	CHECK (in.layout.attr[1].slot == 0 && in.layout.attr[1].offset == 12, "texcoord at 12 in slot 0");
	CHECK (in.layout.attr[2].slot == 1 && in.layout.slot[1].pitch == 8, "lmcoord from the buffer bound at its pointer");
	CHECK (in.const_slot == 2 && in.layout.slot[2].instance == 1 && in.nconst == 1, "color is the instance-rate constant");
	CHECK (in.layout.attr[3].location == ATTR_COLOR &&
	       in.layout.attr[3].format == SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, "color FLOAT4");
	CHECK (in.consts[0][0] == 0.25f && in.consts[0][2] == 0.75f, "the constant is glVertexAttrib4f's");
	CHECK (in.layout.slot[2].pitch == 16, "one FLOAT4 per constant");

	/* The element buffer is the VAO's: a second VAO has none. */
	RG_BindVertexArray (vao[1]);
	CHECK (!Draw (4, &in), "a VAO with no element buffer does not draw");
	RG_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf[2]);
	RG_EnableVertexAttribArray (ATTR_POSITION, true);
	RG_BindBuffer (GL_ARRAY_BUFFER, buf[0]);
	RG_VertexAttribPointer (ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, false, 12, (void *) 0);
	CHECK (Draw (4, &in) && in.nconst == 3, "position only: tc, lm, color constant");
	CHECK (in.consts[0][3] == 1.0f && in.consts[0][0] == 0.0f, "GL's default current value (0,0,0,1)");
	RG_BindVertexArray (vao[0]);
	CHECK (Draw (4, &in) && in.layout.num_slots == 3, "the first VAO is as it was");
	/* Deleting the bound VAO binds 0. */
	RG_DeleteVertexArrays (1, &vao[0]);
	CHECK (!Draw (4, &in), "no VAO bound after deleting the bound one");
	RG_DeleteVertexArrays (1, &vao[1]);
	RG_DeleteBuffers (3, buf);
	EndFrame ();
}

static void uploads_and_hazard (void)
{
	GLuint vao, vb, ib;
	rgvinput_t in;
	Uint8 a[64], b[64];
	unsigned short ix[3] = { 0, 1, 2 };
	int f0, i;

	for (i = 0; i < 64; i++) { a[i] = (Uint8) i; b[i] = (Uint8)(200 + i / 4); }
	RG_GenVertexArrays (1, &vao);
	RG_GenBuffers (1, &vb);
	RG_GenBuffers (1, &ib);
	RG_BindVertexArray (vao);
	RG_BindBuffer (GL_ARRAY_BUFFER, vb);
	RG_BufferData (GL_ARRAY_BUFFER, 64, a, 0);
	RG_EnableVertexAttribArray (ATTR_POSITION, true);
	RG_VertexAttribPointer (ATTR_POSITION, 4, GL_FLOAT, GL_FALSE, false, 16, (void *) 0);
	RG_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, ib);
	RG_BufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof(ix), ix, 0);
	f0 = flushes;
	cycled = plain = 0;
	CHECK (Draw (2, &in), "draws");
	CHECK (flushes == f0, "recording a draw flushes nothing");
	CopyPass ();	/* what the draw's segment would upload first */
	CHECK (!memcmp (in.vb[0]->data, a, 64), "glBufferData's bytes are in the buffer");
	CHECK (cycled == 2 && plain == 0, "a respecify uploads with cycling on");

	/* An upload into a buffer the open segment's draw reads ends the segment. */
	RG_BufferSubData (GL_ARRAY_BUFFER, 16, 16, b);
	CHECK (flushes == f0 + 1, "a sub-update into a buffer read by the open segment flushes first");
	CopyPass ();
	CHECK (plain == 1 && !memcmp (in.vb[0]->data + 16, b, 16) && !memcmp (in.vb[0]->data, a, 16),
	       "a sub-update lands without cycling and keeps the rest");
	RG_BufferData (GL_ARRAY_BUFFER, 64, b, 0);
	CHECK (flushes == f0 + 1, "nothing read it in this segment: no flush");
	CHECK (Draw (2, &in), "draws again");
	RG_BufferData (GL_ARRAY_BUFFER, 128, NULL, 0);
	CHECK (flushes == f0 + 2, "a respecify of a buffer the open segment reads flushes first");
	CHECK (!in.vb[0]->released, "the old buffer lives until the frame ends");
	EndFrame ();
	CHECK (in.vb[0]->released == 1, "the frame's end releases the retired buffer");

	/* A dropped draw stamps nothing. */
	f0 = flushes;
	RG_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
	CHECK (!Draw (2, &in), "no element buffer: dropped");
	RG_BufferData (GL_ARRAY_BUFFER, 128, b, 0);
	CHECK (flushes == f0, "a dropped draw does not make the next upload flush");
	RG_DeleteVertexArrays (1, &vao);
	RG_DeleteBuffers (1, &vb);
	RG_DeleteBuffers (1, &ib);
	EndFrame ();
}

static void table_growth (void)
{
	GLuint names[300], vao;
	Uint8 bytes[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	Uint8 zero[16] = { 0 };
	rgvinput_t in;
	unsigned short ix[3] = { 0, 1, 2 };

	RG_GenBuffers (1, &names[0]);
	RG_BindBuffer (GL_ARRAY_BUFFER, names[0]);
	RG_BufferData (GL_ARRAY_BUFFER, 16, zero, 0);
	EndFrame ();
	RG_GenBuffers (299, &names[1]);	/* the table grows under names[0] */
	RG_BindBuffer (GL_ARRAY_BUFFER, names[0]);
	RG_BufferSubData (GL_ARRAY_BUFFER, 0, 16, bytes);
	RG_GenVertexArrays (1, &vao);
	RG_BindVertexArray (vao);
	RG_EnableVertexAttribArray (ATTR_POSITION, true);
	RG_VertexAttribPointer (ATTR_POSITION, 4, GL_FLOAT, GL_FALSE, false, 16, (void *) 0);
	RG_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, names[299]);
	RG_BufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof(ix), ix, 0);
	EndFrame ();
	CHECK (Draw (2, &in) && !memcmp (in.vb[0]->data, bytes, 16),
	       "a sub-update into a buffer named before the table grew lands");
	RG_DeleteVertexArrays (1, &vao);
	RG_DeleteBuffers (300, names);
	EndFrame ();
}

static void stale_name (void)
{
	GLuint vao[2], vb, ib, other;
	rgvinput_t in;
	unsigned short ix[3] = { 0, 1, 2 };
	Uint8 v[48] = { 0 };

	RG_GenVertexArrays (2, vao);
	RG_GenBuffers (1, &vb);
	RG_GenBuffers (1, &ib);
	RG_BindVertexArray (vao[0]);
	RG_BindBuffer (GL_ARRAY_BUFFER, vb);
	RG_BufferData (GL_ARRAY_BUFFER, sizeof(v), v, 0);
	RG_EnableVertexAttribArray (ATTR_POSITION, true);
	RG_VertexAttribPointer (ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, false, 12, (void *) 0);
	RG_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, ib);
	RG_BufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof(ix), ix, 0);
	/* Deleted while another VAO is bound; the name comes straight back. */
	RG_BindVertexArray (vao[1]);
	RG_DeleteBuffers (1, &vb);
	RG_GenBuffers (1, &other);
	CHECK (other == vb, "the free list hands the name back");
	RG_BindBuffer (GL_ARRAY_BUFFER, other);
	RG_BufferData (GL_ARRAY_BUFFER, 48, v, 0);
	RG_BindVertexArray (vao[0]);
	CHECK (!Draw (2, &in), "a VAO that named the deleted buffer does not draw from its successor");
	RG_DeleteVertexArrays (2, vao);
	RG_DeleteBuffers (1, &other);
	RG_DeleteBuffers (1, &ib);
	/* A name the null GL table made is not ours. */
	unhandled = 0;
	RG_BindVertexArray (7);
	CHECK (unhandled == 1 && !Draw (2, &in), "an untagged VAO name is unknown, its draws dropped");
	RG_BindVertexArray (0);
	EndFrame ();
}

typedef struct { float pos[3]; signed char n[4]; float st[2]; Uint8 w[4], ix[4]; } iqmv_t;

static void skeletal_layout (void)
{
	GLuint vao, vb, ib;
	rgvinput_t in;
	iqmv_t v[3];
	unsigned short ix[3] = { 0, 1, 2 };
	int i, seen = 0;

	memset (v, 0, sizeof(v));
	RG_GenVertexArrays (1, &vao);
	RG_GenBuffers (1, &vb);
	RG_GenBuffers (1, &ib);
	RG_BindVertexArray (vao);
	RG_BindBuffer (GL_ARRAY_BUFFER, vb);
	RG_BufferData (GL_ARRAY_BUFFER, sizeof(v), v, 0);
	/* gl_mesh.c's skeletal VAO, call for call */
	RG_EnableVertexAttribArray (0, true);
	RG_VertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, false, sizeof(iqmv_t), (void *) 0);
	RG_EnableVertexAttribArray (1, true);
	RG_VertexAttribPointer (1, 4, GL_BYTE, GL_TRUE, false, sizeof(iqmv_t), (void *) 12);
	RG_EnableVertexAttribArray (2, true);
	RG_VertexAttribPointer (2, 2, GL_FLOAT, GL_FALSE, false, sizeof(iqmv_t), (void *) 16);
	RG_EnableVertexAttribArray (3, true);
	RG_VertexAttribPointer (3, 4, GL_UNSIGNED_BYTE, GL_TRUE, false, sizeof(iqmv_t), (void *) 24);
	RG_EnableVertexAttribArray (4, true);
	RG_VertexAttribPointer (4, 4, GL_UNSIGNED_BYTE, GL_FALSE, true, sizeof(iqmv_t), (void *) 28);
	RG_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, ib);
	RG_BufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof(ix), ix, 0);
	CHECK (Draw (2, &in), "the skeletal VAO draws");
	CHECK (in.layout.num_slots == 1 && in.const_slot < 0, "one slot, every standard input enabled: no constants");
	CHECK (in.layout.slot[0].pitch == 32, "iqmvert_t's 32-byte stride");
	for (i = 0; i < (int) in.layout.num_attrs; i++)
	{
		const rgvattr_t *a = &in.layout.attr[i];
		static const Uint32 fmt[5] = { SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM,
			SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
			SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4 };
		static const Uint32 ofs[5] = { 0, 12, 16, 24, 28 };

		if (a->location > 4)
			continue;
		seen |= 1 << a->location;
		CHECK (a->format == fmt[a->location], "each skeletal input's SDL_GPU format");
		CHECK (a->offset == ofs[a->location] && a->slot == 0, "each skeletal input's offset in the one slot");
	}
	CHECK (seen == 0x1F, "all five skeletal inputs");
	RG_DeleteVertexArrays (1, &vao);
	RG_DeleteBuffers (1, &vb);
	RG_DeleteBuffers (1, &ib);
	EndFrame ();
}

static void storage (void)
{
	GLuint sb, other, n1, n2, n3;
	GLintptr o1 = 99, o2 = 99;
	Uint8 bytes[64], more[80];
	SDL_GPUBuffer *g, *g1;
	void *read;
	int i, f0, c0;

	for (i = 0; i < 64; i++) bytes[i] = (Uint8)(i * 3);
	for (i = 0; i < 80; i++) more[i] = (Uint8)(250 - i);
	RG_GenBuffers (1, &sb);
	RG_BindBuffer (GL_SHADER_STORAGE_BUFFER, sb);
	RG_BufferData (GL_SHADER_STORAGE_BUFFER, 64, bytes, 0);
	RG_BindBuffer (GL_SHADER_STORAGE_BUFFER, 0);
	CopyPass ();
	unhandled = 0;
	CHECK (!RG_Buf_Storage (4, false, &read) && !read && unhandled == 1,
	       "nothing bound at the point: no buffer, and counted");
	unhandled = 0;
	CHECK (!RG_Buf_Storage (4, true, &read) && !read && unhandled == 0,
	       "nothing bound at a point the draw provably does not read: no buffer, not counted (#520)");
	RG_BindBufferBase (GL_SHADER_STORAGE_BUFFER, 4, sb);
	g = RG_Buf_Storage (4, false, &read);
	CHECK (g && read, "a base bind is what the draw's slot gets");
	CHECK (RG_Buf_Storage (4, true, &read) == g && read, "an excused slot still gets a bound buffer");
	CHECK (g && (g->usage & SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ), "made with storage-read usage");
	CHECK (g && !memcmp (g->data, bytes, 64), "glBufferData through the storage target filled it");
	CHECK (!RG_Buf_Storage (3, false, &read), "the other points are untouched");
	unhandled = 0;
	RG_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 4, sb, 16, 32);
	CHECK (!RG_Buf_Storage (4, false, &read) && unhandled == 1,
	       "a range at a non-zero offset is refused, not bound whole from 0");
	CHECK (!RG_Buf_Storage (4, true, &read) && unhandled == 2,
	       "unbound_ok excuses nothing bound only: a refused range is still counted (#520)");
	RG_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 4, sb, 0, 32);
	CHECK (RG_Buf_Storage (4, false, &read) == g, "a range at offset 0 binds the buffer");
	RG_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 4, sb, 0, 128);
	CHECK (!RG_Buf_Storage (4, false, &read), "a range past the buffer is refused");
	RG_BindBufferBase (GL_SHADER_STORAGE_BUFFER, 4, sb);
	RG_BindBufferBase (GL_UNIFORM_BUFFER, 4, 0);
	CHECK (RG_Buf_Storage (4, false, &read) == g, "a uniform-target bind leaves the storage point alone");

	/* the hazard: a recorded draw read it in the open segment */
	f0 = flushes;
	RG_Buf_StampRead (read);
	draws++;
	RG_BindBuffer (GL_SHADER_STORAGE_BUFFER, sb);
	RG_BufferSubData (GL_SHADER_STORAGE_BUFFER, 0, 16, more);
	CHECK (flushes == f0 + 1, "a storage upload a recorded draw reads ends the segment first");

	/* GL deletes a buffer out of every binding point */
	RG_DeleteBuffers (1, &sb);
	CHECK (!RG_Buf_Storage (4, false, &read), "a deleted buffer leaves its binding point");
	/* and the name, handed straight back out, is not read through the point */
	RG_GenBuffers (1, &other);
	CHECK (other == sb, "the free list hands the name back");
	RG_BindBuffer (GL_SHADER_STORAGE_BUFFER, other);
	RG_BufferData (GL_SHADER_STORAGE_BUFFER, 64, bytes, 0);
	CHECK (!RG_Buf_Storage (4, false, &read), "the point does not read the buffer that inherited the name");
	RG_DeleteBuffers (1, &other);
	EndFrame ();

	/* GL_Upload: a buffer per upload, at offset 0 */
	RG_Upload (GL_SHADER_STORAGE_BUFFER, bytes, 64, &n1, &o1);
	RG_Upload (GL_SHADER_STORAGE_BUFFER, more, 80, &n2, &o2);
	CHECK (n1 && n2 && n1 != n2, "two uploads in a frame, two buffers");
	CHECK (o1 == 0 && o2 == 0, "every upload at offset 0");
	RG_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, n1, o1, 64);
	g1 = RG_Buf_Storage (0, false, &read);
	CopyPass ();
	CHECK (g1 && !memcmp (g1->data, bytes, 64), "the upload is in its buffer");
	CHECK (g1 && (g1->usage & SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ), "a stream buffer is storage too");
	EndFrame ();
	c0 = g1 ? g1->cycles : 0;
	RG_Upload (GL_SHADER_STORAGE_BUFFER, more, 80, &n3, &o1);
	CHECK (n3 == n1, "the next frame's first upload reuses the first buffer");
	CopyPass ();
	CHECK (g1 && g1->cycles == c0 + 1 && !memcmp (g1->data, more, 80),
	       "reused with cycling on: the last frame's draws keep their bytes");
	unhandled = 0;
	RG_Upload (GL_UNIFORM_BUFFER, bytes, 64, &n3, &o1);
	CHECK (n3 == 0 && unhandled == 1, "uniform data is not streamed under -gpu");
	RG_BindBufferBase (GL_SHADER_STORAGE_BUFFER, 0, 0);
	EndFrame ();
}

int main (void)
{
	RG_Buf_Init ();
	binding_rules ();
	uploads_and_hazard ();
	table_growth ();
	stale_name ();
	skeletal_layout ();
	storage ();
	RG_Buf_Shutdown ();
	if (failures)
		return 1;
	printf ("ok\n");
	return 0;
}
'''


def types_block():
    h = INTERNAL.read_text()
    a = h.index("#define RG_MAX_VBUFS")
    b = h.index("} rgvlayout_t;") + len("} rgvlayout_t;")
    c = h.index("typedef struct {\n\trgvlayout_t\tlayout;")
    d = h.index("} rgvinput_t;") + len("} rgvinput_t;")
    decl = re.findall(r"^(?:qboolean|void|Uint32|SDL_GPUBuffer\s*\*)\s*RG_Buf_\w+\s*\([^;]*\);", h, re.M)
    return h[a:b] + "\n" + h[c:d] + "\n" + "\n".join(decl) + "\n"


def main():
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        sys.exit("FAIL: no C compiler")
    with tempfile.TemporaryDirectory() as td:
        t = Path(td)
        (t / "quakedef.h").write_text(QUAKEDEF)
        (t / "rg_internal.h").write_text(RG_INTERNAL % {
            "formats": ", ".join("SDL_GPU_VERTEXELEMENTFORMAT_" + f for f in FORMATS),
            "types": types_block()})
        attrs = re.findall(r"#define\s+(ATTR_\w+)\s+(\d+)", SHADER_H.read_text())
        (t / "gl_shader.h").write_text("".join("#define %s %s\n" % a for a in attrs))
        (t / "harness.c").write_text(HARNESS)
        # Its own quoted includes would find the real headers beside it.
        shutil.copy2(SRC, t / "rg_buffer.c")
        exe = t / "h"
        r = subprocess.run([cc, "-std=gnu99", "-Wall", "-Werror=implicit-function-declaration",
                            "-DUSE_SDL_GPU", "-I", td, "-o", str(exe), str(t / "harness.c"), str(t / "rg_buffer.c")],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit("FAIL: rg_buffer.c does not build against the harness:\n" + r.stderr[-3000:])
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            sys.exit("FAIL: rg_buffer.c's shadow disagrees with GL:\n" + (r.stdout + r.stderr)[-3000:])
    print("PASS rg_buffer.c's buffer and vertex-array shadow keeps GL's binding rules, uploads, the "
          "segment hazard, table growth and name reuse (#340 P2d), the skeletal layout, storage "
          "binding points and the upload stream (#340 P2e)")


if __name__ == "__main__":
    main()
