/*
 * zone_target.h -- the zone sizes that differ per target.
 *
 * These definitions lived in zone.c until the Rust port of it needed them.
 * The consolidated Rust archive is one object linked into every engine target
 * (see #248), so a single Rust implementation cannot carry ZONE_DEFSIZE's two
 * values at once: it is 1 MB in the SERVERONLY binaries (h2ded, hwsv) and 2 MB
 * in the client.  The mechanism chosen for that, recorded on #233, is a
 * per-target C shim -- engine/rust/zone_target.c -- which is compiled into each
 * target with that target's own defines and hands the values to Rust.
 *
 * The definitions live here rather than in the shim so that zone.c and the
 * shim cannot drift apart: both include this header, and the differential
 * harness builds the C original and the shim with the same defines, so a
 * divergence is a failing gate rather than a silent difference.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 2008-2010  O.Sezer <sezero@users.sourceforge.net>
 * Copyright (C) 2026 Hexenwail contributors.
 */

#ifndef ZONE_TARGET_H_
#define ZONE_TARGET_H_

#define	ZONE_MINSIZE	0x100000	/* 1 mb */
#define	ZONE_MAXSIZE	0x800000	/* 8 mb */
#if defined(SERVERONLY)
#define	ZONE_DEFSIZE	0x100000	/* 1 mb */
#else
#define	ZONE_DEFSIZE	0x200000	/* 2 mb */
#endif	/* SERVERONLY */

/* setup size for secondary zone: */
#define	MEM_STATIC_TEX	0x40000

#define	MEM_CODEC_MEM	0
#if defined(CODECS_USE_ZONE)
/* this setup assumes only one codec
 * would be active at a time. */
#if defined(USE_CODEC_MP3)
#if (MEM_CODEC_MEM < LIBMAD_NEEDMEM)
#undef	MEM_CODEC_MEM
#define	MEM_CODEC_MEM	LIBMAD_NEEDMEM
#endif
#endif	/* LIBMAD */
#if defined(USE_CODEC_VORBIS)
#if (MEM_CODEC_MEM < VORBIS_NEEDMEM)
#undef	MEM_CODEC_MEM
#define	MEM_CODEC_MEM	VORBIS_NEEDMEM
#endif
#endif	/* VORBIS */
#endif	/* CODECS_USE_ZONE */

#if defined(SERVERONLY)
#undef	MEM_STATIC_TEX
#define	MEM_STATIC_TEX	0
#undef	MEM_CODEC_MEM
#define	MEM_CODEC_MEM	0
#endif

#define	SECZONE_SIZE			\
	(MEM_STATIC_TEX + MEM_CODEC_MEM)

#endif	/* ZONE_TARGET_H_ */
