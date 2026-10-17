#!/usr/bin/env bash
#
# Program lookup: syscalls 6 and 7 look a bare program name (no '/' or ':') up in the directories
# of PROGRAM_SEARCH_PATHS in config.mk, in order ("0:/" and then "0:/bin/"), and go on to the next
# one only when the name isn't there. Any other load error stops the lookup.
#
# 1. Host check (scripts/tests/host/program-lookup.c, AddressSanitizer): the real syscall 6 and 7
#    handlers with a recording loader; order, first match wins, only "not found" goes on, names
#    with '/' or ':' are tried once from the root, length bounds per directory, and the results
#    callers see (-EIO when nothing was found). See that file for the cases.
# 2. QEMU: a copy of bin/os.bin gets programs/looktest as 0:/looktest.elf, blank.elf as 0:/pick.elf,
#    and in 0:/brk.elf the first 40 bytes of blank.elf (ELF magic, broken header), an empty
#    0:/empty.elf and 0:/big.elf one byte over the raw program limit. Directory 0:/bin/ gets copies
#    of looktest.elf named binonly.elf, pick.elf, brk.elf, empty.elf and big.elf; started that way,
#    looktest.elf prints "looktest: payload <argc> <argv...>". One session, each step waits until the
#    screen is idle:
#      neg       looktest.elf asks syscall 7 for brk.elf, empty.elf, big.elf (refused, -2) and
#                nosuch.elf (-1), and syscall 6 for brk.elf and nosuch.elf (0): no copy in bin/ runs
#      s6 root   syscall 6 starts pick.elf: blank.elf from the root runs, not the bin/ copy
#      s6 bin    syscall 6 starts binonly.elf from bin/
#      shell bin the shell starts "binonly.elf shtag" from bin/, argv[0] is the name as typed
#      s7 bin    syscall 7 starts binonly.elf from bin/ with its arguments unchanged
#      shell/s7 root   "pick.elf ..." runs blank.elf from the root, not the bin/ copy
#    and blank.elf still starts afterwards. bin/os.bin must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60   RUN_TIMEOUT=20 (per step)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-20}"
. ./scripts/qemu-lib.sh

OUT=build/tests/program-lookup
LOOKTEST=programs/looktest/build/looktest.elf
BLANK=programs/blank/build/blank.elf
IMAGE="$OUT/program-lookup.bin"
mkdir -p "$OUT"
[ -f bin/os.bin ] && [ -f "$LOOKTEST" ] && [ -f "$BLANK" ] || { echo "program-lookup: run 'make all' first" >&2; exit 1; }
grep -qx 'PROGRAM_SEARCH_PATHS = 0:/ 0:/bin/' config.mk ||
    { echo "program-lookup: PROGRAM_SEARCH_PATHS changed, update this test" >&2; exit 1; }
