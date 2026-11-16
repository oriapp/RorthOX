# fsck.fat checks for the FAT tests. Source it after scripts/qemu-lib.sh (it uses check()).
#
#   fsck_clean <what> <image> <clusters> <log>
#       one check: fsck.fat -n finds nothing on <image>. Clean means exit 0, nothing on stderr and
#       exactly the two normal lines on stdout, "fsck.fat <version> (<date>)" and
#       "<copy>: <n> files, <used>/<clusters> clusters": a finding that fsck.fat reports but
#       leaves alone (some long name faults) still exits 0, and a different cluster count means
#       the image doesn't have the layout the test was written for.
#   fsck_expect <what> <image> <clusters> <exit> <log> <diagnostics file>
#       one check for a fixture damaged on purpose: exactly that exit status, and exactly the
#       lines of <diagnostics file> between the version line and the summary line.
#
# fsck.fat runs with LC_ALL=C on a sparse copy (in $TMPDIR, removed afterwards, also on TERM or
# INT, after which the caller's own trap runs) made as long
# as the BPB says (TotSec * BytsPerSec): fsck.fat refuses an image file shorter than its
# volume. <image> itself is never changed. <log> gets the output, the exit status, the tool
# versions and the verdict ("clean", "expected" or what was wrong). A missing fsck.fat (package
# dosfstools) fails the check: it is never skipped.
#
# Env: FSCK_FAT=fsck.fat

# fsck_trap_command <"trap -p" line> <default>: FSCK_TRAP_COMMAND = that trap's command, or <default>.
fsck_trap_command() {
    FSCK_TRAP_COMMAND="$2"
    [ -n "$1" ] || return 0
    eval "set -- ${1#trap -- }"
    FSCK_TRAP_COMMAND="$1"
}

# fsck_trap_restore <signal> <"trap -p" line>: put the caller's trap back (or the default).
fsck_trap_restore() {
    if [ -n "$2" ]; then eval "$2"; else trap - "$1"; fi
}

fsck_run() {
    local image="$1" log="$2" dir copy bytes s old_term old_int
    : > "$log"
    if ! command -v "${FSCK_FAT:-fsck.fat}" > /dev/null 2>&1; then
        echo "verdict: fsck.fat not found (install dosfstools)" >> "$log"
        return 1
    fi
    bytes=$(python3 -c 'import struct,sys
d = open(sys.argv[1], "rb").read(64)
print((struct.unpack_from("<H", d, 19)[0] or struct.unpack_from("<I", d, 32)[0]) * struct.unpack_from("<H", d, 11)[0])' "$image") ||
        { echo "verdict: no BPB in $image" >> "$log"; return 1; }
    dir=$(mktemp -d)
    copy="$dir/check.img"
    # A TERM or INT while the copy exists (a runner timeout during a long check) removes it, then
    # does what the caller's trap does (or ends the script). The caller's traps come back after.
    old_term=$(trap -p TERM)
    old_int=$(trap -p INT)
    fsck_trap_command "$old_term" "exit 143"
    trap "rm -rf '$dir'; $FSCK_TRAP_COMMAND" TERM
    fsck_trap_command "$old_int" "exit 130"
    trap "rm -rf '$dir'; $FSCK_TRAP_COMMAND" INT
    cp --sparse=always "$image" "$copy"
    [ "$(stat -c %s "$copy")" -ge "$bytes" ] || truncate -s "$bytes" "$copy"
    if LC_ALL=C "${FSCK_FAT:-fsck.fat}" -n "$copy" > "$dir/out" 2> "$dir/err"; then s=0; else s=$?; fi
    rm -f "$copy"
    # The copy's name is the same for every image, so logs of two images compare line by line.
    sed "s|^$copy: |check.img: |" "$dir/out" > "$log.out"
    cp "$dir/err" "$log.err"
    rm -rf "$dir"
    fsck_trap_restore TERM "$old_term"
    fsck_trap_restore INT "$old_int"
    {
        echo "exit=$s"
        echo "fsck: $(head -n 1 "$log.out")"
        echo "mtools: $(mtools --version 2> /dev/null | head -n 1)"
        sed 's/^/stdout: /' "$log.out"
        sed 's/^/stderr: /' "$log.err"
    } >> "$log"
    return 0
}

# fsck_verdict <log> <clusters> <exit> [<diagnostics file>]: "clean"/"expected", or what is wrong.
fsck_verdict() {
    local log="$1" clusters="$2" want="$3" diag="${4:-/dev/null}" n s
    n=$(wc -l < "$log.out")
    s=$(sed -n 's/^exit=//p' "$log")
    if [ "$s" != "$want" ]; then
        echo "exit status $s, expected $want"
    elif [ -s "$log.err" ]; then
        echo "output on stderr: $(head -n 1 "$log.err")"
    elif ! head -n 1 "$log.out" | grep -qE '^fsck\.fat [0-9][0-9.]* \([0-9-]+\)$'; then
        echo "no version line"
    elif ! tail -n 1 "$log.out" | grep -qE "^check\.img: [0-9]+ files, [0-9]+/$clusters clusters\$"; then
        echo "summary is not \"check.img: N files, U/$clusters clusters\": $(tail -n 1 "$log.out")"
    elif [ "$n" -lt 2 ] || ! sed '1d;$d' "$log.out" | cmp -s - "$diag"; then
        echo "findings differ from the expected ones: $(sed '1d;$d' "$log.out" | head -n 1)"
    elif [ "$diag" = /dev/null ]; then
        echo "clean"
    else
        echo "expected"
    fi
}

fsck_clean() {
    local what="$1" image="$2" clusters="$3" log="$4"
    if fsck_run "$image" "$log"; then
        echo "verdict: $(fsck_verdict "$log" "$clusters" 0)" >> "$log"
    fi
    echo "  fsck.fat -n $image: $(sed -n 's/^fsck: //p' "$log"), $(sed -n 's/^mtools: //p' "$log"); $(grep '^verdict: ' "$log")"
    check "$what" "$log" "verdict: clean" line
}

fsck_expect() {
    local what="$1" image="$2" clusters="$3" want="$4" log="$5" diag="$6"
    if fsck_run "$image" "$log"; then
        echo "verdict: $(fsck_verdict "$log" "$clusters" "$want" "$diag")" >> "$log"
    fi
    echo "  fsck.fat -n $image: $(sed -n 's/^fsck: //p' "$log"), $(sed -n 's/^mtools: //p' "$log"); $(grep '^verdict: ' "$log")"
    check "$what" "$log" "verdict: expected" line
}
