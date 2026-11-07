#!/usr/bin/env bash
# Run a command in the candidate build image; inside that image run it directly.
set -euo pipefail
if [ "$#" -eq 0 ]; then
    echo 'usage: ci/run.sh command [args...]' >&2
    exit 2
fi
if [ "${RORTHOX_IN_IMAGE:-}" = 1 ] && [ -f /opt/rorthox-build-image ]; then
    exec "$@"
fi
root=$(cd "$(dirname "$0")/.." && pwd -P)
work=$(pwd -P)
case "$work" in
    "$root") subdir= ;;
    "$root"/*) subdir=${work#"$root"} ;;
    *) echo 'error: run from inside the checkout' >&2; exit 2 ;;
esac
if [ -n "${RORTHOX_IMAGE_ID:-}" ]; then
    ref=$RORTHOX_IMAGE_ID
    if [[ ! "$ref" =~ ^sha256:[0-9a-f]{64}$ ]]; then
        echo 'error: invalid loaded image ID' >&2
        exit 2
    fi
else
    ref=${RORTHOX_IMAGE_REF:-$(grep -v '^#' "$root/ci/image.ref" | head -n1)}
    if [[ ! "$ref" =~ ^ghcr\.io/oriapp/rorthox-build@sha256:[0-9a-f]{64}$ ]] ||
       [[ "$ref" == *'@sha256:0000000000000000000000000000000000000000000000000000000000000000' ]]; then
        echo 'error: no published image digest; set RORTHOX_IMAGE_REF to a verified digest' >&2
        exit 2
    fi
fi
# This list is deliberately explicit: do not inherit credentials or Actions file-command paths.
args=(--rm --init --user "$(id -u):$(id -g)" --network none
      --cap-drop ALL --security-opt no-new-privileges --pids-limit 512
      --memory 4g -v "$root:/work" -w "/work$subdir")
if [ "${RORTHOX_ACCEL:-}" != tcg ] && [ -c /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    args+=(--device /dev/kvm)
    kvm_gid=$(stat -c %g /dev/kvm)
    if [ "$kvm_gid" != 0 ]; then args+=(--group-add "$kvm_gid"); fi
fi
for name in GITHUB_ACTIONS RORTHOX_ACCEL RORTHOX_REQUIRE_ISO RORTHOX_TEST_TIMEOUT; do
    if [ "${!name+x}" ]; then args+=(-e "$name=${!name}"); fi
done
if [ "${RORTHOX_TEST_ROOT+x}" ]; then
    test_root=$RORTHOX_TEST_ROOT
    case "$test_root" in
        "$root") test_root=/work ;;
        "$root"/*) test_root="/work${test_root#"$root"}" ;;
        /*|..|../*|*/..|*/../*) echo 'error: test root must be inside the checkout' >&2; exit 2 ;;
    esac
    args+=(-e "RORTHOX_TEST_ROOT=$test_root")
fi
exec docker run "${args[@]}" "$ref" "$@"
