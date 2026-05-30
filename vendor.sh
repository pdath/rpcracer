#!/bin/sh
# Download vendored dependencies into include/
# Run this once before building, or on the remote machine

set -e

INCLUDE_DIR="include"
mkdir -p "$INCLUDE_DIR"

echo "Downloading vendored dependencies..."

# yyjson v0.12.0
printf "  yyjson.h... "
curl -sL "https://raw.githubusercontent.com/ibireme/yyjson/0.12.0/src/yyjson.h" -o "$INCLUDE_DIR/yyjson.h"
echo "OK"

printf "  yyjson.c... "
curl -sL "https://raw.githubusercontent.com/ibireme/yyjson/0.12.0/src/yyjson.c" -o "$INCLUDE_DIR/yyjson.c"
echo "OK"

# uthash v2.3.0
printf "  uthash.h... "
curl -sL "https://raw.githubusercontent.com/troydhanson/uthash/master/src/uthash.h" -o "$INCLUDE_DIR/uthash.h"
echo "OK"

echo "Done. All vendored files are in $INCLUDE_DIR/"
