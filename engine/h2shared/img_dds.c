/*
 * img_dds.c -- DDS and KTX loading for pre-compressed replacement textures.
 *
 * A high-res replacement pack decoded to RGBA costs 4 bytes per texel plus a
 * third again for the mip chain: a single 2048x2048 texture is ~21 MB of VRAM,
 * and every PNG in the pack is also being inflated on the CPU at map load.
 * Handing the driver a block-compressed payload instead cuts that by 4x (BC2 /
 * BC3) or 8x (BC1), skips the decode entirely, and brings the pack's own mip
 * chain along so the engine does not have to build one.
 *
 * Nothing here touches GL state -- these functions only parse a container into
 * an imgreplace_t.  The upload lives in gl_draw.c (GL_LoadReplacement).
 *
 * Only the shapes a texture pack actually ships are accepted: single-face,
 * non-array, 2D, block-compressed.  Cube maps, volume textures, arrays and
 * uncompressed containers are rejected so the caller falls through to the
 * PNG/TGA/PCX path rather than being handed something it cannot upload.
 *
 * uhexen2-0vgo.5
 *
 * Copyright (C) 2026 uHexen2 project
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
#include "img_load.h"

/* The GL_COMPRESSED_* internal formats this file maps onto live in glquake.h,
 * which the upload side (gl_draw.c) needs too. */

/*
=================
IMG_ReadU32

DDS is little-endian by definition and KTX declares its endianness in the
header (we only accept little-endian, see IMG_LoadKTX).  Assembling the value
by hand keeps this correct on a big-endian host without dragging byte-swap
macros through every field read.
=================
*/
static unsigned int IMG_ReadU32 (const byte *p)
{
	return  (unsigned int) p[0]        |
	       ((unsigned int) p[1] <<  8) |
	       ((unsigned int) p[2] << 16) |
	       ((unsigned int) p[3] << 24);
}

/*
=================
IMG_BlockBytes

Bytes per 4x4 block for a supported compressed format, or 0 if we do not
handle it.  Doubles as the format whitelist: anything returning 0 is rejected
before it can reach the driver.
=================
*/
static int IMG_BlockBytes (unsigned int glformat)
{
	switch (glformat)
	{
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RED_RGTC1:
		return 8;

	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
	case GL_COMPRESSED_RG_RGTC2:
	case GL_COMPRESSED_RGBA_BPTC_UNORM:
	case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:
		return 16;
	}

	return 0;
}

/*
=================
IMG_FormatHasAlpha

Whether sampling this format can return alpha below 1.0.  Drives TEX_ALPHA on
the resulting texture, which in turn decides the swizzle GL_Upload32's RGBA
path sets by hand (see the GL_TEXTURE_SWIZZLE_A comment in gl_draw.c).
=================
*/
static qboolean IMG_FormatHasAlpha (unsigned int glformat)
{
	switch (glformat)
	{
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
	case GL_COMPRESSED_RGBA_BPTC_UNORM:
	case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:
		return true;
	}

	return false;
}

/*
=================
IMG_FormatSupported

Whether this GL context can accept the format.  The three families ship
independently: S3TC is an extension on every driver, while RGTC and BPTC are
core from GL 3.0 and 4.2 but absent on the ES tier.  gl_vidsdl.c probes all
three at context creation.
=================
*/
static qboolean IMG_FormatSupported (unsigned int glformat)
{
	switch (glformat)
	{
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		return gl_have_s3tc;

	case GL_COMPRESSED_RED_RGTC1:
	case GL_COMPRESSED_RG_RGTC2:
		return gl_have_rgtc;

	case GL_COMPRESSED_RGBA_BPTC_UNORM:
	case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:
		return gl_have_bptc;
	}

	return false;
}

/*
=================
IMG_MipBytes

Payload size of one mip level.  Block formats round the dimensions up to a
whole block, so a 1x1 level still costs a full block -- getting this wrong
walks off the end of the file, which is why it is factored out.
=================
*/
static size_t IMG_MipBytes (int w, int h, int blockbytes)
{
	size_t	bw = (size_t)((w + 3) / 4);
	size_t	bh = (size_t)((h + 3) / 4);

	if (bw < 1) bw = 1;
	if (bh < 1) bh = 1;

	return bw * bh * (size_t)blockbytes;
}

