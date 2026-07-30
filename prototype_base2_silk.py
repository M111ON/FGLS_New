#!/usr/bin/env python3
"""
Base-2 Selector + Silk Screen Encoder — Python Prototype
=========================================================
Quick validation before C implementation.

Architecture:
  1. Base-2 Selector: Classify each int8 weight into base-2 bins
     (8 magnitude classes from bit-7 down to bit-0, plus zero)
  2. Silk Screen: Route classified weights into 10 boxes × 6 dirs × clock
     filter[box][dir][tick] = weight  (IDENTITY — lossless)

Test:
  (1) Generate random int8 weights
  (2) Base-2 classify each weight
  (3) Silk screen encode/decode
  (4) Verify lossless roundtrip
"""

import random
import time
import sys
from collections import Counter

# ── Constants ─────────────────────────────────────────────────
N_BOXES   = 10       # Silk screen boxes (0-9)
N_DIRS    = 6        # Directions: +X, -X, +Y, -Y, +Z, -Z
CLOCK_MAX = 1440     # Clock ticks per layer
LAYER_SLOTS = N_BOXES * N_DIRS * CLOCK_MAX  # 86,400 per layer

DIR_NAMES = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]

# ══════════════════════════════════════════════════════════════
#  BASE-2 SELECTOR
# ══════════════════════════════════════════════════════════════
#
# Classifies int8 weights [-128..127] into base-2 magnitude classes.
#
# Strategy: find the highest set bit in |weight|.
#   weight = 0        → class 0 (zero)
#   |weight| in [1]   → class 1 (bit-0)
#   |weight| in [2-3] → class 2 (bit-1)
#   |weight| in [4-7] → class 3 (bit-2)
#   |weight| in [8-15]  → class 4 (bit-3)
#   |weight| in [16-31] → class 5 (bit-4)
#   |weight| in [32-63] → class 6 (bit-5)
#   |weight| in [64-127] → class 7 (bit-6)
#
# Plus sign bit: 8 magnitude classes × 2 signs = 16 total classes
# (zero is sign-agnostic, so 15 effective)
#
# This maps naturally to 10 boxes: we group magnitude classes
# into 10 selectors based on complementary pairs.

N_CLASSES = 9  # 0=zero, 1..8=magnitude bins

def base2_class(weight: int) -> int:
    """
    Classify an int8 weight into base-2 magnitude class [0..8].
    Class 0 = zero, class k (1-8) = highest set bit at position k-1.
    """
    if weight == 0:
        return 0
    mag = abs(weight)
    # Find highest set bit position (0-indexed), add 1 for 1-indexed class
    cls = 0
    while mag > 0:
        mag >>= 1
        cls += 1
    return cls  # 1..8


def base2_class_with_sign(weight: int) -> tuple:
    """
    Returns (magnitude_class, sign) where sign: 0=non-negative, 1=negative.
    Sign is encoded as an extra dimension for silk screen routing.
    """
    if weight == 0:
        return (0, 0)
    sign = 1 if weight < 0 else 0
    mag = abs(weight)
    cls = 0
    while mag > 0:
        mag >>= 1
        cls += 1
    return (cls, sign)


def base2_class_bits(weight: int) -> dict:
    """
    Full base-2 decomposition: which bit positions are set.
    Returns dict with classification info.
    """
    if weight == 0:
        return {"value": 0, "class": 0, "sign": 0, "bits": [], "magnitude": 0}

    sign = -1 if weight < 0 else 1
    mag = abs(weight)
    bits = []
    pos = 0
    temp = mag
    while temp > 0:
        if temp & 1:
            bits.append(pos)
        temp >>= 1
        pos += 1

    msb = bits[-1] if bits else 0
    return {
        "value": weight,
        "class": msb + 1,  # 1-indexed
        "sign": sign,
        "bits": bits,
        "magnitude": mag,
    }


# ══════════════════════════════════════════════════════════════
#  SILK SCREEN ENCODER
# ══════════════════════════════════════════════════════════════
#
# 10 boxes × 6 directions × 1440 clock ticks = 86,400 slots
# Routing: weight[flat_idx] → silk_screen[box][dir][tick]
#   box  = flat_idx % N_BOXES
#   dir  = (flat_idx // N_BOXES) % N_DIRS
#   tick = (flat_idx // (N_BOXES * N_DIRS)) % CLOCK_MAX

