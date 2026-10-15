#!/usr/bin/env bash
#
# Backspace at the start of a line must do nothing in the user library's readline: no echo
# (the echo erased the shell's prompt) and nothing stored (K26: 0x08 went into the line, so the
# shell got a command it could not run).
#
# 1. Host check (scripts/tests/host/readline.c): the real programs/stdlib/src/rorthox.c and
#    string.c built with the host C compiler, every symbol prefixed with "user_". Scripted keys,
#    recorded echo, guard bytes around the line buffer: leading backspaces are neither stored nor
#    echoed nor counted against the buffer size; ordinary erasing still works.
# 2. QEMU: boot bin/os.bin (a copy), wait for the shell's prompt, read the VGA page, press
#    backspace three times, then type "blank.elf rl2", a backspace, "1" and Enter. blank.elf
#    must run with the argument "rl1"; the prompt row must read "> blank.elf rl1" with the
#    prompt's cells unchanged, and nothing above it may have changed.
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

OUT=build/tests/readline
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.o "$OUT"/*.txt "$OUT"/*.bin "$OUT"/screen.ppm "$OUT"/debugcon.log "$OUT"/monitor.log
[ -f bin/os.bin ] || { echo "readline: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)

echo "readline: host check of the user library's readline"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -ffreestanding -fno-builtin -fno-stack-protector -w \
        -I programs/stdlib/src -I build/config \
        -c programs/stdlib/src/rorthox.c -o "$OUT/user-rorthox.o" > "$OUT/host-build.log" 2>&1 &&
    "$HOST_CC" -std=gnu99 -g -O0 -ffreestanding -fno-builtin -fno-stack-protector -w \
        -I programs/stdlib/src -I build/config \
        -c programs/stdlib/src/string.c -o "$OUT/user-string.o" >> "$OUT/host-build.log" 2>&1 &&
    "$OBJCOPY" --prefix-symbols=user_ "$OUT/user-rorthox.o" "$OUT/user-rorthox-prefixed.o" >> "$OUT/host-build.log" 2>&1 &&
    "$OBJCOPY" --prefix-symbols=user_ "$OUT/user-string.o" "$OUT/user-string-prefixed.o" >> "$OUT/host-build.log" 2>&1 &&
    "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -w -I scripts/tests/host \
        scripts/tests/host/readline.c "$OUT/user-rorthox-prefixed.o" "$OUT/user-string-prefixed.o" \
        -o "$OUT/host-check" >> "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 30
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: every readline case passed"     "$OUT/host-check.log"  "readline host check: 0 of 10 cases failed" line

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

# vga_dump <file>: the 4000-byte VGA page, read until two reads agree (nothing is still printing).
vga_dump() {
    local i
    rm -f "$1" "$OUT/dump-a.bin"
    for ((i = 0; i < 20; i++)); do
        rm -f "$OUT/dump-b.bin"
        qemu_cmd "pmemsave 0xb8000 4000 \"$OUT/dump-b.bin\""
        wait_file "$OUT/dump-b.bin" 4000 || return 1
        if [ -f "$OUT/dump-a.bin" ] && cmp -s "$OUT/dump-a.bin" "$OUT/dump-b.bin"; then
            mv "$OUT/dump-b.bin" "$1"
            rm -f "$OUT/dump-a.bin"
            return 0
        fi
        mv "$OUT/dump-b.bin" "$OUT/dump-a.bin"
        sleep 0.3
    done
    return 1
}

# keys <key>...: press QEMU keys one after the other.
keys() {
    local key
    for key in "$@"; do
        qemu_cmd "sendkey $key"
        sleep 0.1
    done
}

IMG="$OUT/readline.bin"
cp bin/os.bin "$IMG"

echo "readline: backspaces at the prompt, then blank.elf rl1 with one typo"
: > "$OUT/steps.txt"
qemu_start "$IMG" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && wait_prompts 1 && vga_dump "$OUT/before.bin"; then
    echo "prompt" >> "$OUT/steps.txt"
    keys backspace backspace backspace
    keys b l a n k dot e l f spc r l 2 backspace 1 ret
    if wait_screen "Process was terminated" "$RUN_TIMEOUT" && wait_prompts 2; then
        echo "ran" >> "$OUT/steps.txt"
    fi
    # Also when the command didn't run, so the cells below show what the keys did.
    vga_dump "$OUT/after.bin" || true
fi
qemu_stop || true

# Compare the prompt row and everything above it before and after.
python3 -B - "$OUT" > "$OUT/cells.txt" 2>&1 << 'EOF' || true
import os
import sys

out = sys.argv[1]


def cells(name):
    path = os.path.join(out, name)
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    return [(data[2 * i], data[2 * i + 1]) for i in range(2000)]


def text(screen, r):
    return "".join(chr(c) if 32 <= c < 127 else " " for c, _ in screen[r * 80:(r + 1) * 80])


before, after = cells("before.bin"), cells("after.bin")
if before is None or after is None:
    print("no VGA dumps")
    sys.exit(0)
prompt_rows = [r for r in range(25) if text(before, r).rstrip() == ">"]
print("prompt rows before the keys: %s" % prompt_rows)
if len(prompt_rows) != 1:
    sys.exit(0)
p = prompt_rows[0]
print("the shell's prompt is alone on row %d" % p)
print("row %d after: %r" % (p, text(after, p).rstrip()))
if text(after, p).rstrip() == "> blank.elf rl1":
    print("the prompt row reads > blank.elf rl1")
if after[p * 80] == before[p * 80] == (ord(">"), 0x0F) and after[p * 80 + 1] == before[p * 80 + 1] == (0x20, 0x0F):
    print("the prompt cells are unchanged")
changed = [i for i in range(p * 80) if before[i] != after[i]]
for i in changed[:5]:
    print("cell (%d,%d) changed: %r/0x%02x -> %r/0x%02x" % (i % 80, i // 80, chr(before[i][0]), before[i][1],
                                                             chr(after[i][0]), after[i][1]))
if not changed:
    print("nothing above the prompt row changed")
EOF
sed 's/^/  /' "$OUT/cells.txt"

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "the shell's prompt was read before the keys" "$OUT/steps.txt"    "prompt" line
check "the command ran and the shell came back"     "$OUT/steps.txt"    "ran" line
check "blank.elf started"                           "$Q_OUT/screen.txt" "Blank loaded"
check "blank.elf got the argument rl1"              "$Q_OUT/screen.txt" "rl1" line
check "the prompt row reads > blank.elf rl1"        "$OUT/cells.txt"    "the prompt row reads > blank.elf rl1" line
check "the prompt's cells are unchanged"            "$OUT/cells.txt"    "the prompt cells are unchanged" line
check "nothing above the prompt row changed"        "$OUT/cells.txt"    "nothing above the prompt row changed" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish readline
