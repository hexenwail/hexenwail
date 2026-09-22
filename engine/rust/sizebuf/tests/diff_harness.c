// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for sizebuf.c and the Rust sizebuf module.  The C
// original is compiled with c_SZ_* names so both implementations can mutate
// independent buffers in one process.

#include "q_stdinc.h"
#include "sizebuf.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Comparisons actually made.  The gate floors this rather than pinning it, so
// that a regression which empties an enumeration cannot exit 0 and still read
// as green.
static unsigned long cases;

#define CHECK(cond, ...) do { \
	++cases; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		return 1; \
	} \
} while (0)

extern void c_SZ_Init(sizebuf_t *buf, byte *data, int length);
extern void c_SZ_Clear(sizebuf_t *buf);
extern void *c_SZ_GetSpace(sizebuf_t *buf, int length);
extern void c_SZ_Write(sizebuf_t *buf, const void *data, int length);
extern void c_SZ_Print(sizebuf_t *buf, const char *data);

extern size_t SizeBufC_sizeof(void);
extern size_t SizeBufC_alignof(void);
extern size_t SizeBufC_offsetof_allowoverflow(void);
extern size_t SizeBufC_offsetof_overflowed(void);
extern size_t SizeBufC_offsetof_data(void);
extern size_t SizeBufC_offsetof_maxsize(void);
extern size_t SizeBufC_offsetof_cursize(void);
extern size_t SizeBufC_offsetof_name(void);

static char printed[512];
static size_t printed_len;

void *Hunk_AllocName(int size, const char *name)
{
	(void)name;
	return calloc(1, (size_t)size);
}

void CON_Printf(unsigned int flags, const char *fmt, ...)
{
	va_list ap;
	int n;

	(void)flags;
	va_start(ap, fmt);
	n = vsnprintf(printed + printed_len, sizeof(printed) - printed_len, fmt, ap);
	va_end(ap);
	if (n > 0)
		printed_len += (size_t)n < sizeof(printed) - printed_len
			? (size_t)n : sizeof(printed) - printed_len - 1;
}

