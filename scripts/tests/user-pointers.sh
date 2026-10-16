#!/usr/bin/env bash
# Syscalls 0, 1, 3, 4, 5, 6 and 10 read their stack arguments through a checked copy, and
# 1 (print), 6 (load) and 10 (printc) their string too: a stack pointer into a freed page, or a
# freed, unowned or unterminated-at-the-end string pointer gives an error to the program
# instead of a kernel page fault (a halt) or a read past the program's memory. A longer string
# than print's 1024-byte buffer is cut after 1023 bytes, also when it crosses a page edge.
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
check "freed stack refused by every syscall" "$OUT/session-ptr/screen.txt" 'syscheck: freed stack -2 -2 -2 -2 -2 0 0'
check "long string cut after 1023 bytes" "$OUT/session-ptr/screen.txt" 'TRUN|'
check "nothing past the cut printed" "$OUT/session-ptr/screen.txt" 'TRUNX' absent
check "valid string still prints" "$OUT/session-ptr/screen.txt" 'syscheck: valid print'
[ "$base_sum" = "$(sha256sum bin/os.bin)" ] || { echo 'FAIL shipped image changed'; failures=$((failures+1)); }
finish user-pointers
