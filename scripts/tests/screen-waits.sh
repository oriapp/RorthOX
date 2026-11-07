#!/usr/bin/env bash
#
# The screen waits and the cleanup hook of scripts/qemu-lib.sh, checked on the host without QEMU.
#
# Screens: the snapshots come from a script, one per try (the last one stays on the screen). Each
# case has the screens a test can see before its step is done (the typed command's echo, the result
# of the step before, a row that is half printed, a scroll half done: terminal_scroll copies the rows
# one by one, so for a moment one row is on the screen twice) and then the step's own result. The
# old waits (a substring, a count of rows, two waits one after the other) accept one of the early
# screens. wait_screen_lines must accept the result screen and none before it, and must give up with
# status 1 when the result never comes.
#
# Cleanup: qemu_start starts a stand-in for QEMU (sleep). The script then ends normally, fails, or
# gets TERM right after qemu_start. The stand-in and the monitor directory must be gone, Q_CLEANUP
# must have run after QEMU was stopped, and the exit status must be the script's own.

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh

OUT="$(realpath -m "${RORTHOX_TEST_ROOT:-build/tests}/screen-waits")"
rm -rf "$OUT"
mkdir -p "$OUT/run"
Q_OUT="$OUT/run"
Q_POLL=0.05

# Scripted screens: snapshot number n shows $FRAMES/n; after the last one the screen stays as it is.
qemu_running() { return 0; }
screen_snapshot() {
    local n=$((shown + 1))
    [ -f "$FRAMES/$n" ] || n=$shown
    cp "$FRAMES/$n" "$Q_OUT/screen.txt"
    shown=$n
}

