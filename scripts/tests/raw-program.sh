#!/usr/bin/env bash
# Raw programs must fit the load window before any file-sized allocation.
set -euo pipefail
cd "$(dirname "$0")/../.."
. scripts/qemu-lib.sh
. scripts/tests/lib/syscheck.sh
OUT=build/tests/raw-program
mkdir -p "$OUT"
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log"
host_failed=0
if "${HOST_CC:-cc}" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w \
    -I kernel -I build/config -I scripts/tests/host \
    scripts/tests/host/raw-program.c scripts/tests/host/process-host.c scripts/tests/host/kalloc.c \
    kernel/isr80h/process.c kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c \
    kernel/string/string.c kernel/memory/memory.c -Wl,--wrap=kzalloc \
    -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    "$OUT/host-check" programs/blank/build/blank.elf > "$OUT/host-check.log" 2>&1 || host_failed=1
    cat "$OUT/host-check.log"
else
    cat "$OUT/host-build.log"
    host_failed=1
fi
[ "${HOST_ONLY:-0}" = 0 ] || exit "$host_failed"
base_sum=$(sha256sum bin/os.bin)
IMAGE="$OUT/os.bin"
cp bin/os.bin "$IMAGE"
mcopy -o -i "$IMAGE" programs/syscheck/build/syscheck.elf ::syscheck.elf
# Tiny executable raw file: print its marker, then syscall exit; pad to exact window.
cat > "$OUT/raw.asm" <<'ASM'
BITS 32
%include "rorthox-config.inc"
ORG PROGRAM_LOAD_ADDRESS
push message
mov eax, SYSCALL_PRINT
int SYSCALL_VECTOR
add esp, 4
mov eax, SYSCALL_EXIT
int SYSCALL_VECTOR
jmp $
message: db 'raw: ran', 10, 0
ASM
# Read the common load window, not a second independently configured limit.
window=$(awk '/^#define RORTHOX_PROGRAM_VIRTUAL_ADDRESS /{a=$3} /^#define RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT /{b=$3} END{print a,b}' build/config/rorthox-config.h)
read -r low high <<< "$window"
nasm -DPROGRAM_LOAD_ADDRESS="$low" -I build/config/ -f bin "$OUT/raw.asm" -o "$OUT/full.bin"
truncate -s "$((high-low))" "$OUT/full.bin"
: > "$OUT/empty.bin"
cp "$OUT/full.bin" "$OUT/big.bin"
truncate -s "$((high-low+1))" "$OUT/big.bin"
for f in empty big full; do mcopy -o -i "$IMAGE" "$OUT/$f.bin" "::$f.bin"; done
for f in ${RAW_CASES:-empty big full}; do
    if [ "$f" = full ]; then expected='raw: ran'; else expected='syscheck: result -2'; fi
    run_syscheck "$f.bin" "$expected"
done
[ "$base_sum" = "$(sha256sum bin/os.bin)" ] || { echo 'FAIL shipped image changed'; failures=$((failures+1)); }
failures=$((failures+host_failed))
finish raw-program
