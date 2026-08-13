#!/usr/bin/env python3
"""
Disassemble Hexen II progs.dat bytecode (v6 and v7 on-disk formats).

Answers "does the HexenC in gamecode/hc actually match the bytecode players
run?" without building anything -- stock python3, no dependencies.

Usage:
  qcdis.py <progs.dat> [function ...]     disassemble named functions
  qcdis.py <progs.dat> --pseudo <func>    collapse temporaries into expressions
  qcdis.py <progs.dat> --list [PATTERN]   list functions (regex, case-insensitive)
  qcdis.py --check-opcodes                verify the opcode table vs common/pr_comp.h

Examples:
  qcdis.py ~/hexen2/data1/PROGS.DAT Use_TimeBomb
  qcdis.py ~/hexen2/sot/progs.dat --list 'TimeBomb|Glyph'
  diff <(qcdis.py a/progs.dat --pseudo Use_TimeBomb) \
       <(qcdis.py b/progs.dat --pseudo Use_TimeBomb)

Format reference: common/pr_comp.h (dprograms_t, dstatement_v6_t/v7_t,
ddef_v6_t/v7_t, dfunction_t).  See tools/README.md for the caveats that
matter when reading the output.
"""
import argparse
import os
import re
import struct
import sys

# Order is load-bearing: index == opcode number.  Must match the OP_* enum in
# common/pr_comp.h exactly; `--check-opcodes` re-derives it from that header.
OPS = """DONE MUL_F MUL_V MUL_FV MUL_VF DIV_F ADD_F ADD_V SUB_F SUB_V
EQ_F EQ_V EQ_S EQ_E EQ_FNC NE_F NE_V NE_S NE_E NE_FNC LE GE LT GT
LOAD_F LOAD_V LOAD_S LOAD_ENT LOAD_FLD LOAD_FNC ADDRESS
STORE_F STORE_V STORE_S STORE_ENT STORE_FLD STORE_FNC
STOREP_F STOREP_V STOREP_S STOREP_ENT STOREP_FLD STOREP_FNC
RETURN NOT_F NOT_V NOT_S NOT_ENT NOT_FNC IF IFNOT
CALL0 CALL1 CALL2 CALL3 CALL4 CALL5 CALL6 CALL7 CALL8
STATE GOTO AND OR BITAND BITOR
MULSTORE_F MULSTORE_V MULSTOREP_F MULSTOREP_V DIVSTORE_F DIVSTOREP_F
ADDSTORE_F ADDSTORE_V ADDSTOREP_F ADDSTOREP_V
SUBSTORE_F SUBSTORE_V SUBSTOREP_F SUBSTOREP_V
FETCH_GBL_F FETCH_GBL_V FETCH_GBL_S FETCH_GBL_E FETCH_GBL_FNC
CSTATE CWSTATE THINKTIME BITSET BITSETP BITCLR BITCLRP
RAND0 RAND1 RAND2 RANDV0 RANDV1 RANDV2
SWITCH_F SWITCH_V SWITCH_S SWITCH_E SWITCH_FNC CASE CASERANGE""".split()

TYPES = ['void', 'string', 'float', 'vector', 'entity', 'field', 'function', 'pointer']

T_STRING, T_FLOAT, T_VECTOR, T_FUNCTION = 1, 2, 3, 6

# OFS_PARM0..7 from common/pr_comp.h: 3 globals per slot so a vector fits.
PARM_OFS = [4, 7, 10, 13, 16, 19, 22, 25]
RESERVED_OFS = 28       # globals below this are the reserved return/parm block
DEF_SAVEGLOBAL = 1 << 15


def is_immediate(name):
    """True for the shared constant-pool name.

    hcc uses IMMEDIATE_NAME "I+" (common/pr_comp.h); qcc-era progs use the
    older "IMMEDIATE".  Every pooled constant shares the name, so it labels
    nothing useful and the value is printed instead.
    """
    return name.startswith('I+') or name == 'IMMEDIATE'


class ProgsError(Exception):
    """progs.dat is malformed or not a progs.dat at all."""