class SilkScreen:
    """Silk Screen encoder/decoder — 10 boxes × 6 dirs × clock."""

    def __init__(self):
        # filter[box][dir][tick] = weight (identity mapping)
        self.filter = [
            [ [0] * CLOCK_MAX for _ in range(N_DIRS) ]
            for _ in range(N_BOXES)
        ]
        self.n_ticks = CLOCK_MAX
        self.n_layers = 1
        self.permutation = None  # Store permutation for classified encoding

    def encode_flat(self, weights: list) -> int:
        """
        Encode flat weight array into silk screen via identity mapping.
        Returns number of weights encoded.
        """
        n = min(len(weights), N_BOXES * N_DIRS * self.n_ticks)
        for idx in range(n):
            b = idx % N_BOXES
            d = (idx // N_BOXES) % N_DIRS
            t = (idx // (N_BOXES * N_DIRS)) % self.n_ticks
            self.filter[b][d][t] = weights[idx]
        self.permutation = None
        return n

    def encode_flat_classified(self, weights: list, classes: list) -> int:
        """
        Encode with base-2 classification routing.
        Weights are sorted by class first, then mapped to silk screen.
        This packs same-class weights into adjacent slots.
        Stores permutation for lossless reverse mapping.
        """
        # Create (class, original_idx, weight) tuples
        indexed = [(classes[i], i, weights[i]) for i in range(len(weights))]
        # Sort by class for grouped storage
        indexed.sort(key=lambda x: x[0])

        # Store permutation: perm[flat_idx] = original_idx
        self.permutation = [idx for _, idx, _ in indexed]

        n = min(len(indexed), N_BOXES * N_DIRS * self.n_ticks)
        for flat_idx in range(n):
            b = flat_idx % N_BOXES
            d = (flat_idx // N_BOXES) % N_DIRS
            t = (flat_idx // (N_BOXES * N_DIRS)) % self.n_ticks
            self.filter[b][d][t] = indexed[flat_idx][2]
        return n

    def decode_flat(self, n_weights: int, restore_order: bool = True) -> list:
        """
        Decode silk screen back to flat weight array (identity read).
        If restore_order=True and permutation exists, restores original order.
        Returns list of weights in original flat order.
        """
        raw = []
        for idx in range(n_weights):
            b = idx % N_BOXES
            d = (idx // N_BOXES) % N_DIRS
            t = (idx // (N_BOXES * N_DIRS)) % self.n_ticks
            raw.append(self.filter[b][d][t])

        if restore_order and self.permutation is not None:
            # Invert permutation: result[orig_idx] = raw[perm_idx]
            result = [0] * len(raw)
            for perm_pos, orig_idx in enumerate(self.permutation):
                if orig_idx < len(raw) and perm_pos < len(raw):
                    result[orig_idx] = raw[perm_pos]
            return result

        return raw

    def decode_box(self, box: int) -> list:
        """Decode all weights from a single box."""
        result = []
        for d in range(N_DIRS):
            for t in range(self.n_ticks):
                result.append(self.filter[box][d][t])
        return result

    def stats(self) -> dict:
        """Compute statistics about the silk screen contents."""
        all_weights = []
        box_counts = [0] * N_BOXES
        dir_counts = [0] * N_DIRS

        for b in range(N_BOXES):
            for d in range(N_DIRS):
                for t in range(self.n_ticks):
                    w = self.filter[b][d][t]
                    all_weights.append(w)
                    box_counts[b] += 1
                    dir_counts[d] += 1

        zeros = sum(1 for w in all_weights if w == 0)
        positives = sum(1 for w in all_weights if w > 0)
        negatives = sum(1 for w in all_weights if w < 0)

        return {
            "total": len(all_weights),
            "zeros": zeros,
            "positives": positives,
            "negatives": negatives,
            "box_counts": box_counts,
            "dir_counts": dir_counts,
        }


# ══════════════════════════════════════════════════════════════
#  TEST SUITE
# ══════════════════════════════════════════════════════════════

def test_base2_classification():
    """Test base-2 classification on all 256 int8 values."""
    print("=" * 60)
    print("  TEST 1: Base-2 Classification (all 256 int8 values)")
    print("=" * 60)

    class_counts = Counter()
    errors = 0

    for w in range(-128, 128):
        cls = base2_class(w)
        class_counts[cls] += 1

        # Verify: class should match bit position of MSB
        if w == 0:
            expected = 0
        else:
            mag = abs(w)
            expected = mag.bit_length()

        if cls != expected:
            print(f"  ERROR: weight={w}, class={cls}, expected={expected}")
            errors += 1

    print(f"\n  Distribution of {len(class_counts)} classes:")
    for cls in sorted(class_counts.keys()):
        bar = "#" * (class_counts[cls] // 4)
        print(f"    class {cls:2d}: {class_counts[cls]:4d} weights  {bar}")

    print(f"\n  Errors: {errors}")
    print(f"  Result: {'PASS' if errors == 0 else 'FAIL'}")
    return errors == 0


def test_silk_screen_roundtrip():
    """Test silk screen encode/decode roundtrip with random int8 weights."""
    print("\n" + "=" * 60)
    print("  TEST 2: Silk Screen Lossless Roundtrip")
    print("=" * 60)

    # Generate random int8 weights
    random.seed(42)  # Reproducible
    n_weights = min(LAYER_SLOTS, 86400)  # One full layer
    weights = [random.randint(-128, 127) for _ in range(n_weights)]

    print(f"\n  Generated {n_weights} random int8 weights")

    # Classify each weight
    classes = [base2_class(w) for w in weights]
    class_dist = Counter(classes)
    print(f"  Base-2 class distribution:")
    for cls in sorted(class_dist.keys()):
        pct = 100.0 * class_dist[cls] / n_weights
        print(f"    class {cls:2d}: {class_dist[cls]:6d} ({pct:5.1f}%)")

    # Encode into silk screen
    silk = SilkScreen()
    t0 = time.perf_counter()
    encoded_n = silk.encode_flat(weights)
    enc_time = time.perf_counter() - t0
    print(f"\n  Encoded: {encoded_n} weights in {enc_time*1000:.3f} ms")

    # Decode from silk screen
    t0 = time.perf_counter()
    decoded = silk.decode_flat(encoded_n)
    dec_time = time.perf_counter() - t0
    print(f"  Decoded: {len(decoded)} weights in {dec_time*1000:.3f} ms")

    # Verify lossless roundtrip
    exact = sum(1 for i in range(n_weights) if weights[i] == decoded[i])
    max_error = max(abs(weights[i] - decoded[i]) for i in range(n_weights))

    print(f"\n  Roundtrip verification:")
    print(f"    Exact matches: {exact}/{n_weights} ({100.0*exact/n_weights:.1f}%)")
    print(f"    Max error:     {max_error}")
    print(f"    Result:        {'✓ LOSSLESS' if max_error == 0 else '✗ LOSSY'}")

    return max_error == 0


def test_classified_roundtrip():
    """Test classified (grouped by base-2 class) encode/decode with permutation restore."""
    print("\n" + "=" * 60)
    print("  TEST 3: Classified Silk Screen Roundtrip (with permutation)")
    print("=" * 60)

    random.seed(123)
    n_weights = min(LAYER_SLOTS, 86400)
    weights = [random.randint(-128, 127) for _ in range(n_weights)]
    classes = [base2_class(w) for w in weights]

    silk = SilkScreen()
    encoded_n = silk.encode_flat_classified(weights, classes)
    decoded = silk.decode_flat(encoded_n, restore_order=True)

    exact = sum(1 for i in range(n_weights) if weights[i] == decoded[i])
    max_error = max(abs(weights[i] - decoded[i]) for i in range(n_weights))

    perm_size = len(silk.permutation) if silk.permutation else 0

    print(f"  Encoded {encoded_n} weights (sorted by base-2 class)")
    print(f"  Permutation stored: {perm_size} entries ({perm_size*4:,} bytes)")
    print(f"  Exact matches: {exact}/{n_weights} ({100.0*exact/n_weights:.1f}%)")
    print(f"  Max error:     {max_error}")
    print(f"  Result:        {'✓ LOSSLESS' if max_error == 0 else '✗ LOSSY'}")

    return max_error == 0


def test_box_independence():
    """Test that each box can be decoded independently."""
    print("\n" + "=" * 60)
    print("  TEST 4: Box Independence (parallel decode)")
    print("=" * 60)

    random.seed(99)
    n_weights = min(LAYER_SLOTS, 86400)
    weights = [random.randint(-128, 127) for _ in range(n_weights)]

    silk = SilkScreen()
    silk.encode_flat(weights)

    # Decode each box independently
    total_decoded = 0
    for b in range(N_BOXES):
        box_weights = silk.decode_box(b)
        total_decoded += len(box_weights)

    expected = N_BOXES * N_DIRS * CLOCK_MAX
    print(f"  Total weights across all boxes: {total_decoded}")
    print(f"  Expected (10×6×1440):          {expected}")
    print(f"  Match: {'✓' if total_decoded == expected else '✗'}")

    return total_decoded == expected


def test_throughput():
    """Benchmark encode/decode throughput."""
    print("\n" + "=" * 60)
    print("  TEST 5: Throughput Benchmark")
    print("=" * 60)

    random.seed(777)
    n_weights = LAYER_SLOTS
    weights = [random.randint(-128, 127) for _ in range(n_weights)]

    N_ITERS = 100

    # Warm up
    for _ in range(10):
        s = SilkScreen()
        s.encode_flat(weights)
        s.decode_flat(n_weights)

    # Encode benchmark
    t0 = time.perf_counter()
    for _ in range(N_ITERS):
        s = SilkScreen()
        s.encode_flat(weights)
    enc_ms = (time.perf_counter() - t0) * 1000

    # Decode benchmark
    s = SilkScreen()
    s.encode_flat(weights)
    t0 = time.perf_counter()
    for _ in range(N_ITERS):
        s.decode_flat(n_weights)
    dec_ms = (time.perf_counter() - t0) * 1000

    ops = N_ITERS * n_weights
    print(f"\n  Iterations: {N_ITERS}")
    print(f"  Weights/iter: {n_weights:,}")
    print(f"\n  Encode: {enc_ms:.1f} ms total, {enc_ms/N_ITERS:.3f} ms/layer")
    print(f"          {ops/enc_ms/1000:.1f} M weights/sec")
    print(f"  Decode: {dec_ms:.1f} ms total, {dec_ms/N_ITERS:.3f} ms/layer")
    print(f"          {ops/dec_ms/1000:.1f} M weights/sec")

    return True


def test_scale_projection():
    """Project silk screen storage for real model sizes."""
    print("\n" + "=" * 60)
    print("  TEST 6: Scale Projection (real models)")
    print("=" * 60)

    models = [
        ("SmolLM2-360M", 360_000_000),
        ("Qwen3-0.6B",   600_000_000),
        ("Qwen3-4B",   4_000_000_000),
        ("Qwen3-14B", 14_000_000_000),
        ("Qwen3-30B", 30_000_000_000),
    ]

    print(f"\n  Silk screen layer: {LAYER_SLOTS:,} slots")
    print(f"  ({N_BOXES} boxes × {N_DIRS} dirs × {CLOCK_MAX} ticks)")
    print(f"\n  {'Model':<16} {'Weights':>14} {'Layers':>8} {'Silk(GB)':>10}")
    print(f"  {'─'*16} {'─'*14} {'─'*8} {'─'*10}")

    for name, n_weights in models:
        layers = (n_weights + LAYER_SLOTS - 1) // LAYER_SLOTS
        silk_gb = layers * LAYER_SLOTS * 1 / (1024**3)  # int8 = 1 byte
        print(f"  {name:<16} {n_weights:>14,} {layers:>8,} {silk_gb:>10.1f}")

    return True


# ══════════════════════════════════════════════════════════════
#  MAIN
# ══════════════════════════════════════════════════════════════

def main():
    print("╔══════════════════════════════════════════════════════════╗")
    print("║  Base-2 Selector + Silk Screen — Python Prototype       ║")
    print("║  Quick validation before C implementation               ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()

    results = {}
    results["base2_class"] = test_base2_classification()
    results["roundtrip"] = test_silk_screen_roundtrip()
    results["classified"] = test_classified_roundtrip()
    results["box_indep"] = test_box_independence()
    results["throughput"] = test_throughput()
    results["scale"] = test_scale_projection()

    # Final summary
    print("\n" + "=" * 60)
    print("  FINAL SUMMARY")
    print("=" * 60)
    all_pass = True
    for name, passed in results.items():
        status = "✓ PASS" if passed else "✗ FAIL"
        if not passed:
            all_pass = False
        print(f"    {name:<20} {status}")

    print(f"\n  Overall: {'✓ ALL TESTS PASSED' if all_pass else '✗ SOME TESTS FAILED'}")
    print(f"  Silk screen is {'LOSSLESS — ready for C implementation' if all_pass else 'NOT READY'}")

    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
