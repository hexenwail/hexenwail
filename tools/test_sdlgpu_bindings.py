#!/usr/bin/env python3
"""Every Vulkan SPIR-V stage numbers its resources the way SDL_GPU binds them
(GitHub #340, phase P-1a).

SDL_GPU describes a pipeline's resources by COUNT only
(SDL_GPUComputePipelineCreateInfo.num_samplers, num_readonly_storage_textures,
..., SDL_GPUShaderCreateInfo.num_samplers, ...), so its descriptor-set layouts
are positional.  SDL_gpu.h, SDL_CreateGPUComputePipeline:

    For SPIR-V shaders, use the following resource sets:
    - 0: Sampled textures, followed by read-only storage textures, followed by
      read-only storage buffers
    - 1: Read-write storage textures, followed by read-write storage buffers
    - 2: Uniform buffers

and SDL_CreateGPUShader:

    For vertex shaders:
    - 0: Sampled textures, followed by storage textures, followed by storage
      buffers
    - 1: Uniform buffers
    For fragment shaders:
    - 2: Sampled textures, followed by storage textures, followed by storage
      buffers
    - 3: Uniform buffers

"Followed by" with nothing but counts to go on means dense: in each set the
bindings are 0..n-1 with no gap, and a class never appears after a later
class in the same set.  A shader that breaks either compiles cleanly with
glslang -V and cross-compiles cleanly with shadercross, then binds the wrong
texture, or nothing, at run time.  The four SSAO compute stages did exactly
that: their samplers sat at set 0 bindings 8.., copied from the GL unit
numbers.

What is checked, per stage, from the .vk.spv itself (the bytes SDL_GPU will
be handed) rather than from the GLSL: binding numbers there come through
layout.inc macros, per-stage #defines (OIT_TEX0, LIGHTGRID_SET2_SLOT,
DLSHADOW_SET3_SLOT), arithmetic and #ifdef OIT twins, and a source-level
check would have to re-implement the GLSL preprocessor to see them.

  * every resource carries DescriptorSet and Binding;
  * each resource class sits in the set the stage's rule gives it;
  * within a set, bindings are dense from 0 and in class order;
  * nothing SDL_GPU cannot bind: separate images or samplers, texel
    buffers, subpass inputs, resource arrays, push constants, and (graphics) writable storage -- SDL_GPU's
    graphics stages bind storage read-only.

It also lists (without failing) compute storage images declared neither
readonly nor writeonly -- read and written in one dispatch, which SDL_GPU
only allows on a texture created with COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE.

And it holds every graphics stage's read-only storage buffers to the
backend's stand-in (#340 P2a): until P5 routes them, rg_frame.c binds each
such slot to one zeroed buffer of RG_SSBO_STANDIN_BYTES (rg_internal.h),
and Vulkan faults a draw whose buffer is smaller than the block's fixed part
-- every member before a trailing runtime array, laid out by the SPIR-V's
own Offset / ArrayStride / MatrixStride decorations.

Usage:  test_sdlgpu_bindings.py SPIRV_DIR [--sources VK_DIR] [-v]

SPIRV_DIR is CompileSpirv.cmake's output (<build>/engine/generated/spirv);
only *.vk.spv is read.  --sources names EmbedShaders.cmake's VK_DIR, and
fails the run unless every stage there has its .vk.spv, so a gate that
compiled nothing cannot pass.  Stock python3 only: the SPIR-V is parsed here,
no spirv-cross or spirv-reflect needed.
"""
from pathlib import Path
import argparse
import re
import struct
import sys

MAGIC = 0x07230203

# Opcodes
OP_NAME = 5
OP_ENTRY_POINT = 15
OP_TYPE_INT = 21
OP_TYPE_FLOAT = 22
OP_TYPE_VECTOR = 23
OP_TYPE_MATRIX = 24
OP_CONSTANT = 43
OP_TYPE_IMAGE = 25
OP_TYPE_SAMPLER = 26
OP_TYPE_SAMPLED_IMAGE = 27
OP_TYPE_ARRAY = 28
OP_TYPE_RUNTIME_ARRAY = 29
OP_TYPE_STRUCT = 30
OP_TYPE_POINTER = 32
OP_VARIABLE = 59
OP_DECORATE = 71
OP_MEMBER_DECORATE = 72

