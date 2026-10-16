#!/usr/bin/env bash
# Bounded, owned, readable syscall-7 nodes; rejected lists never publish a child.
set -euo pipefail
cd "$(dirname "$0")/../.."
. scripts/qemu-lib.sh
. scripts/tests/lib/syscheck.sh
OUT=build/tests/command-arguments
mkdir -p "$OUT"
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log"
host_failed=0
if "${HOST_CC:-cc}" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w \
    -I kernel -I build/config -I scripts/tests/host \
    scripts/tests/host/command-arguments.c scripts/tests/host/process-host.c scripts/tests/host/kalloc.c \
    kernel/isr80h/process.c kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c \
    kernel/string/string.c kernel/memory/memory.c -Wl,--wrap=kzalloc \
    -Wl,--wrap=task_copy_from_user -Wl,--wrap=process_load_with_arguments -Wl,--wrap=process_switch \
    -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    "$OUT/host-check" programs/blank/build/blank.elf > "$OUT/host-check.log" 2>&1 || host_failed=1
    cat "$OUT/host-check.log"
else
    cat "$OUT/host-build.log"
    host_failed=1
fi
. scripts/tests/lib/command-limit.sh
check_command_limit
[ "${HOST_ONLY:-0}" = 0 ] || exit "$host_failed"
base_sum=$(sha256sum bin/os.bin)
IMAGE="$OUT/os.bin"
cp bin/os.bin "$IMAGE"
mcopy -o -i "$IMAGE" programs/syscheck/build/syscheck.elf ::syscheck.elf
for mode in ${COMMAND_CASES:-self two bad badsp7 freeroot freenext freesp one mixed readnode max boundary}; do
    case "$mode" in self|two|bad|badsp7|free*) expected='syscheck: PASS';; *) expected='syscheck: child PASS';; esac
    run_syscheck "cmd$mode" "$expected"
done
[ "$base_sum" = "$(sha256sum bin/os.bin)" ] || { echo 'FAIL shipped image changed'; failures=$((failures+1)); }
failures=$((failures+host_failed))
finish command-arguments
