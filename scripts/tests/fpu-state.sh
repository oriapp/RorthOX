#!/usr/bin/env bash
#
# Every program has its own x87 and SSE registers: the kernel saves them (FXSAVE) when it switches
# to another program and loads them back (FXRSTOR) when the program runs again, a new program starts
# with clean ones, and an unmasked x87 error ends the program (#MF). programs/fputest goes into
# copies of bin/os.bin only; QEMU monitor samples ("stop", "info registers", "cont") show the CPU's
# registers from outside.
#
#   host  scripts/tests/host/fpu-owner.c: the real kernel/task/task.c with a stand-in FPU that keeps a
#         ledger of every save and restore. It checks which task's state is in the CPU (the owner)
#         after a first launch, a return to the same task, A -> B -> A, a sleeping owner and the
#         idle halt, the owner freed (never saved into afterwards, also when the next task gets its
#         block), another task freed, a task discarded before it was published, task_page_task,
#         a misaligned area, and that a new task starts clean while another one's state is loaded.
#   disk  the default boot. The shell starts fputest.elf (A), which starts "fresh" children (clean
#         registers while A's own x87, then x87 and SSE, state is loaded; A's is the same afterwards)
#         and B. A and B each load their own x87 values, control word, XMM0-7 and MXCSR and spin
#         without a syscall until the kernel has come back to them from an interrupt, 150 times
#         with x87 only and 150 with both, then sleep 10 times and make 50 syscalls with both
#         loaded; after each, their arithmetic on the registers must give the results it gave
#         before. At least half of the spins must have taken 2 or more ticks (another task ran).
#         Samples in ring 3 with A's or B's spin marker in EDI must show that program's control
#         word, stack top, MXCSR and XMM0-7, and there must be some of each program in each kind
#         of spin. Then from the shell, each ended by a key: "dirty" ends with exit and "fault"
#         with a page fault, each right after loading a last XMM7; the program after each
#         ("afterexit", "afterfault", as many words, so the shell's command takes the same heap
#         blocks) gets the same task block and must start clean, and once the shell owns the FPU
#         again (fpu_owner in task.c, read at its address in the relinked kernel, see
#         lib/kernel-symbols.sh) the freed block must not hold that last XMM7 (the kernel must not
#         save the state of a task it freed). Last "mf": a zero divide with the exception unmasked,
#         then FWAIT, must end the program.
#   pending  an unmasked x87 zero divide left pending by "fputest.elf pending <how>" (P) while another
#         program, "survivor" (W), runs with its own state: P is switched away with the error pending
#         by the timer in a spin without a syscall (preempt), by a sleep, or makes 50 syscalls; it
#         must come back with the error still pending and its own SSE state, and end at its FWAIT.
#         The debugger (lib/exceptions.py catch mf) must see #MF (vector 16) from ring 3 at that
#         FWAIT and process_terminate on P's process; W must keep its state, never see an error
#         pending (FNSTSW, then FWAIT) and have been switched away. "pendexit" and "pendfault" end
#         by exit / a page fault with the error pending: W as before, and the next program, which
#         gets the same task block, starts clean and its FWAIT doesn't fault. The shell, which the
#         kernel loads right after the owner dies, doesn't check its own state: the host case
#         "freeowner" (fpu-owner.c) proves it doesn't inherit the error. Needs python3.
#   idle  fputest.elf as shell.elf, the only program, on a Pentium III CPU model (SSE, no SSE2): it
#         loads its state and sleeps in 100 ms steps until a key, so the kernel idles with
#         interrupts on. All 20 samples meanwhile (some halted in ring 0) must show its registers,
#         and its task block must still hold the clean image it started with (the kernel saves
#         a program's state only when it switches to another one); after the key its arithmetic
#         must still be right, and a child it starts is clean.
#   grub  the idle case through the GRUB ISO (the disk's boot sector replaced by "jmp $"). Skipped
#         without the ISO tools (fails with RORTHOX_REQUIRE_ISO=1).
#   cpu   no FXSAVE/FXRSTOR (-cpu qemu32,-fxsr), then no SSE (-cpu qemu32,-sse): the kernel must stop
#         with its unsupported-CPU message before it changes CR0 or CR4 (MP, EM, TS, NE, OSFXSR and
#         OSXMMEXCPT still clear) and before it goes on (no "Thats cool" on debugcon). The kernel
#         decides by CPUID, which QEMU masks under KVM too.
# bin/os.bin itself must not change. In the disk, idle and grub cases, every sample must show the
# control register bits the kernel sets for the FPU (CR0 MP and NE set, EM and TS clear; CR4 OSFXSR
# and OSXMMEXCPT set, OSXSAVE clear). The fresh programs also check the x87 instruction and operand
# pointers (FNSTENV), which FXSAVE may store as zero: they must be zero, and a program's own or zero
# after its child ran.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60   RUN_TIMEOUT=120   FPU_STATE_CASES="host disk pending idle grub cpu"
#      FPU_PENDING_CASES="preempt sleep syscall exit fault"

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-120}"
CASES="${FPU_STATE_CASES:-host disk pending idle grub cpu}"
PENDING="${FPU_PENDING_CASES:-preempt sleep syscall exit fault}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/iso-check.sh
. ./scripts/tests/lib/kernel-symbols.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fpu-state
PROGRAM=programs/fputest/build/fputest.elf
mkdir -p "$OUT"
Q_OUT=$OUT   # finish reads it; qemu_start sets it for each boot
rm -rf "${OUT:?}"/disk "$OUT"/pending "$OUT"/idle "$OUT"/grub "$OUT"/cpu-* "$OUT"/host-*
[ -f "$PROGRAM" ] && [ -f build/kernelfull.o ] || { echo "fpu-state: run 'make all' first" >&2; exit 1; }
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
echo "fpu-state: kernel symbols"
kernel_symbols_or_stop "$OUT" fpu-state || exit 1
# The newest task (the end of the task list), and the task whose FPU state is in the CPU (static in
# task.c).
TASK_TAIL=$SYM_TASK_TAIL
FPU_OWNER=$(awk '$3 == "fpu_owner" { print $1 }' "$OUT/kernel-nm.txt")
# The clean image every task starts with (static in fpu.c), and where a task keeps its saved state.
FPU_PRISTINE=$(awk '$3 == "fpu_pristine" { print $1 }' "$OUT/kernel-nm.txt")
# The kernel heap table: one byte per block, 0 = free.
config_value() { awk -v key="$1" '$1 == "#define" && $2 == key { print $3 }' kernel/config.h; }
HEAP_TABLE=$(($(config_value RORTHOX_HEAP_TABLE_ADDRESS)))
HEAP_BASE=$(($(config_value RORTHOX_HEAP_ADDRESS)))
HEAP_BLOCK=$(($(config_value RORTHOX_HEAP_BLOCK_SIZE)))
printf '#include "task/task.h"\nchar task_fpu_offset[offsetof(struct task, fpu) + 1];\n' > "$OUT/fpu-offset.c"
TASK_FPU_OFFSET=
if i686-elf-gcc -std=gnu99 -ffreestanding -fno-common -I kernel -I build/config -c "$OUT/fpu-offset.c" \
        -o "$OUT/fpu-offset.o" > "$OUT/fpu-offset.log" 2>&1; then
    TASK_FPU_OFFSET=$(printf '%x' $((0x$(i686-elf-nm -S "$OUT/fpu-offset.o" | awk '$4 == "task_fpu_offset" { print $2 }') - 1)))
