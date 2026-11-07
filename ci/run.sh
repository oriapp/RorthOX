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
ref=${RORTHOX_IMAGE_REF:-$(grep -v '^#' "$root/ci/image.ref" | head -n1)}
if [[ ! "$ref" =~ ^ghcr\.io/oriapp/rorthox-build@sha256:[0-9a-f]{64}$ ]] ||
   [[ "$ref" == *'@sha256:0000000000000000000000000000000000000000000000000000000000000000' ]]; then
    echo 'error: no published image digest; set RORTHOX_IMAGE_REF to a verified digest' >&2
    exit 2
fi
# This list is deliberately explicit: do not inherit credentials or Actions file-command paths.
args=(--rm --init --user "$(id -u):$(id -g)" --network none
      --cap-drop ALL --security-opt no-new-privileges --pids-limit 512
      --memory 4g -v "$PWD:/work" -w /work)
if [ -c /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    args+=(--device /dev/kvm)
fi
for name in GITHUB_ACTIONS RORTHOX_ACCEL RORTHOX_REQUIRE_ISO RORTHOX_TEST_TIMEOUT RORTHOX_TEST_ROOT; do
    if [ "${!name+x}" ]; then args+=(-e "$name=${!name}"); fi
done
exec docker run "${args[@]}" "$ref" "$@"
