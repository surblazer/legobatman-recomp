"""Find generated functions without a prologue that write non-volatile registers (r14-r31, f14-f31, v14-v31, v64+).

Compiled functions save those registers in their prologue before using them. Hand-written routines (vector maths,
audio) sometimes write them with no prologue at all, returning results the caller reads back. Under
non_volatile_as_local the caller's copies are separate C++ locals, so those results are lost unless the routine is
marked share_registers (this caused artifacts in the first story cutscene). Functions that save/restore inline, or
are listed in the known-exceptions file, are fine; review new hits by hand.

Usage: python3 -I check_nonvolatile_writers.py generated/default overrides.toml
Prints functions that write non-volatile registers, have no standard prologue and are not marked share_registers.
"""
import glob
import re
import sys

gen, overrides = sys.argv[1], sys.argv[2]
marked = set(re.findall(r'"0x([0-9A-F]{8})" = \{[^}]*share_registers = true', open(overrides).read()))

fn_re = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)')
nv = re.compile(r'^(r(?:1[4-9]|2[0-9]|3[01])|f(?:1[4-9]|2[0-9]|3[01])|'
                r'v(?:1[4-9]|2[0-9]|3[01]|6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7]))\b')
store_v = re.compile(r'^simde_mm_store\w*\(\(?(?:simde__m128i\*\))?(v\d+)\.')
assign = re.compile(r'^([a-z0-9]+)\.\w+(\[\d+\])?\s*=[^=]')
prologue = re.compile(r'^// (mflr r\d+|stwu r1,|std r(1[4-9]|2\d|3[01]),-\d+\(r1\)|stfd f\d+,-\d+\(r1\))')

hits = []
for path in sorted(glob.glob(gen + '/*_recomp.*.cpp')):
    cur = None
    for line in open(path, encoding='utf-8', errors='ignore'):
        m = fn_re.match(line)
        if m:
            cur, first, writes = m.group(1), None, set()
            continue
        if cur is None:
            continue
        if line.startswith('}'):
            if writes and cur not in marked and not (first and prologue.match(first)):
                hits.append((cur, first or '', sorted(writes)))
            cur = None
            continue
        s = line.strip()
        if s.startswith('// ') and first is None:
            first = s
        if s.startswith('//') or s.startswith('const auto s_'):
            continue
        a = assign.match(s)
        if a and nv.match(a.group(1)):
            writes.add(a.group(1))
        vm = store_v.match(s)
        if vm and nv.match(vm.group(1)):
            writes.add(vm.group(1))

for addr, first, regs in hits:
    print(f'sub_{addr} {first[:30]:30} {" ".join(regs)[:80]}')
print(f'TOTAL {len(hits)}', file=sys.stderr)
