#!/usr/bin/env bash
# Prepare a read-only candidate image, or publish a validated candidate on trusted pushes.
set -euo pipefail
cd "$(dirname "$0")/.."
image=ghcr.io/oriapp/rorthox-build
key=$(bash ci/image-key.sh)
tag=$image:$key
out=${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}
archive=${RUNNER_TEMP:?RUNNER_TEMP is required}/candidate-image.tar.gz
fail() { echo "image: $*" >&2; exit 1; }
record() { printf '%s=%s\n' "$1" "$2" >> "$out"; }
# docker manifest inspect is anonymous before login; never assume a tag is a digest.
public_digest() {
    local manifest
    manifest=$(docker manifest inspect --verbose "$tag" 2>/dev/null) || return 1
    printf '%s' "$manifest" | python3 -c '
import json, re, sys
try:
    data = json.load(sys.stdin)
    digest = data["Descriptor"]["digest"]
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest): raise ValueError("digest")
    print(digest)
except (KeyError, ValueError, TypeError): sys.exit(1)
'
}
# Run only non-secret commands before acquiring registry write credentials.
validate() {
    local id=$1 hash revision
    hash=$(docker image inspect --format '{{index .Config.Labels "org.rorthox.image.input-hash"}}' "$id")
    [ "$hash" = "$key" ] || fail 'input hash label mismatch'
    revision=$(docker image inspect --format '{{index .Config.Labels "org.opencontainers.image.revision"}}' "$id")
    [ "$revision" = "$GITHUB_SHA" ] || fail 'revision label mismatch'
    docker run --rm --network none "$id" bash -ec '
        i686-elf-gcc --version | head -n 1
        x86_64-elf-gcc --version | head -n 1
        nasm -v
        qemu-system-i386 --version | head -n 1
        grub-mkrescue --version
        xorriso -version | head -n 1
        python3 --version
    '
}
case "${1:-}" in
    prepare)
        record key "$key"
        if digest=$(public_digest); then
            ref=$image@$digest
            docker pull "$ref"
            hash=$(docker image inspect --format '{{index .Config.Labels "org.rorthox.image.input-hash"}}' "$ref")
            [ "$hash" = "$key" ] || fail 'published image has the wrong input hash'
            record mode digest
            record ref "$ref"
            exit 0
        fi
        context=$(mktemp -d)
        trap 'rm -rf "$context"' EXIT
        bash ci/image-key.sh --context "$context"
        docker build --build-arg "RORTHOX_REVISION=$GITHUB_SHA" \
            --build-arg "RORTHOX_INPUT_HASH=$key" -t "$tag" \
            -f "$context/ci/Dockerfile" "$context"
        id=$(docker image inspect --format '{{.Id}}' "$tag")
        [[ "$id" =~ ^sha256:[0-9a-f]{64}$ ]] || fail 'invalid image ID'
        validate "$id"
        docker save "$id" | gzip -1 > "$archive"
        record mode archive
        record id "$id"
        record checksum "$(sha256sum "$archive" | cut -d' ' -f1)"
        ;;
    fetch)
        ref=${IMAGE_REF:?IMAGE_REF is required}
        [[ "$ref" =~ ^ghcr\.io/oriapp/rorthox-build@sha256:[0-9a-f]{64}$ ]] || fail 'invalid digest reference'
        docker pull "$ref"
        id=$(docker image inspect --format '{{.Id}}' "$ref")
        [[ "$id" =~ ^sha256:[0-9a-f]{64}$ ]] || fail 'invalid image ID'
        hash=$(docker image inspect --format '{{index .Config.Labels "org.rorthox.image.input-hash"}}' "$id")
        [ "$hash" = "$key" ] || fail 'digest input hash mismatch'
        record id "$id"
        ;;
    receive)
        expected=${IMAGE_SHA256:?IMAGE_SHA256 is required}
        id=${IMAGE_ID:?IMAGE_ID is required}
        [[ "$expected" =~ ^[0-9a-f]{64}$ && "$id" =~ ^sha256:[0-9a-f]{64}$ ]] || fail 'invalid archive identity'
        printf '%s  %s\n' "$expected" "$archive" | sha256sum --check --status || fail 'archive checksum mismatch'
        gzip -dc "$archive" | docker load >/dev/null
        docker image inspect "$id" >/dev/null || fail 'image ID missing from archive'
        hash=$(docker image inspect --format '{{index .Config.Labels "org.rorthox.image.input-hash"}}' "$id")
        [ "$hash" = "$key" ] || fail 'archive input hash mismatch'
        record id "$id"
        ;;
    publish)
        # A concurrent push of the same key is serialized by the workflow concurrency group.
        id=${IMAGE_ID:?IMAGE_ID is required}
        [[ "$id" =~ ^sha256:[0-9a-f]{64}$ ]] || fail 'invalid image ID'
        validate "$id"
        printf '%s' "${GHCR_TOKEN:?GHCR_TOKEN is required}" | docker login ghcr.io -u "${GITHUB_ACTOR:?}" --password-stdin >/dev/null
        # Never overwrite an existing content tag, including one hidden from anonymous users.
        error=$(mktemp)
        trap 'rm -f "$error"' EXIT
        if docker manifest inspect "$tag" >/dev/null 2>"$error"; then
            echo 'image: existing tag; publication skipped'
        elif grep -Eiq 'manifest unknown|no such manifest' "$error"; then
            docker tag "$id" "$tag"
            docker push "$tag"
        else
            fail 'registry lookup failed; refusing to overwrite an unverified tag'
        fi
        digest=$(public_digest) || fail 'published digest unavailable'
        record ref "$image@$digest"
        ;;
    *) fail 'usage: image-job.sh prepare|fetch|receive|publish' ;;
esac
