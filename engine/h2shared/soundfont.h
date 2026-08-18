/* soundfont.h -- shared SoundFont discovery for the MIDI synths
 *
 * Copyright (C) 2026  uHexen2 contributors
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

#ifndef _SOUNDFONT_H_
#define _SOUNDFONT_H_

extern cvar_t snd_soundfont;

/* Both the FluidSynth driver and the libTiMidity codec want this cvar, and a
 * build may contain both translation units, so repeat calls are ignored. */
void SF_RegisterCvar (void);

qboolean SF_FileExists (const char *path);

/* True for a compressed SoundFont (SF3) -- Ogg Vorbis sample data in an
 * otherwise ordinary RIFF/sfbk container.  Sniffs the file, not its name.
 * A synth with no Vorbis decoder must not be handed one: it will not fail,
 * it will play the bitstream as PCM.  uhexen2-d4e7. */
qboolean SF_IsCompressed (const char *path);

/* Locate a General MIDI soundfont, or return NULL.  Search order: the
 * snd_soundfont cvar, soundfont.sf2/.sf3 beside the executable, the same
 * under basedir and basedir/data1, the compile-time SOUNDFONT_PATH, then
 * well-known system and Flatpak locations.
 *
 * Pass allow_compressed false unless the caller's synth decodes SF3; every
 * candidate is then sniffed and SF3s are passed over, so the search keeps
 * going and finds a playable font instead of returning an unplayable one. */
const char *SF_FindSoundFont (qboolean allow_compressed);

#endif	/* _SOUNDFONT_H_ */