fi
echo "  fpu_owner ${FPU_OWNER:-not found}, fpu_pristine ${FPU_PRISTINE:-not found}, struct task: fpu at ${TASK_FPU_OFFSET:-?}"

# What QEMU prints for each program's registers (XMM as two 64-bit halves, high first; lanes as in
# programs/fputest: xmm0 the dividends, xmm1 3.0, xmm2-7 lane j = id << 24 | register << 16 | j).
xmm() {
    local s="$2 4040000040400000 4040000040400000" i
    for i in 2 3 4 5 6 7; do
        s+=$(printf ' %02x%02x0003%02x%02x0002 %02x%02x0001%02x%02x0000' "$1" "$i" "$1" "$i" "$1" "$i" "$1" "$i")
    done
    echo "$s"
}
FCW_A=077f MXCSR_A=00003fa0 XMM_A=$(xmm 0x41 "40a0000040800000 400000003f800000")
FCW_B=0a7f MXCSR_B=00005fa0 XMM_B=$(xmm 0x42 "4130000041200000 4100000040e00000")
FCW_I=0e7f MXCSR_I=00007fa0 XMM_I=$(xmm 0x49 "4188000041800000 4160000041500000")

sample() {
    qemu_cmd "stop"
    qemu_cmd "info registers"
    qemu_cmd "cont"
}

# samples <monitor.log>: one line per register dump: cpl hlt edi fcw top mxcsr xmm0..7.
samples() {
    tr -d '\r' < "$1" | awk '
        function emit() { if (have) print cpl, hlt, edi, fcw, top, mx, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7] }
        /^EAX=/ { emit(); have = 1; cpl = hlt = edi = fcw = top = mx = "-"; for (i = 0; i < 8; i++) x[i] = "- -" }
        !have { next }
        { for (i = 1; i <= NF; i++) {
              if ($i ~ /^EDI=/) edi = substr($i, 5)
              else if ($i ~ /^CPL=/) cpl = substr($i, 5)
              else if ($i ~ /^HLT=/) hlt = substr($i, 5)
              else if ($i ~ /^FCW=/) fcw = substr($i, 5)
              else if ($i ~ /^\[ST=/) { top = $i; gsub(/[^0-9]/, "", top) }
              else if ($i ~ /^MXCSR=/) mx = substr($i, 7)
              else if ($i ~ /^XMM0[0-7]=/) x[substr($i, 5, 1)] = substr($i, 7) " " $(i + 1)
          } }
        END { emit() }'
}

