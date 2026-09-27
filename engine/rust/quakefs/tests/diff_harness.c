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
 * What is NOT covered, recorded rather than implied: the private searchpath
 * and pack structures are never walked (they are static), so a port that built
 * the same answers through a different list shape would pass; and miniz is the
 * same C in both binaries, so its behaviour is not under test.
 */

#include "quakedef.h"
#include "quakefs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <setjmp.h>
#include "sys.h"
#include <unistd.h>

#ifndef IMPL
#define IMPL
#endif

/* Every entry point the cases drive, through whichever implementation this
 * translation unit was compiled for. */
#define FS_Init			IMPL##FS_Init
#define FS_Gamedir		IMPL##FS_Gamedir
#define FS_GetGamedir		IMPL##FS_GetGamedir
#define FS_GetUserdir		IMPL##FS_GetUserdir
#define FS_GetBasedir		IMPL##FS_GetBasedir
#define FS_GetUserbase		IMPL##FS_GetUserbase
#define FS_GetPortalsPathID	IMPL##FS_GetPortalsPathID
#define FS_GetGamedirPathID	IMPL##FS_GetGamedirPathID
#define FS_OpenFile		IMPL##FS_OpenFile
#define FS_OpenFile_Silent	IMPL##FS_OpenFile_Silent
#define FS_FileExists		IMPL##FS_FileExists
#define FS_FileExistsInPak	IMPL##FS_FileExistsInPak
#define FS_FileInGamedir	IMPL##FS_FileInGamedir
#define FS_LastFileSource	IMPL##FS_LastFileSource
#define FS_LoadMallocFile	IMPL##FS_LoadMallocFile
#define FS_LoadStackFile	IMPL##FS_LoadStackFile
#define FS_LoadHunkFile		IMPL##FS_LoadHunkFile
#define FS_LoadTempFile		IMPL##FS_LoadTempFile
#define FS_LoadZoneFile		IMPL##FS_LoadZoneFile
#define FS_LoadHunkFileFromOSPath IMPL##FS_LoadHunkFileFromOSPath
#define FS_fread		IMPL##FS_fread
#define FS_fseek		IMPL##FS_fseek
#define FS_ftell		IMPL##FS_ftell
#define FS_feof			IMPL##FS_feof
#define FS_ferror		IMPL##FS_ferror
#define FS_fclose		IMPL##FS_fclose
#define FS_WriteFile		IMPL##FS_WriteFile
#define FS_CopyFile		IMPL##FS_CopyFile
#define FS_CreatePath		IMPL##FS_CreatePath
#define FS_IsGamedir		IMPL##FS_IsGamedir
#define FS_ListSearchSubdirs	IMPL##FS_ListSearchSubdirs
#define FS_BuildMapList		IMPL##FS_BuildMapList
#define FS_MapListName		IMPL##FS_MapListName
#define FS_GetMapTitle		IMPL##FS_GetMapTitle
#define FS_MakePath		IMPL##FS_MakePath
#define FS_MakePath_BUF		IMPL##FS_MakePath_BUF

/* The exported globals, spelled the same way. */
#define fs_gamedir_nopath	IMPL##fs_gamedir_nopath
#define gameflags		IMPL##gameflags
#define fs_filesize		IMPL##fs_filesize
#define file_from_pak		IMPL##file_from_pak

extern int Cmd_Exists(const char *name);

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
	rec_int(0x9911);
	rec_str(err_buf);
}

FUNC_NORETURN void Sys_Error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);

	err_calls++;
	rec_int(0xE44);
	rec_str(err_buf);

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

int Sys_mkdir(const char *path, int crash)
{
	(void)crash;
	return mkdir(path, 0755);
}

/* Directory walk: a tiny sorted implementation, the same for both sides. */
static char find_names[64][256];
static int find_count;
static int find_pos;

static int name_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

