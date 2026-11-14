#!/usr/bin/env bash
#
# The accelerator switch (scripts/qemu-accel.sh, qemu_start in scripts/qemu-lib.sh and the runner),
# with a stand-in for QEMU that logs its arguments and answers the monitor; no real QEMU runs.
# 1. tcg (the default, and set): qemu_start adds exactly one "-accel tcg" and "-cpu qemu32", no KVM
#    probe runs, and "info kvm" is not sent.
# 2. QEMU_FLAGS that choose the accelerator or the CPU (-accel, --accel, -enable-kvm, accel= in
#    -machine/-M, -cpu) are refused before QEMU starts; ordinary flags are not.
# 3. kvm where KVM doesn't work (QEMU can't open /dev/kvm, the probe hangs, or KVM runs with the
#    in-kernel PIC and PIT): qemu_start and the runner stop with a message that says why, start no
#    guest and run no test. QEMU's reason is cut to 1000 bytes and the CI annotation to 1500.
# 4. kvm where it works: the probe asks "info kvm" and "info qtree" (isa-i8259 and isa-pit, as the
#    stand-in, like QEMU 8.2, gives only with kernel-irqchip=split and refuses kernel-irqchip=off);
#    exactly one "-accel kvm,kernel-irqchip=split", no tcg, "info kvm" is the guest's first monitor
#    command; a QEMU that then doesn't say "kvm support: enabled" fails in qemu_stop, and the
#    runner fails a test with a monitor.log without it (but not one with it).
# 5. auto: a failed or hanging probe gives TCG with the reason, decided before the first test starts;
#    a working probe gives KVM, and a test that fails under it fails, it doesn't run again.
# 6. The reports name the accelerator, the reason, the QEMU version and the CPU model.
# 7. Lint: only qemu-lib.sh starts a guest QEMU (no test can leave out or add an accelerator), no
#    test sets QEMU_FLAGS with an accelerator or CPU; tests relying on the TCG-only "Triple fault"
#    line or "-d int" output skip under KVM through qemu_require_tcg ("CPU Reset" dumps appear
#    under both accelerators).

set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
. ./scripts/qemu-lib.sh   # also needs build/config (make all)

OUT=${RORTHOX_TEST_ROOT:-build/tests}/accelerator
rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
# This test chooses the accelerator for each case itself.
unset RORTHOX_ACCEL RORTHOX_ACCEL_REASON QEMU_FLAGS QEMU_CPU GITHUB_STEP_SUMMARY
export GITHUB_ACTIONS=false
runner="$PWD/scripts/run-tests.sh"
failures=0
ok() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; failures=$((failures + 1)); }
expect() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }

FAKE="$OUT/fake-qemu"
cat > "$FAKE" << 'EOF'
#!/usr/bin/env bash
# Stand-in for QEMU. FAKE_KVM: denied (QEMU can't use KVM), long (the same with a 5000-byte reason),
# hang, ok, fallback (QEMU runs, but not with KVM, as with accel=kvm:tcg), guest-off (the probe works,
# a guest then reports KVM disabled), in-kernel (KVM with the in-kernel PIC and PIT even with
# kernel-irqchip=split) or many (as in-kernel, with 400 PITs). Like QEMU 8.2 under KVM:
# kernel-irqchip=off is refused ("KVM does not support userspace APIC"), split gives QEMU's
# isa-i8259/isa-pit/ioapic, on (the default) the in-kernel kvm-i8259/kvm-pit/kvm-ioapic.
# FAKE_DEVICES, if set, is the list of devices info qtree shows instead.
# Logs "start <args>" and every monitor command to FAKE_QEMU_LOG.
if [ "${1:-}" = --version ]; then echo "QEMU emulator version 0.0.0 (stand-in)"; exit 0; fi
kvm=no guest=no irqchip=on
for a in "$@"; do
    case "$a" in
        kvm*) kvm=yes
              case "$a" in *kernel-irqchip=*) irqchip=${a##*kernel-irqchip=}; irqchip=${irqchip%%,*} ;; esac ;;
        file=*) guest=yes ;;
    esac
