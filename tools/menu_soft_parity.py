#!/usr/bin/env python3
"""menu_soft_parity.py -- keep the software renderer's Options menu honest.

The engine ships three client renderers (desktop GL 4.3, GLES3/WebGL2, and the
restored 8bpp software rasterizer) out of one menu.  Existence drift between
them is already impossible: shared code references renderer-policy cvars by
symbol, so r_soft_web.c must define every one of them or the WEBSOFT build
fails to link, and CI builds that config on every PR
(.github/actions/wasm-build/action.yml).

What the linker cannot see is *semantic* drift.  r_soft_web.c openly defines a
block of cvars "for linkage" that no software code path reads.  A menu row
driving one of those links green, draws fine, and does nothing -- and the only
thing standing between a new row and that fate is remembering to extend a
hand-written switch in menu.c.  This script is that reminder, as a build step.

Per menu group whose IsSkip helper has a `#if defined(WEBSOFT)` arm:

  1. The label table lines up with the enum -- same count, and each label's
     trailing /* PREFIX_X */ tag names the member at that index.  A row
     inserted without its label silently shifts every label below it.
  2. Every `case PREFIX_X:` in the WEBSOFT arm names a real enum member, so a
     renamed or deleted row cannot leave a stale case behind.
  3. Every enum member is classified exactly once: either hidden by the
     WEBSOFT arm, or listed in the group's `soft-ok:` comment.  This is the
     forcing function -- a new row fails the build until somebody decides
     which it is.
  4. Every `soft-ok:` row writes at least one cvar that a translation unit in
     the software build actually reads.  A row may write several cvars (the
     glow cycle writes four); one live reader is enough to call the row real,
     because the rest degrade rather than lie.

Reads engine/CMakeLists.txt for the source sets rather than globbing, so the
"software build" this checks against is the one CMake actually compiles.

Exit status 1 on any violation, silent on success.
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CMAKE = os.path.join(REPO, 'engine', 'CMakeLists.txt')
MENU = os.path.join(REPO, 'engine', 'hexen2', 'menu.c')

# menu.c reads cvars to render their own rows, which proves nothing about
# whether the renderer honours them.  Excluded from the reader search for
# check 4 -- without this every row trivially passes.
NOT_A_READER = {os.path.join(REPO, 'engine', 'hexen2', 'menu.c')}

errors = []


def fail(msg):
    errors.append(msg)


def lineno(text, pos):
    return text.count('\n', 0, pos) + 1


# --------------------------------------------------------------------------
# CMake source sets
# --------------------------------------------------------------------------

def cmake_lists(text):
    """Every set(NAME a b c) in the file, as {NAME: [resolved path, ...]}."""
    vars_ = {
        'ENGINE_TOP': os.path.join(REPO, 'engine'),
        'COMMONDIR': os.path.join(REPO, 'engine', 'h2shared'),
    }
    out = {}
    for m in re.finditer(r'^set\((\w+)\n(.*?)^\)', text, re.M | re.S):
        name, body = m.group(1), m.group(2)
        paths = []
        for raw in body.split('\n'):
            raw = raw.split('#', 1)[0].strip()
            if not raw:
                continue
            for var, val in vars_.items():
                raw = raw.replace('${%s}' % var, val)
            if raw.endswith('.c') and '${' not in raw:
                paths.append(os.path.normpath(raw))
        out[name] = paths
    return out


def software_translation_units():
    with open(CMAKE, encoding='utf-8') as fh:
        sets = cmake_lists(fh.read())
    missing = [n for n in ('CLIENT_SOURCES', 'COMMON_SOURCES',
                           'SOFT_RENDERER_SOURCES') if n not in sets]
    if missing:
        fail('engine/CMakeLists.txt: no set(%s ...) found -- has the source '
             'set been renamed?' % ', '.join(missing))
        return []
    tus = []
    for name in ('CLIENT_SOURCES', 'COMMON_SOURCES', 'SOFT_RENDERER_SOURCES'):
        tus.extend(sets[name])
    return [p for p in tus if p not in NOT_A_READER and os.path.exists(p)]


# --------------------------------------------------------------------------
# menu.c parsing
# --------------------------------------------------------------------------

def match_braces(text, open_pos):
    """Index just past the '}' matching the '{' at open_pos."""
    depth = 0
    i = open_pos
    while i < len(text):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return len(text)


def find_enums(text):
    """{PREFIX: [member, ...]} for every anonymous enum ending in PREFIX_ITEMS."""
    out = {}
    for m in re.finditer(r'\benum\s*\{', text):
        body = text[m.end() - 1:match_braces(text, m.end() - 1)]
        term = re.search(r'\b([A-Z][A-Z0-9]*)_ITEMS\b', body)
        if not term:
            continue
        prefix = term.group(1)
        members = []
        for name in re.findall(r'\b(%s_[A-Z0-9_]+)\b' % prefix, body):
            if name != prefix + '_ITEMS' and name not in members:
                members.append(name)
        out[prefix] = members
    return out


def find_function(text, pattern):
    """(body, offset) of the first *definition* matching pattern.

    The pattern must end at the signature's closing paren; the brace is
    required here so a forward declaration cannot match and drag in whatever
    function happens to be defined after it.
    """
    m = re.search(pattern + r'\s*\{', text)
    if not m:
        return None, None
    brace = text.index('{', m.end() - 1)
    return text[brace:match_braces(text, brace)], brace


def websoft_arm(body):
    """The `#if defined(WEBSOFT)` region of a function body, or None."""
    m = re.search(r'#if\s+defined\(WEBSOFT\)', body)
    if not m:
        return None
    end = re.compile(r'^\s*#(else|endif|elif)\b', re.M).search(body, m.end())
    return body[m.end():end.start() if end else len(body)]


def check_labels(text, prefix, members):
    array = re.search(
        r'static\s+const\s+char\s*\*\s*\w+_labels\s*\[\s*%s_ITEMS\s*\]\s*=\s*\{'
        % prefix, text)
    if not array:
        return          # not every group keeps a label table
    brace = array.end() - 1
    body = text[brace:match_braces(text, brace)]
    entries = re.findall(
        r'(?:"(?:[^"\\]|\\.)*"|\bNULL\b)\s*,?\s*(?:/\*\s*(\w+)\s*\*/)?',
        body)
    if len(entries) != len(members):
        fail('menu.c:%d: %s_labels has %d entries but the enum has %d members'
             % (lineno(text, array.start()), prefix.lower(), len(entries),
                len(members)))
        return
    for i, (tag, member) in enumerate(zip(entries, members)):
        if tag and tag != member:
            fail('menu.c:%d: %s_labels[%d] is tagged /* %s */ but that index '
                 'is %s -- the table has slipped'
                 % (lineno(text, array.start()), prefix.lower(), i, tag,
                    member))


def soft_ok_members(body, prefix):
    """Members listed in the group's `soft-ok:` comment."""
    m = re.search(r'soft-ok:(.*?)\*/', body, re.S)
    if not m:
        return None
    return set(re.findall(r'\b%s_[A-Z0-9_]+\b' % prefix, m.group(1)))