/*
=================
IMG_BuildMipChain

Fill in the mip tables and copy the payload out of the file buffer.

filedata/filesize describe the whole file; payload_ofs is where level 0 starts.
Levels are packed back to back with no padding, which is true for DDS and for
KTX once its per-level size prefix has been stripped (KTX pads to 4 bytes, but
every block size is already a multiple of 8, so the padding is always zero).

Returns false if the declared mip chain does not fit in the file -- a
truncated or lying header must not become an out-of-bounds read.
=================
*/
static qboolean IMG_BuildMipChain (imgreplace_t *out, const byte *filedata, size_t filesize,
				   size_t payload_ofs, int nummips, int blockbytes)
{
	int	i;
	int	w = out->width;
	int	h = out->height;
	size_t	total = 0;

	if (nummips < 1)
		nummips = 1;
	if (nummips > IMG_MAX_MIPS)
		nummips = IMG_MAX_MIPS;

	/* First pass: lay out the chain and check it fits. */
	for (i = 0; i < nummips; i++)
	{
		size_t	sz = IMG_MipBytes (w, h, blockbytes);

		out->mipw[i] = w;
		out->miph[i] = h;
		out->mipofs[i] = total;
		out->mipsize[i] = sz;
		total += sz;

		if (w == 1 && h == 1 && i + 1 < nummips)
		{
			/* Header claims more levels than the chain can hold.
			 * Trust the geometry, not the count. */
			nummips = i + 1;
			break;
		}

		w >>= 1; if (w < 1) w = 1;
		h >>= 1; if (h < 1) h = 1;
	}

	if (payload_ofs > filesize || total > filesize - payload_ofs)
		return false;

	out->blocks = (byte *) malloc (total);
	if (!out->blocks)
		return false;

	memcpy (out->blocks, filedata + payload_ofs, total);
	out->nummips = nummips;
	return true;
}

/*
=================
IMG_ReadWholeFile

Slurp a file through the quake filesystem (so pak members work).  Returns the
malloc'd contents and writes the length to *outsize, or NULL.
=================
*/
static byte *IMG_ReadWholeFile (const char *filename, size_t *outsize)
{
	FILE	*f;
	byte	*data;
	long	size;

	size = FS_OpenFile_Silent (filename, &f, NULL);
	if (!f || size <= 0)
	{
		if (f)
			fclose (f);
		return NULL;
	}

	data = (byte *) malloc ((size_t)size);
	if (!data)
	{
		fclose (f);
		return NULL;
	}

	if (fread (data, 1, (size_t)size, f) != (size_t)size)
	{
		fclose (f);
		free (data);
		return NULL;
	}

	fclose (f);
	*outsize = (size_t)size;
	return data;
}

/* DDS pixel-format flags we care about */
#define DDPF_ALPHAPIXELS	0x00000001
#define DDPF_FOURCC		0x00000004

/* DDS caps2 bits that mark shapes we refuse */
#define DDSCAPS2_CUBEMAP	0x00000200
#define DDSCAPS2_VOLUME		0x00200000

#define DDS_FOURCC(a,b,c,d)	((unsigned int)(a) | ((unsigned int)(b) << 8) | \
				 ((unsigned int)(c) << 16) | ((unsigned int)(d) << 24))

/*
=================
IMG_DXGIToGL

Map the DXGI format of a DX10-extended DDS onto a GL internal format.
Returns 0 for anything we do not handle.  The _SRGB variants deliberately fall
onto the plain UNORM enums: the engine's lightmap pipeline works in whatever
space the source art is authored in, and silently switching one texture to
sRGB decode would make it disagree with every neighbouring surface.
=================
*/
static unsigned int IMG_DXGIToGL (unsigned int dxgi)
{
	switch (dxgi)
	{
	case 70: case 71: case 72:	/* BC1_TYPELESS / _UNORM / _UNORM_SRGB */
		return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	case 73: case 74: case 75:	/* BC2 */
		return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	case 76: case 77: case 78:	/* BC3 */
		return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	case 79: case 80:		/* BC4_TYPELESS / _UNORM */
		return GL_COMPRESSED_RED_RGTC1;
	case 82: case 83:		/* BC5_TYPELESS / _UNORM */
		return GL_COMPRESSED_RG_RGTC2;
	case 97: case 98: case 99:	/* BC7_TYPELESS / _UNORM / _UNORM_SRGB */
		return GL_COMPRESSED_RGBA_BPTC_UNORM;
	}

	return 0;
}

