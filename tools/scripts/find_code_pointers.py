"""Find functions that are only reachable through pointers stored in data (vtables, callback tables).

LEGO Batman's engine is built without RTTI, so ReXGlue's vtable scanner finds nothing, and small
virtual methods without a .pdata entry get merged into the preceding function. This scans .rdata/.data
for aligned words that point into code at a plausible function entry and emits ReXGlue [functions]
overrides with sizes computed by walking control flow.

Usage: python3 -I find_code_pointers.py image.bin pdata.txt xexdump.txt jump_tables.toml > out.toml
"""
import re
import struct
import sys
import tomllib

BASE = 0x82000000
img_path, pdata_path, dump_path, jt_path = sys.argv[1:5]
img = open(img_path, 'rb').read()


def word(a):
    return struct.unpack('>I', img[a - BASE:a - BASE + 4])[0]


sections = []  # (name, start, end, is_code)
for line in open(dump_path, encoding='latin-1'):
    m = re.match(r'section (\S+)\s+([0-9a-f]+)-([0-9a-f]+) flags=(\d+)', line)
    if m:
        sections.append((m[1], int(m[2], 16), int(m[3], 16), m[4] == '2'))
code = [(s, e) for _, s, e, c in sections if c]
data = [(s, e) for n, s, e, c in sections if not c and n in ('.rdata', '.data')]


# import thunks (16 bytes each) are resolved by the runtime, never declared as functions
thunks = set()
for line in open(dump_path, encoding='latin-1'):
    if line.startswith('import '):
        t = int(line.split()[1], 16)
        thunks.update(range(t, t + 16, 4))


def in_code(a):
    return any(s <= a < e for s, e in code)


pdata = {}
for line in open(pdata_path):
    b, l, _ = line.split()
    pdata[int(b, 16)] = int(l, 16)
pdata_sorted = sorted(pdata)

jt_labels = set()
bctr_targets = {}  # address of the bctr that dispatches a jump table -> its labels
for t in tomllib.load(open(jt_path, 'rb'))['switch']:
    jt_labels.update(t['labels'])
    a = t['base']
    while word(a) != 0x4E800420:
        a += 4
    bctr_targets[a] = t['labels']


def inside_pdata_body(a):
    # binary search: is a strictly inside a pdata function?
    lo, hi = 0, len(pdata_sorted)
    while lo < hi:
        mid = (lo + hi) // 2
        if pdata_sorted[mid] <= a:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return False
    b = pdata_sorted[lo - 1]
    return b < a < b + pdata[b]


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def looks_like_inline_table(v):
    """Jump tables stored inline after a bctr start with a nearby code address."""
    w = word(v)
    return w & 3 == 0 and in_code(w) and abs(w - v) < 0x10000


def ends_flow(w):
    """Instruction after which execution cannot fall through."""
    if w == 0x4E800020 or w == 0x4E800420:  # blr, bctr
        return True
    if (w & 0xFC000003) == 0x48000000:     # b (no link, relative)
        return True
    return w == 0                          # padding


candidates = set()
for s, e in data:
    for a in range(s, e - 3, 4):
        v = word(a)
        if v & 3 or not in_code(v) or v in thunks or v in pdata or v in jt_labels or inside_pdata_body(v):
            continue
        if word(v) == 0 or not ends_flow(word(v - 4)) or looks_like_inline_table(v):
            continue
        candidates.add(v)

# Addresses built in code: lis rX,hi followed (within 8 instructions) by addi/ori rY,rX,lo.
# Used for callbacks such as sort comparators that are never stored in data.
for s_, e_ in code:
    for a in range(s_, e_ - 3, 4):
        w = word(a)
        if w >> 26 != 15 or (w >> 16) & 31 != 0:   # lis = addis rD,0,imm
            continue
        rd, hi = (w >> 21) & 31, w & 0xFFFF
        for b in range(a + 4, min(a + 36, e_), 4):
            x = word(b)
            op, rt, ra = x >> 26, (x >> 21) & 31, (x >> 16) & 31
            if op == 14 and ra == rd:                 # addi rT,rD,lo
                v = ((hi << 16) + sext(x & 0xFFFF, 16)) & 0xFFFFFFFF
            elif op == 24 and rt == rd:               # ori rA,rS,lo (rS in bits 21-25)
                v = (hi << 16) | (x & 0xFFFF)
            else:
                if rt == rd and op not in (36, 38, 44, 37, 39, 45, 32, 34, 40, 48, 50, 52, 54):
                    break                             # rD overwritten by something else
                continue
            if (v & 3 == 0 and in_code(v) and v not in thunks and v not in pdata and v not in jt_labels
                    and not inside_pdata_body(v) and word(v) != 0 and ends_flow(word(v - 4))
                    and not looks_like_inline_table(v)):
                candidates.add(v)
            break

starts = sorted(set(pdata_sorted) | candidates)


def next_start(a):
    for s in starts:  # small list walk is fine for the number of candidates involved
        if s > a:
            return s
    return max(e for _, e in code)


def extent(start):
    limit = next_start(start)
    seen, work, hi = set(), [start], start
    while work:
        a = work.pop()
        while start <= a < limit and a not in seen:
            seen.add(a)
            hi = max(hi, a)
            ins = word(a)
            op = ins >> 26
            if ins == 0x4E800420 and a in bctr_targets:
                work.extend(bctr_targets[a])
                break
            if ins in (0x4E800020, 0x4E800420):
                break
            if op == 18:
                tgt = (0 if ins & 2 else a) + sext(ins & 0x03FFFFFC, 26)
                if ins & 1:
                    a += 4
                    continue
                if start <= tgt < limit:
                    work.append(tgt)
                break
            if op == 16:
                tgt = (0 if ins & 2 else a) + sext(ins & 0xFFFC, 16)
                if not ins & 1 and start <= tgt < limit:
                    work.append(tgt)
                if ((ins >> 21) & 0x14) == 0x14 and not ins & 1:
                    break
            a += 4
    return hi + 4 - start


print('# GENERATED by tools/scripts/find_code_pointers.py -- functions reachable only via data pointers')
print('# (vtables/callbacks in an engine built without RTTI). Regenerate instead of editing.')
print('[functions]')
for c in sorted(candidates):
    print(f'"0x{c:08X}" = {{ size = 0x{extent(c):X} }}')
print(f'# {len(candidates)} functions', file=sys.stderr)
