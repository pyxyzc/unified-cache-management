#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build and run the manual real-environment MR 2MiB registration probe.
#
# Prerequisite:
#   ./scripts/build.sh
#
# Example:
#   bash scripts/test_mr_2m_real_env.sh --device-id 0
#   bash scripts/test_mr_2m_real_env.sh --device-id 0 --expect-small fail

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
SRC="$REPO_ROOT/tests/kv/test_mr_2m_real_env.cpp"
OUT="${OUT:-$BUILD_DIR/tests/kv/test_mr_2m_real_env}"
CXX="${CXX:-g++}"

if [ ! -e "$BUILD_DIR/libumc.so" ] && [ ! -e "$BUILD_DIR/libumc.a" ]; then
    echo "[mr-2m-real] missing $BUILD_DIR/libumc.so or libumc.a" >&2
    echo "[mr-2m-real] run ./scripts/build.sh first, or set BUILD_DIR=/path/to/build" >&2
    exit 2
fi

mkdir -p "$(dirname "$OUT")"

"$CXX" -std=c++17 -O0 -g -Wall -Wextra -pedantic \
    -I"$REPO_ROOT/include" \
    "$SRC" \
    -L"$BUILD_DIR" -lumc \
    -Wl,-rpath,"$BUILD_DIR" \
    -ldl -pthread \
    -o "$OUT"

LD_LIBRARY_PATH="$BUILD_DIR:${LD_LIBRARY_PATH:-}" "$OUT" "$@"
