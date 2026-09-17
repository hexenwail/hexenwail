/*
 * Unit tests for the HexenWorld signon download parser
 * (engine/hexen2/cl_hw_download.inc).
 *
 * Everything under test consumes data a remote HexenWorld server chose: the
 * file name, the block length, the percent byte.  scripts/hw-smoke.sh proves
 * the cooperative path works against a real hwsv, and a cooperative server is
 * exactly what never produces a device name, a negative length, an unsolicited
 * block or a percent byte that overflowed.  Those are the paths here.
 *
 * The .inc is textually included, so the file-static functions are reachable
 * without publishing them.  Every engine symbol it references is stubbed
 * below; the FS/Sys stubs are backed by a real temp directory so the rename,
 * unlink and already-exists paths exercise the real syscalls.
 *
 * Build and run:
 *     cc -Wall -Wextra -o /tmp/hwdltest scripts/hw_download_test.c && /tmp/hwdltest
 * Exit status 0 on pass, 1 on any failed assertion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* engine types and constants the .inc expects                         */
/* ------------------------------------------------------------------ */

typedef int qboolean;
#define true	1
#define false	0

#define MAX_QPATH	64
#define MAX_OSPATH	256
#define MAX_MODELS	256
#define MAX_SOUNDS	256

#define FS_USERDIR	1
#define FS_ENT_NONE	0
#define FS_ENT_FILE	1
#define FS_ENT_DIRECTORY 2

typedef unsigned char byte;

typedef struct
{
	byte	*data;
	int	cursize;
} sizebuf_t;

/* ------------------------------------------------------------------ */
/* recorded side effects                                               */
/* ------------------------------------------------------------------ */

static char	rec_console[16384];
static char	rec_stringcmd[16][128];
static int	rec_stringcmd_count;
static int	rec_disconnects;
static int	rec_plaque_ends;
static int	rec_loadmodels_calls;
static int	rec_loadsounds_calls;
static qboolean	stub_loadmodels_result = true;

static char	testdir[MAX_OSPATH];
static int	stub_makepath_fail;	/* force FS_MakePath_BUF to report overflow */
static int	stub_fs_fileexists;	/* what FS_FileExists claims */

static void rec_reset (void)
{
	rec_console[0] = 0;
	rec_stringcmd_count = 0;
	rec_disconnects = 0;
	rec_plaque_ends = 0;
	rec_loadmodels_calls = 0;
	rec_loadsounds_calls = 0;
	stub_loadmodels_result = true;
	stub_makepath_fail = 0;
	stub_fs_fileexists = 0;
}

static qboolean console_said (const char *needle)
{
	return strstr (rec_console, needle) != NULL;
}

