/* gl_shadow.c -- dynamic shadow mapping
 *
 * Renders depth-only passes from the nearest N dynamic lights.
 * Only alias models (enemies, pickups) are shadow casters —
 * world BSP is excluded to avoid self-shadowing artifacts.
 * The shadow camera looks from each light toward the viewer.
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

#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16	0x81A5
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE	0x884C
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

cvar_t	r_shadow = {"r_shadow", "0", CVAR_ARCHIVE};
cvar_t	r_shadow_resolution = {"r_shadow_resolution", "256", CVAR_ARCHIVE};
cvar_t	r_shadow_filter = {"r_shadow_filter", "0", CVAR_ARCHIVE};
cvar_t	r_shadow_maxlights = {"r_shadow_maxlights", "3", CVAR_ARCHIVE};

shadow_light_t	shadow_lights[MAX_SHADOW_LIGHTS];
int		shadow_count;

/* World VBO (defined in gl_rsurf.c) */
extern GLuint	world_vao;
extern int	world_num_indices;

static GLuint	shadow_fbo;
static GLuint	shadow_world_prog;
static GLint	shadow_world_u_mvp;
static GLuint	shadow_alias_prog;
static GLint	shadow_alias_u_mvp;
static GLint	shadow_alias_u_pose0;
static GLint	shadow_alias_u_pose1;
static GLint	shadow_alias_u_lerp;
static GLint	shadow_alias_u_scale;
static GLint	shadow_alias_u_scale_origin;
static GLint	shadow_alias_u_poseverts;
static qboolean	shadow_initialized;

/* Simple world depth shader */
static const char shadow_world_vert[] =
	"#version 430 core\n"
	"in vec3 a_position;\n"
	"uniform mat4 u_mvp;\n"
	"void main() {\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

/* ------------------------------------------------------------------ */
/* SSBO-based alias depth shader                                       */
/* ------------------------------------------------------------------ */

static const char shadow_alias_vert[] =
	"#version 430 core\n"
	"\n"
	"struct PoseVert {\n"
	"    uint data;\n"
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

static const char shadow_depth_frag[] =
	"#version 430 core\n"
	"void main() { }\n";

/* ------------------------------------------------------------------ */
/* Build light-space matrix: orthographic, straight down               */
/* ------------------------------------------------------------------ */

