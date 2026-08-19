/*
 * web_canvas.h -- accelerated presentation surface for the 8bpp software
 * renderer on the web platform.
 *
 * The software rasterizer owns a palettized (1 byte per pixel) framebuffer.
 * This module is the only thing that talks to the GPU in the software
 * renderer configuration: it uploads that framebuffer as a single-channel
 * texture, expands it through a 256-entry palette lookup on the GPU, and
 * scales it to the canvas.  See docs/web/SOFTWARE_RENDERER.md.
 *
 * Backends live behind this interface so a second one (WebGPU) can be added
 * without touching the engine.  Exactly one backend is linked at a time.
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

#ifndef __HX2_WEB_CANVAS_H
#define __HX2_WEB_CANVAS_H

/* Create the GPU context bound to the page canvas.  Fatal on failure:
 * there is no non-accelerated fallback on the iOS PWA target. */
void WebCanvas_Init (void);
void WebCanvas_Shutdown (void);

/* Human readable backend id, e.g. "webgl2".  Valid after WebCanvas_Init. */
const char *WebCanvas_BackendName (void);

/* Declare the size of the palettized source framebuffer.  Reallocates the
 * upload texture; call whenever the software resolution changes. */
void WebCanvas_SetSource (int width, int height);

/* 256 RGB triples.  Cheap: only re-uploads the 256x1 LUT texture, so the
 * per-frame damage/bonus/water palette shifts cost nothing per pixel. */
void WebCanvas_SetPalette (const byte *palette);

/* false: nearest neighbour (classic crunchy software look).
 * true:  pixel-art antialiasing, only visible at non-integer scale. */
void WebCanvas_SetFilter (qboolean smooth);

/* Upload `pixels` (rowbytes stride) and draw it into the destination
 * rectangle of a canvas that is canvas_w x canvas_h device pixels.
 * The caller owns aspect-ratio policy and letterboxing. */
void WebCanvas_Present (const byte *pixels, int rowbytes,
			int canvas_w, int canvas_h,
			int dst_x, int dst_y, int dst_w, int dst_h);

#endif	/* __HX2_WEB_CANVAS_H */
