#!/usr/bin/env python3
"""Give each stage's hot constant registers a slot of their own.

The title rewrites vertex and pixel registers 0-15 (world matrix and friends)
on almost every draw and the rest rarely, yet draw_constants.py makes each
stage's 256 registers one slot, so the renderer uploaded a whole 4 KB slot
per change (~20 MB a frame in heavy scenes). This splits each stage's
registers into a hot slot (registers 0-15) and a cold slot (16-255), which the
renderer reuses separately:

  register r < 16:  xu[xd.x + r]
  register r >= 16: xu[xd.y + r]   (the cold slot's index minus 16)
  shared:           xu[xd.z + r]   (unchanged)

The draw record becomes two registers: at firstInstance the vertex stage's
(hot, cold, shared, 0), and after it the pixel stage's, which the vertex
shader hands to the pixel shader in the flat varying. Each stage reads only
its own registers (checked here for literal reads; the title's dynamic reads
index within their stage).

Run on an archive in place (it converts LRWGSL04 to LRWGSL05):
  hot_constants.py LibertyRecompLib/shader/webgpu_shader_archive.bin
spirv_to_wgsl.py applies `rewrite` itself when it builds an archive.
"""
from __future__ import annotations

import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from split_uniforms import SHARED_REGISTERS, RewriteError  # noqa: E402

HOT_REGISTERS = 16

OLD_LOAD = f"""fn xc_load(i: u32) -> vec4<u32> {{
    if (i < 256u) {{ return xu[xd.x + i]; }}
    if (i < 512u) {{ return xu[xd.y + i - 256u]; }}
    return xu[xd.z + min(i - 512u, {SHARED_REGISTERS - 1}u)];
}}
"""
NEW_LOAD = f"""fn xc_load(i: u32) -> vec4<u32> {{
    if (i < 512u) {{
        let r = i & 255u;
        return xu[select(xd.y, xd.x, r < {HOT_REGISTERS}u) + r];
    }}
    return xu[xd.z + min(i - 512u, {SHARED_REGISTERS - 1}u)];
}}
"""
LITERAL = re.compile(r"\bxu\[xd\.([xy]) \+ (\d+)u\]")


def rewrite(code: str) -> str:
    if code.count(OLD_LOAD) != 1:
        raise RewriteError("constant loader not recognized")
    vertex = "@vertex" in code
    own = "x" if vertex else "y"

    def register(match: re.Match) -> str:
        if match.group(1) != own:
            raise RewriteError("shader reads the other stage's constants")
        index = int(match.group(2))
        return f"xu[xd.{'x' if index < HOT_REGISTERS else 'y'} + {index}u]"

    code = LITERAL.sub(register, code.replace(OLD_LOAD, NEW_LOAD))
    if vertex:
        code, returns = re.subn(r"(\breturn VertexOutput\(.*), xd\);", r"\1, xu[xi + 1u]);", code)
        if returns != 1:
            raise RewriteError("vertex output return not recognized")
    return code


def main() -> int:
    path = Path(sys.argv[1])
    raw = zlib.decompress(path.read_bytes())
    if raw[:8] != b"LRWGSL04":
        print(f"{path}: not an LRWGSL04 archive", file=sys.stderr)
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
    path.write_bytes(zlib.compress(b"LRWGSL05" + struct.pack("<I", count) + b"".join(records), 9))
    print(f"rewrote {count} shader variants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
