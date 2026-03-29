/* gl_shadow.c -- dynamic shadow mapping
 *
 * Renders depth-only passes from the nearest N dynamic lights,
 * producing shadow map textures sampled by the world/alias shaders.
 *
 * Copyright (C) 2026  Contributors of the Hexenwail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "gl_shadow.h"
#include "gl_shader.h"
#include "gl_matrix.h"
#include "gl_vbo.h"

/* World VBO (defined in gl_rsurf.c) */
extern GLuint	world_vao;
extern int	world_num_indices;

#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16	0x81A5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24	0x81A6
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE	0x884C
#endif
#ifndef GL_COMPARE_REF_TO_TEXTURE
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC	0x884D
#endif
#ifndef GL_NONE
#define GL_NONE			0
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING	0x8CA6
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT	0x8D00
#endif

cvar_t	r_shadow = {"r_shadow", "1", CVAR_ARCHIVE};
cvar_t	r_shadow_resolution = {"r_shadow_resolution", "256", CVAR_ARCHIVE};
cvar_t	r_shadow_filter = {"r_shadow_filter", "0", CVAR_ARCHIVE};
cvar_t	r_shadow_maxlights = {"r_shadow_maxlights", "3", CVAR_ARCHIVE};

shadow_light_t	shadow_lights[MAX_SHADOW_LIGHTS];
int		shadow_count;

static GLuint	shadow_fbo;		/* shared FBO, swap depth attachment per light */
static GLuint	shadow_depth_prog;	/* depth-only shader for world shadow pass */
static GLint	shadow_depth_u_mvp;
static GLuint	shadow_alias_prog;	/* depth-only shader for alias model shadow pass */
static GLint	shadow_alias_u_mvp;
static GLint	shadow_alias_u_pose0;
static GLint	shadow_alias_u_pose1;
static GLint	shadow_alias_u_lerp;
static GLint	shadow_alias_u_scale;
static GLint	shadow_alias_u_scale_origin;
static GLint	shadow_alias_u_poseverts;
static qboolean	shadow_initialized;

/* ------------------------------------------------------------------ */
/* Depth-only shader for shadow pass                                   */
/* ------------------------------------------------------------------ */

static const char shadow_depth_vert[] =
	"#version 430 core\n"
	"in vec3 a_position;\n"
	"uniform mat4 u_mvp;\n"
	"void main() {\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char shadow_depth_frag[] =
	"#version 430 core\n"
	"void main() {\n"
	"    /* depth-only pass — no color output */\n"
	"}\n";

/* Depth-only vertex shader for alias models (SSBO pose data) */
static const char shadow_alias_vert[] =
	"#version 430 core\n"
	"\n"
	"struct PoseVert {\n"
	"    uint data;  /* byte v[3] + byte lightnormalindex */\n"
	"};\n"
	"\n"
	"layout(std430, binding = 1) readonly buffer PoseBuffer {\n"
	"    PoseVert poses[];\n"
	"};\n"
	"\n"
	"uniform mat4 u_mvp;\n"
	"uniform int u_pose0;\n"
	"uniform int u_pose1;\n"
	"uniform float u_lerp;\n"
	"uniform vec3 u_scale;\n"
	"uniform vec3 u_scale_origin;\n"
	"uniform int u_poseverts;\n"
	"\n"
	"void main() {\n"
	"    int vid = gl_VertexID;\n"
	"    uint p0 = poses[u_pose0 * u_poseverts + vid].data;\n"
	"    uint p1 = poses[u_pose1 * u_poseverts + vid].data;\n"
	"    vec3 v0 = vec3(float(p0 & 0xFFu), float((p0 >> 8) & 0xFFu), float((p0 >> 16) & 0xFFu));\n"
	"    vec3 v1 = vec3(float(p1 & 0xFFu), float((p1 >> 8) & 0xFFu), float((p1 >> 16) & 0xFFu));\n"
	"    vec3 pos = mix(v1, v0, u_lerp) * u_scale + u_scale_origin;\n"
	"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
	"}\n";

