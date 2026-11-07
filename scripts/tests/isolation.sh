#!/usr/bin/env bash
#
# Process memory isolation: ring 3 reaches only its own pages.
#
# Host part: scripts/tests/host/page-tables.c runs the real paging.c (and process_malloc/
# process_free on top of it): default identity maps are present, writable and supervisor-only, a
# user page opens its PDE and no neighbour, free restores a supervisor identity page, a failed
# malloc leaves no user page, the loaded directory is never changed, constructor failures unwind.
#
# QEMU part: fault.elf (programs/faulttest) runs as a probe: it checks that its own data, stack,
# allocation and code work, prints its mailbox and waits. scripts/tests/lib/isolation.py finds
# the probe's process by that mailbox, writes an operation and a target address into it and
# follows what the CPU and the kernel do: a user #PF on exactly the probe's instruction, error
# 0x5 (read or call) or 0x7 (write), CR2 = target, from ring 3, in the probe's task and directory,
# the probe's whole map as the CPU sees it ("info mem": only its stack, ELF segments and live
# allocations are user pages), interrupt_handler's frame, process_terminate on the probe's
# process under the kernel directory (no user page at all), then the probe's slot and task gone.
#   k*: kernel text (call), kernel data, heap table (read, write), VGA, boot stack, TSS entry stack
#       (read, write), 0x7fff000 and 0xfffff000 (PDEs with no user page), the pages around the
#       user stack and after the program (PDEs with user pages: the neighbours stay supervisor)
#   p*: the probe's own page directory and task, the shell's page directory, the heap pages
#       behind the probe's read-only code (read, write) and its stack, a write to its own
#       read-only code page, and a page it freed that the kernel now uses (kernel data, e.g. a child's
#       process or task structures or page tables)
#   f*: "fault.elf owner" keeps a page with a known pattern live, right next to the probe's own
#       pages (same PDE), while its probe reads it, writes it, and hands it to print, write, read
#       and stat (each must return -2); the owner then finds its page unchanged.
# After each boot's attacks blank.elf runs from the same shell, the shell is the only task left
# and the kernel heap is back apart from the one block per typed word the shell keeps. A boot's
# remaining attacks are skipped only when a probe never armed or its debugger session broke.
#
# Needs python3, cc and i686-elf-ld.  Env: HOST_ONLY=1 (host part only), BOOT_TIMEOUT=60,
# STEP_TIMEOUT=20, ISOLATION_BOOTS="kernel process foreign"

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-20}"
BOOTS="${ISOLATION_BOOTS:-kernel process foreign}"
. ./scripts/qemu-lib.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/isolation
PROGRAM=programs/faulttest/build/faulttest.elf
rm -rf "$OUT"
mkdir -p "$OUT"

# --- host part -----------------------------------------------------------------------------------
host_failed=0
if "${HOST_CC:-cc}" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w -Wl,--wrap=kzalloc \
    -I kernel -I build/config -I scripts/tests/host scripts/tests/host/page-tables.c scripts/tests/host/kalloc.c \
    kernel/memory/paging/paging.c kernel/task/process.c kernel/string/string.c kernel/memory/memory.c \
    -o "$OUT/page-tables" > "$OUT/page-tables-build.log" 2>&1; then
    "$OUT/page-tables" > "$OUT/page-tables.log" 2>&1 || host_failed=1
    cat "$OUT/page-tables.log"
else
    cat "$OUT/page-tables-build.log"
    host_failed=1
fi
echo "host=$host_failed" > "$OUT/host.txt"
check "host: real page tables, process_malloc and process_free" "$OUT/host.txt" "host=0" line
if [ "${HOST_ONLY:-0}" != 0 ]; then
    Q_OUT=$OUT   # no QEMU screen in this run; finish prints "(none)"
    finish isolation
    exit 0
fi

