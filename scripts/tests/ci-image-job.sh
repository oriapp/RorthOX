#!/usr/bin/env bash
# Host-only first-run, reuse, archive integrity and publication-order contract.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/ci-image-job
mkdir -p "$OUT"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL ci-image-job: $*" >&2; exit 1; }
mkdir -p "$tmp/bin" "$tmp/runner"
cat > "$tmp/bin/docker" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$DOCKER_LOG"
case "$1 ${2:-}" in
    'manifest inspect')
        if [ "${MANIFEST_PRESENT:-0}" = 1 ] || grep -q '^push ' "$DOCKER_LOG"; then
            if [ "${MANIFEST_INDEX:-0}" = 1 ]; then
                printf '[{"Descriptor":{"digest":"sha256:%064d"}}]\n' 6
            fi
            exit 0
        fi
        if [ "${MANIFEST_ERROR:-}" = network ]; then echo 'connection refused' >&2
        elif [ "${MANIFEST_ERROR:-}" = denied ]; then echo 'ghcr.io/oriapp/rorthox-build: denied' >&2
        else echo 'manifest unknown' >&2; fi
        exit 1 ;;
    'image inspect')
        if [ "${3:-}" = "sha256:$(printf '%064d' 8)" ]; then exit 1; fi
        case "${4:-}" in
            *.RepoDigests*) printf '["ghcr.io/oriapp/rorthox-build@sha256:%064d","ghcr.io/oriapp/rorthox-build:'"$(bash "$ROOT/ci/image-key.sh")"'@sha256:%064d"]\n' 7 7 ;;
            *input-hash*) if [ "${BAD_LABEL:-0}" = 1 ]; then echo bad; else bash "$ROOT/ci/image-key.sh"; fi ;;
            *revision*) printf '%s\n' "$GITHUB_SHA" ;;
            *Id*) printf 'sha256:%064d\n' 9 ;;
            *) exit 0 ;;
        esac ;;
    'save '*) echo 'saved candidate' ;;
    'load '*) cat >/dev/null ;;
    'run '*) [ -z "${GHCR_TOKEN:-}" ] || { echo 'token present during validation' >&2; exit 42; }
             echo 'versions available' ;;
    'login '*) cat >/dev/null ;;
    'push '*) if [ "${PUSH_DENIED:-0}" = 1 ]; then echo 'push denied' >&2; exit 1; fi
              if [ "${PUSH_NO_DIGEST:-0}" = 1 ]; then echo 'push completed without digest'; exit 0; fi
              printf 'key: digest: sha256:%064d size: 1234\n' 7 ;;
    *) : ;;
esac
SH
chmod +x "$tmp/bin/docker"
export PATH="$tmp/bin:$PATH" ROOT=$PWD DOCKER_LOG="$tmp/log"
export RUNNER_TEMP="$tmp/runner" GITHUB_OUTPUT="$tmp/output" GITHUB_SHA=1111111111111111111111111111111111111111
export GITHUB_ACTOR=oriapp GITHUB_STEP_SUMMARY="$tmp/summary"
id=sha256:$(printf '%064d' 9)
: > "$DOCKER_LOG"
MANIFEST_PRESENT=0 bash ci/image-job.sh prepare
[ "$(grep '^mode=' "$GITHUB_OUTPUT")" = 'mode=archive' ] || fail 'cold build did not produce archive'
[ -s "$RUNNER_TEMP/candidate-image.tar.gz" ] || fail 'missing archive'
grep -q '^build ' "$DOCKER_LOG" || fail 'missing cold build'
[ "$(grep '^id=' "$GITHUB_OUTPUT" | tail -n 1)" = "id=$id" ] || fail 'wrong image ID'
sha=$(grep '^checksum=' "$GITHUB_OUTPUT" | cut -d= -f2)
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
export IMAGE_ID=$id IMAGE_SHA256=$sha
bash ci/image-job.sh receive
[ "$(grep '^id=' "$GITHUB_OUTPUT")" = "id=$id" ] || fail 'unverified image ID'
[ "$(grep -c '^load$' "$DOCKER_LOG")" -eq 1 ] || fail 'archive not loaded once'
grep -q '^run ' "$DOCKER_LOG" || fail 'loaded image not validated'
! grep -q '^login ' "$DOCKER_LOG" || fail 'logged in during validation'
if IMAGE_ID="sha256:$(printf '%064d' 8)" bash ci/image-job.sh receive 2>/dev/null; then
    fail 'wrong image ID accepted'