/* ------------------------------------------------------------------ */
/* Light-space matrix: spot projection looking down from torch         */
/* ------------------------------------------------------------------ */

static void Shadow_BuildLightMatrix (shadow_light_t *sl)
{
	float proj[16], view[16], tmp[16];
	float fov = 90.0f;	/* 90 degree cone covers area below torch */
	float near_clip = 1.0f;
	float far_clip = sl->radius;
	float aspect = 1.0f;
	float f, nf;

	/* Perspective projection */
	f = 1.0f / tanf(fov * 0.5f * M_PI / 180.0f);
	nf = 1.0f / (near_clip - far_clip);
	memset(proj, 0, sizeof(proj));
	proj[0] = f / aspect;
	proj[5] = f;
	proj[10] = (far_clip + near_clip) * nf;
	proj[11] = -1.0f;
	proj[14] = 2.0f * far_clip * near_clip * nf;

	/* View matrix: look straight down (most torches light below) */
	memset(view, 0, sizeof(view));
	view[0] = 1.0f;		/* right = +X */
	view[5] = 0.0f;
	view[6] = -1.0f;		/* up = -Z (looking down) */
	view[9] = -1.0f;		/* forward = -Y (down) */
	view[10] = 0.0f;
	view[15] = 1.0f;

	/* Translate to light origin */
	view[12] = -sl->origin[0];
	view[13] = -sl->origin[2];	/* swap Y/Z for downward look */
	view[14] = sl->origin[1];

	/* Multiply: lightMVP = proj * view */
	Mat4_Multiply(proj, view, tmp);
	memcpy(sl->matrix, tmp, sizeof(float) * 16);
}

/* ------------------------------------------------------------------ */
/* Select top N nearest lights                                         */
/* ------------------------------------------------------------------ */

static void Shadow_SelectLights (void)
{
	int i, j;
	float best_dist[MAX_SHADOW_LIGHTS];
	int best_idx[MAX_SHADOW_LIGHTS];
	int maxlights;
	extern vec3_t r_origin;

	shadow_count = 0;
	maxlights = (int)r_shadow_maxlights.value;
	if (maxlights < 1) maxlights = 1;
	if (maxlights > MAX_SHADOW_LIGHTS) maxlights = MAX_SHADOW_LIGHTS;

	for (i = 0; i < maxlights; i++)
	{
		best_dist[i] = 999999999.0f;
		best_idx[i] = -1;
	}

	/* Find nearest N active dlights */
	for (i = 0; i < MAX_DLIGHTS; i++)
	{
		float dx, dy, dz, dist_sq;

		if (cl_dlights[i].die < cl.time || cl_dlights[i].radius <= 0)
			continue;
		if (cl_dlights[i].dark)
			continue;	/* don't shadow-map dark lights */

		dx = cl_dlights[i].origin[0] - r_origin[0];
		dy = cl_dlights[i].origin[1] - r_origin[1];
		dz = cl_dlights[i].origin[2] - r_origin[2];
		dist_sq = dx*dx + dy*dy + dz*dz;

		/* Skip lights too far away */
		if (dist_sq > cl_dlights[i].radius * cl_dlights[i].radius * 4)
			continue;

		/* Insert into sorted best list */
		for (j = 0; j < maxlights; j++)
		{
			if (dist_sq < best_dist[j])
			{
				/* Shift down */
				int k;
				for (k = maxlights - 1; k > j; k--)
				{
					best_dist[k] = best_dist[k-1];
					best_idx[k] = best_idx[k-1];
				}
				best_dist[j] = dist_sq;
				best_idx[j] = i;
				break;
			}
		}
	}

	for (i = 0; i < maxlights; i++)
	{
		if (best_idx[i] < 0)
			break;

		shadow_lights[i].active = true;
		VectorCopy(cl_dlights[best_idx[i]].origin, shadow_lights[i].origin);
		shadow_lights[i].radius = cl_dlights[best_idx[i]].radius;
		VectorCopy(cl_dlights[best_idx[i]].color, shadow_lights[i].color);
		shadow_count++;
	}

	/* Clear unused slots */
	for (i = shadow_count; i < MAX_SHADOW_LIGHTS; i++)
		shadow_lights[i].active = false;
}