# memory <hex address> <bytes> <file>: save guest memory (VM running).
memory() {
    rm -f "$3"
    qemu_cmd "pmemsave 0x$1 $2 \"$3\""
    wait_file "$3" "$2"
}

read_word() { memory "$1" 4 "$D/word.bin" && od -An -tx4 -N4 "$D/word.bin" | tr -d ' \n'; }

# control <monitor.log>: CR0 and CR4 in every register dump. CR0: MP (1) and NE (5) set, EM
# (2) and TS (3) clear; CR4: OSFXSR (9) and OSXMMEXCPT (10) set, OSXSAVE (18) clear.
control() {
    tr -d '\r' < "$1" | awk '
        function hex(s,    v, i) { v = 0; for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", tolower(substr(s, i, 1))) - 1; return v }
        function bit(v, b) { return int(v / 2 ^ b) % 2 }
        { for (i = 1; i <= NF; i++) {
              if ($i ~ /^CR0=/) { n++; v = hex(substr($i, 5)); if (!bit(v, 1) || !bit(v, 5) || bit(v, 2) || bit(v, 3)) bad0++ }
              else if ($i ~ /^CR4=/) { m++; v = hex(substr($i, 5)); if (!bit(v, 9) || !bit(v, 10) || bit(v, 18)) bad4++ }
          } }
        END { print (n > 0 && n == m ? "seen" : "none") ", CR0 wrong " bad0 + 0 ", CR4 wrong " bad4 + 0 " (" n + 0 " samples)" }' > "$D/control.txt"
    sed 's/^/  control registers: /' "$D/control.txt"
    check "$C: every sample: CR0 MP NE set, EM TS clear; CR4 OSFXSR OSXMMEXCPT set, OSXSAVE clear" \
        "$D/control.txt" "seen, CR0 wrong 0, CR4 wrong 0"
}

# pristine <task>: the task's saved FPU image is still the clean image (a task that has never been
# switched away from has never been saved).
pristine() {
    [ -n "$1" ] && [ -n "$FPU_PRISTINE" ] && [ -n "$TASK_FPU_OFFSET" ] || return 1
    memory "$(printf '%x' $((0x$1 + 0x$TASK_FPU_OFFSET)))" 512 "$D/task-fpu.bin" &&
        memory "$FPU_PRISTINE" 512 "$D/fpu-pristine.bin" || return 1
    if cmp -s "$D/task-fpu.bin" "$D/fpu-pristine.bin"; then echo clean; else echo saved; fi > "$D/task-fpu.txt"
}

# wait_tail <task> <seconds>: until the newest task is <task> (the programs after it have ended).
wait_tail() {
    local deadline=$((SECONDS + $2))
    while ((SECONDS < deadline)); do
        [ "$(read_word "$TASK_TAIL")" = "$1" ] && return 0
        qemu_running || return 1
        sleep 0.2
    done
    return 1
}

# look <seconds> <samples per look> <name>=<ere>...: until every <ere> has matched a whole row,
# keeping the first screen each matched on as $D/screen-<name>.txt (later scrolling doesn't
# matter), with a leading "> " (the shell's prompt) dropped from every row. Takes that many
# register samples between looks.
look() {
    local limit=$1 deadline=$((SECONDS + $1)) per=$2 spec left i
    shift 2
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running"; return 1; }
        for ((i = 0; i < per; i++)); do sample; sleep 0.05; done
        screen_snapshot
        sed 's/^> //' "$Q_OUT/screen.txt" > "$D/rows.txt"
        left=0
        for spec in "$@"; do
            [ -f "$D/screen-${spec%%=*}.txt" ] && continue
            if grep -qxE -- "${spec#*=}" "$D/rows.txt"; then cp "$D/rows.txt" "$D/screen-${spec%%=*}.txt"; else left=1; fi
        done
        [ "$left" -eq 0 ] && return 0
        sleep 0.1
    done
    echo "  gave up after $limit s waiting for:"
    for spec in "$@"; do [ -f "$D/screen-${spec%%=*}.txt" ] || echo "    ${spec#*=}"; done
    return 1
}

seen() { check "$C: $2" "$D/screen-$1.txt" "$2" line; }
RESULT="[0-9]+ of [0-9]+ checks failed"
result() { seen "$1" "$2: 0 of $3 checks failed"; }

# windows <name> <who> <checks>: no check failed, and at least 75 of the 150 spins took 2+ ticks.
windows() {
    local row k
    row=$(grep -xE "$2: 0 of $3 checks failed, [0-9]+ of 150 switched away" "$D/screen-$1.txt" 2> /dev/null | head -n 1 || true)
    k=$(sed -E 's/.*, ([0-9]+) of 150 switched away$/\1/' <<< "${row:-x, 0 of 150 switched away}")
    if [ -n "$row" ] && [ "$k" -ge 75 ]; then echo ok; else echo "not ok: ${row:-no row}"; fi > "$D/windows-$1.txt"
    check "$C: $2: 0 of $3 checks failed, at least 75 of 150 switched away" "$D/windows-$1.txt" ok line
}

# step <command>...: run it if the shell is the newest task, then wait until the shell is the newest
# task again. A failed step counts, and the next ones still run.
step() {
    [ "$ready" = 1 ] || { status=1; return 0; }
    "$@" || status=1
    wait_tail "$SHELL_TASK" "$RUN_TIMEOUT" || { echo "  the shell is not the newest task again"; ready=0; status=1; }
}

# exchange <variable> <command> <name> <ready row>: type a command whose program waits for a key;
# at the ready row, note the program's task in <variable>, then send the key.
exchange() {
    type_line "$2"
    look "$RUN_TIMEOUT" 0 "$3=$4" || return 1
    printf -v "$1" '%s' "$(read_word "$TASK_TAIL")"
    qemu_cmd "sendkey ret"
}

# freed <task> <name> <last XMM7>: once the shell owns the FPU again (the kernel has switched to it
# after the program ended), the program's freed task block must not hold its last XMM7.
freed() {
    local deadline=$((SECONDS + RUN_TIMEOUT))
    [ -n "$1" ] && [ -n "$FPU_OWNER" ] || return 1
    until [ "$(read_word "$FPU_OWNER")" = "$SHELL_TASK" ]; do
        ((SECONDS < deadline)) && qemu_running || { echo "  the shell does not own the FPU"; return 1; }
        sleep 0.1
    done
    memory "$1" 4096 "$D/freed-$2.bin" || return 1
    if grep -qaF -- "$3" "$D/freed-$2.bin"; then echo kept; else echo "not there"; fi > "$D/freed-$2.txt"
}

driver() {
    type_line "fputest.elf"
    look "$RUN_TIMEOUT" 5 \
        "x87child=fputest fresh x87: $RESULT" "x87after=fputest A after the x87 child: $RESULT" \
        "x87a=fputest A x87 windows: $RESULT, [0-9]+ of [0-9]+ switched away" \
        "x87b=fputest B x87 windows: $RESULT, [0-9]+ of [0-9]+ switched away" \
        "ssechild=fputest fresh sse: $RESULT" "sseafter=fputest A after the sse child: $RESULT" \
        "ssea=fputest A sse windows: $RESULT, [0-9]+ of [0-9]+ switched away" \
        "sseb=fputest B sse windows: $RESULT, [0-9]+ of [0-9]+ switched away" \
        "calla=fputest A syscall windows: $RESULT" "callb=fputest B syscall windows: $RESULT"
}

x87_error() {
    type_line "fputest.elf mf"
    look "$RUN_TIMEOUT" 0 "mf=fputest mf: .*"
}

host_case() {
    local status="not built"
    echo "fpu-state: host check of kernel/task/task.c"
    if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=undefined -fno-sanitize=alignment \
            -fno-sanitize-recover=all -Wl,--wrap=kzalloc,--wrap=kfree -I kernel -I build/config -I scripts/tests/host \
            scripts/tests/host/fpu-owner.c scripts/tests/host/kalloc.c kernel/task/task.c kernel/memory/memory.c \
            -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
        if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then status=0; else status=$?; fi
        sed 's/^/  /' "$OUT/host-check.log"
    else
        echo "  host build failed, see $OUT/host-build.log"
        head -n 20 "$OUT/host-build.log" | sed 's/^/  /' || true
        : > "$OUT/host-check.log"
    fi
    echo "exit=$status" > "$OUT/host-status.txt"
    check "host: built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
    check "host: every ownership case passed" "$OUT/host-check.log" "fpu-owner host check: 0 of 12 cases failed" line
    check "host: no undefined behaviour" "$OUT/host-check.log" "runtime error" absent
}

disk_case() {
    C=disk D="$OUT/disk"
    mkdir -p "$D"
    cp bin/os.bin "$D/fpu.bin"
    mcopy -i "$D/fpu.bin" "$PROGRAM" ::
    echo "fpu-state: disk"
    status=0 ready=0 DIRTY_TASK= EXIT_TASK= FAULT_TASK= AFTER_TASK=
    qemu_start "$D/fpu.bin" "$D"
    if wait_screen_lines "$BOOT_TIMEOUT" "line:${RORTHOX_NAME} v${RORTHOX_VERSION}" "line:>"; then
        SHELL_TASK=$(read_word "$TASK_TAIL") && ready=1
    fi
    step driver
    step exchange DIRTY_TASK "fputest.elf dirty" dirty "fputest dirty: loaded, waiting for a key"
    step freed "$DIRTY_TASK" exit "EXIT FINAL STATE"
    step exchange EXIT_TASK "fputest.elf afterexit" afterexit "fputest fresh afterexit: waiting for a key"
    step exchange FAULT_TASK "fputest.elf fault" fault "fputest fault: loaded, waiting for a key"
    step freed "$FAULT_TASK" fault "FAULT FINAL STAT"
    step exchange AFTER_TASK "fputest.elf afterfault" afterfault "fputest fresh afterfault: waiting for a key"
    step x87_error
    screen_snapshot
    sed 's/^> //' "$D/screen.txt" > "$D/screen-mfend.txt"
    echo "exit=$status" > "$D/steps.txt"
    qemu_stop || true
    samples "$D/monitor.log" > "$D/samples.txt"
    control "$D/monitor.log"

    check "$C: every step ran" "$D/steps.txt" "exit=0" line
    result x87child "fputest fresh x87" 10
    result x87after "fputest A after the x87 child" 5
    windows x87a "fputest A x87 windows" 600
    windows x87b "fputest B x87 windows" 600
    result ssechild "fputest fresh sse" 10
    result sseafter "fputest A after the sse child" 8
    windows ssea "fputest A sse windows" 1050
    windows sseb "fputest B sse windows" 1050
    result calla "fputest A syscall windows" 420
    result callb "fputest B syscall windows" 420
    awk -v fa="$FCW_A" -v ma="$MXCSR_A" -v xa="$XMM_A" -v fb="$FCW_B" -v mb="$MXCSR_B" -v xb="$XMM_B" '
        $1 != 3 { next }
        { x = $7; for (i = 8; i <= NF; i++) x = x " " $i }
        $3 == "f0410087" { n["A x87"]++; if ($4 != fa || $5 != 1) bad["A x87"]++ }
        $3 == "f0420087" { n["B x87"]++; if ($4 != fb || $5 != 1) bad["B x87"]++ }
        $3 == "f0410055" { n["A sse"]++; if ($4 != fa || $5 != 1 || $6 != ma || x != xa) bad["A sse"]++ }
        $3 == "f0420055" { n["B sse"]++; if ($4 != fb || $5 != 1 || $6 != mb || x != xb) bad["B sse"]++ }
        END { split("A x87,B x87,A sse,B sse", k, ",")
              for (i = 1; i <= 4; i++) print k[i] " samples " (n[k[i]] > 0 ? "seen" : "none") ", wrong " bad[k[i]] + 0
              for (i = 1; i <= 4; i++) print k[i] ": " n[k[i]] + 0 " samples" }
    ' "$D/samples.txt" > "$D/windows.txt"
    sed 's/^/  /' "$D/windows.txt"
    local w
    for w in "A x87" "B x87" "A sse" "B sse"; do
        check "$C: samples in $w spins show that program's registers" "$D/windows.txt" "$w samples seen, wrong 0" line
    done
    seen dirty "fputest dirty: loaded, waiting for a key"
    check "$C: the block freed at exit doesn't hold the program's last XMM7" "$D/freed-exit.txt" "not there" line
    result afterexit "fputest fresh afterexit" 10
    seen fault "fputest fault: loaded, waiting for a key"
    check "$C: the block freed after the fault doesn't hold its last XMM7" "$D/freed-fault.txt" "not there" line
    result afterfault "fputest fresh afterfault" 10
    echo "dirty $DIRTY_TASK afterexit $EXIT_TASK fault $FAULT_TASK afterfault $AFTER_TASK" > "$D/tasks.txt"
    [ -n "$EXIT_TASK" ] && [ "$EXIT_TASK" = "$DIRTY_TASK" ] && echo same > "$D/reuse-exit.txt" || echo differs > "$D/reuse-exit.txt"
    [ -n "$AFTER_TASK" ] && [ "$AFTER_TASK" = "$FAULT_TASK" ] && echo same > "$D/reuse-fault.txt" || echo differs > "$D/reuse-fault.txt"
    check "$C: afterexit got the task block of the program that exited" "$D/reuse-exit.txt" same line
    check "$C: afterfault got the task block of the program that faulted" "$D/reuse-fault.txt" same line
    seen mf "fputest mf: error pending"
    check "$C: mf: the program ended at FWAIT" "$D/screen-mfend.txt" "fputest mf: FAIL" absent
    check "$C: no kernel panic (debugcon)" "$D/debugcon.log" "PANIC" absent
}

# key_when_running: send a key once the monitor reports the guest running again (the debugger has
# just continued it, and QEMU drops key presses while the guest is stopped).
key_when_running() {
    local i seen
    seen=$(grep -a -c 'VM status' "$Q_OUT/monitor.log" || true)
    for ((i = 0; i < 50; i++)); do
        qemu_cmd "info status"
        screen_snapshot      # a monitor round trip: the status line is in monitor.log after it
        if grep -a 'VM status' "$Q_OUT/monitor.log" | tail -n +$((seen + 1)) | grep 'VM status: running' > /dev/null; then
            qemu_cmd "sendkey ret"
            return 0
        fi
        sleep 0.1
    done
    echo "  the guest did not run again after the debugger continued it"
    return 1
}

# survivor <how>: W's result once it has its key: no check failed, and it was switched away from.
survivor() {
    look "$RUN_TIMEOUT" 0 "w-$1=fputest W $1: $RESULT, [0-9]+ of [0-9]+ switched away"
}

# pending_mf <how>: "fputest.elf pending <how>". The debugger (exceptions.py catch mf) waits at the
# #MF, #NM and #GP stubs from the key on; P must end at its FWAIT, then W gets a key.
pending_mf() {
    local py i s=0
    type_line "fputest.elf pending $1"
    look "$RUN_TIMEOUT" 0 "ready-$1=fputest P $1: waiting for a key" || return 1
    rm -f "$D/ready-$1"
    python3 -B scripts/tests/lib/exceptions.py catch "$SOCK" build/kernel.map "$D/catch-$1.txt" "$D/ready-$1" mf \
        > "$D/catch-$1.log" 2>&1 &
    py=$!
    for ((i = 0; i < 100; i++)); do [ -f "$D/ready-$1" ] && break; sleep 0.1; done
    [ -f "$D/ready-$1" ] && { key_when_running || true; }
    wait "$py" || s=1
    sed 's/^/  /' "$D/catch-$1.log"
    look "$RUN_TIMEOUT" 0 "p-$1=fputest P $1: $RESULT, error pending" || return 1
    key_when_running && survivor "$1" && return "$s"
}

# pending_end <exit|fault>: "fputest.elf pend<how>" ends with the error pending. Once its task block
# is free in the kernel heap (the task is gone), W gets a key.
pending_end() {
    local task i
    type_line "fputest.elf pend$1"
    look "$RUN_TIMEOUT" 0 "ready-$1=fputest pend$1: waiting for a key" || return 1
    task=$(read_word "$TASK_TAIL")
    printf -v "PEND_$1" '%s' "$task"
    qemu_cmd "sendkey ret"
    look "$RUN_TIMEOUT" 0 "end-$1=fputest pend$1: $RESULT, (exiting|faulting) with the error pending" || return 1
    for ((i = 0; i < 100; i++)); do
        memory "$(printf '%x' $((HEAP_TABLE + (0x$task - HEAP_BASE) / HEAP_BLOCK)))" 1 "$D/block.bin" || return 1
        [ "$(od -An -tu1 "$D/block.bin" | tr -d ' ')" = 0 ] && break
        sleep 0.1
    done
    ((i < 100)) || { echo "  the task block of fputest pend$1 was not freed"; return 1; }
    qemu_cmd "sendkey ret"
    survivor "$1"
}

# pending_case: an unmasked x87 error left pending across switches (programs/fputest pending,
# survivor, pendexit, pendfault), with the debugger on the exception stubs for the #MF cases.
GDB_DIR=
gdb_cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; GDB_DIR=; }
pending_case() {
    C=pending D="$OUT/pending"
    mkdir -p "$D"
    cp bin/os.bin "$D/fpu.bin"
    mcopy -i "$D/fpu.bin" "$PROGRAM" ::
    echo "fpu-state: pending"
    command -v python3 > /dev/null || { echo "fpu-state: python3 not found" >&2; exit 1; }
    status=0 ready=0 PEND_exit= PEND_fault= AFTER_exit= AFTER_fault=
    GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
    Q_CLEANUP=gdb_cleanup
    SOCK="$GDB_DIR/gdb"
    QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$D/fpu.bin" "$D"
    if wait_screen_lines "$BOOT_TIMEOUT" "line:${RORTHOX_NAME} v${RORTHOX_VERSION}" "line:>"; then
        SHELL_TASK=$(read_word "$TASK_TAIL") && ready=1
    fi
    local how
    for how in $PENDING; do
        case "$how" in
            preempt | sleep | syscall) step pending_mf "$how" ;;
            exit | fault) step pending_end "$how"
                step exchange "AFTER_$how" "fputest.elf after$how" "after$how" "fputest fresh after$how: waiting for a key" ;;
            *) echo "fpu-state: unknown pending case $how" >&2; exit 2 ;;
        esac
    done
    echo "exit=$status" > "$D/steps.txt"
    qemu_stop || true
    gdb_cleanup

    check "$C: every step ran" "$D/steps.txt" "exit=0" line
    for how in $PENDING; do
        [[ $how =~ ^(preempt|sleep|syscall)$ ]] || continue
        local r="$D/catch-$how.txt"
        seen "p-$how" "fputest P $how: 0 of 7 checks failed, error pending"
        check "$C $how: the exception is #MF (vector 16)" "$r" "PASS stub_vector" line
        check "$C $how: from ring 3" "$r" "PASS cpu_ring3" line
        check "$C $how: at FWAIT" "$r" "PASS faulting_instruction" line
        check "$C $how: interrupt_handler got the right vector and frame" "$r" "PASS handler_frame" line
        check "$C $how: process_terminate on the faulting task's process" "$r" "PASS process_terminated" line
        survived "$how"
    done
    for how in $PENDING; do
        [[ $how =~ ^(exit|fault)$ ]] || continue
        local p="PEND_$how" a="AFTER_$how"
        if [ "$how" = exit ]; then seen "end-$how" "fputest pend$how: 0 of 3 checks failed, exiting with the error pending"
        else seen "end-$how" "fputest pend$how: 0 of 3 checks failed, faulting with the error pending"; fi
        survived "$how"
        result "after$how" "fputest fresh after$how" 10
        [ -n "${!a}" ] && [ "${!a}" = "${!p}" ] && echo same > "$D/reuse-$how.txt" || echo differs > "$D/reuse-$how.txt"
        check "$C: after$how got the task block of the program that ended with the error pending" \
            "$D/reuse-$how.txt" same line
    done
    check "$C: no program printed a failed check" "$D/screen.txt" ": FAIL " absent
    check "$C: no kernel panic (debugcon)" "$D/debugcon.log" "PANIC" absent
}