rm -rf "$OUT/files"
rm -f "$OUT"/*.txt "$OUT"/*.log "$OUT"/*.bin "$OUT"/host-*
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)

echo "program-lookup: host check of the lookup (AddressSanitizer)"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w -fsanitize=address -fno-omit-frame-pointer \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/program-lookup.c scripts/tests/host/kalloc.c kernel/isr80h/process.c \
        kernel/string/string.c kernel/memory/memory.c -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" 0:/ 0:/bin/ > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 30
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "no AddressSanitizer report"                 "$OUT/host-check.log"  "AddressSanitizer" absent
check "host check: every lookup case passed"       "$OUT/host-check.log"  "program-lookup host check: 0 of 6 cases failed" line

# The test image: only copies, never bin/os.bin itself.
mkdir -p "$OUT/files"
head -c 40 "$BLANK" > "$OUT/files/brk.elf"
: > "$OUT/files/empty.elf"
head -c $((0x100001)) /dev/zero > "$OUT/files/big.elf"
cp bin/os.bin "$IMAGE"
mcopy -i "$IMAGE" "$LOOKTEST" ::looktest.elf
mcopy -i "$IMAGE" "$BLANK" ::pick.elf
mcopy -i "$IMAGE" "$OUT/files/brk.elf" "$OUT/files/empty.elf" "$OUT/files/big.elf" ::
mmd -i "$IMAGE" ::bin
for name in binonly pick brk empty big; do
    mcopy -i "$IMAGE" "$LOOKTEST" "::bin/$name.elf"
done
{
    for f in looktest.elf:"$LOOKTEST" pick.elf:"$BLANK" brk.elf:"$OUT/files/brk.elf" empty.elf:"$OUT/files/empty.elf" \
             big.elf:"$OUT/files/big.elf" bin/binonly.elf:"$LOOKTEST" bin/pick.elf:"$LOOKTEST" bin/brk.elf:"$LOOKTEST" \
             bin/empty.elf:"$LOOKTEST" bin/big.elf:"$LOOKTEST"; do
        mcopy -n -i "$IMAGE" "::${f%%:*}" "$OUT/files/readback" && cmp -s "$OUT/files/readback" "${f#*:}" &&
            echo "same ${f%%:*}"
    done
} > "$OUT/fixture.txt" 2>&1 || true
[ "$(grep -c '^same ' "$OUT/fixture.txt")" -eq 10 ] && echo "all 10" >> "$OUT/fixture.txt"
check "fixture: every file reads back from the test image" "$OUT/fixture.txt" "all 10" line

# Wait until three screen reads 0.4 s apart are the same: the step's programs have ended and the
# shell waits for the next line.
wait_idle() {
    local deadline=$((SECONDS + 15)) last="" now same=0
    while ((SECONDS < deadline)); do
        qemu_running || return 1
        screen_snapshot
        now=$(cat "$Q_OUT/screen.txt" 2> /dev/null || true)
        if [ "$now" = "$last" ]; then same=$((same + 1)); else same=0; fi
        [ "$same" -ge 2 ] && return 0
        last="$now"
        sleep 0.4
    done
    echo "  screen still changing after 15 s"
    return 1
}

# Screen lines without the shell prompt the shell may print in front of a program's first line.
lines() {
    sed 's/^> //' "$1" 2> /dev/null || true
}

# step <tag> <command> <line to wait for>: type the command, wait for that whole line (not the
# echoed command) and for the screen to settle, and keep the screen, without prompts in front of
# lines, as step-<tag>.txt (the screen scrolls, so the checks read these).
step() {
    local tag="$1" command="$2" text="$3" deadline
    if qemu_running; then
        type_line "$command"
        deadline=$((SECONDS + RUN_TIMEOUT))
        while ((SECONDS < deadline)) && qemu_running; do
            screen_snapshot
            lines "$Q_OUT/screen.txt" | grep -qxF -- "$text" && break
            sleep 0.5
        done
        lines "$Q_OUT/screen.txt" | grep -qxF -- "$text" && echo "  $tag: \"$text\" after ${SECONDS} s" ||
            echo "  $tag: gave up waiting for \"$text\""
        wait_idle || true
    fi
    lines "$Q_OUT/screen.txt" > "$OUT/step-$tag.txt"
}

qemu_start "$IMAGE" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    step neg       "looktest.elf neg"                 "looktest: done" line
    step s6-root   "looktest.elf s6 pick.elf"         "Blank loaded"
    step s6-bin    "looktest.elf s6 binonly.elf"      "looktest: payload 0"
    step shell-bin "binonly.elf shtag"                "looktest: payload 2 binonly.elf shtag"
    step s7-bin    "looktest.elf s7 binonly.elf s7b"  "looktest: payload 2 binonly.elf s7b"
    step shell-root "pick.elf shpick"                 "shpick"
    step s7-root   "looktest.elf s7 pick.elf s7p"     "s7p"
    step recovery  "blank.elf recovery"               "recovery"
fi
qemu_stop || true

s="$OUT/step-neg.txt"
check "kernel reached the end of init (debugcon)"     "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                    "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"              "$s" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "neg: looktest finished"                        "$s" "looktest: done" line
check "neg: syscall 7, broken ELF in the root: -2"    "$s" "looktest: s7 brk.elf -2" line
check "neg: syscall 7, empty file in the root: -2"    "$s" "looktest: s7 empty.elf -2" line
check "neg: syscall 7, too big for raw in the root: -2" "$s" "looktest: s7 big.elf -2" line
check "neg: syscall 7, nowhere: -1"                   "$s" "looktest: s7 nosuch.elf -1" line
check "neg: syscall 6, broken ELF in the root: 0"     "$s" "looktest: s6 brk.elf 0" line
check "neg: syscall 6, nowhere: 0"                    "$s" "looktest: s6 nosuch.elf 0" line
check "neg: no copy from bin/ started"                "$s" "looktest: payload" absent
check "neg: nothing else started"                     "$s" "Blank loaded" absent
s="$OUT/step-s6-root.txt"
check "s6 root: blank.elf from the root started"      "$s" "Blank loaded" line
check "s6 root: the bin/ copy did not start"          "$s" "looktest: payload" absent
check "s6 bin: started from bin/"                     "$OUT/step-s6-bin.txt" "looktest: payload 0" line
check "shell bin: started from bin/ with argv as typed" "$OUT/step-shell-bin.txt" "looktest: payload 2 binonly.elf shtag" line
check "s7 bin: started from bin/ with its arguments"  "$OUT/step-s7-bin.txt" "looktest: payload 2 binonly.elf s7b" line
s="$OUT/step-shell-root.txt"
check "shell root: blank.elf from the root started"   "$s" "shpick" line
check "shell root: the bin/ copy did not start"       "$s" "looktest: payload 2 pick.elf shpick" absent
s="$OUT/step-s7-root.txt"
check "s7 root: blank.elf from the root started"      "$s" "s7p" line
check "s7 root: the bin/ copy did not start"          "$s" "looktest: payload 2 pick.elf s7p" absent
check "blank.elf still starts afterwards"             "$OUT/step-recovery.txt" "recovery" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish program-lookup