# Decorations
DEC_BLOCK = 2
DEC_BUFFER_BLOCK = 3
DEC_ARRAY_STRIDE = 6
DEC_MATRIX_STRIDE = 7
DEC_OFFSET = 35
DEC_NON_WRITABLE = 24
DEC_NON_READABLE = 25
DEC_BINDING = 33
DEC_DESCRIPTOR_SET = 34

# OpTypeImage Dim
DIM_BUFFER = 5
DIM_SUBPASS = 6

# Storage classes
SC_UNIFORM_CONSTANT = 0
SC_UNIFORM = 2
SC_PUSH_CONSTANT = 9
SC_STORAGE_BUFFER = 12

# Execution models
EXEC_MODELS = {0: "vertex", 4: "fragment", 5: "compute"}

# Resource classes, in SDL_GPU's within-set order.
SAMPLED, RO_STORAGE_TEX, RO_STORAGE_BUF, RW_STORAGE_TEX, RW_STORAGE_BUF, UBO = range(6)
CLASS_NAMES = {
    SAMPLED: "sampled texture",
    RO_STORAGE_TEX: "read-only storage texture",
    RO_STORAGE_BUF: "read-only storage buffer",
    RW_STORAGE_TEX: "read-write storage texture",
    RW_STORAGE_BUF: "read-write storage buffer",
    UBO: "uniform buffer",
}

# stage -> {set: [classes in order]}.  A graphics stage's storage is
# read-only by definition, so only the RO classes appear there; a writable
# graphics resource is reported separately.
RULES = {
    "compute": {
        0: [SAMPLED, RO_STORAGE_TEX, RO_STORAGE_BUF],
        1: [RW_STORAGE_TEX, RW_STORAGE_BUF],
        2: [UBO],
    },
    "vertex": {
        0: [SAMPLED, RO_STORAGE_TEX, RO_STORAGE_BUF],
        1: [UBO],
    },
    "fragment": {
        2: [SAMPLED, RO_STORAGE_TEX, RO_STORAGE_BUF],
        3: [UBO],
    },
}


class Resource:
    def __init__(self, name, cls, dset, binding, both_rw=False):
        self.name, self.cls, self.set, self.binding = name, cls, dset, binding
        self.both_rw = both_rw


class SpirvError(Exception):
    pass


