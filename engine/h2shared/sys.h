/* sys.h: non-portable functions
 * relies on: arch_def.h
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
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

#ifndef HX2_SYS_H
#define HX2_SYS_H

/* file IO */

int Sys_mkdir (const char *path, qboolean crash);
int Sys_rmdir (const char *path);
int Sys_unlink (const char *path);
int Sys_rename (const char *oldp, const char *newp);

int Sys_FileType (const char *path);
/* returns an FS entity type, i.e. FS_ENT_FILE or FS_ENT_DIRECTORY.
 * returns FS_ENT_NONE (0) if no such file or directory is present. */

long Sys_filesize (const char *path);

int Sys_CopyFile (const char *frompath, const char *topath);

/* Simplified findfirst/findnext: returns filenames only, not a dirent
 * struct. The enumeration state is caller-owned so that two threads (the
 * background save worker and the main thread) can walk directories at the
 * same time. Declare an fsfind_t on the stack, pass it to all three calls,
 * and always Sys_FindClose it — including on early exit from the loop.
 * Returned names point into ctx->name and stay valid until the next
 * Sys_FindNextFile on the same context. */
typedef struct
{
	void	*priv;			/* platform enumeration state */
	char	name[MAX_OSPATH];	/* current filename, UTF-8 */
} fsfind_t;

const char *Sys_FindFirstFile (fsfind_t *ctx, const char *path, const char *pattern);
const char *Sys_FindNextFile (fsfind_t *ctx);
void Sys_FindClose (fsfind_t *ctx);

int Sys_ListDirectories (const char *path, char dirs[][64], int maxdirs);
/* Scans 'path' for subdirectories, storing their names (not full paths)
 * into 'dirs'. Each name slot is 64 bytes (matching MAX_QPATH).
 * Returns the number of directories found. Skips "." and "..". */

const char *Sys_GetExeDir (void);
/* Directory containing the running executable, with no trailing slash, or
 * NULL where the platform cannot answer (everything but Linux and Windows).
 * This is NOT host_parms->basedir: basedir is the working directory the game
 * was launched from, so anything shipped beside the binary must be looked up
 * through here instead. Callers must treat NULL as "try the next lookup
 * layer", never as an error.
 * The result points into a static buffer resolved on first call; it cannot
 * change during a run, so the buffer is never rewritten. Make that first call
 * from the main thread (the background save worker must not race it). */

/* memory protection */

void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length);


/* user directories */

/* disable user directories on platforms where they
 * are not necessary or not possible. */
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_OS2)
#undef	DO_USERDIRS
#define	DO_USERDIRS	0
#endif	/* DO_USERDIRS  */


/* system IO */

FUNC_NORETURN void Sys_Quit (void);
FUNC_NORETURN void Sys_Error (const char *error, ...) FUNC_PRINTF(1,2);
	/* cause the entire program to exit */
#ifdef __WATCOMC__
#pragma aux Sys_Error aborts;
#pragma aux Sys_Quit aborts;
#endif

void Sys_PrintTerm (const char *msgtxt);
	/* print the given string to the terminal */

double Sys_DoubleTime (void);

char *Sys_DateTimeString (char *buf);
	/* returns date + time string equivalent to the combination
	 * of following calls:	char buf[20]; t = time(NULL);
	 * strftime (buf, sizeof(buf), "%m/%d/%Y %H:%M:%S", localtime(&t)); */

const char *Sys_ConsoleInput (void);

void Sys_Sleep (unsigned long msecs);
	/* yield for about 'msecs' milliseconds */

void Sys_SendKeyEvents (void);
	/* perform Key_Event () callbacks until the input que is empty */

char *Sys_GetClipboardData (void);


/* x86 asm support */
#if defined(USE_INTEL_ASM) && \
   (defined(__i386) || defined(__i386__) || defined(__386__) || defined(_M_IX86) || defined(__I386__))
#	define	id386		1
#else	/* not i386 or no intel asm */
#	define	id386		0
#endif

/* m68k asm support */
#if defined(USE_M68K_ASM) && \
   (defined(__mc68000__) || defined(__M68K__) || defined(__m68k__) || defined(__MC68K__))
#	define	id68k		1
#else /* !m68k or no m68k asm */
#	define	id68k		0
#endif

/* C-linkage for C-ASM shared global vars and functions */
#if id386
#   if defined(__cplusplus)
#	define	ASM_LINKAGE_BEGIN	extern "C" {
#	define	ASM_LINKAGE_END			}
#   else
#	define	ASM_LINKAGE_BEGIN
#	define	ASM_LINKAGE_END
#   endif

#elif id68k
#   if defined(__cplusplus)
#	define	ASM_LINKAGE_BEGIN	extern "C" {
#	define	ASM_LINKAGE_END			}
#   else
#	define	ASM_LINKAGE_BEGIN
#	define	ASM_LINKAGE_END
#   endif

#else
#	define	ASM_LINKAGE_BEGIN
#	define	ASM_LINKAGE_END
#endif

#if id386 /* fpu stuff with x86 asm */
ASM_LINKAGE_BEGIN
void	MaskExceptions (void);
void	Sys_SetFPCW (void);
void	Sys_LowFPPrecision (void);
void	Sys_HighFPPrecision (void);
void	Sys_PopFPCW (void);
void	Sys_PushFPCW_SetHigh (void);
ASM_LINKAGE_END
#else
#define MaskExceptions()	do {} while (0)
#define Sys_SetFPCW()		do {} while (0)
#define Sys_LowFPPrecision()	do {} while (0)
#define Sys_HighFPPrecision()	do {} while (0)
#define Sys_PopFPCW()		do {} while (0)
#define Sys_PushFPCW_SetHigh()	do {} while (0)
#endif

/* set UNALIGNED_OK 0 if unaligned accesses are not supported */
#if defined(__i386) || defined(__i386__) || defined(__386__) || defined(_M_IX86) || defined(__I386__)
#	define	UNALIGNED_OK	1
#else
#	define	UNALIGNED_OK	0
#endif

#endif	/* HX2_SYS_H */