static qboolean sent_cmd (const char *needle)
{
	int i;

	for (i = 0; i < rec_stringcmd_count; i++)
	{
		if (strstr (rec_stringcmd[i], needle))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* engine stubs                                                        */
/* ------------------------------------------------------------------ */

static size_t q_strlcpy (char *dst, const char *src, size_t siz)
{
	size_t len = strlen (src);

	if (siz)
	{
		size_t n = (len >= siz) ? siz - 1 : len;
		memcpy (dst, src, n);
		dst[n] = 0;
	}
	return len;
}

static int q_snprintf (char *str, size_t size, const char *fmt, ...)
{
	va_list	ap;
	int	n;

	va_start (ap, fmt);
	n = vsnprintf (str, size, fmt, ap);
	va_end (ap);
	return n;
}

static int q_strncasecmp (const char *s1, const char *s2, size_t n)
{
	return strncasecmp (s1, s2, n);
}

static char *va (const char *fmt, ...)
{
	static char	buf[4][1024];
	static int	idx;
	va_list		ap;

	idx = (idx + 1) & 3;
	va_start (ap, fmt);
	vsnprintf (buf[idx], sizeof(buf[idx]), fmt, ap);
	va_end (ap);
	return buf[idx];
}

static void Con_Printf (const char *fmt, ...)
{
	char	line[1024];
	va_list	ap;

	va_start (ap, fmt);
	vsnprintf (line, sizeof(line), fmt, ap);
	va_end (ap);
	if (strlen (rec_console) + strlen (line) < sizeof(rec_console))
		strcat (rec_console, line);
}

#define Con_DPrintf Con_Printf

static void SCR_EndLoadingPlaque (void) { rec_plaque_ends++; }

static void HWCL_StringCmd (const char *cmd)
{
	if (rec_stringcmd_count < (int)(sizeof(rec_stringcmd) / sizeof(rec_stringcmd[0])))
		q_strlcpy (rec_stringcmd[rec_stringcmd_count++], cmd, sizeof(rec_stringcmd[0]));
}

static void HWCL_Disconnect (void) { rec_disconnects++; }

static qboolean HWCL_LoadModels (void)
{
	rec_loadmodels_calls++;
	return stub_loadmodels_result;
}

static void HWCL_LoadSounds (void) { rec_loadsounds_calls++; }

static char	fs_gamedir_nopath[MAX_QPATH] = "hw";
static const char *FS_GetBasedir (void) { return "/basedir"; }
static const char *FS_GetUserbase (void) { return "/userbase"; }

static char *FS_MakePath_BUF (int base, int *error, char *buf, size_t siz, const char *path)
{
	int	n;

	(void) base;
	if (stub_makepath_fail)
	{
		if (error) *error = 1;
		return buf;
	}
	n = snprintf (buf, siz, "%s/%s", testdir, path);
	if (error)
		*error = (n < 0 || (size_t)n >= siz);
	return buf;
}

static qboolean FS_FileExists (const char *filename, unsigned int *path_id)
{
	(void) filename; (void) path_id;
	return stub_fs_fileexists ? true : false;
}

/* Real mkdir -p, so a nested "maps/x.bsp" actually gets its directory. */
static int FS_CreatePath (char *path)
{
	char	work[MAX_OSPATH];
	char	*p;

	q_strlcpy (work, path, sizeof(work));
	for (p = work + 1; *p; p++)
	{
		if (*p != '/')
			continue;
		*p = 0;
		if (mkdir (work, 0777) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return 0;
}

static int Sys_unlink (const char *path) { return unlink (path); }
static int Sys_rename (const char *oldp, const char *newp) { return rename (oldp, newp); }

static int Sys_FileType (const char *path)
{
	struct stat st;

	if (stat (path, &st) != 0)
		return FS_ENT_NONE;
	return S_ISDIR(st.st_mode) ? FS_ENT_DIRECTORY : FS_ENT_FILE;
}

/* ------------------------------------------------------------------ */
/* message stubs                                                       */
/* ------------------------------------------------------------------ */

static sizebuf_t	hw_net_message;
static int		msg_readcount;
static qboolean		msg_badread;
static byte		msg_buffer[70000];

static int MSG_ReadShort (void)
{
	int c;

	if (msg_readcount + 2 > hw_net_message.cursize)
	{
		msg_badread = true;
		return -1;
	}
	c = (short)(hw_net_message.data[msg_readcount]
			+ (hw_net_message.data[msg_readcount + 1] << 8));
	msg_readcount += 2;
	return c;
}

static int MSG_ReadByte (void)
{
	int c;

	if (msg_readcount + 1 > hw_net_message.cursize)
	{
		msg_badread = true;
		return -1;
	}
	c = hw_net_message.data[msg_readcount];
	msg_readcount++;
	return c;
}

/* ------------------------------------------------------------------ */
/* precache tables the walk reads                                      */
/* ------------------------------------------------------------------ */

static char	hwcl_model_names[MAX_MODELS][MAX_QPATH];
static char	hwcl_sound_names[MAX_SOUNDS][MAX_QPATH];
static int	hwcl_model_count;
static int	hwcl_sound_count;
static int	hwcl_servercount = 7;

#include "../engine/hexen2/cl_hw_download.inc"

/* ------------------------------------------------------------------ */
/* harness                                                             */
/* ------------------------------------------------------------------ */

static int failures;
static int checks;

#define CHECK(cond, ...) do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			printf ("  FAIL %s:%d: ", __func__, __LINE__); \
			printf (__VA_ARGS__); \
			printf ("\n"); \
		} \
	} while (0)

/* Put the client in "waiting for blocks of NAME" state without going through
 * the precache walk, which would need a whole server conversation. */
static void begin_download (const char *name)
{
	HWCL_CancelDownload ();
	hwcl_download_type = hwcl_dl_model;
	hwcl_download_number = 1;
	q_strlcpy (hwcl_download_name, name, sizeof(hwcl_download_name));
	q_snprintf (hwcl_download_tempname, sizeof(hwcl_download_tempname), "%s.tmp", name);
	hwcl_download_bytes = 0;
	/* One entry, already "present", so the walk that follows a finished or
	 * abandoned file terminates instead of asking for more. */
	memset (hwcl_model_names, 0, sizeof(hwcl_model_names));
	hwcl_model_count = 1;
}

/* Feed one svc_download body: size, percent, then payload bytes. */
static void feed_block (int size, int percent, const byte *payload, int payload_len)
{
	hw_net_message.data = msg_buffer;
	msg_buffer[0] = (byte)(size & 0xff);
	msg_buffer[1] = (byte)((size >> 8) & 0xff);
	msg_buffer[2] = (byte)percent;
	if (payload_len > 0)
		memcpy (msg_buffer + 3, payload, payload_len);
	hw_net_message.cursize = 3 + payload_len;
	msg_readcount = 0;
	msg_badread = false;
	HWCL_ParseDownload ();
}

static char *tmppath (const char *name)
{
	static char buf[MAX_OSPATH * 2];

	snprintf (buf, sizeof(buf), "%s/%s", testdir, name);
	return buf;
}

static qboolean exists (const char *name)
{
	return Sys_FileType (tmppath (name)) != FS_ENT_NONE;
}

static long filesize (const char *name)
{
	struct stat st;

	if (stat (tmppath (name), &st) != 0)
		return -1;
	return (long) st.st_size;
}

/* ================================================================== */
/* 1. name safety                                                      */
/* ================================================================== */

/* The length boundary, spelled out so both sides of it stay in step. */
#define NAME_LONGEST_OK	"maps/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bsp"
#define NAME_TOO_LONG	"maps/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bsp"

static void test_name_safety (void)
{
	static const char *const reject[] = {
		/* empty and over-long */
		"",
		NAME_TOO_LONG,
		/* traversal, in every position */
		"../etc/passwd",
		"maps/../../etc/passwd",
		"maps/..",
		"a/..b/c",		/* ".." as a substring is refused wholesale */
		/* absolute and leading-dot */
		"/etc/passwd",
		".hidden/x.bsp",
		"./maps/x.bsp",
		/* backslash anywhere: a separator on Windows */
		"maps\\x.bsp",
		"\\\\server\\share\\x.bsp",
		"maps/sub\\x.bsp",
		/* drive letters and the other Windows-illegal characters */
		"c:/windows/x.bsp",
		"maps/a<b.bsp", "maps/a>b.bsp", "maps/a\"b.bsp",
		"maps/a|b.bsp", "maps/a?b.bsp", "maps/a*b.bsp",
		/* control characters, including the classic NUL-adjacent ones */
		"maps/a\tb.bsp", "maps/a\nb.bsp", "maps/a\rb.bsp",
		"maps/a\x01" "b.bsp", "maps/a\x1f" "b.bsp", "maps/a\x7f" "b.bsp",
		/* trailing dot or space: Windows strips them, so the checked name
		 * is not the opened name */
		"maps/x.bsp.", "maps/x.bsp ", "maps./x.bsp", "maps /x.bsp",
		/* empty component */
		"maps//x.bsp", "/maps/x.bsp",
		/* no subdirectory at all */
		"pak9.pak", "config.cfg",
		/* Windows reserved devices, any case, any extension, any depth,
		 * and with the spaces Windows ignores before the extension */
		"maps/con", "maps/CON", "maps/Con.bsp", "maps/con.bsp.bsp",
		"maps/prn.wav", "maps/PRN", "maps/aux.mdl", "maps/AUX.bsp",
		"maps/nul", "maps/NUL.bsp", "maps/nUl.lmp",
		"maps/com1", "maps/COM1.bsp", "maps/com9.wav", "maps/lpt1",
		"maps/LPT9.bsp", "maps/Lpt3.mdl",
		"maps/con .bsp", "maps/COM1  .bsp",
		"con/x.bsp", "nul/sub/x.bsp",	/* device as a directory component */
		NULL
	};
	static const char *const accept[] = {
		"maps/hwdm1.bsp",
		"maps/HWDM1.BSP",
		"sound/misc/talk.wav",
		"models/paladin.mdl",
		"progs/sub/dir/thing.mdl",
		"maps/come1.bsp",	/* "come" is not "com" + digit */
		"maps/console.bsp",	/* "console" is not "con" */
		"maps/com0.bsp",	/* COM0 is not a device */
		"maps/com10.bsp",	/* five characters, not COMn */
		"maps/nula.bsp",
		"maps/a.b.c.bsp",
		"maps/x-y_z.bsp",
		NAME_LONGEST_OK,
		NULL
	};
	int i;

	for (i = 0; reject[i]; i++)
		CHECK (!HWCL_DownloadNameIsSafe (reject[i]),
			"accepted unsafe name \"%s\"", reject[i]);
	for (i = 0; accept[i]; i++)
		CHECK (HWCL_DownloadNameIsSafe (accept[i]),
			"rejected legitimate name \"%s\"", accept[i]);

	/* MAX_QPATH-1 is the last accepted length; one more is refused.  Asserted
	 * on the fixtures themselves so a change to MAX_QPATH cannot quietly turn
	 * the boundary pair into two names on the same side of it. */
	CHECK (strlen (NAME_LONGEST_OK) == MAX_QPATH - 1,
		"length fixture is %d, not %d", (int) strlen (NAME_LONGEST_OK), MAX_QPATH - 1);
	CHECK (strlen (NAME_TOO_LONG) == MAX_QPATH,
		"over-long fixture is %d, not %d", (int) strlen (NAME_TOO_LONG), MAX_QPATH);

	/* An unsafe name must never reach the wire, and must not stall the walk:
	 * CheckOrDownloadFile reports "handled" so the caller moves on. */
	rec_reset ();
	HWCL_CancelDownload ();
	CHECK (HWCL_CheckOrDownloadFile ("maps/../../etc/passwd"),
		"unsafe name did not resolve as handled");
	CHECK (!sent_cmd ("download"), "requested an unsafe name from the server");
	CHECK (console_said ("Refusing to download unsafe path"),
		"unsafe name was refused silently");
	CHECK (hwcl_download_name[0] == 0, "unsafe name became the active download");
}

/* ================================================================== */
/* 2. size field: negative, zero, oversized, packet overrun            */
/* ================================================================== */

static void test_size_negative (void)
{
	rec_reset ();
	begin_download ("maps/gone.bsp");
	feed_block (-1, 0, NULL, 0);
	CHECK (console_said ("Server cannot send maps/gone.bsp"),
		"size -1 was not reported as a refusal");
	CHECK (hwcl_download_name[0] == 0, "size -1 left the download active");
	CHECK (!exists ("maps/gone.bsp.tmp"), "size -1 left a temp file behind");
	CHECK (rec_disconnects == 0, "size -1 dropped the connection");
	/* The walk continues: the one-entry model list finishes and asks to spawn. */
	CHECK (rec_loadmodels_calls == 1, "size -1 did not resume the precache walk");

	/* Any negative value, not just -1 (MSG_ReadShort sign-extends). */
	rec_reset ();
	begin_download ("maps/gone.bsp");
	feed_block (-32768, 0, NULL, 0);
	CHECK (rec_disconnects == 0, "size -32768 dropped the connection");
	CHECK (hwcl_download_name[0] == 0, "size -32768 left the download active");
}

static void test_size_zero (void)
{
	rec_reset ();
	begin_download ("maps/empty.bsp");
	feed_block (0, 0, NULL, 0);
	CHECK (console_said ("Server sent an empty maps/empty.bsp"),
		"an empty block was not reported");
	CHECK (!exists ("maps/empty.bsp"), "an empty block created the target file");
	CHECK (!exists ("maps/empty.bsp.tmp"), "an empty block left a temp file");
	CHECK (hwcl_download_name[0] == 0, "an empty block left the download active");
	CHECK (rec_disconnects == 0, "an empty block dropped the connection");

	/* percent 100 must not make an empty block count as a finished file. */
	rec_reset ();
	begin_download ("maps/empty.bsp");
	feed_block (0, 100, NULL, 0);
	CHECK (!exists ("maps/empty.bsp"), "an empty 100%% block created the target file");
}

static void test_packet_overrun (void)
{
	byte payload[64];

	memset (payload, 'A', sizeof(payload));

	/* Claims 4096 bytes, ships 64: the read would run off the packet. */
	rec_reset ();
	begin_download ("maps/lie.bsp");
	feed_block (4096, 50, payload, sizeof(payload));
	CHECK (console_said ("overruns its packet"), "packet overrun was not reported");
	CHECK (rec_disconnects == 1, "packet overrun did not drop the connection");
	CHECK (!exists ("maps/lie.bsp.tmp"), "packet overrun wrote a temp file");

	/* One byte over is still over. */
	rec_reset ();
	begin_download ("maps/lie.bsp");
	feed_block (sizeof(payload) + 1, 50, payload, sizeof(payload));
	CHECK (rec_disconnects == 1, "off-by-one overrun was accepted");

	/* Exactly the remaining bytes is legal and must be written. */
	rec_reset ();
	begin_download ("maps/exact.bsp");
	feed_block (sizeof(payload), 100, payload, sizeof(payload));
	CHECK (rec_disconnects == 0, "an exactly-sized block was rejected");
	CHECK (filesize ("maps/exact.bsp") == (long) sizeof(payload),
		"an exactly-sized block did not land whole (got %ld)",
		filesize ("maps/exact.bsp"));
	unlink (tmppath ("maps/exact.bsp"));
}

/* ================================================================== */
/* 3. unsolicited blocks                                               */
/* ================================================================== */

static void test_unsolicited (void)
{
	byte payload[32];

	memset (payload, 'U', sizeof(payload));

	/* Nothing in progress at all. */
	rec_reset ();
	HWCL_CancelDownload ();
	memset (hwcl_model_names, 0, sizeof(hwcl_model_names));
	hwcl_model_count = 1;
	feed_block (sizeof(payload), 100, payload, sizeof(payload));
	CHECK (rec_disconnects == 0, "an unsolicited block dropped the connection");
	CHECK (msg_readcount == 3 + (int) sizeof(payload),
		"an unsolicited block was not consumed (readcount %d)", msg_readcount);
	CHECK (rec_loadmodels_calls == 0,
		"an unsolicited block drove the precache walk");

	/* A type is set but no name: the half-state left by an abandon. */
	rec_reset ();
	HWCL_CancelDownload ();
	hwcl_download_type = hwcl_dl_model;
	hwcl_download_name[0] = 0;
	feed_block (sizeof(payload), 100, payload, sizeof(payload));
	CHECK (msg_readcount == 3 + (int) sizeof(payload),
		"a nameless block was not consumed");
	CHECK (rec_loadmodels_calls == 0, "a nameless block drove the precache walk");

	/* size -1 with nothing in progress must also be a silent no-op. */
	rec_reset ();
	HWCL_CancelDownload ();
	feed_block (-1, 0, NULL, 0);
	CHECK (rec_disconnects == 0, "an unsolicited -1 dropped the connection");
	CHECK (rec_console[0] == 0, "an unsolicited -1 printed \"%s\"", rec_console);

	/* A truncated header is a bad read, not a parse. */
	rec_reset ();
	begin_download ("maps/x.bsp");
	hw_net_message.data = msg_buffer;
	hw_net_message.cursize = 1;	/* one byte: not even the short */
	msg_readcount = 0;
	msg_badread = false;
	HWCL_ParseDownload ();
	CHECK (msg_badread, "a truncated header did not set msg_badread");
	CHECK (!exists ("maps/x.bsp.tmp"), "a truncated header opened a file");
}

/* ================================================================== */
/* 4. the 64 MB cap                                                    */
/* ================================================================== */

static void test_size_cap (void)
{
	byte payload[1024];

	memset (payload, 'C', sizeof(payload));

	CHECK (HWCL_DOWNLOAD_MAX_BYTES == 64L * 1024L * 1024L,
		"the cap moved: %ld", HWCL_DOWNLOAD_MAX_BYTES);

	/* Exactly at the cap is still accepted. */
	rec_reset ();
	begin_download ("maps/cap.bsp");
	feed_block (sizeof(payload), 50, payload, sizeof(payload));
	hwcl_download_bytes = HWCL_DOWNLOAD_MAX_BYTES - (long) sizeof(payload);
	feed_block (sizeof(payload), 50, payload, sizeof(payload));
	CHECK (!console_said ("exceeds the"), "a block landing exactly on the cap was refused");
	CHECK (hwcl_download_bytes == HWCL_DOWNLOAD_MAX_BYTES,
		"byte counter is %ld, expected %ld", hwcl_download_bytes,
		HWCL_DOWNLOAD_MAX_BYTES);

	/* One byte past it is not. */
	feed_block (1, 50, payload, 1);
	CHECK (console_said ("exceeds the 64 MB download limit"),
		"passing the cap was not reported");
	CHECK (hwcl_download_name[0] == 0, "passing the cap left the download active");
	CHECK (!exists ("maps/cap.bsp"), "passing the cap renamed a partial file into place");
	CHECK (!exists ("maps/cap.bsp.tmp"), "passing the cap left a temp file");
	CHECK (rec_loadmodels_calls == 1, "passing the cap did not resume the walk");
	CHECK (rec_disconnects == 0, "passing the cap dropped the connection");

	/* The running total is a long on purpose.  Counted in an int, a server
	 * that kept feeding blocks would wrap the sum negative right here and
	 * sail straight past the cap. */
	CHECK (sizeof(hwcl_download_bytes) >= sizeof(long),
		"the byte counter is %d bytes, too narrow to hold the sum safely",
		(int) sizeof(hwcl_download_bytes));
	rec_reset ();
	begin_download ("maps/cap2.bsp");
	feed_block (sizeof(payload), 50, payload, sizeof(payload));
	hwcl_download_bytes = (long) INT_MAX;
	feed_block (sizeof(payload), 50, payload, sizeof(payload));
	CHECK (console_said ("exceeds the"), "a block at INT_MAX slipped under the cap");
	CHECK (!exists ("maps/cap2.bsp"), "a block at INT_MAX renamed a partial file");
}

/* ================================================================== */
/* 5. the percent guard (hwsv's int overflow past ~21.4 MB)            */
/* ================================================================== */

static void test_percent_guard (void)
{
	byte payload[16];

	memset (payload, 'P', sizeof(payload));

	/* hwsv computes percent as int downloadcount*100/size, which overflows
	 * once downloadcount passes INT_MAX/100 = 21474836 bytes and then sends
	 * arbitrary values mid-file.  Only exactly 100 may finish the file. */
	CHECK (INT_MAX / 100 == 21474836, "the overflow threshold moved: %d", INT_MAX / 100);

	{
		/* Post-overflow values a real hwsv emits, plus the neighbours of 100. */
		static const int midfile[] = { 0, 1, 50, 99, 101, 128, 185, 200, 255 };
		size_t i;

		for (i = 0; i < sizeof(midfile) / sizeof(midfile[0]); i++)
		{
			rec_reset ();
			begin_download ("maps/pct.bsp");
			feed_block (sizeof(payload), midfile[i], payload, sizeof(payload));
			CHECK (!exists ("maps/pct.bsp"),
				"percent %d renamed a truncated file into place", midfile[i]);
			CHECK (exists ("maps/pct.bsp.tmp"),
				"percent %d did not keep the partial file open", midfile[i]);
			CHECK (sent_cmd ("nextdl"),
				"percent %d did not ask for the next block", midfile[i]);
			CHECK (hwcl_download_name[0] != 0,
				"percent %d cleared the active download", midfile[i]);
			HWCL_CancelDownload ();
			CHECK (!exists ("maps/pct.bsp.tmp"),
				"percent %d: cancel did not remove the partial", midfile[i]);
		}
	}

	/* Exactly 100 finishes it. */
	rec_reset ();
	begin_download ("maps/pct.bsp");
	feed_block (sizeof(payload), 100, payload, sizeof(payload));
	CHECK (exists ("maps/pct.bsp"), "percent 100 did not complete the file");
	CHECK (!exists ("maps/pct.bsp.tmp"), "percent 100 left the temp file");
	CHECK (filesize ("maps/pct.bsp") == (long) sizeof(payload),
		"completed file is %ld bytes, expected %d",
		filesize ("maps/pct.bsp"), (int) sizeof(payload));
	CHECK (!sent_cmd ("nextdl"), "percent 100 asked for another block");
	unlink (tmppath ("maps/pct.bsp"));

	/* A multi-block file: only the block carrying 100 completes it, and the
	 * bytes accumulate in order. */
	rec_reset ();
	begin_download ("maps/multi.bsp");
	feed_block (sizeof(payload), 33, payload, sizeof(payload));
	feed_block (sizeof(payload), 66, payload, sizeof(payload));
	CHECK (!exists ("maps/multi.bsp"), "an intermediate block completed the file");
	feed_block (sizeof(payload), 100, payload, sizeof(payload));
	CHECK (filesize ("maps/multi.bsp") == 3 * (long) sizeof(payload),
		"multi-block file is %ld bytes, expected %d",
		filesize ("maps/multi.bsp"), 3 * (int) sizeof(payload));
	unlink (tmppath ("maps/multi.bsp"));
}

/* ================================================================== */
/* 6. the rename guard                                                 */
/* ================================================================== */

static void test_rename_guard (void)
{
	byte payload[8];
	FILE *f;
	char buf[64];

	memset (payload, 'N', sizeof(payload));

	/* A file that appeared while the download ran must never be clobbered. */
	rec_reset ();
	begin_download ("maps/pre.bsp");
	mkdir (tmppath ("maps"), 0777);
	f = fopen (tmppath ("maps/pre.bsp"), "wb");
	CHECK (f != NULL, "could not create the pre-existing fixture");
	if (f) { fputs ("ORIGINAL", f); fclose (f); }

	feed_block (sizeof(payload), 100, payload, sizeof(payload));
	CHECK (console_said ("maps/pre.bsp already exists"),
		"the already-exists case was not reported");
	CHECK (!exists ("maps/pre.bsp.tmp"), "the discarded download was not unlinked");
	f = fopen (tmppath ("maps/pre.bsp"), "rb");
	CHECK (f != NULL, "the pre-existing file disappeared");
	if (f)
	{
		size_t n = fread (buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		fclose (f);
		CHECK (!strcmp (buf, "ORIGINAL"), "the pre-existing file was overwritten: \"%s\"", buf);
	}
	CHECK (hwcl_download_name[0] == 0, "the discard left the download active");
	CHECK (rec_loadmodels_calls == 1, "the discard did not resume the walk");
	unlink (tmppath ("maps/pre.bsp"));

	/* A directory sitting on the target name is also "exists": discard, do
	 * not try to rename a file onto a directory. */
	rec_reset ();
	begin_download ("maps/dir.bsp");
	mkdir (tmppath ("maps"), 0777);
	mkdir (tmppath ("maps/dir.bsp"), 0777);
	feed_block (sizeof(payload), 100, payload, sizeof(payload));
	CHECK (console_said ("already exists"), "a directory on the target name was not caught");
	CHECK (!exists ("maps/dir.bsp.tmp"), "the discarded download was not unlinked");
	rmdir (tmppath ("maps/dir.bsp"));

	/* An over-long path is reported and must not leave the walk wedged. */
	rec_reset ();
	begin_download ("maps/long.bsp");
	feed_block (sizeof(payload), 50, payload, sizeof(payload));
	stub_makepath_fail = 1;
	feed_block (sizeof(payload), 100, payload, sizeof(payload));
	CHECK (console_said ("Download path for maps/long.bsp is too long"),
		"an unrepresentable path was not reported");
	CHECK (hwcl_download_name[0] == 0, "the path failure left the download active");
	CHECK (rec_loadmodels_calls == 1, "the path failure did not resume the walk");
	stub_makepath_fail = 0;
	unlink (tmppath ("maps/long.bsp.tmp"));
}

/* ================================================================== */
/* 7. the missing-world disconnect                                     */
/* ================================================================== */

static void test_missing_world (void)
{
	/* The world (model index 1) still absent after the walk is the one
	 * failure that cannot be shrugged off: there is nothing to draw. */
	rec_reset ();
	HWCL_CancelDownload ();
	memset (hwcl_model_names, 0, sizeof(hwcl_model_names));
	q_strlcpy (hwcl_model_names[1], "maps/nosuch.bsp", MAX_QPATH);
	hwcl_model_count = 2;
	stub_fs_fileexists = 1;		/* pretend it is on disk, so no request */
	stub_loadmodels_result = false;	/* but it will not load */
	HWCL_ModelNextDownload ();
	CHECK (console_said ("could not be found or downloaded"),
		"a missing world was not reported");
	CHECK (console_said ("maps/nosuch.bsp"), "the report did not name the map");
	CHECK (console_said ("Install hw/pak4.pak"), "the report was not actionable");
	CHECK (rec_disconnects == 1, "a missing world did not disconnect (got %d)",
		rec_disconnects);
	CHECK (!sent_cmd ("prespawn"), "a missing world still asked to spawn");

	/* A world that does load proceeds to prespawn instead. */
	rec_reset ();
	HWCL_CancelDownload ();
	stub_fs_fileexists = 1;
	stub_loadmodels_result = true;
	hwcl_model_count = 2;
	HWCL_ModelNextDownload ();
	CHECK (rec_disconnects == 0, "a loadable world disconnected");
	CHECK (sent_cmd ("prespawn 7 0"), "a loadable world did not ask to spawn");
}

/* ================================================================== */
/* 8. the precache walk requests what is missing                       */
/* ================================================================== */

static void test_walk_requests (void)
{
	/* Inline brush models ("*3") and empty slots are never fetched. */
	rec_reset ();
	HWCL_CancelDownload ();
	memset (hwcl_model_names, 0, sizeof(hwcl_model_names));
	q_strlcpy (hwcl_model_names[1], "maps/world.bsp", MAX_QPATH);
	q_strlcpy (hwcl_model_names[2], "*3", MAX_QPATH);
	q_strlcpy (hwcl_model_names[3], "models/thing.mdl", MAX_QPATH);
	hwcl_model_count = 4;
	stub_fs_fileexists = 0;		/* nothing on disk */
	HWCL_ModelNextDownload ();
	CHECK (sent_cmd ("download maps/world.bsp"),
		"the walk did not request the first missing model");
	CHECK (rec_plaque_ends >= 1, "the walk did not drop the loading plaque");

	/* Inline brush models have no file to fetch: asking hwsv for "*1" gets a
	 * refusal at best, and the walk must step over them rather than stall on
	 * one.  Nothing is on disk here, so the first name the walk asks for is
	 * proof of which entries it considered. */
	rec_reset ();
	HWCL_CancelDownload ();
	memset (hwcl_model_names, 0, sizeof(hwcl_model_names));
	q_strlcpy (hwcl_model_names[1], "*1", MAX_QPATH);
	q_strlcpy (hwcl_model_names[2], "*3", MAX_QPATH);
	/* index 3 deliberately left empty: empty slots are skipped too */
	q_strlcpy (hwcl_model_names[4], "models/thing.mdl", MAX_QPATH);
	hwcl_model_count = 5;
	stub_fs_fileexists = 0;
	HWCL_ModelNextDownload ();
	CHECK (sent_cmd ("download models/thing.mdl"),
		"the walk did not step over the inline brush models");
	CHECK (!sent_cmd ("download *"), "the walk requested an inline brush model");
	/* Not merely unrequested: never even offered to the name check.  "*1"
	 * would be rejected there anyway, but only after printing a scary
	 * "Refusing to download unsafe path" for every brush model in the map. */
	CHECK (!console_said ("Refusing to download unsafe path"),
		"the walk ran inline brush models through the name check");
	CHECK (rec_stringcmd_count == 1, "the walk sent %d commands, expected 1",
		rec_stringcmd_count);

	/* The walk resumes after the file it just handled, not from the top. */
	rec_reset ();
	HWCL_CancelDownload ();
	memset (hwcl_model_names, 0, sizeof(hwcl_model_names));
	q_strlcpy (hwcl_model_names[1], "maps/world.bsp", MAX_QPATH);
	q_strlcpy (hwcl_model_names[2], "models/thing.mdl", MAX_QPATH);
	hwcl_model_count = 3;
	stub_fs_fileexists = 0;
	HWCL_ModelNextDownload ();
	CHECK (sent_cmd ("download maps/world.bsp"), "the walk did not start at index 1");
	rec_reset ();
	hwcl_download_name[0] = 0;
	hwcl_download_tempname[0] = 0;
	HWCL_RequestNextDownload ();
	CHECK (sent_cmd ("download models/thing.mdl"),
		"the walk did not resume past the file it just handled");
	CHECK (!sent_cmd ("download maps/world.bsp"), "the walk restarted from the top");

	/* Sounds get the "sound/" prefix and hand off to the model list. */
	rec_reset ();
	HWCL_CancelDownload ();
	memset (hwcl_sound_names, 0, sizeof(hwcl_sound_names));
	q_strlcpy (hwcl_sound_names[1], "misc/talk.wav", MAX_QPATH);
	hwcl_sound_count = 2;
	stub_fs_fileexists = 0;
	HWCL_SoundNextDownload ();
	CHECK (sent_cmd ("download sound/misc/talk.wav"),
		"the sound walk did not prefix the path");

	rec_reset ();
	HWCL_CancelDownload ();
	stub_fs_fileexists = 1;		/* all present */
	HWCL_SoundNextDownload ();
	CHECK (rec_loadsounds_calls == 1, "a complete sound list did not load");
	CHECK (sent_cmd ("modellist 7 0"), "a complete sound list did not ask for models");
}

/* ================================================================== */

int main (void)
{
	char	tmpl[] = "/tmp/hwdltest.XXXXXX";
	char	cmd[MAX_OSPATH + 16];

	if (!mkdtemp (tmpl))
	{
		perror ("mkdtemp");
		return 1;
	}
	q_strlcpy (testdir, tmpl, sizeof(testdir));

	test_name_safety ();
	test_size_negative ();
	test_size_zero ();
	test_packet_overrun ();
	test_unsolicited ();
	test_size_cap ();
	test_percent_guard ();
	test_rename_guard ();
	test_missing_world ();
	test_walk_requests ();

	HWCL_CancelDownload ();
	snprintf (cmd, sizeof(cmd), "rm -rf '%s'", testdir);
	if (system (cmd) != 0)
		printf ("warning: could not remove %s\n", testdir);

	printf ("hw_download_test: %d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
