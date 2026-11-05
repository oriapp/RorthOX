#!/usr/bin/env bash
#
# Ticks (syscall 19): rorthox_get_ticks() returns the kernel's delivered timer ticks, takes no
# operands and never reads the user stack.
#
# 1. Host check (scripts/tests/host/clock.c, ASan + UBSan): the real handler returns
#    pit_get_ticks() unchanged for 0, 1, 0x7fffffff, 0x80000000, 0xfffffffe and 0xffffffff,
#    reads the clock once, and makes no task lookup, no stack read and no frame access.
# 2. The stdlib wrapper in libtest.elf is exactly push %ebp; mov %esp,%ebp; mov $SYSCALL_GET_TICKS,%eax;
#    int $0x80; pop %ebp; ret: eax is set right before the int and returned as the int left it
#    (a high-bit value is not an error). Anything else, or a wrapper objdump can't read, fails.
# 3. QEMU: copy bin/os.bin, add programs/libtest to the copy only, and run "libtest.elf clock":
#    three sleep(50) take 7..100 ticks each by the new clock, a busy loop sees the value go up
#    5 times (by 1..100 each), int 0x80 with a garbage esp (0, 0xdeadbeef, 0xfffffffc,
#    0x3ff001), repeated for at least 30 ticks so timer IRQs also arrive while esp is garbage,
#    returns a value between the calls before and after it and gets its esp back,
#    and the wrapper keeps ebx, esi, edi, ebp, esp and the stack word above its arguments.
#    The shell then still runs blank.elf. bin/os.bin itself must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60   STEP_TIMEOUT=90

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-90}"
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh

OUT=build/tests/clock
PROGRAM=programs/libtest/build/libtest.elf
mkdir -p "$OUT"
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] && [ -f build/config/rorthox-config.h ] ||
    { echo "clock: run 'make all' first" >&2; exit 1; }

echo "clock: host check of the syscall handler (AddressSanitizer, UndefinedBehaviorSanitizer)"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address,undefined \
        -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/clock.c -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
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
check "host check: every handler case passed"     "$OUT/host-check.log" "clock host check: 0 of 6 cases failed" line

echo "clock: the stdlib wrapper"
number=$(awk '$1 == "#define" && $2 == "SYSCALL_GET_TICKS" { print $3 }' build/config/rorthox-config.h)
vector=$(awk '$1 == "#define" && $2 == "RORTHOX_SYSCALL_VECTOR" { print $3 }' build/config/rorthox-config.h)
# The stdlib's wrappers are in the .asm section, which -d skips.
i686-elf-objdump -D --no-show-raw-insn "$PROGRAM" |
    awk '/^[0-9a-f]+ <rorthox_get_ticks>:/ { on = 1; next } on && /^$/ { on = 0 }
         on { sub(/^ *[0-9a-f]+:\t/, ""); sub(/[ \t]+$/, ""); print }' \
    > "$OUT/wrapper.txt"
sed 's/^/  /' "$OUT/wrapper.txt"
# The only accepted wrapper: these six instructions, nothing before, between or after them.
printf '%s\n' 'push   %ebp' 'mov    %esp,%ebp' "$(printf 'mov    $0x%x,%%eax' $((number)))" \
    "$(printf 'int    $0x%x' $((vector)))" 'pop    %ebp' 'ret' > "$OUT/wrapper-expected.txt"
awk -v int_line="$(printf 'int    $0x%x' $((vector)))" \
    -v mov_line="$(printf 'mov    $0x%x,%%eax' $((number)))" \
    '$0 == int_line { if (prev == mov_line) print "number"; exit } { prev = $0 }' \
    "$OUT/wrapper.txt" > "$OUT/wrapper-check.txt"
if [ -n "$number" ] && [ -n "$vector" ] && cmp -s "$OUT/wrapper.txt" "$OUT/wrapper-expected.txt"; then
    echo "exact" >> "$OUT/wrapper-check.txt"
fi
check "wrapper: eax = SYSCALL_GET_TICKS ($number) right before the int" "$OUT/wrapper-check.txt" "number" line
check "wrapper: exactly the six expected instructions"      "$OUT/wrapper-check.txt" "exact" line

hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
cp bin/os.bin "$OUT/clock.bin"
mcopy -i "$OUT/clock.bin" "$PROGRAM" ::libtest.elf

echo "clock: boot"
qemu_start "$OUT/clock.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "libtest.elf clock"
    if wait_screen "libtest clock: " "$STEP_TIMEOUT" && wait_screen "Process was terminated" "$STEP_TIMEOUT"; then
        type_line "blank.elf after"
        # blank.elf prints its arguments one per line: wait for "after" as a whole line.
        deadline=$((SECONDS + STEP_TIMEOUT))
        until grep -qxF after "$Q_OUT/screen.txt" || ((SECONDS >= deadline)) || ! qemu_running; do
            sleep 0.5
            screen_snapshot
        done
    fi
fi
screen_snapshot
qemu_stop || true
[ "$hash_before" = "$(sha256sum bin/os.bin | cut -d' ' -f1)" ] && echo "unchanged" > "$OUT/image.txt" ||
    echo "changed" > "$OUT/image.txt"

S="$OUT/screen.txt"
check "no kernel panic (debugcon)"                        "$OUT/debugcon.log" "PANIC" absent
check "three sleep(50) took 7..100 ticks each"            "$S" "lt-t: sleeps ok" line
check "busy loop: the value went up 5 times, 1..100 each" "$S" "lt-t: advanced 5 times" line
check "garbage esp: ticks returned, esp given back"       "$S" "lt-t: garbage esp ok" line
check "registers and stack kept by the wrapper"           "$S" "lt-t: ebx esi edi ebp esp and stack kept" line
check "the program finished"                              "$S" "libtest clock: done" line
check "no FAIL line"                                      "$S" "FAIL" absent
check "blank.elf started from the shell afterwards"       "$S" "after" line
check "bin/os.bin not changed by the test"                "$OUT/image.txt" "unchanged" line
finish clock
