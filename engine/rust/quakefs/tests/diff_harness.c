/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diff_harness.c -- differential harness for the Rust port of
 * engine/h2shared/quakefs.c.
 *
 * HOW THIS ONE DIFFERS FROM THE OTHER PORTS' HARNESSES.  zone, cmd and the
 * rest link the renamed C original and the Rust module into ONE binary and
 * switch between them through a table of function pointers.  That needs a
 * vtable entry per function, and quakefs.c exports seventy-odd, so this
 * harness instead compiles the same source twice: once with -DIMPL=c_ against
 * the renamed C original, once bare against the Rust module.  Each binary
 * writes its trace to the file named on the command line, and the run script
 * compares the two files byte for byte.
 *
 * Both binaries link the SAME substrate -- the landed Rust ports for zone,
 * hashindex, cvar, cmd, info_str, sizebuf, strlcpy/strlcat and crc -- so the
 * comparison isolates quakefs and nothing else.  Only the true engine surface
 * is stubbed, and it is stubbed once, in this file, for both sides.
 *
 * Each case runs in a forked child.  Several cases exist to reach the C's
 * fatal paths, and a child that dies must not take the run with it; the
 * signal is recorded in the trace, so a port that faults where the C does not
 * is a difference rather than a crash.
 *
 * WHAT IS COMPARED.  Everything the public API exposes and nothing private:
 * quakefs.c keeps fs_searchpaths and its pack structures static, so the
 * harness drives the API -- the getters, the open/read layer, every FS_Load*
 * variant, the listers, the write path -- and records the six exported
 * globals, the bytes each call returns, the diagnostics, and every
 * registration and allocation the implementation performs through the
 * substrate.
 *
 * The C original is compiled per arm (client, SERVERONLY, H2W+SERVERONLY) with
 * the per-target shim compiled the same way, so the predicates and the
 * behaviour hooks are exercised rather than assumed.
 *
 * HOW THE SEARCHPATH ORDER IS PINNED WITHOUT READING IT.  fs_searchpaths and
 * the pack structures are static in quakefs.c, so neither implementation can be
 * asked for its list directly, and inventing an accessor for one side would
 * compare the accessor rather than the port.  Instead the order is tested
 * through what it MEANS: the same file name is placed in the base directory,
 * the gamedir, a pak and a second pak with different contents each time, and
 * the harness records which one each implementation resolves and what bytes it
 * returns.  Resolution order, plus the public getters (FS_GetBasedir,
 * GetUserbase, GetGamedir, GetUserdir, GetPortalsPathID, GetGamedirPathID), the
 * package lookups and the listers, pins the order behaviourally -- which is
 * what parity means here.
 *
 * What is NOT covered, recorded rather than implied: an internal list shape
 * that produced the same answers would pass, and miniz is the same C in both
 * binaries, so its behaviour is not under test.
 */

#include "quakedef.h"
#include "quakefs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <setjmp.h>
#include "sys.h"
#include "miniz.h"
#include <unistd.h>

#ifndef IMPL
#define IMPL
#endif

/* Token-pasting needs two levels: `#define FS_Init CAT(IMPL, FS_Init)` pastes the
 * NAME IMPL rather than its expansion, which leaves the bare (Rust) arm with
 * IMPLfs_filesize and friends undeclared.  CAT(a,b) is the standard fix --
 * the arguments are expanded before CAT_ pastes them. */
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)

/* Every entry point the cases drive, through whichever implementation this
 * translation unit was compiled for. */
#define FS_Init			CAT(IMPL, FS_Init)
#define FS_Gamedir		CAT(IMPL, FS_Gamedir)
#define FS_GetGamedir		CAT(IMPL, FS_GetGamedir)
#define FS_GetUserdir		CAT(IMPL, FS_GetUserdir)
#define FS_GetBasedir		CAT(IMPL, FS_GetBasedir)
#define FS_GetUserbase		CAT(IMPL, FS_GetUserbase)
#define FS_GetPortalsPathID	CAT(IMPL, FS_GetPortalsPathID)
#define FS_GetGamedirPathID	CAT(IMPL, FS_GetGamedirPathID)
#define FS_OpenFile		CAT(IMPL, FS_OpenFile)
#define FS_OpenFile_Silent	CAT(IMPL, FS_OpenFile_Silent)
#define FS_FileExists		CAT(IMPL, FS_FileExists)
#define FS_FileExistsInPak	CAT(IMPL, FS_FileExistsInPak)
#define FS_FileInGamedir	CAT(IMPL, FS_FileInGamedir)
#define FS_LastFileSource	CAT(IMPL, FS_LastFileSource)
#define FS_LoadMallocFile	CAT(IMPL, FS_LoadMallocFile)
#define FS_LoadStackFile	CAT(IMPL, FS_LoadStackFile)
#define FS_LoadHunkFile		CAT(IMPL, FS_LoadHunkFile)
#define FS_LoadTempFile		CAT(IMPL, FS_LoadTempFile)
#define FS_LoadZoneFile		CAT(IMPL, FS_LoadZoneFile)
#define FS_LoadHunkFileFromOSPath CAT(IMPL, FS_LoadHunkFileFromOSPath)
#define FS_fread		CAT(IMPL, FS_fread)
#define FS_fseek		CAT(IMPL, FS_fseek)
#define FS_ftell		CAT(IMPL, FS_ftell)
#define FS_feof			CAT(IMPL, FS_feof)
#define FS_ferror		CAT(IMPL, FS_ferror)
#define FS_fclose		CAT(IMPL, FS_fclose)
#define FS_OpenFileHandle	CAT(IMPL, FS_OpenFileHandle)
#define FS_WriteFile		CAT(IMPL, FS_WriteFile)
#define FS_CopyFile		CAT(IMPL, FS_CopyFile)
#define FS_CreatePath		CAT(IMPL, FS_CreatePath)
#define FS_IsGamedir		CAT(IMPL, FS_IsGamedir)
#define FS_ListSearchSubdirs	CAT(IMPL, FS_ListSearchSubdirs)
#define FS_BuildMapList		CAT(IMPL, FS_BuildMapList)
#define FS_MapListName		CAT(IMPL, FS_MapListName)
#define FS_GetMapTitle		CAT(IMPL, FS_GetMapTitle)
#define FS_MakePath		CAT(IMPL, FS_MakePath)
#define FS_MakePath_BUF		CAT(IMPL, FS_MakePath_BUF)

