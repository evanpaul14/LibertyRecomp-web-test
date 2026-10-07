"""Small SPIR-V rewrites applied before naga reads a module."""
from __future__ import annotations

import struct

OP_NAME = 5
OP_DECORATE = 71
OP_TYPE_INT = 21
OP_TYPE_POINTER = 32
OP_VARIABLE = 59
OP_ACCESS_CHAIN = 65
OP_STORE = 62
OP_BITCAST = 124
OP_FUNCTION = 54
DECORATION_BUILTIN = 11
BUILTIN_SAMPLE_MASK = 20
STORAGE_OUTPUT = 3


def _instructions(words):
    i = 5
    while i < len(words):
        length = words[i] >> 16
        if length == 0:
            raise ValueError("malformed SPIR-V")
        yield i, words[i] & 0xFFFF, words[i : i + length]
        i += length


def scalarize_sample_mask(data: bytes) -> bytes:
    """Turns `out int gl_SampleMask[1]` into `out uint gl_SampleMask`.

    naga rejects the array form glslang emits. Stores through `[0]` become
    bitcast stores to the scalar variable.
    """
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    mask_var = None
    for _, op, ins in _instructions(words):
        if op == OP_DECORATE and ins[2] == DECORATION_BUILTIN and ins[3] == BUILTIN_SAMPLE_MASK:
            mask_var = ins[1]
    if mask_var is None:
        return data

    bound = words[3]
    uint_type = None
    out_ptr_uint = None
    for _, op, ins in _instructions(words):
        if op == OP_TYPE_INT and ins[2] == 32 and ins[3] == 0:
            uint_type = ins[1]
    for _, op, ins in _instructions(words):
        if op == OP_TYPE_POINTER and ins[2] == STORAGE_OUTPUT and ins[3] == uint_type:
            out_ptr_uint = ins[1]

    out = words[:5]
    new_types = []
    if uint_type is None:
        uint_type = bound
        bound += 1
        new_types += [(4 << 16) | OP_TYPE_INT, uint_type, 32, 0]
    if out_ptr_uint is None:
        out_ptr_uint = bound
        bound += 1
        new_types += [(4 << 16) | OP_TYPE_POINTER, out_ptr_uint, STORAGE_OUTPUT, uint_type]
    chains = set()
    for _, op, ins in _instructions(words):
        if op == OP_ACCESS_CHAIN and ins[3] == mask_var:
            chains.add(ins[2])
    for _, op, ins in _instructions(words):
        if op == OP_VARIABLE and ins[2] == mask_var:
            out += new_types
            new_types = []
            out += [ins[0], out_ptr_uint, mask_var] + ins[3:]
        elif op == OP_ACCESS_CHAIN and ins[2] in chains:
            continue
        elif op == OP_STORE and ins[1] in chains:
            cast = bound
            bound += 1
            out += [(4 << 16) | OP_BITCAST, uint_type, cast, ins[2]]
            out += [(3 << 16) | OP_STORE, mask_var, cast] + ins[3:]
        else:
            out += ins
    if new_types:
        raise ValueError("sample mask variable not found")
    out[3] = bound
    return struct.pack(f"<{len(out)}I", *out)
