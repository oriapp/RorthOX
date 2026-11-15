#!/usr/bin/env bash
#
# Page zero is absent in every page directory: a kernel NULL access faults and halts the kernel,
# on both boot paths.
#
# Each case boots with QEMU's gdbstub; scripts/tests/lib/null_page.py does the debugger side.
#   disk  the default boot: the boot sector jumps to _start (multiboot_entry never runs)
#   grub  the real GRUB ISO with the disk's boot sector replaced by "jmp $" (as in grub-boot.sh):
#         multiboot_entry runs with the Multiboot magic in EAX, _start does not
# At enable_paging (paging still off) the kernel directory paging_new_4gb built must already
# have PTE 0 = 0 under a present PDE 0. At the idle shell's getkey syscall the mapping is checked
# as it is (the test changes no page-table entry): PTE 0 is 0 in the shell's directory and in the
# kernel directory, the other pages of the first 4 MiB are identity, present, writable and
# supervisor-only (the user stack aside), and QEMU's own page walk can't read address 0. Then
# the debugger puts "mov eax, [0]" and "mov [0], eax" into the kernel, once at isr80h_handler
# (the shell's directory loaded) and once right after kernel_page() returned (the kernel
# directory loaded), so the CPU itself loads and stores through address 0 in ring 0. Right before
# each access an injected "mov cr2, eax" sets CR2 to 0x5A5A5A5A (it is 0 from reset until the first
# #PF, so only a fault that writes CR2 can make it 0 again). Each access must
# give #PF with error 0x0 (load) or 0x2 (store), the injected instruction's eip, CR2 = 0; the
# last one goes on to PANIC("Kernel exception") and PAUSE, and physical page zero is unchanged.
# With page zero mapped the access simply completes: a breakpoint after it reports that at once.
#
# The grub case needs the ISO tools: without them it is skipped, or fails with RORTHOX_REQUIRE_ISO=1.
# It boots the ISO that make test (or make iso) built; the test never builds it.
# Needs python3. Env: BOOT_TIMEOUT=60  STEP_TIMEOUT=20  NULL_PAGE_CASES="disk grub"

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-20}"
CASES="${NULL_PAGE_CASES:-disk grub}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/iso-check.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/null-page
GDB_DIR=
DBG_PID=
# On any exit (also an early one: an error, a failed setup step, TERM or INT) stop QEMU and the
# debugger and remove the gdb socket directory. The EXIT trap is set before anything is created, and
# it is the one qemu_start sets again: qemu_exit stops QEMU, then calls Q_CLEANUP.
cleanup() {
    if [ -n "$DBG_PID" ]; then kill "$DBG_PID" 2> /dev/null || :; wait "$DBG_PID" 2> /dev/null || :; fi
    if [ -n "$GDB_DIR" ]; then rm -rf "$GDB_DIR"; fi
}
Q_CLEANUP=cleanup
trap qemu_exit EXIT
trap 'exit 1' TERM INT
# A command that fails outside a check stops the test (set -e): say which one.
trap 'echo "FAIL null-page: command failed at line $LINENO" >&2' ERR
fail() {
    echo "FAIL null-page: $*" >&2
    exit 1
}

rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || fail "python3 not found"
[ -f build/kernel.map ] || fail "build/kernel.map not found, run 'make all' first"
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX) || fail "mktemp for the gdb socket failed"
SOCK="$GDB_DIR/gdb"
panic_line=$(grep -n 'PANIC(msg, __FILE__, __LINE__);' kernel/idt/idt.c | cut -d: -f1 || true)
[ "$(echo "$panic_line" | wc -w)" = 1 ] || fail "no single PANIC(msg, __FILE__, __LINE__) in kernel/idt/idt.c"

# debugger <log> <args...>: run null_page.py, keep its output, return its status.
# It runs in the background so a TERM or INT stops the test at once, not when the debugger ends.
debugger() {
    local log="$1" status
    shift
    timeout $((BOOT_TIMEOUT + 60)) python3 -B scripts/tests/lib/null_page.py "$@" > "$log" 2>&1 &
    DBG_PID=$!
    if wait "$DBG_PID"; then status=0; else status=$?; fi
    DBG_PID=
    sed 's/^/  /' "$log"
    return "$status"
}

