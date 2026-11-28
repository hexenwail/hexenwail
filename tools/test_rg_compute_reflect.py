#!/usr/bin/env python3
"""The SDL_GPU backend reads every compute stage's resources off the real
SPIR-V the way the GL side binds them (GitHub #340, phase P5c).

A compute dispatch under -gpu binds its read-write storage buffers by slot,
from the GL binding point each block's NAME maps to (rg_shader.c
rg_compute_points, RG_ComputeStoragePoint), and its read-only ones and
samplers in set 0 after them; RG_CheckStage holds what it found to the
reflection row's counts and to SDL_GPU's dense numbering.  None of that had
run on a blob glslang made: tools/test_rg_sampler_units.py feeds RG_Reflect
SPIR-V assembled by hand (samplers only), and tools/test_sdlgpu_bindings.py
checks the blobs' numbering in Python.  The world cull (P5c) is the first
dispatch with read-write buffers, four of them beside four read-only ones,
so a slip in set 1 -- a block at the wrong slot, a name that maps to no GL
point, the slots counted from the wrong base -- binds the dedup buffer as
the indirect one and the world goes missing with no validation error.

So this compiles engine/shaders' compute stages as the build does
(EmbedShaders.cmake's expansion, then glslangValidator -V --target-env
vulkan1.0, CompileSpirv.cmake's flags), lifts RG_Reflect, RG_CheckStage and
the storage-point tables out of rg_shader.c with a stub prelude, compiles
that with cc, and runs it on each blob against its row of
engine/h2shared/gl_progid_refl.h:

  * every compute row's blob passes RG_CheckStage;
  * each read-write and read-only storage block's GL point is the `gl`
    number its SSBO_RW / SSBO_RO declaration gives in the source, and its
    slot the `vk` one (counted past the storage images for set 1, past the
    samplers for set 0);
  * cull_mark: IndirectBuffer 3 / DstIndexBuffer 5 / DedupBuffer 6 /
    StatsBuffer 7 at slots 0-3, SurfaceBuffer 0 / MarkSurfBuffer 1 /
    VisBuffer 2 / SrcIndexBuffer 4 at slots 0-3, u_hiz_pyramid a read-only
    storage texture at slot 0 with no unit until R_ProgramSamplerUnit names
    one (its Vulkan arm reads with texelFetch and no sampler, #340 P5f), no
    sampler, CullParams at uniform slot 0; hiz_reduce: u_src the same, at
    slot 0; cull_clear: IndirectBuffer 3 at slot 0;
  * RG_CheckStage REFUSES cull_mark with DstIndexBuffer's binding moved
    onto IndirectBuffer's (two blocks at one slot), with a read-write block
    renamed to one no table maps (point -1 is RG_Buf_Storage's "nothing
    bound": the dispatch would be dropped every frame, so the reflection is
    held to name a point), and against a row with one buffer fewer.

Needs cmake, glslangValidator and cc (ci.yml runs it under `nix develop`).

  --spirv-dir DIR   use a build's <blob>.vk.spv (shaders-spirv.yml) instead
                    of compiling, with --vk-dir its expanded sources

Usage: test_rg_compute_reflect.py [--root DIR] [--spirv-dir DIR --vk-dir DIR] [-v]
"""
from pathlib import Path
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
errors = []


def fail(msg):
    errors.append(msg)


def strip_c(text):
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)


def lift(code, head_rx, what):
    """The definition starting at head_rx (a line-anchored regex) through its
    closing brace and semicolon or brace at column 0."""
    m = re.search(head_rx, code, re.M)
    if not m:
        fail("cannot lift %s" % what)
        return ""
    end = re.compile(r"^\}[^\n]*$", re.M).search(code, m.end())
    if not end:
        fail("cannot find the end of %s" % what)
        return ""
    return code[m.start():end.end()]