/* ------------------------------------------------------------------ */
/* Allocate / resize shadow map textures                               */
/* ------------------------------------------------------------------ */

static void Shadow_EnsureTexture (shadow_light_t *sl, int res)
{
	if (sl->depth_tex && sl->resolution == res)
		return;	/* already correct size */

	if (sl->depth_tex)
		glDeleteTextures_fp(1, &sl->depth_tex);

	glGenTextures_fp(1, &sl->depth_tex);
	glBindTexture_fp(GL_TEXTURE_2D, sl->depth_tex);
	glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16,
			res, res, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);

	if (r_shadow_filter.integer)
	{
		/* PCF: linear filter, manual comparison in shader */
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	else
	{
		/* Retro: nearest, manual comparison in shader */
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture_fp(GL_TEXTURE_2D, 0);

	sl->resolution = res;
}

/* ------------------------------------------------------------------ */
/* Render shadow map for one light                                     */
/* ------------------------------------------------------------------ */

static void Shadow_RenderOne (shadow_light_t *sl, int res)
{
	Shadow_EnsureTexture(sl, res);
	Shadow_BuildLightMatrix(sl);

	/* Attach this light's depth texture to the shared FBO */
	glBindFramebuffer_fp(GL_FRAMEBUFFER, shadow_fbo);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				  GL_TEXTURE_2D, sl->depth_tex, 0);
	{
		GLenum none = GL_NONE;
		glDrawBuffers_fp(1, &none);
	}

	glViewport_fp(0, 0, res, res);
	glClear_fp(GL_DEPTH_BUFFER_BIT);

	glEnable_fp(GL_DEPTH_TEST);
	glDepthFunc_fp(GL_LESS);
	glDepthMask_fp(1);

	/* Disable face culling for shadow pass — front-face cull was
	 * corrupting scene state and hiding thin/single-sided geometry */
	glDisable_fp(GL_CULL_FACE);

	glUseProgram_fp(shadow_depth_prog);
	glUniformMatrix4fv_fp(shadow_depth_u_mvp, 1, GL_FALSE, sl->matrix);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	/* Render world BSP from static VBO */
	if (world_vao && world_num_indices > 0)
	{
		glBindVertexArray_fp(world_vao);
		glDrawElements_fp(GL_TRIANGLES, world_num_indices, GL_UNSIGNED_INT, NULL);
		glBindVertexArray_fp(0);
	}

	/* Render alias models (enemies, pickups) within light radius.
	 * Uses SSBO-based alias shadow shader for correct pose unpacking. */
	if (shadow_alias_prog)
	{
		int j;
		extern int cl_numvisedicts;
		extern entity_t *cl_visedicts[];

		glUseProgram_fp(shadow_alias_prog);

		for (j = 0; j < cl_numvisedicts; j++)
		{
			entity_t *e = cl_visedicts[j];
			float dx, dy, dz, dist_sq;
			alias_gpu_mesh_t *gm;
			aliashdr_t *hdr;
			int frame, pose;

			if (!e->model || e->model->type != mod_alias)
				continue;

			dx = e->origin[0] - sl->origin[0];
			dy = e->origin[1] - sl->origin[1];
			dz = e->origin[2] - sl->origin[2];
			dist_sq = dx*dx + dy*dy + dz*dz;
			if (dist_sq > sl->radius * sl->radius)
				continue;

			hdr = (aliashdr_t *)Mod_Extradata(e->model);
			gm = GL_GetAliasGPUMesh(hdr);
			if (!gm || !gm->valid)
				continue;

			/* Get current pose */
			frame = e->frame;
			if (frame >= hdr->numframes || frame < 0)
				frame = 0;
			pose = hdr->frames[frame].firstpose;
			if (hdr->frames[frame].numposes > 1)
			{
				float interval = hdr->frames[frame].interval;
				pose += (int)(cl.time / interval) % hdr->frames[frame].numposes;
			}

			/* Build entity transform: translate to origin, apply
			 * rotation, then multiply by light projection.
			 * scale/scale_origin are handled by the shader. */
			{
				float ent_world[16], mvp[16];
				float a = e->angles[1] * (float)M_PI / 180.0f;
				float ca = cosf(a), sa = sinf(a);

				/* Yaw rotation + translation to entity origin */
				memset(ent_world, 0, sizeof(ent_world));
				ent_world[0] = ca;
				ent_world[1] = sa;
				ent_world[4] = -sa;
				ent_world[5] = ca;
				ent_world[10] = 1.0f;
				ent_world[12] = e->origin[0];
				ent_world[13] = e->origin[1];
				ent_world[14] = e->origin[2];
				ent_world[15] = 1.0f;
				Mat4_Multiply(sl->matrix, ent_world, mvp);
				glUniformMatrix4fv_fp(shadow_alias_u_mvp, 1, GL_FALSE, mvp);
			}

			glUniform1i_fp(shadow_alias_u_pose0, pose);
			glUniform1i_fp(shadow_alias_u_pose1, pose);
			glUniform1f_fp(shadow_alias_u_lerp, 0.0f);
			glUniform3f_fp(shadow_alias_u_scale,
				       hdr->scale[0], hdr->scale[1], hdr->scale[2]);
			glUniform3f_fp(shadow_alias_u_scale_origin,
				       hdr->scale_origin[0], hdr->scale_origin[1], hdr->scale_origin[2]);
			glUniform1i_fp(shadow_alias_u_poseverts, hdr->poseverts);

			glBindVertexArray_fp(gm->vao);
			glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 1, gm->ssbo_pose);
			glDrawElements_fp(GL_TRIANGLES, gm->num_indices, GL_UNSIGNED_SHORT, NULL);
		}
		glBindVertexArray_fp(0);

		/* Switch back to world depth shader for next light */
		glUseProgram_fp(shadow_depth_prog);
	}

	/* Restore state */
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable_fp(GL_CULL_FACE);
	glCullFace_fp(GL_FRONT);	/* scene default for Quake */
	glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void GL_Shadow_Init (void)
{
	GLuint vs, fs;

	Cvar_RegisterVariable(&r_shadow);
	Cvar_RegisterVariable(&r_shadow_resolution);
	Cvar_RegisterVariable(&r_shadow_filter);
	Cvar_RegisterVariable(&r_shadow_maxlights);

	memset(shadow_lights, 0, sizeof(shadow_lights));
	shadow_count = 0;

	/* Create shared FBO (depth attachment swapped per light) */
	glGenFramebuffers_fp(1, &shadow_fbo);

	/* Compile depth-only shader */
	vs = GL_CompileShader(GL_VERTEX_SHADER, shadow_depth_vert);
	if (!vs) return;
	fs = GL_CompileShader(GL_FRAGMENT_SHADER, shadow_depth_frag);
	if (!fs) { glDeleteShader_fp(vs); return; }

	shadow_depth_prog = GL_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);

	if (!shadow_depth_prog)
	{
		Con_SafePrintf("Shadow: depth shader failed\n");
		return;
	}

	shadow_depth_u_mvp = glGetUniformLocation_fp(shadow_depth_prog, "u_mvp");

	/* Compile alias depth shader (SSBO-based pose unpacking) */
	vs = GL_CompileShader(GL_VERTEX_SHADER, shadow_alias_vert);
	if (!vs) goto done;
	fs = GL_CompileShader(GL_FRAGMENT_SHADER, shadow_depth_frag);
	if (!fs) { glDeleteShader_fp(vs); goto done; }

	shadow_alias_prog = GL_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);

	if (shadow_alias_prog)
	{
		shadow_alias_u_mvp = glGetUniformLocation_fp(shadow_alias_prog, "u_mvp");
		shadow_alias_u_pose0 = glGetUniformLocation_fp(shadow_alias_prog, "u_pose0");
		shadow_alias_u_pose1 = glGetUniformLocation_fp(shadow_alias_prog, "u_pose1");
		shadow_alias_u_lerp = glGetUniformLocation_fp(shadow_alias_prog, "u_lerp");
		shadow_alias_u_scale = glGetUniformLocation_fp(shadow_alias_prog, "u_scale");
		shadow_alias_u_scale_origin = glGetUniformLocation_fp(shadow_alias_prog, "u_scale_origin");
		shadow_alias_u_poseverts = glGetUniformLocation_fp(shadow_alias_prog, "u_poseverts");
	}

