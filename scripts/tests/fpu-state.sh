#!/usr/bin/env bash
#
# Every program has its own x87 and SSE registers: the kernel saves them (FXSAVE) when it switches
# to another program and loads them back (FXRSTOR) when the program runs again, a new program starts
# with clean ones, and an unmasked x87 error ends the program (#MF). programs/fputest goes into
# copies of bin/os.bin only; QEMU monitor samples ("stop", "info registers", "cont") show the CPU's
# registers from outside.
#
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
#   idle  fputest.elf as shell.elf, the only program, on a Pentium III CPU model (SSE, no SSE2): it
#         loads its state and sleeps in 100 ms steps until a key, so the kernel idles with
#         interrupts on. All 20 samples meanwhile (some halted in ring 0) must show its registers;
#         after the key its arithmetic must still be right, and a child it starts is clean.
#   grub  the idle case through the GRUB ISO (the disk's boot sector replaced by "jmp $"). Skipped
#         without the ISO tools (fails with RORTHOX_REQUIRE_ISO=1).
# bin/os.bin itself must not change.
#
# Env: BOOT_TIMEOUT=60   RUN_TIMEOUT=120   FPU_STATE_CASES="disk idle grub"

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-120}"
CASES="${FPU_STATE_CASES:-disk idle grub}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/iso-check.sh
. ./scripts/tests/lib/kernel-symbols.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fpu-state
PROGRAM=programs/fputest/build/fputest.elf
mkdir -p "$OUT"
Q_OUT=$OUT   # finish reads it; qemu_start sets it for each boot
rm -rf "${OUT:?}"/disk "$OUT"/idle "$OUT"/grub
[ -f "$PROGRAM" ] && [ -f build/kernelfull.o ] || { echo "fpu-state: run 'make all' first" >&2; exit 1; }
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
echo "fpu-state: kernel symbols"
kernel_symbols_or_stop "$OUT" fpu-state || exit 1
# The newest task (the end of the task list), and the task whose FPU state is in the CPU (static in
# task.c).
TASK_TAIL=$SYM_TASK_TAIL
FPU_OWNER=$(awk '$3 == "fpu_owner" { print $1 }' "$OUT/kernel-nm.txt")
echo "  fpu_owner ${FPU_OWNER:-not found}"

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

    check "$C: every step ran" "$D/steps.txt" "exit=0" line
    result x87child "fputest fresh x87" 9
    result x87after "fputest A after the x87 child" 4
    windows x87a "fputest A x87 windows" 600
    windows x87b "fputest B x87 windows" 600
    result ssechild "fputest fresh sse" 9
    result sseafter "fputest A after the sse child" 7
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
    result afterexit "fputest fresh afterexit" 9
    seen fault "fputest fault: loaded, waiting for a key"
    check "$C: the block freed after the fault doesn't hold its last XMM7" "$D/freed-fault.txt" "not there" line
    result afterfault "fputest fresh afterfault" 9
    echo "dirty $DIRTY_TASK afterexit $EXIT_TASK fault $FAULT_TASK afterfault $AFTER_TASK" > "$D/tasks.txt"
    [ -n "$EXIT_TASK" ] && [ "$EXIT_TASK" = "$DIRTY_TASK" ] && echo same > "$D/reuse-exit.txt" || echo differs > "$D/reuse-exit.txt"
    [ -n "$AFTER_TASK" ] && [ "$AFTER_TASK" = "$FAULT_TASK" ] && echo same > "$D/reuse-fault.txt" || echo differs > "$D/reuse-fault.txt"
    check "$C: afterexit got the task block of the program that exited" "$D/reuse-exit.txt" same line
    check "$C: afterfault got the task block of the program that faulted" "$D/reuse-fault.txt" same line
    seen mf "fputest mf: error pending"
    check "$C: mf: the program ended at FWAIT" "$D/screen-mfend.txt" "fputest mf: FAIL" absent
    check "$C: no kernel panic (debugcon)" "$D/debugcon.log" "PANIC" absent
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
        qemu_cmd "sendkey ret" &&
        look "$RUN_TIMEOUT" 0 "idle=fputest I idle: $RESULT" "child=fputest fresh idle: $RESULT" \
            "after=fputest I after the child: $RESULT"
    } || status=1
    echo "exit=$status" > "$D/steps.txt"
    qemu_stop || true
    samples "$D/monitor.log" > "$D/samples.txt"
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
    result idle "fputest I idle" 7
    result child "fputest fresh idle" 9
    result after "fputest I after the child" 7
    check "$C: no kernel panic (debugcon)" "$D/debugcon.log" "PANIC" absent
}

for c in $CASES; do
    case "$c" in
        disk) disk_case ;;
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
