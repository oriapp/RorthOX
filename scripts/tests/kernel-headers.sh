#!/usr/bin/env bash
#
# Kernel headers (K31): kernel/include/kernel/time/pit.h defines its include guard and declares
# pit_sleep with the unit its implementation uses (seconds, idt.c); kernel/include/kernel/tty.h
# no longer switches off -Wunused-variable for every file that includes it (it had an "ignored"
# pragma with an unmatched "pop", so the warning stayed off for the rest of each file).
#
# Compile checks only (i686-elf-gcc with the kernel's FLAGS from the Makefile, -Werror), no QEMU:
#   guard       pit.h included twice defines _KERNEL_TIME_PIT
#   pit-tty     pit.h then tty.h, and tty.h then pit.h, compile without warnings: the five
#   tty-pit     terminal state stays in tty.c, not in files including tty.h
#   warning     a file that includes tty.h and then has an unused static and an unused local still
#               gets both unused-variable warnings
#   state       the five terminal variables keep their names and types, and stay file-local in
#               tty.c; an object that includes only tty.h contains no terminal state
#   pragma      tty.h has no "#pragma GCC diagnostic" left
#   unit        pit.h declares pit_sleep(int sec) and says the unit is seconds
#
# Needs i686-elf-gcc and i686-elf-nm (the cross toolchain).

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh   # only for check()

OUT=build/tests/kernel-headers
rm -rf "$OUT"
mkdir -p "$OUT"
[ -f build/config/rorthox-config.h ] || { echo "kernel-headers: run 'make all' first" >&2; exit 1; }

# The kernel's compiler flags, from the Makefile.
printf 'print-kernel-flags:\n\t@echo $(FLAGS)\n' > "$OUT/flags.mk"
FLAGS=$(make -s --no-print-directory -f Makefile -f "$OUT/flags.mk" print-kernel-flags)
echo "kernel-headers: FLAGS = $FLAGS"
case " $FLAGS " in *" -Werror "*" -Wall "* | *" -Wall "*" -Werror "*) ;; *) echo "kernel-headers: FLAGS lack -Wall -Werror" >&2; exit 1 ;; esac

: > "$OUT/results.txt"
# compile <name> <expect ok|error>: build $OUT/<name>.c with the kernel flags.
compile() {
    # shellcheck disable=SC2086
    if i686-elf-gcc $FLAGS -std=gnu99 -I kernel -I build/config -c "$OUT/$1.c" -o "$OUT/$1.o" > "$OUT/$1.log" 2>&1; then
        echo ok
    else
        echo error
    fi
}

cat > "$OUT/guard.c" << 'EOF'
#include "include/kernel/time/pit.h"
#include "include/kernel/time/pit.h"
#ifndef _KERNEL_TIME_PIT
#error "pit.h does not define _KERNEL_TIME_PIT"
#endif
int guard_fixture;
EOF
r=$(compile guard)
echo "  guard: $r"
[ "$r" = ok ] && echo "PASS guard" >> "$OUT/results.txt"

cat > "$OUT/pit-tty.c" << 'EOF'
#include "include/kernel/time/pit.h"
#include "include/kernel/tty.h"
int pit_tty_fixture;
EOF
cat > "$OUT/tty-pit.c" << 'EOF'
#include "include/kernel/tty.h"
#include "include/kernel/time/pit.h"
int tty_pit_fixture;
EOF
for f in pit-tty tty-pit; do
    r=$(compile $f)
    echo "  $f: $r"
    sed 's/^/    /' "$OUT/$f.log"
    [ "$r" = ok ] && [ ! -s "$OUT/$f.log" ] && echo "PASS $f" >> "$OUT/results.txt"
done

cat > "$OUT/warning.c" << 'EOF'
#include "include/kernel/tty.h"
static int unrelated_unused_static;
void warning_fixture(void)
{
    int unrelated_unused_local;
}
EOF
r=$(compile warning)
echo "  warning: $r (must be an error: two unused variables)"
sed 's/^/    /' "$OUT/warning.log"
if [ "$r" = error ] && grep -q "unrelated_unused_static.*unused-variable" "$OUT/warning.log" &&
    grep -q "unrelated_unused_local.*unused-variable" "$OUT/warning.log"; then
    echo "PASS warning" >> "$OUT/results.txt"
fi

cat > "$OUT/state.c" << 'EOF'
#include "arch/i686/tty.c"
#define SAME_TYPE(x, t) _Static_assert(__builtin_types_compatible_p(__typeof__(x), t), #x " has the wrong type")
SAME_TYPE(video_mem, volatile uint16_t *);
SAME_TYPE(terminal_row, uint16_t);
SAME_TYPE(terminal_col, uint16_t);
SAME_TYPE(terminal_color, uint8_t);
SAME_TYPE(termianl_background_color, uint8_t);
uint16_t *state_fixture(void)
{
    terminal_row = 1;
    terminal_col = 2;
    terminal_color = 3;
    termianl_background_color = 4;
    return (uint16_t *)video_mem;
}
EOF
r=$(compile state)
echo "  state: $r"
sed 's/^/    /' "$OUT/state.log"
if [ "$r" = ok ]; then
    i686-elf-nm "$OUT/state.o" > "$OUT/state-nm.txt"
    sed 's/^/    /' "$OUT/state-nm.txt"
    names='(video_mem|terminal_row|terminal_col|terminal_color|termianl_background_color)$'
    i686-elf-nm "$OUT/tty-pit.o" > "$OUT/header-nm.txt"
    if [ "$(grep -cE " [bd] $names" "$OUT/state-nm.txt")" = 5 ] &&
        ! grep -qE " [A-Z] $names" "$OUT/state-nm.txt" &&
        ! grep -qE " $names" "$OUT/header-nm.txt"; then
        echo "PASS state" >> "$OUT/results.txt"
    fi
fi

grep -n "pragma" kernel/include/kernel/tty.h | sed 's/^/  tty.h: /' || true
grep -q "#pragma GCC diagnostic" kernel/include/kernel/tty.h || echo "PASS pragma" >> "$OUT/results.txt"

grep -n "pit_sleep" kernel/include/kernel/time/pit.h | sed 's/^/  pit.h: /' || true
if grep -qE '^void pit_sleep\(int sec\);' kernel/include/kernel/time/pit.h &&
    grep -qiE '//.*second|/\*.*second|^ \*.*second' kernel/include/kernel/time/pit.h &&
    ! grep -qE 'pit_sleep\(int ms\)' kernel/include/kernel/time/pit.h; then
    echo "PASS unit" >> "$OUT/results.txt"
fi

R="$OUT/results.txt"
check "pit.h defines its include guard"                          "$R" "PASS guard" line
check "pit.h then tty.h: no warning"                             "$R" "PASS pit-tty" line
check "tty.h then pit.h: no warning"                             "$R" "PASS tty-pit" line
check "unused-variable warnings still on after tty.h"            "$R" "PASS warning" line
check "terminal state local to tty.c, absent from header objects"     "$R" "PASS state" line
check "no #pragma GCC diagnostic in tty.h"                       "$R" "PASS pragma" line
check "pit_sleep(int sec) declared, unit seconds documented"    "$R" "PASS unit" line
if [ "$failures" -ne 0 ]; then
    echo "kernel-headers: $failures check(s) failed" >&2
    exit 1
fi
echo "kernel-headers: all checks passed"
