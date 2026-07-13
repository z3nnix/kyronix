#!/bin/sh
set -eu

SRC_DIR="$1"
PREFIX="${4:-/usr/local}"
BUILD_DIR="/tmp/tcc-build"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cp -R "$SRC_DIR"/tcc-* ./
TCC_DIR="$(find . -maxdepth 1 -type d -name 'tcc-*' | head -n 1)"
[ -n "$TCC_DIR" ]
cd "$TCC_DIR"

./configure --prefix="$PREFIX"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
make install
