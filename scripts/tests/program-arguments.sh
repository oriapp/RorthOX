#!/usr/bin/env bash
# Full-range, caller-owned, writable syscall-8 output; no partial stores on refusal.
set -euo pipefail
cd "$(dirname "$0")/../.."
. scripts/qemu-lib.sh
. scripts/tests/lib/syscheck.sh
OUT=build/tests/program-arguments
mkdir -p "$OUT"
host_failed=0
for oracle in program-arguments user-paging; do
    rm -f "$OUT/$oracle" "$OUT/$oracle-build.log" "$OUT/$oracle.log"
    if [ "$oracle" = user-paging ]; then
        sources='kernel/memory/paging/paging.c'
    else
        sources='scripts/tests/host/process-host.c kernel/isr80h/process.c kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c kernel/string/string.c kernel/memory/memory.c'
    fi
    if "${HOST_CC:-cc}" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w \
        -I kernel -I build/config -I scripts/tests/host "scripts/tests/host/$oracle.c" \
        scripts/tests/host/kalloc.c $sources -o "$OUT/$oracle" > "$OUT/$oracle-build.log" 2>&1; then
        "$OUT/$oracle" programs/blank/build/blank.elf > "$OUT/$oracle.log" 2>&1 || host_failed=1
        cat "$OUT/$oracle.log"
    else
        cat "$OUT/$oracle-build.log"
        host_failed=1
    fi
done
[ "${HOST_ONLY:-0}" = 0 ] || exit "$host_failed"
base_sum=$(sha256sum bin/os.bin)
IMAGE="$OUT/os.bin"
cp bin/os.bin "$IMAGE"
mcopy -o -i "$IMAGE" programs/syscheck/build/syscheck.elf ::syscheck.elf
for mode in ${PROGRAM_CASES:-get badsp freeget freesp}; do
    if [ "$mode" != badsp ]; then expected='syscheck: PASS'; else expected='syscheck: result -2'; fi
    run_syscheck "$mode" "$expected"
done
[ "$base_sum" = "$(sha256sum bin/os.bin)" ] || { echo 'FAIL shipped image changed'; failures=$((failures+1)); }
failures=$((failures+host_failed))
finish program-arguments