class Progs:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.buf = buf = f.read()
        self.path = path

        if len(buf) < 60:
            raise ProgsError('%s: too short to be a progs.dat (%d bytes)'
                             % (path, len(buf)))
        h = struct.unpack('<15i', buf[:60])
        (self.version, self.crc, self.ofs_st, self.n_st, self.ofs_gd, self.n_gd,
         self.ofs_fd, self.n_fd, self.ofs_fn, self.n_fn, self.ofs_str,
         self.n_str, self.ofs_gl, self.n_gl, self.entityfields) = h

        if self.version not in (6, 7):
            raise ProgsError('%s: unsupported progs version %d (expected 6 or 7)'
                             % (path, self.version))
        v7 = self.version == 7

        def check(what, ofs, count, size):
            if ofs < 0 or count < 0 or ofs + count * size > len(buf):
                raise ProgsError(
                    '%s: %s section runs past end of file (ofs %d, %d entries '
                    'of %d bytes, file is %d bytes)'
                    % (path, what, ofs, count, size, len(buf)))

        # dstatement_v6_t {ushort op; short a,b,c;}
        # dstatement_v7_t {ushort pad; ushort op; int a,b,c;}
        #
        # v6 a/b/c are declared short but read *unsigned* as global offsets --
        # a progs with >32767 globals overflows the short and the engine
        # zero-extends (PR_ConvertOldStmts, and the OPA/OPB/OPC unsigned short
        # casts in pr_exec.c).  Only jump offsets are re-signed; see jump().
        sz, fmt = (16, '<HHiii') if v7 else (8, '<HHHH')
        check('statements', self.ofs_st, self.n_st, sz)
        self.statements = []
        for i in range(self.n_st):
            o = self.ofs_st + i * sz
            f = struct.unpack(fmt, buf[o:o + sz])
            self.statements.append(f[1:5] if v7 else f[0:4])

        # ddef_v6_t {ushort type; ushort ofs; int s_name;}
        # ddef_v7_t {ushort pad; ushort type; int ofs; int s_name;}
        dsz, dfmt = (12, '<HHii') if v7 else (8, '<HHi')

        def defs(what, ofs, n):
            check(what, ofs, n, dsz)
            out = []
            for i in range(n):
                o = ofs + i * dsz
                f = struct.unpack(dfmt, buf[o:o + dsz])
                out.append(f[1:4] if v7 else f[0:3])
            return out

        self.globaldefs = defs('globaldefs', self.ofs_gd, self.n_gd)
        self.fielddefs = defs('fielddefs', self.ofs_fd, self.n_fd)

        # dfunction_t: 7 ints then byte parm_size[MAX_PARMS==8].
        check('functions', self.ofs_fn, self.n_fn, 36)
        self.functions = []
        for i in range(self.n_fn):
            o = self.ofs_fn + i * 36
            self.functions.append(struct.unpack('<7i8B', buf[o:o + 36])[:7])

        check('strings', self.ofs_str, self.n_str, 1)
        check('globals', self.ofs_gl, self.n_gl, 4)
        self.globals = buf[self.ofs_gl:self.ofs_gl + self.n_gl * 4]

        # First def wins: hcc emits one def per name, but unions and the
        # shared immediate pool can alias a single offset several times.
        self.gname, self.gtype, self.fname = {}, {}, {}
        for t, ofs, s in self.globaldefs:
            self.gname.setdefault(ofs, self.string(s))
            self.gtype.setdefault(ofs, t & ~DEF_SAVEGLOBAL)
        for t, ofs, s in self.fielddefs:
            self.fname.setdefault(ofs, self.string(s))
        self.funcbyname = {}
        for i, f in enumerate(self.functions):
            self.funcbyname.setdefault(self.string(f[4]), i)
        self._fs = None

    # -- raw accessors ----------------------------------------------------

    def string(self, ofs):
        """Read a NUL-terminated string from the string pool."""
        if ofs < 0 or ofs >= self.n_str:
            return '?<%d>' % ofs
        start = self.ofs_str + ofs
        end = self.buf.find(b'\0', start, self.ofs_str + self.n_str)
        if end < 0:                     # unterminated: stop at the pool edge
            end = self.ofs_str + self.n_str
        return self.buf[start:end].decode('latin-1')

    def in_globals(self, ofs):
        return 0 <= ofs and (ofs + 1) * 4 <= len(self.globals)

    def gfloat(self, ofs):
        if not self.in_globals(ofs):
            return 0.0
        return struct.unpack('<f', self.globals[ofs * 4:ofs * 4 + 4])[0]

    def gint(self, ofs):
        if not self.in_globals(ofs):
            return 0
        return struct.unpack('<i', self.globals[ofs * 4:ofs * 4 + 4])[0]

    def jump(self, v):
        """Re-sign a v6 operand being used as a relative jump offset.

        Mirrors `if (is_progs_v6) jump_ofs = (signed short)jump_ofs` in
        pr_exec.c: the same 16 bits mean unsigned when they address a global
        and signed when they displace the statement pointer.
        """
        if self.version == 6 and v >= 0x8000:
            return v - 0x10000
        return v

    def func_starts(self):
        if self._fs is None:
            self._fs = set(f[0] for f in self.functions if f[0] > 0)
        return self._fs

    def func_name(self, index):
        if 0 <= index < self.n_fn:
            return self.string(self.functions[index][4])
        return None

    def field_name(self, b):
        return self.fname.get(self.gint(b), 'fld%d' % b)

    def _end_of(self, first):
        """Last statement index this function can possibly own.

        Functions are laid out in statement order, so the next function's
        first_statement is a hard bound even when the DONE heuristic misses.
        """
        later = [s for s in self.func_starts() if s > first]
        return min(later) if later else self.n_st

    # -- operand rendering ------------------------------------------------

    def gsym(self, ofs):
        """Render a global operand: a name, a named constant, or a literal."""
        if not self.in_globals(ofs):
            return 'g%d?' % ofs
        n = self.gname.get(ofs)
        t = self.gtype.get(ofs)
        if n and not is_immediate(n) and n != '':
            if t == T_FLOAT:
                return '%s{%g}' % (n, self.gfloat(ofs))
            if t == T_VECTOR:
                return "%s{%g %g %g}" % (n, self.gfloat(ofs), self.gfloat(ofs + 1),
                                         self.gfloat(ofs + 2))
            return n
        if n is None:
            return 't%d' % ofs
        # immediate / temp: show the constant value
        if t == T_STRING:
            return '"%s"' % self.string(self.gint(ofs))[:48]
        if t == T_VECTOR:
            return "'%g %g %g'" % (self.gfloat(ofs), self.gfloat(ofs + 1),
                                   self.gfloat(ofs + 2))
        if t == T_FUNCTION:
            fn = self.func_name(self.gint(ofs))
            if fn is not None:
                return '%s()' % fn
        f = self.gfloat(ofs)
        i = self.gint(ofs)
        if n:
            return '%s{%g}' % (n, f)
        if -1e9 < f < 1e9 and (f == f):
            return '%g' % f if f != 0 or i == 0 else 'g%d' % ofs
        return 'g%d' % ofs

    # -- disassembly ------------------------------------------------------

    def disasm(self, name, maxst=400):
        fi = self.funcbyname.get(name)
        if fi is None:
            return '<no function %s>' % name
        fn = self.functions[fi]
        first, parm_start, locals_, profile, s_name, s_file, numparms = fn
        out = ['; function %s  (file %s, first_statement %d, locals %d)'
               % (name, self.string(s_file), first, locals_)]
        if first < 0:
            out.append('  builtin #%d' % -first)
            return '\n'.join(out)
        limit = min(self.n_st, self._end_of(first))
        pc = first
        while pc < limit and pc - first < maxst:
            op, a, b, c = self.statements[pc]
            nm = OPS[op] if op < len(OPS) else 'OP%d' % op
            out.append('%5d  %-12s %s' % (pc, nm, self.fmt(nm, a, b, c, pc)))
            if nm in ('DONE', 'RETURN'):
                # A function can contain several returns; only the one that
                # butts up against the next function ends it.
                if pc + 1 >= limit or pc + 1 in self.func_starts():
                    break
            pc += 1
        else:
            if pc - first >= maxst:
                out.append('; ... truncated at %d statements (raise --max-statements)'
                           % maxst)
        return '\n'.join(out)

    def fmt(self, nm, a, b, c, pc):
        g = self.gsym
        if nm.startswith('CALL'):
            n = int(nm[4:])
            # The operand's *static* value is the callee.  For an indirect call
            # this is whatever the linker left there (usually function 0, whose
            # name is empty) -- see "Indirect calls" in tools/README.md.
            fname = self.func_name(self.gint(a))
            if fname is None:
                fname = g(a)
            args = ', '.join(g(o) for o in PARM_OFS[:n])
            return '%s(%s)' % (fname, args)
        if nm in ('IF', 'IFNOT'):
            return '%s -> %d' % (g(a), pc + self.jump(b))
        if nm == 'GOTO':
            return '-> %d' % (pc + self.jump(a))
        if nm.startswith('SWITCH_'):
            return '%s -> %d' % (g(a), pc + self.jump(b))
        if nm == 'CASE':
            return '%s -> %d' % (g(a), pc + self.jump(b))
        if nm == 'CASERANGE':
            return '%s .. %s -> %d' % (g(a), g(b), pc + self.jump(c))
        if nm.startswith('STORE_'):
            return '%s := %s' % (g(b), g(a))
        if nm.startswith('STOREP_'):
            return '*%s := %s' % (g(b), g(a))
        if nm.startswith('LOAD_'):
            return '%s := %s.%s' % (g(c), g(a), self.field_name(b))
        if nm == 'ADDRESS':
            return '%s := &%s.%s' % (g(c), g(a), self.field_name(b))
        if nm in ('ADDSTOREP_F', 'ADDSTOREP_V', 'SUBSTOREP_F', 'SUBSTOREP_V',
                  'MULSTOREP_F', 'MULSTOREP_V', 'BITSETP', 'BITCLRP'):
            return '*%s %s= %s' % (g(b), nm[:3], g(a))
        if nm == 'THINKTIME':
            return '%s.nextthink := time + %s' % (g(a), g(b))
        if nm == 'RETURN':
            return g(a)
        if nm == 'DONE':
            return ''
        return '%s := %s %s %s' % (g(c), g(a), nm, g(b))