void Sys_Error(const char *fmt, ...)
{
	va_list ap;

	fputs("FATAL ERROR: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	_exit(42);
}

static void reset_printed(void)
{
	printed[0] = '\0';
	printed_len = 0;
}

static void normalize_c_function_name(char *message)
{
	char *function = strstr(message, "c_SZ_GetSpace");

	if (function != NULL)
		memmove(function, function + 2, strlen(function + 2) + 1);
}

static int same_state(const sizebuf_t *a, const sizebuf_t *b)
{
	return a->allowoverflow == b->allowoverflow
		&& a->overflowed == b->overflowed
		&& a->maxsize == b->maxsize
		&& a->cursize == b->cursize
		&& (a->name == NULL) == (b->name == NULL);
}

static int same_bytes(const sizebuf_t *a, const sizebuf_t *b, size_t size)
{
	return memcmp(a->data, b->data, size) == 0;
}

static int abi_checks(void)
{
	CHECK(sizeof(sizebuf_t) == SizeBufC_sizeof(),
		"sizebuf_t size differs: C=%zu Rust=%zu", sizeof(sizebuf_t), SizeBufC_sizeof());
	CHECK(_Alignof(sizebuf_t) == SizeBufC_alignof(),
		"sizebuf_t alignment differs: C=%zu Rust=%zu", _Alignof(sizebuf_t), SizeBufC_alignof());
	CHECK(offsetof(sizebuf_t, allowoverflow) == SizeBufC_offsetof_allowoverflow(), "allowoverflow offset");
	CHECK(offsetof(sizebuf_t, overflowed) == SizeBufC_offsetof_overflowed(), "overflowed offset");
	CHECK(offsetof(sizebuf_t, data) == SizeBufC_offsetof_data(), "data offset");
	CHECK(offsetof(sizebuf_t, maxsize) == SizeBufC_offsetof_maxsize(), "maxsize offset");
	CHECK(offsetof(sizebuf_t, cursize) == SizeBufC_offsetof_cursize(), "cursize offset");
	CHECK(offsetof(sizebuf_t, name) == SizeBufC_offsetof_name(), "name offset");
	return 0;
}

static int init_checks(void)
{
	byte c_data[32], r_data[32];
	sizebuf_t c, r;
	void *c_alloc, *r_alloc;
	int i;

	memset(c_data, 0xa5, sizeof(c_data));
	memset(r_data, 0xa5, sizeof(r_data));
	c_SZ_Init(&c, c_data, (int)sizeof(c_data));
	SZ_Init(&r, r_data, (int)sizeof(r_data));
	CHECK(same_state(&c, &r), "externally supplied init state differs");
	CHECK(c.data == c_data && r.data == r_data, "external data pointer not retained");
	for (i = 0; i < (int)sizeof(c_data); ++i)
		CHECK(c_data[i] == 0xa5 && r_data[i] == 0xa5, "init touched external data");

	c_SZ_Init(&c, NULL, 4);
	c_alloc = c.data;
	SZ_Init(&r, NULL, 4);
	r_alloc = r.data;
	CHECK(c_alloc != NULL && r_alloc != NULL, "allocated init returned NULL");
	CHECK(c.maxsize == 256 && r.maxsize == 256, "short allocation was not rounded to 256");
	CHECK(same_state(&c, &r), "allocated init state differs");
	for (i = 0; i < 256; ++i)
		CHECK(((byte *)c_alloc)[i] == 0 && ((byte *)r_alloc)[i] == 0,
			"allocated buffer is not zeroed at byte %d", i);
	free(c_alloc);
	free(r_alloc);
	return 0;
}

static int write_checks(void)
{
	byte c_data[32], r_data[32], bytes[] = { 1, 2, 3, 4, 5 };
	sizebuf_t c, r;
	void *c_ptr, *r_ptr;

	memset(c_data, 0xcc, sizeof(c_data));
	memset(r_data, 0xcc, sizeof(r_data));
	c_SZ_Init(&c, c_data, (int)sizeof(c_data));
	SZ_Init(&r, r_data, (int)sizeof(r_data));
	c_ptr = c_SZ_GetSpace(&c, 5);
	r_ptr = SZ_GetSpace(&r, 5);
	CHECK((byte *)c_ptr - c.data == (byte *)r_ptr - r.data, "GetSpace offset differs");
	CHECK(c_ptr == c.data && r_ptr == r.data, "first GetSpace did not return buffer start");
	CHECK(same_state(&c, &r) && c.cursize == 5, "GetSpace state differs");

	c_SZ_Write(&c, bytes, (int)sizeof(bytes));
	SZ_Write(&r, bytes, (int)sizeof(bytes));
	CHECK(same_state(&c, &r), "Write state differs");
	CHECK(same_bytes(&c, &r, sizeof(c_data)), "Write bytes differ");

	c_SZ_Clear(&c);
	SZ_Clear(&r);
	c.cursize = r.cursize = 30;
	reset_printed();
	c_ptr = c_SZ_GetSpace(&c, 2);
	CHECK(printed[0] == '\0', "exact-capacity C write unexpectedly printed");
	r_ptr = SZ_GetSpace(&r, 2);
	CHECK(printed[0] == '\0', "exact-capacity Rust write unexpectedly printed");
	CHECK((byte *)c_ptr - c.data == (byte *)r_ptr - r.data
		&& c.cursize == 32 && r.cursize == 32,
		"exact-capacity GetSpace differs");

	c_SZ_Clear(&c);
	SZ_Clear(&r);
	c.allowoverflow = r.allowoverflow = 1;
	c.name = r.name = "testbuf";
	reset_printed();
	c.cursize = r.cursize = 30;
	c_ptr = c_SZ_GetSpace(&c, 4);
	normalize_c_function_name(printed);
	CHECK(strcmp(printed, "SZ_GetSpace: overflow\ntestbuf: currently 30 of 32, requested 4\n") == 0,
		"C overflow diagnostic differs: %s", printed);
	reset_printed();
	r_ptr = SZ_GetSpace(&r, 4);
	CHECK(strcmp(printed, "SZ_GetSpace: overflow\ntestbuf: currently 30 of 32, requested 4\n") == 0,
		"Rust overflow diagnostic differs: %s", printed);
	CHECK((byte *)c_ptr - c.data == (byte *)r_ptr - r.data, "overflow pointer offset differs");
	CHECK(same_state(&c, &r) && c.overflowed && c.cursize == 4,
		"allowed overflow state differs");

	/* A second overflow must remain observable and reset the buffer again. */
	reset_printed();
	c_ptr = c_SZ_GetSpace(&c, 29);
	normalize_c_function_name(printed);
	CHECK(strcmp(printed, "SZ_GetSpace: overflow\ntestbuf: currently 4 of 32, requested 29\n") == 0,
		"repeated C overflow diagnostic differs: %s", printed);
	reset_printed();
	r_ptr = SZ_GetSpace(&r, 29);
	CHECK(strcmp(printed, "SZ_GetSpace: overflow\ntestbuf: currently 4 of 32, requested 29\n") == 0,
		"repeated Rust overflow diagnostic differs: %s", printed);
	CHECK((byte *)c_ptr - c.data == (byte *)r_ptr - r.data
		&& same_state(&c, &r) && c.overflowed && c.cursize == 29,
		"repeated overflow state differs");

	c_SZ_Clear(&c);
	SZ_Clear(&r);
	CHECK(same_state(&c, &r) && c.overflowed == 0 && c.cursize == 0,
		"clear after overflow differs");
	return 0;
}

static int print_checks(void)
{
	byte c_data[32], r_data[32];
	sizebuf_t c, r;

	memset(c_data, 0, sizeof(c_data));
	memset(r_data, 0, sizeof(r_data));
	c_SZ_Init(&c, c_data, (int)sizeof(c_data));
	SZ_Init(&r, r_data, (int)sizeof(r_data));
	c_SZ_Print(&c, "abc");
	SZ_Print(&r, "abc");
	c_SZ_Print(&c, "def");
	SZ_Print(&r, "def");
	CHECK(same_state(&c, &r) && c.cursize == 7, "trailing-NUL state differs");
	CHECK(memcmp(c.data, "abcdef", 6) == 0 && c.data[6] == '\0', "C SZ_Print bytes differ");
	CHECK(memcmp(r.data, "abcdef", 6) == 0 && r.data[6] == '\0', "Rust SZ_Print bytes differ");
	CHECK(same_bytes(&c, &r, sizeof(c_data)), "SZ_Print buffers differ");
	return 0;
}

typedef void (*fatal_call)(sizebuf_t *buf);

static void fatal_without_allowoverflow(sizebuf_t *buf)
{
	SZ_GetSpace(buf, 5);
}

static void fatal_without_allowoverflow_c(sizebuf_t *buf)
{
	c_SZ_GetSpace(buf, 5);
}

static void fatal_too_large(sizebuf_t *buf)
{
	buf->allowoverflow = 1;
	SZ_GetSpace(buf, 5);
}

static void fatal_too_large_c(sizebuf_t *buf)
{
	buf->allowoverflow = 1;
	c_SZ_GetSpace(buf, 5);
}

static int run_fatal(fatal_call call, int c_original, char *message, size_t message_size)
{
	int fds[2], status;
	pid_t pid;
	byte data[4];
	sizebuf_t buf;
	ssize_t n;
	size_t total = 0;

	CHECK(pipe(fds) == 0, "pipe failed: %s", strerror(errno));
	pid = fork();
	CHECK(pid >= 0, "fork failed: %s", strerror(errno));
	if (pid == 0) {
		close(fds[0]);
		if (dup2(fds[1], STDERR_FILENO) < 0)
			_exit(100);
		close(fds[1]);
		if (c_original)
			c_SZ_Init(&buf, data, sizeof(data));
		else
			SZ_Init(&buf, data, sizeof(data));
		call(&buf);
		_exit(101);
	}
	close(fds[1]);
	while (total < message_size - 1) {
		n = read(fds[0], message + total, message_size - 1 - total);
		CHECK(n >= 0, "read failed: %s", strerror(errno));
		if (n == 0)
			break;
		total += (size_t)n;
	}
	message[total] = '\0';
	if (c_original)
		normalize_c_function_name(message);
	close(fds[0]);
	CHECK(waitpid(pid, &status, 0) == pid, "waitpid failed: %s", strerror(errno));
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42,
		"fatal call had unexpected status 0x%x", status);
	return 0;
}

