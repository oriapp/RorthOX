#!/bin/sh
set -e

. ./config.sh

for folder in ./*/; do
    if [ -f "$folder/Makefile" ]; then
        ($MAKE clean)
    fi
done