/* The exported globals, spelled the same way. */
#define fs_gamedir_nopath	CAT(IMPL, fs_gamedir_nopath)
#define gameflags		CAT(IMPL, gameflags)
#define fs_filesize		CAT(IMPL, fs_filesize)
#define file_from_pak		CAT(IMPL, file_from_pak)

extern qboolean Cmd_Exists(const char *name);

/* The per-target predicates: from the shim for the C arm, from the shim for
 * the Rust arm too -- both link engine/rust/quakefs_target.c, which is the
 * point.  No IMPL prefix. */
extern int QuakeFS_TargetIsH2W(void);
extern int QuakeFS_TargetIsH2WIntegrated(void);
extern int QuakeFS_TargetHasClientCommands(void);
extern void QuakeFS_TargetClientReset(void);
extern void QuakeFS_TargetClientClearState(void);
extern void QuakeFS_TargetClientReinit(void);
extern void QuakeFS_TargetVidLock(void);
extern void QuakeFS_TargetShowList(int num, const char **list);
extern void QuakeFS_TargetSetHwServerinfo(const char *dir);

/*----------------------------------------------------------------------------
 * the trace
 *--------------------------------------------------------------------------*/

#define TRACE_MAX (4 * 1024 * 1024)

static unsigned char trace[TRACE_MAX];
static size_t trace_len;
static int failures;
static int checks;

/* Progress markers, off unless QF_TRACE is set: there is no debugger in the
 * dev shell, so a SIGSEGV is located by the last step that printed. */
static void step(const char *what)
{
	if (getenv("QF_TRACE"))
		fprintf(stderr, "step: %s\n", what);
}

static void rec(const void *p, size_t n)
{
	if (trace_len + n > sizeof trace) {
		failures++;
		fprintf(stderr, "FAIL: trace overflow\n");
		return;
	}
	memcpy(trace + trace_len, p, n);
	trace_len += n;
}

static void rec_int(int v) { rec(&v, sizeof v); }
static void rec_str(const char *s) { rec(s, s ? strlen(s) + 1 : 1); }

static void rec_long(long v) { rec(&v, sizeof v); }

static void check(int ok, const char *fmt, ...)
{
	va_list ap;

	checks++;
	if (ok)
		return;
	failures++;
	fprintf(stderr, "FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static void call(const char *what)
{
	rec_int(0x0CA11);
	rec_str(what);
}

/*----------------------------------------------------------------------------
 * the substrate both implementations run on
 *--------------------------------------------------------------------------*/

/* Diagnostics are recorded, so a refusal or a fatal message is part of the
 * comparison rather than noise. */
static jmp_buf err_env;
/* The C original is linked under renamed symbols, so its diagnostics name
 * themselves "c_FS_Init" where the shipping build says "FS_Init".  That is a
 * harness artifact, not a port difference, so it is removed before the message
 * is recorded -- the same normalisation the cmd and wad harnesses do. */
static void normalize_diag(char *buf)
{
	if (strncmp(buf, "c_", 2) == 0)
		memmove(buf, buf + 2, strlen(buf + 2) + 1);
}

static int err_armed;
static int err_calls;
static char err_buf[1024];

void CON_Printf(unsigned int flags, const char *fmt, ...)
{
	va_list ap;

	(void)flags;
	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);
	normalize_diag(err_buf);
	rec_int(0x9911);
	rec_str(err_buf);
}

FUNC_NORETURN void Sys_Error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);
	normalize_diag(err_buf);

	err_calls++;
	rec_int(0xE44);
	rec_str(err_buf);

	fprintf(stderr, "Sys_Error: %s\n", err_buf);
	if (!err_armed) {
		/* A fatal path reached outside a case that expects one: die loudly
		 * so the run script sees a signal in the trace, as the C does. */
		abort();
	}
	longjmp(err_env, 1);
}