static int fatal_checks(void)
{
	char c_message[512], r_message[512];

	CHECK(run_fatal(fatal_without_allowoverflow_c, 1, c_message, sizeof(c_message)) == 0,
		"C overflow child failed");
	CHECK(run_fatal(fatal_without_allowoverflow, 0, r_message, sizeof(r_message)) == 0,
		"Rust overflow child failed");
	CHECK(strstr(c_message, "overflow without allowoverflow set") != NULL,
		"C overflow took the wrong fatal path: %s", c_message);
	CHECK(strstr(r_message, "overflow without allowoverflow set") != NULL,
		"Rust overflow took the wrong fatal path: %s", r_message);
	CHECK(strcmp(c_message, r_message) == 0,
		"overflow fatal diagnostics differ:\nC: %sRust: %s", c_message, r_message);

	CHECK(run_fatal(fatal_too_large_c, 1, c_message, sizeof(c_message)) == 0,
		"C too-large child failed");
	CHECK(run_fatal(fatal_too_large, 0, r_message, sizeof(r_message)) == 0,
		"Rust too-large child failed");
	CHECK(strstr(c_message, "is > full buffer size") != NULL,
		"C too-large took the wrong fatal path: %s", c_message);
	CHECK(strstr(r_message, "is > full buffer size") != NULL,
		"Rust too-large took the wrong fatal path: %s", r_message);
	CHECK(strcmp(c_message, r_message) == 0,
		"too-large fatal diagnostics differ:\nC: %sRust: %s", c_message, r_message);
	return 0;
}

int main(void)
{
	if (abi_checks() || init_checks() || write_checks() || print_checks() || fatal_checks())
		return 1;
	printf("sizebuf differential harness: PASS (%lu cases)\n", cases);
	return 0;
}
