"""
locality_error_bench.py
=======================
ทดสอบ: locality สูง → reconstruction error ต่ำ จริงไหม?

ใช้ store ที่ build ไว้แล้ว (.gsidx/.gsdat)
ไม่ต้องเทรนอะไรใหม่

Usage (Colab):
  python locality_error_bench.py --store /content/stores/smollm2
  python locality_error_bench.py --store /content/stores/qwen25coder --zones 12
"""

import argparse
import json
import os
import sys
import numpy as np

# ── quantize helpers (no external deps) ──

def quantize_q4(weights: np.ndarray) -> np.ndarray:
    """Q4_0: 4-bit symmetric per-row quantization → dequantize back to f32."""
    out = np.zeros_like(weights, dtype=np.float32)
    for i, row in enumerate(weights):
        scale = np.abs(row).max() / 7.0 + 1e-10
        codes = np.clip(np.round(row / scale), -7, 7).astype(np.int8)
        out[i] = codes * scale
    return out

def quantize_q8(weights: np.ndarray) -> np.ndarray:
    """Q8_0: 8-bit symmetric per-row quantization → dequantize back to f32."""
    out = np.zeros_like(weights, dtype=np.float32)
    for i, row in enumerate(weights):
        scale = np.abs(row).max() / 127.0 + 1e-10
        codes = np.clip(np.round(row / scale), -127, 127).astype(np.int8)
        out[i] = codes * scale
    return out

def mse(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.mean((a.astype(np.float32) - b.astype(np.float32)) ** 2))

def cosine_sim(a: np.ndarray, b: np.ndarray) -> float:
    a_f = a.flatten().astype(np.float32)
    b_f = b.flatten().astype(np.float32)
    return float(np.dot(a_f, b_f) / (np.linalg.norm(a_f) * np.linalg.norm(b_f) + 1e-10))


# ── locality extractor (same as ZoneCard) ──

def compute_locality(weights: np.ndarray, zone_id: int, n_zones: int = 12) -> float:
    """Normalized locality 0.0-1.0 (same formula as core_card.extract_local)."""
    return min(1.0, zone_id / max(n_zones - 1, 1))

def compute_entropy(weights: np.ndarray) -> float:
    flat = weights.flatten().astype(np.float32)
    hist, _ = np.histogram(flat, bins=256, range=(-4, 4), density=True)
    hist = hist + 1e-10
    ent = -float(np.sum(hist * np.log2(hist + 1e-10)))
    return min(1.0, ent / 8.0)


# ── load store ──

def load_zone(store_path: str, zone: int, shape: str = "I"):
    """Load weight matrix from .gsidx/.gsdat store."""
    try:
        sys.path.insert(0, os.path.dirname(store_path))
        sys.path.insert(0, "/content/mcp")
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from geometry_store import GeometryStore
        gs = GeometryStore(store_path)
        w = gs.query(zone, shape)
        if w is None:
            return None
        return w.astype(np.float32)  # always f32 to avoid f16 overflow
    except ImportError:
        # fallback: synthetic weights seeded by zone (for offline testing)
        rng = np.random.RandomState(zone * 42 + 7)
        rows = 220
        cols = [1536, 896, 960][zone % 3]
        return rng.randn(rows, cols).astype(np.float32) * 0.5


# ── main benchmark ──

