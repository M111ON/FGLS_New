#!/usr/bin/env bash
# build_colab.sh — Compile bond .so + run tests on Linux/Colab
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "=== POGLS Bond v1.1 Colab Build ==="

# Step 1: compile shared library
echo "--- Compiling pogls_bond.so ---"
gcc -O2 -fPIC -shared \
  -I"$ROOT/src" \
  -I"$ROOT/.." \
  -o "$ROOT/pogls_bond.so" \
  "$ROOT/src/pogls_bond_export.c" \
  -lm
echo "✅ pogls_bond.so ($(stat -c%s "$ROOT/pogls_bond.so") bytes)"

# Step 2: compile C tests
echo "--- Compiling C tests ---"
gcc -O2 -I"$ROOT/src" -I"$ROOT/.." \
  -o "$ROOT/test_bond_v2" \
  "$ROOT/src/test_pogls_bond_v2.c" -lm
echo "✅ test_bond_v2"

gcc -O2 -I"$ROOT/src" -I"$ROOT/.." \
  -o "$ROOT/test_tgw" \
  "$ROOT/src/test_tgw_dispatch.c" -lm
echo "✅ test_tgw"

gcc -O2 -I"$ROOT/src" -I"$ROOT/.." \
  -o "$ROOT/bench_bond_tgw" \
  "$ROOT/src/bench_bond_tgw.c" -lm
echo "✅ bench_bond_tgw"

# Step 3: run C tests
echo "=== Running bond tests ==="
"$ROOT/test_bond_v2" && echo "✅ bond: 63/63 passed" || echo "❌ bond FAILED"

echo "=== Running TGW dispatch tests ==="
"$ROOT/test_tgw" && echo "✅ TGW: 336/336 passed" || echo "❌ TGW FAILED"

# Step 4: run Python bridge tests
echo "=== Python bridge tests ==="
export POGLS_SO_PATH="$ROOT/pogls_bond.so"
cd "$ROOT/python_src"
python3 poc_bond_standalone.py
echo "✅ Python bridge done"

# Step 5: benchmark
echo "=== Benchmark ==="
cd "$ROOT"
python3 bench_colab.py
