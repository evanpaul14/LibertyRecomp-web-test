#!/usr/bin/env python3
"""Extract the stock SPIR-V shaders from LibertyRecompLib/shader/shader_cache.cpp.

Writes <HASH>_e.spv (and <HASH>_l.spv for late-Z variants) plus meta.json.
SMOL-V decoding uses a small helper built from tools/decode_smolv.cpp.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ENTRY = re.compile(
    r'\{ (0x[0-9A-Fa-f]+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), '
    r'"([^"]+)", nullptr, (\d+) \}')


def spirv_stage(words: bytes) -> str:
    count = len(words) // 4
    i = 5
    while i < count:
        word = struct.unpack_from("<I", words, i * 4)[0]
        op, length = word & 0xFFFF, word >> 16
        if length == 0:
            break
        if op == 15:  # OpEntryPoint
            model = struct.unpack_from("<I", words, (i + 1) * 4)[0]
            return {0: "vs", 4: "ps"}.get(model, "other")
        i += length
    return "other"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cache", required=True, type=Path)
    ap.add_argument("--decoder", required=True, type=Path, help="decode_smolv executable")
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    try:
        import zstandard
    except ImportError:
        sys.exit("python package 'zstandard' is required (pip install zstandard)")

    entries = []
    blob = None
    with open(args.cache) as f:
        for line in f:
            m = ENTRY.search(line)
            if m:
                entries.append(m.groups())
            elif line.startswith("const uint8_t g_compressedSpirvCache[]"):
                body = line[line.index("{") + 1 : line.rindex("}")]
                blob = bytes(int(x) for x in body.split(","))
    if blob is None or not entries:
        sys.exit("shader cache layout not recognized")
    data = zstandard.ZstdDecompressor().decompress(blob, max_output_size=256 << 20)
    args.output.mkdir(parents=True, exist_ok=True)
    meta = []
    for e in entries:
        shader_hash = int(e[0], 16)
        early_offset, early_size, late_offset, late_size = map(int, e[3:7])
        stage = None
        for tag, offset, size in (("e", early_offset, early_size), ("l", late_offset, late_size)):
            if not size:
                continue
            smolv = args.output / f"{shader_hash:016X}_{tag}.smolv"
            spv = smolv.with_suffix(".spv")
            smolv.write_bytes(data[offset : offset + size])
            subprocess.check_call([str(args.decoder), str(smolv), str(spv)])
            smolv.unlink()
            stage = stage or spirv_stage(spv.read_bytes())
        if stage is None:
            continue
        meta.append({"hash": f"{shader_hash:016X}", "name": e[10], "stage": stage,
                     "spec": int(e[9]), "tex": int(e[11]), "late": bool(late_size)})
    (args.output / "meta.json").write_text(json.dumps(meta, indent=0))
    print(f"extracted {len(meta)} shaders")
    return 0


if __name__ == "__main__":
    sys.exit(main())