/*
=================
IMG_LoadDDS

Parse a DDS container.  Layout: 4-byte magic, 124-byte DDS_HEADER, then either
the payload or -- when the pixel format's fourCC is 'DX10' -- a further 20-byte
DDS_HEADER_DXT10 first.
=================
*/
qboolean IMG_LoadDDS (const char *filename, imgreplace_t *out)
{
	byte		*filedata;
	size_t		filesize = 0;
	unsigned int	flags, height, width, mipcount;
	unsigned int	pf_flags, pf_fourcc, caps2;
	unsigned int	glformat = 0;
	size_t		payload_ofs = 4 + 124;
	int		blockbytes;
	int		nummips;

	filedata = IMG_ReadWholeFile (filename, &filesize);
	if (!filedata)
		return false;

	if (filesize < 128 || IMG_ReadU32 (filedata) != DDS_FOURCC('D','D','S',' '))
		goto fail;

	/* File offsets, not header-relative: the 124-byte DDS_HEADER starts at 4.
	 *   4 dwSize   8 dwFlags  12 dwHeight  16 dwWidth  20 dwPitchOrLinearSize
	 *  24 dwDepth 28 dwMipMapCount  32..75 dwReserved1[11]
	 *  76 ddspf (32 bytes: 76 dwSize, 80 dwFlags, 84 dwFourCC, 88 dwRGBBitCount,
	 *            92..107 masks)
	 * 108 dwCaps 112 dwCaps2 116 dwCaps3 120 dwCaps4 124 dwReserved2
	 * which puts the payload (or the DX10 header) at 128. */
	if (IMG_ReadU32 (filedata + 4) != 124)
		goto fail;

	flags    = IMG_ReadU32 (filedata + 8);
	height   = IMG_ReadU32 (filedata + 12);
	width    = IMG_ReadU32 (filedata + 16);
	mipcount = IMG_ReadU32 (filedata + 28);

	pf_flags  = IMG_ReadU32 (filedata + 80);
	pf_fourcc = IMG_ReadU32 (filedata + 84);

	caps2 = IMG_ReadU32 (filedata + 112);

	if (width < 1 || height < 1 || width > 32768 || height > 32768)
		goto fail;

	/* Cube maps and volumes need upload paths we do not have. */
	if (caps2 & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME))
		goto fail;

	/* Uncompressed DDS is not worth supporting: it costs the same VRAM as
	 * the PNG path with none of the tooling, and packs that ship it can
	 * ship a PNG instead. */
	if (!(pf_flags & DDPF_FOURCC))
		goto fail;

	switch (pf_fourcc)
	{
	case DDS_FOURCC('D','X','T','1'):
		/* BC1 always carries a punch-through alpha bit per block.  Honour
		 * what the file declares rather than always picking the RGBA enum:
		 * an opaque texture whose encoder happened to emit 3-colour blocks
		 * would otherwise sprout transparent texels. */
		glformat = (pf_flags & DDPF_ALPHAPIXELS) ?
			   GL_COMPRESSED_RGBA_S3TC_DXT1_EXT :
			   GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
		break;
	case DDS_FOURCC('D','X','T','3'):
		glformat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
		break;
	case DDS_FOURCC('D','X','T','5'):
		glformat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
		break;
	case DDS_FOURCC('A','T','I','1'):
	case DDS_FOURCC('B','C','4','U'):
		glformat = GL_COMPRESSED_RED_RGTC1;
		break;
	case DDS_FOURCC('A','T','I','2'):
	case DDS_FOURCC('B','C','5','U'):
		glformat = GL_COMPRESSED_RG_RGTC2;
		break;
	case DDS_FOURCC('D','X','1','0'):
		if (filesize < 148)
			goto fail;
		/* DDS_HEADER_DXT10: dxgiFormat, resourceDimension, miscFlag,
		 * arraySize, miscFlags2 */
		glformat = IMG_DXGIToGL (IMG_ReadU32 (filedata + 128));
		if (IMG_ReadU32 (filedata + 132) != 3)	/* D3D10_RESOURCE_DIMENSION_TEXTURE2D */
			goto fail;
		if (IMG_ReadU32 (filedata + 140) > 1)	/* arraySize */
			goto fail;
		payload_ofs = 148;
		break;
	}

	if (!glformat)
		goto fail;

	blockbytes = IMG_BlockBytes (glformat);
	if (!blockbytes)
		goto fail;

	if (!IMG_FormatSupported (glformat))
	{
		Con_DPrintf ("%s: unsupported compressed format, ignoring\n", filename);
		goto fail;
	}

	/* DDSD_MIPMAPCOUNT is 0x20000; without it the count field is undefined. */
	nummips = (flags & 0x20000) ? (int)mipcount : 1;
	if (nummips < 1)
		nummips = 1;

	memset (out, 0, sizeof(*out));
	out->width = (int)width;
	out->height = (int)height;
	out->glformat = glformat;
	out->has_alpha = IMG_FormatHasAlpha (glformat);

	if (!IMG_BuildMipChain (out, filedata, filesize, payload_ofs, nummips, blockbytes))
	{
		Con_DPrintf ("%s: truncated mip chain, ignoring\n", filename);
		memset (out, 0, sizeof(*out));
		goto fail;
	}

	free (filedata);

	if (developer.value >= 2)
		Con_Printf ("Loaded compressed texture: %s (%dx%d, %d mips)\n",
			    filename, out->width, out->height, out->nummips);
	return true;

fail:
	free (filedata);
	return false;
}

