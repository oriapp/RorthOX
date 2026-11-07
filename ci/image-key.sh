#!/usr/bin/env bash
# This sorted list is the complete Docker build context and the content-key input.
set -euo pipefail
cd "$(dirname "$0")/.."
inputs=(ci/Dockerfile configure.sh)
case "${1:-}" in
    '')
        for file in "${inputs[@]}"; do
            printf '%s  %s\n' "$(sha256sum "$file" | cut -d' ' -f1)" "$file"
        done | LC_ALL=C sort | sha256sum | cut -d' ' -f1
        ;;
    --inputs)
        printf '%s\n' "${inputs[@]}" | LC_ALL=C sort
        ;;
    --context)
        [ "$#" -eq 2 ] && [ -d "$2" ] || { echo 'usage: image-key.sh --context <empty-dir>' >&2; exit 2; }
        [ -z "$(ls -A "$2")" ] || { echo 'context directory is not empty' >&2; exit 2; }
        for file in "${inputs[@]}"; do
            mkdir -p "$2/$(dirname "$file")"
            cp "$file" "$2/$file"
        done
        ;;
    *) echo 'usage: image-key.sh [--inputs | --context <empty-dir>]' >&2; exit 2 ;;
esac
