# The real kernel heap out-of-memory tests (heap.sh, heap-oom.sh and the tests that run their cases
# through it, print-oom.sh) fill the kernel heap from one program. A per-process allocation budget
# (PROCESS_HEAP_LIMIT_BYTES in config.mk) would refuse that program long before the heap is full
# and hide the failure these tests are about. So their QEMU part runs on a separately built copy of
# the same source, where only the copied config.mk sets the budget to 100 MiB, the whole heap
# (scripts/tests/lib/budget-profile.sh). The shipped image and its budget don't change.
#
# Source it right after changing to the checkout root, then:
#   pressure_profile <test script> "$@"
#       Outside the profile: build it (or reuse it) under the test root, change into it, check that
#       the working directory is the profile, and run the same test script there again (exec), with
#       its output still under this test root. The host checks a test makes before this stay in
#       the checkout. Inside the profile (RORTHOX_BUDGET_PROFILE set): check that the working
#       directory is that profile and return.
#   pressure_profile_checks <outdir>
#       Inside the profile, after qemu-lib.sh: the profile's config has the 100 MiB budget and is
#       the checkout's config with only that value changed, and the checkout keeps the shipped
#       budget.
PRESSURE_PROFILE_LIMIT=104857600
PRESSURE_SHIPPED_LIMIT=8388608

pressure_profile() {
    local script="$1" root main dir
    shift
    if [ -n "${RORTHOX_BUDGET_PROFILE:-}" ]; then
        if [ "$(pwd -P)" != "$RORTHOX_BUDGET_PROFILE" ] || [ -z "${RORTHOX_BUDGET_MAIN:-}" ]; then
            echo "$script: not running in the budget profile $RORTHOX_BUDGET_PROFILE (in $(pwd -P))" >&2
            exit 1
        fi
        return 0
    fi
    main=$(pwd -P)
    root=${RORTHOX_TEST_ROOT:-build/tests}
    mkdir -p "$root" && root=$(cd "$root" && pwd -P) || exit 1
    . ./scripts/tests/lib/budget-profile.sh
    budget_profile_build "$root" || { echo "$script: the 100 MiB budget profile did not build" >&2; exit 1; }
    dir=$(cd "$BUDGET_PROFILE_DIR" && pwd -P) || { echo "$script: no profile at $BUDGET_PROFILE_DIR" >&2; exit 1; }
    if [ "$dir" = "$main" ] || [ ! -f "$dir/scripts/tests/$script" ]; then
        echo "$script: $dir is not a separate built copy of the checkout" >&2
        exit 1
    fi
    cd "$dir" || exit 1
    [ "$(pwd -P)" = "$dir" ] || { echo "$script: could not change to $dir" >&2; exit 1; }
    echo "$script: QEMU part on the 100 MiB budget profile ($dir)"
    RORTHOX_BUDGET_PROFILE="$dir" RORTHOX_BUDGET_MAIN="$main" RORTHOX_TEST_ROOT="$root" exec bash "scripts/tests/$script" "$@"
}

pressure_profile_checks() {
    local out="$1/profile.txt"
    {
        awk '$1 == "#define" && $2 == "PROCESS_HEAP_LIMIT_BYTES" { print "profile=" $3 }' build/config/rorthox-config.h
        # What the kernel's own headers give, and the kernel built after them.
        if printf '#include "config.h"\n_Static_assert(PROCESS_HEAP_LIMIT_BYTES == %s, "budget");\n' \
                "$PRESSURE_PROFILE_LIMIT" | i686-elf-gcc -std=gnu99 -ffreestanding -I kernel -I build/config \
                -x c -c - -o /dev/null && [ build/kernelfull.o -nt build/config/rorthox-config.h ]; then
            echo "kernel=$PRESSURE_PROFILE_LIMIT"
        fi
        awk '$1 == "#define" && $2 == "PROCESS_HEAP_LIMIT_BYTES" { print "shipped=" $3 }' \
            "$RORTHOX_BUDGET_MAIN/build/config/rorthox-config.h"
        if diff <(grep -v '^PROCESS_HEAP_LIMIT_BYTES[[:space:]]*=' "$RORTHOX_BUDGET_MAIN/config.mk") \
                <(grep -v '^PROCESS_HEAP_LIMIT_BYTES[[:space:]]*=' config.mk) > /dev/null; then
            echo "config=same"
        fi
    } > "$out" 2> /dev/null
    sed 's/^/  profile: /' "$out"
    check "runs on the budget profile: process budget $PRESSURE_PROFILE_LIMIT" "$out" "profile=$PRESSURE_PROFILE_LIMIT" line
    check "the profile's kernel was built with that budget"      "$out" "kernel=$PRESSURE_PROFILE_LIMIT" line
    check "the profile's config.mk differs only in the budget"   "$out" "config=same" line
    check "the checkout keeps the shipped budget ($PRESSURE_SHIPPED_LIMIT)" "$out" "shipped=$PRESSURE_SHIPPED_LIMIT" line
}
