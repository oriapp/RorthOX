#!/usr/bin/env bash
#
# strtok(str, ...) must start over when it gets a new string, even when the last string isn't
# finished (K25: the user library's strtok kept going through the old string).
#
# 1. Host check (scripts/tests/host/strtok.c): the real programs/stdlib/src/string.c built with
#    the host C compiler, every symbol prefixed with "user_" so it cannot clash with the host C
#    library. Fixed steps (a new string after an unfinished one, after a one-token string, after
#    an empty string, leading/repeated/trailing delimiters, calls after the end) and many random
#    calls compared with a model of strtok: token start and the bytes of every string.
# 2. QEMU: copy bin/os.bin, add programs/libtest to the copy only, boot, run
#    "libtest.elf strtok": the same fixed steps in a user program, under the task's paging.
# bin/os.bin must not change.
#
# Env: HOST_CC=cc   OBJCOPY=objcopy   BOOT_TIMEOUT=60 (seconds to wait for the shell)
#      RUN_TIMEOUT=30 (for each step)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
OBJCOPY="${OBJCOPY:-objcopy}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh

OUT=build/tests/strtok
PROGRAM=programs/libtest/build/libtest.elf
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.o "$OUT"/*.txt "$OUT"/*.bin "$OUT"/screen.ppm "$OUT"/debugcon.log "$OUT"/monitor.log
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] || { echo "strtok: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)

echo "strtok: host check of the user library's strtok"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -ffreestanding -fno-builtin -fno-stack-protector -w \
        -I programs/stdlib/src -I build/config \
        -c programs/stdlib/src/string.c -o "$OUT/user-string.o" > "$OUT/host-build.log" 2>&1 &&
    "$OBJCOPY" --prefix-symbols=user_ "$OUT/user-string.o" "$OUT/user-string-prefixed.o" >> "$OUT/host-build.log" 2>&1 &&
    "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -w -I scripts/tests/host \
        scripts/tests/host/strtok.c "$OUT/user-string-prefixed.o" \
        -o "$OUT/host-check" >> "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 30
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: fixed steps ran"                "$OUT/host-check.log"  "fixed steps: 35 checks" line
check "host check: random calls ran"               "$OUT/host-check.log"  "random calls: 19200 calls, 38400 checks" line
check "host check: every check passed"             "$OUT/host-check.log"  "strtok host check: 0 of 38435 checks failed" line

# prompts: the number of shell prompts below the shell banner.
prompts() {
    sed -n "/${RORTHOX_NAME} v${RORTHOX_VERSION}/,\$p" "$Q_OUT/screen.txt" | tr -cd '>' | wc -c
}

# wait_prompts <n>: wait until the shell printed its prompt <n> times (it is waiting for keys).
wait_prompts() {
    local deadline=$((SECONDS + RUN_TIMEOUT))
    while ((SECONDS < deadline)); do
        qemu_running || return 1
        screen_snapshot
        [ "$(prompts)" -ge "$1" ] && return 0
        sleep 0.5
    done
    echo "  gave up waiting for shell prompt $1"
    return 1
}

IMG="$OUT/strtok.bin"
cp bin/os.bin "$IMG"
mcopy -i "$IMG" "$PROGRAM" ::

echo "strtok: run libtest.elf strtok"
: > "$OUT/steps.txt"
qemu_start "$IMG" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && wait_prompts 1; then
    type_line "libtest.elf strtok"
    if wait_screen "libtest strtok: start" "$RUN_TIMEOUT"; then
        echo "started" >> "$OUT/steps.txt"
        if wait_screen "checks failed" "$RUN_TIMEOUT" && wait_screen "Process was terminated" "$RUN_TIMEOUT"; then
            echo "ended" >> "$OUT/steps.txt"
        fi
    fi
fi
qemu_stop || true

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "libtest strtok started"                      "$OUT/steps.txt"    "started" line
check "libtest strtok ended"                        "$OUT/steps.txt"    "ended" line
check "user strtok: every step passed"              "$Q_OUT/screen.txt" "libtest strtok: 0 of 27 checks failed"
check "user strtok: no failed step"                 "$Q_OUT/screen.txt" "libtest strtok: FAIL" absent

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish strtok
