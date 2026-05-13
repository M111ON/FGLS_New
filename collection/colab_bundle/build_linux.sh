#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT_DIR/build/linux"

mkdir -p "$BIN_DIR"

CFLAGS=(
  -O2
  -Wall
  -Wextra
  -Isrc
  -Icore/core
  -Icore/geo_headers
  -Icore/pogls_engine
  -Igeopixel
  -Iactive_updates
)

build_test() {
  local src="$1"
  local out="$2"
  echo "Compiling $src"
  gcc "${CFLAGS[@]}" "$ROOT_DIR/$src" -o "$BIN_DIR/$out"
}

build_test "tests/integration/test_geopixel_session_feed.c" "test_geopixel_session_feed"
build_test "tests/integration/test_startup_trace_payload.c" "test_startup_trace_payload"

echo "Running test_geopixel_session_feed"
"$BIN_DIR/test_geopixel_session_feed"

echo "Running test_startup_trace_payload"
"$BIN_DIR/test_startup_trace_payload"

echo "Linux bundle build complete."