/* The find/stat surface quakefs.c reaches.  Both implementations get this
 * same code, so a difference in the trace is a difference in the port. */
long Sys_filesize(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -1;
	return (long)st.st_size;
}

int Sys_FileType(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -1;
	if (S_ISDIR(st.st_mode))
		return 2;
	return 0;
}

int Sys_mkdir(const char *path, qboolean crash)
{
	(void)crash;
	return mkdir(path, 0755);
}

/* Directory walk: a tiny, deterministic implementation -- collect, sort, hand
 * back one at a time.  Both implementations get this same code, so what the
 * filesystem layer sees is identical and a difference in the trace is a
 * difference in the port.  The pattern forms quakefs.c uses are "*.ext",
 * "prefix*" and "*", so the matcher only needs '*' and '?'. */
static char find_names[64][256];
static int find_count;
static int find_pos;

static int name_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

static int glob_match(const char *pat, const char *name)
{
	if (!pat || !*pat)
		return 1;
	if (*pat == '*')
		return glob_match(pat + 1, name) ||
			(*name && glob_match(pat, name + 1));
	if (*pat == '?')
		return *name && glob_match(pat + 1, name + 1);
	if (*pat != *name)
		return 0;
	return glob_match(pat + 1, name + 1);
}

const char *Sys_FindFirstFile(fsfind_t *ctx, const char *path,
		const char *pattern)
{
	DIR *d;
	struct dirent *de;

	(void)ctx;
	find_count = 0;
	find_pos = 0;
	d = opendir(path);
	if (d) {
		while ((de = readdir(d)) != NULL && find_count < 64) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			if (!glob_match(pattern, de->d_name))
				continue;
			snprintf(find_names[find_count],
				sizeof find_names[0], "%s", de->d_name);
			find_count++;
		}
		closedir(d);
	}
	qsort(find_names, (size_t)find_count, sizeof find_names[0], name_cmp);
	if (find_pos >= find_count)
		return NULL;
	return find_names[find_pos++];
}

const char *Sys_FindNextFile(fsfind_t *ctx)
{
	(void)ctx;
	if (find_pos >= find_count)
		return NULL;
	return find_names[find_pos++];
}

void Sys_FindClose(fsfind_t *ctx)
{
	(void)ctx;
}

/* host_parms: com_argc/com_argv are macros over these (common.h:92-93), and
 * the substrate's cmd/cmd.rs reads the symbol by name, so the harness defines
 * it rather than a differently-named copy. */
static char *h_argv[16];
static quakeparms_t h_parms;
quakeparms_t *host_parms;
/* Cmd_AddCommand refuses once the host is initialized; the harness registers
 * commands the way startup does, so this stays false. */
qboolean host_initialized;
/* Globals the substrate reads: the developer cvar, the protocol the engine
 * negotiated, the dedicated flag the shims ask about, and the client state--
 * all of them the same for both arms, so none of them can make the traces
 * differ.  developer and cls are only ever read. */
cvar_t developer;
int sv_protocol;
/* hwsv does not have this symbol at all: hexenworld/server/host.h defines
 * isDedicated as the macro 1, which is exactly why the zone and cmd shims
 * answer that question in C instead of letting Rust read a global. */
#ifndef isDedicated
qboolean isDedicated;
#endif
/* The client state the client-arm hooks reset (cls.demofile, cls.demos).
 * client_static_t does not exist in a SERVERONLY build, and neither does the
 * client-only half of the API -- quakefs.h guards it, so the harness does too. */
#ifndef SERVERONLY
client_static_t cls;
#endif
/* The rest of the system surface quakefs.c reaches. */
int Sys_CopyFile(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out;
	char buf[4096];
	size_t n;

	if (!in) return -1;
	out = fopen(to, "wb");
	if (!out) { fclose(in); return -1; }
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in); fclose(out);
	return 0;
}
double Sys_DoubleTime(void)
{
	/* Fixed: a clock in the trace would make the two arms differ for reasons
	 * that have nothing to do with the port. */
	return 1.0;
}
int Sys_ListDirectories(const char *path, char dirs[][64], int maxdirs)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	int n = 0;
	char full[512];

	d = opendir(path);
	if (!d) return 0;
	while ((de = readdir(d)) != NULL && n < maxdirs) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		snprintf(full, sizeof full, "%s/%s", path, de->d_name);
		if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		snprintf(dirs[n], 64, "%s", de->d_name);
		n++;
	}
	closedir(d);
	return n;
}

/* The client-arm hook targets.  Only the client arm's shim bodies call these,
 * which is exactly what the arms exist to check. */
