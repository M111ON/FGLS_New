#!/bin/bash
# ──────────────────────────────────────────────────────────────────────
# colab_bandwidth_bench_opt.sh — Deploy + Run Optimized GPU Jet Puller Bandwidth Bench
# ──────────────────────────────────────────────────────────────────────
# Usage:
#   Step 1 — Compile static binary, upload to Colab via hermes colab
#   Step 2 — Run bench on T4, inspect results
#
# PREREQUISITES:
#   hermes colab installed
#   WSL distro 'Geomatt' running for cross-compile
# ──────────────────────────────────────────────────────────────────────

set -e

PROJECT_DIR="/i/FGLS_new"
BENCH_DIR="${PROJECT_DIR}/runner/gpu_jet_puller"
BENCH_SRC="${BENCH_DIR}/gpu_jet_puller_bench_opt.cu"
DEPLOY_DIR="/tmp/colab_bandwidth_opt"

echo "═══════════════════════════════════════════════════════"
echo "  GPU Jet Puller — Optimized Bandwidth Bench Deploy (Colab T4)"
echo "═══════════════════════════════════════════════════════"

# ── 1. Collect source files ──────────────────────────────────
echo "  ├─ Collecting source files..."
mkdir -p "${DEPLOY_DIR}"
cp "${BENCH_SRC}" "${DEPLOY_DIR}/"

# Copy needed headers
for h in \
  "${PROJECT_DIR}/collection/src/fibo_spine.h" \
  "${PROJECT_DIR}/collection/src/gear_lock.h" \
  "${PROJECT_DIR}/collection/rdh/rdh_addr.h" \
  "${PROJECT_DIR}/collection/core/core/geom_common.h" \
  "${PROJECT_DIR}/collection/core/core/geom_macros.h" \
  "${PROJECT_DIR}/collection/core/core/geom_base_types.h" \
  "${PROJECT_DIR}/collection/core/core/geom_platform.h"; do
    cp "$h" "${DEPLOY_DIR}/" 2>/dev/null || echo "  ! WARN: $h not found (optional)"
done

# Create flat include structure matching bench's -I flags
mkdir -p "${DEPLOY_DIR}/collection/src"
mkdir -p "${DEPLOY_DIR}/collection/rdh"
mkdir -p "${DEPLOY_DIR}/collection/core/core"

cp "${PROJECT_DIR}/collection/src/fibo_spine.h" "${DEPLOY_DIR}/collection/src/"
cp "${PROJECT_DIR}/collection/src/gear_lock.h" "${DEPLOY_DIR}/collection/src/"
cp "${PROJECT_DIR}/collection/rdh/rdh_addr.h" "${DEPLOY_DIR}/collection/rdh/"

for h in \
  geom_common.h geom_macros.h geom_base_types.h geom_platform.h; do
  find "${PROJECT_DIR}" -name "$h" -exec cp {} "${DEPLOY_DIR}/collection/core/core/" \; 2>/dev/null || true
done

# ── 2. Create Colab runner ───────────────────────────────────
echo "  ├─ Writing Colab runner..."
cat > "${DEPLOY_DIR}/run_bench.py" << 'PYEOF'
import subprocess, sys, os, json, time

# ── 1. Compile ──
print("Compiling optimized benchmark...")
result = subprocess.run([
    "nvcc", "-O3", "-std=c++17", "-arch=sm_75",
    "-I.", "-Icollection/src", "-Icollection/rdh", "-Icollection/core/core",
    "-o", "gpu_jet_puller_bench_opt",
    "gpu_jet_puller_bench_opt.cu",
    "-lm"
], capture_output=True, text=True, timeout=300)

if result.returncode != 0:
    print("COMPILE ERROR:")
    print(result.stderr)
    sys.exit(1)
print("  Compile OK")

# ── 2. Check GPU ──
result = subprocess.run(["nvidia-smi", "--query-gpu=name,memory.total", "--format=csv,noheader"],
                       capture_output=True, text=True)
print(f"  GPU: {result.stdout.strip()}")

# ── 3. Run benchmark ──
print("Running optimized bandwidth benchmark...")
result = subprocess.run(["./gpu_jet_puller_bench_opt"], capture_output=True, text=True, timeout=600)

print("\n" + "="*60)
print("BENCHMARK OUTPUT")
print("="*60)
print(result.stdout)
if result.stderr:
    print("STDERR:")
    print(result.stderr)

# ── 4. Parse results ──
print("\n" + "="*60)
print("SUMMARY (64B / 256B / 1024B — with and without XOR)")
print("="*60)
for line in result.stdout.split('\n'):
    if 'Throughput' in line or 'CONFIG' in line or 'optimal' in line.lower() or 'Results' in line or '║' in line:
        print(f"  {line.strip()}")
PYEOF

# ── 3. Create tar.gz ─────────────────────────────────────────
echo "  ├─ Creating tarball..."
cd "${DEPLOY_DIR}"
tar czf /tmp/gpu-jet-bandwidth-opt.tar.gz \
  gpu_jet_puller_bench_opt.cu \
  collection/ \
  run_bench.py
SIZE=$(stat -c%s /tmp/gpu-jet-bandwidth-opt.tar.gz 2>/dev/null || echo "?")
echo "  └─ Created: /tmp/gpu-jet-bandwidth-opt.tar.gz (${SIZE} bytes)"

# ── 4. Upload to Colab ───────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════════"
echo "  NEXT STEPS:"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "  hermes colab new -s bandwidth-opt --gpu T4"
echo "  hermes colab upload -s bandwidth-opt /tmp/gpu-jet-bandwidth-opt.tar.gz /content/"
echo ""
echo "  Then in Colab:"
echo "    cd /content && tar xzf gpu-jet-bandwidth-opt.tar.gz"
echo "    python3 run_bench.py"
echo ""
echo "═══════════════════════════════════════════════════════"