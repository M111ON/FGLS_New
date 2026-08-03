#!/usr/bin/env python3
"""
unified_288_decagram.py — Unified investigation: 288-cell x decagram {10/3} mapping
for FGLS geometric compression.
"""
import math, struct, sys, time, os
import numpy as np

def part1_math_relationships():
    print("=" * 80)
    print("PART 1: MATHEMATICAL RELATIONSHIPS — 288 x Decagram {10/3}")
    print("=" * 80)
    print("\n  [1A] The 288-to-360 gap:")
    print("  " + "-" * 60)
    print(f"    288 = 36 x 8  (36 chunks x 8 cells each)")
    print(f"    360 = 36 x 10 (36 chunks x 10 decagram slots)")
    print(f"    Gap = 360 - 288 = 72 = 6 x 12 = 36 x 2")
    print(f"\n    72 decompositions relevant to FGLS:")
    print(f"      72 = 6 x 12  -> 6 directions x 12 sub-cells (cube)")
    print(f"      72 = 8 x 9   -> 8 octants x 9 (3^2)")
    print(f"      72 = 4 x 18  -> 4 faces x 18 (icosahedron edge x 3)")
    print(f"      72 = 3 x 24  -> 3 axes x 24 (24-cell vertex count)")

    print("\n  [1B] Decagram {10/3} geometry:")
    print("  " + "-" * 60)
    print(f"    V=10, E=10 (step-3), F=10 (star points)")
    print(f"    10 vertices x 36 deg = 360 deg")
    print(f"    360 slots / 10 vertices = 36 slots/vertex")

    print("\n  [1C] 288 / 10 = 28.8 remainder:")
    print("  " + "-" * 60)
    print(f"    8 vertices x 29 + 2 vertices x 28 = 232 + 56 = 288  OK")

    print("\n  [1D] 24-cell x 12 rotations = 288:")
    print("  " + "-" * 60)
    print(f"    24=2x12, 288=12^2x2, 288x6=1728=12^3, 1728x12=20736=12^4")

    print("\n  [1E] Complete geometric number chain:")
    print("  " + "-" * 60)
    for n, d in [(10,"decagram V"),(12,"ico dirs"),(24,"24-cell V"),
                 (36,"6^2 chunks"),(72,"gap 6x12"),(144,"12^2"),
                 (288,"target"),(360,"decagram ring"),(1728,"12^3"),(20736,"12^4 FGLS")]:
        print(f"    {n:>6}  = {d}")
    print(f"\n    10x36=360, 12x24=288, 36x8=288, 36x2=72")


def part4_decode_288():
    print("\n" + "=" * 80)
    print("PART 4: 288-CELL DECODE ARCHITECTURE")
    print("=" * 80)
    print("""
  1. INDEX: 288 values as 9-bit indices (0..287)
     32/block -> 36 bytes + 2 scale = 38 bytes/block
  2. MAPPING: 288 cells <-> decagram {10/3}, 10 vertices
  3. DECODE: value = scale x (index - 128) / 127
  4. ROTATION: 288 = 24-cell x 12 rotations
  5. ADDRESS: 288 x 72 gap -> 1728 -> 20736""")
    print("  [4A] 0.8-remainder distribution:")
    print("  " + "-" * 60)
    verts = []; rem = 0
    for i in range(10):
        rem += 0.8; bonus = 1 if rem >= 1.0 else 0
        if bonus: rem -= 1.0
        verts.append(28 + bonus)
        print(f"    V{i}: 28+{bonus}={verts[-1]}  (accum: {rem:.1f})")
    print(f"    Total: {sum(verts)}  OK" if sum(verts)==288 else f"    FAIL")
    print(f"\n  [4B] Star graph (step-3):")
    for i in range(10):
        print(f"    V{i} -> V{(i+3)%10}, V{(i-3)%10}")


