# Helpers for tests that boot RorthOX in a headless QEMU and drive it through the QEMU monitor.
# Source this file from a bash test script, then:
#
#   qemu_start <image> <outdir>   boot <image>; the kernel's debugcon output goes to <outdir>/debugcon.log
#   wait_screen <text> <seconds>  poll the VGA text screen until <text> shows up (returns 1 on timeout)
#   type_line <text>              type <text> and Enter on the PS/2 keyboard
#   qemu_cmd <monitor command>    send any QEMU monitor command
#   screen_snapshot               refresh <outdir>/screen.txt from guest memory
#   qemu_stop                     save screen.txt and screen.ppm, quit QEMU (returns 1 if QEMU failed or timed out)
#   check <description> <file> <text> [present|absent|line]
#                                 one check, printed as ok/FAIL; failures are counted in $failures
#   finish <name>                 print the screen and the result; exits 1 if a check failed
#
# The screen is read straight from the VGA text buffer at 0xB8000 ("pmemsave"), so tests never depend on
# fixed delays: they wait until the expected text is there, which also works on a slow machine.
#
# Env: QEMU=qemu-system-i386   QEMU_FLAGS="extra flags"   QEMU_TIMEOUT=180 (hard limit per QEMU run, seconds)
# Needs only qemu-system-i386 and the base system tools (timeout, mkfifo, od, awk, grep, sed).

QEMU="${QEMU:-qemu-system-i386}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
failures=0

# VGA text memory is (character, attribute) pairs, 80 columns x 25 rows. Keep the characters,
# show anything that isn't printable ASCII as a space, and strip trailing spaces.
vga_to_text() {
    od -An -v -tu1 -w2 "$1" |
        awk '{ printf "%c", ($1 >= 32 && $1 < 127) ? $1 : 32 } NR % 80 == 0 { printf "\n" }' |
        sed 's/ *$//'
}

qemu_start() {
    Q_IMAGE="$1"
    Q_OUT="$2"
    [ -f "$Q_IMAGE" ] || { echo "$Q_IMAGE not found, run 'make all' first" >&2; exit 2; }
    mkdir -p "$Q_OUT"
    rm -f "$Q_OUT/debugcon.log" "$Q_OUT/screen.txt" "$Q_OUT/screen.ppm" "$Q_OUT/vga.bin" "$Q_OUT/monitor.log"
    # QEMU reads monitor commands from a named pipe, so this script can send them one at a time.
    Q_TMP="$(mktemp -d)"
    mkfifo "$Q_TMP/monitor"
    timeout "$QEMU_TIMEOUT" "$QEMU" -m 128 -display none -no-reboot \
        -drive "file=$Q_IMAGE,format=raw,index=0,media=disk" \
        -debugcon "file:$Q_OUT/debugcon.log" -serial none -monitor stdio \
        ${QEMU_FLAGS:-} < "$Q_TMP/monitor" > "$Q_OUT/monitor.log" 2>&1 &
    Q_PID=$!
    exec 3> "$Q_TMP/monitor"
    trap '' PIPE   # if QEMU is gone, writing a command fails instead of killing the script
    trap 'qemu_kill' EXIT
}

qemu_kill() {
    if [ -n "${Q_PID:-}" ]; then kill "$Q_PID" 2> /dev/null; wait "$Q_PID" 2> /dev/null; fi
    Q_PID=
    [ -n "${Q_TMP:-}" ] && rm -rf "$Q_TMP"
    return 0
}

qemu_cmd() {
    echo "$*" >&3 2> /dev/null || true
}

qemu_running() {
    [ -n "${Q_PID:-}" ] && kill -0 "$Q_PID" 2> /dev/null
}

# wait_file <file> <size>: wait up to 10 s for QEMU to finish writing <file>.
wait_file() {
    local i
    for ((i = 0; i < 100; i++)); do
        [ -f "$1" ] && [ "$(wc -c < "$1")" -ge "$2" ] && return 0
        qemu_running || return 1
        sleep 0.1
    done
    return 1
}

screen_snapshot() {
    rm -f "$Q_OUT/vga.bin"
    # Quote file names: the monitor would parse "4000 /tmp/x" as the expression 4000/tmp.
    qemu_cmd "pmemsave 0xb8000 4000 \"$Q_OUT/vga.bin\""
    wait_file "$Q_OUT/vga.bin" 4000 && vga_to_text "$Q_OUT/vga.bin" > "$Q_OUT/screen.txt"
    return 0
}

wait_screen() {
    local text="$1" deadline=$((SECONDS + $2))
    while ((SECONDS < deadline)); do
        if ! qemu_running; then
            echo "  QEMU is not running, stopped waiting for \"$text\""
            return 1
        fi
        screen_snapshot
        if grep -qF -- "$text" "$Q_OUT/screen.txt" 2> /dev/null; then
            echo "  \"$text\" on screen after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for \"$text\" after $2 s"
    return 1
}

type_line() {
    local s="$1" i c key
    for ((i = 0; i < ${#s}; i++)); do
        c="${s:i:1}"
        case "$c" in
            ' ') key=spc ;;
            '.') key=dot ;;
            '/') key=slash ;;
            '-') key=minus ;;
            *)   key="$c" ;;
        esac
        qemu_cmd "sendkey $key"
        sleep 0.1
    done
    qemu_cmd "sendkey ret"
}

qemu_stop() {
    local rc=0
    screen_snapshot
    rm -f "$Q_OUT/screen.ppm"
    qemu_cmd "screendump \"$Q_OUT/screen.ppm\""
    wait_file "$Q_OUT/screen.ppm" 1 || true
    qemu_cmd "quit"
    exec 3>&-
    wait "$Q_PID" || rc=$?
    Q_PID=
    rm -rf "$Q_TMP"
    if [ "$rc" -ne 0 ]; then
        echo "  QEMU failed or timed out (exit $rc), see $Q_OUT/monitor.log"
        failures=$((failures + 1))
        return 1
    fi
    [ -f "$Q_OUT/screen.txt" ] || { echo "  no screen dump, see $Q_OUT/monitor.log"; failures=$((failures + 1)); return 1; }
    return 0
}

check() {
    local what="$1" file="$2" text="$3" mode="${4:-present}" opts=-qF found
    [ "$mode" = line ] && { opts=-qxF; mode=present; }   # "line": the whole line must match
    if grep $opts -- "$text" "$file" 2> /dev/null; then found=1; else found=0; fi
    if { [ "$mode" = present ] && [ $found = 1 ]; } || { [ "$mode" = absent ] && [ $found = 0 ]; }; then
        echo "  ok   $what"
    else
        echo "  FAIL $what"
        failures=$((failures + 1))
    fi
}

finish() {
    echo "$1: screen"
    if [ -f "$Q_OUT/screen.txt" ]; then sed 's/^/  | /' "$Q_OUT/screen.txt"; else echo "  (none)"; fi
    if [ "$failures" -ne 0 ]; then
        echo "$1: $failures check(s) failed" >&2
        exit 1
    fi
    echo "$1: all checks passed"
}