# survived <how>: W's last line, after P ended: no check failed, and it was switched away from.
survived() {
    local row k
    row=$(grep -xE "fputest W $1: 0 of [0-9]+ checks failed, [0-9]+ of [0-9]+ switched away" "$D/screen-w-$1.txt" 2> /dev/null | head -n 1 || true)
    k=$(sed -E 's/.*, ([0-9]+) of [0-9]+ switched away$/\1/' <<< "${row:-x, 0 of 0 switched away}")
    if [ -n "$row" ] && [ "$k" -ge 1 ]; then echo ok; else echo "not ok: ${row:-no row}"; fi > "$D/survivor-$1.txt"
    check "$C $1: the other program (W) kept its state, never had an error pending and was switched away" \
        "$D/survivor-$1.txt" ok line
}

# cpu_case <name> <cpu> <message>: the kernel must stop with <message> before it changes CR0 or CR4.
cpu_case() {
    C=cpu-$1 D="$OUT/cpu-$1"
    echo "fpu-state: $C ($2)"
    mkdir -p "$D"
    cp bin/os.bin "$D/fpu.bin"
    QEMU_CPU=$2 qemu_start "$D/fpu.bin" "$D"
    local status=0 re
    re=$(sed 's/[][\.*^$()+?{}|]/\\&/g' <<< "$3")
    look "$BOOT_TIMEOUT" 0 "stop=.*$re.*" || status=1
    qemu_cmd "info registers"
    screen_snapshot      # a monitor round trip: the register dump is in monitor.log after it
    echo "exit=$status" > "$D/steps.txt"
    qemu_stop || true
    # The bits fpu_init sets or clears: CR0 MP (1), EM (2), TS (3), NE (5); CR4 OSFXSR (9),
    # OSXMMEXCPT (10). Before it, all of them are clear.
    tr -d '\r' < "$D/monitor.log" | awk '
        function hex(s,    v, i) { v = 0; for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", tolower(substr(s, i, 1))) - 1; return v }
        function bit(v, b) { return int(v / 2 ^ b) % 2 }
        { for (i = 1; i <= NF; i++) {
              if ($i ~ /^CR0=/) { v = hex(substr($i, 5)); cr0 = bit(v, 1) bit(v, 2) bit(v, 3) bit(v, 5) }
              else if ($i ~ /^CR4=/) { v = hex(substr($i, 5)); cr4 = bit(v, 9) bit(v, 10) }
          } }
        END { print "CR0 MP EM TS NE " (cr0 == "" ? "?" : cr0) ", CR4 OSFXSR OSXMMEXCPT " (cr4 == "" ? "?" : cr4) }' > "$D/control.txt"
    sed 's/^/  /' "$D/control.txt"
    check "$C: every step ran" "$D/steps.txt" "exit=0" line
    check "$C: the kernel stopped with: $3" "$D/screen.txt" "$3"
    check "$C: CR0 and CR4 as before (the FPU bits not set)" "$D/control.txt" \
        "CR0 MP EM TS NE 0000, CR4 OSFXSR OSXMMEXCPT 00" line
    check "$C: the shell did not start" "$D/screen.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}" absent
    check "$C: the kernel went no further than fpu_init (debugcon)" "$D/debugcon.log" "Thats cool" absent
    check "$C: no CPU exception in the kernel (debugcon)" "$D/debugcon.log" "exception:" absent
}

# idle_case <case> <image> <cpu> [qemu flags]
idle_case() {
    C=$1 D="$OUT/$1"
    echo "fpu-state: $C"
    local status=0 i
    QEMU_CPU=$3 QEMU_FLAGS="${4:-}" qemu_start "$2" "$D"
    {
        look "$BOOT_TIMEOUT" 0 "loaded=fputest I: loaded, sleeping until a key" &&
        for ((i = 0; i < 20; i++)); do sample; sleep 0.1; done &&
        { pristine "$(read_word "$TASK_TAIL")" || echo "not read" > "$D/task-fpu.txt"; } &&
        qemu_cmd "sendkey ret" &&
        look "$RUN_TIMEOUT" 0 "idle=fputest I idle: $RESULT" "child=fputest fresh idle: $RESULT" \
            "after=fputest I after the child: $RESULT"
    } || status=1
    echo "exit=$status" > "$D/steps.txt"
    qemu_stop || true
    samples "$D/monitor.log" > "$D/samples.txt"
    control "$D/monitor.log"
    awk -v f="$FCW_I" -v m="$MXCSR_I" -v xi="$XMM_I" '
        { x = $7; for (i = 8; i <= NF; i++) x = x " " $i; n++ }
        $1 == 0 && $2 == 1 { halted++ }
        $4 != f || $5 != 1 || $6 != m || x != xi { bad++ }
        END { print "samples " n + 0 ", halted in ring 0 " (halted > 0 ? "yes" : "no") ", wrong " bad + 0 }
    ' "$D/samples.txt" > "$D/idle.txt"
    sed 's/^/  /' "$D/idle.txt"
    check "$C: every step ran" "$D/steps.txt" "exit=0" line
    check "$C: 20 samples while it sleeps, some halted, all with its registers" "$D/idle.txt" \
        "samples 20, halted in ring 0 yes, wrong 0" line
    check "$C: its saved FPU image is still the clean one (no FPU work on a return to the same task)" \
        "$D/task-fpu.txt" clean line
    result idle "fputest I idle" 7
    result child "fputest fresh idle" 10
    result after "fputest I after the child" 8
    check "$C: no kernel panic (debugcon)" "$D/debugcon.log" "PANIC" absent
}

for c in $CASES; do
    case "$c" in
        host) host_case ;;
        disk) disk_case ;;
        pending) pending_case ;;
        cpu)
            cpu_case fxsr qemu32,-fxsr "Unsupported CPU: no FXSAVE/FXRSTOR (CPUID.1:EDX bit 24)"
            cpu_case sse qemu32,-sse "Unsupported CPU: no SSE (CPUID.1:EDX bit 25)"
            ;;
        idle | grub)
            mkdir -p "$OUT/$c"
            cp bin/os.bin "$OUT/$c/fpu.bin"
            mattrib -i "$OUT/$c/fpu.bin" -r ::shell.elf
            mcopy -o -i "$OUT/$c/fpu.bin" "$PROGRAM" ::shell.elf
            if [ "$c" = idle ]; then
                idle_case idle "$OUT/idle/fpu.bin" pentium3
            elif iso_check "$OUT/grub" "fpu-state grub"; then
                # "jmp $" in place of the boot sector's first bytes: only GRUB can start the kernel.
                printf '\xeb\xfe' | dd of="$OUT/grub/fpu.bin" bs=1 count=2 conv=notrunc status=none
                idle_case grub "$OUT/grub/fpu.bin" qemu32 "-boot d -cdrom bin/rorthox.iso"
            else
                [ $? -eq 77 ] || { echo "FAIL fpu-state: no up-to-date GRUB ISO (see above)" >&2; exit 1; }
            fi
            ;;
        *) echo "fpu-state: unknown case $c" >&2; exit 2 ;;
    esac
done

[ "$hash_before" = "$(image_hash)" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test" "$OUT/image.txt" unchanged line
finish fpu-state