static void Shadow_BuildLightMatrix (shadow_light_t *sl)
{
	float proj[16], view[16], tmp[16];
	float near_clip = 2.0f;
	float far_clip = 800.0f;
	float nf;
	float eye[3], fwd[3], right[3], up[3], len;

	extern vec3_t r_origin, vpn;

	/* Stabilize the eye position: snap to integers to prevent jitter */
	eye[0] = floorf(sl->origin[0] + 0.5f);
	eye[1] = floorf(sl->origin[1] + 0.5f);
	eye[2] = floorf(sl->origin[2] + 0.5f);

	/* 90° perspective from the light */
	nf = 1.0f / (near_clip - far_clip);
	memset(proj, 0, sizeof(proj));
	proj[0] = 1.0f;
	proj[5] = 1.0f;
	proj[10] = (far_clip + near_clip) * nf;
	proj[11] = -1.0f;
	proj[14] = 2.0f * far_clip * near_clip * nf;

	/* Look from light toward the viewer */
	fwd[0] = r_origin[0] - eye[0];
	fwd[1] = r_origin[1] - eye[1];
	fwd[2] = r_origin[2] - eye[2];
	len = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
	if (len < 32.0f) { VectorCopy(vpn, fwd); len = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]); }
	if (len > 0.001f) { fwd[0] /= len; fwd[1] /= len; fwd[2] /= len; }
	else { fwd[0] = 1; fwd[1] = 0; fwd[2] = 0; }

	/* Build orthonormal basis */
	{
		float world_up[3] = {0, 0, 1};
		if (fabsf(fwd[2]) > 0.99f) { world_up[0] = 0; world_up[1] = 1; world_up[2] = 0; }
		right[0] = fwd[1]*world_up[2] - fwd[2]*world_up[1];
		right[1] = fwd[2]*world_up[0] - fwd[0]*world_up[2];
		right[2] = fwd[0]*world_up[1] - fwd[1]*world_up[0];
		len = sqrtf(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
		right[0] /= len; right[1] /= len; right[2] /= len;
		up[0] = right[1]*fwd[2] - right[2]*fwd[1];
		up[1] = right[2]*fwd[0] - right[0]*fwd[2];
		up[2] = right[0]*fwd[1] - right[1]*fwd[0];
	}

	/* lookAt view matrix */
	memset(view, 0, sizeof(view));
	view[0]  = right[0];  view[4]  = right[1];  view[8]  = right[2];
	view[1]  = up[0];     view[5]  = up[1];     view[9]  = up[2];
	view[2]  = -fwd[0];   view[6]  = -fwd[1];   view[10] = -fwd[2];
	view[15] = 1.0f;
	view[12] = -(right[0]*eye[0] + right[1]*eye[1] + right[2]*eye[2]);
	view[13] = -(up[0]*eye[0]    + up[1]*eye[1]    + up[2]*eye[2]);
	view[14] =  (fwd[0]*eye[0]   + fwd[1]*eye[1]   + fwd[2]*eye[2]);

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

	for (i = 0; i < maxlights; i++) { best_dist[i] = 999999999.0f; best_idx[i] = -1; }

	for (i = 0; i < MAX_DLIGHTS; i++)
	{
		float dx, dy, dz, dist_sq;
		if (cl_dlights[i].die < cl.time || cl_dlights[i].radius <= 0) continue;
		if (cl_dlights[i].dark) continue;

		dx = cl_dlights[i].origin[0] - r_origin[0];
		dy = cl_dlights[i].origin[1] - r_origin[1];
		dz = cl_dlights[i].origin[2] - r_origin[2];
		dist_sq = dx*dx + dy*dy + dz*dz;
		if (dist_sq > cl_dlights[i].radius * cl_dlights[i].radius * 4) continue;

		for (j = 0; j < maxlights; j++)
		{
			if (dist_sq < best_dist[j])
			{
				int k;
				for (k = maxlights - 1; k > j; k--) { best_dist[k] = best_dist[k-1]; best_idx[k] = best_idx[k-1]; }
				best_dist[j] = dist_sq; best_idx[j] = i;
				break;
			}
		}
	}

	for (i = 0; i < maxlights; i++)
	{
		if (best_idx[i] < 0) break;
		shadow_lights[i].active = true;
		VectorCopy(cl_dlights[best_idx[i]].origin, shadow_lights[i].origin);
		shadow_lights[i].radius = cl_dlights[best_idx[i]].radius;
		VectorCopy(cl_dlights[best_idx[i]].color, shadow_lights[i].color);
		shadow_count++;
	}
	for (i = shadow_count; i < MAX_SHADOW_LIGHTS; i++)
		shadow_lights[i].active = false;
}

/* ------------------------------------------------------------------ */
/* Texture management                                                  */
/* ------------------------------------------------------------------ */

static void Shadow_EnsureTexture (shadow_light_t *sl, int res)
{
	if (sl->depth_tex && sl->resolution == res) return;
	if (sl->depth_tex) glDeleteTextures_fp(1, &sl->depth_tex);

	glGenTextures_fp(1, &sl->depth_tex);
	glBindTexture_fp(GL_TEXTURE_2D, sl->depth_tex);
	glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16,
			res, res, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture_fp(GL_TEXTURE_2D, 0);
	sl->resolution = res;
}

/* ------------------------------------------------------------------ */
/* Render shadow map: alias models ONLY (no world self-shadowing)      */
/* ------------------------------------------------------------------ */