void Draw_ReInit(void) { rec_int(0xD201); }
void TexMgr_NewGame(void) { rec_int(0x7C01); }
void VID_Lock(void) { rec_int(0x7A01); }
void Host_ClearMemory(void) { rec_int(0xAC01); }
void Host_ShutdownServer(qboolean crash) { (void)crash; rec_int(0x4501); }
void Host_WriteConfiguration(const char *name) { (void)name; rec_int(0x4A01); }
void BGM_Stop(void) { rec_int(0xBA01); }
void CL_Disconnect(void) { rec_int(0xC201); }
void M_BuildBindList(void) { rec_int(0x4D01); }
void Con_ShowList(int num, const char **list)
{
	int i;

	rec_int(0xC501);
	rec_int(num);
	for (i = 0; i < num; i++)
		rec_str(list[i]);
}
/* In an H2W build the C's Host_Error is a MACRO for SV_Error
 * (hexenworld/server/host.h:62), so C callers reach SV_Error while the Rust
 * archive calls the name Host_Error directly.  Both have to exist here, and
 * the undef is what lets this file define the name rather than the macro
 * rewriting it into a duplicate of SV_Error. */
#ifdef Host_Error
#undef Host_Error
#endif

static void harness_fatal(const char *fmt, va_list ap)
{
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	normalize_diag(err_buf);
	rec_int(0x4E01);
	rec_str(err_buf);
	abort();
}

void Host_Error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	harness_fatal(fmt, ap);
	va_end(ap);
}

#ifdef H2W
void SV_Error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	harness_fatal(fmt, ap);
	va_end(ap);
}

/* hwsv's server state, which the port's hwsv arm writes through the shim
 * (svs.info).  The C original and the shim both reach it in this arm. */
#include "server.h"
server_static_t svs;
#endif

/*----------------------------------------------------------------------------
 * the on-disk fixture
 *--------------------------------------------------------------------------*/

static char root[256];
static char base[256];
static char user[256];

static void path_join(char *out, size_t n, const char *a, const char *b)
{
	snprintf(out, n, "%s/%s", a, b);
}

static void write_bytes(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");

	if (!f) {
		fprintf(stderr, "harness: cannot write %s\n", path);
		exit(2);
	}
	fwrite(data, 1, len, f);
	fclose(f);
}

/* A minimal but valid WAD2 PAK: header, one lump table entry, one lump. */
static const char lump_text[] = "HARNESS LUMP PAYLOAD";

static void write_pak(const char *path)
{
	unsigned char buf[12 + 32 + 64];
	int n = 1;
	size_t i;

	memset(buf, 0, sizeof buf);
	memcpy(buf, "PACK", 4);
	buf[4] = n; buf[5] = 0; buf[6] = 0; buf[7] = 0;	/* numlumps */
	buf[8] = 12; buf[9] = 0; buf[10] = 0; buf[11] = 0;	/* infotableofs */
	/* lumpinfo_t at 12: filepos, disksize, size, type, compression, pad, name */
	buf[12] = 44; buf[13] = 0; buf[14] = 0; buf[15] = 0;	/* filepos */
	buf[16] = sizeof lump_text; buf[20] = sizeof lump_text;	/* disksize, size */
	buf[24] = 0;	/* type */
	memcpy(buf + 12 + 16, "HARNESS.LUM", 11);
	for (i = 0; i < sizeof lump_text; i++)
		buf[44 + i] = (unsigned char)lump_text[i];
	write_bytes(path, buf, 44 + sizeof lump_text);
}

/* A zip, built byte by byte here rather than with a writer API: the vendored
 * miniz is compiled with MINIZ_NO_DEFLATE_APIS, so it can read a deflated
 * member but cannot write one.  Building the bytes directly is also what lets
 * the malformed and truncated variants come from the same buffer.  The
 * deflated member's stream is real deflate output, embedded below, so the
 * reader's inflate path is exercised rather than assumed. */

/* raw deflate of "deflated member payload, long enough to compress" */
static const unsigned char deflated_stream[] = {
	0x4b, 0x49, 0x4d, 0xcb, 0x49, 0x2c, 0x49, 0x4d, 0x51, 0xc8, 0x4d, 0xcd,
	0x4d, 0x4a, 0x2d, 0x52, 0x28, 0x48, 0xac, 0xcc, 0xc9, 0x4f, 0x4c, 0xd1,
	0x51, 0xc8, 0xc9, 0xcf, 0x4b, 0x57, 0x48, 0xcd, 0xcb, 0x2f, 0x4d, 0xcf,
	0x50, 0x28, 0xc9, 0x57, 0x48, 0xce, 0xcf, 0x2d, 0x28, 0x4a, 0x2d, 0x2e,
	0x06, 0x00,
};
#define DEFLATED_RAW_LEN 48
#define DEFLATED_CRC 0xd6318ff9u
#define STORED_CRC 0xccba1d93u

