#!/bin/bash
# build_fgls_dll.sh — Build pogls_fgls.so on Linux (for testing)
# Run from collection root
set -e

OUT_DIR=build
mkdir -p $OUT_DIR

echo "[1/2] Compiling pogls_fgls.so ..."
gcc -O2 -shared -fPIC \
    -DPOGLS_FGLS_EXPORT_DLL \
    -I. \
    -Icore \
    -Icore/core \
    -Icore/pogls_engine \
    -o $OUT_DIR/pogls_fgls.so pogls_fgls_export.c

echo "[2/2] Exports:"
nm -D $OUT_DIR/pogls_fgls.so | grep " T " | grep pogls_fgls

echo ""
echo "Done: $OUT_DIR/pogls_fgls.so"