done
{ printf 'start'; printf ' %s' "$@"; printf '\n'; } >> "$FAKE_QEMU_LOG"
if [ $kvm = yes ]; then
    case "${FAKE_KVM:-denied}" in
        denied)
            echo "Could not access KVM kernel module: Permission denied" >&2
            echo "fake-qemu: failed to initialize kvm: Permission denied" >&2
            exit 1 ;;
        long)
            printf 'fake-qemu: failed to initialize kvm: %%0A%s\n' "$(head -c 5000 /dev/zero | tr '\0' x)" >&2
            exit 1 ;;
        hang) exec sleep 60 ;;
    esac
    if [ "$irqchip" = off ]; then
        echo "fake-qemu: KVM does not support userspace APIC" >&2
        exit 1
    fi
fi
pic=isa-i8259 pit=isa-pit ioapic=ioapic pits=1
if [ $kvm = yes ] && { [ "$irqchip" != split ] || [ "$FAKE_KVM" = in-kernel ] || [ "$FAKE_KVM" = many ]; }; then
    pic=kvm-i8259 pit=kvm-pit ioapic=kvm-ioapic
fi
[ "${FAKE_KVM:-}" = many ] && pits=400
answer="kvm support: disabled"
if [ $kvm = yes ] && { [ "$FAKE_KVM" = ok ] || [ "$FAKE_KVM" = in-kernel ] || [ "$FAKE_KVM" = many ] ||
                       { [ "$FAKE_KVM" = guest-off ] && [ $guest = no ]; }; }; then
    answer="kvm support: enabled"
fi
while IFS= read -r line; do
    echo "(qemu) $line"
    echo "monitor $line" >> "$FAKE_QEMU_LOG"
    case "$line" in
        "info kvm") echo "$answer" ;;
        "info qtree")
            if [ -n "${FAKE_DEVICES:-}" ]; then
                printf 'bus: main-system-bus\n'
                for d in $FAKE_DEVICES; do printf '  dev: %s, id ""\n' "$d"; done
                continue
            fi
            printf 'bus: main-system-bus\n  type System\n  dev: %s, id ""\n    gpio-in "" 24\n' "$ioapic"
            printf '      dev: isa-pcspk, id ""\n'
            for ((i = 0; i < pits; i++)); do printf '          dev: %s, id ""\n' "$pit"; done
            printf '          dev: %s, id ""\n            master = true\n' "$pic"
            printf '          dev: %s, id ""\n            master = false\n' "$pic" ;;
        pmemsave* | screendump*) f=${line#*\"}; f=${f%\"}; head -c 4000 /dev/zero | tr '\0' ' ' > "$f" ;;
        quit) exit 0 ;;
    esac
done
EOF
chmod +x "$FAKE"
IMG="$OUT/disk.bin"
: > "$IMG"

# boot <case> [VAR=value...]: qemu_start and qemu_stop with these settings, in a subshell. Sets
# status, log ($OUT/<case>.log) and qlog (what the stand-in saw).
boot() {
    local name="$1"
    shift
    qlog="$OUT/$name.qemu"
    log="$OUT/$name.log"
    : > "$qlog"
    ( export QEMU="$FAKE" FAKE_QEMU_LOG="$qlog" QEMU_TIMEOUT=20 RORTHOX_ACCEL_PROBE_TIMEOUT=2 "$@"
      failures=0
      qemu_start "$IMG" "$OUT/runs/boot-$name"
      qemu_stop || exit 1
      exit 0 ) > "$log" 2>&1
    status=$?
}
# guest_args: the arguments of the guest QEMU (the one with a disk) in $qlog.
guest_args() { grep '^start .*file=' "$qlog"; }
count() { guest_args | tr ' ' '\n' | grep -cxF -- "$1"; }
after() { guest_args | tr ' ' '\n' | grep -A1 -xF -- "$1" | sed -n 2p; }