static void Shadow_RenderOne (shadow_light_t *sl, int res)
{
	Shadow_EnsureTexture(sl, res);
	Shadow_BuildLightMatrix(sl);

	glBindFramebuffer_fp(GL_FRAMEBUFFER, shadow_fbo);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				  GL_TEXTURE_2D, sl->depth_tex, 0);
	{ GLenum none = GL_NONE; glDrawBuffers_fp(1, &none); }

	glViewport_fp(0, 0, res, res);
	glClear_fp(GL_DEPTH_BUFFER_BIT);
	glEnable_fp(GL_DEPTH_TEST);
	glDepthFunc_fp(GL_LESS);
	glDepthMask_fp(1);
	glDisable_fp(GL_CULL_FACE);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	/* Only alias models cast shadows — no world BSP.
	 * World self-shadowing is impossible to fix with polygon offset alone.
	 * The shader skips cleared depth (1.0) so only entity-occluded pixels get shadows. */

	/* Render alias models: decompress vertices on CPU, draw with world shader.
	 * Mirrors the GL command parsing from GL_DrawAliasFrame. */
	if (shadow_world_prog)
	{
		int j;
		extern int cl_numvisedicts;
		extern entity_t *cl_visedicts[];
		static GLuint s_vao, s_vbo;
		#define SHADOW_MAX_VERTS (MAXALIASTRIS * 3)
		static float s_verts[SHADOW_MAX_VERTS * 3];

		if (!s_vao)
		{
			glGenVertexArrays_fp(1, &s_vao);
			glGenBuffers_fp(1, &s_vbo);
			glBindVertexArray_fp(s_vao);
			glBindBuffer_fp(GL_ARRAY_BUFFER, s_vbo);
			glBufferData_fp(GL_ARRAY_BUFFER, sizeof(s_verts), NULL, GL_STREAM_DRAW);
			glEnableVertexAttribArray_fp(0);
			glVertexAttribPointer_fp(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);
			glBindVertexArray_fp(0);
		}

		glUseProgram_fp(shadow_world_prog);
		glUniformMatrix4fv_fp(shadow_world_u_mvp, 1, GL_FALSE, sl->matrix);

		for (j = 0; j < cl_numvisedicts; j++)
		{
			entity_t *e = cl_visedicts[j];
			float dx, dy, dz, dist_sq;
			aliashdr_t *hdr;
			trivertx_t *poseverts;
			int *order;
			int frame, pose, count, num_out;
			float a, ca, sa;
			float tmp_pos[128][3];
			int vi;

			if (!e->model || e->model->type != mod_alias) continue;

			dx = e->origin[0] - sl->origin[0];
			dy = e->origin[1] - sl->origin[1];
			dz = e->origin[2] - sl->origin[2];
			dist_sq = dx*dx + dy*dy + dz*dz;
			if (dist_sq > sl->radius * sl->radius * 16) continue;

			hdr = (aliashdr_t *)Mod_Extradata(e->model);

			frame = e->frame;
			if (frame >= hdr->numframes || frame < 0) frame = 0;
			pose = hdr->frames[frame].firstpose;
			if (hdr->frames[frame].numposes > 1)
			{
				float interval = hdr->frames[frame].interval;
				pose += (int)(cl.time / interval) % hdr->frames[frame].numposes;
			}

			poseverts = (trivertx_t *)((byte *)hdr + hdr->posedata);
			poseverts += pose * hdr->poseverts;

			a = e->angles[1] * (float)M_PI / 180.0f;
			ca = cosf(a); sa = sinf(a);

			order = (int *)((byte *)hdr + hdr->commands);
			num_out = 0;

			while (1)
			{
				qboolean is_fan;
				trivertx_t *cv = poseverts; /* reset per strip/fan */

				count = *order++;
				if (!count) break;
				if (count < 0) { count = -count; is_fan = true; }
				else is_fan = false;
				if (count > 128) count = 128;

				/* Collect strip/fan vertices (mirrors GL_DrawAliasFrame) */
				for (vi = 0; vi < count; vi++)
				{
					float mx, my, mz;
					order += 2; /* skip s, t */
					/* vertices consumed sequentially */
					mx = cv->v[0] * hdr->scale[0] + hdr->scale_origin[0];
					my = cv->v[1] * hdr->scale[1] + hdr->scale_origin[1];
					mz = cv->v[2] * hdr->scale[2] + hdr->scale_origin[2];
					tmp_pos[vi][0] = mx * ca - my * sa + e->origin[0];
					tmp_pos[vi][1] = mx * sa + my * ca + e->origin[1];
					tmp_pos[vi][2] = mz + e->origin[2];
					cv++;
				}
				poseverts += count; /* advance for next strip/fan */

				/* Convert to triangles */
				#define SHADOW_EMIT(idx) do { \
					if (num_out < SHADOW_MAX_VERTS) { \
						s_verts[num_out*3+0] = tmp_pos[idx][0]; \
						s_verts[num_out*3+1] = tmp_pos[idx][1]; \
						s_verts[num_out*3+2] = tmp_pos[idx][2]; \
						num_out++; \
					} \
				} while(0)

				if (is_fan)
				{
					for (vi = 2; vi < count; vi++)
					{
						SHADOW_EMIT(0);
						SHADOW_EMIT(vi - 1);
						SHADOW_EMIT(vi);
					}
				}
				else
				{
					for (vi = 2; vi < count; vi++)
					{
						if (vi & 1)
						{
							SHADOW_EMIT(vi);
							SHADOW_EMIT(vi - 1);
							SHADOW_EMIT(vi - 2);
						}
						else
						{
							SHADOW_EMIT(vi - 2);
							SHADOW_EMIT(vi - 1);
							SHADOW_EMIT(vi);
						}
					}
				}
				#undef SHADOW_EMIT
			}

			if (num_out > 0)
			{
				glBindVertexArray_fp(s_vao);
				glBindBuffer_fp(GL_ARRAY_BUFFER, s_vbo);
				glBufferSubData_fp(GL_ARRAY_BUFFER, 0,
						   num_out * 3 * sizeof(float), s_verts);
				glDrawArrays_fp(GL_TRIANGLES, 0, num_out);
				glBindVertexArray_fp(0);
			}
		}
	}
	#undef SHADOW_MAX_VERTS

	/* Restore */
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable_fp(GL_CULL_FACE);
	glCullFace_fp(GL_FRONT);
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
	glGenFramebuffers_fp(1, &shadow_fbo);

	/* Compile world depth shader */
	vs = GL_CompileShader(GL_VERTEX_SHADER, shadow_world_vert);
	if (vs)
	{
		fs = GL_CompileShader(GL_FRAGMENT_SHADER, shadow_depth_frag);
		if (fs)
		{
			shadow_world_prog = GL_LinkProgram(vs, fs);
			glDeleteShader_fp(fs);
			if (shadow_world_prog)
				shadow_world_u_mvp = glGetUniformLocation_fp(shadow_world_prog, "u_mvp");
		}
		glDeleteShader_fp(vs);
	}

	/* Compile alias depth shader */
	vs = GL_CompileShader(GL_VERTEX_SHADER, shadow_alias_vert);
	if (!vs) return;
	fs = GL_CompileShader(GL_FRAGMENT_SHADER, shadow_depth_frag);
	if (!fs) { glDeleteShader_fp(vs); return; }

	shadow_alias_prog = GL_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);

	if (!shadow_alias_prog) { Con_SafePrintf("Shadow: shader failed\n"); return; }

	shadow_alias_u_mvp = glGetUniformLocation_fp(shadow_alias_prog, "u_mvp");
	shadow_alias_u_pose0 = glGetUniformLocation_fp(shadow_alias_prog, "u_pose0");
	shadow_alias_u_pose1 = glGetUniformLocation_fp(shadow_alias_prog, "u_pose1");
	shadow_alias_u_lerp = glGetUniformLocation_fp(shadow_alias_prog, "u_lerp");
	shadow_alias_u_scale = glGetUniformLocation_fp(shadow_alias_prog, "u_scale");
	shadow_alias_u_scale_origin = glGetUniformLocation_fp(shadow_alias_prog, "u_scale_origin");
	shadow_alias_u_poseverts = glGetUniformLocation_fp(shadow_alias_prog, "u_poseverts");

	shadow_initialized = true;
	Con_SafePrintf("Shadow: initialized (world=%u alias=%u)\n",
		       shadow_world_prog, shadow_alias_prog);
}

