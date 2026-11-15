#!/usr/bin/env bash
#
# An ELF file bigger than ELF_FILE_LIMIT_BYTES (config.mk) is refused with -ENOMEM before the
# kernel allocates its buffer or reads more than the ELF magic, and it is never run as a flat binary.
#
# 1. Host check (scripts/tests/host/elf-limit.c): the real ELF loader and process loader on the
#    host with a ledger of every allocation request and read: a file of exactly the limit loads,
#    the limit + 1, + 4096, 2 GiB and 4 GiB - 1 are refused right after the magic with nothing
#    allocated or read after it and no request above 64 KiB before it, the output pointer kept,
#    the file closed, the heap and task list as before and the file opened once. A file of the
#    limit + 1 without the ELF magic still goes to the flat binary loader, which refuses it by the
#    load window.
# 2. QEMU: a copy of the image gets syscheck.elf and big.elf (blank.elf padded with zeroes to the
#    limit + 1 bytes). syscheck starts big.elf with syscall 7 and must get -3 (-ENOMEM); nothing
#    may start, the shell stays alone with the keyboard, and blank.elf still runs afterwards.
# bin/os.bin itself must not change.
#
# Env: HOST_CC=cc   HOST_ONLY=1 (host check only)

set -euo pipefail
cd "$(dirname "$0")/../.."
. scripts/qemu-lib.sh
. scripts/tests/lib/syscheck.sh
HOST_CC="${HOST_CC:-cc}"
OUT=${RORTHOX_TEST_ROOT:-build/tests}/elf-limit
BLANK_ELF=programs/blank/build/blank.elf
mkdir -p "$OUT"
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log" "$OUT/host-status.txt"
[ -f "$BLANK_ELF" ] || { echo "elf-limit: $BLANK_ELF not found, run 'make all' first" >&2; exit 1; }

echo "elf-limit: host check"
status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/elf-limit.c scripts/tests/host/process-host.c scripts/tests/host/kalloc.c \
        kernel/isr80h/process.c kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c \
        kernel/string/string.c kernel/memory/memory.c \
        -Wl,--wrap=kzalloc -Wl,--wrap=kmalloc -Wl,--wrap=fread \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$BLANK_ELF" > "$OUT/host-check.log" 2>&1; then status=0; else status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 20 "$OUT/host-build.log" | sed 's/^/  /' || true
    : > "$OUT/host-check.log"
fi
echo "exit=$status" > "$OUT/host-status.txt"
check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
for c in elf_load-at-limit elf_load-limit+1 elf_load-limit+page elf_load-2GiB elf_load-4GiB-1 \
        process_load-at-limit process_load-limit+1 process_load-4GiB-1 raw-limit+1; do
    check "host $c" "$OUT/host-check.log" "PASS $c" line
done
check "host: every case ran and passed" "$OUT/host-check.log" "elf-limit host check: 0 of 9 cases failed" line
if [ "${HOST_ONLY:-0}" != 0 ]; then
    exit $((failures != 0))
fi

base_sum=$(sha256sum bin/os.bin)
IMAGE="$OUT/os.bin"
cp bin/os.bin "$IMAGE"
mcopy -o -i "$IMAGE" programs/syscheck/build/syscheck.elf ::syscheck.elf
# blank.elf with zeroes after it: still a good ELF file, one byte over the limit.
cp "$BLANK_ELF" "$OUT/big.elf"
truncate -s "$((ELF_FILE_LIMIT_BYTES + 1))" "$OUT/big.elf"
mcopy -o -i "$IMAGE" "$OUT/big.elf" ::big.elf
rm -f "$OUT/big.elf"
run_syscheck big.elf 'syscheck: result -3'
# blank.elf prints its arguments: a line "big.elf" alone means big.elf ran.
if grep -qx 'big.elf' "$OUT/session-big.elf/screen.txt" 2> /dev/null; then
    echo "  FAIL big.elf did not start"
    failures=$((failures + 1))
else
    echo "  ok   big.elf did not start"
fi
[ "$base_sum" = "$(sha256sum bin/os.bin)" ] || { echo 'FAIL shipped image changed'; failures=$((failures+1)); }
finish elf-limit
