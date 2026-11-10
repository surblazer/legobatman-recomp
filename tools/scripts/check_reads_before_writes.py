"""Find generated functions that can read a register-local before writing it (flow-sensitive).

With cr/xer/ctr/non_volatile_as_local, registers r14-r31, f14-f31, v14-v31/v64-v127, cr0-7, ctr and xer are C++
locals initialised to zero. A normal function never reads those before setting them on every path (it only saves
r14-r31/f14-f31/vector registers to the stack in its prologue). A function that does is a piece of a split
function entered with live registers: it must be marked share_registers so its callers pass them through ctx.

Each function is split into blocks at `loc_X:` labels; edges come from `goto loc_X` (conditional or not, including
switch cases) and fall-through. A register counts as written at a block's entry only if it is written on every path
from the function entry (intersection), so loops entered by a forward jump are no longer false alarms.

Usage: python3 -I check_reads_before_writes.py legobatman/generated/default
"""
import glob
import re
import sys

gen = sys.argv[1]
fn_re = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)')
label_re = re.compile(r'^(loc_[0-9A-F]+):')
goto_re = re.compile(r'goto (loc_[0-9A-F]+);')
reg = re.compile(r'(?<![.\w])(r(?:1[4-9]|2[0-9]|3[01])|f(?:1[4-9]|2[0-9]|3[01])|'
                 r'v(?:1[4-9]|2[0-9]|3[01]|6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])|cr[0-7]|ctr|xer)\b')
assign = re.compile(r'^([A-Za-z0-9_]+)(?:\.[A-Za-z0-9_\[\]]+)*\s*=[^=]')
# stvx/stvx128 of a vector register to memory (byte-swapped store of a single register): prologue save
vector_save = re.compile(r'^simde_mm_store_si128\(\(simde__m128i\*\)REX_RAW_ADDR\(ea\), simde_mm_shuffle_epi8\('
                         r'simde_mm_load_si128\(\(simde__m128i\*\)v\d+\.u8\)')
prologue_save = re.compile(r'^REX_STORE_U64\(ctx\.r1\.u32 \+ -?\d+, [rf]\d+\.(u64|f64)\);$')


def effect(s):
    """(registers read, register written or None) for one statement, after the false-alarm filters."""
    if prologue_save.match(s) or vector_save.match(s) or s.startswith('const auto s_'):
        return [], None
    s2 = re.sub(r',\s*xer\)', ')', s)          # compare(..., xer) only copies the summary-overflow bit
    s2 = re.sub(r'cr\d\.so = xer\.so;', '', s2)
    cm = re.match(r'^(cr[0-7])\.(compare|setFromMask)', s2)
    if cm:
        return reg.findall(s2[len(cm.group(1)):]), cm.group(1)
    vm = re.match(r'^simde_mm_store\w*\(\(?(?:simde__m128i\*\))?(v\d+)\.\w+,(.*)$', s2)
    if vm:
        return reg.findall(vm.group(2)), vm.group(1)
    am = assign.match(s2)
    if am:
        lhs = am.group(1)
        return reg.findall(s2[am.end() - 1:]), lhs if reg.fullmatch(lhs) else None
    return reg.findall(s2), None


# A function that starts with a normal prologue owns its registers: reads-before-writes there are path-correlated
# false alarms (the original reads them only on paths where they were set), never a fragment.
prologue_start = re.compile(r'^// (mflr r12|stwu r1,|std r3[01],-\d+\(r1\)|stfd f\d+,-\d+\(r1\))')


def analyse(lines):
    # blocks: list of (label, statements); statements are stripped lines
    blocks, cur_label, cur = [], '<entry>', []
    for s in lines:
        if s.startswith('// '):
            continue
        m = label_re.match(s)
        if m:
            blocks.append((cur_label, cur))
            cur_label, cur = m.group(1), []
        else:
            cur.append(s)
    blocks.append((cur_label, cur))
    index = {lab: i for i, (lab, _) in enumerate(blocks)}

    succ = []
    for i, (_, stmts) in enumerate(blocks):
        targets = set()
        falls = True
        for s in stmts:
            for t in goto_re.findall(s):
                targets.add(t)
            if s.startswith('goto ') or s.startswith('return') or s == 'return;':
                falls = False
        if falls and i + 1 < len(blocks):
            targets.add(blocks[i + 1][0])
        succ.append([index[t] for t in targets if t in index])

    ALL = None  # "top": every register (unknown yet)
    entry_in = [ALL] * len(blocks)
    entry_in[0] = frozenset()
    work = [0]
    out_cache = {}
    while work:
        i = work.pop()
        written = set(entry_in[i])
        for s in blocks[i][1]:
            _, w = effect(s)
            if w:
                written.add(w)
        out = frozenset(written)
        if out_cache.get(i) == out:
            continue
        out_cache[i] = out
        for j in succ[i]:
            new = out if entry_in[j] is ALL else entry_in[j] & out
            if new != entry_in[j]:
                entry_in[j] = new
                work.append(j)

    bad = {}
    for i, (_, stmts) in enumerate(blocks):
        if entry_in[i] is ALL:
            continue  # unreachable from the entry
        written = set(entry_in[i])
        for s in stmts:
            reads, w = effect(s)
            for r in reads:
                if r not in written:
                    bad.setdefault(r, s[:110])
            if w:
                written.add(w)
    return bad


hits = {}
for path in sorted(glob.glob(gen + '/*_recomp.*.cpp')):
    current, body = None, []
    for line in open(path, encoding='utf-8', errors='ignore'):
        m = fn_re.match(line)
        if m:
            current, body = m.group(1), []
            continue
        if current is None:
            continue
        if line.startswith('}'):
            first = next((x for x in body if x.startswith('// ')), '')
            bad = {} if prologue_start.match(first) else analyse(body)
            if bad:
                hits[current] = bad
            current = None
            continue
        s = line.strip()
        if s.startswith('// '):
            body.append(s)  # instruction comments: only the first one is used (prologue detection)
            continue
        if not s or s.startswith(('//', 'PPC', 'REX_FUNC_PROLOGUE', 'uint32_t ea')) or s.endswith('{};'):
            continue
        body.append(s)

for name in sorted(hits):
    regs = ' '.join(sorted(hits[name]))
    print(f'{name} {regs} | {next(iter(hits[name].values()))}')
print(f'TOTAL {len(hits)}', file=sys.stderr)
