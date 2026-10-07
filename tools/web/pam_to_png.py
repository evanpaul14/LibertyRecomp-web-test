#!/usr/bin/env python3
"""Convert the WebGPU renderer's frame dumps (--webgpu_frame_dump_path, PAM
RGBA images) to PNG next to each input. Standard library only.

    python3 tools/web/pam_to_png.py /tmp/frames/*.pam
"""
import struct
import sys
import zlib


def convert(path: str) -> str:
    data = open(path, "rb").read()
    head, _, body = data.partition(b"ENDHDR\n")
    fields = dict(line.split(" ", 1) for line in head.decode().splitlines()[1:] if " " in line)
    width, height = int(fields["WIDTH"]), int(fields["HEIGHT"])
    if int(fields.get("DEPTH", "4")) != 4 or len(body) < width * height * 4:
        raise ValueError(f"{path}: not an RGBA PAM image")
    rows = b"".join(b"\0" + body[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    out = path[:-4] + ".png" if path.endswith(".pam") else path + ".png"
    with open(out, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 6)))
        f.write(chunk(b"IEND", b""))
    return out


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        print(convert(p))