/*
=================
IMG_LoadKTX

Parse a KTX 1.1 container.  Unlike DDS, KTX stores the GL internal format
directly, so there is no format table -- but it also prefixes every level with
its own size and pads levels to 4 bytes, so the payload has to be repacked
rather than copied wholesale.
=================
*/
qboolean IMG_LoadKTX (const char *filename, imgreplace_t *out)
{
	static const byte ktx_magic[12] =
		{ 0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n' };

	byte		*filedata;
	size_t		filesize = 0;
	unsigned int	endianness, gltype, glformat_in, glinternal;
	unsigned int	width, height, depth, arrayelems, faces, levels, kvbytes;
	size_t		cursor, total;
	int		blockbytes;
	int		i, w, h;

	filedata = IMG_ReadWholeFile (filename, &filesize);
	if (!filedata)
		return false;

	if (filesize < 64 || memcmp (filedata, ktx_magic, 12) != 0)
		goto fail;

	endianness = IMG_ReadU32 (filedata + 12);
	if (endianness != 0x04030201)
		goto fail;		/* byte-swapped KTX: vanishingly rare, not worth the code */

	gltype      = IMG_ReadU32 (filedata + 16);
	glformat_in = IMG_ReadU32 (filedata + 24);
	glinternal  = IMG_ReadU32 (filedata + 28);
	width       = IMG_ReadU32 (filedata + 36);
	height      = IMG_ReadU32 (filedata + 40);
	depth       = IMG_ReadU32 (filedata + 44);
	arrayelems  = IMG_ReadU32 (filedata + 48);
	faces       = IMG_ReadU32 (filedata + 52);
	levels      = IMG_ReadU32 (filedata + 56);
	kvbytes     = IMG_ReadU32 (filedata + 60);

	/* glType and glFormat are both zero exactly when the payload is
	 * compressed; anything else is an uncompressed KTX we do not want. */
	if (gltype != 0 || glformat_in != 0)
		goto fail;

	if (width < 1 || width > 32768 || height < 1 || height > 32768)
		goto fail;
	if (depth > 1 || arrayelems > 1 || faces != 1)
		goto fail;

	blockbytes = IMG_BlockBytes (glinternal);
	if (!blockbytes)
		goto fail;

	if (!IMG_FormatSupported (glinternal))
	{
		Con_DPrintf ("%s: unsupported compressed format, ignoring\n", filename);
		goto fail;
	}

	if (levels < 1)
		levels = 1;		/* 0 means "generate mips"; we just take level 0 */
	if (levels > IMG_MAX_MIPS)
		levels = IMG_MAX_MIPS;

	cursor = 64 + (size_t)kvbytes;
	if (cursor > filesize)
		goto fail;

	memset (out, 0, sizeof(*out));
	out->width = (int)width;
	out->height = (int)height;
	out->glformat = glinternal;
	out->has_alpha = IMG_FormatHasAlpha (glinternal);

	/* First pass: walk the level headers to size the repacked payload and
	 * confirm every level is actually present. */
	total = 0;
	w = (int)width;
	h = (int)height;
	for (i = 0; i < (int)levels; i++)
	{
		unsigned int	imagesize;
		size_t		expect = IMG_MipBytes (w, h, blockbytes);

		if (cursor + 4 > filesize)
			goto fail;
		imagesize = IMG_ReadU32 (filedata + cursor);
		cursor += 4;

		if ((size_t)imagesize != expect || cursor + expect > filesize)
			goto fail;

		out->mipw[i] = w;
		out->miph[i] = h;
		out->mipofs[i] = total;
		out->mipsize[i] = expect;
		total += expect;

		cursor += expect;
		cursor = (cursor + 3) & ~(size_t)3;	/* mipPadding */

		if (w == 1 && h == 1)
		{
			levels = (unsigned int)(i + 1);
			break;
		}

		w >>= 1; if (w < 1) w = 1;
		h >>= 1; if (h < 1) h = 1;
	}

	out->blocks = (byte *) malloc (total);
	if (!out->blocks)
		goto fail;
	out->nummips = (int)levels;

	/* Second pass: copy each level into the packed buffer. */
	cursor = 64 + (size_t)kvbytes;
	for (i = 0; i < (int)levels; i++)
	{
		cursor += 4;
		memcpy (out->blocks + out->mipofs[i], filedata + cursor, out->mipsize[i]);
		cursor += out->mipsize[i];
		cursor = (cursor + 3) & ~(size_t)3;
	}

	free (filedata);

	if (developer.value >= 2)
		Con_Printf ("Loaded compressed texture: %s (%dx%d, %d mips)\n",
			    filename, out->width, out->height, out->nummips);
	return true;

fail:
	memset (out, 0, sizeof(*out));
	free (filedata);
	return false;
}
