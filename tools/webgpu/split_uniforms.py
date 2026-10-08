#!/usr/bin/env python3
"""Split the title shaders' uniform block into one binding per part.

spirv_to_wgsl.py emits one 768-register block per shader: vertex constants
in registers 0-255, pixel constants in 256-511 and the shared constants
(with the specialization word in register 592) from 512. This rewrites it as
three bindings of group 0, so the renderer can reuse each part on its own:

  binding 0: vertex constants, 256 registers
  binding 1: pixel constants, 256 registers
  binding 2: shared constants, 81 registers

Reads with a literal register index are mapped directly; other reads (bone
palettes and the like) go through xc_load, which picks the binding.

Run on an archive in place (it converts LRWGSL02 to LRWGSL03):
  split_uniforms.py LibertyRecompLib/shader/webgpu_shader_archive.bin
spirv_to_wgsl.py applies `rewrite` itself when it builds an archive.
"""
from __future__ import annotations

import re
import struct
import sys
import zlib
from pathlib import Path

SHARED_REGISTERS = 81

BLOCK = re.compile(
    r"struct XenosConstants \{\n    w: array<vec4<u32>, 768>,\n\}\n")
BINDING = re.compile(r"@group\(0\) @binding\(0\) \nvar<uniform> xc: XenosConstants;\n")

DECLARATIONS = f"""struct XenosRegisters {{
    w: array<vec4<u32>, 256>,
}}
struct XenosShared {{
    w: array<vec4<u32>, {SHARED_REGISTERS}>,
}}
"""
BINDINGS = f"""@group(0) @binding(0) var<uniform> xv: XenosRegisters;
@group(0) @binding(1) var<uniform> xp: XenosRegisters;
@group(0) @binding(2) var<uniform> xs: XenosShared;
fn xc_load(i: u32) -> vec4<u32> {{
    if (i < 256u) {{ return xv.w[i]; }}
    if (i < 512u) {{ return xp.w[i - 256u]; }}
    return xs.w[min(i - 512u, {SHARED_REGISTERS - 1}u)];
}}
"""


class RewriteError(Exception):
    pass


def literal(register: int) -> str:
    if register < 256:
        return f"xv.w[{register}u]"
    if register < 512:
        return f"xp.w[{register - 256}u]"
    if register < 512 + SHARED_REGISTERS:
        return f"xs.w[{register - 512}u]"
    raise RewriteError(f"register {register} is outside the shared constants")


def rewrite(code: str) -> str:
    code, blocks = BLOCK.subn(DECLARATIONS, code)
    code, bindings = BINDING.subn(BINDINGS, code)
    if blocks != 1 or bindings != 1:
        raise RewriteError("uniform block layout not recognized")
    out = []
    pos = 0
    while True:
        start = code.find("xc.w[", pos)
        if start < 0:
            break
        out.append(code[pos:start])
        # The index expression, to its matching bracket.
        depth, end = 1, start + 5
        while depth:
            if end >= len(code):
                raise RewriteError("unbalanced uniform index")
            depth += {"[": 1, "]": -1}.get(code[end], 0)
            end += 1
        index = code[start + 5:end - 1]
        m = re.fullmatch(r"(\d+)u", index)
        out.append(literal(int(m.group(1))) if m else f"xc_load({index})")
        pos = end
    out.append(code[pos:])
    code = "".join(out)
    if re.search(r"\bxc\b", code):
        raise RewriteError("uniform block used other than by register")
    return code


def main() -> int:
    path = Path(sys.argv[1])
    raw = zlib.decompress(path.read_bytes())
    if raw[:8] != b"LRWGSL02":
        print(f"{path}: not an LRWGSL02 archive", file=sys.stderr)
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
    path.write_bytes(zlib.compress(b"LRWGSL03" + struct.pack("<I", count) + b"".join(records), 9))
    print(f"rewrote {count} shader variants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
