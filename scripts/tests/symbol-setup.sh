#!/usr/bin/env bash
#
# The debugger-side tests link an ELF copy of the kernel at run time to find its symbols
# (scripts/tests/lib/kernel-symbols.sh). Without the cross toolchain that setup must fail loudly
# and the test must stop before it starts QEMU: no debugger run with blank addresses, no
# misleading checks after it.
# 1. With every i686-elf-* tool hidden from PATH: kernel_symbols_or_stop, guest_state_setup and
#    heap_snapshot_setup each return 1, name the missing tool and write symbols.txt "missing".
#    Three small callers written the way the tests call them (guest_state_setup at the top of a
#    "set -e" script, heap_snapshot_setup with "|| exit 1", run_syscheck from lib/syscheck.sh) end
#    with a non-zero status, say why, and never start QEMU (QEMU is a stand-in that only records
#    that it ran).
# 2. With an i686-elf-ld that fails: the same, the error names the failed link.
# 3. With the real toolchain: the same setup finds every symbol and offset, and the callers get as
#    far as starting QEMU (so the checks above are not met by a setup that always fails).
# No QEMU runs in this test.

set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
. ./scripts/qemu-lib.sh
OUT=${RORTHOX_TEST_ROOT:-build/tests}/symbol-setup
rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
[ -f build/kernelfull.o ] && [ -f bin/kernel.bin ] && [ -f bin/os.bin ] ||
    { echo "symbol-setup: run 'make all' first" >&2; exit 1; }
command -v i686-elf-ld > /dev/null || { echo "symbol-setup: needs the cross toolchain in PATH" >&2; exit 1; }

# no_cross_path: $PATH with every directory that holds an i686-elf-* tool replaced by a copy made of
# links to everything else in it.
no_cross_path() {
    local d f n=0 out="" dirs
    IFS=: read -r -a dirs <<< "$PATH"
    for d in "${dirs[@]}"; do
        if [ -n "$d" ] && compgen -G "$d/i686-elf-*" > /dev/null; then
            n=$((n + 1))
            mkdir -p "$OUT/shadow-$n"
            for f in "$d"/*; do
                case "${f##*/}" in i686-elf-*) ;; *) ln -s "$f" "$OUT/shadow-$n/" 2> /dev/null ;; esac
            done
            d="$OUT/shadow-$n"
        fi
        out+="${out:+:}$d"
    done
    echo "$out"
}

# The QEMU stand-in records that it ran; a linker that runs and fails.
mkdir -p "$OUT/bin"
printf '#!/bin/sh\necho "$@" >> "%s/qemu-ran"\nexit 1\n' "$OUT" > "$OUT/qemu-stand-in"
printf '#!/bin/sh\necho "i686-elf-ld stand-in: failing" >&2\nexit 1\n' > "$OUT/bin/i686-elf-ld"
chmod +x "$OUT/qemu-stand-in" "$OUT/bin/i686-elf-ld"

NO_CROSS=$(no_cross_path)
if PATH="$NO_CROSS" command -v i686-elf-ld > /dev/null || ! PATH="$NO_CROSS" command -v awk > /dev/null; then
    echo "symbol-setup: could not hide the cross toolchain from PATH" >&2
    exit 1
fi

# The callers, as the tests are written: setup first, then QEMU.
cat > "$OUT/guest-state-caller.sh" << 'EOF'
set -euo pipefail
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh
OUT="$CALLER_OUT"
guest_state_setup "$OUT"
qemu_start bin/os.bin "$OUT/run"
qemu_stop || true
EOF
cat > "$OUT/heap-snapshot-caller.sh" << 'EOF'
set -euo pipefail
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/heap-snapshot.sh
OUT="$CALLER_OUT"
heap_snapshot_setup "$OUT" || { echo "caller: setup failed" >&2; exit 1; }
QEMU_FLAGS="$HEAP_QEMU_FLAGS" qemu_start bin/os.bin "$OUT/run"
qemu_stop || true
EOF
cat > "$OUT/syscheck-caller.sh" << 'EOF'
set -euo pipefail
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/syscheck.sh
OUT="$CALLER_OUT"
IMAGE=bin/os.bin
run_syscheck ptr 'syscheck: PASS'
EOF