const char *Sys_FindFirstFile(fsfind_t *ctx, const char *path, const char *pattern)
{
	char dirname[512];
	DIR *d;
	struct dirent *de;

	(void)pattern;
	(void)base;
	find_count = 0;
	find_pos = 0;
	snprintf(dirname, sizeof dirname, "%s", path);
	d = opendir(dirname);
	if (!d) {
		*ctxp = NULL;
		return;
	}
	while ((de = readdir(d)) != NULL && find_count < 64) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		snprintf(find_names[find_count], sizeof find_names[0], "%s",
			de->d_name);
		find_count++;
	}
	closedir(d);
	qsort(find_names, find_count, sizeof find_names[0], name_cmp);
	(void)ctx;
	return find_count ? find_names[find_pos++] : NULL;
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

/* host_parms: com_argc/com_argv are macros over these (common.h:92-93), so the
 * harness owns the storage and the init path. */
static char *h_argv[16];
static quakeparms_t h_parms;

/* The client-arm hook targets.  Only the client arm's shim bodies call these,
 * which is exactly what the arms exist to check. */
void Draw_ReInit(void) { rec_int(0xD201); }
void TexMgr_NewGame(void) { rec_int(0x7C01); }
void VID_Lock(void) { rec_int(0x7A01); }
void Host_ClearMemory(void) { rec_int(0xAC01); }
void Host_ShutdownServer(int total) { (void)total; rec_int(0x4501); }
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
void Host_Error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);
	rec_int(0x4E01);
	rec_str(err_buf);
	abort();
}
/* hwsv's arm writes serverinfo; the shim calls this in that arm only. */
void Info_SetValueForStarKey(char *s, const char *k, const char *v, int max)
{
	(void)s; (void)k; (void)v; (void)max;
	rec_int(0x1F01);
	rec_str(k ? k : "");
	rec_str(v ? v : "");
}

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

/* A zip with one stored and one deflated member, written with the same miniz
 * the port calls, so the archive is valid by construction. */
static void write_zip(const char *path)
{
	void *zip = mz_zip_writer_create();
	const char *stored = "stored member payload";
	const char *deflated = "deflated member payload, long enough to compress";

	if (!zip)
		exit(2);
	if (!mz_zip_writer_add_mem(zip, "stored.txt", stored,
			strlen(stored), MZ_NO_COMPRESSION) ||
	    !mz_zip_writer_add_mem(zip, "deflated.txt", deflated,
			strlen(deflated), MZ_DEFAULT_COMPRESSION) ||
	    !mz_zip_writer_finalize_archive(zip) ||
	    !mz_zip_writer_write_archive(path, zip)) {
		fprintf(stderr, "harness: miniz could not write %s\n", path);
		exit(2);
	}
	mz_zip_writer_end(zip);
}

static void fixture_setup(void)
{
	char p[512];

	snprintf(root, sizeof root, "/tmp/quakefs-harness-%d", (int)getpid());
	snprintf(base, sizeof base, "%s/base", root);
	snprintf(user, sizeof user, "%s/user", root);

	mkdir(root, 0755);
	mkdir(base, 0755);
	mkdir(user, 0755);
	path_join(p, sizeof p, base, "data1");
	mkdir(p, 0755);
	path_join(p, sizeof p, base, "data1/pak0.pak");
	write_pak(p);
	path_join(p, sizeof p, base, "data1/gfx.zip");
	write_zip(p);
}

/*----------------------------------------------------------------------------
 * cases
 *--------------------------------------------------------------------------*/