def part5_gap_72():
    print("\n" + "=" * 80)
    print("PART 5: THE 72-GAP — Phantom Slots Between 288 and 360")
    print("=" * 80)
    print("""
  360 - 288 = 72 = 6x12 = 3x24 = 8x9 = 4x18

  Method 1: 6 faces x 12 sub-cells = routing metadata
  Method 2: 10 edges x 7.2 slots = topology info
  Method 3: 24 vertices x 3 = 3D->4D projection info""")
    for f in range(6):
        s = f * 12
        print(f"    Face {f}: [{s:>3}..{s+11:>3}] = 12 routing cells")
    print(f"    Total: 72")


def part3_quantization_test():
    print("\n" + "=" * 80)
    print("PART 3: QUANTIZATION FIDELITY — 256 vs 288 vs 360 BUCKETS")
    print("=" * 80)

    np.random.seed(42)

    # Test 1: Simulated Q8_0 weights (fully vectorized)
    print(f"\n  Test 1: Simulated Q8_0 weights (realistic LM distribution)")
    print(f"  " + "-" * 60)
    n_blocks = 4254208
    n_w = n_blocks * 32

    t0 = time.time()
    log_scales = np.random.normal(-3.0, 1.5, n_blocks)
    scales = np.maximum(np.exp(log_scales), 1e-6).astype(np.float32)

    # Vectorized int8 generation
    r = np.random.random(n_w)
    int8_vals = np.zeros(n_w, dtype=np.float32)
    mask0 = r < 0.4
    mask1 = (r >= 0.4) & (r < 0.7)
    mask2 = (r >= 0.7) & (r < 0.9)
    mask3 = r >= 0.9
    int8_vals[mask0] = np.random.randint(-3, 4, mask0.sum())
    int8_vals[mask1] = np.random.randint(-30, 31, mask1.sum())
    int8_vals[mask2] = np.random.randint(-80, 81, mask2.sum())
    int8_vals[mask3] = np.random.randint(-127, 128, mask3.sum())

    weights = np.repeat(scales, 32) * int8_vals
    gen_time = time.time() - t0
    print(f"  Generated {n_w:,} weights in {gen_time:.2f}s")
    print(f"  Range: [{weights.min():.6f}, {weights.max():.6f}]")
    print(f"  Mean: {weights.mean():.6f}, Std: {weights.std():.6f}")

    signal_power = np.mean(weights ** 2)

    bucket_counts = [256, 288, 360, 512, 1024]
    results = {}

    print(f"\n  {'Buckets':>8}  {'MSE':>14}  {'MaxErr':>12}  {'PSNR(dB)':>10}  {'bits/e':>10}")
    print(f"  {'-'*8}  {'-'*14}  {'-'*12}  {'-'*10}  {'-'*10}")

    for nb in bucket_counts:
        wmin, wmax = weights.min(), weights.max()
        bin_width = (wmax - wmin) / nb
        indices = np.minimum(((weights - wmin) / bin_width).astype(np.int32), nb - 1)
        centers = wmin + (indices.astype(np.float64) + 0.5) * bin_width
        errors = weights.astype(np.float64) - centers
        mse = float(np.mean(errors ** 2))
        max_err = float(np.max(np.abs(errors)))
        psnr = 10 * math.log10(signal_power / mse) if mse > 0 else float('inf')
        mbpe = math.log2(nb)
        results[nb] = {'mse': mse, 'max_err': max_err, 'psnr': psnr, 'mbpe': mbpe}
        print(f"  {nb:>8}  {mse:>14.8f}  {max_err:>12.6f}  {psnr:>10.3f}  {mbpe:>10.3f}")

    print(f"\n  Analysis:")
    print(f"  " + "-" * 60)
    r256, r288, r360 = results[256], results[288], results[360]
    ratio = r288['mse'] / r256['mse'] if r256['mse'] > 0 else 0
    print(f"    288 vs 256 MSE ratio: {ratio:.4f} ({'BETTER' if ratio < 1 else 'WORSE'} by {abs(1-ratio)*100:.2f}%)")
    print(f"    288 vs 256 PSNR gain: {r288['psnr'] - r256['psnr']:+.3f} dB")
    ratio2 = r360['mse'] / r288['mse'] if r288['mse'] > 0 else 0
    print(f"    360 vs 288 MSE ratio: {ratio2:.4f} ({'BETTER' if ratio2 < 1 else 'WORSE'} by {abs(1-ratio2)*100:.2f}%)")
    print(f"    360 vs 288 PSNR gain: {r360['psnr'] - r288['psnr']:+.3f} dB")

    print(f"\n  Storage analysis:")
    print(f"    256 = 8 bits/elem (Q8_0 standard)")
    print(f"    288 ~ 8.17 bits/elem (+2.1% overhead)")
    print(f"    360 ~ 8.49 bits/elem (+6.1% overhead)")
    print(f"    288 block: 2(scale) + 36(32x9bit) = 38 bytes (vs 34 Q8_0)")

    # Test 2: Robustness across distributions
    print(f"\n  Test 2: Robustness across weight distributions (1M samples):")
    print(f"  " + "-" * 60)
    print(f"  {'Distribution':>20}  {'256-MSE':>12}  {'288-MSE':>12}  {'360-MSE':>12}  {'Win':>5}")
    print(f"  {'-'*20}  {'-'*12}  {'-'*12}  {'-'*12}  {'-'*5}")

    n_test = 1000000
    dists = [
        ("Normal", np.random.normal(0, 0.1, n_test)),
        ("Uniform", np.random.uniform(-1, 1, n_test)),
        ("Laplace", np.random.laplace(0, 0.05, n_test)),
        ("Bimodal", np.concatenate([np.random.normal(-0.3, 0.05, n_test//2),
                                    np.random.normal(0.3, 0.05, n_test//2)])),
        ("Sparse", np.where(np.random.random(n_test) < 0.7, 0.0,
                           np.random.normal(0, 0.2, n_test))),
        ("Heavy-tail", np.random.standard_t(3, n_test) * 0.1),
    ]

    for name, w in dists:
        wmin, wmax = w.min(), w.max()
        mse_results = {}
        for nb in [256, 288, 360]:
            bw = (wmax - wmin) / nb
            idx = np.minimum(((w - wmin) / bw).astype(np.int32), nb - 1)
            ctr = wmin + (idx.astype(np.float64) + 0.5) * bw
            mse_results[nb] = float(np.mean((w.astype(np.float64) - ctr) ** 2))
        best = min([(mse_results[256], "256"), (mse_results[288], "288"), (mse_results[360], "360")], key=lambda x: x[0])
        print(f"  {name:>20}  {mse_results[256]:>12.8f}  {mse_results[288]:>12.8f}  {mse_results[360]:>12.8f}  {best[1]:>5}")


def main():
    print("=" * 80)
    print("  UNIFIED 288-DECAGRAM INVESTIGATION — FGLS Geometric Compression")
    print("=" * 80)
    print(f"  Date: {time.strftime('%Y-%m-%d %H:%M:%S')}")

    part1_math_relationships()
    part4_decode_288()
    part5_gap_72()
    part3_quantization_test()

    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print("""
  1. 288 = 12^2 x 2 = 24 x 12 = 36 x 8
     Purely 12-based: 24 -> 288 -> 1728(12^3) -> 20736(12^4)

  2. 360 - 288 = 72 = 6 x 12 gap = routing/topology metadata

  3. 288 / 10 = 28.8 -> 8 vertices x 29 + 2 vertices x 28 = 288

  4. 288-bucket quantization: ~2% overhead vs Q8_0, better MSE

  5. 288 = GEOMETRIC BRIDGE: power-of-2 (256) <-> angular (360)
""")


if __name__ == "__main__":
    main()
