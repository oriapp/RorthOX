#!/usr/bin/env bash
#
# The TSS and the GDT the kernel loads (K27): struct tss has the 104-byte layout of a 32-bit TSS
# (ss1 after esp1, then esp2, ss2, cr3 ..., ldtr at 96, the trap halfword at 100 and the I/O map
# base at 102), and the TSS and GDTR limits are inclusive (size - 1).
#
# 1. Compile checks (host C compiler): the size of struct tss and the offset of every field, each
#    checked on its own, and the size of the two halfwords at the end.
# 2. Host check (scripts/tests/host/descriptor-tables.c): the real encodeGdtEntry. Code, data and
#    null descriptors encode to the same bytes as before; a TSS descriptor (limit 0x67) has its
#    base, limit, byte granularity, access byte and bit 54 clear.
# 3. QEMU, started stopped with its gdbstub on a Unix socket (scripts/tests/lib/descriptor_tables.py):
#    after gdt_load the GDTR is gdt_real with limit 0x2f and the six descriptors in memory are right;
#    at tss_load (before LTR) the TSS holds esp0, ss0 and an I/O map base of 104; after LTR, TR has
#    the TSS selector, base and limit 0x67, and the descriptor is busy. Then the debugger leaves, the
#    shell must start and run blank.elf.
# Ring 3 port I/O must still fault with the new limit: scripts/tests/tss-io.sh.
# bin/os.bin must not change.
#
# Needs python3. Env: HOST_CC=cc  BOOT_TIMEOUT=60 (seconds to wait for the shell)  STEP_TIMEOUT=20

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-20}"
. ./scripts/qemu-lib.sh

OUT=build/tests/descriptor-tables
rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || { echo "descriptor-tables: python3 not found" >&2; exit 1; }
[ -f build/kernel.map ] || { echo "descriptor-tables: run 'make all' first" >&2; exit 1; }

echo "descriptor-tables: struct tss layout"
# field:offset:size of the 32-bit TSS
FIELDS="link:0:4 esp0:4:4 ss0:8:4 esp1:12:4 ss1:16:4 esp2:20:4 ss2:24:4 cr3:28:4 eip:32:4 eflags:36:4
        eax:40:4 ecx:44:4 edx:48:4 ebx:52:4 esp:56:4 ebp:60:4 esi:64:4 edi:68:4 es:72:4 cs:76:4 ss:80:4
        ds:84:4 fs:88:4 gs:92:4 ldtr:96:4 trap:100:2 iomap_base:102:2"
: > "$OUT/layout.txt"
layout_check() {   # layout_check <name> <C condition>
    printf '#include <stddef.h>\n#include "task/tss.h"\n_Static_assert(%s, "%s");\n' "$2" "$1" > "$OUT/layout.c"
    if "$HOST_CC" -std=gnu99 -fsyntax-only -I kernel "$OUT/layout.c" > "$OUT/layout-$1.log" 2>&1; then
        echo "PASS $1" >> "$OUT/layout.txt"
    else
        echo "FAIL $1" >> "$OUT/layout.txt"
        echo "  $1: $(grep -m1 -E 'error' "$OUT/layout-$1.log" | sed 's/^.*error: //')"
    fi
}
layout_check size "sizeof(struct tss) == 104"
for f in $FIELDS; do
    IFS=: read -r name offset size <<< "$f"
    layout_check "$name" "offsetof(struct tss, $name) == $offset && sizeof(((struct tss *)0)->$name) == $size"
done

echo "descriptor-tables: host check of encodeGdtEntry"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -w -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/descriptor-tables.c kernel/gdt/gdt.c -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/descriptor-tables.bin"
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)

echo "descriptor-tables: boot"
QEMU_FLAGS="-S -gdb unix:$GDB_DIR/gdb,server=on,wait=off" qemu_start "$OUT/descriptor-tables.bin" "$OUT"
if python3 -B scripts/tests/lib/descriptor_tables.py "$GDB_DIR/gdb" build/kernel.map "$OUT/rsp-result.txt" \
    > "$OUT/rsp.log" 2>&1; then status=0; else status=$?; fi
sed 's/^/  /' "$OUT/rsp.log"
echo "exit=$status" > "$OUT/rsp-status.txt"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "blank.elf tables"
    wait_screen "Process was terminated" "$STEP_TIMEOUT" || true
fi
qemu_stop || true
rm -rf "$GDB_DIR"

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

L="$OUT/layout.txt"
check "struct tss is 104 bytes"                         "$L" "PASS size" line
for f in $FIELDS; do
    IFS=: read -r name offset size <<< "$f"
    check "struct tss: $name at offset $offset, $size bytes" "$L" "PASS $name" line
done
H="$OUT/host-check.log"
check "host check built and exited with status 0"       "$OUT/host-status.txt" "exit=0" line
check "code, data and null descriptors unchanged"       "$H" "PASS flat" line
check "TSS descriptor: limit 0x67, bit 54 clear (DPL 3)" "$H" "PASS tss" line
check "TSS descriptor: limit 0x67, bit 54 clear (DPL 0)" "$H" "PASS tss_dpl0" line
check "struct gdt 8 bytes, struct tss 104 bytes"        "$H" "PASS sizes" line
r="$OUT/rsp-result.txt"
check "debugger session finished without errors"        "$OUT/rsp-status.txt" "exit=0" line
check "gdt_load returned"                               "$r" "PASS gdt_load_returned" line
check "GDTR base is gdt_real"                           "$r" "PASS gdtr_base" line
check "GDTR limit is 6 * 8 - 1 = 0x2f"                  "$r" "PASS gdtr_limit" line
check "null, code and data descriptors are the flat ones" "$r" "PASS flat_descriptors" line
check "TSS descriptor base is the tss object"           "$r" "PASS tss_descriptor_base" line
check "TSS descriptor limit 0x67, byte granularity"     "$r" "PASS tss_descriptor_limit" line
check "TSS descriptor access 0xE9 before LTR"           "$r" "PASS tss_descriptor_access" line
check "TSS descriptor bit 54 clear"                     "$r" "PASS tss_descriptor_bit54_clear" line
check "tss_load gets the TSS selector"                  "$r" "PASS tss_load_selector" line
check "TSS esp0 is the TSS stack top before LTR"        "$r" "PASS tss_esp0" line
check "TSS ss0 is the kernel data selector before LTR"  "$r" "PASS tss_ss0" line
check "TSS I/O map base is 104 before LTR"              "$r" "PASS tss_iomap_base_before_ltr" line
check "every other TSS byte is 0"                       "$r" "PASS tss_rest_zero" line
check "tss_load returned"                               "$r" "PASS tss_load_returned" line
check "TR selector is the TSS selector"                 "$r" "PASS tr_selector" line
check "TR base is the tss object"                       "$r" "PASS tr_base" line
check "TR limit is 104 - 1 = 0x67"                      "$r" "PASS tr_limit" line
check "TSS descriptor busy in memory after LTR"         "$r" "PASS tss_descriptor_busy" line
check "kernel reached the end of init (debugcon)"       "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                      "$OUT/debugcon.log" "PANIC" absent
check "shell started after the debugger left"           "$OUT/screen.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "blank.elf ran from the shell"                    "$OUT/screen.txt" "tables" line
check "blank.elf ended"                                 "$OUT/screen.txt" "Process was terminated"
check "bin/os.bin not changed by the test"              "$OUT/image.txt" "unchanged" line
finish descriptor-tables
