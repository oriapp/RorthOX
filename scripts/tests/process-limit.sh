#!/usr/bin/env bash
#
# Each program may allocate at most PROCESS_HEAP_LIMIT_BYTES (config.mk) with malloc (syscall 4),
# charged in whole 4 KiB pages, and its argument array and argument strings count too. A request
# over the budget gets malloc's usual NULL while the kernel heap still has room; free gives the
# charge back. Each program has a budget of its own, so this is not a limit on the whole heap:
# a program can still use up the kernel heap by starting children with syscalls 6 and 7 (each child
# gets a new budget, a page directory and its own memory, and there is no syscall to end a child).
#
# 1. Host check (scripts/tests/host/process-limit.c), built twice: with the shipped config, and
#    with a copy of build/config where the budget is 64 KiB (16 pages), so the budget runs out
#    before the 1024 allocation slots do. The real kernel/task/process.c on the stand-ins of
#    process-host.c: the exact limit, page rounding, sizes of 0, sizes whose rounding or sum wraps,
#    slot exhaustion, a heap or mapping failure, a failed free and invalid frees, argument
#    injection that the budget, the heap or a mapping refuses, a corrupted usage above the limit
#    (the budget must not reopen), and a free whose charge is more than the usage (the kernel must
#    stop before the usage wraps). The usage always equals the page-rounded sizes of the live
#    allocations.
# 2. QEMU, the shipped budget: a copy of bin/os.bin gets programs/budget. budget.elf fills
#    its budget to the byte, is refused one byte more, frees and allocates again, checks that 1
#    byte costs a page and that bad frees give nothing back. Then, holding its whole budget, it
#    starts "budget.elf child", which must still get its own whole budget (the heap had room
#    when the parent was refused) and be refused one byte more. bin/os.bin itself must not change.
#
# Env: HOST_CC=cc   HOST_ONLY=1 (host checks only)
#      BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for each program)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/process-limit
PROGRAM=programs/budget/build/budget.elf
BLANK_ELF=programs/blank/build/blank.elf
SMALL_LIMIT=65536
mkdir -p "$OUT"
rm -rf "$OUT/small-config"
rm -f "$OUT"/host-check-* "$OUT"/host-build-*.log "$OUT"/host-check-*.log "$OUT"/host-status-*.txt
[ -f "$BLANK_ELF" ] || { echo "process-limit: $BLANK_ELF not found, run 'make all' first" >&2; exit 1; }

# The smaller budget: the shipped config with only PROCESS_HEAP_LIMIT_BYTES changed.
mkdir -p "$OUT/small-config"
sed "s/^#define PROCESS_HEAP_LIMIT_BYTES .*/#define PROCESS_HEAP_LIMIT_BYTES $SMALL_LIMIT/" \
    build/config/rorthox-config.h > "$OUT/small-config/rorthox-config.h"
diff build/config/rorthox-config.h "$OUT/small-config/rorthox-config.h" > "$OUT/small-config/diff.txt" || true

host_check() {
    local name="$1" config="$2" status="not built"
    echo "process-limit: host check, $name budget"
    if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w -Wl,--wrap=PANIC \
            -I kernel -I "$config" -I build/config -I scripts/tests/host \
            scripts/tests/host/process-limit.c scripts/tests/host/process-host.c scripts/tests/host/kalloc.c \
            kernel/isr80h/process.c kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c \
            kernel/string/string.c kernel/memory/memory.c \
            -o "$OUT/host-check-$name" > "$OUT/host-build-$name.log" 2>&1; then
        if "$OUT/host-check-$name" "$BLANK_ELF" > "$OUT/host-check-$name.log" 2>&1; then status=0; else status=$?; fi
        sed 's/^/  /' "$OUT/host-check-$name.log"
    else
        echo "  host build failed, see $OUT/host-build-$name.log"
        head -n 20 "$OUT/host-build-$name.log" | sed 's/^/  /' || true
        : > "$OUT/host-check-$name.log"
    fi
    echo "exit=$status" > "$OUT/host-status-$name.txt"
}
host_check shipped build/config
host_check small "$OUT/small-config"