# setup <case> <path> <function>: one setup function in a fresh shell; its status and output go to
# $OUT/<case>/output.log, its symbols.txt next to it.
setup() {
    local dir="$OUT/$1"
    mkdir -p "$dir"
    PATH="$2" bash -c '
        set -euo pipefail
        . ./scripts/qemu-lib.sh
        . ./scripts/tests/lib/guest-state.sh
        . ./scripts/tests/lib/heap-snapshot.sh
        if "$1" "$2" symbol-setup; then echo "status=0"; else echo "status=$?"; fi
        [ "$1" != kernel_symbols_or_stop ] || [ -z "$SYM_ISR80H_HANDLER$SYM_CURRENT_TASK$TASK_NEXT_OFFSET" ] ||
            echo "blank=no"
    ' setup "$3" "$dir" > "$dir/output.log" 2>&1
}

# caller <case> <path> <caller>: one caller with the QEMU stand-in; $OUT/<case>/output.log, status.txt.
caller() {
    local dir="$OUT/$1"
    mkdir -p "$dir"
    rm -f "$OUT/qemu-ran"
    if PATH="$2" QEMU="$OUT/qemu-stand-in" CALLER_OUT="$dir" TMPDIR="$dir" QEMU_TIMEOUT=10 \
        timeout 60 bash "$OUT/$3-caller.sh" > "$dir/output.log" 2>&1; then
        echo "status=0" > "$dir/status.txt"
    else
        echo "status=$?" > "$dir/status.txt"
    fi
}

failures=0
ok() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; failures=$((failures + 1)); }
expect() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }

# stopped <case> <label> <error>: the checks for a caller that must stop before QEMU.
stopped() {
    local d="$OUT/$1" err="$3"
    expect "$2: fails" '[ -f "$d/status.txt" ] && ! grep -qx "status=0" "$d/status.txt"'
    expect "$2: says the setup failed and why" \
        'grep -qF "$err" "$d/output.log" &&
         grep -qF "setup failed: no kernel symbols for the debugger, QEMU not started" "$d/output.log"'
    expect "$2: QEMU never started" '[ ! -e "$OUT/qemu-ran" ]'
    expect "$2: no debugger run with blank addresses" '! grep -qE "Traceback|ValueError" "$d/output.log"'
    expect "$2: symbols.txt says missing" 'grep -qx missing "$d/symbols.txt"'
}

echo "symbol-setup: 1. no cross toolchain in PATH"
for f in kernel_symbols_or_stop guest_state_setup heap_snapshot_setup; do
    setup "missing-$f" "$NO_CROSS" "$f"
    d="$OUT/missing-$f"
    expect "$f: returns 1" 'grep -qx "status=1" "$d/output.log"'
    expect "$f: says i686-elf-ld is not in PATH" 'grep -qF "ERROR: i686-elf-ld not found in PATH" "$d/output.log"'
    expect "$f: symbols.txt says missing" 'grep -qx missing "$d/symbols.txt"'
done
for c in guest-state heap-snapshot syscheck; do
    caller "missing-$c-caller" "$NO_CROSS" "$c"
    stopped "missing-$c-caller" "$c caller" "ERROR: i686-elf-ld not found in PATH"
done

echo "symbol-setup: 2. an i686-elf-ld that fails"
setup failing-link "$OUT/bin:$PATH" guest_state_setup
d="$OUT/failing-link"
expect "guest_state_setup: returns 1" 'grep -qx "status=1" "$d/output.log"'
expect "guest_state_setup: names the failed link" \
    'grep -qF "ERROR: i686-elf-ld could not link build/kernelfull.o" "$d/output.log"'
expect "guest_state_setup: symbols.txt says missing" 'grep -qx missing "$d/symbols.txt"'
caller failing-link-caller "$OUT/bin:$PATH" guest-state
stopped failing-link-caller "guest-state caller" "ERROR: i686-elf-ld could not link build/kernelfull.o"

echo "symbol-setup: 3. the real toolchain"
for f in kernel_symbols_or_stop guest_state_setup heap_snapshot_setup; do
    setup "found-$f" "$PATH" "$f"
    d="$OUT/found-$f"
    expect "$f: returns 0, symbols.txt says found" 'grep -qx "status=0" "$d/output.log" && grep -qx found "$d/symbols.txt"'
done
expect "kernel_symbols_or_stop: sets the symbols" 'grep -qx "blank=no" "$OUT/found-kernel_symbols_or_stop/output.log"'
expect "no error from the setup" '! grep -q ERROR "$OUT"/found-*/output.log'
for c in guest-state heap-snapshot syscheck; do
    caller "found-$c-caller" "$PATH" "$c"
    expect "$c caller: gets as far as starting QEMU" '[ -e "$OUT/qemu-ran" ] && grep -qx found "$OUT/found-$c-caller/symbols.txt"'
done

if [ "$failures" -ne 0 ]; then
    echo "symbol-setup: $failures check(s) failed" >&2
    exit 1
fi
echo "symbol-setup: all checks passed"
