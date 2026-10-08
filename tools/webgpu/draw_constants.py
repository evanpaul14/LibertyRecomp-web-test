#!/usr/bin/env python3
"""Read the title shaders' constants from one storage buffer, selected per draw.

split_uniforms.py leaves three uniform bindings (vertex, pixel and shared
constants), each with a dynamic offset, so the renderer calls SetBindGroup on
almost every draw. This rewrites them as one read-only storage buffer holding
the renderer's whole constant arena:

  @group(0) @binding(0) var<storage, read> xu: array<vec4<u32>>;

Each draw writes a draw record to the arena (the register index of its vertex,
pixel and shared constants) and passes the record's index as firstInstance.
The vertex shader loads the record with @builtin(instance_index) and hands it
to the pixel shader in a flat varying at @location(FLAT_LOCATION). Title
draws are never instanced, so instance_index is always firstInstance.

Run on an archive in place (it converts LRWGSL03 to LRWGSL04):
  draw_constants.py LibertyRecompLib/shader/webgpu_shader_archive.bin
spirv_to_wgsl.py applies `rewrite` itself when it builds an archive.
"""
from __future__ import annotations

import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from split_uniforms import BINDINGS, DECLARATIONS, SHARED_REGISTERS, RewriteError  # noqa: E402

# Vertex shaders write locations 0-17.
FLAT_LOCATION = 18

STORAGE = f"""@group(0) @binding(0) var<storage, read> xu: array<vec4<u32>>;
var<private> xd: vec4<u32>;
fn xc_load(i: u32) -> vec4<u32> {{
    if (i < 256u) {{ return xu[xd.x + i]; }}
    if (i < 512u) {{ return xu[xd.y + i - 256u]; }}
    return xu[xd.z + min(i - 512u, {SHARED_REGISTERS - 1}u)];
}}
"""
LITERAL = re.compile(r"\bx([vps])\.w\[(\d+)u\]")
BASE = {"v": "x", "p": "y", "s": "z"}
ENTRY = re.compile(r"^(@(vertex|fragment) \nfn main\()(.*)(\) -> \w+ \{\n)", re.M)


def rewrite(code: str) -> str:
    code, declarations = re.subn(re.escape(DECLARATIONS), "", code)
    code, bindings = re.subn(re.escape(BINDINGS), lambda _: STORAGE, code)
    if declarations != 1 or bindings != 1:
        raise RewriteError("uniform bindings not recognized")
    code = LITERAL.sub(lambda m: f"xu[xd.{BASE[m.group(1)]} + {m.group(2)}u]", code)
    if re.search(r"\bx[vps]\b", code):
        raise RewriteError("uniform binding used other than by register")

    entries = list(ENTRY.finditer(code))
    if len(entries) != 1:
        raise RewriteError("entry point not recognized")
    entry = entries[0]
    head, stage, params, tail = entry.groups()
    if f"@location({FLAT_LOCATION})" in params:
        raise RewriteError(f"entry point already uses location {FLAT_LOCATION}")
    separator = ", " if params else ""
    if stage == "vertex":
        params += f"{separator}@builtin(instance_index) xi: u32"
        body = "    xd = xu[xi];\n"
    else:
        params += f"{separator}@location({FLAT_LOCATION}) @interpolate(flat) xdraw: vec4<u32>"
        body = "    xd = xdraw;\n"
    code = code[:entry.start()] + head + params + tail + body + code[entry.end():]

    if stage == "vertex":
        struct = re.search(r"^struct VertexOutput \{\n(.*?)^\}\n", code, re.M | re.S)
        if not struct:
            raise RewriteError("vertex output not recognized")
        if f"@location({FLAT_LOCATION})" in struct.group(1):
            raise RewriteError(f"vertex output already uses location {FLAT_LOCATION}")
        member = f"    @location({FLAT_LOCATION}) @interpolate(flat) xdraw: vec4<u32>,\n"
        code = code[:struct.end(1)] + member + code[struct.end(1):]
        code, returns = re.subn(r"(\breturn VertexOutput\(.*)\);", r"\1, xd);", code)
        if returns != 1:
            raise RewriteError("vertex output return not recognized")
    return code


def main() -> int:
    path = Path(sys.argv[1])
    raw = zlib.decompress(path.read_bytes())
    if raw[:8] != b"LRWGSL03":
        print(f"{path}: not an LRWGSL03 archive", file=sys.stderr)
        return 1
    (count,) = struct.unpack_from("<I", raw, 8)
    pos = 12
    records = []
    for _ in range(count):
        header = raw[pos:pos + 40]
        attribute_count = struct.unpack_from("<I", header, 36)[0]
        pos += 40
        attributes = raw[pos:pos + attribute_count + (-attribute_count % 4)]
        pos += len(attributes)
        (length,) = struct.unpack_from("<I", raw, pos)
        pos += 4
        code = raw[pos:pos + length].decode()
        pos += length + (-length % 4)
        body = rewrite(code).encode()
        records.append(header + attributes + struct.pack("<I", len(body)) + body +
                       b"\0" * (-len(body) % 4))
    path.write_bytes(zlib.compress(b"LRWGSL04" + struct.pack("<I", count) + b"".join(records), 9))
    print(f"rewrote {count} shader variants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