BINOP = {'ADD_F': '+', 'ADD_V': '+', 'SUB_F': '-', 'SUB_V': '-', 'MUL_F': '*',
         'MUL_V': '*', 'MUL_FV': '*', 'MUL_VF': '*', 'DIV_F': '/',
         'BITAND': '&', 'BITOR': '|', 'AND': '&&', 'OR': '||',
         'EQ_F': '==', 'EQ_V': '==', 'EQ_S': '==', 'EQ_E': '==', 'EQ_FNC': '==',
         'NE_F': '!=', 'NE_V': '!=', 'NE_S': '!=', 'NE_E': '!=', 'NE_FNC': '!=',
         'LE': '<=', 'GE': '>=', 'LT': '<', 'GT': '>'}


def pseudo(p, name, maxst=400):
    """Resolve temporaries into expressions; print only observable effects.

    Lossier than disasm() but far easier to diff: two compilers that allocate
    temporaries differently produce identical pseudo output for identical
    source.
    """
    fi = p.funcbyname.get(name)
    if fi is None:
        return '<no function %s>' % name
    first = p.functions[fi][0]
    if first < 0:
        return 'builtin #%d' % -first
    expr = {}          # global ofs -> expression string
    lines = []

    def val(o):
        if o in expr:
            return expr[o]
        if not p.in_globals(o):
            return 'g%d?' % o
        n = p.gname.get(o)
        t = p.gtype.get(o)
        if n and not is_immediate(n) and t not in (T_FLOAT, T_VECTOR):
            return n
        if t == T_STRING:
            return '"%s"' % p.string(p.gint(o))
        if t == T_VECTOR:
            return "'%g %g %g'" % (p.gfloat(o), p.gfloat(o + 1), p.gfloat(o + 2))
        if t == T_FUNCTION:
            fn = p.func_name(p.gint(o))
            return 'fn?' if fn is None else fn
        if t == T_FLOAT:
            return '%g' % p.gfloat(o)
        if n:
            return n
        return 'g%d' % o

    limit = min(p.n_st, p._end_of(first))
    pc = first
    while pc < limit and pc - first < maxst:
        op, a, b, c = p.statements[pc]
        nm = OPS[op] if op < len(OPS) else 'OP%d' % op
        if nm == 'ADDRESS':
            expr[c] = '&%s.%s' % (val(a), p.field_name(b))
        elif nm.startswith('LOAD_'):
            expr[c] = '%s.%s' % (val(a), p.field_name(b))
        elif nm in BINOP:
            expr[c] = '(%s %s %s)' % (val(a), BINOP[nm], val(b))
        elif nm.startswith('STORE_'):
            expr[b] = val(a)
            # Below RESERVED_OFS is the return/parm block, not a real variable.
            if b >= RESERVED_OFS and p.gname.get(b):
                lines.append('%s = %s' % (p.gname[b], val(a)))
        elif nm.startswith('STOREP_'):
            lines.append('%s = %s' % (val(b).lstrip('&'), val(a)))
        elif nm.startswith('CALL'):
            n = int(nm[4:])
            fname = p.func_name(p.gint(a))
            if fname is None:
                fname = val(a)
            args = ', '.join(val(o) for o in PARM_OFS[:n])
            lines.append('%s(%s)' % (fname, args))
            expr[1] = '%s(...)' % fname       # OFS_RETURN
        elif nm == 'THINKTIME':
            lines.append('%s.nextthink = time + %s' % (val(a), val(b)))
        elif nm in ('IF', 'IFNOT'):
            lines.append('%s %s -> %d' % (nm, val(a), pc + p.jump(b)))
        elif nm == 'GOTO':
            lines.append('goto %d' % (pc + p.jump(a)))
        elif nm.startswith('SWITCH_'):
            lines.append('switch %s -> %d' % (val(a), pc + p.jump(b)))
        elif nm == 'CASE':
            lines.append('case %s -> %d' % (val(a), pc + p.jump(b)))
        elif nm == 'CASERANGE':
            lines.append('case %s .. %s -> %d' % (val(a), val(b), pc + p.jump(c)))
        elif nm.endswith('STOREP_F') or nm.endswith('STOREP_V'):
            lines.append('%s %s= %s' % (val(b).lstrip('&'), nm[:3], val(a)))
        elif nm in ('BITSETP', 'BITCLRP'):
            lines.append('%s %s= %s' % (val(b).lstrip('&'), nm[:6], val(a)))
        elif nm == 'RETURN':
            lines.append('return %s' % val(a))
        elif nm == 'DONE':
            if pc + 1 >= limit or pc + 1 in p.func_starts():
                break
            lines.append('done')
        else:
            lines.append('%s %s %s' % (nm, val(a), val(b)))
        pc += 1
    else:
        if pc - first >= maxst:
            lines.append('; ... truncated at %d statements (raise --max-statements)'
                         % maxst)
    return '\n'.join(lines)


