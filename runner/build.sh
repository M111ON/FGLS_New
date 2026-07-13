#!/usr/bin/env bash
#
# build.sh — POGLS Toolchain build script (Linux/WSL)
# Usage:  ./build.sh [all|core|tools|modules|test|clean]
#
# Cross-platform equivalent of the Makefile targets.
# Auto-detects OS: on Linux builds without .exe, links against .so.
# On Windows/MinGW falls back to mingw32-make.

set -euo pipefail
cd "$(dirname "$0")"

OS="$(uname -s 2>/dev/null || echo Windows)"
if [ "$OS" = "Linux" ]; then
    EXE=""
    DLLEXT=".so"
    ZSTD="-lzstd"
    LDLIBS_EXTRA="-lpthread -ldl"
    NULLDEV="/dev/null"
    RM="rm -f"
else
    # Windows/MinGW — delegate to mingw32-make
    MAKE="$(command -v mingw32-make 2>/dev/null || command -v make 2>/dev/null || echo make)"
    exec "$MAKE" "$@"
fi

# Default target
TARGET="${1:-all}"

# Flags (mirrors Makefile)
LLAMA_DIR="${LLAMA_DIR:-/usr/local/llama.cpp}"
INCS="-I. -I../collection -I../collection/src -I../collection/core"
INCS="$INCS -I../collection/core/core -I../collection/geopixel"
INCS="$INCS -I../collection/geopixel/Metatron/core -I../collection/pogls_engine"
INCS="$INCS -I../collection/geo_jump_module/include"
INCS="$INCS -I../collection/geopixel/hbv_bundle/Diamond_shell_encoder"
INCS="$INCS -I../collection/geopixel/hbv_bundle/Diamond_decode_hamburger"
INCS="$INCS -I../collection/geopixel/hbv_bundle/core"
INCS="$INCS -I../collection/Hfolder -I../collection/rdh"
INCS="$INCS -I$LLAMA_DIR/include -I$LLAMA_DIR/ggml/include"
INCS="$INCS -I$LLAMA_DIR/src"
INCS="$INCS -Ipogls_core -Ipogls_gguf -Ipogls_geo -Ipogls_dram -Ipogls_bermuda -Ipogls_kv -Ipogls_geopixel"
CFLAGS="-O2 -std=c11 -fno-strict-aliasing $INCS"
LDLIBS="-lm $LDLIBS_EXTRA"

# Core objects
POGLS_CORE_OBJS="pogls_core/pogls_platform.o pogls_core/pogls_compress.o pogls_core/pogls_addr.o pogls_core/pogls_meta.o"

echo "=== POGLS Toolchain build.sh (Linux) ==="
echo "Target: $TARGET"
echo "LLAMA_DIR: $LLAMA_DIR"
echo ""

build_core() {
    echo "-- Core Library --"
    for obj in $POGLS_CORE_OBJS; do
        base="$(basename "$obj" .o)"
        dir="$(dirname "$obj")"
        src="$dir/$base.c"
        extra=""
        [ "$base" = "pogls_compress" ] && extra="-DPOGLS_USE_ZSTD"
        echo "  CC $src"
        gcc $CFLAGS $extra -c -o "$obj" "$src"
    done
    ar rcs pogls_core.lib $POGLS_CORE_OBJS
    echo "  -> pogls_core.lib"
}

build_modules() {
    echo "-- Modules --"
    for mod in "pogls_gguf/pogls_gguf" "pogls_geo/pogls_geo" \
               "pogls_dram/pogls_dram" "pogls_bermuda/pogls_bermuda" \
               "pogls_kv/pogls_kv" "pogls_geopixel/pogls_geopixel"; do
        echo "  CC $mod.c"
        gcc $CFLAGS -c -o "$mod.o" "$mod.c"
    done
}

