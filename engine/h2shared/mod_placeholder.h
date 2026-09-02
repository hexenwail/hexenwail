/*
 * mod_placeholder.h -- stand-in mesh for a model the gamecode precached and
 * the filesystem does not have.
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

#ifndef H2_MOD_PLACEHOLDER_H
#define H2_MOD_PLACEHOLDER_H

/* Returns a complete little-endian IDPO (alias v6) model image: a 32-unit
 * cube skinned with the black/pink notexture checker.  The buffer is static
 * and is built on first call, so it stays valid for the process lifetime and
 * may be handed straight to Mod_LoadAliasModel.
 *
 * Lives apart from the model loaders because both of them need it and they
 * are never linked together: gl_model.c serves the GL client, model.c the
 * 8bpp software client. */
void *Mod_SynthPlaceholderMDL (void);

#endif	/* H2_MOD_PLACEHOLDER_H */
