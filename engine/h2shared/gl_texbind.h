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
 * The slot convention was already fixed before this existed -- unit 0 diffuse,
 * unit 1 lightmap atlas or soft-depth snapshot, unit 2 fullbright mask (see
 * u_texture0..2 in gl_shader.h) -- so nothing about the layout changed here,
 * only who spells it.
 *
 * INVARIANT: every entry point leaves texture unit 0 active.  158 glTexParameter
 * calls in this engine operate on "whatever is bound to the active unit", so a
 * module that left the cursor somewhere else would silently retarget them.  The
 * cursor is still shadowed, so a run of slot-0 binds -- the overwhelming
 * majority -- issues no glActiveTexture at all.
 */

#ifndef GL_TEXBIND_H
#define GL_TEXBIND_H

/* Only 0, 1 and 2 are in use; the fourth is headroom so an added slot does not
 * silently fall off the end of the shadow. */
#define RT_MAX_TEXTURE_UNITS	4

void	R_TexBind_Init (void);
void	R_TexBind_Shutdown (void);

/* Bind `count` GL_TEXTURE_2D textures to consecutive slots from `first`.
 * A zero entry unbinds that slot. */
void	R_BindTextures (GLuint first, GLsizei count, const GLuint *textures);

/* Single slot, GL_TEXTURE_2D. */
void	R_BindTextureSlot (GLuint slot, GLuint texture);

/* Single slot on a non-2D target -- the softemu palette LUT (GL_TEXTURE_3D)
 * and the MSAA OIT attachments (GL_TEXTURE_2D_MULTISAMPLE). */
void	R_BindTextureTarget (GLuint slot, GLenum target, GLuint texture);

/* What slot 0 currently holds.  A handful of sites compare against it to
 * decide whether a texture they are about to delete is live. */
GLuint	R_CurrentTexture (void);

/* Delete textures and forget the shadow in one step.  Deleting behind the
 * shadow's back is the one way to corrupt it: GL recycles names, so the very
 * next glGenTextures can hand back a number the shadow still believes is
 * bound, and the bind that would have made a freshly created texture current
 * gets folded away -- after which its glTexImage2D lands on nothing. */
void	R_DeleteTextures (GLsizei n, const GLuint *textures);

/* Forget the shadow.  For context boundaries, and for anything that binds a
 * texture without going through this module. */
void	R_ResetTextureBindings (void);

#endif	/* GL_TEXBIND_H */
