"""Rewrite direct calls between game functions to bypass their weak aliases.

The generated code calls `sub_X(ctx, base)`, and `sub_X` is a weak alias of `__imp__sub_X` (so a hook can replace
it at link time). Compilers never inline through a weak alias, not even with LTO. This project has no hooks, so every
direct call is rewritten to `__imp__sub_X(ctx, base)` (the idea of nfsmw-nx's tools/llamadas_directas.py). The
indirect-call table is untouched. Functions listed in --keep (hooked ones, if any are added later) are left alone.

Run it after every codegen: regenerating silently restores the weak calls.
Usage: python3 -I direct_calls.py legobatman/generated/default [--keep 0x82123456,...] [--undo]
"""
import glob
import re
import sys

gen = sys.argv[1]
undo = '--undo' in sys.argv
keep = set()
if '--keep' in sys.argv:
    keep = {a.upper().removeprefix('0X') for a in sys.argv[sys.argv.index('--keep') + 1].split(',')}

if undo:
    pattern = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    repl = lambda m: f'sub_{m.group(1)}(ctx, base);'
else:
    pattern = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    repl = lambda m: m.group(0) if m.group(1) in keep else f'__imp__sub_{m.group(1)}(ctx, base);'

changed_files = total = 0
for path in sorted(glob.glob(gen + '/*_recomp.*.cpp')):
    text = open(path, encoding='utf-8').read()
    new, n = pattern.subn(repl, text)
    if new != text:
        open(path, 'w', encoding='utf-8').write(new)
        changed_files += 1
        total += n
print(f'{"restored" if undo else "rewrote"} {total} calls in {changed_files} files')
