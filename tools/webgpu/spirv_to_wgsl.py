#!/usr/bin/env python3
"""Translate LibertyRecomp's precompiled title shaders to WGSL for WebGPU.

The stock shader cache (LibertyRecompLib/shader/shader_cache.cpp) stores
SMOL-V encoded SPIR-V written for the native Vulkan renderer. That SPIR-V uses
two features WebGPU does not have:

* Buffer device addresses. A push-constant block holds three 64-bit pointers:
  vertex constants (_m0), pixel constants (_m1) and the shared constants
  (_m2, `SharedConstants` in gta4_native/core/draw_state.h).
* Bindless descriptors. Textures and samplers are runtime arrays indexed by
  words loaded from the shared constants.

Both are rewritten here, on the GLSL that SPIRV-Cross produces:

* The three blocks become one uniform buffer: vertex constants at byte 0,
  pixel constants at 4096 and shared constants at 8192. A pointer becomes a
  byte offset into it, and `.value` becomes a typed load. The final WGSL then
  splits it into one part per block (split_uniforms.py), so the renderer can
  reuse each block on its own, and reads the parts from one storage buffer
  at register indices given by a per-draw record (draw_constants.py), with
  each stage's hot registers in a slot of their own (hot_constants.py).
* Every bindless index is a load from a constant shared-constants offset, so
  each one resolves to a fixed texture or sampler slot:
  @group(1) @binding(slot) for textures, @binding(32 + slot) for samplers.

The result is compiled back to SPIR-V with glslangValidator and translated to
WGSL with naga. Tools needed on PATH: spirv-cross, glslangValidator, spirv-opt,
naga.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from spirv_patch import scalarize_sample_mask  # noqa: E402
from split_uniforms import RewriteError, rewrite as split_uniforms  # noqa: E402
from draw_constants import rewrite as draw_constants  # noqa: E402
from hot_constants import rewrite as hot_constants  # noqa: E402

BLOCK_STRIDE = 4096
UNIFORM_WORDS = (3 * BLOCK_STRIDE) // 16  # uvec4 registers
SAMPLER_BINDING_BASE = 32

# SharedConstants descriptor arrays (byte offsets, 26 words each).
SLOTS = 26
TEXTURE_RANGES = {
    "texture2D": 0x000,
    "texture2DArray": 0x068,
    "texture3D": 0x0D0,
    "textureCube": 0x138,
}
SAMPLER_BASE = 0x1A0

POINTER_TYPES = {
    "vec4Pointer": "vec4",
    "vec2Pointer": "vec2",
    "uintPointer": "uint",
    "floatPointer": "float",
}

PRELUDE = f"""
layout(set = 0, binding = 0, std140) uniform XenosConstants {{ uvec4 w[{UNIFORM_WORDS}]; }} xc;
uint xc_u(uint o) {{ return xc.w[o >> 4u][(o >> 2u) & 3u]; }}
float xc_f(uint o) {{ return uintBitsToFloat(xc_u(o)); }}
vec2 xc_v2(uint o) {{ return vec2(xc_f(o), xc_f(o + 4u)); }}
vec4 xc_v4(uint o) {{ return uintBitsToFloat(xc.w[o >> 4u]); }}
bool xisnan(float v) {{ return (floatBitsToUint(v) & 0x7FFFFFFFu) > 0x7F800000u; }}
bvec2 xisnan(vec2 v) {{ return greaterThan(floatBitsToUint(v) & uvec2(0x7FFFFFFFu), uvec2(0x7F800000u)); }}
bvec3 xisnan(vec3 v) {{ return greaterThan(floatBitsToUint(v) & uvec3(0x7FFFFFFFu), uvec3(0x7F800000u)); }}
bvec4 xisnan(vec4 v) {{ return greaterThan(floatBitsToUint(v) & uvec4(0x7FFFFFFFu), uvec4(0x7F800000u)); }}
bool xisinf(float v) {{ return (floatBitsToUint(v) & 0x7FFFFFFFu) == 0x7F800000u; }}
bvec2 xisinf(vec2 v) {{ return equal(floatBitsToUint(v) & uvec2(0x7FFFFFFFu), uvec2(0x7F800000u)); }}
bvec3 xisinf(vec3 v) {{ return equal(floatBitsToUint(v) & uvec3(0x7FFFFFFFu), uvec3(0x7F800000u)); }}
bvec4 xisinf(vec4 v) {{ return equal(floatBitsToUint(v) & uvec4(0x7FFFFFFFu), uvec4(0x7F800000u)); }}
"""
# The pipeline specialization word (Vulkan constant_id 0) is read from the
# word after SharedConstants (0x500) instead: naga cannot translate
# OpSpecConstantOp, and a uniform keeps one WGSL module per shader.
SPEC_CONSTANT_OFFSET = 2 * BLOCK_STRIDE + 0x500
LOADERS = {"vec4": "xc_v4", "vec2": "xc_v2", "uint": "xc_u", "float": "xc_f"}


class ConversionError(Exception):
    pass


def matching_paren(text: str, open_index: int) -> int:
    depth = 0
    for i in range(open_index, len(text)):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
    raise ConversionError("unbalanced parentheses")


def evaluate_constant(expr: str) -> int | None:
    """Evaluates a rewritten offset expression made of integer literals."""
    cleaned = re.sub(r"(?<=[0-9])u", "", expr)
    if not re.fullmatch(r"[0-9+*()&<>| -]+", cleaned):
        return None
    try:
        return int(eval(cleaned, {"__builtins__": {}}))  # noqa: S307 - integer literals and operators only
    except Exception:
        return None


TERMINATORS = ("break;", "continue;", "discard;", "return;")


def _close_brace(lines: list[str], start: int, indent: str) -> int:
    for j in range(start + 1, len(lines)):
        if lines[j] == indent + "}":
            return j
    raise ConversionError("unterminated block")


def remove_switch_fallthrough(src: str) -> str:
    """Rewrites switches whose cases fall through into a guarded if-chain.

    XenosRecomp lowers Xenos control flow to a loop around `switch (pc)` where
    one exec block may fall into the next. WGSL has no fall-through. The
    chain lives in `switch (0) { default: ... }`, so `break` still leaves the
    original switch and `continue` still reaches the enclosing loop.
    """
    lines = src.split("\n")
    counter = 0
    i = 0
    while i < len(lines):
        m = re.fullmatch(r"(\s*)switch \((.+)\)", lines[i])
        if not m or i + 1 >= len(lines) or lines[i + 1] != m.group(1) + "{":
            i += 1
            continue
        ind, expr = m.group(1), m.group(2)
        end = _close_brace(lines, i + 1, ind)
        inner = ind + "    "
        groups = []  # (labels, body_lines)
        j = i + 2
        labels: list[str] = []
        while j < end:
            line = lines[j]
            lm = re.fullmatch(re.escape(inner) + r"(?:case (.+)|(default)):", line)
            if lm:
                labels.append(lm.group(1) or "default")
                j += 1
                continue
            if line == inner + "{":
                close = _close_brace(lines, j, inner)
                groups.append((labels, lines[j + 1 : close]))
                labels = []
                j = close + 1
                continue
            raise ConversionError(f"unexpected switch layout: {line.strip()[:60]}")
        if labels:
            groups.append((labels, []))
        falls = any(
            not (body and body[-1].strip() in TERMINATORS) for _, body in groups[:-1])
        if not falls:
            i += 1
            continue
        counter += 1
        sel, ft = f"xsw_sel{counter}", f"xsw_ft{counter}"
        all_labels = [l for ls, _ in groups for l in ls if l != "default"]
        unsigned = all(l.endswith("u") for l in all_labels) if all_labels else True
        out = [ind + "switch (0)", ind + "{", inner + "default:", inner + "{",
               inner + f"{'uint' if unsigned else 'int'} {sel} = {'uint' if unsigned else 'int'}({expr});",
               inner + f"bool {ft} = false;"]
        for ls, body in groups:
            terms = []
            for l in ls:
                if l == "default":
                    terms.append("!(" + " || ".join(f"{sel} == {x}" for x in all_labels) + ")"
                                 if all_labels else "true")
                else:
                    terms.append(f"{sel} == {l}")
            out.append(inner + f"if ({ft} || " + " || ".join(terms) + ")")
            out.append(inner + "{")
            out.append(inner + f"    {ft} = true;")
            out.extend(body)
            out.append(inner + "}")
        out += [inner + "}", ind + "}"]
        lines[i : end + 1] = out
        i += 1
    return "\n".join(lines)


def rewrite_glsl(src: str) -> tuple[str, dict]:
    info = {"textures": {}, "samplers": [], "attributes": [], "outputs": 0}

    # Vertex inputs use semantic locations up to 39; WebGPU allows 16. Pack
    # them densely and record the semantic location of each attribute.
    if "gl_Position" in src:
        def remap(mm):
            if mm.group(2) != "vec4":
                raise ConversionError(f"unsupported vertex input type {mm.group(2)}")
            info["attributes"].append(int(mm.group(1)))
            return f"layout(location = {len(info['attributes']) - 1}) in vec4 "
        src = re.sub(r"layout\(location = (\d+)\) in (\w+) ", remap, src)
    else:
        for om in re.finditer(r"layout\(location = (\d+)\) out ", src):
            info["outputs"] |= 1 << int(om.group(1))

    # Push-constant block -> byte offsets of the merged uniform buffer.
    m = re.search(
        r"layout\(push_constant, std430\) uniform (\w+)\s*\{\s*uint64_t _m0;\s*uint64_t _m1;\s*"
        r"uint64_t _m2;\s*\}\s*(\w+);\n",
        src,
    )
    if not m:
        raise ConversionError("push-constant layout not recognized")
    inst = m.group(2)
    src = src[: m.start()] + src[m.end():]
    for k in range(3):
        src = src.replace(f"{inst}._m{k}", f"{k * BLOCK_STRIDE}u")

    # Pointer struct declarations (newer SPIRV-Cross also forward-declares them).
    src = re.sub(
        r"layout\(buffer_reference[^)]*\) buffer (\w+Pointer)\s*\{[^}]*\};\n", "", src
    )
    src = re.sub(r"layout\(buffer_reference[^)]*\) buffer \w+Pointer;\n", "", src)
    # 64-bit values are only buffer offsets (< 12 KB) and 32-bit boolean masks,
    # so they narrow to 32 bits.
    src = re.sub(r"\b(\d+)ul\b", r"\1u", src)
    src = re.sub(r"\b(\d+)l\b", r"\1", src)
    src = re.sub(r"\buint64_t\b", "uint", src)
    src = re.sub(r"\bint64_t\b", "int", src)
    src = re.sub(r"\bu64vec([234])\b", r"uvec\1", src)
    src = re.sub(r"\bi64vec([234])\b", r"ivec\1", src)

    # Scalars used to build offsets, for constant propagation.
    scalars: dict[str, str] = {}
    for sm in re.finditer(r"\b(?:uint|int) (_\w+) = ([^;]+);", src):
        scalars.setdefault(sm.group(1), sm.group(2))

    def constant_value(expr: str, depth: int = 0) -> int | None:
        if depth > 16:
            return None
        def sub(mm):
            v = scalars.get(mm.group(0))
            if v is None:
                return mm.group(0)
            r = constant_value(v, depth + 1)
            return mm.group(0) if r is None else str(r)
        expr = re.sub(r"\b_\w+\b", sub, expr)
        expr = re.sub(r"\b(?:uint|int)\(", "(", expr)
        return evaluate_constant(expr)

    # Pointer variables: `vec4Pointer _208 = vec4Pointer(EXPR);` -> uint offset.
    var_types: dict[str, str] = {}
    var_values: dict[str, int | None] = {}
    decl = re.compile(r"\b(\w+Pointer) (_\w+) = \1\(")
    pos = 0
    out = []
    while True:
        dm = decl.search(src, pos)
        if not dm:
            out.append(src[pos:])
            break
        ptype, var = dm.group(1), dm.group(2)
        if ptype not in POINTER_TYPES:
            raise ConversionError(f"unknown pointer type {ptype}")
        open_i = dm.end() - 1
        close_i = matching_paren(src, open_i)
        expr = src[open_i + 1 : close_i]
        var_types[var] = POINTER_TYPES[ptype]
        var_values[var] = constant_value(expr)
        out.append(src[pos : dm.start()])
        out.append(f"uint {var} = uint({expr})")
        pos = close_i + 1
    src = "".join(out)

    # Inline pointer constructions `vec4Pointer(EXPR).value` -> loads.
    def replace_inline(text: str) -> str:
        pat = re.compile(r"\b(\w+Pointer)\(")
        res = []
        pos = 0
        while True:
            pm = pat.search(text, pos)
            if not pm:
                res.append(text[pos:])
                return "".join(res)
            ptype = pm.group(1)
            if ptype not in POINTER_TYPES:
                raise ConversionError(f"unknown pointer type {ptype}")
            open_i = pm.end() - 1
            close_i = matching_paren(text, open_i)
            inner = replace_inline(text[open_i + 1 : close_i])
            if not text.startswith(".value", close_i + 1):
                raise ConversionError("pointer used without .value")
            res.append(text[pos : pm.start()])
            res.append(f"{LOADERS[POINTER_TYPES[ptype]]}(uint({inner}))")
            pos = close_i + 1 + len(".value")

    # Bindless arrays. Resolve before the generic load rewrite so the slot is
    # taken from the pointer offset, not from the loaded index.
    arrays: dict[str, str] = {}
    for am in re.finditer(r"layout\(set = \d+, binding = \d+\) uniform (texture\w+|sampler) (_\w+)\[\];\n", src):
        arrays[am.group(2)] = am.group(1)
    src = re.sub(r"layout\(set = \d+, binding = \d+\) uniform (texture\w+|sampler) (_\w+)\[\];\n", "", src)
    if "[]" in src and re.search(r"uniform \w+ \w+\[\]", src):
        raise ConversionError("unhandled runtime array")

    def slot_for(kind: str, offset: int) -> int:
        base = SAMPLER_BASE if kind == "sampler" else TEXTURE_RANGES.get(kind)
        if base is None:
            raise ConversionError(f"unsupported texture type {kind}")
        if offset < base or offset >= base + SLOTS * 4 or (offset - base) % 4:
            raise ConversionError(f"{kind} index at shared offset {offset:#x} outside its array")
        return (offset - base) // 4

    def resolve_arrays(text: str) -> str:
        if not arrays:
            return text
        pat = re.compile(r"\b(" + "|".join(map(re.escape, arrays)) + r")\[")
        res = []
        pos = 0
        while True:
            am = pat.search(text, pos)
            if not am:
                res.append(text[pos:])
                return "".join(res)
            name = am.group(1)
            open_i = am.end() - 1
            depth = 0
            close_i = None
            for i in range(open_i, len(text)):
                if text[i] == "[":
                    depth += 1
                elif text[i] == "]":
                    depth -= 1
                    if depth == 0:
                        close_i = i
                        break
            if close_i is None:
                raise ConversionError("unbalanced brackets")
            index = text[open_i + 1 : close_i].strip()
            offset = None
            vm = re.fullmatch(r"(_\w+)\.value", index)
            if vm and vm.group(1) in var_values:
                offset = var_values[vm.group(1)]
                if var_types[vm.group(1)] != "uint":
                    raise ConversionError("descriptor index is not a uint")
            else:
                # Newer SPIRV-Cross loads the index into a temporary first.
                loaded = scalars.get(index, index) if re.fullmatch(r"_\w+", index) else index
                im = re.fullmatch(r"uintPointer\((.*)\)\.value", loaded, re.S)
                if im:
                    offset = constant_value(im.group(1))
            if offset is None:
                raise ConversionError(f"non-constant descriptor index `{index[:60]}`")
            offset -= 2 * BLOCK_STRIDE
            kind = arrays[name]
            slot = slot_for(kind, offset)
            if kind == "sampler":
                if slot not in info["samplers"]:
                    info["samplers"].append(slot)
                ident = f"xs_{slot}"
            else:
                prev = info["textures"].get(slot)
                if prev and prev != kind:
                    raise ConversionError(f"slot {slot} used as {prev} and {kind}")
                info["textures"][slot] = kind
                ident = f"xt_{slot}"
            res.append(text[pos : am.start()])
            res.append(ident)
            pos = close_i + 1

    src = resolve_arrays(src)
    src = replace_inline(src)

    # Remaining `.value` reads of pointer variables.
    if var_types:
        src = re.sub(
            r"\b(" + "|".join(map(re.escape, var_types)) + r")\.value\b",
            lambda mm: f"{LOADERS[var_types[mm.group(1)]]}({mm.group(1)})",
            src,
        )
    if "Pointer" in src or "uint64_t" in src or "int64_t" in src:
        raise ConversionError("pointer or 64-bit value left after rewrite")

    # Specialization constants and constants derived from them.
    spec_defines = []
    spec_names: set[str] = set()
    for sm in re.finditer(r"layout\(constant_id = (\d+)\) const uint (\w+) = [^;]+;\n", src):
        if sm.group(1) != "0":
            raise ConversionError("unexpected specialization constant id")
        spec_names.add(sm.group(2))
        spec_defines.append(f"#define {sm.group(2)} xc_u({SPEC_CONSTANT_OFFSET}u)\n")
    src = re.sub(r"layout\(constant_id = \d+\) const uint \w+ = [^;]+;\n", "", src)
    if spec_names:
        def derived(mm):
            names = set(re.findall(r"\b_\w+\b", mm.group(3)))
            if names & spec_names:
                spec_names.add(mm.group(2))
                spec_defines.append(f"#define {mm.group(2)} ({mm.group(3)})\n")
                return ""
            return mm.group(0)
        src = re.sub(r"^const (\w+) (\w+) = ([^;]+);\n", derived, src, flags=re.M)

    src = remove_switch_fallthrough(src)
    src = re.sub(r"\bisnan\(", "xisnan(", src)
    src = re.sub(r"\bisinf\(", "xisinf(", src)

    decls = [PRELUDE] + spec_defines
    for slot, kind in sorted(info["textures"].items()):
        decls.append(f"layout(set = 1, binding = {slot}) uniform {kind} xt_{slot};\n")
    for slot in sorted(info["samplers"]):
        decls.append(f"layout(set = 1, binding = {SAMPLER_BINDING_BASE + slot}) uniform sampler xs_{slot};\n")

    # Drop extensions the rewritten shader no longer needs; insert declarations
    # after the last extension/#if preamble line block (before first layout()).
    src = re.sub(r"#if defined\(GL_ARB_gpu_shader_int64\)\n.*?#endif\n", "", src, flags=re.S)
    src = re.sub(r"#extension GL_EXT_buffer_reference\w* : require\n", "", src)
    src = re.sub(r"#extension GL_EXT_nonuniform_qualifier : require\n", "", src)
    src = src.replace("nonuniformEXT", "")
    insert_at = src.find("\nlayout(")
    if insert_at < 0:
        insert_at = src.find("\nvoid main")
    src = src[: insert_at + 1] + "".join(decls) + src[insert_at + 1 :]
    info["textures"] = {str(k): v for k, v in sorted(info["textures"].items())}
    info["samplers"].sort()
    return src, info


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def convert_one(spv: Path, stage: str, work: Path) -> tuple[str, dict]:
    base = work / spv.stem
    glsl = run(["spirv-cross", str(spv), "--version", "450", "--vulkan-semantics"])
    if glsl.returncode:
        raise ConversionError("spirv-cross: " + glsl.stderr.strip()[:300])
    rewritten, info = rewrite_glsl(glsl.stdout)
    ext = "frag" if stage == "ps" else "vert"
    gpath = base.with_suffix("." + ext)
    gpath.write_text(rewritten)
    out_spv = base.with_suffix(".rw.spv")
    comp = run(["glslangValidator", "-V", "--target-env", "vulkan1.1", "-o", str(out_spv), str(gpath)])
    if comp.returncode:
        raise ConversionError("glslang: " + (comp.stdout + comp.stderr).strip()[-600:])
    out_spv.write_bytes(scalarize_sample_mask(out_spv.read_bytes()))
    wgsl_path = base.with_suffix(".wgsl")
    # Optimizing first roughly halves the WGSL (inlined loads, folded
    # constants). Fall back to the unoptimized module if either step fails.
    opt_spv = base.with_suffix(".opt.spv")
    if (run(["spirv-opt", "-O", str(out_spv), "-o", str(opt_spv)]).returncode == 0 and
            run(["naga", str(opt_spv), str(wgsl_path)]).returncode == 0):
        return finish_wgsl_file(wgsl_path), info
    tr = run(["naga", str(out_spv), str(wgsl_path)])
    if tr.returncode:
        raise ConversionError("naga: " + (tr.stdout + tr.stderr).strip()[-600:])
    return finish_wgsl_file(wgsl_path), info


def finish_wgsl_file(path: Path) -> str:
    try:
        code = hot_constants(draw_constants(split_uniforms(finish_wgsl(path.read_text()))))
    except RewriteError as e:
        raise ConversionError(f"uniform split: {e}") from e
    path.write_text(code)
    return code


def finish_wgsl(code: str) -> str:
    """Adjusts naga's output for Tint (Dawn, Chrome)."""
    if code.startswith("diagnostic(off, derivative_uniformity);"):
        return code
    # naga prints FLT_MAX in decimal, rounded beyond the f32 range; Tint
    # rejects that literal. Hex floats are exact.
    code = re.sub(r"\b340282350000000000000000000000000000000f\b", "0x1.fffffep+127f", code)
    # Xenos shaders sample with implicit derivatives inside control flow that
    # depends on varyings, exactly as the Vulkan and Metal renderers run them.
    return "diagnostic(off, derivative_uniformity);\n" + code


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--spirv-dir", required=True, type=Path,
                    help="directory of <HASH>_e.spv / <HASH>_l.spv and meta.json (extract_shader_cache.py)")
    ap.add_argument("--output", required=True, type=Path, help="archive file to write")
    ap.add_argument("--work", required=True, type=Path)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--report", type=Path)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    meta = json.loads((args.spirv_dir / "meta.json").read_text())
    if args.limit:
        meta = meta[: args.limit]
    args.work.mkdir(parents=True, exist_ok=True)
    jobs = []
    for entry in meta:
        stage = "ps" if entry["stage"] == "ps" else "vs"
        for variant in ("e", "l") if entry["late"] else ("e",):
            spv = args.spirv_dir / f"{entry['hash']}_{variant}.spv"
            jobs.append((entry, variant, stage, spv))

    results = {}
    failures = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futs = {pool.submit(convert_one, spv, stage, args.work): (entry, variant, stage)
                for entry, variant, stage, spv in jobs}
        for fut in concurrent.futures.as_completed(futs):
            entry, variant, stage = futs[fut]
            key = (entry["hash"], variant)
            try:
                results[key] = (stage, entry, fut.result())
            except ConversionError as e:
                failures[f"{entry['hash']}_{variant} {entry['name']}"] = str(e)

    write_archive(args.output, results)
    print(f"converted {len(results)} / {len(jobs)} shader variants; {len(failures)} failed")
    if args.report:
        args.report.write_text(json.dumps(failures, indent=1, sort_keys=True))
    return 0 if not failures else 1