def parse(data):
    """Return (stage, [Resource], [problem]) for one SPIR-V module."""
    if len(data) % 4 or len(data) < 20:
        raise SpirvError("not a SPIR-V module (length)")
    endian = "<" if struct.unpack_from("<I", data)[0] == MAGIC else ">"
    words = struct.unpack(endian + "%dI" % (len(data) // 4), data)
    if words[0] != MAGIC:
        raise SpirvError("not a SPIR-V module (magic)")

    names, decos, member_decos = {}, {}, {}
    types, variables, models = {}, [], []
    i = 5
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        if wc == 0 or i + wc > len(words):
            raise SpirvError("truncated instruction at word %d" % i)
        ops = words[i + 1:i + wc]
        if op == OP_NAME:
            # SPIR-V strings are little-endian within each word.
            names[ops[0]] = bytes(struct.pack("<%dI" % (len(ops) - 1), *ops[1:])) \
                .split(b"\0", 1)[0].decode("utf-8", "replace")
        elif op == OP_ENTRY_POINT:
            models.append(ops[0])
        elif op == OP_DECORATE:
            decos.setdefault(ops[0], {})[ops[1]] = ops[2:]
        elif op == OP_MEMBER_DECORATE:
            member_decos.setdefault(ops[0], {}).setdefault(ops[1], {})[ops[2]] = ops[3:]
        elif op == OP_TYPE_IMAGE:
            # result, sampled type, dim, depth, arrayed, ms, sampled, format
            types[ops[0]] = ("image", ops[6], ops[2])
        elif op == OP_TYPE_SAMPLER:
            types[ops[0]] = ("sampler",)
        elif op == OP_TYPE_SAMPLED_IMAGE:
            types[ops[0]] = ("sampled_image", ops[1])
        elif op in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
            types[ops[0]] = ("array", ops[1])
        elif op == OP_TYPE_STRUCT:
            types[ops[0]] = ("struct", len(ops) - 1)
        elif op == OP_TYPE_POINTER:
            types[ops[0]] = ("pointer", ops[1], ops[2])
        elif op == OP_VARIABLE:
            variables.append((ops[0], ops[1], ops[2]))
        i += wc

    if len(models) != 1 or models[0] not in EXEC_MODELS:
        raise SpirvError("expected one vertex, fragment or compute entry point, got %r" % models)
    stage = EXEC_MODELS[models[0]]

    resources, problems = [], []
    for ptr_type, var, storage in variables:
        if storage not in (SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER, SC_PUSH_CONSTANT):
            continue
        pointee = types.get(ptr_type, (None, None, None))[2]
        # An anonymous block instance has an empty OpName; its block name is
        # on the struct type.
        name = names.get(var) or names.get(pointee) or "%%%d" % var
        if storage == SC_PUSH_CONSTANT:
            problems.append("%s: push constant (SDL_GPU has only uniform buffers)" % name)
            continue
        if types.get(pointee, ("",))[0] == "array":
            problems.append("%s: resource array (SDL_GPU binds one resource per slot)" % name)
            pointee = types[pointee][1]
            name = names.get(var) or names.get(pointee) or name
        vdec = decos.get(var, {})
        kind = types.get(pointee, ("?",))
        cls, both_rw = None, False
        if storage == SC_UNIFORM_CONSTANT:
            img = types.get(kind[1], ()) if kind[0] == "sampled_image" else kind
            if img[:1] == ("image",) and img[2] in (DIM_BUFFER, DIM_SUBPASS):
                problems.append("%s: %s" % (name, "texel buffer (SDL_GPU has no texel-buffer binding)"
                                            if img[2] == DIM_BUFFER else
                                            "subpass input (SDL_GPU has no input attachments)"))
                continue
            if kind[0] == "sampled_image":
                cls = SAMPLED
            elif kind[0] == "image" and kind[1] == 2 and DEC_NON_WRITABLE in vdec:
                # SDL_gpu_vulkan.c writes a read-only storage texture as a
                # VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ("because shaders are
                # stupid"), which a storage image does not match (#340 P5f).
                problems.append("%s: readonly storage image (SDL_GPU binds a read-only storage "
                                "texture as a sampled image: declare it texture2D, "
                                "GL_EXT_samplerless_texture_functions)" % name)
                continue
            elif kind[0] == "image" and kind[1] == 2:
                cls = RW_STORAGE_TEX
                both_rw = DEC_NON_READABLE not in vdec
            elif kind[0] == "image" and kind[1] == 1:
                # A texture with no sampler, read with texelFetch: what SDL_GPU
                # calls a read-only storage texture (#340 P5f).
                cls = RO_STORAGE_TEX
            elif kind[0] == "image":
                problems.append("%s: separate texture of unknown sampling kind" % name)
                continue
            elif kind[0] == "sampler":
                problems.append("%s: separate sampler (SDL_GPU binds combined texture-samplers)" % name)
                continue
            else:
                problems.append("%s: unrecognised uniform-constant type" % name)
                continue
        else:
            sdec = decos.get(pointee, {})
            if kind[0] != "struct":
                problems.append("%s: buffer variable whose type is not a struct" % name)
                continue
            is_ssbo = storage == SC_STORAGE_BUFFER or DEC_BUFFER_BLOCK in sdec
            if not is_ssbo and DEC_BLOCK not in sdec:
                problems.append("%s: uniform struct with neither Block nor BufferBlock" % name)
                continue
            if is_ssbo:
                members = member_decos.get(pointee, {})
                ro = DEC_NON_WRITABLE in vdec or (
                    kind[1] > 0 and all(DEC_NON_WRITABLE in members.get(m, {}) for m in range(kind[1])))
                cls = RO_STORAGE_BUF if ro else RW_STORAGE_BUF
            else:
                cls = UBO
        if DEC_DESCRIPTOR_SET not in vdec or DEC_BINDING not in vdec:
            problems.append("%s: %s without DescriptorSet and Binding" % (name, CLASS_NAMES[cls]))
            continue
        resources.append(Resource(name, cls, vdec[DEC_DESCRIPTOR_SET][0], vdec[DEC_BINDING][0], both_rw))
    return stage, resources, problems


def _words(data):
    endian = "<" if struct.unpack_from("<I", data)[0] == MAGIC else ">"
    return struct.unpack(endian + "%dI" % (len(data) // 4), data)


def storage_min_sizes(data):
    """[(name, bytes)] for each read-only storage buffer of a graphics stage:
    the size of the block's fixed part, which is the least a bound buffer
    may hold (a trailing runtime array may be empty)."""
    words = _words(data)
    names, decos, mdecos, types, consts, variables, models = {}, {}, {}, {}, {}, [], []
    i = 5
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        if wc == 0 or i + wc > len(words):
            raise SpirvError("truncated instruction at word %d" % i)
        ops = words[i + 1:i + wc]
        if op == OP_NAME:
            names[ops[0]] = bytes(struct.pack("<%dI" % (len(ops) - 1), *ops[1:])) \
                .split(b"\0", 1)[0].decode("utf-8", "replace")
        elif op == OP_ENTRY_POINT:
            models.append(ops[0])
        elif op == OP_DECORATE:
            decos.setdefault(ops[0], {})[ops[1]] = ops[2:]
        elif op == OP_MEMBER_DECORATE:
            mdecos.setdefault(ops[0], {}).setdefault(ops[1], {})[ops[2]] = ops[3:]
        elif op in (OP_TYPE_INT, OP_TYPE_FLOAT):
            types[ops[0]] = ("scalar", ops[1] // 8)
        elif op == OP_TYPE_VECTOR:
            types[ops[0]] = ("vector", ops[1], ops[2])
        elif op == OP_TYPE_MATRIX:
            types[ops[0]] = ("matrix", ops[1], ops[2])
        elif op == OP_TYPE_ARRAY:
            types[ops[0]] = ("array", ops[1], ops[2])
        elif op == OP_TYPE_RUNTIME_ARRAY:
            types[ops[0]] = ("runtime", ops[1])
        elif op == OP_TYPE_STRUCT:
            types[ops[0]] = ("struct", list(ops[1:]))
        elif op == OP_TYPE_POINTER:
            types[ops[0]] = ("pointer", ops[1], ops[2])
        elif op == OP_CONSTANT and len(ops) >= 3:
            consts[ops[1]] = ops[2]
        elif op == OP_VARIABLE:
            variables.append((ops[0], ops[1], ops[2]))
        i += wc
    if len(models) != 1 or EXEC_MODELS.get(models[0]) == "compute":
        return []

    def size(t, mstride=None):
        k = types.get(t, ("?",))
        if k[0] == "scalar":
            return k[1]
        if k[0] == "vector":
            return k[2] * size(k[1])
        if k[0] == "matrix":
            return k[2] * (mstride if mstride else size(k[1]))
        if k[0] == "array":
            stride = decos.get(t, {}).get(DEC_ARRAY_STRIDE, [None])[0]
            n = consts.get(k[2])
            if stride is None or n is None:
                raise SpirvError("array %%%d without ArrayStride or a constant length" % t)
            return n * stride
        if k[0] == "runtime":
            return 0
        if k[0] == "struct":
            end = 0
            for m, mt in enumerate(k[1]):
                md = mdecos.get(t, {}).get(m, {})
                if DEC_OFFSET not in md:
                    raise SpirvError("struct %%%d member %d has no Offset" % (t, m))
                end = max(end, md[DEC_OFFSET][0] + size(mt, md.get(DEC_MATRIX_STRIDE, [None])[0]))
            return end
        raise SpirvError("type %%%d has no size here" % t)

    out = []
    for ptr_type, var, storage in variables:
        if storage not in (SC_UNIFORM, SC_STORAGE_BUFFER):
            continue
        pointee = types.get(ptr_type, (None, None, None))[2]
        sdec = decos.get(pointee, {})
        if types.get(pointee, ("",))[0] != "struct":
            continue
        if storage == SC_STORAGE_BUFFER or DEC_BUFFER_BLOCK in sdec:
            out.append((names.get(var) or names.get(pointee) or "%%%d" % var, size(pointee)))
    return out


def standin_bytes(root):
    m = re.search(r"^#define RG_SSBO_STANDIN_BYTES\s+(\d+)\b",
                  (root / "engine/h2shared/rg_internal.h").read_text(), re.M)
    return int(m.group(1)) if m else None


def check(stage, resources):
    """Return the rule violations for one stage's resources."""
    rule = RULES[stage]
    home = {c: s for s, classes in rule.items() for c in classes}
    errors = []
    by_set = {}
    for r in resources:
        if stage != "compute" and r.cls in (RW_STORAGE_TEX, RW_STORAGE_BUF):
            errors.append("%s: %s in a %s stage (SDL_GPU graphics storage is read-only)"
                          % (r.name, CLASS_NAMES[r.cls], stage))
            continue
        want = home[r.cls]
        if r.set != want:
            errors.append("%s: %s in set %d, SDL_GPU %s puts it in set %d"
                          % (r.name, CLASS_NAMES[r.cls], r.set, stage, want))
            continue
        by_set.setdefault(r.set, []).append(r)
    for dset, members in sorted(by_set.items()):
        order = rule[dset]
        members.sort(key=lambda r: (r.binding, order.index(r.cls)))
        # The binding SDL_GPU will give each resource: earlier classes first,
        # then this class in binding order.
        expected = {}
        n = 0
        for c in order:
            for r in sorted((m for m in members if m.cls == c), key=lambda m: m.binding):
                expected[id(r)] = n
                n += 1
        for r in members:
            if r.binding != expected[id(r)]:
                errors.append("%s: %s at set %d binding %d, SDL_GPU binds it at %d "
                              "(set %d is %s, dense from 0)"
                              % (r.name, CLASS_NAMES[r.cls], dset, r.binding, expected[id(r)], dset,
                                 ", then ".join(CLASS_NAMES[c] + "s" for c in order)))
    return errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("spirv_dir", type=Path)
    ap.add_argument("--sources", type=Path,
                    help="EmbedShaders VK_DIR: every stage there must have its .vk.spv")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    files = sorted(args.spirv_dir.glob("*.vk.spv"))
    failures = []
    if not files:
        failures.append("%s: no *.vk.spv -- run the shaders_spirv target first" % args.spirv_dir)
    if args.sources is not None:
        stages = sorted(p.name for p in args.sources.iterdir()
                        if p.suffix in (".vert", ".frag", ".comp"))
        if not stages:
            failures.append("%s: no shader sources" % args.sources)
        have = {f.name[:-len(".vk.spv")] for f in files}
        for s in stages:
            if s not in have:
                failures.append("%s: no %s.vk.spv in %s" % (s, s, args.spirv_dir))

    simultaneous = []
    standin = standin_bytes(Path(__file__).resolve().parents[1])
    biggest = (0, None)
    if standin is None:
        failures.append("engine/h2shared/rg_internal.h: no RG_SSBO_STANDIN_BYTES")
    for f in files:
        label = f.name[:-len(".vk.spv")]
        try:
            stage, resources, problems = parse(f.read_bytes())
        except SpirvError as e:
            failures.append("%s: %s" % (label, e))
            continue
        errs = problems + check(stage, resources)
        try:
            for name, need in storage_min_sizes(f.read_bytes()):
                biggest = max(biggest, (need, "%s's %s" % (label, name)))
                if standin is not None and need > standin:
                    errs.append("%s: read-only storage buffer needs %d bytes, the backend's stand-in "
                                "(RG_SSBO_STANDIN_BYTES) has %d" % (name, need, standin))
        except SpirvError as e:
            errs.append("storage-buffer size: %s" % e)
        failures += ["%s (%s): %s" % (label, stage, e) for e in errs]
        if stage == "compute":
            simultaneous += ["%s: %s" % (label, r.name) for r in resources if r.both_rw]
        if args.verbose:
            summary = ", ".join("%s=%d.%d" % (r.name, r.set, r.binding)
                                for r in sorted(resources, key=lambda r: (r.set, r.binding)))
            print("%-28s %-8s %s%s" % (label, stage, summary or "(no resources)",
                                       "  FAIL" if errs else ""))

    for s in simultaneous:
        print("note: %s is read and written in one dispatch; its SDL_GPU texture needs "
              "COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE" % s)
    if failures:
        for e in failures:
            print("FAIL: " + e, file=sys.stderr)
        print("%d SDL_GPU binding violation(s) in %d stage(s)" % (len(failures), len(files)),
              file=sys.stderr)
        return 1
    print("SDL_GPU bindings OK: %d Vulkan SPIR-V stages; the largest graphics storage block is %s, "
          "%d of the stand-in's %d bytes" % (len(files), biggest[1], biggest[0], standin))
    return 0


if __name__ == "__main__":
    sys.exit(main())