static void parms_init(void)
{
	int n = 0;

	memset(&h_parms, 0, sizeof h_parms);
	h_parms.basedir = base;
	h_parms.userdir = user;
	h_argv[n++] = (char *)"hexenwail";
	h_argv[n++] = (char *)"-basedir";
	h_argv[n++] = base;
	h_argv[n++] = (char *)"-userdir";
	h_argv[n++] = user;
	h_parms.argc = n;
	h_parms.argv = h_argv;
	host_parms = &h_parms;
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
	parms_init();
	call("FS_Init");
	FS_Init();
	rec_getters();
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
		rec_int((int)FS_fread(buf, 1, sizeof buf - 1, f));
		rec_str(buf);
		rec_long(FS_ftell(f));
		rec_int(FS_fseek(f, 0, SEEK_SET));
		rec_long(FS_ftell(f));
		rec_int(FS_feof(f));
		rec_int(FS_ferror(f));
		FS_fclose(f);
	}
	rec_long(fs_filesize);

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
		rec_int((int)FS_fread(buf, 1, sizeof buf - 1, f));
		rec_str(buf);
		FS_fclose(f);
	} else {
		rec_int(-1);
	}

	call("open a deflated zip member");
	len = FS_OpenFile_Silent("deflated.txt", &f, &path_id);
	rec_long(len);
	if (f) {
		memset(buf, 0, sizeof buf);
		rec_int((int)FS_fread(buf, 1, sizeof buf - 1, f));
		rec_str(buf);
		FS_fclose(f);
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

	call("build the map list");
	rec_int(FS_BuildMapList("maps"));
	rec_int(FS_BuildMapList(""));
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

	call("FS_IsGamedir");
	rec_int(FS_IsGamedir(base, "data1"));
	rec_int(FS_IsGamedir(base, "nosuchdir"));

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
	parms_init();
	call("FS_Gamedir to a directory that is not there");
	FS_Gamedir("nosuchdir");
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
		printf("case %-24s SIGNAL %d\n", name,
			WIFSIGNALED(status) ? WTERMSIG(status) : -1);
		rec_int(WIFSIGNALED(status) ? WTERMSIG(status) : -1);
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
	return 0;
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "/tmp/quakefs-harness.trace";
	FILE *f;

	fixture_setup();

	printf("quakefs differential harness, implementation: %s\n",
#ifdef IMPL_C_ARM
		"C original (renamed)"
#else
		"Rust"
#endif
	);

	run_case("init", scenario_init);
	run_case("pak", scenario_pak);
	run_case("zip", scenario_zip);
	run_case("lists", scenario_lists);
	run_case("write", scenario_write);
	run_case("fatal-missing", scenario_fatal_missing);
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

/*============================================================================
 * WIP -- THIS FILE DOES NOT COMPILE YET.  It is committed so a later run
 * starts from the design rather than from a blank page.  `cc -c -O1 -DGLQUAKE
 * -Iengine/hexen2 -Iengine/h2shared -Icommon` reports 69 errors, in these
 * classes:
 *
 *  1. The IMPL prefix does not work yet.  `#define FS_Init IMPL##FS_Init`
 *     pastes the *name* IMPL instead of its expansion, so the no-`-DIMPL`
 *     (Rust) arm ends up with IMPLfs_filesize and friends undeclared.  Fix is
 *     the two-level paste: `#define CAT_(a,b) a##b` / `#define CAT(a,b) CAT_(a,b)`
 *     and `#define FS_Init CAT(IMPL, FS_Init)`.
 *  2. Missing includes: <fcntl.h> for open()/O_RDWR/O_CREAT/O_TRUNC,
 *     "miniz.h" for MZ_NO_COMPRESSION/MZ_DEFAULT_COMPRESSION and the writer.
 *  3. mz_zip_writer_create()/mz_zip_writer_write_archive() are invented; the
 *     vendored miniz has no such names.  Either use its real writer API
 *     (grep miniz.h for mz_zip_writer_*) or build the STORED member by hand.
 *  4. Sys_FindFirstFile/Sys_FindNextFile/Sys_FindClose are now the real
 *     fsfind_t signatures but the body still returns before filling names on
 *     the first call -- rework against sys.h:54-56.
 *  5. Several int-to-pointer diagnostics are downstream of 1.
 *
 * After it compiles: the run script (two binaries, `-DIMPL=c_` and bare, same
 * substrate, traces compared byte for byte), then scripts/check-rust-quakefs.sh
 * with per-arm floors and the structural table comparison, then the single
 * wiring commit.  See the file header for the design and what it deliberately
 * does not cover.
 *============================================================================*/