# screen <case> <row>...: add the next screen of <case> (25 rows, missing rows empty).
screen() {
    local d="$OUT/screens/$1" n i
    shift
    mkdir -p "$d"
    n=$(($(ls "$d" | wc -l) + 1))
    { printf '%s\n' "$@"; for ((i = $#; i < 25; i++)); do echo; done; } > "$d/$n"
}

# mid_scroll <case> <k>: the last screen of <case> while it scrolls up, after rows 2..k+1 were
# copied to rows 1..k: row k is now on the screen twice.
mid_scroll() {
    local d="$OUT/screens/$1" n
    n=$(ls "$d" | wc -l)
    awk -v k="$2" '{ row[NR] = $0 } END { for (i = 1; i <= 25; i++) print (i <= k ? row[i + 1] : row[i]) }' \
        "$d/$n" > "$d/$((n + 1))"
}

# play <case> <name> <wait command...>: run a wait on the screens of <case>. $OUT/<name>.txt gets
# "status=<exit status> screen=<the screen it stopped on>".
play() {
    local name="$2" status
    FRAMES="$OUT/screens/$1"
    shown=0
    shift 2
    if "$@" > "$OUT/$name.log" 2>&1; then status=0; else status=$?; fi
    echo "status=$status screen=$shown" > "$OUT/$name.txt"
}

# The old ways to wait: n rows matching a regex (as exception-frames.sh and exceptions.sh did), and
# two waits one after the other.
old_count() {
    local deadline=$((SECONDS + $3)) n
    while ((SECONDS < deadline)); do
        screen_snapshot
        n=$(grep -cE -- "$1" "$Q_OUT/screen.txt" || true)
        [ "$n" -ge "$2" ] && return 0
        sleep "$Q_POLL"
    done
    return 1
}
old_both() {
    wait_screen "$1" "$3" && wait_screen "$2" "$3"
}

BANNER="${RORTHOX_NAME} v${RORTHOX_VERSION}"

# scroll: a full screen, the second program is typed on the last row and Enter scrolls the screen.
rows=("$BANNER")
for ((i = 0; i < 20; i++)); do rows+=(">"); done
rows+=("> syscheck first" "first: done" "Process was terminated" "> syscheck second")
screen scroll "${rows[@]}"                                # 1: the command is typed
mid_scroll scroll 23                                      # 2: "Process was terminated" twice
rows=("${rows[@]:1}" "")
screen scroll "${rows[@]}"                                # 3: the scroll is done
rows[24]="second: do"
screen scroll "${rows[@]}"                                # 4: the result is half printed
rows[24]="second: done"
rows=("${rows[@]:2}" "Process was terminated" ">")
screen scroll "${rows[@]}"                                # 5: done, the shell is back

# echo: blank prints its arguments; the command's echo has the same text.
screen echo "$BANNER" "> blank sum-12"                    # 1
screen echo "$BANNER" "> blank sum-1234abcd"              # 2: the echo
screen echo "$BANNER" "> blank sum-1234abcd" "Blank loaded" "blank"
screen echo "$BANNER" "> blank sum-1234abcd" "Blank loaded" "blank" "sum-1234abcd" ">"   # 4

# stale: the result of case 1 is still on the screen when case 2 starts.
screen stale "$BANNER" "> heaptest case-1" "result case-1 ok" "> heaptest case-2"
screen stale "$BANNER" "> heaptest case-1" "result case-1 ok" "> heaptest case-2" "result case-2 o"
screen stale "$BANNER" "> heaptest case-1" "result case-1 ok" "> heaptest case-2" "result case-2 ok" ">"

# both: two rows that must be on one screen; each is on some screen before that, never both.
screen both "$BANNER" "a-ok"
screen both "$BANNER" "b-ok"
screen both "$BANNER" "a-ok" "b-ok"

# prompt: the shell's prompt was printed before the program's line, on the same row.
screen prompt "$BANNER" "> syscheck armed"
screen prompt "$BANNER" "> syscheck armed" "> armed"

# never: the result never comes.
screen never "$BANNER" "> blank never"

echo "screen-waits: scripted screens"
play scroll scroll-new        wait_screen_lines 5 "line:second: done"
play scroll scroll-old-count  old_count '^Process was terminated$' 2 5
play scroll scroll-old-text   wait_screen "second" 5
play echo   echo-new-line     wait_screen_lines 5 "line:sum-1234abcd"
play echo   echo-new-regex    wait_screen_lines 5 "regex:sum-[0-9a-f]{8}"
play echo   echo-old-text     wait_screen "sum-1234abcd" 5
play stale  stale-new         wait_screen_lines 5 "line:result case-2 ok"
play stale  stale-old-count   old_count '^result case-[0-9]+ ok$' 1 5
play stale  stale-old-text    wait_screen "result case-2" 5
play stale  stale-reused-tag  wait_screen_lines 5 "line:result case-1 ok"
play both   both-new          wait_screen_lines 5 "line:a-ok" "line:b-ok"
play both   both-old          old_both "a-ok" "b-ok" 5
play prompt prompt-new        wait_screen_lines 5 "prompt:armed"
play prompt prompt-line       wait_screen_lines 2 "line:armed"
start=$SECONDS
play never  never-new         wait_screen_lines 2 "line:never" "line:Blank loaded"
elapsed=$((SECONDS - start))
if ((elapsed >= 1 && elapsed <= 4)); then echo "about 2 s" > "$OUT/never-time.txt"; else echo "$elapsed s" > "$OUT/never-time.txt"; fi
start=$SECONDS
play scroll unknown           wait_screen_lines 5 "word:never"
play scroll empty-line        wait_screen_lines 5 "line:"
play scroll empty-prompt      wait_screen_lines 5 "line:second: done" "prompt:"
elapsed=$((SECONDS - start))
# Five frames each, half a second apart: polling on would take 2 s or more.
if ((elapsed <= 1)); then echo "at once" > "$OUT/unknown-time.txt"; else echo "$elapsed s" > "$OUT/unknown-time.txt"; fi
for f in "$OUT"/*.log; do sed "s|^|  $(basename "$f" .log): |" "$f"; done

check "scroll: old count accepts the half-done scroll"    "$OUT/scroll-old-count.txt"  "status=0 screen=2" line
check "scroll: old substring accepts the typed command"   "$OUT/scroll-old-text.txt"   "status=0 screen=1" line
check "scroll: exact row waits for the result"            "$OUT/scroll-new.txt"        "status=0 screen=5" line
check "echo: old substring accepts the command's echo"    "$OUT/echo-old-text.txt"     "status=0 screen=2" line
check "echo: exact row waits for the program's line"      "$OUT/echo-new-line.txt"     "status=0 screen=4" line
check "echo: whole-row regex waits for the program's line" "$OUT/echo-new-regex.txt"   "status=0 screen=4" line
check "stale: old count accepts the last case's result"   "$OUT/stale-old-count.txt"   "status=0 screen=1" line
check "stale: old substring accepts a half printed row"   "$OUT/stale-old-text.txt"    "status=0 screen=2" line
check "stale: a reused tag is accepted at once"           "$OUT/stale-reused-tag.txt"  "status=0 screen=1" line
check "stale: exact row with a new tag waits"             "$OUT/stale-new.txt"         "status=0 screen=3" line
check "both: two waits in a row accept separate screens"  "$OUT/both-old.txt"          "status=0 screen=2" line
check "both: both rows on one screen"                     "$OUT/both-new.txt"          "status=0 screen=3" line
check "prompt: accepts \"> \" in front, not the echo"      "$OUT/prompt-new.txt"        "status=0 screen=2" line
check "prompt: line: does not accept \"> \" in front"      "$OUT/prompt-line.txt"       "status=1 screen=2" line
check "never: gives up with status 1"                     "$OUT/never-new.txt"         "status=1 screen=1" line
check "never: says what it waited for"                    "$OUT/never-new.log" "  gave up after 2 s waiting for: line:never line:Blank loaded" line
check "never: waited about 2 s"                           "$OUT/never-time.txt"        "about 2 s" line
check "unknown condition: fails on the first screen"        "$OUT/unknown.txt"           "status=1 screen=1" line
check "unknown condition: names it"                       "$OUT/unknown.log" "  unknown screen condition \"word:never\"" line
check "empty line: text: fails on the first screen"       "$OUT/empty-line.txt"        "status=1 screen=1" line
check "empty prompt: text: fails, even with a good row"  "$OUT/empty-prompt.txt"      "status=1 screen=1" line
check "empty text: names it"                              "$OUT/empty-line.log" "  empty screen condition \"line:\"" line
check "unknown and empty conditions fail at once"         "$OUT/unknown-time.txt"      "at once" line

# Cleanup hook. The child script sources qemu-lib.sh with a stand-in QEMU that records its pid.
echo "screen-waits: cleanup hook"
cat > "$OUT/child.sh" << 'EOF'
set -euo pipefail
mode="$1"
d="$2"
cd "$3"
. ./scripts/qemu-lib.sh
QEMU="$d/qemu"
hook() {
    local alive=no p
    for p in $(cat "$d/pids" 2> /dev/null); do kill -0 "$p" 2> /dev/null && alive=yes; done
    echo "hook: qemu alive=$alive" > "$d/hook.txt"
    [ -e "$Q_TMP" ] && echo "hook: monitor dir left" >> "$d/hook.txt"
    return 1   # a failing hook must not change the exit status
}
started() {
    local i
    for ((i = 0; i < 100; i++)); do [ -s "$d/pids" ] && return 0; sleep 0.1; done
    echo "the stand-in did not start" >&2
    exit 9
}
[ "$mode" = default ] || Q_CLEANUP=hook
case "$mode" in
    term-now|term-running) trap 'exit 1' TERM INT ;;
    term-launch) trap 'exit 1' TERM INT; QEMU_TIMEOUT=2; export TMPDIR="$d/tmp" ;;
esac
qemu_start "$d/disk.img" "$d/out"
case "$mode" in
    term-now|term-launch) kill -TERM $$ ;;
    term-running) started; kill -TERM $$ ;;
    error)        started; false ;;
    *)            started; echo "$Q_TMP" > "$d/monitor-dir"; exit 3 ;;
esac
sleep 30
exit 8
EOF
# term-launch: TERM between starting QEMU in the background and Q_PID=$!, through a copy of
# qemu-lib.sh with a kill there. Neither the exit trap nor Q_PID is set yet: QEMU must still start
# and end at QEMU_TIMEOUT (2 s), and no shell may stay blocked opening the monitor pipe.
for mode in normal default error term-running term-now term-launch; do
    d="$OUT/hook-$mode"
    mkdir -p "$d"
    : > "$d/disk.img"
    printf '#!/usr/bin/env bash\necho $$ >> "%s/pids"\nexec sleep 7391\n' "$d" > "$d/qemu"
    chmod +x "$d/qemu"
    repo="$PWD"
    if [ "$mode" = term-launch ]; then
        repo="$d/repo"
        mkdir -p "$repo/scripts" "$d/tmp"
        ln -sfn "$PWD/build" "$repo/build"
        sed 's/^    Q_PID=\$!$/    kill -TERM $$; Q_PID=$!/' scripts/qemu-lib.sh > "$repo/scripts/qemu-lib.sh"
        grep -qF 'kill -TERM $$; Q_PID=' "$repo/scripts/qemu-lib.sh" || echo "no kill put in" > "$d/inject.txt"
    fi
    # timeout leads a process group of its own: what is left of this child afterwards is in group
    # $child, and only that group is counted and stopped (never another run's child).
    timeout 30 bash "$OUT/child.sh" "$mode" "$d" "$repo" > "$d/child.log" 2>&1 &
    child=$!
    if wait "$child"; then
        status=0
    else
        status=$?
    fi
    if [ "$mode" = term-launch ]; then
        for ((i = 0; i < 80; i++)); do
            # Read the pid list once: it may be written between two reads of it.
            started_pids=$(cat "$d/pids" 2> /dev/null || true)
            alive=0
            for p in $started_pids; do kill -0 "$p" 2> /dev/null && alive=1; done
            [ -n "$started_pids" ] && [ "$alive" = 0 ] && break
            sleep 0.1
        done
        echo "blocked=$(pgrep -g "$child" | wc -l)" > "$d/blocked.txt"
        pkill -g "$child" || :
    fi
    left=0
    for p in $(cat "$d/pids" 2> /dev/null); do
        if kill -0 "$p" 2> /dev/null; then left=$((left + 1)); kill "$p" 2> /dev/null || :; fi
    done
    { echo "exit=$status"; echo "left=$left"; echo "started=$(cat "$d/pids" 2> /dev/null | wc -w)"; } > "$d/result.txt"
    if [ -s "$d/monitor-dir" ] && [ -e "$(cat "$d/monitor-dir")" ]; then echo "monitor dir left" >> "$d/result.txt"; fi
    sed "s|^|  $mode: |" "$d/result.txt" "$d/child.log" "$d/hook.txt" 2> /dev/null || true
done
for mode in normal default error term-running term-now term-launch; do
    r="$OUT/hook-$mode/result.txt"
    check "$mode: no stand-in QEMU left" "$r" "left=0" line
done
check "term-launch: the kill is in the lib copy"     "$OUT/hook-term-launch/inject.txt"  "no kill" absent
check "term-launch: exits through the TERM trap"     "$OUT/hook-term-launch/result.txt"  "exit=1" line
check "term-launch: QEMU still started"              "$OUT/hook-term-launch/result.txt"  "started=1" line
check "term-launch: no shell blocked on the pipe"    "$OUT/hook-term-launch/blocked.txt" "blocked=0" line
for mode in normal error term-running term-now; do
    h="$OUT/hook-$mode/hook.txt"
    check "$mode: hook ran after QEMU was stopped" "$h" "hook: qemu alive=no" line
    check "$mode: monitor directory removed"       "$h" "hook: monitor dir left" absent
done
check "normal: QEMU was running"                 "$OUT/hook-normal/result.txt"       "started=1" line
check "normal: exit status kept"                 "$OUT/hook-normal/result.txt"       "exit=3" line
check "default: exit status kept"                "$OUT/hook-default/result.txt"      "exit=3" line
check "default: monitor directory removed"       "$OUT/hook-default/result.txt"      "monitor dir left" absent
check "default: no hook runs"                    "$OUT/hook-default/hook.txt"        "hook" absent
check "error: exits like set -e"                 "$OUT/hook-error/result.txt"        "exit=1" line
check "term-running: QEMU was running"           "$OUT/hook-term-running/result.txt" "started=1" line
check "term-running: exits through the TERM trap" "$OUT/hook-term-running/result.txt" "exit=1" line
check "term-now: exits through the TERM trap"    "$OUT/hook-term-now/result.txt"     "exit=1" line

if [ "$failures" -ne 0 ]; then
    echo "screen-waits: $failures check(s) failed" >&2
    exit 1
fi
echo "screen-waits: all checks passed"