void GL_Shadow_Shutdown (void)
{
	int i;
	for (i = 0; i < MAX_SHADOW_LIGHTS; i++)
		if (shadow_lights[i].depth_tex) glDeleteTextures_fp(1, &shadow_lights[i].depth_tex);
	memset(shadow_lights, 0, sizeof(shadow_lights));
	if (shadow_fbo) { glDeleteFramebuffers_fp(1, &shadow_fbo); shadow_fbo = 0; }
	if (shadow_world_prog) { glDeleteProgram_fp(shadow_world_prog); shadow_world_prog = 0; }
	if (shadow_alias_prog) { glDeleteProgram_fp(shadow_alias_prog); shadow_alias_prog = 0; }
	shadow_initialized = false;
}

void GL_Shadow_RenderMaps (void)
{
	int i, res;
	GLint saved_fbo, saved_viewport[4];

	if (!shadow_initialized || !r_shadow.integer) { shadow_count = 0; return; }

	glGetIntegerv_fp(GL_FRAMEBUFFER_BINDING, &saved_fbo);
	glGetIntegerv_fp(GL_VIEWPORT, saved_viewport);

	res = (int)r_shadow_resolution.value;
	if (res < 64) res = 64;
	if (res > 1024) res = 1024;

	Shadow_SelectLights();

	for (i = 0; i < shadow_count; i++)
		Shadow_RenderOne(&shadow_lights[i], res);

	glBindFramebuffer_fp(GL_FRAMEBUFFER, saved_fbo);
	glViewport_fp(saved_viewport[0], saved_viewport[1],
		      saved_viewport[2], saved_viewport[3]);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glCullFace_fp(GL_FRONT);
	glDepthFunc_fp(GL_LEQUAL);
	glUseProgram_fp(0);
	glBindVertexArray_fp(0);
	if (saved_fbo)
	{
		GLenum buf = GL_COLOR_ATTACHMENT0;
		glDrawBuffers_fp(1, &buf);
	}
}

