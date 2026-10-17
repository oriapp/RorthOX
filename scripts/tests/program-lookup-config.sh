#!/usr/bin/env bash
#
# PROGRAM_SEARCH_PATHS in config.mk: the ordered list of directories syscalls 6 and 7 look bare
# program names up in. Build and host checks only, no QEMU.
#
# A copy of Makefile and config.mk in build/tests gets each value below.
#   accepted  make (with /bin/sh and with SHELL=/bin/bash) parses it, and the rorthox-config.h it
#             writes defines RORTHOX_PROGRAM_SEARCH_PATHS as the directories as C strings,
#             in the order written. The real kernel/isr80h/process.c, built against that header,
#             tries them in that order (scripts/tests/host/program-lookup.c, AddressSanitizer).
#   rejected  make stops with the PROGRAM_SEARCH_PATHS error (not a later build error): no entry,
#             five entries, a missing drive, a drive other than 0 (only disk 0 exists), a missing
#             slash or trailing slash, a two-digit drive, empty,
#             "." and ".." parts, and quotes, apostrophes, backslashes, commas, ';' and '$', and a
#             value that runs a command if it reaches the shell without quoting.
#             Setting it on the make command line is refused as well.
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check()

OUT=build/tests/program-lookup-config
rm -rf "$OUT"
mkdir -p "$OUT"
FIX="$OUT/config"
RESULTS="$OUT/results.txt"
: > "$RESULTS"

# set_value <value>: the fixture's config.mk with PROGRAM_SEARCH_PATHS = <value>.
set_value() {
    rm -rf "$FIX"
    mkdir -p "$FIX"
    cp Makefile "$FIX/Makefile"
    VALUE="$1" awk '/^PROGRAM_SEARCH_PATHS = / { print "PROGRAM_SEARCH_PATHS = " ENVIRON["VALUE"]; next } { print }' \
        config.mk > "$FIX/config.mk"
    grep -qxF "PROGRAM_SEARCH_PATHS = $1" "$FIX/config.mk"
}

# accept <tag> <value> <directory>...: parsed by both shells, emitted in order, looked up in order.
accept() {
    local tag="$1" value="$2" sh want d
    shift 2
    set_value "$value" || { echo "FAIL $tag: could not set the value" >> "$RESULTS"; return; }
    for sh in /bin/sh /bin/bash; do
        if make -s -C "$FIX" -n SHELL="$sh" build/config/rorthox-config.h > "$FIX/$tag-$(basename "$sh").log" 2>&1; then
            echo "PASS $tag parsed $sh" >> "$RESULTS"
        else
            echo "  $tag: make $sh refused it: $(grep -m1 -F '***' "$FIX/$tag-$(basename "$sh").log" || true)"
        fi
    done
    want="#define RORTHOX_PROGRAM_SEARCH_PATHS"
    for d in "$@"; do want="$want \"$d\","; done
    if make -s -C "$FIX" build/config/rorthox-config.h > "$FIX/$tag-emit.log" 2>&1 &&
            grep -qxF -- "$want" "$FIX/build/config/rorthox-config.h"; then
        echo "PASS $tag emitted" >> "$RESULTS"
    else
        echo "  $tag: expected '$want', got '$(grep -F RORTHOX_PROGRAM_SEARCH_PATHS "$FIX/build/config/rorthox-config.h" 2> /dev/null || true)'"
    fi
    if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w -fsanitize=address -fno-omit-frame-pointer \
            -I kernel -I "$FIX/build/config" -I scripts/tests/host \
            scripts/tests/host/program-lookup.c scripts/tests/host/kalloc.c kernel/isr80h/process.c \
            kernel/string/string.c kernel/memory/memory.c -o "$OUT/host-$tag" > "$OUT/host-$tag-build.log" 2>&1 &&
            ASAN_OPTIONS=detect_leaks=0 "$OUT/host-$tag" "$@" > "$OUT/host-$tag.log" 2>&1 &&
            grep -qxF "program-lookup host check: 0 of 6 cases failed" "$OUT/host-$tag.log"; then
        echo "PASS $tag lookup" >> "$RESULTS"
    else
        echo "  $tag: lookup host check failed, see $OUT/host-$tag.log"
        grep -m5 FAIL "$OUT/host-$tag.log" 2> /dev/null | sed 's/^/  /' || true
    fi
    echo "  accept $tag: $value"
}

