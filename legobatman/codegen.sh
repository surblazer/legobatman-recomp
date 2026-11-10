#!/bin/bash
# Regenerate the C++ for LEGO Batman and apply the post-codegen steps. Always use this instead of a bare
# `rexglue codegen`: a fresh codegen silently undoes the direct-call rewrite.
#   1. (optional, --scan) refresh functions_data_refs.toml from the executable
#   2. rexglue codegen (nfsmw-nx SDK generator)
#   3. direct calls (tools/scripts/direct_calls.py)
#   4. register-local safety checks: functions that read registers they were handed, or write non-volatile
#      registers without a prologue, must be marked share_registers (or listed as known exceptions)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/.."
cd "$here"
if [ "${1:-}" = "--scan" ]; then
    python3 -I "$root/tools/scripts/find_code_pointers.py" "$root/work/image.bin" "$root/work/pdata.txt" \
        "$root/work/xexdump.txt" "$root/work/jump_tables.toml" > functions_data_refs.toml
fi
"$root/tools/nfsmw-nx/sdk/out/linux-amd64/rexglue" codegen legobatman_manifest.toml > codegen-nx.log 2>&1 \
    || { tail -20 codegen-nx.log; exit 1; }
# Hooked functions (src/*.cpp REX_HOOK_RAW) must keep their weak-alias calls
python3 -I "$root/tools/scripts/direct_calls.py" generated/default --keep 0x82396360
python3 -I "$root/tools/scripts/check_reads_before_writes.py" generated/default > rbw.txt 2>/dev/null
# Known exceptions: 826BB770 = game setjmp (replaced by ppc_setjmp); 823872A4 = a data word analysis took for code (never runs)
unexpected=$(grep -vE '^sub_(826BB770|823872A4) ' rbw.txt || true)
if [ -n "$unexpected" ]; then
    echo "Functions read register-locals before writing them; mark them share_registers in overrides.toml:" >&2
    echo "$unexpected" | cut -c1-160 >&2
    exit 1
fi
# Prologue-less functions that write non-volatile registers (outputs lost under non_volatile_as_local).
# Known exceptions: 823872A4 (data), 826BB330 (longjmp, native), 8238711C (C++ EH context restore).
python3 -I "$root/tools/scripts/check_nonvolatile_writers.py" generated/default overrides.toml > nvw.txt 2>/dev/null
unexpected=$(grep -vE '^sub_(823872A4|826BB330|8238711C) ' nvw.txt || true)
if [ -n "$unexpected" ]; then
    echo "Functions write non-volatile registers without a prologue; review and mark share_registers:" >&2
    echo "$unexpected" >&2
    exit 1
fi
echo "codegen OK"