def check_opcodes(header):
    """Re-derive OPS from the engine's OP_* enum and diff it against ours."""
    try:
        with open(header) as f:
            src = f.read()
    except OSError as e:
        print('cannot read %s: %s' % (header, e), file=sys.stderr)
        return 1
    body = src[src.index('OP_DONE'):src.index('OP_CASERANGE') + len('OP_CASERANGE')]
    names = re.findall(r'OP_([A-Z0-9_]+)', body)
    if names == OPS:
        print('opcode table matches %s (%d opcodes)' % (header, len(OPS)))
        return 0
    print('MISMATCH vs %s: header has %d opcodes, qcdis.py has %d'
          % (header, len(names), len(OPS)), file=sys.stderr)
    for i in range(max(len(names), len(OPS))):
        h = names[i] if i < len(names) else '<none>'
        o = OPS[i] if i < len(OPS) else '<none>'
        if h != o:
            print('  %3d: header=%s qcdis=%s' % (i, h, o), file=sys.stderr)
    return 1


def default_header():
    """common/pr_comp.h, resolved relative to this script's checkout."""
    return os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        os.pardir, 'common', 'pr_comp.h')


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Disassemble Hexen II progs.dat bytecode (v6/v7).')
    ap.add_argument('progs', nargs='?', help='path to progs.dat')
    ap.add_argument('functions', nargs='*', help='function names to disassemble')
    ap.add_argument('--pseudo', action='store_true',
                    help='collapse temporaries into expressions (easier to diff)')
    ap.add_argument('--list', nargs='?', const='', metavar='PATTERN',
                    help='list function names matching a regex (default: all)')
    ap.add_argument('--max-statements', type=int, default=400, metavar='N',
                    help='statement budget per function (default: 400)')
    ap.add_argument('--check-opcodes', nargs='?', const=default_header(),
                    metavar='PR_COMP_H',
                    help='verify the opcode table against common/pr_comp.h and exit')
    args = ap.parse_args(argv)

    if args.check_opcodes is not None:
        return check_opcodes(args.check_opcodes)
    if not args.progs:
        ap.error('a progs.dat path is required')

    try:
        p = Progs(args.progs)
    except ProgsError as e:
        print('qcdis: %s' % e, file=sys.stderr)
        return 2
    except OSError as e:
        print('qcdis: cannot read %s: %s' % (args.progs, e), file=sys.stderr)
        return 2

    if args.list is not None:
        try:
            pat = re.compile(args.list, re.I)
        except re.error as e:
            print('qcdis: bad --list pattern: %s' % e, file=sys.stderr)
            return 2
        for name in sorted(p.funcbyname):
            if pat.search(name):
                fn = p.functions[p.funcbyname[name]]
                where = ('builtin #%d' % -fn[0]) if fn[0] < 0 else \
                        ('%s:%d' % (p.string(fn[5]), fn[0]))
                print('%-40s %s' % (name, where))
        return 0

    print('; progs version %d, %d functions, %d statements'
          % (p.version, p.n_fn, p.n_st))
    missing = False
    for name in args.functions:
        print()
        text = pseudo(p, name, args.max_statements) if args.pseudo \
            else p.disasm(name, args.max_statements)
        if text.startswith('<no function '):
            missing = True
        print(text)
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