PRELUDE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t Uint8; typedef int8_t Sint8; typedef uint32_t Uint32; typedef uint16_t Uint16;
typedef int qboolean;
#define true 1
#define false 0
typedef unsigned int GLuint; typedef int GLint; typedef unsigned int GLenum;
#define MAX_OSPATH 1024
typedef int rprog_id_t;
static void Con_Printf (const char *fmt, ...) { printf ("CON %s", fmt); }
static void Sys_Error (const char *fmt, ...) { fprintf (stderr, "Sys_Error %s\n", fmt); exit (3); }
static size_t q_strlcpy (char *d, const char *s, size_t n) { snprintf (d, n, "%s", s); return strlen (s); }
#define q_max(a, b) ((a) > (b) ? (a) : (b))
#define q_min(a, b) ((a) < (b) ? (a) : (b))
"""

HARNESS = r"""
static rprog_refl_t	the_row;
static const char	*the_blob;
static const rprog_refl_t *R_Program_Reflection (rprog_id_t id, int stage) { (void) id; (void) stage; return &the_row; }
static const char *R_Program_Blob (rprog_id_t id, int stage) { (void) id; (void) stage; return the_blob; }
static char		rg_spvsource[MAX_OSPATH + 32] = "the test";
static qboolean		rg_spv_fromdir = false;
@CHECK@

