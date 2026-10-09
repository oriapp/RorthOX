#!/bin/sh
# Remove all build outputs.
set -e
cd "$(dirname "$0")"
make clean
