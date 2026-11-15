# Source from the repository root. Build an isolated image with the full heap available
# to one process, so pressure tests still reach the kernel allocator after the budget
# becomes effective. The shipped config and build are never changed.

# budget_profile_build <test-root>: sets BUDGET_PROFILE_DIR to an absolute path.
budget_profile_build() {
    local root fingerprint profile stage
    [ "$#" -eq 1 ] && [ -n "$1" ] || {
        echo 'budget profile: expected one test-root directory' >&2; return 1;
    }
    [ -z "${RORTHOX_BUDGET_PROFILE:-}" ] || {
        echo 'budget profile: recursive build refused' >&2; return 1;
    }
    [ -f config.mk ] && [ -f Makefile ] && git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
        echo 'budget profile: run from the repository root' >&2; return 1;
    }
    mkdir -p "$1" || { echo "budget profile: cannot create $1" >&2; return 1; }
    root=$(cd "$1" && pwd -P) || { echo "budget profile: cannot enter $1" >&2; return 1; }
    profile="$root/budget-profile"
    # Include the revision, every tracked source/config byte and the tracked file modes.
    # Scratch output is untracked, so it cannot change the fingerprint mid-run.
    fingerprint=$({ git rev-parse HEAD && git ls-files -s &&
        git ls-files -z | xargs -0 -r sha256sum; } | sha256sum | cut -d ' ' -f 1) || return 1
    [ -n "$fingerprint" ] || { echo 'budget profile: cannot fingerprint source' >&2; return 1; }
    (
        flock -x 9 || { echo 'budget profile: cannot lock build' >&2; exit 1; }
        if [ ! -f "$profile/.source-fingerprint" ] ||
           [ "$(cat "$profile/.source-fingerprint")" != "$fingerprint" ] ||
           [ ! -f "$profile/bin/os.bin" ] || [ ! -f "$profile/build/kernel.map" ]; then
            stage=$(mktemp -d "$root/.budget-profile.build.XXXXXXXX") || exit 1
            # Preserve the working tree's tracked bytes (including an uncommitted patch).
            if ! git ls-files -z | tar --null -T - -cf - | (cd "$stage" && tar -xf -); then
                rm -rf "$stage"; echo 'budget profile: copying source failed' >&2; exit 1
            fi
            if ! awk '
                /^PROCESS_HEAP_LIMIT_BYTES = / { print "PROCESS_HEAP_LIMIT_BYTES = 104857600"; found++; next }
                { print }
                END { if (found != 1) exit 1 }
            ' "$stage/config.mk" > "$stage/config.mk.tmp"; then
                rm -rf "$stage"; echo 'budget profile: config entry missing or duplicated' >&2; exit 1
            fi
            mv "$stage/config.mk.tmp" "$stage/config.mk" || exit 1
            if ! (cd "$stage" && make clean all iso) > "$root/budget-profile-build.log" 2>&1; then
                echo "budget profile: build failed (see $root/budget-profile-build.log)" >&2
                rm -rf "$stage"; exit 1
            fi
            if ! grep -qxF '#define PROCESS_HEAP_LIMIT_BYTES 104857600' "$stage/build/config/rorthox-config.h" ||
               ! grep -qxF 'PROCESS_HEAP_LIMIT_BYTES=104857600' "$stage/build/config/rorthox-config.env"; then
                echo 'budget profile: built configuration is not 100 MiB' >&2
                rm -rf "$stage"; exit 1
            fi
            printf '%s\n' "$fingerprint" > "$stage/.source-fingerprint"
            rm -rf "$profile" && mv "$stage" "$profile" || {
                echo 'budget profile: cannot install built profile' >&2; exit 1;
            }
        fi
    ) 9> "$root/.budget-profile.lock" || return 1
    BUDGET_PROFILE_DIR="$profile"
}

# budget_profile_run <relative test script> [test args...]
# In the child, RORTHOX_BUDGET_PROFILE prevents a second build/dispatch.
budget_profile_run() {
    local script="$1" source_root test_root
    shift
    [ -z "${RORTHOX_BUDGET_PROFILE:-}" ] || {
        echo 'budget profile: recursive test dispatch refused' >&2; return 1;
    }
    source_root=$(pwd -P) || return 1
    budget_profile_build "${RORTHOX_TEST_ROOT:-build/tests}" || return 1
    case "$script" in
        "$source_root"/*) script=${script#"$source_root"/} ;;
        /*) echo 'budget profile: test must be inside the source tree' >&2; return 1 ;;
    esac
    script=${script#./}
    case "$script" in scripts/tests/*.sh) ;; *) echo 'budget profile: invalid test path' >&2; return 1 ;; esac
    [ -f "$BUDGET_PROFILE_DIR/$script" ] || return 1
    test_root=${RORTHOX_TEST_ROOT:-build/tests}
    mkdir -p "$test_root" || return 1
    test_root=$(cd "$test_root" && pwd -P) || return 1
    (cd "$BUDGET_PROFILE_DIR" && RORTHOX_TEST_ROOT="$test_root" RORTHOX_BUDGET_PROFILE="$BUDGET_PROFILE_DIR" bash "./$script" "$@")
}