int main (int argc, char **argv)
{
	static rgstage_t	st;
	FILE			*f;
	long			sz;
	void			*code;
	int			i;

	/* argv: blob name, the spv, then the row's counts */
	if (argc != 9)
		return 2;
	the_blob = argv[1];
	f = fopen (argv[2], "rb");
	if (!f)
		return 4;
	fseek (f, 0, SEEK_END); sz = ftell (f); fseek (f, 0, SEEK_SET);
	code = malloc (sz);
	if (fread (code, 1, sz, f) != (size_t) sz)
		return 4;
	fclose (f);
	the_row.blob = argv[1];
	the_row.stage = RPROG_REFL_COMPUTE;
	the_row.samplers = (unsigned char) atoi (argv[3]);
	the_row.ro_storage_textures = (unsigned char) atoi (argv[4]);
	the_row.ro_storage_buffers = (unsigned char) atoi (argv[5]);
	the_row.rw_storage_textures = (unsigned char) atoi (argv[6]);
	the_row.rw_storage_buffers = (unsigned char) atoi (argv[7]);
	the_row.uniform_buffers = (unsigned char) atoi (argv[8]);
	/* RG_MakeCompute's sets: 0 samplers and read-only storage, 1 read-write, 2 uniforms */
	if (!RG_CheckStage (0, RPROG_STAGE_COMPUTE, code, (size_t) sz, 0, 2, 1, &st))
	{
		puts ("CHECK-FAIL");
		return 0;
	}
	puts ("CHECK-OK");
	for (i = 0; i < st.num_samplers; i++)
		printf ("SAMPLER %s binding=%d unit=%d\n", st.samplers[i].name, st.samplers[i].binding,
			st.samplers[i].unit);
	for (i = 0; i < st.num_ssbos; i++)
		printf ("RO %s slot=%d point=%d\n", st.ssbos[i].name, st.ssbos[i].slot, st.ssbos[i].point);
	for (i = 0; i < st.num_rotex; i++)
		printf ("ROTEX %s binding=%d unit=%d\n", st.rotex[i].name, st.rotex[i].binding, st.rotex[i].unit);
	for (i = 0; i < st.num_images; i++)
		printf ("IMAGE %s binding=%d\n", st.images[i].name, st.images[i].binding);
	for (i = 0; i < st.num_rwssbos; i++)
		printf ("RW %s slot=%d point=%d\n", st.rwssbos[i].name, st.rwssbos[i].slot, st.rwssbos[i].point);
	for (i = 0; i < st.num_ubos; i++)
		printf ("UBO %s binding=%d\n", st.ubos[i].name, st.ubos[i].binding);
	return 0;
}
"""

REFL_ROW = re.compile(r'\{\s*"([a-z0-9_]+)\.comp",\s*RPROG_REFL_COMPUTE,\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),'
                      r'\s*(\d+),\s*(\d+),')
SSBO_DECL = re.compile(r"\bSSBO_(RO|RW)\s*\(\s*(\w+)\s*,\s*(\d+)\s*\)[^;{]*?\bbuffer\s+(\w+)\s*\{")


def build_harness(root):
    rgi = strip_c((root / "engine/h2shared/rg_internal.h").read_text())
    rgs = strip_c((root / "engine/h2shared/rg_shader.c").read_text())
    pid = strip_c((root / "engine/h2shared/gl_progid.h").read_text())
    lch = strip_c((root / "engine/h2shared/gl_lightcluster.h").read_text())
    mdl = strip_c((root / "engine/h2shared/gl_model.h").read_text())
    parts = []
    for d in ("RG_UNIT_NONE", "RG_MAX_STAGE_SAMPLERS", "RG_MAX_STAGE_UBOS", "RG_MAX_STAGE_SSBOS",
              "RG_MAX_STAGE_IMAGES"):
        m = re.search(r"^#define %s\s+\S+" % d, rgi, re.M)
        parts.append(m.group(0) if m else fail("rg_internal.h: no %s" % d) or "")
    for d in ("RPROG_STAGE_VERTEX", "RPROG_STAGE_FRAGMENT", "RPROG_STAGE_COMPUTE", "RPROG_MAX_STAGES"):
        m = re.search(r"^#define %s\s+\S+" % d, pid, re.M)
        parts.append(m.group(0) if m else fail("gl_progid.h: no %s" % d) or "")
    m = re.search(r"^#define LIGHT_SSBO_BINDING\s+\S+", lch, re.M)
    parts.append(m.group(0) if m else fail("gl_lightcluster.h: no LIGHT_SSBO_BINDING") or "")
    m = re.search(r"typedef enum\s*\{[^{}]*\bPV_QUAKE1\b[^{}]*\}\s*poseverttype_t\s*;", mdl)
    parts.append(m.group(0) if m else fail("gl_model.h: no poseverttype_t") or "")
    for t in ("rg_textype_t", "rg_sampclass_t", "rg_ssbokind_t"):
        m = re.search(r"typedef enum \{[^{}]*\} %s;" % t, rgi)
        parts.append(m.group(0) if m else fail("rg_internal.h: cannot lift %s" % t) or "")
    for t in ("rgsampler_t", "rgubo_t", "rgssbo_t", "rgimage_t", "rgstage_t"):
        m = re.search(r"typedef struct \{[^{}]*\} %s;" % t, rgi)
        parts.append(m.group(0) if m else fail("rg_internal.h: cannot lift %s" % t) or "")
    for t in ("rprog_refl_stage_t", "rprog_refl_t"):
        m = re.search(r"typedef (?:enum|struct) \{[^{}]*\} %s;" % t, pid)
        parts.append(m.group(0) if m else fail("gl_progid.h: cannot lift %s" % t) or "")
    tables = re.search(r"static const struct \{[^{}]*\} rg_storage_points\[\] = \{.*?\n\};", rgs, re.S)
    ctab = re.search(r"static const struct \{[^{}]*\} rg_compute_points\[\] = \{.*?\n\};", rgs, re.S)
    if not tables or not ctab:
        fail("rg_shader.c: cannot lift rg_storage_points / rg_compute_points")
        return None
    shader = "\n".join([
        lift(rgs, r"^enum \{\s*SPV_OpName", "rg_shader.c's SPIR-V enum"),
        (re.search(r"typedef struct \{[^{}]*\} spvid_t;", rgs) or re.search("$^", "")).group(0)
        if re.search(r"typedef struct \{[^{}]*\} spvid_t;", rgs) else "",
        tables.group(0), ctab.group(0),
        lift(rgs, r"^static int RG_StorageRow \(", "RG_StorageRow"),
        lift(rgs, r"^int RG_StoragePoint \(", "RG_StoragePoint"),
        lift(rgs, r"^rg_ssbokind_t RG_StorageKind \(", "RG_StorageKind"),
        lift(rgs, r"^int RG_ComputeStoragePoint \(", "RG_ComputeStoragePoint"),
        lift(rgs, r"^static rg_textype_t RG_ImageType \(", "RG_ImageType"),
        lift(rgs, r"^static qboolean RG_Reflect \(", "RG_Reflect"),
    ])
    check = lift(rgs, r"^static qboolean RG_CheckStage \(", "RG_CheckStage")
    if errors:
        return None
    return PRELUDE + "\n".join(parts) + "\n" + shader + "\n" + HARNESS.replace("@CHECK@", check)


def compile_blobs(root, td):
    """engine/shaders' compute stages through EmbedShaders.cmake and glslang
    -V, as CompileSpirv.cmake runs them; {name: (spv path, expanded source)}."""
    cmake, glslang = shutil.which("cmake"), shutil.which("glslangValidator")
    if not cmake or not glslang:
        fail("needs cmake and glslangValidator (run under `nix develop`), or --spirv-dir/--vk-dir")
        return {}
    vk = td / "vk"
    r = subprocess.run([cmake, "-DSHADER_DIR=%s" % (root / "engine/shaders"), "-DOUT_FILE=%s" % (td / "sh.h"),
                        "-DVK_DIR=%s" % vk, "-P", str(root / "engine/cmake/EmbedShaders.cmake")],
                       capture_output=True, text=True)
    if r.returncode:
        fail("EmbedShaders.cmake failed:\n" + (r.stdout + r.stderr)[-2000:])
        return {}
    out = {}
    for src in sorted(vk.glob("*.comp")):
        spv = td / (src.name + ".vk.spv")
        r = subprocess.run([glslang, "-V", "--target-env", "vulkan1.0", "-o", str(spv), str(src)],
                           capture_output=True, text=True)
        if r.returncode:
            fail("glslang -V failed for %s:\n%s" % (src.name, (r.stdout + r.stderr)[-1500:]))
            continue
        out[src.stem] = (spv, src.read_text())
    return out


def run(exe, blob, spv, row):
    r = subprocess.run([str(exe), blob + ".comp", str(spv)] + [str(c) for c in row],
                       capture_output=True, text=True)
    if r.returncode:
        fail("%s: the lifted RG_CheckStage exits %d:\n%s" % (blob, r.returncode, (r.stdout + r.stderr)[-1500:]))
        return None
    return r.stdout


def parse(out):
    got = {"ok": "CHECK-OK" in out, "RO": {}, "RW": {}, "SAMPLER": {}, "UBO": {}, "IMAGE": {}, "ROTEX": {}}
    for m in re.finditer(r"^ROTEX (\w+) binding=(\d+) unit=(\d+)$", out, re.M):
        got["ROTEX"][m.group(1)] = (int(m.group(2)), int(m.group(3)))
    for m in re.finditer(r"^(RO|RW) (\w+) slot=(\d+) point=(-?\d+)$", out, re.M):
        got[m.group(1)][m.group(2)] = (int(m.group(3)), int(m.group(4)))
    for m in re.finditer(r"^SAMPLER (\w+) binding=(\d+) unit=(\d+)$", out, re.M):
        got["SAMPLER"][m.group(1)] = (int(m.group(2)), int(m.group(3)))
    for m in re.finditer(r"^UBO (\w+) binding=(\d+)$", out, re.M):
        got["UBO"][m.group(1)] = int(m.group(2))
    for m in re.finditer(r"^IMAGE (\w+) binding=(\d+)$", out, re.M):
        got["IMAGE"][m.group(1)] = int(m.group(2))
    return got


def spv_patch_binding(data, var_name, new_binding):
    """A copy of module `data` with the Binding decoration of the variable whose
    block type is `var_name` (OpName of the struct) set to new_binding."""
    w = list(struct.unpack("<%dI" % (len(data) // 4), data))
    names, decor, var_of = {}, [], {}
    ptr_to = {}
    p = 5
    while p < len(w):
        op, wc = w[p] & 0xFFFF, w[p] >> 16
        if op == 5:	# OpName
            b = struct.pack("<%dI" % (wc - 2), *w[p + 2:p + wc])
            names[w[p + 1]] = b.split(b"\0")[0].decode()
        elif op == 71 and w[p + 2] == 33:	# OpDecorate Binding
            decor.append(p)
        elif op == 32:	# OpTypePointer
            ptr_to[w[p + 1]] = w[p + 3]
        elif op == 59:	# OpVariable
            var_of[w[p + 2]] = w[p + 1]
        p += wc
    target = [v for v, t in var_of.items() if names.get(ptr_to.get(t)) == var_name]
    if len(target) != 1:
        return None
    for p in decor:
        if w[p + 1] == target[0]:
            w[p + 3] = new_binding
            return struct.pack("<%dI" % len(w), *w)
    return None


def spv_rename(data, old, new):
    """A copy with OpName `old` (same length) spelled `new`."""
    assert len(old) == len(new)
    return data.replace(old.encode() + b"\0", new.encode() + b"\0", 1) if (old.encode() + b"\0") in data else None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("--spirv-dir", type=Path)
    ap.add_argument("--vk-dir", type=Path)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    root = a.root.resolve()

    rows = {m.group(1): [int(m.group(k)) for k in range(2, 8)]
            for m in REFL_ROW.finditer((root / "engine/h2shared/gl_progid_refl.h").read_text())}
    if not rows.get("cull_mark") or not rows.get("cull_clear"):
        fail("gl_progid_refl.h: no cull_mark / cull_clear compute row")
    src = build_harness(root)
    # the GL points a source spells by name
    consts = dict(re.findall(r"^#define\s+(LIGHT_SSBO_BINDING)\s+(\d+)",
                             (root / "engine/h2shared/gl_lightcluster.h").read_text(), re.M))
    checked = 0
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        if src is not None:
            (td / "h.c").write_text(src)
            r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-w", "-o", str(td / "h"),
                                str(td / "h.c")], capture_output=True, text=True)
            if r.returncode:
                fail("the lifted RG_CheckStage does not compile:\n" + r.stderr[-3000:])
                src = None
        blobs = {}
        if src is not None:
            if a.spirv_dir:
                for name in rows:
                    spv = a.spirv_dir / (name + ".comp.vk.spv")
                    vsrc = (a.vk_dir / (name + ".comp")) if a.vk_dir else None
                    if not spv.is_file() or not vsrc or not vsrc.is_file():
                        fail("%s: no %s (or its source in --vk-dir)" % (name, spv))
                        continue
                    blobs[name] = (spv, vsrc.read_text())
            else:
                blobs = compile_blobs(root, td)
        missing = sorted(set(rows) - set(blobs))
        if src is not None and missing and not errors:
            fail("compute rows with no blob: %s" % ", ".join(missing))
        results = {}
        for name in sorted(blobs):
            if name not in rows:
                fail("%s.comp compiled but has no gl_progid_refl.h row" % name)
                continue
            spv, text = blobs[name]
            out = run(td / "h", name, spv, rows[name])
            if out is None:
                continue
            if a.verbose:
                print("== %s\n%s" % (name, out))
            got = parse(out)
            results[name] = got
            checked += 1
            if not got["ok"]:
                fail("%s: RG_CheckStage refuses the real blob against its row %s:\n%s" % (name, rows[name], out))
                continue
            nsamp = rows[name][0] + rows[name][1]
            nimg = rows[name][3]
            decl = [(k, int(consts.get(gl, gl)), int(vk), blk) for k, gl, vk, blk in SSBO_DECL.findall(strip_c(text))]
            for k, gl, vk, blk in decl:
                want = (vk - (nimg if k == "RW" else nsamp), gl)
                have = got[k].get(blk)
                if have != want:
                    fail("%s: %s block %s reflects as (slot, GL point) %s, its SSBO_%s(%d, %d) says %s"
                         % (name, k, blk, have, k, gl, vk, want))
            for k in ("RO", "RW"):
                extra = set(got[k]) - {blk for kk, _, _, blk in decl if kk == k}
                if extra:
                    fail("%s: %s blocks %s reflected that no SSBO_%s declares" % (name, k, sorted(extra), k))
                for blk, (slot, point) in got[k].items():
                    if point < 0:
                        fail("%s: %s block %s maps to no GL point: RG_Buf_Storage would find nothing "
                             "bound and drop the dispatch" % (name, k, blk))

        cm = results.get("cull_mark")
        if cm and cm["ok"]:
            want_rw = {"IndirectBuffer": (0, 3), "DstIndexBuffer": (1, 5), "DedupBuffer": (2, 6),
                       "StatsBuffer": (3, 7)}
            want_ro = {"SurfaceBuffer": (0, 0), "MarkSurfBuffer": (1, 1), "VisBuffer": (2, 2),
                       "SrcIndexBuffer": (3, 4)}
            if cm["RW"] != want_rw:
                fail("cull_mark: read-write blocks (slot, GL point) %s, gl_worldcull.c binds %s"
                     % (cm["RW"], want_rw))
            if cm["RO"] != want_ro:
                fail("cull_mark: read-only blocks (slot, GL point) %s, gl_worldcull.c binds %s"
                     % (cm["RO"], want_ro))
            if cm["SAMPLER"] or cm["ROTEX"] != {"u_hiz_pyramid": (0, 0xFF)}:
                fail("cull_mark: samplers %s, read-only storage textures %s, want u_hiz_pyramid a read-only "
                     "storage texture at slot 0 with no unit until R_ProgramSamplerUnit names one, and no "
                     "sampler (#340 P5f)" % (cm["SAMPLER"], cm["ROTEX"]))
            if cm["UBO"] != {"CullParams": 0}:
                fail("cull_mark: uniform blocks %s, want CullParams at slot 0" % cm["UBO"])
        hr = results.get("hiz_reduce")
        if hr and hr["ok"] and (hr["SAMPLER"] or hr["ROTEX"] != {"u_src": (0, 0xFF)} or
                                hr["IMAGE"] != {"u_dst": 0}):
            fail("hiz_reduce: samplers %s, read-only storage textures %s, images %s, want u_src a read-only "
                 "storage texture at slot 0 and u_dst storage image 0 (#340 P5f)"
                 % (hr["SAMPLER"], hr["ROTEX"], hr["IMAGE"]))
        cc = results.get("cull_clear")
        if cc and cc["ok"] and (cc["RW"] != {"IndirectBuffer": (0, 3)} or cc["RO"] or cc["SAMPLER"]):
            fail("cull_clear: RW %s RO %s samplers %s, want IndirectBuffer alone at slot 0, GL point 3"
                 % (cc["RW"], cc["RO"], cc["SAMPLER"]))

        # what RG_CheckStage must refuse, on the real cull_mark
        if "cull_mark" in blobs and cm and cm["ok"]:
            data = Path(blobs["cull_mark"][0]).read_bytes()
            bad = {}
            two = spv_patch_binding(data, "DstIndexBuffer", 0)
            if two is None:
                fail("cull_mark: cannot find DstIndexBuffer's Binding to move")
            else:
                bad["two blocks at one set-1 slot"] = (two, rows["cull_mark"])
            ren = spv_rename(data, "DedupBuffer", "DedupBuffeX")
            if ren is None:
                fail("cull_mark: cannot find the DedupBuffer name to change")
            else:
                bad["a read-write block no table maps"] = (ren, rows["cull_mark"])
            short = list(rows["cull_mark"])
            short[4] -= 1
            bad["a row with one read-write buffer fewer"] = (data, short)
            for why, (blob, row) in bad.items():
                (td / "bad.spv").write_bytes(blob)
                out = run(td / "h", "cull_mark", td / "bad.spv", row)
                if out is None:
                    continue
                got = parse(out)
                refused = not got["ok"] or any(p < 0 for s, p in got["RW"].values())
                if not refused:
                    fail("cull_mark with %s: RG_CheckStage accepts it and maps every block to a point:\n%s"
                         % (why, out))
                elif a.verbose:
                    print("refused, as it must be: %s" % why)
    if errors:
        for e in errors:
            print("FAIL: " + e, file=sys.stderr)
        print("test_rg_compute_reflect: %d failure(s)" % len(errors), file=sys.stderr)
        return 1
    print("test_rg_compute_reflect: OK, %d compute blobs through RG_CheckStage on real SPIR-V, every storage "
          "block at its source's slot and GL point, the cull's four read-write buffers where "
          "gl_worldcull.c binds them, and three broken modules refused" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
