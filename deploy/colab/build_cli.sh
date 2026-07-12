#!/bin/bash
# build_cli.sh — Build POGLS CLI tools on Colab/Kaggle (Linux)
# Called by build_runner.sh after the main runner is built.
# Requires: LLAMA_DIR and LLAMA_BUILD_DIR to be set.
set -euo pipefail

WORK_DIR="${WORK_DIR:-/content/FGLS_new_colab}"
LLAMA_DIR="${LLAMA_DIR:-/content/llama.cpp}"
LLAMA_BUILD_DIR="${LLAMA_BUILD_DIR:-$LLAMA_DIR/build}"
BUILD_DIR="${BUILD_DIR:-/content/build}"

cd "$WORK_DIR"

INCS=(
    -I"$WORK_DIR"
    -I"$WORK_DIR/runner"
    -I"$WORK_DIR/collection"
    -I"$WORK_DIR/collection/src"
    -I"$WORK_DIR/collection/core"
    -I"$WORK_DIR/collection/core/core"
    -I"$WORK_DIR/collection/geopixel"
    -I"$WORK_DIR/collection/geopixel/Metatron/core"
    -I"$WORK_DIR/collection/pogls_engine"
    -I"$WORK_DIR/collection/geo_jump_module/include"
    -I"$WORK_DIR/collection/dgls/diamond/include"
    -I"$WORK_DIR/collection/Hfolder"
    -I"$WORK_DIR/collection/rdh"
    -I"$WORK_DIR/runner/pogls_core"
    -I"$WORK_DIR/runner/pogls_gguf"
    -I"$WORK_DIR/runner/pogls_geo"
    -I"$WORK_DIR/runner/pogls_dram"
    -I"$WORK_DIR/runner/pogls_bermuda"
    -I"$WORK_DIR/runner/pogls_kv"
    -I"$WORK_DIR/runner/pogls_geopixel"
    -I"$LLAMA_DIR"
    -I"$LLAMA_DIR/ggml/include"
    -I"$LLAMA_DIR/src"
)

CFLAGS="-O2 -std=c11 -D_GNU_SOURCE -D_POSIX_C_SOURCE=199309L -fno-strict-aliasing ${INCS[*]}"
LDLIBS="-lm -lpthread -ldl"
ZSTD_LIB="-lzstd"

echo ""
echo "-- POGLS CLI Tools Build --"

# Stage 1: Build pogls_core objects into BUILD_DIR
CORE_SRC="$WORK_DIR/runner/pogls_core"
echo "  CC core objects"
gcc $CFLAGS -c -o "$BUILD_DIR/pogls_platform.o" "$CORE_SRC/pogls_platform.c"
gcc $CFLAGS -DPOGLS_USE_ZSTD -c -o "$BUILD_DIR/pogls_compress.o" "$CORE_SRC/pogls_compress.c"
gcc $CFLAGS -c -o "$BUILD_DIR/pogls_addr.o" "$CORE_SRC/pogls_addr.c"
gcc $CFLAGS -c -o "$BUILD_DIR/pogls_meta.o" "$CORE_SRC/pogls_meta.c"
(cd "$BUILD_DIR" && ar rcs libpogls_core.a pogls_platform.o pogls_compress.o pogls_addr.o pogls_meta.o)
echo "  -> $BUILD_DIR/libpogls_core.a"

# Stage 2: Build CLI tools
CLI_SRC="$WORK_DIR/runner/pogls_tools"
cd "$BUILD_DIR"

build_tool() {
    local name="$1"
    local src="$2"
    shift 2
    echo "  CC $name"
    gcc $CFLAGS "$@" -o "$BUILD_DIR/$name" "$src" -L"$BUILD_DIR" -lpogls_core $ZSTD_LIB $LDLIBS 2>&1
}

build_tool pogls_compress "$CLI_SRC/pogls_compress.c"
build_tool pogls_decompress "$CLI_SRC/pogls_decompress.c"
build_tool pogls_inspect "$CLI_SRC/pogls_inspect.c"
build_tool pogls_roundtrip "$CLI_SRC/pogls_roundtrip.c"
build_tool addr_resolve "$CLI_SRC/addr_resolve.c"
build_tool gguf_dump "$CLI_SRC/gguf_dump.c" -L"$LLAMA_BUILD_DIR/bin" -lllama
build_tool dramtile_dump "$CLI_SRC/dramtile_dump.c"
build_tool dramtile_bench "$CLI_SRC/dramtile_bench.c"
build_tool dramtile_cli "$CLI_SRC/dramtile_cli.c"
build_tool kv_delta_bench "$CLI_SRC/kv_delta_bench.c"
build_tool kv_delta_test "$CLI_SRC/kv_delta_test.c" -L"$LLAMA_BUILD_DIR/bin" -lllama
build_tool pogls_verify "$CLI_SRC/pogls_verify.c"
build_tool pogls_build "$CLI_SRC/pogls_build.c"
build_tool pogls_cat "$CLI_SRC/pogls_cat.c"
build_tool pogls_diff "$CLI_SRC/pogls_diff.c"
build_tool pogls_test "$CLI_SRC/pogls_test.c"
# Also copy back to runner/ dir for pack consistency
for t in pogls_compress pogls_decompress pogls_inspect pogls_roundtrip \
         addr_resolve gguf_dump pogls_verify pogls_build pogls_cat pogls_diff pogls_test; do
    if [ -f "$BUILD_DIR/$t" ]; then
        cp "$BUILD_DIR/$t" "$WORK_DIR/runner/$t" 2>/dev/null || true
    fi
done
echo ""
echo "CLI tools: $(ls -1 $BUILD_DIR/pogls_* $BUILD_DIR/addr_resolve $BUILD_DIR/gguf_dump 2>/dev/null | wc -l) binaries"
