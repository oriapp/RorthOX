#!/usr/bin/env bash
# Syscalls 1 (print), 6 (load) and 10 (printc) read their string through a checked copy:
# a freed, unowned or unterminated-at-the-end pointer gives an error to the program
# instead of a kernel page fault (a halt) or a read past the program's memory.
set -euo pipefail
cd "$(dirname "$0")/../.."
. scripts/qemu-lib.sh
. scripts/tests/lib/syscheck.sh
OUT=build/tests/user-pointers
mkdir -p "$OUT"
base_sum=$(sha256sum bin/os.bin)
IMAGE="$OUT/os.bin"
cp bin/os.bin "$IMAGE"
mcopy -o -i "$IMAGE" programs/syscheck/build/syscheck.elf ::syscheck.elf
run_syscheck ptr 'syscheck: PASS'
check "valid string still prints" "$OUT/session-ptr/screen.txt" 'syscheck: valid print'
[ "$base_sum" = "$(sha256sum bin/os.bin)" ] || { echo 'FAIL shipped image changed'; failures=$((failures+1)); }
finish user-pointers
