/* rg_spirv.h -- SDL_GPU backend: the SPIR-V compiled into the client (#340)
 *
 * <build>/engine/generated/spirv_gen.c, written by
 * engine/cmake/EmbedSpirv.cmake, defines the table: one entry per row of
 * gl_progid_refl.h, sorted by name, the name being R_Program_Blob's
 * ("s2d.vert").  Included by that file and rg_shader.c only.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef USE_SDL_GPU
#error rg_spirv.h is the SDL_GPU backend; CMake compiles it only with USE_SDL_GPU
#endif

#ifndef RG_SPIRV_H
#define RG_SPIRV_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	const char	*name;		/* R_Program_Blob's name */
	const uint32_t	*code;		/* the <name>.vk.spv words, native order */
	size_t		size;		/* in bytes */
} rg_spirv_t;

extern const rg_spirv_t	rg_spirv[];
extern const int	rg_spirv_count;

#endif /* RG_SPIRV_H */
