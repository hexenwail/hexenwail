#!/usr/bin/env python3
"""The shadow-map unit always holds a depth cube array (GitHub #526).

sworld_frag, sworld_opaque_frag and salias_frag declare u_dlshadowmap, a
samplerCubeArrayShadow on DLSHADOW_TMU, on every desktop frame, and GL checks a
sampler against what its unit holds at every draw, whether the shader reads it
or not.  R_Shadow_Draw used to bind the cube array only `if (sh_frame_maps >
0)`, so on every frame without a map (r_dynamic 0, r_dlight_occlusion 0 or 1,
r_shadow_maxlights 0, no dlight in view) the unit held texture 0, a non-depth
format with no compare, and the driver reported undefined behaviour once per
world and alias draw: 27,844 times on a short demo1 run.  Checked here, from
the sources (stock python3):

  * R_Shadow_Draw binds DLSHADOW_TMU as a GL_TEXTURE_CUBE_MAP_ARRAY exactly
    once, unconditionally -- at the top level of its body, not under an `if`,
    before the -gpu arm returns.  What it binds (the expression, or the one
    assignment of the local it names) is evaluated as C over the four cases:
    a frame with maps binds sh_tex on both backends; a frame without binds
    SH_NoneTexture () on GL and 0 under -gpu, where the backend's own typed
    placeholder (rg_resource.c, with its D32 fallback) fills the slot;
  * SH_NoneTexture makes, once, a square GL_TEXTURE_CUBE_MAP_ARRAY in a depth
    format, whole cubes of layers, with defined contents (a non-NULL upload),
    base and max level 0, a non-mipmap filter, the compare on
    (GL_COMPARE_REF_TO_TEXTURE: a shadow sampler on a depth texture with the
    compare off is undefined too) with a compare func, and only through the
    R_* texture seams;
  * R_Shadow_Shutdown deletes it and forgets its name, so a vid_restart
    makes a fresh one in the new context;
  * every stage that declares u_dlshadowmap declares a samplerCubeArrayShadow,
    the class the placeholder stands in for;
  * outside gl_shadow.c (and the seam and backend themselves) nothing binds
    a GL_TEXTURE_CUBE_MAP_ARRAY on any unit, nor anything at DLSHADOW_TMU by
    that name.  A literal 9 is test_dlshadow_block.py's to catch; SSAO's 2D
    texture on unit 9 is in a program that declares no shadow sampler.

The checks also run on mutants that must fail (the pre-fix conditional bind,
a conditional placeholder bind, sh_none bound under -gpu, a non-square, a
colour-format and a compare-less placeholder) and on harmless rewrites that
must pass (`sh_frame_maps ? ...`, the value hoisted into a local).

tools/shadow-sampler-check.sh is the live half: the engine run, counting the
driver's reports.

Usage:  test_shadow_tmu_bound.py [-v]
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SHADOW_C = "engine/h2shared/gl_shadow.c"
VERBOSE = "-v" in sys.argv[1:]


def read(rel):
    return (ROOT / rel).read_text(errors="replace")


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def body_of(text, name):
    """The body (between the braces) of the first top-level definition of name."""
    m = re.search(r"^[A-Za-z_][\w \t\*]*\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", text, re.M)
    if not m:
        return None
    i, depth = m.end() - 1, 0
    while i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[m.end():i]
        i += 1
    return None


def desktop_arm(code):
    """gl_shadow.c from `#ifndef USE_GLES` to the `#else` / `#endif` that ends
    it: the GL 4.3 definitions.  Nested #if arms inside are kept."""
    m = re.search(r"^#ifndef\s+USE_GLES\b", code, re.M)
    if not m:
        return code
    depth = 0
    for d in re.finditer(r"^#\s*(if|ifdef|ifndef|else|elif|endif)\b", code[m.end():], re.M):
        kw = d.group(1)
        if kw.startswith("if"):
            depth += 1
        elif kw == "endif":
            if depth == 0:
                return code[m.end():m.end() + d.start()]
            depth -= 1
        elif depth == 0:
            return code[m.end():m.end() + d.start()]
    return code[m.end():]


def norm(s):
    return re.sub(r"\s+", " ", s).strip()