done:
	shadow_initialized = true;
	Con_SafePrintf("Shadow: initialized (depth prog=%u, alias prog=%u)\n",
		       shadow_depth_prog, shadow_alias_prog);
}

void GL_Shadow_Shutdown (void)
{
	int i;

	for (i = 0; i < MAX_SHADOW_LIGHTS; i++)
	{
		if (shadow_lights[i].depth_tex)
			glDeleteTextures_fp(1, &shadow_lights[i].depth_tex);
	}
	memset(shadow_lights, 0, sizeof(shadow_lights));

	if (shadow_fbo) { glDeleteFramebuffers_fp(1, &shadow_fbo); shadow_fbo = 0; }
	if (shadow_depth_prog) { glDeleteProgram_fp(shadow_depth_prog); shadow_depth_prog = 0; }
	if (shadow_alias_prog) { glDeleteProgram_fp(shadow_alias_prog); shadow_alias_prog = 0; }
	shadow_initialized = false;
}

void GL_Shadow_RenderMaps (void)
{
	int i, res;
	GLint saved_fbo, saved_viewport[4];

	if (!shadow_initialized || !r_shadow.integer)
	{
		shadow_count = 0;
		return;
	}

	/* Save current state */
	glGetIntegerv_fp(GL_FRAMEBUFFER_BINDING, &saved_fbo);
	glGetIntegerv_fp(GL_VIEWPORT, saved_viewport);

	/* Clamp resolution to power of 2 */
	res = (int)r_shadow_resolution.value;
	if (res < 64) res = 64;
	if (res > 1024) res = 1024;

	Shadow_SelectLights();

	for (i = 0; i < shadow_count; i++)
		Shadow_RenderOne(&shadow_lights[i], res);

	/* Restore all GL state touched by shadow passes */
	glBindFramebuffer_fp(GL_FRAMEBUFFER, saved_fbo);
	glViewport_fp(saved_viewport[0], saved_viewport[1],
		      saved_viewport[2], saved_viewport[3]);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glCullFace_fp(GL_FRONT);	/* scene default */
	glDepthFunc_fp(GL_LEQUAL);	/* scene default */
	glUseProgram_fp(0);
	glBindVertexArray_fp(0);
	{
		GLenum buf = GL_COLOR_ATTACHMENT0;
		if (saved_fbo)
			glDrawBuffers_fp(1, &buf);
	}
}