void GL_Shadow_BindForScene (GLuint program)
{
	int i;
	char name[64];

	if (!shadow_initialized || shadow_count == 0) return;

	for (i = 0; i < shadow_count; i++)
	{
		GLint loc;

		glActiveTextureARB_fp(GL_TEXTURE4_ARB + i);
		glBindTexture_fp(GL_TEXTURE_2D, shadow_lights[i].depth_tex);

		q_snprintf(name, sizeof(name), "u_shadow_matrix[%d]", i);
		loc = glGetUniformLocation_fp(program, name);
		if (loc >= 0) glUniformMatrix4fv_fp(loc, 1, GL_FALSE, shadow_lights[i].matrix);

		q_snprintf(name, sizeof(name), "u_shadow_tex[%d]", i);
		loc = glGetUniformLocation_fp(program, name);
		if (loc >= 0) glUniform1i_fp(loc, 4 + i);

		q_snprintf(name, sizeof(name), "u_shadow_origin[%d]", i);
		loc = glGetUniformLocation_fp(program, name);
		if (loc >= 0) glUniform3f_fp(loc, shadow_lights[i].origin[0],
					     shadow_lights[i].origin[1], shadow_lights[i].origin[2]);

		q_snprintf(name, sizeof(name), "u_shadow_radius[%d]", i);
		loc = glGetUniformLocation_fp(program, name);
		if (loc >= 0) glUniform1f_fp(loc, shadow_lights[i].radius);
	}

	{
		GLint loc = glGetUniformLocation_fp(program, "u_shadow_count");
		if (loc >= 0) glUniform1i_fp(loc, shadow_count);
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
	if (!shadow_initialized || shadow_count == 0) return 1.0f;

	for (i = 0; i < shadow_count; i++)
	{
		shadow_light_t *sl = &shadow_lights[i];
		float dx, dy, dz, dist;
		if (!sl->active || !sl->depth_tex) continue;

		dx = point[0] - sl->origin[0];
		dy = point[1] - sl->origin[1];
		dz = point[2] - sl->origin[2];
		dist = sqrtf(dx*dx + dy*dy + dz*dz);
		if (dist > sl->radius) continue;

		{ float atten = 1.0f - dist / sl->radius; factor *= 1.0f - 0.5f * atten; }
	}
	return factor;
}
