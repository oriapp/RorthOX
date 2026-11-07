#!/usr/bin/env bash
# Host-only contract for image inputs and the container launch boundary.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/ci-image
mkdir -p "$OUT"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL ci-image: $*" >&2; exit 1; }
for file in ci/Dockerfile ci/image-key.sh ci/run.sh ci/image.ref; do
    [ -f "$file" ] || fail "missing $file"
done
bash -n ci/image-key.sh ci/run.sh configure.sh
key=$(bash ci/image-key.sh)
[[ "$key" =~ ^[0-9a-f]{64}$ ]] || fail 'invalid image key'
mkdir -p "$tmp/copy/ci" "$tmp/context"
cp ci/Dockerfile ci/image-key.sh "$tmp/copy/ci/"
cp configure.sh "$tmp/copy/"
[ "$(bash "$tmp/copy/ci/image-key.sh")" = "$key" ] || fail 'key changes outside repository'
printf 'unrelated\n' > "$tmp/copy/unrelated"
[ "$(bash "$tmp/copy/ci/image-key.sh")" = "$key" ] || fail 'unrelated file changes key'
while IFS= read -r file; do
    printf '\n# changed\n' >> "$tmp/copy/$file"
    [ "$(bash "$tmp/copy/ci/image-key.sh")" != "$key" ] || fail "$file mutation did not change key"
    cp "$file" "$tmp/copy/$file"
done < <(bash ci/image-key.sh --inputs)
bash ci/image-key.sh --context "$tmp/context"
[ "$(find "$tmp/context" -type f | LC_ALL=C sort | sed "s|$tmp/context/||")" = "$(bash ci/image-key.sh --inputs)" ] || fail 'build context has unlisted files'
if bash ci/image-key.sh --context "$tmp/context" >/dev/null 2>&1; then fail 'nonempty context accepted'; fi
# The recorded placeholder has the same digest form as the published reference.
ref=$(grep -v '^#' ci/image.ref | head -n1)
[[ "$ref" =~ ^ghcr\.io/oriapp/rorthox-build@sha256:[0-9a-f]{64}$ ]] || fail 'invalid reference'
[ "$(grep -c '^FROM ' ci/Dockerfile)" -eq 2 ] || fail 'unexpected image stages'
if grep '^FROM ' ci/Dockerfile | grep -Ev '^FROM ubuntu:24\.04@sha256:[0-9a-f]{64}( AS [a-z]+)?$' >/dev/null; then
    fail 'unfixed base'
fi
grep -q 'snapshot.ubuntu.com/ubuntu/[0-9]\{8\}T[0-9]\{6\}Z' ci/Dockerfile || fail 'no apt snapshot'
! grep -Eq '(^|[[:space:]])(COPY[[:space:]]+\.|ADD[[:space:]]+https?://|latest)' ci/Dockerfile || fail 'unbounded Docker input'
grep -q 'org.opencontainers.image.licenses="GPL-2.0"' ci/Dockerfile || fail 'license label'
# Fake docker records exact argv with NUL separators; no daemon is needed.
mkdir -p "$tmp/bin" "$tmp/work with spaces"
cat > "$tmp/bin/docker" <<'SH'
#!/usr/bin/env bash
printf '%s\0' "$@" > "$DOCKER_ARGS"
exit "${DOCKER_EXIT:-0}"
SH
chmod +x "$tmp/bin/docker"
digest=ghcr.io/oriapp/rorthox-build@sha256:1111111111111111111111111111111111111111111111111111111111111111
(
    cd "$tmp/work with spaces"
    export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/argv" DOCKER_EXIT=37
    unset RORTHOX_TEST_ROOT RORTHOX_TEST_TIMEOUT
    export RORTHOX_IMAGE_REF="$digest" GITHUB_ACTIONS=true RORTHOX_REQUIRE_ISO=1
    export GITHUB_TOKEN=secret ACTIONS_RUNTIME_TOKEN=secret GITHUB_ENV="$tmp/secret"
    export GITHUB_OUTPUT="$tmp/secret" GITHUB_STEP_SUMMARY="$tmp/secret"
    export RORTHOX_ACCEL=tcg
    if bash "$OLDPWD/ci/run.sh" /bin/printf '%s' 'a b'; then fail 'exit status lost'; else
        [ "$?" -eq 37 ] || fail 'wrong docker exit status'
    fi
)
mapfile -d '' -t actual < "$tmp/argv"
expected=(run --rm --init --user "$(id -u):$(id -g)" --network none
          --cap-drop ALL --security-opt no-new-privileges --pids-limit 512
          --memory 4g -v "$tmp/work with spaces:/work" -w /work)
if [ -c /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then expected+=(--device /dev/kvm); fi
expected+=(-e GITHUB_ACTIONS=true -e RORTHOX_ACCEL=tcg -e RORTHOX_REQUIRE_ISO=1
           "$digest" /bin/printf '%s' 'a b')
[ "${#expected[@]}" = "${#actual[@]}" ] || fail "docker argv length ${#actual[@]}, expected ${#expected[@]}"
for i in "${!expected[@]}"; do
    [ "${actual[i]}" = "${expected[i]}" ] || fail "docker argv[$i]: ${actual[i]}"
done
if ( export PATH="$tmp/bin:$PATH" DOCKER_ARGS="$tmp/argv"; unset RORTHOX_IMAGE_REF; bash ci/run.sh true ) >/dev/null 2>&1; then
    fail 'placeholder digest accepted'
fi
echo 'PASS ci-image: key, context, reference, Dockerfile, isolated argv and exit status'