def call_args(text, start):
    """Split the argument list whose '(' is at text[start] at top-level commas."""
    args, depth, cur = [], 0, ""
    for j in range(start, len(text)):
        ch = text[j]
        if ch == "(":
            depth += 1
            if depth == 1:
                continue
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(norm(cur))
                return args, j + 1
        if ch == "," and depth == 1:
            args.append(norm(cur))
            cur = ""
        else:
            cur += ch
    return None, len(text)


def calls(text, name):
    """(offset, [args]) of each call to name."""
    out = []
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*(?=\()", text):
        args, _ = call_args(text, m.end())
        if args is not None:
            out.append((m.start(), args))
    return out


# --- a C conditional expression, evaluated -------------------------------

class Tex:
    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return self.name


SH_TEX, SH_NONE = Tex("sh_tex"), Tex("SH_NoneTexture ()")


def split_ternary(e):
    """(cond, a, b) for the top-level `cond ? a : b` of e, or None."""
    depth, q = 0, -1
    for i, ch in enumerate(e):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == "?" and depth == 0:
            q = i
            break
    if q < 0:
        return None
    depth, nest = 0, 0
    for i in range(q + 1, len(e)):
        ch = e[i]
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif depth == 0 and ch == "?":
            nest += 1
        elif depth == 0 and ch == ":":
            if nest == 0:
                return e[:q], e[q + 1:i], e[i + 1:]
            nest -= 1
    return None


def c_eval(e, maps, gpu):
    e = e.strip()
    while e.startswith("(") and e.endswith(")") and call_args(e, 0)[1] == len(e):
        e = e[1:-1].strip()
    t = split_ternary(e)
    if t:
        return c_eval(t[1], maps, gpu) if c_eval(t[0], maps, gpu) else c_eval(t[2], maps, gpu)
    py = re.sub(r"&&", " and ", e)
    py = re.sub(r"\|\|", " or ", py)
    py = re.sub(r"!(?!=)", " not ", py)
    py = re.sub(r"\b(SH_GPU|R_GPU)\s*\(\s*\)", "gpu", py)
    py = re.sub(r"\bSH_NoneTexture\s*\(\s*\)", "SH_NONE", py)
    py = re.sub(r"\(\s*GLuint\s*\)", "", py)
    names = set(re.findall(r"\b[A-Za-z_]\w*", py))
    if not re.fullmatch(r"[\w\s<>=!()+\-*]*", py) or \
            names - {"and", "or", "not", "gpu", "SH_NONE", "sh_tex", "sh_frame_maps"}:
        raise ValueError("cannot evaluate `%s`" % e)
    return eval(py, {"__builtins__": {}},
                {"gpu": gpu, "SH_NONE": SH_NONE, "sh_tex": SH_TEX, "sh_frame_maps": maps})


def check_draw(code):
    errs = []
    body = body_of(desktop_arm(code), "R_Shadow_Draw")
    if body is None:
        return ["cannot find R_Shadow_Draw in the desktop arm of " + SHADOW_C]
    b = [(at, a) for at, a in calls(body, "R_BindTextureTarget") if a and a[0] == "DLSHADOW_TMU"]
    if len(b) != 1:
        return ["R_Shadow_Draw must bind DLSHADOW_TMU exactly once, found %d" % len(b)]
    at, args = b[0]
    if len(args) != 3 or args[1] != "GL_TEXTURE_CUBE_MAP_ARRAY":
        return ["R_Shadow_Draw's DLSHADOW_TMU bind must be (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY, tex), "
                "got (%s)" % ", ".join(args)]
    before = body[:at]
    depth = before.count("{") - before.count("}")
    # a brace-less `if (...)` / `else` / loop head right before it is a condition too
    head = re.search(r"(\b(if|while|for)\s*\([^;{}]*\)|\belse)\s*$", before)
    if depth != 0 or head:
        errs.append("R_Shadow_Draw's DLSHADOW_TMU bind must run every frame, not under a condition "
                    "(a frame without a map left texture 0 there, #526)")
    gpu = re.search(r"#ifdef\s+USE_SDL_GPU", body)
    ret = re.search(r"\breturn\b", body)
    first_exit = min([m.start() for m in (gpu, ret) if m] or [len(body)])
    if at > first_exit:
        errs.append("R_Shadow_Draw's DLSHADOW_TMU bind must come before the -gpu arm returns")

    expr = args[2]
    if re.fullmatch(r"[A-Za-z_]\w*", expr) and expr not in ("sh_tex", "sh_none"):
        sets = re.findall(r"\b" + expr + r"\s*=(?!=)\s*([^;]*);", before)
        if len(sets) != 1:
            return errs + ["R_Shadow_Draw's bind names the local `%s`, which must be assigned exactly "
                           "once before it (found %d)" % (expr, len(sets))]
        expr = norm(sets[0])
    want = {(True, False): SH_TEX, (True, True): SH_TEX, (False, False): SH_NONE, (False, True): 0}
    for (maps, on_gpu), w in want.items():
        try:
            got = c_eval(expr, 3 if maps else 0, on_gpu)
        except Exception as ex:
            return errs + ["R_Shadow_Draw's DLSHADOW_TMU texture: %s" % ex]
        same = got is w if isinstance(w, Tex) else (not isinstance(got, Tex) and got == w)
        if not same:
            errs.append("R_Shadow_Draw binds %r at DLSHADOW_TMU on a frame %s maps %s; must be %r"
                        % (got, "with" if maps else "without", "under -gpu" if on_gpu else "on GL", w))
    return errs


def check_none(code):
    errs = []
    arm = desktop_arm(code)
    if not re.search(r"^static\s+GLuint\s+sh_none\s*;", arm, re.M):
        errs.append("gl_shadow.c must keep the placeholder's name in `static GLuint sh_none;`")
    body = body_of(arm, "SH_NoneTexture")
    if body is None:
        return errs + ["cannot find SH_NoneTexture in " + SHADOW_C]
    if not re.search(r"\bif\s*\(\s*sh_none\s*\)\s*return\s+sh_none\s*;", body) or \
            body.index("sh_none") > body.find("R_GenTextures"):
        errs.append("SH_NoneTexture must return the one it already made before making another")
    if re.search(r"\bgl\w+_fp\s*\(|\bgl[A-Z]\w*\s*\(", body):
        errs.append("SH_NoneTexture must create through the R_* seams alone (no raw GL)")
    img = calls(body, "R_TexImage3D")
    if len(img) != 1:
        errs.append("SH_NoneTexture must allocate with one R_TexImage3D")
    else:
        a = img[0][1]
        if len(a) != 10 or a[0] != "GL_TEXTURE_CUBE_MAP_ARRAY" or a[1] != "sh_none" or a[2] != "0":
            errs.append("SH_NoneTexture's R_TexImage3D must make level 0 of sh_none as a "
                        "GL_TEXTURE_CUBE_MAP_ARRAY, got (%s)" % ", ".join(a))
        else:
            if not re.fullmatch(r"GL_DEPTH_COMPONENT(16|24|32F?)", a[3]):
                errs.append("SH_NoneTexture's format must be a depth format, got %s" % a[3])
            if not (a[4] == a[5] and a[4].isdigit() and int(a[4]) > 0):
                errs.append("SH_NoneTexture's cube faces must be square, got %sx%s" % (a[4], a[5]))
            if not (a[6].isdigit() and int(a[6]) > 0 and int(a[6]) % 6 == 0):
                errs.append("SH_NoneTexture's depth must be whole cubes (a multiple of 6 layers), got %s" % a[6])
            if a[7] != "GL_DEPTH_COMPONENT":
                errs.append("SH_NoneTexture's upload format must be GL_DEPTH_COMPONENT, got %s" % a[7])
            if a[9] in ("NULL", "0"):
                errs.append("SH_NoneTexture must upload defined texels (far depth), not NULL")
    params = {}
    for _, p in calls(body, "R_TexParameteri"):
        if len(p) == 4 and p[0] == "GL_TEXTURE_CUBE_MAP_ARRAY" and p[1] == "sh_none":
            params[p[2]] = p[3]
    if params.get("GL_TEXTURE_COMPARE_MODE") != "GL_COMPARE_REF_TO_TEXTURE":
        errs.append("SH_NoneTexture must turn the compare on (GL_TEXTURE_COMPARE_MODE "
                    "GL_COMPARE_REF_TO_TEXTURE): a shadow sampler with it off is undefined")
    if not params.get("GL_TEXTURE_COMPARE_FUNC", "").startswith("GL_"):
        errs.append("SH_NoneTexture must set a GL_TEXTURE_COMPARE_FUNC")
    if params.get("GL_TEXTURE_BASE_LEVEL") != "0" or params.get("GL_TEXTURE_MAX_LEVEL") != "0":
        errs.append("SH_NoneTexture must set GL_TEXTURE_BASE_LEVEL and GL_TEXTURE_MAX_LEVEL 0, "
                    "or its one level is incomplete")
    if not any(len(f) == 4 and f[:2] == ["GL_TEXTURE_CUBE_MAP_ARRAY", "sh_none"]
               and f[2] in ("GL_NEAREST", "GL_LINEAR") and f[3] in ("GL_NEAREST", "GL_LINEAR")
               for _, f in calls(body, "R_TexSetFilter")):
        errs.append("SH_NoneTexture must set a non-mipmap filter")
    if not any(f == ["GL_TEXTURE_CUBE_MAP_ARRAY", "0"] for _, f in calls(body, "R_BindTextureForEdit")):
        errs.append("SH_NoneTexture must take it off the edit unit again")
    shut = body_of(arm, "R_Shadow_Shutdown")
    if shut is None or not re.search(r"\bR_DeleteTextures\s*\(\s*1\s*,\s*&sh_none\s*\)", shut) \
            or not re.search(r"\bsh_none\s*=\s*0\s*;", shut):
        errs.append("R_Shadow_Shutdown must delete sh_none and zero it, so a vid_restart remakes it")
    return errs


def check_stages():
    errs = []
    seen = 0
    for p in sorted((ROOT / "engine/shaders").glob("*")):
        if p.is_dir():
            continue
        t = strip(p.read_text(errors="replace"))
        for m in re.finditer(r"\buniform\s+(?:\w+\s+)*?(\w+)\s+u_dlshadowmap\b", t):
            seen += 1
            if m.group(1) != "samplerCubeArrayShadow":
                errs.append("%s declares u_dlshadowmap as %s; the placeholder is a "
                            "samplerCubeArrayShadow's texture" % (p.name, m.group(1)))
    if seen < 3:
        errs.append("expected u_dlshadowmap in sworld_frag, sworld_opaque_frag and salias_frag, found %d" % seen)
    return errs


# The seam itself, and the SDL_GPU backend it hands binds to.
NOT_SITES = {"engine/h2shared/gl_texbind.c", "engine/h2shared/gl_texbind.h"}


def check_others():
    errs = []
    for d in ("engine/h2shared", "engine/hexen2", "engine/hexenworld"):
        for p in sorted((ROOT / d).glob("*.[ch]")):
            rel = str(p.relative_to(ROOT))
            if rel == SHADOW_C or rel in NOT_SITES or p.name.startswith("rg_"):
                continue
            t = strip(p.read_text(errors="replace"))
            for name in ("R_BindTextureTarget", "R_BindTextureForEdit", "glBindTexture_fp", "glBindTexture"):
                for _, a in calls(t, name):
                    if "GL_TEXTURE_CUBE_MAP_ARRAY" in a or (a and a[0] == "DLSHADOW_TMU"):
                        errs.append("%s: %s (%s) -- only gl_shadow.c binds a cube array or DLSHADOW_TMU"
                                    % (rel, name, ", ".join(a)))
    return errs


FIXED_BIND = (r"R_BindTextureTarget\s*\(\s*DLSHADOW_TMU\s*,\s*GL_TEXTURE_CUBE_MAP_ARRAY\s*,"
              r"\s*sh_frame_maps\s*>\s*0\s*\?\s*sh_tex\s*:\s*SH_GPU\s*\(\s*\)\s*\?\s*0\s*:"
              r"\s*SH_NoneTexture\s*\(\s*\)\s*\)\s*;")


def self_test(code):
    """The checks have to fail what they exist for and pass what is harmless."""
    errors = []
    m = re.search(FIXED_BIND, code)
    if not m:
        return ["self-test: cannot find the fixed bind to mutate"]

    def with_bind(text):
        return code[:m.start()] + text + code[m.end():]

    must_fail = {
        "the pre-#526 conditional bind":
            "if (sh_frame_maps > 0)\n\t\tR_BindTextureTarget (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex);",
        "a conditional placeholder bind": "if (sh_tex)\n\t\t" + code[m.start():m.end()],
        "sh_none bound under -gpu":
            "R_BindTextureTarget (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY, sh_frame_maps > 0 ? sh_tex : SH_NoneTexture ());",
        "nothing bound on GL without maps":
            "R_BindTextureTarget (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY, sh_frame_maps > 0 ? sh_tex : 0);",
        "the placeholder bound with maps":
            "R_BindTextureTarget (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY, SH_GPU () ? 0 : SH_NoneTexture ());",
    }
    for what, text in must_fail.items():
        if not check_draw(with_bind(text)):
            errors.append("self-test: %s passed check_draw" % what)
    must_pass = {
        "`sh_frame_maps ? ...`":
            "R_BindTextureTarget (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY,\n"
            "\t\t\t     sh_frame_maps ? sh_tex : SH_GPU () ? 0 : SH_NoneTexture ());",
        "the value hoisted into a local":
            "GLuint tex = sh_frame_maps > 0 ? sh_tex : (SH_GPU () ? 0 : SH_NoneTexture ());\n"
            "\tR_BindTextureTarget (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY, tex);",
        "the cases spelled the other way round":
            "R_BindTextureTarget (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY,\n"
            "\t\t\t     !sh_frame_maps && !SH_GPU () ? SH_NoneTexture () : (sh_frame_maps ? sh_tex : 0));",
    }
    for what, text in must_pass.items():
        e = check_draw(with_bind(text))
        if e:
            errors.append("self-test: harmless rewrite %s failed check_draw: %s" % (what, e[0]))

    def sub(rx, repl):
        out = re.sub(rx, repl, code, count=1)
        return None if out == code else out

    img = r"(R_TexImage3D\s*\(\s*GL_TEXTURE_CUBE_MAP_ARRAY\s*,\s*sh_none\s*,\s*0\s*,\s*)"
    none_mutants = {
        "a placeholder without its compare":
            sub(r"R_TexParameteri\s*\(\s*GL_TEXTURE_CUBE_MAP_ARRAY\s*,\s*sh_none\s*,\s*"
                r"GL_TEXTURE_COMPARE_MODE\s*,\s*GL_COMPARE_REF_TO_TEXTURE\s*\)\s*;", ""),
        "a placeholder without its compare func":
            sub(r"R_TexParameteri\s*\(\s*GL_TEXTURE_CUBE_MAP_ARRAY\s*,\s*sh_none\s*,\s*"
                r"GL_TEXTURE_COMPARE_FUNC\s*,\s*\w+\s*\)\s*;", ""),
        "a placeholder without its base level":
            sub(r"R_TexParameteri\s*\(\s*GL_TEXTURE_CUBE_MAP_ARRAY\s*,\s*sh_none\s*,\s*"
                r"GL_TEXTURE_BASE_LEVEL\s*,\s*0\s*\)\s*;", ""),
        "a colour-format placeholder": sub(img + r"GL_DEPTH_COMPONENT\w*", r"\1GL_RGBA8"),
        "a non-square placeholder (1, 2, 6)": sub(img + r"(\w+\s*,\s*)1\s*,\s*1\s*,\s*6", r"\1\g<2>1, 2, 6"),
        "a placeholder of 4 layers": sub(img + r"(\w+\s*,\s*)1\s*,\s*1\s*,\s*6", r"\1\g<2>1, 1, 4"),
        "a placeholder with undefined texels":
            sub(img + r"([^;]*GL_UNSIGNED_SHORT\s*,\s*)\w+(\s*\)\s*;)", r"\1\2NULL\3"),
    }
    for what, mutant in none_mutants.items():
        if mutant is None:
            errors.append("self-test: cannot build the mutant: %s" % what)
        elif not check_none(mutant):
            errors.append("self-test: %s passed check_none" % what)
    return errors


def main():
    code = strip(read(SHADOW_C))
    errors = check_draw(code) + check_none(code) + check_stages() + check_others() + self_test(code)
    if errors:
        for e in errors:
            print("FAIL:", e)
        return 1
    if VERBOSE:
        print("R_Shadow_Draw binds DLSHADOW_TMU every frame (sh_tex / SH_NoneTexture () / 0 under -gpu); "
              "SH_NoneTexture is a square far-depth cube array with the compare on; "
              "the mutants fail and the rewrites pass")
    print("OK: DLSHADOW_TMU always holds a depth cube array (#526)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