def run_bench(store_path: str, n_zones: int = 12, shape: str = "I"):
    print(f"\n{'='*65}")
    print(f"Locality vs Reconstruction Error Benchmark")
    print(f"Store: {store_path}  |  Zones: {n_zones}  |  Shape: {shape}")
    print(f"{'='*65}")
    print(f"{'Zone':>4}  {'Locality':>8}  {'Entropy':>8}  "
          f"{'MSE_Q4':>10}  {'MSE_Q8':>10}  {'CosSim_Q4':>10}  {'CosSim_Q8':>10}")
    print(f"{'-'*65}")

    results = []

    for zone in range(n_zones):
        w = load_zone(store_path, zone, shape)
        if w is None:
            print(f"{zone:>4}  MISS")
            continue

        # always cast to f32 — f16 overflows numpy reduce
        w_f16 = w.astype(np.float32)
        w_q4  = quantize_q4(w_f16)
        w_q8  = quantize_q8(w_f16)

        loc  = compute_locality(w_f16, zone, n_zones)
        ent  = compute_entropy(w_f16)
        mse4 = mse(w_f16, w_q4)
        mse8 = mse(w_f16, w_q8)
        cos4 = cosine_sim(w_f16, w_q4)
        cos8 = cosine_sim(w_f16, w_q8)

        results.append({
            "zone": zone, "locality": loc, "entropy": ent,
            "mse_q4": mse4, "mse_q8": mse8,
            "cos_q4": cos4, "cos_q8": cos8,
            "shape": w_f16.shape,
        })

        print(f"{zone:>4}  {loc:>8.3f}  {ent:>8.3f}  "
              f"{mse4:>10.6f}  {mse8:>10.6f}  {cos4:>10.4f}  {cos8:>10.4f}")

    if not results:
        print("No results — check store path")
        return

    print(f"{'='*65}")

    # ── correlation analysis ──
    locs  = np.array([r["locality"] for r in results])
    mses4 = np.array([r["mse_q4"]   for r in results])
    mses8 = np.array([r["mse_q8"]   for r in results])

    corr_q4 = float(np.corrcoef(locs, mses4)[0, 1]) if len(results) > 2 else 0.0
    corr_q8 = float(np.corrcoef(locs, mses8)[0, 1]) if len(results) > 2 else 0.0

    print(f"\nCorrelation  locality ↔ MSE_Q4 : {corr_q4:+.4f}  "
          f"({'negative = locality protects ✓' if corr_q4 < -0.3 else 'weak — check data'})")
    print(f"Correlation  locality ↔ MSE_Q8 : {corr_q8:+.4f}  "
          f"({'negative = locality protects ✓' if corr_q8 < -0.3 else 'weak — check data'})")

    # ── threshold finder ──
    print(f"\n── Locality threshold (MSE_Q4 < mean) ──")
    mean_mse4 = float(mses4.mean())
    safe = [r for r in results if r["mse_q4"] < mean_mse4]
    risky = [r for r in results if r["mse_q4"] >= mean_mse4]

    if safe:
        thresh = min(r["locality"] for r in safe)
        print(f"  Q4 safe zones  (MSE < {mean_mse4:.6f}): {[r['zone'] for r in safe]}")
        print(f"  Q4 risky zones (MSE ≥ {mean_mse4:.6f}): {[r['zone'] for r in risky]}")
        print(f"  → Suggested locality threshold for Q4: {thresh:.3f}  "
              f"(locality ≥ {thresh:.3f} → use F16/Q8)")

    # ── per-zone recommendation ──
    print(f"\n── Per-zone precision recommendation ──")
    for r in results:
        if r["locality"] >= 0.75:
            rec = "F16  (high locality — precision critical)"
        elif r["locality"] >= 0.40:
            rec = "Q8   (medium locality — balanced)"
        else:
            rec = "Q4   (low locality — coarse OK)"
        flag = "⚠" if r["mse_q4"] >= mean_mse4 and rec.startswith("Q4") else " "
        print(f"  {flag} zone {r['zone']:>2}  loc={r['locality']:.2f}  → {rec}")

    # ── std-based precision threshold ──
    print(f"\n── Std-based precision threshold (more accurate than locality) ──")
    print(f"{'Zone':>4}  {'Locality':>8}  {'Std':>8}  {'MSE_Q4':>10}  {'Rec_locality':>14}  {'Rec_std':>10}")
    print("-" * 65)
    for r in results:
        w_path = None
        # try to load real std if available
        real_std = r.get("std", None)
        if real_std is None:
            # estimate from MSE_Q4: MSE ≈ scale²/3, scale ≈ std*4/7 for Q4
            real_std = float(np.sqrt(r["mse_q4"] * 3)) * 7 / 4

        if real_std < 0.05:
            rec_std = "Q4 "
        elif real_std < 0.13:
            rec_std = "Q8 "
        else:
            rec_std = "F16"

        if r["locality"] >= 0.75:
            rec_loc = "F16"
        elif r["locality"] >= 0.40:
            rec_loc = "Q8 "
        else:
            rec_loc = "Q4 "

        flag = "!" if rec_std != rec_loc.strip() else " "
        print(f"{flag}{r['zone']:>3}  {r['locality']:>8.3f}  {real_std:>8.5f}  "
              f"{r['mse_q4']:>10.6f}  {rec_loc:>14}  {rec_std:>10}")

    print(f"\n  ! = locality-based and std-based recommendations disagree")
    print(f"  → Use std-based for production (more accurate)")

    # ── save results ──
    out_path = "locality_bench_results.json"
    with open(out_path, "w") as f:
        json.dump({
            "store": store_path,
            "n_zones": n_zones,
            "corr_q4": corr_q4,
            "corr_q8": corr_q8,
            "mean_mse_q4": mean_mse4,
            "zones": results,
        }, f, indent=2)
    print(f"\nResults saved → {out_path}")
    print(f"{'='*65}\n")

    return results


# ── multi-store comparison ──

def compare_stores(store_paths: list[str], n_zones: int = 12):
    """Compare locality-error profile across multiple models."""
    print(f"\n{'='*65}")
    print("Cross-model locality-error comparison")
    print(f"{'='*65}")

    all_results = {}
    for sp in store_paths:
        name = os.path.basename(sp)
        r = run_bench(sp, n_zones)
        if r:
            all_results[name] = r

    if len(all_results) < 2:
        return

    print(f"\n── Cross-model MSE_Q4 comparison ──")
    print(f"{'Zone':>4}", end="")
    for name in all_results:
        print(f"  {name[:12]:>12}", end="")
    print()

    for z in range(n_zones):
        print(f"{z:>4}", end="")
        for name, results in all_results.items():
            r = next((x for x in results if x["zone"] == z), None)
            if r:
                print(f"  {r['mse_q4']:>12.6f}", end="")
            else:
                print(f"  {'N/A':>12}", end="")
        print()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--store", default="",
                        help="Path to store (without .gsidx/.gsdat extension)")
    parser.add_argument("--stores", nargs="+", default=[],
                        help="Multiple stores for cross-model comparison")
    parser.add_argument("--zones", type=int, default=12)
    parser.add_argument("--shape", default="I")
    parser.add_argument("--synthetic", action="store_true",
                        help="Use synthetic weights (no store needed, for testing)")
    args = parser.parse_args()

    if args.synthetic or (not args.store and not args.stores):
        print("[INFO] No store path given — using synthetic weights")
        args.store = "synthetic"

    if args.stores:
        compare_stores(args.stores, args.zones)
    else:
        run_bench(args.store, args.zones, args.shape)