fi
if BAD_LABEL=1 bash ci/image-job.sh receive 2>/dev/null; then
    fail 'wrong input label accepted'
fi
: > "$DOCKER_LOG"
printf 'damaged\n' >> "$RUNNER_TEMP/candidate-image.tar.gz"
if bash ci/image-job.sh receive 2>/dev/null; then fail 'bad sha accepted'; fi
[ ! -s "$DOCKER_LOG" ] || fail 'loaded corrupt archive'
: > "$GITHUB_OUTPUT"; : > "$DOCKER_LOG"
MANIFEST_PRESENT=1 MANIFEST_INDEX=1 bash ci/image-job.sh prepare
[ "$(grep '^mode=' "$GITHUB_OUTPUT")" = mode=digest ] || fail 'public image not reused'
[ "$(grep '^ref=' "$GITHUB_OUTPUT")" = "ref=ghcr.io/oriapp/rorthox-build@sha256:$(printf '%064d' 7)" ] || fail 'index digest not preserved'
if MANIFEST_PRESENT=1 BAD_LABEL=1 bash ci/image-job.sh prepare 2>/dev/null; then fail 'bad public label accepted'; fi
! grep -Eq '^build |^save ' "$DOCKER_LOG" || fail 'rebuilt public image'
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
export IMAGE_REF=ghcr.io/oriapp/rorthox-build@sha256:$(printf '%064d' 7)
bash ci/image-job.sh fetch
[ "$(grep '^id=' "$GITHUB_OUTPUT")" = "id=$id" ] || fail 'digest did not resolve by ID'
if BAD_LABEL=1 bash ci/image-job.sh fetch 2>/dev/null; then fail 'bad fetched label accepted'; fi
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
GHCR_TOKEN=dummy MANIFEST_ERROR=denied MANIFEST_PRESENT=0 bash ci/image-job.sh publish
[ "$(grep '^ref=' "$GITHUB_OUTPUT")" = "ref=$IMAGE_REF" ] || fail 'missing published digest'
python3 - "$DOCKER_LOG" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
assert not any(x.startswith('run ') for x in lines), 'validation happened after acquiring credentials'
assert next(i for i, x in enumerate(lines) if x.startswith('login ')) < next(i for i, x in enumerate(lines) if x.startswith('push '))
PY
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
GHCR_TOKEN=dummy MANIFEST_PRESENT=1 bash ci/image-job.sh publish
! grep -q '^push ' "$DOCKER_LOG" || fail 'overwrote existing tag'
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
GHCR_TOKEN=dummy MANIFEST_PRESENT=0 bash ci/image-job.sh publish
[ "$(grep '^ref=' "$GITHUB_OUTPUT")" = "ref=$IMAGE_REF" ] || fail 'manifest unknown blocked first push'
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
if GHCR_TOKEN=dummy MANIFEST_ERROR=denied PUSH_DENIED=1 MANIFEST_PRESENT=0 bash ci/image-job.sh publish 2>/dev/null; then
    fail 'real push denial hidden'
fi
grep -q 'denied' "$GITHUB_STEP_SUMMARY" || fail 'lookup error not visible in summary'
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
if GHCR_TOKEN=dummy MANIFEST_ERROR=network MANIFEST_PRESENT=0 bash ci/image-job.sh publish 2>/dev/null; then
    fail 'pushed after uncertain registry error'
fi
! grep -q '^push ' "$DOCKER_LOG" || fail 'pushed after registry failure'
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
if GHCR_TOKEN=dummy MANIFEST_PRESENT=0 PUSH_NO_DIGEST=1 bash ci/image-job.sh publish >"$tmp/no-digest.log" 2>&1; then
    fail 'successful push with no reported digest accepted'
fi
grep -q 'registry push did not report a digest' "$tmp/no-digest.log" || fail 'wrong missing-digest error'
[ ! -s "$GITHUB_OUTPUT" ] || fail 'missing digest was published'
echo 'PASS ci-image-job: cold build, reused digest, verified archive, publish order and no overwrite'
