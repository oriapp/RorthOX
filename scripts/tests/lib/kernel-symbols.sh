# Kernel addresses and struct task offsets for the sleep tests. Source it after qemu-lib.sh,
# then call kernel_symbols <outdir>. Sets (hex, without 0x):
#   SYM_TICKS          the PIT tick counter (jiffies, static in pit.c)
#   SYM_CURRENT_TASK   current_task
#   SYM_PAUSE          IRQ_ON_AND_PAUSE
#   SYM_KERNEL_CHUNK   kernel_chunk (static in kernel.c), the kernel's page directory
#   TASK_REGISTERS_OFFSET, TASK_STATE_OFFSET, TASK_WAKE_OFFSET   offsets in struct task
# Statics are not in build/kernel.map, so the kernel objects are linked once more as an ELF.
# That link uses ld directly while bin/kernel.bin is linked through gcc, so the result is checked
# against the real link: every global named in build/kernel.map must have the same address in the
# ELF, and the ELF's loaded bytes must be bin/kernel.bin byte for byte (this covers the statics).
# Returns 1 if something is missing (a symbol or the struct task fields) or the check fails.
#
# The tests that use these addresses (sleep.sh, sleep-idle.sh, and libtest's sleep modes) read
# jiffies, current_task and struct task from ring 3 through the user page tables, which map all
# kernel memory read-only. Once user tasks can no longer read kernel memory, those reads page
# fault and the tests fail (they can't pass by accident); they then need another clock source.
kernel_symbols() {
    local out="$1" elf="$1/kernel.elf" fix="$1/task-offsets" map=build/kernel.map linked=ok
    SYM_TICKS='' SYM_CURRENT_TASK='' SYM_PAUSE='' SYM_KERNEL_CHUNK=''
    TASK_REGISTERS_OFFSET='' TASK_STATE_OFFSET='' TASK_WAKE_OFFSET=''
    i686-elf-ld -L build/config -T kernel/linker.ld --oformat elf32-i386 build/kernelfull.o -o "$elf" ||
        return 1
    i686-elf-nm "$elf" > "$out/kernel-nm.txt"
    sym() { awk -v n="$1" '$3 == n { print $1; exit }' "$out/kernel-nm.txt"; }
    SYM_TICKS=$(sym jiffies)
    SYM_CURRENT_TASK=$(sym current_task)
    SYM_PAUSE=$(sym IRQ_ON_AND_PAUSE)
    SYM_KERNEL_CHUNK=$(sym kernel_chunk)
    echo "  jiffies $SYM_TICKS, current_task $SYM_CURRENT_TASK, IRQ_ON_AND_PAUSE $SYM_PAUSE, kernel_chunk $SYM_KERNEL_CHUNK"

    # Cross-check with the real link. Map lines "0x<address> <name>" are the global symbols;
    # addresses are compared as numbers (leading zeros dropped).
    if [ -f "$map" ]; then
        awk 'function num(a) { sub(/^0x/, "", a); sub(/^0+/, "", a); return a == "" ? "0" : a }
             NR == FNR { if (NF == 3) nm[$3] = num($1); next }
             NF == 2 && $1 ~ /^0x[0-9a-fA-F]+$/ && $2 ~ /^[A-Za-z_][A-Za-z0-9_.]*$/ {
                 if (!($2 in nm))             print "missing " $2 " (map " $1 ")"
                 else if (nm[$2] != num($1)) print "mismatch " $2 " (map " $1 ", relinked " nm[$2] ")"
                 else                         print "ok " $2
             }' "$out/kernel-nm.txt" "$map" > "$out/map-check.txt"
        local checked bad
        checked=$(grep -c '^ok ' "$out/map-check.txt" || true)
        bad=$(grep -v '^ok ' "$out/map-check.txt" || true)
        if [ -n "$bad" ]; then
            echo "  ERROR: the relinked kernel does not match $map:"
            echo "$bad" | head -n 10 | sed 's/^/    /'
            linked=bad
        fi
        for s in current_task IRQ_ON_AND_PAUSE; do
            grep -qx "ok $s" "$out/map-check.txt" ||
                { echo "  ERROR: $s is not at the same address in $map and the relinked kernel"; linked=bad; }
        done
        echo "  $checked globals from $map match the relinked kernel"
    else
        echo "  ERROR: $map not found, can't check the relinked kernel"; linked=bad
    fi
    if i686-elf-objcopy -O binary "$elf" "$out/kernel-relinked.bin" &&
            cmp -s "$out/kernel-relinked.bin" bin/kernel.bin; then
        echo "  relinked kernel image is identical to bin/kernel.bin"
    else
        echo "  ERROR: the relinked kernel image differs from bin/kernel.bin"; linked=bad
    fi

    # The offsets as the sizes of three arrays, compiled with the kernel's own header.
    cat > "$fix.c" << 'EOF'
#include "task/task.h"
char task_registers_offset[offsetof(struct task, registers) + 1];
char task_state_offset[offsetof(struct task, state) + 1];
char task_wake_offset[offsetof(struct task, wake_tick) + 1];
EOF
    if i686-elf-gcc -std=gnu99 -ffreestanding -fno-common -I kernel -I build/config -c "$fix.c" -o "$fix.o" \
            > "$fix.log" 2>&1; then
        off() { printf '%x' $((0x$(i686-elf-nm -S "$fix.o" | awk -v n="$1" '$4 == n { print $2 }') - 1)); }
        TASK_REGISTERS_OFFSET=$(off task_registers_offset)
        TASK_STATE_OFFSET=$(off task_state_offset)
        TASK_WAKE_OFFSET=$(off task_wake_offset)
    else
        echo "  struct task offsets: the fixture does not compile, see $fix.log"
    fi
    echo "  struct task: registers at $TASK_REGISTERS_OFFSET, state at $TASK_STATE_OFFSET, wake_tick at $TASK_WAKE_OFFSET"
    [ "$linked" = ok ] &&
        [ -n "$SYM_TICKS" ] && [ -n "$SYM_CURRENT_TASK" ] && [ -n "$SYM_PAUSE" ] && [ -n "$SYM_KERNEL_CHUNK" ] &&
        [ -n "$TASK_STATE_OFFSET" ] && [ -n "$TASK_WAKE_OFFSET" ] && [ -n "$TASK_REGISTERS_OFFSET" ]
}
