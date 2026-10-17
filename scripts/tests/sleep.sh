#!/usr/bin/env bash
#
# Sleep (syscall 12): sleep(ms) blocks only the caller for at least ceil(ms / 10) + 2 timer ticks,
# while other tasks run, and then returns 0 with the caller's registers and stack as they were.
#
# 1. Host check (scripts/tests/host/sleep.c, ASan + UBSan): the real syscall handler reads its
#    operand once through the checked stack copy, returns 0 at once for 0, -EINVARG for negative
#    values and everything above INT32_MAX, a failed read's error, and for 1..INT32_MAX puts the
#    task to sleep until now + ticks (also across the 2^32 wrap) with its saved eax already 0.
#    Refused and zero requests don't sleep, don't switch tasks and change nothing in the task.
# 2. QEMU: copy bin/os.bin, add programs/libtest to the copy only, and run
#    "libtest.elf sleep <jiffies> <current_task> <state offset>" from the shell (addresses from the
#    kernel objects; ring 3 can read kernel memory, so the program times its sleeps in ticks):
#      - sleep(0) is 0, sleep(-1) and sleep(0x80000000) are -2;
#      - two sleep(50) return 0 after at least 7 ticks each;
#      - it starts "libtest.elf sleeppeer", which waits until the sleeper's task state is
#        sleeping and counts the ticks it runs on until it is runnable again, while the sleeper
#        runs sleep(1000): that returns 0 after at least 102 ticks, and the peer's ticks lie
#        inside the sleeper's own window (so the sleeper was really blocked and the peer ran);
#      - every sleep keeps ebx, esi, edi, ebp, esp and the stack word above its argument;
#      - both programs end, and the shell then still runs blank.elf.
# bin/os.bin itself must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60   STEP_TIMEOUT=60

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-60}"
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/kernel-symbols.sh

OUT=build/tests/sleep
PROGRAM=programs/libtest/build/libtest.elf
mkdir -p "$OUT"
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] && [ -f build/kernelfull.o ] || { echo "sleep: run 'make all' first" >&2; exit 1; }

echo "sleep: host check of the syscall handler (AddressSanitizer, UndefinedBehaviorSanitizer)"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address,undefined \
        -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/sleep.c -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 20
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
check "no sanitizer report"                       "$OUT/host-check.log" "runtime error" absent
check "no AddressSanitizer report"                "$OUT/host-check.log" "AddressSanitizer" absent
check "host check: every handler case passed"     "$OUT/host-check.log" "sleep host check: 0 of 67 cases failed" line

echo "sleep: kernel symbols"
if kernel_symbols "$OUT"; then echo "found" > "$OUT/symbols.txt"; else echo "missing" > "$OUT/symbols.txt"; fi
check "tick counter, current_task and struct task offsets found" "$OUT/symbols.txt" "found" line

hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
cp bin/os.bin "$OUT/sleep.bin"
mcopy -i "$OUT/sleep.bin" "$PROGRAM" ::libtest.elf

# wait_count <text> <count> <seconds>: wait until <count> screen lines contain <text>.
wait_count() {
    local deadline=$((SECONDS + $3)) n
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running, stopped waiting for \"$1\" x$2"; return 1; }
        screen_snapshot
        n=$(grep -cF -- "$1" "$Q_OUT/screen.txt" 2> /dev/null || true)
        if [ "${n:-0}" -ge "$2" ]; then
            echo "  \"$1\" x$2 on screen after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for \"$1\" x$2 after $3 s"
    return 1
}

# keep: add the current screen to screens.txt, so lines that scroll off later still count.
keep() { screen_snapshot; cat "$OUT/screen.txt" >> "$OUT/screens.txt"; }

echo "sleep: boot"
: > "$OUT/screens.txt"
qemu_start "$OUT/sleep.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "libtest.elf sleep $SYM_TICKS $SYM_CURRENT_TASK ${TASK_STATE_OFFSET:-0}"
    if wait_screen "libtest sleep: armed" "$STEP_TIMEOUT"; then
        qemu_cmd "sendkey a"
        wait_screen "lt-s: short sleeps" "$STEP_TIMEOUT" && keep
        # The peer ends when the sleeper wakes; then both programs end.
        if wait_screen "lt-s: slept" "$STEP_TIMEOUT" && wait_screen "lt-p: " "$STEP_TIMEOUT" &&
            wait_count "Process was terminated" 2 "$STEP_TIMEOUT"; then
            keep
            type_line "blank.elf after"
            wait_count "Process was terminated" 3 "$STEP_TIMEOUT" || true
        fi
    fi
fi
keep
qemu_stop || true

S="$OUT/screens.txt"
# The peer's ticks must lie in the sleeper's window, and it must have run on a good part of them.
window=$(sed -n 's/.*lt-s: slept r 0 ticks \([0-9]*\) from \([0-9]*\) to \([0-9]*\).*/\2 \3/p' "$S" | head -n 1)
peer=$(sed -n 's/.*lt-p: ran on \([0-9]*\) ticks while the sleeper slept, from \([0-9]*\) to \([0-9]*\).*/\1 \2 \3/p' "$S" | head -n 1)
echo "  sleeper window: ${window:-none}; peer ticks, first, last: ${peer:-none}"
if [ -n "$window" ] && [ -n "$peer" ]; then
    read -r start end <<< "$window"
    read -r count first last <<< "$peer"
    if [ "$first" -ge "$start" ] && [ "$last" -le "$end" ] && [ "$first" -le "$last" ] && [ "$count" -ge 10 ]; then
        echo "inside" > "$OUT/peer.txt"
    else
        echo "outside" > "$OUT/peer.txt"
    fi
else
    echo "no lines" > "$OUT/peer.txt"
fi
[ "$hash_before" = "$(sha256sum bin/os.bin | cut -d' ' -f1)" ] && echo "unchanged" > "$OUT/image.txt" ||
    echo "changed" > "$OUT/image.txt"

check "no kernel panic (debugcon)"                       "$OUT/debugcon.log" "PANIC" absent
check "sleep(0) 0, sleep(-1) -2, sleep(0x80000000) -2"   "$S" "lt-s: zero 0 negative -2 high -2"
check "two sleep(50): 0 after at least 7 ticks each"     "$S" "lt-s: short sleeps ok"
check "sleep(1000): 0 after at least 102 ticks"          "$S" "lt-s: long sleep ok"
check "registers and stack kept by every sleep"          "$S" "lt-s: ebx esi edi ebp esp and stack kept"
check "the peer saw the sleeper asleep"                  "$S" "lt-p: ran on"
check "the peer ran on 10+ ticks inside the sleep"       "$OUT/peer.txt" "inside" line
check "the sleeper finished"                             "$S" "libtest sleep: done"
check "no FAIL line"                                     "$S" "FAIL" absent
check "blank.elf started from the shell afterwards"      "$OUT/screen.txt" "after" line
check "bin/os.bin not changed by the test"               "$OUT/image.txt" "unchanged" line
finish sleep
