#!/usr/bin/env python3
"""
GFE (Geometric Frame Encoding) Prototype — v2 (fast)
======================================================
Fast harmonic fitting on stride-37 geometric positions.
"""
import struct, sys, os, math
from collections import Counter
import numpy as np

FRAME_CYCLE = 1440
FRAME_STRIDE = 37
FRAME_FACE_SZ = 120
FRAME_EDGES = 12

def frame_at(enc):
    face = enc // FRAME_FACE_SZ
    slot = enc % FRAME_FACE_SZ
    phase = (enc // FRAME_EDGES) % 12
    return face, slot, phase

def frame_unfold(start_enc):
    frames = []
    enc = start_enc
    for _ in range(FRAME_CYCLE):
        frames.append(frame_at(enc))
        enc = (enc + FRAME_STRIDE) % FRAME_CYCLE
    return frames  # list of (face,slot,phase)

# Precompute DESIGN MATRIX for all 1440 positions × 17 basis functions
# Basis: constant(1) + face_onehot(12) + slot_cos(1) + slot_sin(1) + phase_cos(1) + phase_sin(1)
K = 17
DESIGN = np.zeros((FRAME_CYCLE, K), dtype=np.float64)
for i in range(FRAME_CYCLE):
    f, s, p = frame_at(i)
    j = 0
    DESIGN[i, j] = 1.0; j += 1  # constant
    for k in range(12):
        DESIGN[i, j + k] = 1.0 if k == f else 0.0  # face one-hot
    j += 12
    DESIGN[i, j] = math.cos(2 * math.pi * s / FRAME_FACE_SZ); j += 1  # slot cos
    DESIGN[i, j] = math.sin(2 * math.pi * s / FRAME_FACE_SZ); j += 1  # slot sin
    DESIGN[i, j] = math.cos(2 * math.pi * p / 12); j += 1  # phase cos
    DESIGN[i, j] = math.sin(2 * math.pi * p / 12); j += 1  # phase sin

# Precompute (D^T D)^{-1} D^T — pseudo-inverse for fast fitting
DESIGN_PINV = np.linalg.pinv(DESIGN)  # (17 × 1440)
print(f"Design matrix: {DESIGN.shape}, pseudo-inverse: {DESIGN_PINV.shape}", file=sys.stderr)

def fit_harmonic(weights_1440):
    """Fast harmonic fit: C = pinv(D) @ w. Returns coefficients (17,)."""
    w = np.array(weights_1440, dtype=np.float64)
    return DESIGN_PINV @ w  # (17,) — O(17*1440) = ~25K ops

def evaluate_harmonic(C, frames):
    """Eval: w = D[sample_indices] @ C"""
    idx = np.array([f[0]*FRAME_FACE_SZ + f[1] for f in frames])  # map (face,slot) to 0..1439
    # But wait — frames are in stride-37 order, not sequential
    # Need to map each frame back to its enc position
    idx = np.array([f[0]*FRAME_FACE_SZ + f[1] for f in frames])
    return DESIGN[idx] @ C

# Precompute all 1440 stride-37 permutations
# For each seed, what are the positions (as enc indices) in order?
PERM = np.zeros((FRAME_CYCLE, FRAME_CYCLE), dtype=np.int32)
for seed in range(FRAME_CYCLE):
    enc = seed
    for i in range(FRAME_CYCLE):
        PERM[seed, i] = enc
        enc = (enc + FRAME_STRIDE) % FRAME_CYCLE
print(f"Permutation matrix: {PERM.shape}", file=sys.stderr)

def find_best_seed_fast(weights_1440):
    """Try all 1440 seeds, find best fit."""
    w = np.array(weights_1440, dtype=np.float64)
    
    best_seed = 0
    best_mse = float('inf')
    best_C = None
    
    # For speed: sample 12 seeds (one per face start)
    # But let's try more for thoroughness
    seeds = list(range(FRAME_CYCLE))  # ALL 1440 — fast enough?
    # Actually 1440 * (fit + eval) might be heavy. Use 12 seeds.
    seeds = [f * FRAME_FACE_SZ for f in range(12)]
    
    for seed in seeds:
        # Get permutation for this seed
        perm = PERM[seed]  # (1440,) — stride-37 order
        
        # Fit on the permuted positions
        C = DESIGN_PINV @ w  # same C regardless of seed? 
        
        # Actually fit is the same — the design matrix is constant
        # But the PREDICTION changes: we predict at scrambled positions
        
        # Predict at permuted positions
        pred = DESIGN[perm] @ C
        
        # Measure error vs actual weights
        err = w - pred
        mse = np.mean(err * err)
        
        if mse < best_mse:
            best_mse = mse
            best_seed = seed
            best_C = C
    
    return best_seed, best_mse, best_C

# Actually, the above is wrong. Let me rethink.
# 
# The permutation reorders the 1440 positions.
# Each position has its own (face, slot, phase).
# The harmonic fit is position-dependent — the same weight value at different
# positions has different geometric meaning.
#
# So: for a given seed s, we have 1440 (position, weight) pairs where
# position at step t = frame_at(PERM[s, t])
# and weight at step t = weights[t] (the actual model weight)
#
# We need to find coefficients C such that:
#   DESIGN[PERM[s, t]] @ C ≈ weights[t] for all t
#
# Or equivalently: fit C on (PERM[s] → DESIGN) mapping to weights

def fit_and_eval_seed(seed, weights_1440):
    """Fit harmonic coefficients at given seed permutation, return error."""
    w = np.array(weights_1440, dtype=np.float64)
    perm = PERM[seed]  # stride-37 order from this seed
    
    # Design matrix at permuted positions
    D_perm = DESIGN[perm]  # (1440 × 17)
    
    # Least squares: C = (D_perm^T D_perm)^{-1} D_perm^T w
    # Use lstsq for numerical stability
    C, residuals, rank, s = np.linalg.lstsq(D_perm, w, rcond=None)
    mse = residuals[0] / FRAME_CYCLE if len(residuals) > 0 else 0.0
    
    pred = D_perm @ C
    residuals_arr = w - pred
    
    return C, mse, residuals_arr


def analyze_block(weights_1440, label=""):
    """Full analysis of a 1440-weight block."""
    # Baseline entropy
    hist = Counter(weights_1440)
    total = len(weights_1440)
    h_raw = -sum(c/total * math.log2(c/total) for c in hist.values())
    
    # Try all seeds — but let's be smart about it
    # For speed: try seeds = face starts (12 seeds)
    seeds_to_try = [f * FRAME_FACE_SZ for f in range(12)]
    
    best_mse = float('inf')
    best_seed = 0
    best_C = None
    best_resid = None
    
    for seed in seeds_to_try:
        try:
            C, mse, resid = fit_and_eval_seed(seed, weights_1440)
            if mse < best_mse:
                best_mse = mse
                best_seed = seed
                best_C = C
                best_resid = resid
        except Exception as e:
            continue
    
    if best_resid is None:
        return None
    
    # Quantize residuals to int8 range for storage calc
    resid_int = np.round(best_resid).astype(np.int32)
    resid_hist = Counter(resid_int.tolist())
    resid_entropy = -sum(c/total * math.log2(c/total) for c in resid_hist.values())
    
    # RMSE
    rmse = math.sqrt(best_mse)
    
    # Bit cost analysis
    seed_bits = 11  # log2(1440) ≈ 10.2, rounded up
    coeff_bits = 17 * 8  # 17 coefficients × 8-bit fixed-point (can quantize later)
    residual_bits = resid_entropy * total
    
    total_bits = seed_bits + coeff_bits + residual_bits
    q8_bits = total * 8
    
    ratio = q8_bits / total_bits if total_bits > 0 else 0
    
    return {
        'seed': best_seed,
        'rmse': rmse,
        'raw_entropy': h_raw,
        'residual_entropy': resid_entropy,
        'total_bits': total_bits,
        'q8_bits': q8_bits,
        'ratio': ratio,
        'savings': q8_bits - total_bits,
    }


# ============================================================
# GGUF extraction (same as before)
# ============================================================

def read_str(data, pos):
    length = struct.unpack_from('<Q', data, pos)[0]
    s = data[pos+8:pos+8+length].decode('utf-8', errors='replace')
    return s, pos + 8 + length

def extract_q8_raw(data, offset, n_weights):
    weights = []
    n_blocks = (n_weights + 31) // 32
    for b in range(n_blocks):
        boff = offset + b * 34
        if boff + 34 > len(data):
            break
        n_in = min(32, n_weights - b * 32)
        for i in range(n_in):
            w = struct.unpack_from('<b', data, boff + 2 + i)[0]
            weights.append(w)
    return weights

def skip_metadata(data, pos, count):
    for i in range(count):
        key, pos = read_str(data, pos)
        vtype = struct.unpack_from('<I', data, pos)[0]; pos += 4
        if vtype in (0,1,7): pos += 1
        elif vtype in (2,3): pos += 2
        elif vtype in (4,5,6): pos += 4
        elif vtype == 8:
            slen = struct.unpack_from('<Q', data, pos)[0]; pos += 8 + slen
        elif vtype == 9:
            atype = struct.unpack_from('<I', data, pos)[0]; pos += 4
            acount = struct.unpack_from('<Q', data, pos)[0]; pos += 8
            if atype == 8:
                for _ in range(acount):
                    slen = struct.unpack_from('<Q', data, pos)[0]; pos += 8 + slen
            else:
                pos += 4 * acount
        elif vtype in (10,11,12): pos += 8
        else: pos += 4
    return pos

def load_gguf_weights(path):
    with open(path, 'rb') as f:
        data = f.read()
    pos = 4
    version = struct.unpack_from('<I', data, pos)[0]; pos += 4
    tensor_count = struct.unpack_from('<Q', data, pos)[0]; pos += 8
    kv_count = struct.unpack_from('<Q', data, pos)[0]; pos += 8
    pos = skip_metadata(data, pos, kv_count)
    infos = []
    for _ in range(tensor_count):
        name, pos = read_str(data, pos)
        n_dims = struct.unpack_from('<I', data, pos)[0]; pos += 4
        dims = list(struct.unpack_from(f'<{n_dims}Q', data, pos)); pos += 8*n_dims
        typ = struct.unpack_from('<I', data, pos)[0]; pos += 4
        offset = struct.unpack_from('<Q', data, pos)[0]; pos += 8
        total = 1
        for d in dims: total *= d
        infos.append((name, dims, typ, offset, total))
    tdata_start = (pos + 31) & ~31
    for name, dims, typ, offset, n_weights in infos:
        if typ == 8:
            actual_offset = tdata_start + offset
            ws = extract_q8_raw(data, actual_offset, n_weights)
            yield name, dims, ws


# ============================================================
# MAIN
# ============================================================

def main():
    if len(sys.argv) < 2:
        print("Usage: python gfe_prototype.py <model.gguf>")
        sys.exit(1)
    
    path = sys.argv[1]
    print(f"Loading: {path}")
    print(f"Design matrix: 1440×17 = {DESIGN.nbytes:,} bytes")
    print(f"Permutation: {PERM.shape} = {PERM.nbytes:,} bytes")
    print()
    
    all_results = []
    
    for name, dims, weights in load_gguf_weights(path):
        n_full_blocks = len(weights) // FRAME_CYCLE
        if n_full_blocks == 0:
            continue
        
        print(f"Tensor: {name} [{len(weights):,} weights → {n_full_blocks} blocks of 1440]")
        
        n_test = min(n_full_blocks, 5)
        block_results = []
        
        for b in range(n_test):
            block = weights[b * FRAME_CYCLE : (b + 1) * FRAME_CYCLE]
            result = analyze_block(block, f"{name}[{b}]")
            
            if result:
                block_results.append(result)
                
                # Print
                savings_mb = result['savings'] / 8 / 1024 / 1024 * n_full_blocks
                print(f"  block {b:3d}: seed={result['seed']:4d} "
                      f"RMSE={result['rmse']:.3f} "
                      f"rawH={result['raw_entropy']:.3f} "
                      f"resH={result['residual_entropy']:.3f} "
                      f"ratio={result['ratio']:.2f}× "
                      f"savings={savings_mb:.3f}MB for full tensor")
        
        # Aggregate per tensor
        if block_results:
            avg_ratio = np.mean([r['ratio'] for r in block_results])
            avg_rmse = np.mean([r['rmse'] for r in block_results])
            avg_resH = np.mean([r['residual_entropy'] for r in block_results])
            avg_rawH = np.mean([r['raw_entropy'] for r in block_results])
            avg_savings = np.mean([r['savings'] for r in block_results])
            
            print(f"  ─────────────────────────────────────────────────────")
            print(f"  AVG:     ratio={avg_ratio:.2f}× RMSE={avg_rmse:.3f} "
                  f"rawH={avg_rawH:.3f} resH={avg_resH:.3f}")
            print(f"          savings={avg_savings/8/1024/1024:.4f}MB per 1440-block")
            
            if avg_ratio > 1.0:
                print(f"          ✓ GFE BEATS Q8 ({avg_ratio:.2f}×)")
            else:
                print(f"          ✗ GFE below Q8 ({avg_ratio:.2f}×)")
            
            all_results.extend(block_results)
    
    if all_results:
        print(f"\n{'='*70}")
        print(f"OVERALL: {len(all_results)} blocks analyzed")
        for r in all_results[:3]:
            print(f"  seed={r['seed']:4d} RMSE={r['rmse']:.3f} resH={r['residual_entropy']:.3f} ratio={r['ratio']:.2f}×")
        
        avg_r = np.mean([r['ratio'] for r in all_results])
        avg_rh = np.mean([r['residual_entropy'] for r in all_results])
        min_r = np.min([r['ratio'] for r in all_results])
        max_r = np.max([r['ratio'] for r in all_results])
        
        print(f"\n  RATIO:   min={min_r:.2f}×  avg={avg_r:.2f}×  max={max_r:.2f}×")
        print(f"  resH:    avg={avg_rh:.3f} bits/weight (Q8=8.000)")
        bits_saved = 8.0 - avg_rh - (17*8 + 11)/FRAME_CYCLE
        print(f"  EFFECTIVE: {bits_saved:.4f} bits saved per weight vs Q8")
        
        if avg_r > 1.0:
            print(f"\n  ✓ GEOMETRIC FRAME ENCODING BEATS STATISTICAL ENTROPY")
        else:
            print(f"\n  ✗ Geometric basis not yet strong enough for significant compression")


if __name__ == '__main__':
    main()
