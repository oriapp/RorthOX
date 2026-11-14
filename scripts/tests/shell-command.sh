#!/usr/bin/env bash
#
# The shell's command list belongs to the user library: every command, run or refused, gives its
# nodes back, an allocation failure never runs a prefix of the command, and a command with more
# than MAX_COMMAND_ARGUMENTS words (config.mk, the program name included) is refused with a
# message that names the limit, without starting a program.
#
# 1. Host check (scripts/tests/host/shell-command.c): the real programs/stdlib/src/rorthox.c and
#    string.c built with the host C compiler, every symbol prefixed with "user_", with stand-ins
#    for the malloc, free and system syscalls. rorthox_parse_command() with 0, 1, MAX-1, MAX,
#    MAX+1 and more words, repeated, leading and trailing spaces and a tab inside a word, with
#    each node's allocation failing in turn, and many calls in a row; rorthox_system_run() the
#    same way: the syscall gets the whole list once, the list is freed after it returns and its
#    result is returned.
# 2. QEMU: boot a copy of bin/os.bin, count the free kernel heap blocks while the shell is idle,
#    press Enter on an empty line (nothing but a new prompt), type a command of MAX+1 words: the
#    row below it names the limit, then a new prompt, no program started, the shell alone and no
#    heap block kept. Then a command of MAX words with repeated spaces runs blank.elf with all its
#    arguments, and again no heap block is kept. The shell is idle when
#    scripts/tests/lib/guest-state.sh has stopped the guest at its key request with the shell
#    alone in the task list and the same shell owning the keyboard; the heap table is read through
#    the QEMU monitor right after.
# bin/os.bin must not change.
#
# Needs python3. Env: HOST_CC=cc   OBJCOPY=objcopy   BOOT_TIMEOUT=60 (seconds to wait for the shell)
#      RUN_TIMEOUT=30 (for each step)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
OBJCOPY="${OBJCOPY:-objcopy}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/shell-command
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.o "$OUT"/*.txt "$OUT"/*.txt.log "$OUT"/*.bin "$OUT"/screen.ppm "$OUT"/debugcon.log "$OUT"/monitor.log
[ -f bin/os.bin ] && [ -f build/config/rorthox-config.h ] || { echo "shell-command: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
MAX=$(awk '$1 == "#define" && $2 == "RORTHOX_MAX_COMMAND_ARGUMENTS" { print $3 }' build/config/rorthox-config.h)
MAX=$((MAX))

echo "shell-command: host check of the user library's command parser"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -ffreestanding -fno-builtin -fno-stack-protector -w \
        -I programs/stdlib/src -I build/config \
        -c programs/stdlib/src/rorthox.c -o "$OUT/user-rorthox.o" > "$OUT/host-build.log" 2>&1 &&
    "$HOST_CC" -std=gnu99 -g -O0 -ffreestanding -fno-builtin -fno-stack-protector -w \
        -I programs/stdlib/src -I build/config \
        -c programs/stdlib/src/string.c -o "$OUT/user-string.o" >> "$OUT/host-build.log" 2>&1 &&
    "$OBJCOPY" --prefix-symbols=user_ "$OUT/user-rorthox.o" "$OUT/user-rorthox-prefixed.o" >> "$OUT/host-build.log" 2>&1 &&
    "$OBJCOPY" --prefix-symbols=user_ "$OUT/user-string.o" "$OUT/user-string-prefixed.o" >> "$OUT/host-build.log" 2>&1 &&
    "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -w -I scripts/tests/host -I build/config \
        scripts/tests/host/shell-command.c "$OUT/user-rorthox-prefixed.o" "$OUT/user-string-prefixed.o" \
        -o "$OUT/host-check" >> "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    head -n 30 "$OUT/host-check.log" | sed 's/^/  /'
    [ "$(wc -l < "$OUT/host-check.log")" -le 30 ] || { echo "  ..."; tail -n 1 "$OUT/host-check.log" | sed 's/^/  /'; }
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: every parser case passed"       "$OUT/host-check.log"  "shell-command host check: 0 of 109 cases failed" line

# The heap table: one byte per block, 0 = free.
config_value() {
    awk -v key="$1" '$1 == "#define" && $2 == key { print $3 }' kernel/config.h
}
table_address=$(config_value RORTHOX_HEAP_TABLE_ADDRESS)
heap_bytes=$(config_value RORTHOX_HEAP_SIZE_BYTES)
block_size=$(config_value RORTHOX_HEAP_BLOCK_SIZE)
if [ -z "$table_address" ] || [ -z "$heap_bytes" ] || [ -z "$block_size" ]; then
    echo "shell-command: heap settings not found in kernel/config.h" >&2
    exit 1
fi
table_bytes=$((heap_bytes / block_size))

# free_blocks: print the number of free heap blocks, read twice with the guest stopped until two
# reads agree.
free_blocks() {
    local i n last=
    for ((i = 0; i < 20; i++)); do
        rm -f "$OUT/heap.bin"
        qemu_cmd "stop"
        qemu_cmd "pmemsave $table_address $table_bytes \"$OUT/heap.bin\""
        qemu_cmd "cont"
        wait_file "$OUT/heap.bin" "$table_bytes" || return 1
        n=$(LC_ALL=C tr -d '\001-\377' < "$OUT/heap.bin" | wc -c)
        if [ "$n" = "$last" ]; then
            echo "$n"
            return 0
        fi
        last=$n
        sleep 0.3
    done
    return 1
}

# state <tag> [options]: the guest-state rendezvous at the shell's key request, the shell alone
# with the keyboard; results in $OUT/state-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$OUT/state-$tag.txt" "$tag" timeout="$RUN_TIMEOUT" owner=first "$@"
}

# One word more than the limit, on one screen row: "blank.elf", x repeated, "over" (no digits).
over="blank.elf"
for ((i = 2; i < MAX + 1; i++)); do over+=" x"; done
over+=" over"
# Exactly the limit, with repeated spaces before, between and after the words.
fits=" blank.elf"
for ((i = 2; i < MAX; i++)); do fits+="  y"; done
fits+="  fits "

# message_row <screen>: the row right below the typed command, if it names the limit (and is not a
# prompt); exit status 1 if there is none.
message_row() {
    awk -v cmd="> $over" -v re="(^|[^0-9])$MAX([^0-9]|\$)" '
        $0 == cmd { r = NR; next }
        r && NR == r + 1 && $0 ~ re && $0 !~ /^>/ { print; found = 1 }
        END { exit !found }' "$1"
}

# wait_message <seconds>: wait for that row on the screen (the typed command has no digits).
wait_message() {
    local deadline=$((SECONDS + $1))
    while ((SECONDS < deadline)); do
        qemu_running || break
        screen_snapshot
        if message_row "$Q_OUT/screen.txt" > /dev/null; then
            echo "  on screen after ${SECONDS} s: the message below the command"
            return 0
        fi
        sleep "$Q_POLL"
    done
    echo "  gave up after $1 s waiting for a row naming $MAX below the command"
    return 1
}

IMG="$OUT/shell-command.bin"
cp bin/os.bin "$IMG"
guest_state_setup "$OUT"
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

echo "shell-command: an empty line, $((MAX + 1)) words, then $MAX words"
: > "$OUT/counts.txt"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$IMG" "$OUT"
counts=()
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" &&
    state boot save="$OUT/shell.txt" screen="$OUT/screen-boot.txt" && n=$(free_blocks); then
    counts+=("$n")
    qemu_cmd "sendkey ret"
    sleep 0.1
    type_line "$over"
    # The message below the typed command, then the shell asks for a key. Go on without it too, so
    # the heap counts show what the command kept.
    wait_message "$RUN_TIMEOUT" || true
    if state over shell="$OUT/shell.txt" screen="$OUT/screen-over.txt" && n=$(free_blocks); then
        counts+=("$n")
        type_line "$fits"
        # blank.elf's last argument row (the echoed command doesn't match), then the shell alone again.
        if wait_screen_lines "$RUN_TIMEOUT" "prompt:fits" &&
            state fits shell="$OUT/shell.txt" screen="$OUT/screen-fits.txt" && n=$(free_blocks); then
            counts+=("$n")
        fi
    fi
fi
qemu_stop || true

echo "  free blocks: ${counts[*]:-none} (no command may keep one)"
[ "${#counts[@]}" -gt 1 ] && echo "over kept=$((counts[0] - counts[1]))" >> "$OUT/counts.txt"
[ "${#counts[@]}" -gt 2 ] && echo "fits kept=$((counts[1] - counts[2]))" >> "$OUT/counts.txt"
for s in boot over fits; do [ -f "$OUT/screen-$s.txt" ] || : > "$OUT/screen-$s.txt"; done

# The rows from the banner down, at the rendezvous after the refused command (nothing prints there):
# banner, the empty line's prompt alone, the typed command, the message, the new prompt alone.
awk -v cmd="> $over" -v max="$MAX" -v banner="${RORTHOX_NAME} v${RORTHOX_VERSION}" '
    $0 == banner { b = NR }
    { row[NR] = $0 }
    END {
        if (!b) { print "layout=no banner"; exit }
        if (row[b + 1] != ">") { print "layout=empty line printed something: " row[b + 1]; exit }
        if (row[b + 2] != cmd) { print "layout=typed command not below the empty prompt: " row[b + 2]; exit }
        if (row[b + 3] !~ ("(^|[^0-9])" max "([^0-9]|$)") || row[b + 3] ~ /^>/) { print "layout=no message below the command: " row[b + 3]; exit }
        if (row[b + 4] != ">") { print "layout=no new prompt below the message: " row[b + 4]; exit }
        for (i = b + 5; i <= NR; i++) if (row[i] != "") { print "layout=more rows below the prompt: " row[i]; exit }
        print "layout=ok"
    }' "$OUT/screen-over.txt" > "$OUT/layout.txt"
sed 's/^/  /' "$OUT/layout.txt"
# blank.elf prints one row per argument, more rows than the screen has: at the rendezvous the last
# argument ("fits", the limit's word) must be below a "y" row and above the exit row (the shell's
# prompt may be in front of any of them).
awk '
    { sub(/^> /, "") }
    $0 == "y" { y = NR }
    $0 == "fits" && y == NR - 1 { f = NR }
    f && NR > f && $0 == "Process was terminated" { t = 1 }
    END { print "last argument=" (t ? "ok" : "missing") }' "$OUT/screen-fits.txt" >> "$OUT/counts.txt"

check "kernel reached the end of init (debugcon)"    "$OUT/debugcon.log"     "Thats cool"
check "no kernel panic (debugcon)"                    "$OUT/debugcon.log"     "PANIC" absent
check "shell started (banner on screen)"              "$OUT/screen-boot.txt"  "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "kernel symbols for the rendezvous found"       "$OUT/symbols.txt"      "found" line
check "empty line, refused command, message, prompt"  "$OUT/layout.txt"       "layout=ok" line
check "the refused command started no program"        "$OUT/screen-over.txt"  "Blank loaded" absent
check "the refused command ended no program"          "$OUT/screen-over.txt"  "Process was terminated" absent
check "the refused command kept no heap block"        "$OUT/counts.txt"       "over kept=0" line
check "$MAX words: blank.elf got the last one, ended" "$OUT/counts.txt"       "last argument=ok" line
check "$MAX words: the command kept no heap block"    "$OUT/counts.txt"       "fits kept=0" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the shell's getkey"          "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"        "$f" "PASS $1_list_consistent" line
    check "$2: the shell alone"                        "$f" "PASS $1_task_count" line
    check "$2: the shell owns the keyboard"           "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot "boot"
stop_checks over "after the refused command"
stop_checks fits "after the $MAX-word command"

hash_after=$(sha256sum bin/os.bin | cut -d' ' -f1)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish shell-command
