/*
 * img_load.h - External image loading interface
 */

#ifndef __IMG_LOAD_H
#define __IMG_LOAD_H

#include "quakedef.h"

// Load external texture with automatic format detection
// Returns allocated RGBA buffer, caller must free()
byte *IMG_LoadExternalTexture (const char *name, int *width, int *height, qboolean *has_alpha);

// Individual format loaders (also available for direct use)
byte *IMG_LoadPCX (const char *filename, int *width, int *height);
byte *IMG_LoadTGA (const char *filename, int *width, int *height, int *has_alpha);
byte *IMG_LoadPNG (const char *filename, int *width, int *height, int *has_alpha);

#endif /* __IMG_LOAD_H */
