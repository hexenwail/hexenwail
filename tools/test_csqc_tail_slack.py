#!/usr/bin/env python3
"""The engine runs csprogs.dat images the old hcc built (#120, engine half).

hcc used to end the globals block at the last global it allocated, which left
two reads the engine checks running past it -- `fix(hcc): pad the globals
block past every read the engine checks (#120)' fixed hcc, and this is the
engine accepting images already shipped without that padding:

  - a vector operand (OP_RETURN reads three words whatever it returns) on one
    of the last two globals.  The CSQC loader now gives the globals their own
    buffer with PR_GLOBALS_TAIL_SLACK zeroed words after them, and
    PR_ValidateBytecode lets a wide operand run into that tail, never start in
    it;
  - a builtin's function record with parm_start at the end.  Nothing reads a
    builtin's parm_start: OP_CALL dispatches builtins before EnterFunction,
    and PR_ExecuteProgram now refuses a builtin entry point outright (it used
    to run one from a negative statement index), so the record check skips
    builtins.

Part 1 compiles the real PR_ValidateBytecode and its operand-role table out of
engine/h2shared/pr_edict.c and runs both shapes and their near misses.  Part 2
checks the wiring at source level.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
edict = (ROOT / "engine/h2shared/pr_edict.c").read_text()
errors = []


def body(text, signature):
    start = text.index(signature)
    i = text.index("{", start)
    depth, end = 1, i + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


roles_enum = edict[edict.index("enum {\n\tPRO_UNUSED"):]
roles_enum = roles_enum[:roles_enum.index("};") + 2]
table = edict[edict.index("static const byte pr_operandrole[][3] ="):]
table = table[:table.index("\n};") + 3]
validate = body(edict, "const char *PR_ValidateBytecode (")

SRC = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char byte;
typedef int qboolean;
enum { false, true };
#define q_snprintf snprintf
#define COMPILE_TIME_ASSERT(name, x) typedef int dummy_ ## name[(x) ? 1 : -1]
#include "pr_comp.h"
''' + roles_enum + "\n" + table + "\n" + validate + r'''

static dstatement_t st[4];
static dfunction_t fn[3];

/* function 1: QC, statements 0..1; function 2 as set by the case */
static const char *check (int op, int a, int numglobals, int slack, int builtin_parm_start)
{
	memset (st, 0, sizeof st);
	memset (fn, 0, sizeof fn);
	st[0].op = op; st[0].a = a;
	st[1].op = OP_DONE;
	fn[1].first_statement = 0;
	fn[1].parm_start = 0;
	fn[2].first_statement = -5;		/* builtin #5 */
	fn[2].parm_start = builtin_parm_start;
	fn[2].numparms = 1;
	fn[2].parm_size[0] = 3;
	return PR_ValidateBytecode (st, 2, fn, 3, numglobals, false, slack);
}

int main (void)
{
	const char *r;

	/* The tail.hc shape: OP_RETURN on the last global of 33. */
	r = check (OP_RETURN, 32, 33, 0, 0);
	assert (r && strstr (r, "statement 0 reaches global 32 of 33"));
	assert (!check (OP_RETURN, 32, 33, 2, 0));		/* accepted with the tail */
	assert (!check (OP_RETURN, 31, 33, 2, 0));
	/* ...but never starting in the tail, wide or not. */
	assert (check (OP_RETURN, 33, 33, 2, 0));
	assert (check (OP_STORE_F, 33, 33, 2, 0));
	/* A 1-wide operand gets no slack at all. */
	assert (!check (OP_STORE_F, 32, 33, 2, 0));
	assert (check (OP_STORE_F, 33, 33, 0, 0));
	/* Slack is capped at what a vector can use. */
	assert (check (OP_RETURN, 33, 33, 100, 0));

	/* The builtin.hc shape: a builtin's parm_start at the end of 32.  Its
	 * record is no longer checked; a QC function's still is. */
	assert (!check (OP_DONE, 0, 32, 0, 32));
	memset (fn, 0, sizeof fn);
	fn[1].first_statement = 0; fn[1].parm_start = 32; fn[1].numparms = 1; fn[1].parm_size[0] = 3;
	memset (st, 0, sizeof st); st[0].op = OP_DONE;
	r = PR_ValidateBytecode (st, 1, fn, 2, 32, false, 2);
	assert (r && strstr (r, "function 1 has"));

	puts ("ok   PR_ValidateBytecode: tail slack for wide operands, builtin records unchecked");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c, exe = Path(tmp) / "t.c", Path(tmp) / "t"
    c.write_text(SRC)
    r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall", "-Werror",
                        "-Wno-unused-function", "-I", str(ROOT / "common"), "-o", str(exe), str(c)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("FAIL compiling PR_ValidateBytecode:\n" + r.stderr[-3000:])
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode:
        sys.exit("FAIL PR_ValidateBytecode:\n" + r.stderr)

csqc = strip((ROOT / "engine/hexen2/cl_csqc.c").read_text())
if not re.search(r"Hunk_AllocName\s*\(\s*\(\s*p->numglobals\s*\+\s*PR_GLOBALS_TAIL_SLACK\s*\)\s*\*\s*4", csqc):
    errors.append("cl_csqc.c: the CSQC globals need their own buffer with PR_GLOBALS_TAIL_SLACK words after them")
if not re.search(r"p->numglobals\s*,\s*false\s*,\s*PR_GLOBALS_TAIL_SLACK\s*\)", csqc):
    errors.append("cl_csqc.c: PR_ValidateBytecode must be told about the tail it allocated")
if not re.search(r"if\s*\(\s*fn->first_statement\s*<\s*0\s*\)\s*continue\s*;", csqc):
    errors.append("cl_csqc.c: a builtin must never become a CSQC entry point")
if not re.search(r"progs->numglobals\s*,\s*is_progs_v6\s*,\s*0\s*\)", strip(edict)):
    errors.append("pr_edict.c: the server progs have no tail and must pass slack 0")
exe_src = strip((ROOT / "engine/h2shared/pr_exec.c").read_text())
entry = body(exe_src, "void PR_ExecuteProgram (func_t fnum, const char *funcname)")
cut = entry.index("EnterFunction")
if not re.search(r"if\s*\(\s*f->first_statement\s*<\s*0\s*\)", entry[:cut]):
    errors.append("pr_exec.c: PR_ExecuteProgram must refuse a builtin before EnterFunction")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS old hcc csprogs.dat images load, and nothing reaches past the globals (#120)")
