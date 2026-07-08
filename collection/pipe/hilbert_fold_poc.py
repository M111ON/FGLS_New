"""
POC: Geometric Hierarchical Frequency Decomposition
=====================================================
Pipeline:
  1. Image -> FFT2D (wave/frequency domain)
  2. Reorder magnitude coefficients along a Hilbert curve
  3. At every scale: fold 4 rotated copies (0/90/180/270) -> 4-axis symmetry map
     (bits/values that survive across all 4 rotations = "geometric constants",
      analogous to fold_fibo_intersect in POGLS)
  4. Repeat at each pyramid scale (multi-scale, hierarchical)

This is exploratory / measurement code, not production POGLS code.
"""

import numpy as np
from PIL import Image
import json
import os

OUT_DIR = "/home/claude/poc/out"
os.makedirs(OUT_DIR, exist_ok=True)


# ---------------------------------------------------------------------------
# 1. Hilbert curve (integer, d2xy / xy2d - classic bit-twiddling version)
# ---------------------------------------------------------------------------

def hilbert_d2xy(n, d):
    """d -> (x,y) on an n x n Hilbert curve (n must be power of 2)."""
    x = y = 0
    t = d
    s = 1
    while s < n:
        rx = 1 & (t // 2)
        ry = 1 & (t ^ rx)
        # rotate
        if ry == 0:
            if rx == 1:
                x = s - 1 - x
                y = s - 1 - y
            x, y = y, x
        x += s * rx
        y += s * ry
        t //= 4
        s *= 2
    return x, y


def hilbert_order_indices(n):
    """Return list of (x,y) coordinates in Hilbert-curve order for an n x n grid."""
    total = n * n
    coords = [hilbert_d2xy(n, d) for d in range(total)]
    return coords


# ---------------------------------------------------------------------------
# 2. FFT -> frequency domain
# ---------------------------------------------------------------------------

def to_frequency_domain(img_gray):
    """2D FFT, shifted so DC is centered. Returns complex spectrum."""
    F = np.fft.fft2(img_gray.astype(np.float64))
    Fshift = np.fft.fftshift(F)
    return Fshift


def magnitude_log(Fshift):
    mag = np.abs(Fshift)
    return np.log1p(mag)


# ---------------------------------------------------------------------------
# 3. Hilbert reorder of a 2D array (must be power-of-2 sized, square)
# ---------------------------------------------------------------------------

def next_pow2(x):
    return 1 << (x - 1).bit_length()


def pad_to_pow2_square(arr):
    """Pad (with edge values) to a square power-of-2 array so Hilbert curve is well-defined."""
    h, w = arr.shape
    n = next_pow2(max(h, w))
    padded = np.zeros((n, n), dtype=arr.dtype)
    padded[:h, :w] = arr
    return padded, (h, w)


def hilbert_reorder(arr2d):
    """
    Flatten a 2D array into a 1D sequence following Hilbert curve order.
    Returns (sequence, n) where n is the padded square size.
    """
    padded, orig_shape = pad_to_pow2_square(arr2d)
    n = padded.shape[0]
    coords = hilbert_order_indices(n)
    seq = np.array([padded[x, y] for (x, y) in coords])
    return seq, n, orig_shape, coords


def hilbert_unreorder(seq, n, coords, orig_shape):
    """Inverse: put a Hilbert-ordered sequence back into a 2D array, then crop to orig_shape."""
    padded = np.zeros((n, n), dtype=seq.dtype)
    for (x, y), v in zip(coords, seq):
        padded[x, y] = v
    h, w = orig_shape
    return padded[:h, :w]


# ---------------------------------------------------------------------------
# 4. 4-axis symmetry fold
#    Rotate the (padded, square) array by 0/90/180/270 and find where all 4
#    rotated copies agree within a tolerance -> "symmetry-surviving" mask.
#    This is the frequency-domain analogue of fold_fibo_intersect (AND of
#    4 rotated copies -> bits/values that persist = geometric constants).
# ---------------------------------------------------------------------------

def four_axis_fold(arr2d, tol_frac=0.05, significance_percentile=75):
    """
    arr2d: square 2D array (real-valued, e.g. log-magnitude spectrum)
    tol_frac: relative tolerance band (fraction of global max) for "agreement"
    significance_percentile: coefficients below this percentile of magnitude
        are treated as "background" (near-DC / near-zero) and excluded from
        the weighted agreement metric, since a uniform background trivially
        agrees across all 4 rotations and inflates the raw ratio.

    Returns:
      fold_mask: bool array, True where all 4 rotations agree within tolerance
      fold_value: array with the mean of the 4 rotations (the "surviving" value)
      raw_agreement_ratio: fraction of ALL coefficients that agree (old metric,
          biased upward by uniform/background regions)
      weighted_agreement_ratio: fraction of agreement measured ONLY over
          "significant" (high-magnitude, non-background) coefficients — this
          is the metric that actually reflects structural/geometric symmetry
      significance_mask: bool array, True where the coefficient counted as
          "significant" (i.e. not background)
    """
    r0 = arr2d
    r90 = np.rot90(arr2d, k=1)
    r180 = np.rot90(arr2d, k=2)
    r270 = np.rot90(arr2d, k=3)

    stack = np.stack([r0, r90, r180, r270], axis=0)
    mean_val = stack.mean(axis=0)
    max_dev = np.max(np.abs(stack - mean_val), axis=0)

    global_max = np.max(np.abs(arr2d)) + 1e-9
    tol = tol_frac * global_max

    fold_mask = max_dev <= tol
    raw_agreement_ratio = fold_mask.sum() / fold_mask.size

    # significance = magnitude above the given percentile of |arr2d|
    # (excludes uniform backgrounds / near-DC low-energy regions)
    thresh = np.percentile(np.abs(arr2d), significance_percentile)
    significance_mask = np.abs(arr2d) >= thresh

    sig_count = significance_mask.sum()
    if sig_count > 0:
        weighted_agreement_ratio = (fold_mask & significance_mask).sum() / sig_count
    else:
        weighted_agreement_ratio = 0.0

    return fold_mask, mean_val, raw_agreement_ratio, weighted_agreement_ratio, significance_mask


# ---------------------------------------------------------------------------
# 5. Multi-scale pyramid: downsample repeatedly, run FFT + fold at each scale
# ---------------------------------------------------------------------------

def build_pyramid(img_gray, min_size=8):
    """Return list of grayscale arrays at halving resolutions."""
    levels = [img_gray.astype(np.float64)]
    cur = img_gray
    while min(cur.shape) // 2 >= min_size:
        pil = Image.fromarray(cur.astype(np.uint8))
        new_size = (cur.shape[1] // 2, cur.shape[0] // 2)
        cur = np.array(pil.resize(new_size, Image.BILINEAR)).astype(np.float64)
        levels.append(cur)
    return levels


def process_image(path, name, min_size=16):
    img = Image.open(path).convert("L")
    img_arr = np.array(img)

    pyramid = build_pyramid(img_arr, min_size=min_size)

    results = []
    for scale_idx, level in enumerate(pyramid):
        # 1. FFT -> frequency domain
        Fshift = to_frequency_domain(level)
        mag = magnitude_log(Fshift)

        # 2. Hilbert reorder of the magnitude spectrum
        seq, n, orig_shape, coords = hilbert_reorder(mag)

        # Reconstruct padded 2D (for fold step we fold the padded square version,
        # since 4-axis rotation requires a square array)
        padded_mag, _ = pad_to_pow2_square(mag)

        # 3. 4-axis fold on this scale's (padded) spectrum
        fold_mask, fold_val, raw_agree, weighted_agree, sig_mask = four_axis_fold(
            padded_mag, tol_frac=0.05, significance_percentile=75
        )

        results.append({
            "scale_idx": scale_idx,
            "shape": level.shape,
            "hilbert_n": n,
            "raw_agreement_ratio": float(raw_agree),
            "weighted_agreement_ratio": float(weighted_agree),
            "significant_fraction": float(sig_mask.sum() / sig_mask.size),
            "seq_len": len(seq),
        })

        # save a visualization for the first 3 scales
        if scale_idx < 3:
            vis = (fold_mask * 255).astype(np.uint8)
            Image.fromarray(vis).save(f"{OUT_DIR}/{name}_scale{scale_idx}_foldmask.png")
            mag_vis = ((mag - mag.min()) / (mag.max() - mag.min() + 1e-9) * 255).astype(np.uint8)
            Image.fromarray(mag_vis).save(f"{OUT_DIR}/{name}_scale{scale_idx}_spectrum.png")
            # significant-only fold mask (the de-biased view)
            sig_fold_vis = np.zeros_like(vis)
            sig_fold_vis[sig_mask] = ((fold_mask & sig_mask)[sig_mask] * 255).astype(np.uint8)
            Image.fromarray(sig_fold_vis).save(f"{OUT_DIR}/{name}_scale{scale_idx}_foldmask_significant.png")

    return results


if __name__ == "__main__":
    all_results = {}
    for path, name in [
        ("/home/claude/poc/wheel.png", "wheel"),
        ("/home/claude/poc/tresd34.png", "tresd34"),
        ("/home/claude/poc/flower.png", "flower"),
    ]:
        print(f"Processing {name} ...")
        res = process_image(path, name)
        all_results[name] = res
        for r in res:
            print(f"  scale {r['scale_idx']}: shape={r['shape']} "
                  f"hilbert_n={r['hilbert_n']} "
                  f"raw_agree={r['raw_agreement_ratio']:.4f} "
                  f"weighted_agree={r['weighted_agreement_ratio']:.4f} "
                  f"sig_frac={r['significant_fraction']:.4f} "
                  f"seq_len={r['seq_len']}")

    with open(f"{OUT_DIR}/results.json", "w") as f:
        json.dump(all_results, f, indent=2)

    print("\nSaved visualizations + results.json to", OUT_DIR)