def row_cvars(text, group, prefix):
    """{member: {cvar names the row writes or reads}} from M_<group>_AdjustSliders."""
    body, _ = find_function(
        text, r'static\s+void\s+M_%s_AdjustSliders\s*\([^)]*\)' % group)
    if body is None:
        return {}
    out = {}
    cases = list(re.finditer(r'case\s+(%s_[A-Z0-9_]+)\s*:' % prefix, body))
    for i, case in enumerate(cases):
        end = cases[i + 1].start() if i + 1 < len(cases) else len(body)
        block = body[case.end():end]
        names = set(re.findall(r'Cvar_Set\w*\s*\(\s*"([^"]+)"', block))
        names |= set(re.findall(r'\b([a-z_][a-z0-9_]*)\.(?:integer|value|string)\b',
                                block))
        out.setdefault(case.group(1), set()).update(names)
    return out


def cvar_has_software_reader(cvar, sources):
    """Does any translation unit in the software build consume this cvar?

    Two idioms count: a direct .integer/.value/.string field read, and passing
    the cvar by address to a helper that reads it (SCR_CalcUIScale is the one
    that matters).  Registration is deliberately not a read -- r_soft_web.c
    registers the whole GPU-only block precisely so that shared code links,
    and counting that would make every row pass.
    """
    field = re.compile(r'\b%s\.(?:integer|value|string)\b' % re.escape(cvar))
    byref = re.compile(r'&\s*%s\b' % re.escape(cvar))
    for path in sources:
        with open(path, encoding='utf-8', errors='replace') as fh:
            src = fh.read()
        for line in src.split('\n'):
            if line.lstrip().startswith('cvar_t ') or 'Cvar_Register' in line:
                continue
            if field.search(line) or byref.search(line):
                return True
    return False


