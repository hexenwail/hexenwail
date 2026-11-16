/* gl_texbind.h -- texture binding, shaped like a descriptor set
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* GL hands textures to a draw one at a time through a mutable "active unit"
 * cursor: glActiveTexture, then glBindTexture, then usually glActiveTexture
 * back to zero.  SDL_GPU and Vulkan hand them over as an array bound to a
 * range of slots in one call.  This module is the second shape, implemented
 * on the first, so the call sites stop naming the cursor.  uhexen2-p4ln.3.
 *
 * It is the only renderer code that calls glActiveTexture, glBindTexture,
 * glBindImageTexture or glDeleteTextures (the loader table gl_func.h and
 * WEBSOFT's raw-GLES canvas web_canvas_gl2.c aside);
 * tools/test_texture_bind_routing.py fails on any other caller, and
 * tools/test_texbind_shadow.py checks the shadow against a fake GL.  #338.
 *
 * The slot convention was already fixed before this existed -- unit 0 diffuse,
 * unit 1 lightmap atlas or soft-depth snapshot, unit 2 fullbright mask, unit 3
 * normal map, unit 4 gloss map, unit 5 light grid, units 6-8 GPU lightstyle
 * layer atlases, units 12-15 MBOIT moment targets (see u_texture0..5,
 * LMSTYLE_TMU and MBOIT_TMU_* in gl_shader.h) -- so nothing about the layout
 * changed here, only who spells it.
 *
 * INVARIANT: every entry point leaves texture unit 0 active.  193 glTexParameter
 * calls in this engine operate on "whatever is bound to the active unit", so a
 * module that left the cursor somewhere else would silently retarget them.  The
 * cursor is still shadowed, so a run of slot-0 binds -- the overwhelming
 * majority -- issues no glActiveTexture at all.
 */

#ifndef GL_TEXBIND_H
#define GL_TEXBIND_H

/* Units 0-15 are in use; the shadow covers all sixteen so an added slot does
 * not silently fall off the end of it. */
#define RT_MAX_TEXTURE_UNITS	16

void	R_TexBind_Init (void);
void	R_TexBind_Shutdown (void);

/* Bind `count` GL_TEXTURE_2D textures to consecutive slots from `first`.
 * A zero entry unbinds that slot. */
void	R_BindTextures (GLuint first, GLsizei count, const GLuint *textures);

/* Single slot, GL_TEXTURE_2D. */
void	R_BindTextureSlot (GLuint slot, GLuint texture);

/* Single slot on a non-2D target -- the softemu palette LUT and light grid
 * (GL_TEXTURE_3D), the MSAA OIT attachments (GL_TEXTURE_2D_MULTISAMPLE),
 * the skybox (GL_TEXTURE_CUBE_MAP), and the shadow maps
 * (GL_TEXTURE_CUBE_MAP_ARRAY -- not shadowed, so always issued). */
void	R_BindTextureTarget (GLuint slot, GLenum target, GLuint texture);

/* Bind to edit: glTexImage*, glTexSubImage*, glTexStorage*, glTexParameter*,
 * glCopyTexSubImage*, glGenerateMipmap and the glGetTex* reads all act on
 * the texture bound to the ACTIVE unit, and the invariant below pins that at
 * 0 -- so an edit bind is a tracked bind on unit 0, with no slot to name.
 * Going through here is what keeps the shadow true: a raw edit bind leaves
 * unit 0 holding a texture the shadow knows nothing about, and the next
 * R_BindTextureSlot (0, ...) of what the shadow believes is there folds
 * away.  The texture stays bound afterwards; pass 0 to unbind.  SDL_GPU has
 * no such bind -- uploads go through a copy pass -- which is why these
 * sites are spelled apart from the sampling ones. */
void	R_BindTextureForEdit (GLenum target, GLuint texture);

/* A compute pass's storage image (glBindImageTexture).  Image units are not
 * texture units -- binding one cannot change what a sampler reads -- so
 * there is nothing to shadow; it is here so a pass's storage textures are
 * named in the same module as its sampled ones.  No-op on the GLES tier,
 * which has no compute (gl_func.h). */
void	R_BindImageTexture (GLuint unit, GLuint texture, GLint level, GLboolean layered,
			    GLint layer, GLenum access, GLenum format);

/* Delete textures and forget the shadow in one step.  Deleting behind the
 * shadow's back is the one way to corrupt it: GL recycles names, so the very
 * next glGenTextures can hand back a number the shadow still believes is
 * bound, and the bind that would have made a freshly created texture current
 * gets folded away -- after which its glTexImage2D lands on nothing. */
void	R_DeleteTextures (GLsizei n, const GLuint *textures);

/* Forget the shadow, so the next bind of every slot is issued.  For context
 * boundaries and for code that would rather not trust the shadow across one
 * of its own boundaries.  Nothing binds behind this module's back any more
 * -- tools/test_texture_bind_routing.py fails the build if anything does --
 * so this is never a repair, only a re-issue. */
void	R_ResetTextureBindings (void);

#endif	/* GL_TEXBIND_H */