build_tools() {
    echo "-- CLI Tools --"
    build_core

    # Phase 1: Core CLI
    echo "  CC pogls_tools/pogls_compress.c"
    gcc $CFLAGS -DPOGLS_USE_ZSTD -o pogls_compress$EXE pogls_tools/pogls_compress.c -L. -lpogls_core $ZSTD $LDLIBS

    echo "  CC pogls_tools/pogls_decompress.c"
    gcc $CFLAGS -DPOGLS_USE_ZSTD -o pogls_decompress$EXE pogls_tools/pogls_decompress.c -L. -lpogls_core $ZSTD $LDLIBS

    echo "  CC pogls_tools/pogls_inspect.c"
    gcc $CFLAGS -o pogls_inspect$EXE pogls_tools/pogls_inspect.c -L. -lpogls_core $LDLIBS

    echo "  CC pogls_tools/pogls_roundtrip.c"
    gcc $CFLAGS -DPOGLS_USE_ZSTD -o pogls_roundtrip$EXE pogls_tools/pogls_roundtrip.c -L. -lpogls_core $ZSTD $LDLIBS

    # Phase 2: Address + GGUF
    echo "  CC pogls_tools/addr_resolve.c"
    gcc $CFLAGS -o addr_resolve$EXE pogls_tools/addr_resolve.c -L. -lpogls_core $LDLIBS

    echo "  CC gguf_dump.c"
    gcc $CFLAGS -o gguf_dump$EXE pogls_tools/gguf_dump.c $LDLIBS

    # Phase 3: Store + Delta
    echo "  CC pogls_tools/dramtile_dump.c"
    gcc $CFLAGS -o dramtile_dump$EXE pogls_tools/dramtile_dump.c $LDLIBS

    echo "  CC pogls_tools/dramtile_bench.c"
    gcc $CFLAGS -o dramtile_bench$EXE pogls_tools/dramtile_bench.c $LDLIBS

    echo "  CC pogls_tools/dramtile_cli.c"
    gcc $CFLAGS -o dramtile_cli$EXE pogls_tools/dramtile_cli.c -L. -lpogls_core $LDLIBS

    echo "  CC pogls_tools/kv_delta_bench.c"
    gcc $CFLAGS -o kv_delta_bench$EXE pogls_tools/kv_delta_bench.c $LDLIBS

    echo "  CC pogls_tools/kv_delta_test.c"
    gcc $CFLAGS -o kv_delta_test$EXE pogls_tools/kv_delta_test.c $LDLIBS

    # Phase 4: Pipeline
    echo "  CC pogls_tools/pogls_verify.c"
    gcc $CFLAGS -o pogls_verify$EXE pogls_tools/pogls_verify.c -L. -lpogls_core $LDLIBS

    echo "  CC pogls_tools/pogls_build.c"
    gcc $CFLAGS -o pogls_build$EXE pogls_tools/pogls_build.c -L. -lpogls_core $LDLIBS

    echo "  CC pogls_tools/pogls_cat.c"
    gcc $CFLAGS -o pogls_cat$EXE pogls_tools/pogls_cat.c -L. -lpogls_core $LDLIBS

    echo "  CC pogls_tools/pogls_diff.c"
    gcc $CFLAGS -o pogls_diff$EXE pogls_tools/pogls_diff.c -L. -lpogls_core $LDLIBS

    echo "  CC pogls_tools/pogls_test.c"
    gcc $CFLAGS -DPOGLS_USE_ZSTD -o pogls_test$EXE pogls_tools/pogls_test.c -L. -lpogls_core $ZSTD $LDLIBS

    # POGLS Loader
    echo "  CC pogls_tools/test_pogls_loader.c"
    gcc $CFLAGS -DPOGLS_LOADER_IMPLEMENTATION -o test_pogls_loader$EXE pogls_tools/test_pogls_loader.c $LDLIBS

    echo "  CC pogls_tools/pogls_loader_demo.c"
    gcc $CFLAGS -DPOGLS_LOADER_IMPLEMENTATION -o pogls_loader_demo$EXE pogls_tools/pogls_loader_demo.c $LDLIBS

    echo ""
    echo "Tools built:"
    for t in pogls_compress pogls_decompress pogls_inspect pogls_roundtrip \
             addr_resolve gguf_dump \
             dramtile_dump dramtile_bench dramtile_cli \
             kv_delta_bench kv_delta_test \
             pogls_verify pogls_build pogls_cat pogls_diff pogls_test \
             test_pogls_loader pogls_loader_demo; do
        if [ -f "$t$EXE" ]; then
            sz=$(stat -c%s "$t$EXE" 2>/dev/null || stat -f%z "$t$EXE" 2>/dev/null)
            printf "  %-25s %s\n" "$t$EXE" "$(numfmt --to=iec $sz 2>/dev/null || echo "${sz}B")"
        fi
    done
}

run_test() {
    echo "=== Running toolchain tests ==="
    build_tools

    echo ""
    echo "[1/7] core tests:"
    ./pogls_test$EXE && echo "  PASS" || echo "  FAIL"

    echo "[2/7] roundtrip:"
    ./pogls_roundtrip$EXE pogls_core/pogls_platform.h && echo "  PASS" || echo "  FAIL"

    echo "[3/7] verify:"
    ./pogls_verify$EXE ../test_verify.pogls && echo "  PASS" || echo "  FAIL"

    echo "[4/7] cat:"
    ./pogls_cat$EXE _test_cat.bin pogls_core/pogls_platform.h pogls_core/pogls_compress.h && echo "  PASS" || echo "  FAIL"

    echo "[5/7] diff identical:"
    ./pogls_diff$EXE pogls_core/pogls_platform.h pogls_core/pogls_platform.h && echo "  PASS" || echo "  FAIL"

    echo "[6/7] addr_resolve:"
    ./addr_resolve$EXE --name "blk.0.attn_q.weight" >$NULLDEV 2>&1 && echo "  PASS" || echo "  FAIL"

    echo "[7/7] kv_delta_test:"
    ./kv_delta_test$EXE 2>&1 | grep -q "ALL PASS" && echo "  PASS" || echo "  FAIL"

    $RM _test_cat.bin 2>$NULLDEV
    echo "=== Done ==="
}

case "$TARGET" in
    all)
        build_core
        build_modules
        build_tools
        ;;
    core) build_core ;;
    modules) build_modules ;;
    tools) build_tools ;;
    test) run_test ;;
    clean)
        echo "-- Clean --"
        $RM *.o pogls_core/*.o pogls_tools/*.o
        $RM pogls_gguf/*.o pogls_geo/*.o pogls_dram/*.o
        $RM pogls_bermuda/*.o pogls_kv/*.o pogls_geopixel/*.o
        $RM pogls_core.lib *.a
        $RM pogls_compress$EXE pogls_decompress$EXE pogls_inspect$EXE
        $RM pogls_roundtrip$EXE addr_resolve$EXE gguf_dump$EXE
        $RM dramtile_dump$EXE dramtile_bench$EXE dramtile_cli$EXE
        $RM kv_delta_bench$EXE kv_delta_test$EXE
        $RM pogls_verify$EXE pogls_build$EXE pogls_cat$EXE pogls_diff$EXE
        $RM pogls_test$EXE test_pogls_loader$EXE pogls_loader_demo$EXE
        $RM _test_core$EXE _test_gguf$EXE _test_geo$EXE
        $RM _test_dram$EXE _test_bermuda$EXE _test_kv$EXE _test_geopixel$EXE
        $RM test_pogls_core$EXE
        echo "  done"
        ;;
    *)
        echo "Usage: $0 [all|core|modules|tools|test|clean]"
        exit 1
        ;;
esac