# run_case <case> <image> [qemu flags]
run_case() {
    local c="$1" image="$2" boot_status="not run" access_status="not run"
    D="$OUT/$c"
    mkdir -p "$D"
    echo "null-page: $c"
    rm -f "$SOCK"
    QEMU_FLAGS="-S -gdb unix:$SOCK,server=on,wait=off ${3:-}" qemu_start "$image" "$D"
    if debugger "$D/boot.log" boot "$SOCK" build/kernel.map "$D/boot.txt" "$D/directory.txt" "$c" "$BOOT_TIMEOUT"; then
        boot_status=0
        if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
            if debugger "$D/access.log" access "$SOCK" build/kernel.map "$D/directory.txt" "$D/access.txt" "$c"; then
                access_status=0
                wait_screen "A problem has been detected" "$STEP_TIMEOUT" || true
            else
                access_status=$?
            fi
        fi
    else
        boot_status=$?
    fi
    echo "exit=$boot_status" > "$D/boot-status.txt"
    echo "exit=$access_status" > "$D/access-status.txt"
    qemu_stop || true
    echo "exceptions=$(grep -a -c '^exception: ' "$D/debugcon.log" || true)" > "$D/counts.txt"
    local b="$D/boot.txt" r="$D/access.txt"
    check "$c: boot followed without errors"                "$D/boot-status.txt" "exit=0" line
    if [ "$c" = grub ]; then
        check "$c: GRUB entered multiboot_entry with the magic"  "$b" "PASS grub_multiboot_entry" line
        check "$c: then kernel_main, not through _start"         "$b" "PASS grub_kernel_main" line
    else
        check "$c: the boot sector entered _start"               "$b" "PASS disk_start" line
        check "$c: then kernel_main"                             "$b" "PASS disk_kernel_main" line
    fi
    check "$c: enable_paging reached, paging still off"      "$b" "PASS before_paging" line
    check "$c: kernel directory built with PTE 0 = 0"        "$b" "PASS built_page_zero" line
    check "$c: and the rest of the first 4 MiB mapped"       "$b" "PASS built_neighbours" line
    check "$c: debugger session finished without errors"    "$D/access-status.txt" "exit=0" line
    check "$c: ring 0 in the shell's syscall, its directory" "$r" "PASS in_shell_syscall" line
    check "$c: shell directory: PTE 0 = 0, PDE 0 present"    "$r" "PASS task_page_zero" line
    check "$c: shell directory: low pages still mapped"      "$r" "PASS task_neighbours" line
    check "$c: kernel directory: PTE 0 = 0, PDE 0 present"   "$r" "PASS kernel_page_zero" line
    check "$c: kernel directory: low pages still mapped"     "$r" "PASS kernel_neighbours" line
    check "$c: QEMU's walk: 0 unreadable, 0x1000 readable"   "$r" "PASS cpu_walk" line
    local a
    for a in task_load task_store kernel_load kernel_store; do
        check "$c: $a: CR2 set to 0x5A5A5A5A before it"     "$r" "PASS ${a}_cr2_seeded" line
        check "$c: $a: the CPU's access faulted (#PF)"        "$r" "PASS ${a}_faulted" line
        check "$c: $a: error code, eip, cs, instruction"      "$r" "PASS ${a}_frame" line
        check "$c: $a: the fault set CR2 = 0"                 "$r" "PASS ${a}_cr2" line
        check "$c: $a: CR3 = the directory it ran under"      "$r" "PASS ${a}_cr3" line
    done
    check "$c: kernel_page() selected the kernel directory"  "$r" "PASS kernel_cr3" line
    check "$c: interrupt_handler got vector 14, 0x2, cr2 0"  "$r" "PASS handler_frame" line
    check "$c: PANIC(\"Kernel exception\")"                  "$r" "PASS panic" line
    check "$c: halted in PAUSE"                              "$r" "PASS halted_in_pause" line
    check "$c: physical page zero unchanged"                 "$r" "PASS page_zero_unchanged" line
    check "$c: still the kernel directory"                   "$r" "PASS cr3_kernel" line
    check "$c: exception line on debugcon"  "$D/debugcon.log" "exception: vector=0x0000000e error=0x00000002 cr2=0x00000000" line
    check "$c: one exception line"          "$D/counts.txt" "exceptions=1" line
    check "$c: panic message on debugcon"   "$D/debugcon.log" "[PANIC]: Kernel exception at ./kernel/idt/idt.c:"
    check "$c: screen shows the reason"     "$D/screen.txt" "Reason: Kernel exception" line
    check "$c: screen shows file and line"  "$D/screen.txt" "at ./kernel/idt/idt.c:$panic_line" line
}

for c in $CASES; do
    case "$c" in
        disk)
            cp bin/os.bin "$OUT/disk.bin"
            run_case disk "$OUT/disk.bin"
            ;;
        grub)
            if iso_check "$OUT" "null-page grub"; then
                :
            else
                status=$?
                [ "$status" -eq 77 ] && continue
                fail "no up-to-date GRUB ISO (see above)"
            fi
            # "jmp $" in place of the boot sector's first bytes: only GRUB can start the kernel.
            python3 -B - "$OUT/trap.img" << 'PY' || fail "could not write the trap disk image"
import pathlib
import sys
original = pathlib.Path("bin/os.bin").read_bytes()
pathlib.Path(sys.argv[1]).write_bytes(b"\xeb\xfe" + original[2:])
PY
            run_case grub "$OUT/trap.img" "-boot d -cdrom bin/rorthox.iso"
            ;;
        *) fail "unknown case $c" ;;
    esac
done
rm -rf "$GDB_DIR"
GDB_DIR=

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test" "$OUT/image.txt" "unchanged" line
finish null-page
