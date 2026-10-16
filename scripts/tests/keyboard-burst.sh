#!/usr/bin/env bash
# A release/make/release backlog must not lose the make to a discarded port-0x60 read.
# Two fresh boots prove both a period inside the filename and Enter submitting the command.
set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh

OUT="$(pwd)/build/tests/keyboard-burst"
mkdir -p "$OUT"
# Keep the Unix socket path short even when the checkout path is long.
cleanup() { qemu_kill; rm -rf "$SOCKET_DIR"; }
SOCKET_DIR=$(mktemp -d "$HOME/keyboard-burst.XXXXXX")
trap cleanup EXIT
BASE_FLAGS=${QEMU_FLAGS:-}

for key in dot ret; do
    SESSION="$OUT/$key"
    mkdir -p "$SESSION"
    SOCK="$SOCKET_DIR/qmp"
    rm -f "$SOCK"
    QEMU_FLAGS="$BASE_FLAGS -qmp unix:$SOCK,server=on,wait=off"
    qemu_start bin/os.bin "$SESSION"
    trap cleanup EXIT
    wait_screen "$RORTHOX_NAME v$RORTHOX_VERSION" 20

    if [ "$key" = dot ]; then prefix=blan; last=k; before=blank;
    else prefix='blank.elf burs'; last=t; before='blank.elf burst'; fi
    for ((i = 0; i < ${#prefix}; i++)); do
        c=${prefix:i:1}
        case "$c" in ' ') c=spc;; '.') c=dot;; esac
        qemu_cmd "sendkey $c"
        sleep 0.1 # space key presses, never a boot/command-completion wait
    done
    wait_screen "> $prefix" 10
    python3 -B scripts/tests/lib/keyboard-qmp.py "$SOCK" down "$last" > "$SESSION/qmp.log"
    wait_screen "> $before" 10
    cp "$SESSION/screen.txt" "$SESSION/before.txt"

    # One QMP request queues all three events while the guest is running.
    python3 -B scripts/tests/lib/keyboard-qmp.py "$SOCK" burst "$key" "$last" >> "$SESSION/qmp.log"
    if [ "$key" = dot ]; then type_line 'elf burst'; fi
    if ! wait_screen 'Process was terminated' 20; then
        echo "  FAIL $key burst did not run the command"
        failures=$((failures + 1))
    fi
    qemu_stop || true
    check "$key burst: exact command echo" "$SESSION/screen.txt" '> blank.elf burst' line
    check "$key burst: child ran" "$SESSION/screen.txt" 'Blank loaded'
    check "$key burst: argument printed by child" "$SESSION/screen.txt" burst line
    check "$key burst: child completed" "$SESSION/screen.txt" 'Process was terminated'
done
finish keyboard-burst