def main():
    with open(MENU, encoding='utf-8') as fh:
        text = fh.read()

    enums = find_enums(text)
    sources = software_translation_units()

    groups = []
    for m in re.finditer(r'static\s+qboolean\s+M_(\w+)_IsSkip\s*\([^)]*\)\s*\{',
                         text):
        if m.group(1) not in groups:
            groups.append(m.group(1))
    checked = 0

    for group in groups:
        body, off = find_function(
            text, r'static\s+qboolean\s+M_%s_IsSkip\s*\([^)]*\)' % group)
        if body is None:
            continue
        bound = re.search(r'>=\s*([A-Z][A-Z0-9]*)_ITEMS', body)
        if not bound:
            fail('menu.c:%d: M_%s_IsSkip has no `>= PREFIX_ITEMS` bounds check, '
                 'so this script cannot tell which enum it guards'
                 % (lineno(text, off), group))
            continue
        prefix = bound.group(1)
        members = enums.get(prefix)
        if not members:
            fail('menu.c:%d: M_%s_IsSkip guards %s_ITEMS but no such enum was '
                 'found' % (lineno(text, off), group, prefix))
            continue

        check_labels(text, prefix, members)

        arm = websoft_arm(body)
        if arm is None:
            continue    # group has no software/GL split to keep honest
        checked += 1

        hidden = set(re.findall(r'case\s+(%s_[A-Z0-9_]+)\s*:' % prefix, arm))
        hidden |= set(re.findall(r'==\s*(%s_[A-Z0-9_]+)\b' % prefix, arm))
        for name in sorted(hidden - set(members)):
            fail('menu.c:%d: M_%s_IsSkip hides %s, which is not a member of '
                 'the %s_ enum' % (lineno(text, off), group, name, prefix))

        declared = soft_ok_members(body, prefix)
        if declared is None:
            fail('menu.c:%d: M_%s_IsSkip has a WEBSOFT arm but no `soft-ok:` '
                 'comment listing the rows that do work in the software '
                 'renderer' % (lineno(text, off), group))
            continue

        for name in members:
            in_hidden, in_ok = name in hidden, name in declared
            if not in_hidden and not in_ok:
                fail('menu.c:%d: %s is unclassified -- either hide it in '
                     'M_%s_IsSkip\'s WEBSOFT arm or add it to that function\'s '
                     '`soft-ok:` list'
                     % (lineno(text, off), name, group))
            elif in_hidden and in_ok:
                fail('menu.c:%d: %s is both hidden under WEBSOFT and listed as '
                     'soft-ok' % (lineno(text, off), name, ))

        cvars = row_cvars(text, group, prefix)
        for name in sorted(declared & set(members)):
            driven = cvars.get(name, set())
            if not driven:
                fail('menu.c:%d: %s is declared soft-ok but M_%s_AdjustSliders '
                     'has no case for it, so there is nothing to verify'
                     % (lineno(text, off), name, group))
                continue
            if not any(cvar_has_software_reader(c, sources) for c in sorted(driven)):
                fail('menu.c:%d: %s is declared soft-ok, but no translation '
                     'unit in the software build reads any of the cvars it '
                     'drives (%s) -- the row is a dead knob there'
                     % (lineno(text, off), name, ', '.join(sorted(driven))))

    if not checked:
        fail('no menu group with a `#if defined(WEBSOFT)` arm was found in '
             'menu.c -- has the software renderer\'s menu handling moved?')

    for msg in errors:
        print('menu_soft_parity: %s' % msg, file=sys.stderr)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