void GL_Shadow_BindForScene (GLuint program)
{
	int i;
	char name[64];

	if (!shadow_initialized || shadow_count == 0)
		return;

	for (i = 0; i < shadow_count; i++)
	{
		GLint loc_matrix, loc_tex, loc_origin, loc_radius;

		/* Bind shadow map to texture units 4+ (0-3 reserved for diffuse/lightmap/etc) */
		glActiveTextureARB_fp(GL_TEXTURE4_ARB + i);
		glBindTexture_fp(GL_TEXTURE_2D, shadow_lights[i].depth_tex);

		q_snprintf(name, sizeof(name), "u_shadow_matrix[%d]", i);
		loc_matrix = glGetUniformLocation_fp(program, name);
		if (loc_matrix >= 0)
			glUniformMatrix4fv_fp(loc_matrix, 1, GL_FALSE, shadow_lights[i].matrix);

		q_snprintf(name, sizeof(name), "u_shadow_tex[%d]", i);
		loc_tex = glGetUniformLocation_fp(program, name);
		if (loc_tex >= 0)
			glUniform1i_fp(loc_tex, 4 + i);

		q_snprintf(name, sizeof(name), "u_shadow_origin[%d]", i);
		loc_origin = glGetUniformLocation_fp(program, name);
		if (loc_origin >= 0)
			glUniform3f_fp(loc_origin, shadow_lights[i].origin[0],
				       shadow_lights[i].origin[1],
				       shadow_lights[i].origin[2]);

		q_snprintf(name, sizeof(name), "u_shadow_radius[%d]", i);
		loc_radius = glGetUniformLocation_fp(program, name);
		if (loc_radius >= 0)
			glUniform1f_fp(loc_radius, shadow_lights[i].radius);
	}

	{
		GLint loc = glGetUniformLocation_fp(program, "u_shadow_count");
		if (loc >= 0)
			glUniform1i_fp(loc, shadow_count);
	}
	{
		GLint loc = glGetUniformLocation_fp(program, "u_shadow_filter");
		if (loc >= 0)
			glUniform1i_fp(loc, r_shadow_filter.integer);
	}
	{
		GLint loc = glGetUniformLocation_fp(program, "u_shadow_texel");
		if (loc >= 0)
		{
			int res = (int)r_shadow_resolution.value;
			if (res < 64) res = 64;
			glUniform1f_fp(loc, 1.0f / (float)res);
		}
	}

	glActiveTextureARB_fp(GL_TEXTURE0_ARB);
}

