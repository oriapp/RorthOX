#!/usr/bin/env bash
#
# Supervisor write protection (CR0.WP): the kernel itself must fault when it stores to a page its
# loaded directory maps read-only, on both boot paths.
#
# Each case boots with QEMU's gdbstub; scripts/tests/lib/write_protect.py does the debugger side.
#   disk  the default boot: the boot sector jumps to _start (multiboot_entry never runs)
#   grub  the real GRUB ISO with the disk's boot sector replaced by "jmp $" (as in grub-boot.sh):
#         multiboot_entry runs with the Multiboot magic in EAX, _start does not
# At the idle shell's getkey syscall (ring 0, the shell's directory still loaded) CR0 must have
# PG and WP set and CR4 PGE, SMEP and SMAP clear. Then the debugger puts "mov [dest], eax" at
# isr80h_handler, with dest in the shell's read-only ELF code page (PTE present and user, R/W
# clear). The CPU's own store must give #PF with error 0x3, eip = isr80h_handler, CR2 = dest,
# then PANIC("Kernel exception"), with the page's bytes unchanged. Without WP the store simply
# goes through: a breakpoint after it reports that at once.
#
# The grub case needs the ISO tools: without them it is skipped, or fails with RORTHOX_REQUIRE_ISO=1.
# Needs python3. Env: BOOT_TIMEOUT=60  STEP_TIMEOUT=20  WRITE_PROTECT_CASES="disk grub"

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-20}"
CASES="${WRITE_PROTECT_CASES:-disk grub}"
. ./scripts/qemu-lib.sh

OUT=build/tests/write-protect
SHELL_ELF=programs/shell/build/shell.elf
GDB_DIR=
DBG_PID=
# On any exit (also an early one: an error, a failed setup step, TERM or INT) stop QEMU and the
# debugger and remove the gdb socket directory.
cleanup() {
    if [ -n "$DBG_PID" ]; then kill "$DBG_PID" 2> /dev/null || :; wait "$DBG_PID" 2> /dev/null || :; fi
    qemu_kill
    if [ -n "$GDB_DIR" ]; then rm -rf "$GDB_DIR"; fi
}
trap cleanup EXIT
trap 'exit 1' TERM INT
# A command that fails outside a check stops the test (set -e): say which one.
trap 'echo "FAIL write-protect: command failed at line $LINENO" >&2' ERR
fail() {
    echo "FAIL write-protect: $*" >&2
    exit 1
}

rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || fail "python3 not found"
[ -f "$SHELL_ELF" ] || fail "$SHELL_ELF not found, run 'make all' first"
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX) || fail "mktemp for the gdb socket failed"
SOCK="$GDB_DIR/gdb"
panic_line=$(grep -n 'PANIC(msg, __FILE__, __LINE__);' kernel/idt/idt.c | cut -d: -f1 || true)
[ "$(echo "$panic_line" | wc -w)" = 1 ] || fail "no single PANIC(msg, __FILE__, __LINE__) in kernel/idt/idt.c"
if ! dest=$(python3 -B -c 'import sys; sys.path.insert(0, "scripts/tests/lib"); import write_protect as w
print("%08x" % w.read_only_segment(sys.argv[1])[0])' "$SHELL_ELF"); then
    fail "no read-only PT_LOAD segment found in $SHELL_ELF"
fi
[ -n "$dest" ] || fail "no read-only PT_LOAD segment found in $SHELL_ELF"

# debugger <log> <args...>: run write_protect.py, keep its output, return its status.
# It runs in the background so a TERM or INT stops the test at once, not when the debugger ends.
debugger() {
    local log="$1" status
    shift
    timeout $((BOOT_TIMEOUT + 60)) python3 -B scripts/tests/lib/write_protect.py "$@" > "$log" 2>&1 &
    DBG_PID=$!
    if wait "$DBG_PID"; then status=0; else status=$?; fi
    DBG_PID=
    sed 's/^/  /' "$log"
    return "$status"
}

# run_case <case> <image> [qemu flags]
run_case() {
    local c="$1" image="$2" boot_status="not run" wp_status="not run"
    D="$OUT/$c"
    mkdir -p "$D"
    echo "write-protect: $c"
    rm -f "$SOCK"
    QEMU_FLAGS="-S -gdb unix:$SOCK,server=on,wait=off ${3:-}" qemu_start "$image" "$D"
    # qemu_start installs its own EXIT trap; restore the full cleanup after each boot.
    trap cleanup EXIT
    if debugger "$D/boot.log" boot "$SOCK" build/kernel.map "$D/boot.txt" "$c" "$BOOT_TIMEOUT"; then
        boot_status=0
        if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
            if debugger "$D/wp.log" wp "$SOCK" build/kernel.map "$SHELL_ELF" "$D/wp.txt" "$c"; then
                wp_status=0
                wait_screen "A problem has been detected" "$STEP_TIMEOUT" || true
            else
                wp_status=$?
            fi
        fi
    else
        boot_status=$?
    fi
    echo "exit=$boot_status" > "$D/boot-status.txt"
    echo "exit=$wp_status" > "$D/wp-status.txt"
    qemu_stop || true
    echo "exceptions=$(grep -a -c '^exception: ' "$D/debugcon.log" || true)" > "$D/counts.txt"
    local b="$D/boot.txt" r="$D/wp.txt"
    check "$c: boot followed without errors"                "$D/boot-status.txt" "exit=0" line
    if [ "$c" = grub ]; then
        check "$c: GRUB entered multiboot_entry with the magic"  "$b" "PASS grub_multiboot_entry" line
        check "$c: then kernel_main, not through _start"         "$b" "PASS grub_kernel_main" line
    else
        check "$c: the boot sector entered _start"               "$b" "PASS disk_start" line
        check "$c: then kernel_main"                             "$b" "PASS disk_kernel_main" line
    fi
    check "$c: debugger session finished without errors"   "$D/wp-status.txt" "exit=0" line
    check "$c: CR0 has PG and WP set"                       "$r" "PASS cr0_pg_wp" line
    check "$c: CR4 has PGE, SMEP and SMAP clear"            "$r" "PASS cr4_no_pge_smep_smap" line
    check "$c: ring 0 in the shell's syscall, its directory" "$r" "PASS in_shell_syscall" line
    check "$c: target is the shell's read-only user page"   "$r" "PASS read_only_user_page" line
    check "$c: the kernel's store faulted"                  "$r" "PASS store_faulted" line
    check "$c: #PF error 0x3 at isr80h_handler, ring 0"     "$r" "PASS pf_frame" line
    check "$c: CR2 is the target"                           "$r" "PASS pf_cr2" line
    check "$c: interrupt_handler got vector 14, 0x3, cr2"   "$r" "PASS handler_frame" line
    check "$c: PANIC(\"Kernel exception\")"                 "$r" "PASS panic" line
    check "$c: halted in PAUSE"                             "$r" "PASS halted_in_pause" line
    check "$c: target bytes unchanged (virtual, physical)"  "$r" "PASS bytes_unchanged" line
    check "$c: CR3 untouched (the shell's directory)"       "$r" "PASS cr3_untouched" line
    check "$c: exception line on debugcon"  "$D/debugcon.log" "exception: vector=0x0000000e error=0x00000003 cr2=0x$dest" line
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
            if bash scripts/check-iso-tools.sh > "$OUT/tools.log" 2>&1; then
                if ! make iso > "$OUT/iso-build.log" 2>&1; then
                    cat "$OUT/iso-build.log"
                    fail "make iso failed"
                fi
            else
                status=$?
                cat "$OUT/tools.log"
                if [ "$status" -eq 77 ] && [ "${RORTHOX_REQUIRE_ISO:-0}" != 1 ]; then
                    echo "SKIP write-protect grub: optional BIOS ISO tools unavailable"
                    continue
                fi
                fail "ISO prerequisites unavailable or preflight failed"
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
finish write-protect
