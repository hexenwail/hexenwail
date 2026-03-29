/* gl_shadow.h -- dynamic shadow mapping
 *
 * Scalable shadow maps for the nearest N dynamic lights.
 * Low-res (64-128px) for retro, high-res (512-1024px) for modern.
 *
 * Copyright (C) 2026  Contributors of the Hexenwail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef __GL_SHADOW_H
#define __GL_SHADOW_H

#define MAX_SHADOW_LIGHTS	4	/* max simultaneous shadow-casting lights */

typedef struct {
	vec3_t		origin;
	float		radius;
	float		color[3];
	float		matrix[16];	/* light-space MVP for shadow lookup */
	GLuint		depth_tex;	/* shadow map depth texture */
	int		resolution;	/* current shadow map size */
	qboolean	active;
} shadow_light_t;

/* Call once after GL context creation */
void GL_Shadow_Init (void);

/* Call when GL context is torn down */
void GL_Shadow_Shutdown (void);

/* Call each frame before R_RenderScene — selects top N lights,
 * renders shadow maps, makes them available for scene shaders */
void GL_Shadow_RenderMaps (void);

/* Bind shadow map textures + uniforms for the given shader program.
 * Call before drawing world/alias geometry in the main scene pass. */
void GL_Shadow_BindForScene (GLuint program);

/* Returns true if shadow mapping is active this frame */
qboolean GL_Shadow_Active (void);

/* Cvars */
extern cvar_t r_shadow;		/* 0=off, 1=on */
extern cvar_t r_shadow_resolution;	/* shadow map size (64-1024) */
extern cvar_t r_shadow_filter;	/* 0=nearest (retro), 1=PCF (modern) */
extern cvar_t r_shadow_maxlights;	/* max shadow-casting lights per frame */

/* Current frame's shadow lights (read by shaders) */
extern shadow_light_t	shadow_lights[MAX_SHADOW_LIGHTS];
extern int		shadow_count;	/* active shadow lights this frame */

#endif	/* __GL_SHADOW_H */
