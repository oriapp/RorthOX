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
            if [ "${3:-}" = --verbose ]; then
                printf '{"Descriptor":{"digest":"sha256:%064d"}}\n' 7
            fi
            exit 0
        fi
        if [ "${MANIFEST_ERROR:-}" = network ]; then echo 'connection refused' >&2
        else echo 'manifest unknown' >&2; fi
        exit 1 ;;
    'image inspect')
        if [ "${3:-}" = "sha256:$(printf '%064d' 8)" ]; then exit 1; fi
        case "${4:-}" in
            *input-hash*) if [ "${BAD_LABEL:-0}" = 1 ]; then echo bad; else bash "$ROOT/ci/image-key.sh"; fi ;;
            *revision*) printf '%s\n' "$GITHUB_SHA" ;;
            *Id*) printf 'sha256:%064d\n' 9 ;;
            *) exit 0 ;;
        esac ;;
    'save '*) echo 'saved candidate' ;;
    'load '*) cat >/dev/null ;;
    'run '*) echo 'versions available' ;;
    'login '*) cat >/dev/null ;;
    *) : ;;
esac
SH
chmod +x "$tmp/bin/docker"
export PATH="$tmp/bin:$PATH" ROOT=$PWD DOCKER_LOG="$tmp/log"
export RUNNER_TEMP="$tmp/runner" GITHUB_OUTPUT="$tmp/output" GITHUB_SHA=1111111111111111111111111111111111111111
export GITHUB_ACTOR=oriapp GHCR_TOKEN=dummy
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
MANIFEST_PRESENT=1 bash ci/image-job.sh prepare
[ "$(grep '^mode=' "$GITHUB_OUTPUT")" = mode=digest ] || fail 'public image not reused'
! grep -Eq '^build |^save ' "$DOCKER_LOG" || fail 'rebuilt public image'
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
export IMAGE_REF=ghcr.io/oriapp/rorthox-build@sha256:$(printf '%064d' 7)
bash ci/image-job.sh fetch
[ "$(grep '^id=' "$GITHUB_OUTPUT")" = "id=$id" ] || fail 'digest did not resolve by ID'
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
MANIFEST_PRESENT=0 bash ci/image-job.sh publish
[ "$(grep '^ref=' "$GITHUB_OUTPUT")" = "ref=$IMAGE_REF" ] || fail 'missing published digest'
python3 - "$DOCKER_LOG" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
assert next(i for i, x in enumerate(lines) if x.startswith('run ')) < next(i for i, x in enumerate(lines) if x.startswith('login '))
assert next(i for i, x in enumerate(lines) if x.startswith('login ')) < next(i for i, x in enumerate(lines) if x.startswith('push '))
PY
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
MANIFEST_PRESENT=1 bash ci/image-job.sh publish
! grep -q '^push ' "$DOCKER_LOG" || fail 'overwrote existing tag'
: > "$DOCKER_LOG"; : > "$GITHUB_OUTPUT"
if MANIFEST_ERROR=network MANIFEST_PRESENT=0 bash ci/image-job.sh publish 2>/dev/null; then
    fail 'pushed after uncertain registry error'
fi
! grep -q '^push ' "$DOCKER_LOG" || fail 'pushed after registry failure'
echo 'PASS ci-image-job: cold build, reused digest, verified archive, publish order and no overwrite'
