"""
bench_colab.py — POGLS Bond + TGW performance on Colab T4
Measures:
  - fibo_addr throughput (ops/s)
  - bond_verify throughput
  - TGW dispatch throughput
  - Python↔C call latency
  - GPU vs CPU (if CUDA available)
"""

import sys, os, time, json, platform
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "python_src"))

print("="*60)
print("POGLS Bond Performance Bench — Colab T4")
print(f"Host: {platform.node()} | Python: {platform.python_version()}")
print("="*60)

# detect GPU
try:
    import subprocess
    nvidia = subprocess.run(["nvidia-smi", "--query-gpu=name,memory.total,compute_cap",
                             "--format=csv,noheader"], capture_output=True, text=True)
    if nvidia.returncode == 0:
        print(f"GPU: {nvidia.stdout.strip()}")
    else:
        print("GPU: nvidia-smi not available")
except:
    print("GPU: not detected")

print()

# ── bond init ──────────────────────────────────────────────────────
from pogls_bridge import PoglsBridge, SHAPE_I, SHAPE_O, SHAPE_T, SHAPE_S, SHAPE_Z

br = PoglsBridge(so_path=os.environ.get("POGLS_SO_PATH"))
print(f"Library: {br._path}")

# ── 1. fibo_addr throughput ───────────────────────────────────────
N = 200000
seeds = [i * 0x9E3779B97F4A7C15 for i in range(N)]

t0 = time.perf_counter()
for s in seeds:
    br.fibo_addr(s)
t1 = time.perf_counter()
addr_s = N / (t1 - t0)
print(f"\n1. fibo_addr({N:,} calls):  {addr_s:,.0f} ops/s  ({(t1-t0)*1e3:.1f} ms)")

# 1b. fibo_addr BATCH (bypass Python ctypes per-call overhead)
t0 = time.perf_counter()
results = br.fibo_addr_batch(seeds)
t1 = time.perf_counter()
batch_s = N / (t1 - t0)
print(f"1b. fibo_addr BATCH({N:,}):   {batch_s:,.0f} ops/s  ({(t1-t0)*1e3:.1f} ms)  === {batch_s/addr_s:.0f}x ===")

# ── 2. make_piece throughput ──────────────────────────────────────
t0 = time.perf_counter()
for s in seeds:
    br.make_piece(s, 1)
t1 = time.perf_counter()
piece_s = N / (t1 - t0)
print(f"2. make_piece({N:,}):        {piece_s:,.0f} ops/s  ({(t1-t0)*1e3:.1f} ms)")

# ── 3. bond_verify throughput ─────────────────────────────────────
M = 50000
pieces_a = [br.make_piece(i * 0x9E3779B9, 1) for i in range(M)]
pieces_b = [br.make_piece(i * 0x9E3779B9, 1) for i in range(M)]
t0 = time.perf_counter()
for pa, pb in zip(pieces_a, pieces_b):
    br.bond_verify(pa, pb)
t1 = time.perf_counter()
verify_s = M / (t1 - t0)
print(f"3. bond_verify({M:,}):       {verify_s:,.0f} ops/s  ({(t1-t0)*1e3:.1f} ms)")

# ── 4. multi-axis piece (all 7 shapes) ────────────────────────────
t0 = time.perf_counter()
for s in seeds:
    br.make_piece(s, 1)
    br.make_piece(s, 2)
    br.make_piece(s, 3)
    br.make_piece(s, 4)
    br.make_piece(s, 5)
    br.make_piece(s, 6)
    br.make_piece(s, 7)
t1 = time.perf_counter()
multi_s = (N * 7) / (t1 - t0)
print(f"4. multi-axis({N:,}×7):       {multi_s:,.0f} ops/s  ({(t1-t0)*1e3:.1f} ms)")

# ── 5. false-positive rate ────────────────────────────────────────
import random
N_FP = 100000
random.seed(42)
fp_hits = 0
t0 = time.perf_counter()
for _ in range(N_FP):
    sa = random.getrandbits(64)
    sb = random.getrandbits(64)
    pa = br.make_piece(sa, random.randint(1,7))
    pb = br.make_piece(sb, random.randint(1,7))
    valid, _ = br.bond_verify(pa, pb)
    if valid: fp_hits += 1
t1 = time.perf_counter()
fp_rate = fp_hits / N_FP
print(f"5. false-positive ({N_FP:,}): {fp_hits} hits = {fp_rate:.6%}  ({(t1-t0)*1e3:.1f} ms)")

# ── Summary ────────────────────────────────────────────────────────
print(f"\n{'='*60}")
print(f"SUMMARY:  fibo_addr={addr_s:,.0f}  batch={batch_s:,.0f}  piece={piece_s:,.0f}  verify={verify_s:,.0f}  multi={multi_s:,.0f}  FP={fp_rate:.6%}")
print(f"{'='*60}")