# reject <tag> <value>: both shells stop at the PROGRAM_SEARCH_PATHS check.
reject() {
    local tag="$1" value="$2" sh log
    set_value "$value" || { echo "FAIL $tag: could not set the value" >> "$RESULTS"; return; }
    for sh in /bin/sh /bin/bash; do
        log="$FIX/$tag-$(basename "$sh").log"
        if make -s -C "$FIX" -n SHELL="$sh" build/config/rorthox-config.h > "$log" 2>&1; then
            echo "  $tag: make $sh accepted it"
        elif grep -qF '*** PROGRAM_SEARCH_PATHS must contain' "$log"; then
            echo "PASS $tag rejected $sh" >> "$RESULTS"
        else
            echo "  $tag: make $sh failed, but not at the check: $(grep -m1 -F '***' "$log" || head -n1 "$log")"
        fi
    done
    echo "  reject $tag: $value"
}

echo "program-lookup-config: accepted values"
accept default  "0:/ 0:/bin/"                       0:/ 0:/bin/
accept one      "0:/bin/"                           0:/bin/
accept three-a  "0:/ 0:/bin/ 0:/tools/"             0:/ 0:/bin/ 0:/tools/
accept three-b  "0:/ 0:/tools/ 0:/bin/"             0:/ 0:/tools/ 0:/bin/
accept four     "0:/x/y/ 0:/z/ 0:/A-b_c.9/ 0:/"     0:/x/y/ 0:/z/ 0:/A-b_c.9/ 0:/
accept spacing  "  0:/bin/    0:/   # bin first"    0:/bin/ 0:/

echo "program-lookup-config: rejected values"
reject none        ""
reject comment     "# nothing"
reject five        "0:/ 0:/a/ 0:/b/ 0:/c/ 0:/d/"
reject no-slash    "0:/bin"
reject no-drive    "/bin/"
reject no-root     "0:bin/"
reject drive-1     "1:/"
reject drive-9     "9:/bin/"
reject drive-1-in  "0:/x/y/ 1:/ 0:/A-b_c.9/ 0:/"
reject drive-10    "10:/"
reject drive-a     "a:/"
reject double      "0://"
reject double-in   "0:/bin//"
reject dot         "0:/./"
reject dotdot      "0:/bin/../"
reject quote       '0:/a"b/'
reject apostrophe  "0:/it's/"
reject backslash   '0:/a\b/'
reject comma       "0:/,0:/bin/"
reject semicolon   "0:/;/"
reject dollar      '0:/$$x/'
# Breaks out of the quotes if the value reaches the shell unquoted (the check would then pass).
reject inject      "0:/';echo\$\${IFS}valid;exit;'/"

echo "program-lookup-config: command line"
set_value "0:/ 0:/bin/"
if make -s -C "$FIX" -n PROGRAM_SEARCH_PATHS=0:/ build/config/rorthox-config.h > "$FIX/override.log" 2>&1; then
    echo "  the command line value was accepted"
elif grep -qF 'PROGRAM_SEARCH_PATHS must be set in config.mk, not on the command line' "$FIX/override.log"; then
    echo "PASS override rejected" >> "$RESULTS"
fi

for t in default one three-a three-b four spacing; do
    check "$t: parsed with /bin/sh"              "$RESULTS" "PASS $t parsed /bin/sh" line
    check "$t: parsed with /bin/bash"            "$RESULTS" "PASS $t parsed /bin/bash" line
    check "$t: emitted in order"                 "$RESULTS" "PASS $t emitted" line
    check "$t: kernel looks up in that order"    "$RESULTS" "PASS $t lookup" line
done
for t in none comment five no-slash no-drive no-root drive-1 drive-9 drive-1-in drive-10 drive-a double double-in dot \
         dotdot quote apostrophe backslash comma semicolon dollar inject; do
    check "$t: rejected with /bin/sh"            "$RESULTS" "PASS $t rejected /bin/sh" line
    check "$t: rejected with /bin/bash"          "$RESULTS" "PASS $t rejected /bin/bash" line
done
check "command-line value rejected"              "$RESULTS" "PASS override rejected" line
if [ "$failures" -ne 0 ]; then
    echo "program-lookup-config: $failures check(s) failed" >&2
    exit 1
fi
echo "program-lookup-config: all checks passed"
