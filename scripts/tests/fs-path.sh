#!/usr/bin/env bash
#
# Paths for the file calls. Host checks of the real file.c, pparser.c, fat16.c and streamer.c on
# the image disk model (scripts/tests/host/fat-image.c), with bin/os.bin as the disk:
#
#   legacy matrix  scripts/tests/host/fs-path-legacy.c prints what file_validate_path,
#                  pathparser_parse, fopen_status (r, w, a, x) and file_open (modes 0-3) give for
#                  about 1100 paths (every byte at four places in a name, the 8.3 lengths, 104-112
#                  characters around the 108-byte limit, bad drives, separators, dots, spaces); it
#                  must equal scripts/tests/host/fs-path-legacy.txt byte for byte, the matrix of
#                  the code before the extended paths, with nothing leaked and no crash;
#   extended       scripts/tests/host/fs-path.c checks pathparser_parse_extended and the open
#                  policies: 260 bytes with the NUL, components of 1-255 characters, at most 128,
#                  no empty, "." or ".." components, the character rules for every byte, inputs
#                  that end right before an unreadable page (also 260 bytes with no NUL), one
#                  allocation per path and none for a refused one, allocation failures, and an
#                  extended FAT open that is -EUNIMP in every mode without touching the disk.
#
# Built with AddressSanitizer. Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check() and finish()

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fs-path
mkdir -p "$OUT"
[ -f bin/os.bin ] || { echo "fs-path: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -f "$OUT"/host-* "$OUT"/legacy-* "$OUT"/*.txt

# build <name> <source>: the host check with the kernel's path, VFS and FAT code.
build() {
    "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address \
        -I kernel -I build/config -I scripts/tests/host \
        "$2" scripts/tests/host/fat-image.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/$1" > "$OUT/$1-build.log" 2>&1
}

echo "fs-path: the legacy path matrix"
legacy_status="not built"
if build legacy-check scripts/tests/host/fs-path-legacy.c; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/legacy-check" bin/os.bin > "$OUT/legacy-matrix.txt" 2> "$OUT/legacy-check.log"; then
        legacy_status=0
    else
        legacy_status=$?
    fi
    if diff scripts/tests/host/fs-path-legacy.txt "$OUT/legacy-matrix.txt" > "$OUT/legacy-diff.txt"; then
        echo same > "$OUT/legacy-same.txt"
    else
        echo different > "$OUT/legacy-same.txt"
        echo "  the matrix differs (expected <, got >):"
        head -n 20 "$OUT/legacy-diff.txt" | sed 's/^/  /'
    fi
    echo "  $(wc -l < "$OUT/legacy-matrix.txt") paths"
else
    echo "  host build failed, see $OUT/legacy-check-build.log"
    head -n 10 "$OUT/legacy-check-build.log" | sed 's/^/  /' || true
fi
echo "exit=$legacy_status" > "$OUT/legacy-status.txt"
check "legacy matrix built and exited with status 0"  "$OUT/legacy-status.txt" "exit=0" line
check "legacy matrix equals the recorded one"          "$OUT/legacy-same.txt"   "same" line

echo "fs-path: host check of the extended paths"
host_status="not built"
if build host-check scripts/tests/host/fs-path.c; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" bin/os.bin > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-check-build.log"
    head -n 10 "$OUT/host-check-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: every case passed"              "$OUT/host-check.log"  "fs-path host check: 0 of 6 cases failed" line
for c in "known answers" "refusals" "characters" "lengths" "allocation" "fail closed"; do
    grep -E "^  $c: [1-9][0-9]* checks, 0 failed$" "$OUT/host-check.log" > /dev/null 2>&1 && echo "$c ran" >> "$OUT/cases.txt" || true
    check "host check: $c"  "$OUT/cases.txt" "$c ran" line
done

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "fs-path: $failures check(s) failed" >&2
    exit 1
fi
echo "fs-path: all checks passed"