check "small config differs only in the budget" "$OUT/small-config/diff.txt" \
    "> #define PROCESS_HEAP_LIMIT_BYTES $SMALL_LIMIT"
[ "$(grep -c '^[<>]' "$OUT/small-config/diff.txt")" = 2 ] && echo "two lines" > "$OUT/small-config/count.txt" ||
    echo "other changes" > "$OUT/small-config/count.txt"
check "small config changes one line"            "$OUT/small-config/count.txt"   "two lines" line
check "host check (shipped budget) exited 0"     "$OUT/host-status-shipped.txt"  "exit=0" line
check "host check (shipped budget) is 8 MiB"     "$OUT/host-check-shipped.log"   "budget $PROCESS_HEAP_LIMIT_BYTES bytes (2048 pages), 1024 allocation slots" line
check "host check (shipped budget) all passed"   "$OUT/host-check-shipped.log"   "process-limit host check: 0 of 29 cases failed" line
check "host check (64 KiB budget) exited 0"      "$OUT/host-status-small.txt"    "exit=0" line
check "host check (64 KiB budget) is 16 pages"   "$OUT/host-check-small.log"     "budget $SMALL_LIMIT bytes (16 pages), 1024 allocation slots" line
check "host check (64 KiB budget) all passed"    "$OUT/host-check-small.log"     "process-limit host check: 0 of 29 cases failed" line
if [ "${HOST_ONLY:-0}" != 0 ]; then
    exit $((failures != 0))
fi

[ -f "$PROGRAM" ] || { echo "process-limit: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/process-limit.bin"
mcopy -i "$OUT/process-limit.bin" "$PROGRAM" ::

echo "process-limit: boot a copy of bin/os.bin with budget.elf and run it"
qemu_start "$OUT/process-limit.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "budget.elf"
    if ! wait_screen "budget child: done" "$RUN_TIMEOUT"; then
        echo "  budget child did not finish"
        failures=$((failures + 1))
    elif ! wait_screen "Process was terminated" "$RUN_TIMEOUT"; then
        # The parent never ends, so this is the child.
        echo "  budget child did not end"
        failures=$((failures + 1))
    elif ! wait_screen "budget parent: child started" "$RUN_TIMEOUT"; then
        echo "  budget parent did not run on after starting the child"
        failures=$((failures + 1))
    fi
fi
qemu_stop || true
[ "$hash_before" = "$(image_hash)" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

argv_used=$((4096 + 4096 + 2 * 4096))
check "no kernel panic (debugcon)"                "$OUT/debugcon.log" "PANIC" absent
check "typed command echoed by the shell"         "$OUT/screen.txt"   "> budget.elf"
check "parent knows the budget and its arguments" "$OUT/screen.txt"   "budget parent: budget $PROCESS_HEAP_LIMIT_BYTES, in use $argv_used"
check "parent: no check failed"                   "$OUT/screen.txt"   "budget parent: FAIL" absent
check "parent: all 11 checks passed"              "$OUT/screen.txt"   "budget parent: 0 of 11 checks failed"
check "parent started the child with its budget held" "$OUT/screen.txt" "budget parent: holds its whole budget, starting the child"
check "child: no check failed"                    "$OUT/screen.txt"   "budget child: FAIL" absent
check "child: both checks passed"                 "$OUT/screen.txt"   "budget child: 0 of 2 checks failed"
check "child ended"                               "$OUT/screen.txt"   "Process was terminated"
check "parent runs on after starting the child"   "$OUT/screen.txt"   "budget parent: child started"
check "bin/os.bin not changed by the test"        "$OUT/image.txt"    "unchanged" line
finish process-limit
