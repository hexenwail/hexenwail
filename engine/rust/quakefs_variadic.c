/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * quakefs_variadic.c -- the parts of engine/h2shared/quakefs.c that cannot be
 * written in stable Rust, and nothing else.
 *
 * Two reasons, both of them about the boundary rather than the logic, which
 * stays in engine/rust/src/quakefs.rs:
 *
 * 1. FS_MakePath_VA and FS_MakePath_VABUF are C-variadic (quakefs.h:239,242,
 *    thirteen call sites in host.c, host_cmd.c, menu.c, server/host_cmd.c and
 *    sv_ccmds.c).  Rust cannot *define* a C-variadic function on stable --
 *    `c_variadic` is unstable -- so the variadic signature has to be C.  It
 *    formats with q_vsnprintf into the buffer the Rust module hands it and
 *    then applies the same overflow rule the non-variadic do_MakePath does,
 *    so the only thing this file owns is the va_list plumbing.
 *
 * 2. FS_ResolveCasePath reads a directory entry's name out of `struct dirent`.
 *    That struct's layout is glibc's, not the C standard's, and it differs
 *    between the targets this archive is built for; modelling it by hand in
 *    Rust would be an ABI guess where the C is exact.  The function stays here
 *    and the Rust call site reaches it through the same shim.  It is
 *    POSIX-only in the C (#ifndef PLATFORM_WINDOWS below, matching the guard on
 *    the call site), so on Windows neither side has it.
 *
 * This file is per *build*, not per target -- it is not quakefs_target.c --
 * because neither reason above depends on which target is being compiled.
 *
 * It is deliberately NOT added to engine_rs_attach yet: engine/h2shared/quakefs.c
 * still defines both variadic entry points, so compiling this into the engine
 * now would be a duplicate definition.  The commit that removes quakefs.c from
 * the source lists is the one that adds this file to engine_rs_attach, and the
 * differential harness compiles it directly until then.
 */

#include "quakedef.h"

/* 3. A fatal diagnostic whose NAME differs per build.  In an H2W build the
 *    C's Host_Error is a macro for SV_Error (engine/hexenworld/server/host.h:62)
 *    and only SV_Error is ever defined; in every other target it is
 *    Host_Error.  The archive is one object linked into all of them, so a Rust
 *    extern of either name alone would be an undefined reference somewhere --
 *    "Host_Error" in hwsv, "SV_Error" everywhere else.  The name is therefore
 *    chosen here, where the target's own headers are in scope, and the Rust
 *    call site reaches it through this shim.  Forwarded as "%s" so the format
 *    is consumed once, here, rather than re-interpreted by the callee.
 *
 *    Found by building the harness's h2w arm: the port's first link in that
 *    target failed on `undefined reference to Host_Error'. */
FUNC_NORETURN void QuakeFS_TargetHostError (const char *fmt, ...)
{
	va_list	ap;
	char	buf[1024];

	va_start (ap, fmt);
	q_vsnprintf (buf, sizeof (buf), fmt, ap);
	va_end (ap);

#if defined(H2W)
	SV_Error ("%s", buf);
#else
	Host_Error ("%s", buf);
#endif
}

#ifndef PLATFORM_WINDOWS
/* quakefs.c includes this itself for FS_ResolveCasePath; quakedef.h does not,
 * and struct dirent is what that function reads. */
#include <dirent.h>
#endif

/* The Rust side's non-variadic helpers.  Declared rather than included: the
 * Rust module owns them, and these are the only three names this file needs. */
extern char *QuakeFS_GetBuffer (void);
extern size_t QuakeFS_BufferLen (void);
extern int QuakeFS_MakePathPrefix (int base, char *buf, size_t siz);

/* The shared body of the two exported wrappers: the base directory, then the
 * formatted path, then the same `*error` rule do_MakePath applies -- 0 when it
 * fit, 1 when it was truncated, with Con_DPrintf saying so. */
static char *make_path_va (int base, int *error, char *buf, size_t siz,
				const char *format, va_list args)
{
	int	len, ret;

	len = QuakeFS_MakePathPrefix (base, buf, siz);
	if (len < 0)
		goto _bad;

	ret = q_vsnprintf (&buf[len], siz - len, format, args);
	if (ret < (int)siz - len)
	{
		if (error) *error = 0;
	}
	else
	{
	_bad:
		if (error) *error = 1;
		Con_DPrintf ("%s: overflow (string truncated)\n", "do_MakePath");
	}

	return buf;
}

char *FS_MakePath_VA (int base, int *error, const char *format, ...)
{
	va_list	argptr;
	char	*p;

	p = QuakeFS_GetBuffer ();
	va_start (argptr, format);
	p = make_path_va (base, error, p, QuakeFS_BufferLen (), format, argptr);
	va_end (argptr);

	return p;
}

char *FS_MakePath_VABUF (int base, int *error, char *buf, size_t siz, const char *format, ...)
{
	va_list	argptr;
	char	*p;

	va_start (argptr, format);
	p = make_path_va (base, error, buf, siz, format, argptr);
	va_end (argptr);

	return p;
}

#ifndef PLATFORM_WINDOWS
/* The C original's case-insensitive fallback for loose files: walk the path
 * one component at a time, matching each against the directory's entries with
 * q_strcasecmp.  Only macOS and Windows have case-insensitive filesystems by
 * default, and Windows does not compile this, so in practice this is the
 * "user typed the wrong case on Linux" path. */
qboolean FS_ResolveCasePath (const char *basedir, const char *relpath, char *resolved)
{
	char		buf[MAX_OSPATH];
	const char	*p, *end;
	DIR		*dir;
	struct dirent	*ent;

	q_strlcpy (buf, basedir, sizeof(buf));

	p = relpath;
	while (*p)
	{
		/* extract next path component */
		end = p;
		while (*end && *end != '/')
			end++;

		dir = opendir (buf);
		if (!dir)
			return false;

		{
			char component[MAX_QPATH];
			size_t len = end - p;
			qboolean found = false;

			if (len >= sizeof(component))
				len = sizeof(component) - 1;
			memcpy (component, p, len);
			component[len] = '\0';

			while ((ent = readdir(dir)) != NULL)
			{
				if (!q_strcasecmp(ent->d_name, component))
				{
					q_strlcat (buf, "/", sizeof(buf));
					q_strlcat (buf, ent->d_name, sizeof(buf));
					found = true;
					break;
				}
			}
			closedir (dir);
			if (!found)
				return false;
		}

		p = end;
		if (*p == '/')
			p++;
	}

	q_strlcpy (resolved, buf, MAX_OSPATH);
	return true;
}
#endif	/* !PLATFORM_WINDOWS */