# Archive (zlib-compressed): "LRWGSL05", u32 count, then per record (little-endian):
#   u64 hash, u32 stage (0 pixel, 1 vertex), u32 variant (0 early, 1 late),
#   u32 texture mask, u32 cube mask, u32 sampler mask, u32 spec mask,
#   u32 color output mask, u32 attribute count, u8 semantic location per
#   attribute (WGSL @location(i) reads semantic attributes[i]), padding to
#   four bytes, u32 WGSL length, WGSL bytes, padding to four bytes.
MAGIC = b"LRWGSL05"


def write_archive(path: Path, results: dict) -> None:
    records = []
    for (h, variant), (stage, entry, (wgsl, info)) in sorted(results.items()):
        tex_mask = cube_mask = 0
        for slot, kind in info["textures"].items():
            tex_mask |= 1 << int(slot)
            if kind == "textureCube":
                cube_mask |= 1 << int(slot)
        smp_mask = 0
        for slot in info["samplers"]:
            smp_mask |= 1 << slot
        attributes = bytes(info["attributes"])
        attributes += b"\0" * (-len(attributes) % 4)
        body = wgsl.encode()
        body += b"\0" * (-len(body) % 4)
        records.append(struct.pack("<QIIIIIIII", int(h, 16), 0 if stage == "ps" else 1,
                                   0 if variant == "e" else 1, tex_mask, cube_mask, smp_mask,
                                   entry["spec"], info["outputs"], len(info["attributes"])) +
                       attributes + struct.pack("<I", len(wgsl.encode())) + body)
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = MAGIC + struct.pack("<I", len(records)) + b"".join(records)
    path.write_bytes(zlib.compress(raw, 9))


if __name__ == "__main__":
    sys.exit(main())