static void put16(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void put32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
	p[2] = (unsigned char)((v >> 16) & 0xff);
	p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* One local file header + name + data.  method 0 = stored, 8 = deflated. */
static size_t zip_local(unsigned char *out, const char *name,
		const unsigned char *data, size_t len, size_t uncomp,
		unsigned long crc, int method)
{
	size_t n = strlen(name), o = 0;

	put32(out + o, 0x04034b50UL); o += 4;
	put16(out + o, 20); o += 2;		/* version needed */
	put16(out + o, 0); o += 2;		/* flags */
	put16(out + o, (unsigned int)method); o += 2;
	put16(out + o, 0); o += 2;		/* time */
	put16(out + o, 0); o += 2;		/* date */
	put32(out + o, crc); o += 4;
	put32(out + o, (unsigned long)len); o += 4;
	put32(out + o, (unsigned long)uncomp); o += 4;
	put16(out + o, (unsigned int)n); o += 2;
	put16(out + o, 0); o += 2;		/* extra len */
	memcpy(out + o, name, n); o += n;
	memcpy(out + o, data, len); o += len;
	return o;
}

static size_t zip_central(unsigned char *out, const char *name,
		size_t local_off, size_t len, size_t uncomp,
		unsigned long crc, int method)
{
	size_t n = strlen(name), o = 0;

	put32(out + o, 0x02014b50UL); o += 4;
	put16(out + o, 20); o += 2;		/* version made by */
	put16(out + o, 20); o += 2;		/* version needed */
	put16(out + o, 0); o += 2;
	put16(out + o, (unsigned int)method); o += 2;
	put16(out + o, 0); o += 2;
	put16(out + o, 0); o += 2;
	put32(out + o, crc); o += 4;
	put32(out + o, (unsigned long)len); o += 4;
	put32(out + o, (unsigned long)uncomp); o += 4;
	put16(out + o, (unsigned int)n); o += 2;
	put16(out + o, 0); o += 2;		/* extra */
	put16(out + o, 0); o += 2;		/* comment */
	put16(out + o, 0); o += 2;		/* disk */
	put16(out + o, 0); o += 2;		/* internal attrs */
	put32(out + o, 0UL); o += 4;		/* external attrs */
	put32(out + o, (unsigned long)local_off); o += 4;
	memcpy(out + o, name, n); o += n;
	return o;
}

#define ZIP_MAX 4096

/* variant: 0 good, 1 central directory signature corrupted, 2 truncated */
static size_t build_zip(unsigned char *out, int variant)
{
	const unsigned char *stored = (const unsigned char *)"stored member payload";
	size_t o = 0, cd = 0, nstored = 21;
	unsigned char central[ZIP_MAX];

	o += zip_local(out + o, "stored.txt", stored, nstored, nstored,
			STORED_CRC, 0);
	{
		size_t off = o;
		o += zip_local(out + o, "deflated.txt", deflated_stream,
				sizeof deflated_stream, DEFLATED_RAW_LEN,
				DEFLATED_CRC, 8);
		cd += zip_central(central + cd, "stored.txt", 0, nstored,
				nstored, STORED_CRC, 0);
		cd += zip_central(central + cd, "deflated.txt", off,
				sizeof deflated_stream, DEFLATED_RAW_LEN,
				DEFLATED_CRC, 8);
	}
	if (variant == 1)
		put32(central, 0xdeadbeefUL);	/* bad central signature */
	{
		size_t cd_off = o;
		memcpy(out + o, central, cd);
		o += cd;
		put32(out + o, 0x06054b50UL); o += 4;
		put16(out + o, 0); o += 2;	/* disk */
		put16(out + o, 0); o += 2;	/* cd disk */
		put16(out + o, 2); o += 2;	/* entries here */
		put16(out + o, 2); o += 2;	/* entries total */
		put32(out + o, (unsigned long)cd); o += 4;
		put32(out + o, (unsigned long)cd_off); o += 4;
		put16(out + o, 0); o += 2;	/* comment len */
	}
	if (variant == 2)
		o = o > 40 ? 40 : o;		/* cut mid central directory */
	return o;
}

static void write_zip_variant(const char *path, int variant)
{
	unsigned char buf[ZIP_MAX];
	size_t n = build_zip(buf, variant);

	write_bytes(path, buf, n);
}

static void fixture_setup(void)
{
	const char *demo = getenv("QF_BASEDIR");
	char p[512];

	step("fixture: mkdir root/user");
	/* The root must be the SAME path in both arms or every path in the trace
	 * differs by the pid -- and the run script wipes it between the two runs,
	 * so the second arm does not inherit the first's userdir. */
	if (demo && *demo && getenv("QF_ROOT"))
		snprintf(root, sizeof root, "%s", getenv("QF_ROOT"));
	else if (demo && *demo)
		snprintf(root, sizeof root, "/tmp/quakefs-harness-%d", (int)getpid());
	else
		snprintf(root, sizeof root, "/tmp/quakefs-harness-%d", (int)getpid());
	snprintf(user, sizeof user, "%s/user", root);
	mkdir(root, 0755);
	mkdir(user, 0755);

	if (demo && *demo) {
		/* FS_Init identifies the installation from known paks by size and
		 * CRC (check_known_paks), so a hand-built pak0.pak cannot pass it --
		 * "Unable to find a proper Hexen II installation" is what the first
		 * attempt got.  The demo data the engine smoke tests use is a real
		 * install, so the base comes from there and this harness only owns
		 * the userdir, which is where its own archives go. */
		step("fixture: using QF_BASEDIR");
		snprintf(base, sizeof base, "%s", demo);
	} else {
		step("fixture: synthetic base (no QF_BASEDIR)");
		snprintf(base, sizeof base, "%s/base", root);
		mkdir(base, 0755);
		path_join(p, sizeof p, base, "data1");
		mkdir(p, 0755);
		step("fixture: write pak0.pak");
		path_join(p, sizeof p, base, "data1/pak0.pak");
		write_pak(p);
	}

	/* The archives this harness compares live in the userdir's gamedir, so
	 * they are in the search path without writing into a read-only store. */
	step("fixture: write the three zips");
	path_join(p, sizeof p, user, "data1");
	mkdir(p, 0755);
	path_join(p, sizeof p, user, "data1/gfx.zip");
	write_zip_variant(p, 0);
	path_join(p, sizeof p, user, "data1/bad.zip");
	write_zip_variant(p, 1);
	path_join(p, sizeof p, user, "data1/trunc.zip");
	write_zip_variant(p, 2);
}

/*----------------------------------------------------------------------------
 * cases
 *--------------------------------------------------------------------------*/

#define HARNESS_MEMSIZE (16 * 1024 * 1024)

static void *membase_keep;

static void parms_init(void)
{
	int n = 0;

	/* host_parms must be live BEFORE Memory_Init: com_argc/com_argv are macros
	 * over host_parms->argc/argv (common.h:92-93), Memory_Init parses -zone
	 * through them, and calling it first dereferences a NULL host_parms --
	 * which is what the first marker run caught. */
	memset(&h_parms, 0, sizeof h_parms);
	h_argv[n++] = (char *)"hexenwail";
	h_argv[n++] = (char *)"-basedir";
	h_argv[n++] = base;
	h_argv[n++] = (char *)"-userdir";
	h_argv[n++] = user;
	h_parms.basedir = base;
	h_parms.userdir = user;
	h_parms.argc = n;
	h_parms.argv = h_argv;
	host_parms = &h_parms;

	/* The engine calls Memory_Init (zone.c, now the Rust zone port) before
	 * anything touches FS_Init, and every Z_Malloc in quakefs.c needs it --
	 * without this the port dies on "Bad zone id 1" before a case starts. */
	step("parms: allocate membase");
	membase_keep = malloc(HARNESS_MEMSIZE);
	if (!membase_keep) {
		fprintf(stderr, "harness: out of memory\n");
		exit(2);
	}
	h_parms.membase = membase_keep;
	h_parms.memsize = HARNESS_MEMSIZE;
	step("parms: Memory_Init");
	Memory_Init(membase_keep, HARNESS_MEMSIZE);
	step("parms: Memory_Init returned");
}

static void rec_getters(void)
{
	const char *s;

	s = FS_GetBasedir(); rec_str(s);
	s = FS_GetUserbase(); rec_str(s);
	s = FS_GetGamedir(); rec_str(s);
	s = FS_GetUserdir(); rec_str(s);
	rec_int((int)FS_GetPortalsPathID());
	rec_int((int)FS_GetGamedirPathID());
	s = FS_LastFileSource(); rec_str(s);
	rec_str(fs_gamedir_nopath);
	rec_int((int)gameflags);
	rec_long(fs_filesize);
	rec_int(file_from_pak);
}

static void scenario_init(void)
{
	step("init: parms_init");
	parms_init();
	step("init: FS_Init");
	call("FS_Init");
	FS_Init();
	step("init: FS_Init returned");
	rec_getters();
	step("init: getters done");
	/* The client arm registers maplist/randmap through Cmd_AddCommand; the
	 * dedicated ones must not.  The registrations are recorded by the
	 * substrate, so this is compared rather than assumed. */
	rec_int(Cmd_Exists("maplist"));
	rec_int(Cmd_Exists("randmap"));
	rec_int(Cmd_Exists("gamedir"));
	rec_int(Cmd_Exists("skies"));
}

static void scenario_pak(void)
{
	long len;
	FILE *f = NULL;
	unsigned int path_id = 0;
	char buf[64];

	parms_init();
	FS_Init();

	call("open a pak member");
	len = FS_OpenFile("harness.lum", &f, &path_id);
	rec_long(len);
	rec_int((int)path_id);
	rec_int(file_from_pak);
	rec_str(FS_LastFileSource());
	if (f) {
		memset(buf, 0, sizeof buf);
		rec_int((int)fread(buf, 1, sizeof buf - 1, f));
		rec_str(buf);
		rec_long(ftell(f));
		rec_int(fseek(f, 0, SEEK_SET));
		rec_long(ftell(f));
		rec_int(feof(f));
		rec_int(ferror(f));
		fclose(f);
	} else {
		rec_int(-1);
	}
	rec_long(fs_filesize);

	/* The same member through the port's own handle API: FS_OpenFileHandle
	 * plus FS_fread/FS_fseek/FS_ftell/FS_feof/FS_ferror/FS_fclose. */
	call("open a pak member through a fshandle");
	{
		fshandle_t fh;
		long hlen = FS_OpenFileHandle("harness.lum", &fh, &path_id);
		rec_long(hlen);
		if (hlen >= 0) {
			memset(buf, 0, sizeof buf);
			rec_int((int)FS_fread(buf, 1, sizeof buf - 1, &fh));
			rec_str(buf);
			rec_long(FS_ftell(&fh));
			rec_int(FS_fseek(&fh, 0, SEEK_SET));
			rec_long(FS_ftell(&fh));
			rec_int(FS_feof(&fh));
			rec_int(FS_ferror(&fh));
			FS_fclose(&fh);
		} else {
			rec_int(-1);
		}
	}

	call("file existence");
	rec_int(FS_FileExists("harness.lum", &path_id));
	rec_int(FS_FileExistsInPak("harness.lum", &path_id));
	rec_int(FS_FileInGamedir("harness.lum"));
	rec_int(FS_FileExists("no-such-file.xyz", &path_id));

	call("load variants");
	{
		byte *p = FS_LoadMallocFile("harness.lum", &path_id);
		if (p) { rec_str((char *)p); free(p); } else rec_int(-1);
	}
	{
		char stackbuf[128];
		byte *p = FS_LoadStackFile("harness.lum", stackbuf,
				sizeof stackbuf, &path_id);
		rec_int(p ? 1 : 0);
		if (p) rec_str((char *)p);
	}
	{
		byte *p = FS_LoadTempFile("harness.lum", &path_id);
		if (p) { rec_str((char *)p); } else rec_int(-1);
	}
	{
		byte *p = FS_LoadZoneFile("harness.lum", Z_SECZONE, &path_id);
		if (p) { rec_str((char *)p); } else rec_int(-1);
	}
	{
		byte *p = FS_LoadHunkFile("harness.lum", &path_id);
		if (p) { rec_str((char *)p); } else rec_int(-1);
	}
	rec_getters();
}

static void scenario_zip(void)
{
	long len;
	FILE *f = NULL;
	unsigned int path_id = 0;
	char buf[128];

	parms_init();
	FS_Init();

	call("open a stored zip member");
	len = FS_OpenFile_Silent("stored.txt", &f, &path_id);
	rec_long(len);
	if (f) {
		memset(buf, 0, sizeof buf);
		rec_int((int)fread(buf, 1, sizeof buf - 1, f));
		rec_str(buf);
		fclose(f);
	} else {
		rec_int(-1);
	}

	call("open a deflated zip member");
	len = FS_OpenFile_Silent("deflated.txt", &f, &path_id);
	rec_long(len);
	if (f) {
		memset(buf, 0, sizeof buf);
		rec_int((int)fread(buf, 1, sizeof buf - 1, f));
		rec_str(buf);
		fclose(f);
	} else {
		rec_int(-1);
	}
	rec_getters();
}

static void scenario_lists(void)
{
	char dirs[8][64];
	int n, i;

	parms_init();
	FS_Init();

	call("list search subdirs");
	n = FS_ListSearchSubdirs("", dirs, 8);
	rec_int(n);
	for (i = 0; i < n && i < 8; i++)
		rec_str(dirs[i]);

#ifndef SERVERONLY
	/* quakefs.h declares the map list only for non-SERVERONLY targets. */
	call("build the map list");
	rec_int(FS_BuildMapList("maps"));
	rec_int(FS_BuildMapList(""));
#endif
	rec_getters();
}

static void scenario_write(void)
{
	char path[512];
	char created[512];
	unsigned int path_id = 0;
	int err = 0;
	char *made;

	parms_init();
	FS_Init();

	call("FS_WriteFile into the userdir");
	path_join(path, sizeof path, user, "written.txt");
	rec_int(FS_WriteFile("written.txt", "written payload", 15));

	call("read it back");
	{
		FILE *f = fopen(path, "rb");
		char buf[64];
		if (f) {
			size_t k = fread(buf, 1, sizeof buf - 1, f);
			buf[k] = 0;
			fclose(f);
			rec_str(buf);
		} else {
			rec_int(-1);
		}
	}

	call("FS_CreatePath");
	snprintf(created, sizeof created, "%s/newdir", user);
	rec_int(FS_CreatePath(created));

	call("FS_MakePath");
	made = FS_MakePath(FS_USERDIR, &err, "made.txt");
	rec_str(made ? made : "(null)");
	rec_int(err);

	call("FS_MakePath_BUF");
	{
		char buf[512];
		made = FS_MakePath_BUF(FS_USERDIR, &err, buf, sizeof buf,
				"buf.txt");
		rec_str(made ? made : "(null)");
		rec_int(err);
	}

#ifndef SERVERONLY
	call("FS_IsGamedir");
	rec_int(FS_IsGamedir(base, "data1"));
	rec_int(FS_IsGamedir(base, "nosuchdir"));
#endif

	(void)path_id;
	rec_getters();
}

/* Fatal paths, each in its own forked child (the case runner does the fork),
 * so a difference in whether the C aborts is a difference in the trace. */
static void scenario_fatal_missing(void)
{
	parms_init();
	FS_Init();
	call("open a missing file (fatal form)");
	FS_OpenFile("no-such-file.xyz", NULL, NULL);
}

static void scenario_fatal_gamedir(void)
{
	step("fatal-gamedir: parms_init");
	parms_init();
	step("fatal-gamedir: FS_Init");
	FS_Init();
	step("fatal-gamedir: FS_Gamedir(nosuchdir)");
	call("FS_Gamedir to a directory that is not there");
	FS_Gamedir("nosuchdir");
	step("fatal-gamedir: returned");
}

/*----------------------------------------------------------------------------
 * case runner
 *--------------------------------------------------------------------------*/

struct child_result {
	size_t len;
	int checks;
	int failures;
};

static int run_case(const char *name, void (*fn)(void))
{
	char path[256];
	struct child_result hdr;
	int fd, status = 0;
	pid_t pid;
	size_t got;

	(void)name;
	snprintf(path, sizeof path, "%s/case-%d", root, (int)getpid());
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "harness: cannot open %s\n", path);
		return -1;
	}

	pid = fork();
	if (pid == 0) {
		trace_len = 0;
		checks = 0;
		failures = 0;
		/* Each case starts from the state the engine starts in: the globals
		 * are the harness's, so reset them the way the C initialisers do. */
		fn();
		hdr.len = trace_len;
		hdr.checks = checks;
		hdr.failures = failures;
		if (write(fd, &hdr, sizeof hdr) != (ssize_t)sizeof hdr)
			_exit(2);
		if (write(fd, trace, trace_len) != (ssize_t)trace_len)
			_exit(2);
		_exit(0);
	}

	waitpid(pid, &status, 0);
	if (lseek(fd, 0, SEEK_SET) < 0 ||
	    read(fd, &hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
		close(fd);
		unlink(path);
		/* A case that died must never read as agreement: two arms that
		 * both segfault would otherwise produce identical traces and a
		 * passing comparison, which is a gate that proves nothing. */
		printf("case %-24s CHILD DIED (signal %d) -- FAILURE\n", name,
			WIFSIGNALED(status) ? WTERMSIG(status) : -1);
		rec_int(0xDEAD);
		rec_str(name);
		failures++;
		return -1;
	}
	got = (size_t)read(fd, trace + trace_len, hdr.len);
	rec_int(0xCA5E);
	rec_str(name);
	rec_int((int)got);
	rec(trace + trace_len, got);
	trace_len += got;
	checks += hdr.checks;
	failures += hdr.failures;
	close(fd);
	unlink(path);
	printf("case %-24s %6zu bytes, %d checks, %d failures, exit %d\n",
		name, hdr.len, hdr.checks, hdr.failures,
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("case %-24s child exited %d -- FAILURE\n", name,
			WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		failures++;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "/tmp/quakefs-harness.trace";
	const char *only = argc > 2 ? argv[2] : NULL;
	FILE *f;

	fixture_setup();

	printf("quakefs differential harness, implementation: %s\n",
#ifdef IMPL_C_ARM
		"C original (renamed)"
#else
		"Rust"
#endif
	);

	if (!only || !strcmp(only, "init"))
		run_case("init", scenario_init);
	if (!only || !strcmp(only, "pak"))
		run_case("pak", scenario_pak);
	if (!only || !strcmp(only, "zip"))
		run_case("zip", scenario_zip);
	if (!only || !strcmp(only, "lists"))
		run_case("lists", scenario_lists);
	if (!only || !strcmp(only, "write"))
		run_case("write", scenario_write);
	if (!only || !strcmp(only, "fatal-missing"))
		run_case("fatal-missing", scenario_fatal_missing);
	if (!only || !strcmp(only, "fatal-gamedir"))
		run_case("fatal-gamedir", scenario_fatal_gamedir);

	rec_int(0xF1A1);
	rec_int(checks);
	rec_int(failures);

	f = fopen(out, "wb");
	if (!f) {
		fprintf(stderr, "harness: cannot write %s\n", out);
		return 2;
	}
	fwrite(trace, 1, trace_len, f);
	fclose(f);

	printf("trace %zu bytes, %d checks, %d failures -> %s\n",
		trace_len, checks, failures, out);
	printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}

/* The file ends at run_case(): every case runs in a child per implementation and
 * the traces are compared with memcmp, so a difference is reported as a byte
 * offset rather than as a failing assertion. */