echo "1. tcg"
for setting in "" RORTHOX_ACCEL=tcg; do
    boot "tcg${setting:+-set}" ${setting:+"$setting"}
    expect "tcg${setting:+ (set)}: one -accel tcg, -cpu qemu32, no probe, no info kvm" \
        '[ $status -eq 0 ] && [ $(count -accel) -eq 1 ] && [ "$(after -accel)" = tcg ] && [ $(count -cpu) -eq 1 ] &&
         [ "$(after -cpu)" = qemu32 ] && [ $(grep -c "^start" "$qlog") -eq 1 ] && ! grep -q "info kvm" "$qlog"'
done
boot cpu QEMU_CPU=pentium
expect "QEMU_CPU chooses the CPU model" '[ $status -eq 0 ] && [ $(count -cpu) -eq 1 ] && [ "$(after -cpu)" = pentium ]'

echo "2. QEMU_FLAGS"
n=0
for flags in "-accel kvm" "--accel tcg" "-enable-kvm" "-S -machine pc,accel=kvm" "-M accel=kvm:tcg" "-cpu host" \
             "-accel=kvm" "-gdb unix:/x -cpu=max"; do
    n=$((n + 1))
    boot "flags-$n" "QEMU_FLAGS=$flags"
    expect "refused before QEMU starts: $flags" \
        '[ $status -eq 2 ] && [ ! -s "$qlog" ] && grep -q "QEMU_FLAGS may not choose the accelerator" "$log"'
done
boot flags-ok "QEMU_FLAGS=-S -gdb unix:$OUT/gdb-accel-x,server=on,wait=off -d trace:ide_sector_read -D $OUT/t.log"
expect "ordinary flags are passed on, the accelerator once" \
    '[ $status -eq 0 ] && [ $(count -gdb) -eq 1 ] && [ $(count -accel) -eq 1 ] && [ "$(after -accel)" = tcg ]'

echo "3. kvm where KVM doesn't work"
boot kvm-denied RORTHOX_ACCEL=kvm FAKE_KVM=denied
expect "qemu_start: exit 2, says why, no guest started" \
    '[ $status -eq 2 ] && grep -q "RORTHOX_ACCEL=kvm, but KVM doesn.t work here: .*Permission denied" "$log" &&
     [ $(grep -c "^start" "$qlog") -eq 1 ] && ! guest_args > /dev/null'
boot kvm-hang RORTHOX_ACCEL=kvm FAKE_KVM=hang
expect "a hanging probe ends after its timeout: exit 2" \
    '[ $status -eq 2 ] && grep -q "KVM doesn.t work here: .*exit 124" "$log" && ! guest_args > /dev/null'
boot kvm-fallback RORTHOX_ACCEL=kvm FAKE_KVM=fallback
expect "a QEMU that starts but not with KVM: exit 2" \
    '[ $status -eq 2 ] && grep -q "KVM doesn.t work here: .*exit 0: kvm support: disabled" "$log" && ! guest_args > /dev/null'
boot kvm-in-kernel RORTHOX_ACCEL=kvm FAKE_KVM=in-kernel
expect "KVM with the in-kernel PIC and PIT: exit 2, names the devices" \
    '[ $status -eq 2 ] && ! guest_args > /dev/null &&
     grep -Fq "KVM doesn'\''t work here: a QEMU with -accel kvm,kernel-irqchip=split ran with KVM, but not with QEMU'\''s PIC and PIT (kvm-i8259 kvm-i8259 kvm-ioapic kvm-pit)" "$log"'