# --- QEMU part -----------------------------------------------------------------------------------
command -v python3 > /dev/null || { echo "isolation: python3 not found" >&2; exit 1; }
[ -f "$PROGRAM" ] || { echo "isolation: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
i686-elf-ld -L build/config -T kernel/linker.ld --oformat elf32-i386 build/kernelfull.o -o "$OUT/kernel.elf"
i686-elf-objcopy -O binary "$OUT/kernel.elf" "$OUT/kernel-relinked.bin"
cmp -s "$OUT/kernel-relinked.bin" bin/kernel.bin && echo same > "$OUT/relink.txt" || echo differs > "$OUT/relink.txt"
check "relinked kernel ELF is bin/kernel.bin byte for byte" "$OUT/relink.txt" "same" line

# Struct offsets, compiled with the kernel's own headers (an array of offset + 1 bytes each).
cat > "$OUT/offsets.c" << 'EOF'
#include "task/task.h"
#include "task/process.h"
char off_task_next[offsetof(struct task, next) + 1];
char off_task_process[offsetof(struct task, process) + 1];
char off_task_page_directory[offsetof(struct task, page_directory) + 1];
char off_process_allocations[offsetof(struct process, allocations) + 1];
char off_process_filename[offsetof(struct process, filename) + 1];
char off_allocation_size[sizeof(struct process_allocation) + 1];
char off_max_allocations[RORTHOX_MAX_PROGRAM_ALLOCATIONS + 1];
EOF
i686-elf-gcc -std=gnu99 -ffreestanding -fno-common -I kernel -I build/config -c "$OUT/offsets.c" -o "$OUT/offsets.o"
i686-elf-nm -S "$OUT/offsets.o" | while read -r _ size _ name; do echo "${name#off_} $((0x$size - 1))"; done > "$OUT/offsets.txt"
[ "$(wc -l < "$OUT/offsets.txt")" -eq 7 ] || { echo "isolation: struct offsets missing" >&2; cat "$OUT/offsets.txt"; exit 1; }

# A "ret" in the kernel's text: calling it from ring 3 must fault on the fetch (on a kernel that
# let ring 3 run it, it would just return).
KTEXT=$(i686-elf-objdump -d "$OUT/kernel.elf" |
    awk '/^[0-9a-f]+ <paging_is_aligned>:/ { f = 1 } f && /\tret/ && !done { sub(":", "", $1); print "0x" $1; done = 1 }')
KDATA=$(grep -E '^\s+0x[0-9a-f]+\s+current_task$' build/kernel.map | awk '{ print $1 }' | head -n 1)
echo "  kernel text ret at $KTEXT, current_task at $KDATA"
[ -n "$KTEXT" ] && [ -n "$KDATA" ] || { echo "isolation: kernel addresses missing" >&2; exit 1; }

config_value() {
    awk -v key="$1" '$1 == "#define" && $2 == key { print $3 }' kernel/config.h
}
table_address=$(config_value RORTHOX_HEAP_TABLE_ADDRESS)
table_bytes=$(( $(config_value RORTHOX_HEAP_SIZE_BYTES) / $(config_value RORTHOX_HEAP_BLOCK_SIZE) ))

# free_blocks: number of free heap blocks, read twice with the guest stopped until two reads agree.
free_blocks() {
    local i n last=
    for ((i = 0; i < 20; i++)); do
        rm -f "$Q_OUT/heap.bin"
        qemu_cmd "stop"
        qemu_cmd "pmemsave $table_address $table_bytes \"$Q_OUT/heap.bin\""
        qemu_cmd "cont"
        wait_file "$Q_OUT/heap.bin" "$table_bytes" || return 1
        n=$(LC_ALL=C tr -d '\001-\377' < "$Q_OUT/heap.bin" | wc -c)
        [ "$n" = "$last" ] && { echo "$n"; return 0; }
        last=$n
        sleep 0.3
    done
    return 1
}

# wait_line <ERE> <seconds>: wait for a whole screen line matching <ERE>; the line goes to $LINE.
# Only the probe and owner status lines are matched, and the shell's "> " may land in front of them.
wait_line() {
    local regex="^(> )?($1)\$" deadline=$((SECONDS + $2))
    LINE=
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running, stopped waiting for /$1/"; return 1; }
        screen_snapshot
        LINE=$(grep -E -- "$regex" "$Q_OUT/screen.txt" | tail -n 1 || true)
        [ -n "$LINE" ] && { LINE=${LINE#> }; echo "  \"$LINE\" after ${SECONDS} s"; return 0; }
        sleep 0.3
    done
    echo "  gave up waiting for /$1/ after $2 s"
    return 1
}

# wait_prompt: the shell's first prompt below its banner (nothing has scrolled yet).
wait_prompt() {
    local deadline=$((SECONDS + STEP_TIMEOUT))
    while ((SECONDS < deadline)); do
        qemu_running || return 1
        screen_snapshot
        sed -n "/${RORTHOX_NAME} v${RORTHOX_VERSION}/,\$p" "$Q_OUT/screen.txt" | grep -qx '>' && return 0
        sleep 0.3
    done
    echo "  gave up waiting for the shell prompt"
    return 1
}

# wait_heap <free blocks> : the kernel heap settles at that many free blocks (programs ending).
wait_heap() {
    local deadline=$((SECONDS + STEP_TIMEOUT)) n=
    while ((SECONDS < deadline)); do
        n=$(free_blocks) || n=
        [ "$n" = "$1" ] && break
        sleep 0.5
    done
    echo "${n:-?}"
}

debugger_list() {   # debugger_list <tag> [options]
    local tag="$1"
    shift
    python3 -B scripts/tests/lib/exceptions.py list "$SOCK" build/kernel.map "$OUT/kernel.elf" "$D/list-$tag.txt" "$tag" 1 "$@" \
        > "$D/list-$tag.log" 2>&1 || true
    sed 's/^/  /' "$D/list-$tag.log"
}

IMAGE="$OUT/isolation.bin"
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$IMAGE"
mcopy -i "$IMAGE" "$PROGRAM" ::fault.elf
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
trap 'rm -rf "$GDB_DIR"' EXIT
SOCK="$GDB_DIR/gdb"

FAULT_IDS="probe_found target_pte target_pde stub_vector error_code cr2 cpl3 faulting_task instruction user_map handler_frame terminated kernel_map slot_cleared"

# attack <tag> <mode> <op> <target> [options]: launch "fault.elf <mode> <tag>" and run one attack.
attack() {
    local tag="$1" mode="$2" op="$3" target="$4" box owner_done= ids id status
    shift 4
    local opts=("$@")
    echo "isolation: $tag ($mode, $op $target)"
    WORDS=$((WORDS + 3))
    STOP=1
    type_line "fault.elf $mode $tag"
    if [ "$mode" = owner ]; then
        wait_line "faultowner $tag: done 0x[0-9a-f]{8}" "$STEP_TIMEOUT" || return 0
        owner_done=${LINE##* }
        wait_line "faultowner $tag: sentinel 0x[0-9a-f]{8}" 2 || return 0
        target=${LINE##* }
        opts+=("done=$owner_done")
    fi
    wait_line "faulttest $tag: armed" "$STEP_TIMEOUT" || return 0
    wait_line "faulttest $tag: box 0x[0-9a-f]{8}" 2 || return 0
    box=${LINE##* }
    if python3 -B scripts/tests/lib/isolation.py attack "$SOCK" build/kernel.map "$OUT/kernel.elf" "$OUT/offsets.txt" \
        "$PROGRAM" "$D/$tag.txt" "$tag" "$op" "$target" "box=$box" "${opts[@]}" > "$D/$tag.log" 2>&1; then
        status=0
    else
        status=$?
    fi
    sed 's/^/  /' "$D/$tag.log"
    screen_snapshot
    cp "$Q_OUT/screen.txt" "$D/$tag-screen.txt"
    echo "exit=$status" > "$D/$tag-status.txt"
    check "$tag: debugger session finished without errors" "$D/$tag-status.txt" "exit=0" line
    check "$tag: own data, stack, allocation and code work" "$D/$tag-screen.txt" "faulttest $tag: own memory ok"
    check "$tag: the probe did not go on after its access" "$D/$tag-screen.txt" "faulttest $tag: survived" absent
    if [ "$op" = s ]; then
        ids="probe_found target_pte target_pde no_exception kernel_map slot_cleared"
    else
        ids=$FAULT_IDS
    fi
    case " ${opts[*]} " in *" owner=1 "*) ids="$ids owner_live owner_unchanged" ;; esac
    case " ${opts[*]} " in *" freed=1 "*) ids="$ids freed_reused" ;; esac
    for id in $ids; do
        check "$tag: $id" "$D/$tag.txt" "PASS ${tag}_$id" line
    done
    if [ "$op" = s ]; then
        wait_line "faulttest $tag: print -?[0-9]+ write -?[0-9]+ read -?[0-9]+ stat -?[0-9]+" "$STEP_TIMEOUT" || true
        echo "$LINE" > "$D/$tag-syscalls.txt"
        check "$tag: print, write, read and stat refuse the owner's page" "$D/$tag-syscalls.txt" \
            "faulttest $tag: print -2 write -2 read -2 stat -2" line
    fi
    if [ "$mode" = owner ]; then
        wait_line "faultowner $tag: sentinel (intact|changed)" "$STEP_TIMEOUT" || true
        echo "$LINE" > "$D/$tag-owner.txt"
        check "$tag: the owner's page is unchanged (owner's own check)" "$D/$tag-owner.txt" "faultowner $tag: sentinel intact" line
    fi
    if [ "$mode" = reuse ]; then
        # The child that got the freed page still waits for its key.
        qemu_cmd "sendkey k"
        wait_line "faulttest child: done" "$STEP_TIMEOUT" || true
        echo "$LINE" > "$D/$tag-child.txt"
        check "$tag: the child that reused the page finished" "$D/$tag-child.txt" "faulttest child: done" line
    fi
    [ "$status" = 0 ] && STOP=0
    return 0
}

run_boot() {
    local name="$1" before= after= want
    D="$OUT/$name"
    mkdir -p "$D"
    STOP=1
    WORDS=2   # "blank.elf <name>done" at the end; each attack adds its three words
    echo "isolation: boot $name"
    rm -f "$SOCK"
    QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$IMAGE" "$D"
    trap 'qemu_kill; rm -rf "$GDB_DIR"' EXIT
    if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && wait_prompt && before=$(free_blocks); then
        debugger_list idle save="$D/shell.txt" owner=first
        STOP=0
        case "$name" in
            kernel)
                attack k1 probe x "$KTEXT" pde_u=1
                ((STOP)) || attack k2 probe r "$KDATA" pde_u=1
                ((STOP)) || attack k3 probe r 0x7e00 pde_u=1
                ((STOP)) || attack k4 probe w 0x7e00 pde_u=1
                ((STOP)) || attack k5 probe r 0xb8000 pde_u=1
                ((STOP)) || attack k6 probe r 0x1ff000 pde_u=1
                ((STOP)) || attack k7 probe r 0x5ff000 pde_u=1
                ((STOP)) || attack k8 probe w 0x5ff000 pde_u=1
                ((STOP)) || attack k9 probe r 0x7fff000 pde_u=0
                ((STOP)) || attack k10 probe r 0xfffff000 pde_u=0
                ((STOP)) || attack k11 probe r 0x3fa000 pde_u=1
                ((STOP)) || attack k12 probe r 0x3ff000 pde_u=1
                ((STOP)) || attack k13 probe r afterprog pde_u=1
                ;;
            process)
                attack p1 probe r owndir
                ((STOP)) || attack p2 probe r owntask
                ((STOP)) || attack p3 probe r shelldir
                ((STOP)) || attack p4 probe r elfalias
                ((STOP)) || attack p5 probe w elfalias
                ((STOP)) || attack p6 probe r stackalias
                ((STOP)) || attack p7 probe w 0x400000 pte=user-ro pde_u=1
                ((STOP)) || attack p8 reuse r freed freed=1
                ;;
            foreign)
                attack f1 owner r sentinel owner=1 pde_u=1
                ((STOP)) || attack f2 owner w sentinel owner=1 pde_u=1
                ((STOP)) || attack f3 owner s sentinel owner=1 pde_u=1
                ;;
        esac
        if ((STOP == 0)); then
            type_line "blank.elf ${name}done"
            if wait_line "${name}done" "$STEP_TIMEOUT"; then
                want=$((before - WORDS))
                after=$(wait_heap "$want")
                debugger_list after shell="$D/shell.txt" owner=first
            fi
        fi
    fi
    qemu_stop || true
    { [ -n "$before" ] && [ -n "$after" ] && echo "heap kept=$((before - after))"; } > "$D/state.txt" || true
    echo "  free blocks before ${before:-?}, after ${after:-?} ($WORDS kept for typed words)"
    ((STOP == 0)) && echo "all attacks" > "$D/ran.txt" || echo "stopped" > "$D/ran.txt"
    check "$name: all attacks ran to the end"                    "$D/ran.txt" "all attacks" line
    check "$name: blank.elf ran from the shell afterwards"       "$D/screen.txt" "${name}done"
    check "$name: idle: task list and slots consistent"          "$D/list-idle.txt" "PASS idle_list_consistent" line
    check "$name: after: task list and slots consistent"         "$D/list-after.txt" "PASS after_list_consistent" line
    check "$name: after: only one task left"                     "$D/list-after.txt" "PASS after_task_count" line
    check "$name: after: it is the same shell task and process"  "$D/list-after.txt" "PASS after_same_shell" line
    check "$name: after: the shell owns the keyboard"            "$D/list-after.txt" "PASS after_keyboard_owner" line
    check "$name: kernel heap back apart from $WORDS command nodes" "$D/state.txt" "heap kept=$WORDS" line
    check "$name: no kernel panic (debugcon)"                    "$D/debugcon.log" "PANIC" absent
}

for b in $BOOTS; do
    run_boot "$b"
done
rm -rf "$GDB_DIR"

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test" "$OUT/image.txt" "unchanged" line
finish isolation
