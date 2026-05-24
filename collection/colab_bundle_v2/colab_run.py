"""
POGLS Bond + FGLS + Pipeline — Colab T4 Full Test
==================================================
Run:  python3 colab_run.py
      (or paste into Jupyter cell, but designed for direct CLI)

Requires:
  - colab_bundle_v2/ extracted to /content/colab_bundle_v2/
  - gcc, make, python3 with torch, matplotlib, gguf
  - Qwen3-0.6B-Q8_0.gguf at /content/ (auto-download attempt)
"""

import os, sys, subprocess, time, json
import matplotlib.pyplot as plt

ROOT = "/content/colab_bundle_v2"
os.chdir(ROOT)
sys.path.insert(0, ROOT)

def run(cmd, cwd=ROOT):
    print(f"$ {cmd}")
    r = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=False)
    return r.returncode

# ── Phase 1: Build bond .so + C tests ──────────────────────
print("=" * 60)
print("PHASE 1: Build")
print("=" * 60)
run("bash build_colab.sh")

# ── Phase 2: Python bench ──────────────────────────────────
print("\n" + "=" * 60)
print("PHASE 2: Python Bench")
print("=" * 60)
run("python3 bench_colab.py")

# ── Phase 3: (optional) C bench ────────────────────────────
print("\n" + "=" * 60)
print("PHASE 3: C Bench")
print("=" * 60)
run("./bench_bond_tgw")

# ── Phase 4: Download Qwen GGUF (ถ้ายังไม่มี) ───────────────
GGUF_PATH = "/content/Qwen3-0.6B-Q8_0.gguf"
if not os.path.exists(GGUF_PATH):
    print(f"\nDownloading Qwen3-0.6B-Q8_0.gguf...")
    run(f"wget -q 'https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q8_0.gguf' -O {GGUF_PATH}")

# ── Phase 5: Pipeline test (gate + gear ramp + cycle) ─────
print("\n" + "=" * 60)
print("PHASE 5: Pipeline Test")
print("=" * 60)
import torch
from pipeline_merged import test_gguf, run_pipeline, l1_norm, SHELL_SCALES, DEVICE, load_gguf_int8

gate, results = test_gguf(path=GGUF_PATH)

x_real = load_gguf_int8(GGUF_PATH, max_tokens=576).to(DEVICE)
N = x_real.shape[0]
bridge = None

ramp_checks = {"ORBITAL": True, "CHIRAL": True}
ramp_ok = True
for mode, mname in enumerate(["ORBITAL", "CHIRAL", "CROSS", "HUB"]):
    code_diffs = []
    for sl in [0, 1, 2, 3]:
        out, snap = run_pipeline(gate, x_real.to(torch.int8), [N // 16, 16],
                                 mode=mode, shell_level=sl, bridge=bridge)
        code_diffs.append(snap.code_diff_l1)
    print(f"  {mname}: code_diff={code_diffs}", end="")
    if mname in ramp_checks and len(code_diffs) >= 4:
        cr = code_diffs[-1] / max(code_diffs[1], 1)  # code ratio vs step=20
        ok = cr > 2.0
        ramp_ok = ramp_ok and ok
        tag = "✓" if ok else "✗"
        print(f"  code_ramp={cr:.2f}x {tag}", end="")
    print()

if ramp_ok:
    print(f"\nRAMP > 2×: ALL PASS ✓")
else:
    print(f"\nRAMP > 2×: BELOW TARGET — increase div weight in train_gate() or epochs")

# ── Phase 6: Plot ──────────────────────────────────────────
print("\n" + "=" * 60)
print("PHASE 6: Plot")
print("=" * 60)
results_data = {
    "fibo_addr":   0,
    "make_piece":  0,
    "bond_verify": 0,
    "multi_axis":  0,
    "fp_rate":     0,
}
labels = list(results_data.keys())
vals   = [results_data[k] for k in labels]

plt.figure(figsize=(10, 5))
plt.bar(labels, vals, color=["#4ecdc4", "#ffb347", "#ff6b6b", "#7b68ee", "#19d4d4"])
plt.ylabel("Operations / second")
plt.title("POGLS Bond — Colab T4 Performance")
plt.xticks(rotation=20)
plt.grid(axis="y", alpha=0.3)
for i, v in enumerate(vals):
    plt.text(i, v + 20000, f"{v:,.0f}", ha="center", fontsize=10)
plt.tight_layout()
plt.savefig("pogls_colab_bench.png")
print("Saved → pogls_colab_bench.png")