# annotate <mode>: accel_select with forced KVM as in a GitHub job; stdout in ann-<mode>.out, stderr in .err.
annotate() {
    ( export QEMU="$FAKE" FAKE_QEMU_LOG="$OUT/ann-$1.qemu" RORTHOX_ACCEL=kvm FAKE_KVM=$1 GITHUB_ACTIONS=true \
          RORTHOX_ACCEL_PROBE_TIMEOUT=2
      accel_select ) > "$OUT/ann-$1.out" 2> "$OUT/ann-$1.err"
    status=$?
}
prefix="::error title=KVM unavailable::"
qline="fake-qemu: failed to initialize kvm: %0A"   # the start of the stand-in's long line
annotate long
expect "a 5000-byte reason: QEMU's line cut to 1000 bytes, one annotation of at most 1500" \
    '[ $status -eq 1 ] && [ $(wc -l < "$OUT/ann-long.out") -eq 1 ] && [ "$(head -c ${#prefix} "$OUT/ann-long.out")" = "$prefix" ] &&
     [ $(wc -c < "$OUT/ann-long.out") -le $((${#prefix} + 1501)) ] && [ $(wc -l < "$OUT/ann-long.err") -eq 1 ] &&
     [ "$(grep -o "x\{100,\}" "$OUT/ann-long.err" | tr -d "\n" | wc -c)" -eq $((1000 - ${#qline})) ] &&
     grep -q "failed to initialize kvm: %250Axxxxxxxxxx" "$OUT/ann-long.out"'
annotate many
expect "400 in-kernel PITs: one annotation of at most 1500 bytes" \
    '[ $status -eq 1 ] && [ $(wc -l < "$OUT/ann-many.out") -eq 1 ] && [ "$(head -c ${#prefix} "$OUT/ann-many.out")" = "$prefix" ] &&
     [ $(wc -c < "$OUT/ann-many.out") -eq $((${#prefix} + 1501)) ] && grep -q "PIC and PIT (kvm-i8259 kvm-i8259 kvm-ioapic kvm-pit kvm-pit" "$OUT/ann-many.out"'

echo "4. kvm where it works"
boot kvm-ok RORTHOX_ACCEL=kvm FAKE_KVM=ok
expect "one -accel kvm,kernel-irqchip=split, no tcg, -cpu qemu32" \
    '[ $status -eq 0 ] && [ $(count -accel) -eq 1 ] && [ "$(after -accel)" = kvm,kernel-irqchip=split ] &&
     [ $(count tcg) -eq 0 ] && [ "$(after -cpu)" = qemu32 ]'
expect "the probe asks info kvm, then info qtree, then quits" \
    '[ "$(sed -n "1,/^start .*file=/p" "$qlog" | grep "^monitor" | tr "\n" ";")" = "monitor info kvm;monitor info qtree;monitor quit;" ]'
expect "info kvm is the guest's first monitor command, info qtree is not sent to it" \
    '[ "$(sed -n "/^start .*file=/,\$p" "$qlog" | grep -m1 "^monitor")" = "monitor info kvm" ] &&
     ! sed -n "/^start .*file=/,\$p" "$qlog" | grep -q "info qtree"'
( export QEMU="$FAKE" FAKE_QEMU_LOG="$OUT/probe.qemu" FAKE_KVM=ok
  accel_probe && printf '%s\n---\n%s\n' "$accel_why" "$accel_probe_details" ) > "$OUT/probe.txt" 2>&1
probe=$?
expect "the probe's reason names the PIC, PIT and I/O APIC; its details leave out the device tree" \
    '[ $probe -eq 0 ] && [ "$(sed -n 1p "$OUT/probe.txt")" = "a QEMU with -accel kvm,kernel-irqchip=split reported kvm support: enabled (ioapic isa-i8259 isa-i8259 isa-pit)" ] &&
     grep -Fxq "kvm support: enabled" "$OUT/probe.txt" && ! grep -q "dev: \|^bus: " "$OUT/probe.txt"'
: > "$OUT/devices.txt"
for devices in "isa-i8259 isa-i8259 ioapic" "isa-pit ioapic" "isa-i8259 isa-i8259 isa-pit kvm-pit" "kvm-i8259 isa-i8259 isa-pit" ""; do
    ( export QEMU="$FAKE" FAKE_QEMU_LOG="$OUT/devices.qemu" FAKE_KVM=ok FAKE_DEVICES="${devices:-none}"
      accel_probe
      echo "$?: $accel_why" ) >> "$OUT/devices.txt" 2>&1
done
expect "the probe refuses KVM without both isa-i8259 and isa-pit, or with kvm-i8259 or kvm-pit" \
    '[ "$(grep -c "^1: a QEMU with -accel kvm,kernel-irqchip=split ran with KVM, but not with QEMU.s PIC and PIT (" "$OUT/devices.txt")" -eq 5 ] &&
     grep -Fq "PIT (none in info qtree)" "$OUT/devices.txt" || { sed "s/^/    /" "$OUT/devices.txt"; false; }'
boot kvm-guest-off RORTHOX_ACCEL=kvm FAKE_KVM=guest-off
expect "a guest without kvm support: enabled fails in qemu_stop" \
    '[ $status -eq 1 ] && grep -q "FAIL QEMU did not run with KVM" "$log"'
for accel in tcg kvm; do
    ( export QEMU="$FAKE" FAKE_QEMU_LOG="$OUT/require-$accel.qemu" RORTHOX_ACCEL=$accel FAKE_KVM=ok
      qemu_require_tcg
      echo "went on" ) > "$OUT/require-$accel.log" 2>&1
    eval "require_$accel=\$?"
done
expect "qemu_require_tcg: skips (77) under KVM, goes on under TCG" \
    '[ $require_kvm -eq 77 ] && grep -q "only holds under TCG" "$OUT/require-kvm.log" && ! grep -q "went on" "$OUT/require-kvm.log" &&
     [ $require_tcg -eq 0 ] && grep -q "went on" "$OUT/require-tcg.log"'

# The runner, with fixture tests. run <case> [VAR=value...] -- <tests>: results in $res/<case>.
fx="$OUT/fixtures"
res="$OUT/runs"
mkdir -p "$fx" "$res"
cat > "$fx/order.sh" << 'EOF'
echo "test RORTHOX_ACCEL=$RORTHOX_ACCEL" >> "$FAKE_QEMU_LOG"
. scripts/qemu-lib.sh
qemu_start "$FIXTURE_IMG" "$RORTHOX_TEST_ROOT/vm"
qemu_stop
EOF
cat > "$fx/vm.sh" << 'EOF'
mkdir -p "$RORTHOX_TEST_ROOT/vm/a" "$RORTHOX_TEST_ROOT/vm/b" "$RORTHOX_TEST_ROOT/vm/c"
printf '(qemu) info kvm\nkvm support: enabled\n' > "$RORTHOX_TEST_ROOT/vm/a/monitor.log"
printf "$FIXTURE_MONITOR" > "$RORTHOX_TEST_ROOT/vm/b/monitor.log"
printf "$FIXTURE_MONITOR" > "$RORTHOX_TEST_ROOT/vm/c/monitor.log"
EOF
cat > "$fx/fails.sh" << 'EOF'
echo "test fails RORTHOX_ACCEL=$RORTHOX_ACCEL" >> "$FAKE_QEMU_LOG"
exit 1
EOF
run() {
    local name="$1"
    shift
    qlog="$OUT/$name.qemu"
    log="$OUT/$name.log"
    : > "$qlog"
    local vars=()
    while [ "$1" != -- ]; do vars+=("$1"); shift; done
    shift
    env -u RORTHOX_ACCEL_REASON RORTHOX_TEST_ROOT="$res/$name.root" QEMU="$FAKE" FAKE_QEMU_LOG="$qlog" \
        RORTHOX_ACCEL_PROBE_TIMEOUT=2 FIXTURE_MONITOR='kvm support: enabled\n' FIXTURE_IMG="$IMG" QEMU_TIMEOUT=20 "${vars[@]}" \
        timeout 120 "$runner" --results "$res/$name" "$@" > "$log" 2>&1
    status=$?
    json="$res/$name/results.json"
}
field() { python3 -B -c 'import json, sys; print(json.load(open(sys.argv[1])).get(sys.argv[2]))' "$json" "$1" 2> /dev/null; }

echo "3. the runner, kvm where KVM doesn't work"
run runner-denied RORTHOX_ACCEL=kvm FAKE_KVM=denied -- "$fx/order.sh"
expect "exit 2 before any test, says why" \
    '[ $status -eq 2 ] && grep -q "KVM doesn.t work here: .*Permission denied" "$log" && ! grep -q "^test" "$qlog" &&
     [ ! -e "$json" ]'

echo "4. the runner, kvm: the monitor.log of every QEMU"
run runner-kvm RORTHOX_ACCEL=kvm FAKE_KVM=ok -- "$fx/vm.sh" "$fx/order.sh"
expect "every monitor.log says kvm support: enabled: passes" \
    '[ $status -eq 0 ] && grep -Fxq "=== 2 passed, 0 failed, 0 skipped" "$log" && grep -Fxq "test RORTHOX_ACCEL=kvm" "$qlog" &&
     [ "$(grep "^start" "$qlog" | grep -vc file=)" -eq 1 ] && [ "$(grep -c "^start .*-accel kvm,kernel-irqchip=split .*file=" "$qlog")" -eq 1 ]'
run runner-guest-off RORTHOX_ACCEL=kvm FAKE_KVM=guest-off -- "$fx/order.sh"
expect "a guest that didn't run with KVM: qemu_stop fails the test" \
    '[ $status -eq 1 ] && grep -q "FAIL QEMU did not run with KVM (no .kvm support: enabled. in .*001-order/vm/monitor.log)" "$log"'
run runner-kvm-missing RORTHOX_ACCEL=kvm FAKE_KVM=ok 'FIXTURE_MONITOR=(qemu) info kvm\nkvm support: disabled\n' -- "$fx/vm.sh"
expect "two monitor.logs without it: the test fails and the log names both QEMUs" \
    '[ $status -eq 1 ] && grep -Fxq "=== 0 passed, 1 failed, 0 skipped" "$log" && grep -q "did not report kvm support: enabled" "$log" &&
     grep -q "vm/b/monitor.log" "$log" && grep -q "vm/c/monitor.log" "$log" && ! grep -q "vm/a/monitor.log" "$log"'
run runner-tcg-missing RORTHOX_ACCEL=tcg 'FIXTURE_MONITOR=nothing\n' -- "$fx/vm.sh"
expect "under tcg the monitor.log is not checked" '[ $status -eq 0 ]'

echo "5. auto"
for mode in denied hang fallback in-kernel; do
    run "auto-$mode" RORTHOX_ACCEL=auto FAKE_KVM=$mode -- "$fx/order.sh" "$fx/order.sh"
    case $mode in
        denied) why="Permission denied" ;; hang) why="exit 124" ;; fallback) why="exit 0: kvm support: disabled" ;;
        in-kernel) why="ran with KVM, but not with QEMU.s PIC and PIT (kvm-i8259 kvm-i8259 kvm-ioapic kvm-pit)" ;;
    esac
    expect "auto, probe $mode: TCG, one probe before the first test, the reason in the report" \
        '[ $status -eq 0 ] && [ "$(grep "^start" "$qlog" | grep -vc file=)" -eq 1 ] && [ "$(sed -n 1p "$qlog" | cut -c1-5)" = start ] &&
         [ "$(grep -c "^test RORTHOX_ACCEL=tcg$" "$qlog")" -eq 2 ] && [ "$(grep -c "^start .*-accel tcg .*file=" "$qlog")" -eq 2 ] &&
         [ "$(field accelerator)" = tcg ] && field accelerator_reason | grep -q "RORTHOX_ACCEL=auto: .*$why"'
done
run auto-ok RORTHOX_ACCEL=auto FAKE_KVM=ok -- "$fx/fails.sh"
expect "auto, KVM works: kvm; a test that fails under it ran once and failed" \
    '[ $status -eq 1 ] && [ "$(field accelerator)" = kvm ] && [ "$(grep -c "^test fails" "$qlog")" -eq 1 ] &&
     grep -Fxq "test fails RORTHOX_ACCEL=kvm" "$qlog" && grep -Fxq "=== 0 passed, 1 failed, 0 skipped" "$log"'
run bad-value RORTHOX_ACCEL=fast -- "$fx/order.sh"
expect "RORTHOX_ACCEL=fast: exit 2" '[ $status -eq 2 ] && grep -q "must be auto, tcg or kvm" "$log"'

echo "6. reports"
run report RORTHOX_ACCEL=tcg QEMU_CPU=qemu32 -- "$fx/order.sh"
expect "results.json: accelerator, reason, QEMU version, CPU" \
    '[ "$(field accelerator)" = tcg ] && [ "$(field accelerator_reason)" = RORTHOX_ACCEL=tcg ] &&
     [ "$(field qemu)" = "QEMU emulator version 0.0.0 (stand-in)" ] && [ "$(field cpu)" = qemu32 ]'
expect "junit.xml properties and summary.md" \
    'grep -q "<property name=\"qemu\" value=\"QEMU emulator version 0.0.0 (stand-in)\"" "$res/report/junit.xml" &&
     grep -q "<property name=\"cpu\" value=\"qemu32\"" "$res/report/junit.xml" &&
     grep -Fq "accelerator tcg)" "$res/report/summary.md" && grep -q "qemu: QEMU emulator version 0.0.0" "$res/report/summary.md"'
expect "the runner's last lines name them" \
    'grep -Fxq "=== accelerator tcg (RORTHOX_ACCEL=tcg), QEMU emulator version 0.0.0 (stand-in), CPU qemu32" "$log"'

echo "7. lint"
# lint <dir>...: files that start QEMU, choose an accelerator in QEMU_FLAGS, or use -d int/cpu_reset
# without qemu_require_tcg. This file holds the fixtures, so it is not scanned.
lint() {
    local f
    for f in $(find "$@" -maxdepth 1 -type f \( -name '*.sh' -o -name '*.py' \) | LC_ALL=C sort); do
        case "${f##*/}" in accelerator.sh) continue ;; esac
        code=$(grep -v '^[[:space:]]*#' "$f")
        case "${f##*/}" in
            qemu-lib.sh | qemu-accel.sh | run-tests.sh | ci-artifact.py) ;;   # QEMU and its version
            *) if grep -qE 'qemu-system-|"\$QEMU"|\$\{QEMU[:}]|\$QEMU([^_A-Za-z0-9]|$)' <<< "$code"; then
                   echo "starts QEMU itself: $f"; fi ;;
        esac
        if grep -qE 'QEMU_FLAGS=.*([[:space:]"]--?(accel|enable-kvm|cpu)([[:space:]="]|$)|accel=)' <<< "$code"; then
            echo "chooses the accelerator or CPU in QEMU_FLAGS: $f"
        fi
        if grep -qE -- '-d[[:space:]]+([^[:space:]"]*,)?(int|cpu_reset)([,[:space:]"]|$)|cpu_reset' <<< "$code" &&
           ! grep -q 'qemu_require_tcg' <<< "$code"; then
            echo "uses -d int or cpu_reset without qemu_require_tcg: $f"
        fi
    done
}
lint scripts scripts/tests scripts/tests/lib > "$OUT/lint.txt"
expect "the tests and helpers pass the lint" '[ ! -s "$OUT/lint.txt" ] || { sed "s/^/    /" "$OUT/lint.txt"; false; }'
lf="$OUT/lint-fixtures"
mkdir -p "$lf"
d_int="-d in""t"
printf 'QEMU_FLAGS="%s,cpu_rese%s -D x" qemu_start a b\n' "$d_int" t > "$lf/int.sh"
printf 'qemu_require_tcg\nQEMU_FLAGS="%s -D x" qemu_start a b\n' "$d_int" > "$lf/int-tcg.sh"
printf 'QEMU_FLAGS="-d trace:ide_sector_read -D x" qemu_start a b\n' > "$lf/trace.sh"
printf '"$QEMU" -m 128 -drive file=x\n' > "$lf/direct.sh"
printf 'QEMU_FLAGS="-S -enable-kvm" qemu_start a b\n' > "$lf/accel.sh"
lint "$lf" > "$OUT/lint-fixtures.txt"
expect "the lint finds each kind (and only those)" \
    '[ "$(cat "$OUT/lint-fixtures.txt")" = "chooses the accelerator or CPU in QEMU_FLAGS: $lf/accel.sh
starts QEMU itself: $lf/direct.sh
uses -d int or cpu_reset without qemu_require_tcg: $lf/int.sh" ] || { sed "s/^/    /" "$OUT/lint-fixtures.txt"; false; }'

if [ "$failures" -ne 0 ]; then
    echo "accelerator: $failures check(s) failed" >&2
    exit 1
fi
echo "accelerator: all checks passed"
