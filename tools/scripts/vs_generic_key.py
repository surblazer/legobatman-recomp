#!/usr/bin/env python3
"""LEGO Batman port: the fetch-agnostic key of a vertex shader.

D3D rewrites a vertex shader's vfetch instructions when the game binds it (instruction order, data format, offset,
stride, fetch constant, destination swizzle: all from the vertex declaration). The disc copy and the bound copy
differ only there, and each fetch keeps its destination register and the components it writes. The key is FNV-1a 64
over the big-endian microcode with every vfetch instruction zeroed, followed by the sorted (register, write mask)
of the vfetches. The backend computes the same key (GenericVsKey in deko3d_backend.cpp) for a bound shader it has
no exact DKSH for.

    key, fetches = generic_key(words)   # words: the microcode dwords (values); fetches: [(reg, mask)] in order
"""
import struct

EXEC_OPS = {1, 2, 3, 4, 5, 6, 13, 14}


def fetch_slots(words):
    """Instruction indices of the vertex fetches (from the exec clauses' sequence bits)."""
    slots = []
    limit = len(words)  # the instructions start at the lowest exec address: the control flow ends there
    i = 0
    while i + 2 < len(words) and i < limit:
        w0, w1, w2 = words[i], words[i + 1], words[i + 2]
        cfs = [((w1 >> 12) & 15, w0 & 0xFFF, (w0 >> 12) & 7, (w0 >> 16) & 0xFFF),
               (w2 >> 28, (w1 >> 16) & 0xFFF, (w1 >> 28) & 7, w2 & 0xFFF)]
        for op, addr, count, seq in cfs:
            if op not in EXEC_OPS or count == 0:
                continue
            limit = min(limit, addr * 3)
            for k in range(count):
                if (seq >> (2 * k)) & 1:
                    n = addr + k
                    if n * 3 + 2 < len(words) and words[n * 3] & 0x1F == 0:  # FetchOpcode::kVertexFetch
                        slots.append(n)
        i += 3
    return sorted(set(slots))


def generic_key(words):
    words = list(words)
    fetches = []
    for n in fetch_slots(words):
        reg = (words[n * 3] >> 12) & 0x3F
        swz = words[n * 3 + 1] & 0xFFF
        mask = sum(1 << c for c in range(4) if (swz >> (3 * c)) & 7 != 7)
        fetches.append((reg, mask))
        words[n * 3:n * 3 + 3] = [0, 0, 0]
    raw = struct.pack('>%dI' % len(words), *words) + bytes(b for f in sorted(fetches) for b in f)
    h = 1469598103934665603
    for c in raw:
        h ^= c
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h, fetches


def key_of_bin(path):
    data = open(path, 'rb').read()
    return generic_key(struct.unpack('<%dI' % (len(data) // 4), data))