qboolean GL_Shadow_Active (void)
{
	return shadow_initialized && r_shadow.integer && shadow_count > 0;
}

float GL_Shadow_PointFactor (vec3_t point)
{
	int i;
	float factor = 1.0f;

	if (!shadow_initialized || shadow_count == 0)
		return 1.0f;

	for (i = 0; i < shadow_count; i++)
	{
		shadow_light_t *sl = &shadow_lights[i];
		float dx, dy, dz, dist;
		float lpos[4], proj[3];

		if (!sl->active || !sl->depth_tex)
			continue;

		dx = point[0] - sl->origin[0];
		dy = point[1] - sl->origin[1];
		dz = point[2] - sl->origin[2];
		dist = sqrtf(dx*dx + dy*dy + dz*dz);
		if (dist > sl->radius)
			continue;

		/* Transform point to light clip space */
		lpos[0] = sl->matrix[0]*point[0] + sl->matrix[4]*point[1] + sl->matrix[8]*point[2] + sl->matrix[12];
		lpos[1] = sl->matrix[1]*point[0] + sl->matrix[5]*point[1] + sl->matrix[9]*point[2] + sl->matrix[13];
		lpos[2] = sl->matrix[2]*point[0] + sl->matrix[6]*point[1] + sl->matrix[10]*point[2] + sl->matrix[14];
		lpos[3] = sl->matrix[3]*point[0] + sl->matrix[7]*point[1] + sl->matrix[11]*point[2] + sl->matrix[15];

		if (lpos[3] <= 0)
			continue;	/* behind light */

		/* Perspective divide → NDC → [0,1] */
		proj[0] = (lpos[0] / lpos[3]) * 0.5f + 0.5f;
		proj[1] = (lpos[1] / lpos[3]) * 0.5f + 0.5f;
		proj[2] = (lpos[2] / lpos[3]) * 0.5f + 0.5f;

		if (proj[0] < 0 || proj[0] > 1 || proj[1] < 0 || proj[1] > 1)
			continue;	/* outside shadow map */

		/* Simple shadow test: if the point is far from light and within
		 * the shadow frustum, darken it based on distance attenuation.
		 * Full per-pixel readback would be too slow for CPU side. */
		{
			float atten = 1.0f - dist / sl->radius;
			factor *= 1.0f - 0.5f * atten; /* 50% shadow at closest */
		}
	}

	return factor;
}
