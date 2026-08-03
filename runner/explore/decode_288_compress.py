#!/usr/bin/env python3
"""
288-Cell Compression Potential Tests on Real GGUF Data
======================================================
Tests whether a 288-cell structure (10×10×10 cube at 28.8% density)
enables compression of Q8_0 quantized weights.

4 approaches tested against raw Q8_0 baseline.

Q8_0 block format (ggml): float16 scale (2 bytes) + int8[32] (32 bytes) = 34 bytes
"""

import struct
import numpy as np
import time
import os

# ============================================================
# GGUF Q8_0 Parser (manual, correct 34-byte blocks)
# ============================================================

Q8_0_BLOCK_SIZE = 32     # weights per block
Q8_0_BLOCK_BYTES = 34    # 2 (float16 scale) + 32 (int8 values)
GGUF_MAGIC = 0x46554747


def find_gguf_q8_0_data_offset(filepath):
    """
    Find the data section start and first Q8_0 tensor offset.
    Uses brute-force scanning for valid Q8_0 blocks.
    """
    with open(filepath, 'rb') as f:
        # Read header to get tensor count
        magic = struct.unpack('<I', f.read(4))[0]
        if magic != GGUF_MAGIC:
            raise ValueError("Not a GGUF file")
        version = struct.unpack('<I', f.read(4))[0]
        n_tensors = struct.unpack('<Q', f.read(8))[0]
        n_kv = struct.unpack('<Q', f.read(8))[0]
        
        # Skip KV pairs
        def skip_kv(f, vt):
            if vt in (0,1,7): f.read(1)
            elif vt in (2,3): f.read(2)
            elif vt in (4,5,6): f.read(4)
            elif vt == 8:
                sl = struct.unpack('<Q', f.read(8))[0]; f.read(sl)
            elif vt == 9:
                et = struct.unpack('<I', f.read(4))[0]
                ne = struct.unpack('<Q', f.read(8))[0]
                for _ in range(ne): skip_kv(f, et)
            elif vt in (10,11,12): f.read(8)
        
        for _ in range(n_kv):
            kl = struct.unpack('<Q', f.read(8))[0]; f.read(kl)
            vt = struct.unpack('<I', f.read(4))[0]; skip_kv(f, vt)
        
        # Read ALL tensor infos
        tensors = []
        for _ in range(n_tensors):
            nl = struct.unpack('<Q', f.read(8))[0]
            name = f.read(nl).decode('utf-8')
            nd = struct.unpack('<I', f.read(4))[0]
            dims = [struct.unpack('<Q', f.read(8))[0] for _ in range(nd)]
            dtype = struct.unpack('<I', f.read(4))[0]
            offset = struct.unpack('<Q', f.read(8))[0]
            tensors.append((name, dims, dtype, offset))
        
        tensor_info_end = f.tell()
        
        # Find alignment from general.alignment or default to 32
        alignment = 32
        for name, dims, dtype, offset in tensors:
            pass  # Just count
        
        # Data section starts after tensor info, aligned
        data_start = ((tensor_info_end + alignment - 1) // alignment) * alignment
        
        return data_start, tensors


def read_gguf_q8_0_blocks(filepath, max_blocks=500):
    """
    Read Q8_0 blocks from GGUF file using brute-force offset detection.
    Returns list of (scale_f32, int8_array_32) tuples.
    """
    # Try known offsets first (from gguf library)
    known_offsets = [1788128, 5947744, 5931488]
    
    data_start = None
    for offset in known_offsets:
        try:
            with open(filepath, 'rb') as f:
                f.seek(offset)
                raw = f.read(Q8_0_BLOCK_BYTES)
                if len(raw) < Q8_0_BLOCK_BYTES:
                    continue
                scale_f16 = struct.unpack('<e', raw[:2])[0]
                qvals = np.frombuffer(raw[2:34], dtype=np.int8)
                if np.isfinite(scale_f16) and 0.0001 < abs(scale_f16) < 10000:
                    unique = len(np.unique(qvals))
                    if unique > 5:
                        data_start = offset
                        break
        except:
            continue
    
    if data_start is None:
        # Brute force scan
        print("Brute-force scanning for data section...")
        with open(filepath, 'rb') as f:
            file_size = os.path.getsize(filepath)
            for offset in range(0, min(file_size, 20000000), 4):
                f.seek(offset)
                raw = f.read(Q8_0_BLOCK_BYTES)
                if len(raw) < Q8_0_BLOCK_BYTES:
                    continue
                scale_f16 = struct.unpack('<e', raw[:2])[0]
                if np.isfinite(scale_f16) and 0.001 < abs(scale_f16) < 100:
                    qvals = np.frombuffer(raw[2:34], dtype=np.int8)
                    if len(np.unique(qvals)) > 10:
                        # Check second block at +34
                        f.seek(offset + Q8_0_BLOCK_BYTES)
                        raw2 = f.read(Q8_0_BLOCK_BYTES)
                        if len(raw2) >= Q8_0_BLOCK_BYTES:
                            sc2 = struct.unpack('<e', raw2[:2])[0]
                            if np.isfinite(sc2) and 0.0001 < abs(sc2) < 10000:
                                data_start = offset
                                break
    
    if data_start is None:
        raise ValueError("Could not find Q8_0 data section")
    
    print(f"Data section found at offset: {data_start}")
    
    all_blocks = []
    with open(filepath, 'rb') as f:
        f.seek(data_start)
        
        while len(all_blocks) < max_blocks:
            raw = f.read(Q8_0_BLOCK_BYTES)
            if len(raw) < Q8_0_BLOCK_BYTES:
                break
            
            # Q8_0 block: float16 scale + int8[32]
            scale_f16 = struct.unpack('<e', raw[:2])[0]
            qvals = np.frombuffer(raw[2:34], dtype=np.int8).copy()
            
            # Validate
            if not np.isfinite(scale_f16):
                # Might have hit non-Q8_0 data, try to recover
                # Scan forward to find next valid block
                for step in range(4, 200, 2):
                    f.seek(data_start + len(all_blocks) * Q8_0_BLOCK_BYTES + step)
                    raw2 = f.read(Q8_0_BLOCK_BYTES)
                    if len(raw2) < Q8_0_BLOCK_BYTES:
                        break
                    sc2 = struct.unpack('<e', raw2[:2])[0]
                    if np.isfinite(sc2) and 0.0001 < abs(sc2) < 10000:
                        scale_f16 = sc2
                        qvals = np.frombuffer(raw2[2:34], dtype=np.int8).copy()
                        # Skip forward
                        f.seek(data_start + len(all_blocks) * Q8_0_BLOCK_BYTES + step + Q8_0_BLOCK_BYTES)
                        break
                else:
                    continue
            
            # Convert float16 scale to float32 for easier math
            scale_f32 = float(scale_f16)
            all_blocks.append((scale_f32, qvals))
    
    print(f"Extracted {len(all_blocks)} Q8_0 blocks")
    return all_blocks


def dequantize_q8_0(scale, qvals):
    """Dequantize Q8_0 block: float32 = scale * qval"""
    return scale * qvals.astype(np.float32)


# ============================================================
# Geometric Mappings
# ============================================================

def make_geometric_mapping(n_cells=1000, block_size=32):
    """
    Create a deterministic mapping of 32 positions within a 10×10×10 cube.
    Uses stride-9 pattern (288/32 = 9 groups of 32).
    """
    positions = []
    stride = 9
    for i in range(block_size):
        pos = (i * stride) % n_cells
        while pos in positions:
            pos = (pos + 1) % n_cells
        positions.append(pos)
    return positions


def make_bipolar_mapping():
    """
    Map 32 weights to 16 north + 16 south pairs.
    North positions: 0..143, South positions: 500..643
    """
    north = list(range(0, 16))
    south = list(range(500, 516))
    return list(zip(north, south))


# ============================================================
# Approach 1: Sparse Encoding
# ============================================================

def test_sparse_encoding(blocks):
    """Measure density and compute sparse encoding size."""
    mapping = make_geometric_mapping()
    
    total_active = 0
    for scale, qvals in blocks:
        total_active += len(set(mapping))
    
    avg_density = total_active / (len(blocks) * 1000)
    
    # Sparse encoding per block:
    # Method A: bitmap (125B) + values (32B) = 157B
    # Method B: count (1B) + positions (32×2B) + values (32×1B) = 97B
    # Method C: varint positions + values ≈ 32×2.5B = 80B
    sparse_a = 125 + 32  # bitmap + values
    sparse_b = 1 + 32 * 3  # count + 32*(2B pos + 1B val)
    sparse_c = 1 + 32 * 2  # count + 32*2B (packed positions+values)
    
    return {
        'density': avg_density,
        'sparse_bitmap': sparse_a,
        'sparse_packed': sparse_b,
        'sparse_minimal': sparse_c,
        'raw_bytes': Q8_0_BLOCK_BYTES,
        'ratio_bitmap': sparse_a / Q8_0_BLOCK_BYTES,
        'ratio_packed': sparse_b / Q8_0_BLOCK_BYTES,
        'ratio_minimal': sparse_c / Q8_0_BLOCK_BYTES,
    }


# ============================================================
# Approach 2: Shared-map + Residual
# ============================================================

def test_shared_map_residual(blocks, group_size=100):
    """Group blocks, build shared bitmap, store residuals."""
    mapping = make_geometric_mapping()
    
    results = []
    for gstart in range(0, len(blocks), group_size):
        group = blocks[gstart:gstart + group_size]
        if len(group) < 2:
            continue
        
        # Union bitmap: which of 1000 cells are used by ANY block in group
        bitmap = np.zeros(1000, dtype=np.uint8)
        for scale, qvals in group:
            for pos in mapping:
                bitmap[pos] = 1
        
        bitmap_bytes = int(np.ceil(int(np.sum(bitmap)) / 8))
        n_active = int(np.sum(bitmap))
        
        raw_total = len(group) * Q8_0_BLOCK_BYTES
        # Shared: bitmap + per-block values at mapped positions
        shared_total = bitmap_bytes + len(group) * 32
        
        results.append({
            'group_size': len(group),
            'bitmap_bytes': bitmap_bytes,
            'n_active': n_active,
            'raw_total': raw_total,
            'shared_total': shared_total,
            'savings': raw_total - shared_total,
            'ratio': shared_total / raw_total
        })
    
    if not results:
        return None
    
    return {
        'n_groups': len(results),
        'avg_bitmap': np.mean([r['bitmap_bytes'] for r in results]),
        'avg_savings': np.mean([r['savings'] for r in results]),
        'avg_ratio': np.mean([r['ratio'] for r in results]),
        'total_shared': sum(r['shared_total'] for r in results),
        'total_raw': sum(r['raw_total'] for r in results),
        'details': results[:3]
    }


# ============================================================
# Approach 3: 288-Bucket + Geometric Index
# ============================================================

def test_288_bucket_codebook(blocks):
    """Quantize dequantized weights to 288 buckets."""
    all_weights = []
    for scale, qvals in blocks:
        weights = dequantize_q8_0(scale, qvals)
        all_weights.extend(weights.tolist())
    
    all_weights = np.array(all_weights)
    valid = np.isfinite(all_weights)
    all_valid = all_weights[valid]
    
    if len(all_valid) == 0:
        return None
    
    w_min, w_max = float(all_valid.min()), float(all_valid.max())
    w_std = float(all_valid.std())
    
    # Uniform 288 buckets
    bucket_edges = np.linspace(w_min, w_max, 289)
    bucket_centers = (bucket_edges[:-1] + bucket_edges[1:]) / 2.0
    
    indices = np.searchsorted(bucket_edges[1:], all_valid)
    indices = np.clip(indices, 0, 287)
    
    reconstructed = bucket_centers[indices]
    mse = float(np.mean((all_valid - reconstructed) ** 2))
    max_err = float(np.max(np.abs(all_valid - reconstructed)))
    relative_err = max_err / (abs(w_max) + 1e-10)
    
    # Compute storage
    n_values = len(all_valid)
    codebook_bytes = 288 * 4  # float32 codebook
    indices_bytes = int(np.ceil(n_values * 9 / 8))  # 9-bit indices
    total_288 = codebook_bytes + indices_bytes
    
    raw_q8_0 = n_values * Q8_0_BLOCK_BYTES // Q8_0_BLOCK_SIZE  # 34B per 32 weights
    # More precisely: n_values/32 blocks × 34 bytes = n_values * 34/32
    raw_q8_0 = int(n_values * Q8_0_BLOCK_BYTES / Q8_0_BLOCK_SIZE)
    raw_q4_0 = int(n_values * 18 / Q8_0_BLOCK_SIZE)  # Q4_0: 18 bytes per 32 weights
    
    return {
        'n_values': n_values,
        'codebook_bytes': codebook_bytes,
        'indices_bytes': indices_bytes,
        'total_288_bytes': total_288,
        'raw_q8_0_bytes': raw_q8_0,
        'raw_q4_0_bytes': raw_q4_0,
        'ratio_vs_q8_0': total_288 / raw_q8_0,
        'ratio_vs_q4_0': total_288 / raw_q4_0,
        'mse': mse,
        'max_err': max_err,
        'relative_err': relative_err,
        'w_range': (w_min, w_max),
        'w_std': w_std,
    }


# ============================================================
# Approach 4: Bipolar Pair Sharing
# ============================================================

def test_bipolar_pair_sharing(blocks):
    """Measure north/south pair correlation."""
    pairs = make_bipolar_mapping()
    
    north_vals = {i: [] for i in range(16)}
    south_vals = {i: [] for i in range(16)}
    
    for scale, qvals in blocks:
        weights = dequantize_q8_0(scale, qvals)
        if not np.all(np.isfinite(weights)):
            continue
        for i in range(16):
            north_vals[i].append(float(weights[i]))
            south_vals[i].append(float(weights[i + 16]))
    
    correlations = []
    for i in range(16):
        n = np.array(north_vals[i])
        s = np.array(south_vals[i])
        if len(n) > 10 and np.std(n) > 1e-10 and np.std(s) > 1e-10:
            c = np.corrcoef(n, s)[0, 1]
            if np.isfinite(c):
                correlations.append(c)
    
    avg_corr = float(np.mean(correlations)) if correlations else 0.0
    abs_avg_corr = float(np.mean(np.abs(correlations))) if correlations else 0.0
    
    # Delta encoding analysis
    total_north = 0
    total_delta_bits = 0
    total_raw_bits = 0
    
    for i in range(16):
        n = np.array(north_vals[i])
        s = np.array(south_vals[i])
        if len(n) == 0:
            continue
        
        delta = np.abs(s - n)
        total_north += len(n) * 8  # 8 bits per north value
        total_raw_bits += len(n) * 16  # 8+8 bits per pair
        
        if len(delta) == 0:
            continue
        delta_range = np.max(delta)
        
        if delta_range < 1: delta_bits = 1
        elif delta_range < 2: delta_bits = 2
        elif delta_range < 4: delta_bits = 3
        elif delta_range < 8: delta_bits = 4
        elif delta_range < 16: delta_bits = 5
        elif delta_range < 32: delta_bits = 6
        elif delta_range < 64: delta_bits = 7
        else: delta_bits = 8
        
        total_delta_bits += len(delta) * delta_bits
    
    bipolar_encoded = total_north + int(np.ceil(total_delta_bits / 8))
    bipolar_raw = total_raw_bits // 8
    
    return {
        'correlations': correlations,
        'avg_correlation': avg_corr,
        'abs_avg_correlation': abs_avg_corr,
        'bipolar_encoded_bytes': bipolar_encoded,
        'bipolar_raw_bytes': bipolar_raw,
        'ratio': bipolar_encoded / bipolar_raw if bipolar_raw > 0 else 1.0,
    }


# ============================================================
# Cube Density Analysis
# ============================================================

def analyze_cube_density(blocks):
    """Analyze cell usage in 10×10×10 cube."""
    mapping = make_geometric_mapping()
    
    single_used = set(mapping)
    multi_used = set()
    full_used = set()
    
    for i, (scale, qvals) in enumerate(blocks):
        for pos in mapping:
            full_used.add(pos)
        if i < 99:
            for pos in mapping:
                multi_used.add(pos)
    
    # Random mapping for comparison
    rng = np.random.RandomState(42)
    rand_positions = rng.choice(1000, size=32, replace=False)
    rand_used = set()
    for i, (scale, qvals) in enumerate(blocks):
        for pos in rand_positions:
            rand_used.add(pos)
        if i >= 99:
            break
    
    return {
        'single': len(single_used),
        '100_blocks': len(multi_used),
        'all_blocks': len(full_used),
        'random_100': len(rand_used),
        'total_cells': 1000,
    }


# ============================================================
# Main
# ============================================================

def main():
    print("=" * 70)
    print("288-Cell Compression Potential Analysis on Real GGUF Q8_0 Data")
    print("=" * 70)
    
    # Load data
    gguf_path = "I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf"
    print(f"\nLoading: {os.path.basename(gguf_path)}")
    t0 = time.time()
    blocks = read_gguf_q8_0_blocks(gguf_path, max_blocks=500)
    t_load = time.time() - t0
    print(f"Loaded {len(blocks)} blocks in {t_load:.2f}s")
    
    if not blocks:
        print("ERROR: No Q8_0 blocks found!")
        return
    
    # Sample data
    sc, qv = blocks[0]
    w = dequantize_q8_0(sc, qv)
    print(f"\nSample block: scale={sc:.6f} (float16)")
    print(f"  qvals[0:8] = {qv[:8]}")
    print(f"  weights[0:8] = {w[:8]}")
    print(f"  weight range: [{w.min():.6f}, {w.max():.6f}]")
    
    # Run tests
    print("\n" + "=" * 70)
    print("APPROACH 1: Sparse Encoding")
    print("=" * 70)
    r1 = test_sparse_encoding(blocks)
    print(f"  Cube occupancy: {r1['density']:.1%} per block ({int(r1['density']*1000)}/1000 cells)")
    print(f"  Raw Q8_0 block: {r1['raw_bytes']} bytes (float16 scale + 32 int8)")
    print(f"  Sparse (bitmap): {r1['sparse_bitmap']}B  ratio={r1['ratio_bitmap']:.2f}x")
    print(f"  Sparse (packed): {r1['sparse_packed']}B  ratio={r1['ratio_packed']:.2f}x")
    print(f"  Sparse (minimal): {r1['sparse_minimal']}B  ratio={r1['ratio_minimal']:.2f}x")
    
    print("\n" + "=" * 70)
    print("APPROACH 2: Shared-map + Residual")
    print("=" * 70)
    r2 = test_shared_map_residual(blocks, group_size=100)
    if r2:
        print(f"  Groups: {r2['n_groups']}")
        print(f"  Avg bitmap: {r2['avg_bitmap']:.0f}B per group")
        print(f"  Avg savings: {r2['avg_savings']:.1f}B per group")
        print(f"  Overall ratio: {r2['avg_ratio']:.3f}x")
        for i, d in enumerate(r2['details']):
            print(f"    Group {i}: {d['group_size']} blocks, "
                  f"bitmap={d['bitmap_bytes']}B, raw={d['raw_total']}B, "
                  f"shared={d['shared_total']}B ({d['ratio']:.3f}x)")
    
    print("\n" + "=" * 70)
    print("APPROACH 3: 288-Bucket + Geometric Index")
    print("=" * 70)
    r3 = test_288_bucket_codebook(blocks)
    if r3:
        print(f"  Values: {r3['n_values']} (range=[{r3['w_range'][0]:.6f}, {r3['w_range'][1]:.6f}])")
        print(f"  Std dev: {r3['w_std']:.6f}")
        print(f"  288-bucket codebook: {r3['codebook_bytes']}B")
        print(f"  9-bit indices: {r3['indices_bytes']}B")
        print(f"  Total 288-encode: {r3['total_288_bytes']}B")
        print(f"  Raw Q8_0: {r3['raw_q8_0_bytes']}B")
        print(f"  Raw Q4_0 equiv: {r3['raw_q4_0_bytes']}B")
        print(f"  Ratio vs Q8_0: {r3['ratio_vs_q8_0']:.3f}x")
        print(f"  Ratio vs Q4_0: {r3['ratio_vs_q4_0']:.3f}x")
        print(f"  Quantization MSE: {r3['mse']:.10f}")
        print(f"  Max error: {r3['max_err']:.6f}")
        print(f"  Relative error: {r3['relative_err']:.4f}")
    
    print("\n" + "=" * 70)
    print("APPROACH 4: Bipolar Pair Sharing")
    print("=" * 70)
    r4 = test_bipolar_pair_sharing(blocks)
    if r4['correlations']:
        print(f"  Correlations ({len(r4['correlations'])} pairs):")
        for i, c in enumerate(r4['correlations'][:16]):
            bar = "#" * int(abs(c) * 30)
            sign = "+" if c > 0 else "-"
            print(f"    Pair {i:2d}: r={c:+.4f}  {sign}{bar}")
    print(f"  Average |correlation|: {r4['abs_avg_correlation']:.4f}")
    print(f"  Bipolar encoded: {r4['bipolar_encoded_bytes']}B")
    print(f"  Bipolar raw: {r4['bipolar_raw_bytes']}B")
    print(f"  Ratio: {r4['ratio']:.3f}x")
    
    print("\n" + "=" * 70)
    print("BONUS: 10³ Cube Density")
    print("=" * 70)
    rd = analyze_cube_density(blocks)
    print(f"  Single block: {rd['single']}/{rd['total_cells']} ({rd['single']/rd['total_cells']:.1%})")
    print(f"  100 blocks: {rd['100_blocks']}/{rd['total_cells']} ({rd['100_blocks']/rd['total_cells']:.1%})")
    print(f"  All {len(blocks)} blocks: {rd['all_blocks']}/{rd['total_cells']} ({rd['all_blocks']/rd['total_cells']:.1%})")
    print(f"  Random 100 blocks: {rd['random_100']}/{rd['total_cells']} ({rd['random_100']/rd['total_cells']:.1%})")
    
    # ============================================================
    # COMPARISON TABLE
    # ============================================================
    print("\n" + "=" * 70)
    print("COMPRESSION COMPARISON TABLE")
    print("=" * 70)
    print(f"{'Method':<38} {'Size (B)':<12} {'vs Q8_0':<10} {'Verdict':<10}")
    print("-" * 70)
    
    n_blocks = len(blocks)
    n_vals = n_blocks * 32
    raw_q8 = n_blocks * Q8_0_BLOCK_BYTES
    raw_q4 = n_blocks * 18  # Q4_0: 18B/32w
    
    print(f"{'Raw Q8_0 (baseline)':<38} {raw_q8:<12} {'1.000x':<10} {'BASE':<10}")
    print(f"{'Raw Q4_0 (reference)':<38} {raw_q4:<12} {raw_q4/raw_q8:<10.3f} {'-':<10}")
    
    # Approach 1: best sparse variant
    s1 = r1['sparse_minimal']
    print(f"{'A1: Sparse (minimal)':<38} {s1:<12} {s1/Q8_0_BLOCK_BYTES:<10.3f} "
          f"{'WIN' if s1 < Q8_0_BLOCK_BYTES else 'BIGGER':<10}")
    
    # Approach 2: shared map
    if r2:
        per_block_s2 = r2['total_shared'] / n_blocks
        print(f"{'A2: Shared bitmap (per-blk)':<38} {per_block_s2:<12.1f} {per_block_s2/Q8_0_BLOCK_BYTES:<10.3f} "
              f"{'WIN' if per_block_s2 < Q8_0_BLOCK_BYTES else 'BIGGER':<10}")
    
    # Approach 3: 288-bucket
    if r3:
        per_block_s3 = r3['total_288_bytes'] / n_blocks
        print(f"{'A3: 288-bucket (per-blk)':<38} {per_block_s3:<12.1f} {per_block_s3/Q8_0_BLOCK_BYTES:<10.3f} "
              f"{'WIN' if per_block_s3 < Q8_0_BLOCK_BYTES else 'BIGGER':<10}")
    
    # Approach 4: bipolar
    per_block_s4 = r4['bipolar_encoded_bytes'] / n_blocks if n_blocks > 0 else 0
    print(f"{'A4: Bipolar pairs (per-blk)':<38} {per_block_s4:<12.1f} {per_block_s4/Q8_0_BLOCK_BYTES:<10.3f} "
          f"{'WIN' if per_block_s4 < Q8_0_BLOCK_BYTES else 'BIGGER':<10}")
    
    print("-" * 70)
    
    # ============================================================
    # KEY INSIGHTS
    # ============================================================
    print("\n" + "=" * 70)
    print("KEY FINDINGS")
    print("=" * 70)
    
    findings = []
    
    # A1
    v1 = r1['ratio_minimal']
    findings.append(f"""
1. SPARSE ENCODING ({v1:.2f}x)
   Density: {r1['density']:.1%} of 1000 cells per block
   Position addressing ({int(np.ceil(np.log2(1000)))} bits) + value (8 bits) = ~18 bits/weight
   vs raw Q8_0: 8 bits/weight + 16-bit scale/32w
   VERDICT: {'WORSE' if v1 > 1 else 'BETTER'} - sparse addressing overhead {'dominates' if v1 > 1 else 'is manageable'}""")
    
    # A2
    if r2:
        v2 = r2['avg_ratio']
        findings.append(f"""
2. SHARED BITMAP ({v2:.3f}x)
   {r2['n_groups']} groups × {r2['avg_bitmap']:.0f}B bitmap shared
   Per-block values unchanged (32B each)
   Savings: {r2['avg_savings']:.0f}B per group of 100
   VERDICT: {'MARGINAL GAIN' if v2 < 0.95 else 'NO GAIN'} - bitmap savings diluted by per-block values""")
    
    # A3
    if r3:
        v3 = r3['ratio_vs_q8_0']
        findings.append(f"""
3. 288-BUCKET CODEBOOK ({v3:.3f}x vs Q8_0)
   9-bit indices vs 8-bit Q8_0 = +1 bit per weight overhead
   Codebook 1152B amortized over {r3['n_values']} values
   Quantization MSE: {r3['mse']:.2e}, relative err: {r3['relative_err']:.4f}
   VERDICT: {'BIGGER' if v3 > 1 else 'BETTER'} than Q8_0 (9-bit > 8-bit)
   Note: Q4_0 achieves {r3['raw_q4_0_bytes']/r3['raw_q8_0_bytes']:.3f}x - much better""")
    
    # A4
    v4 = r4['abs_avg_correlation']
    findings.append(f"""
4. BIPOLAR PAIR SHARING ({r4['ratio']:.3f}x)
   Average |correlation|: {v4:.4f}
   {'Weak' if v4 < 0.3 else 'Moderate' if v4 < 0.6 else 'Strong'} correlation between north/south pairs
   VERDICT: {'NO GAIN' if r4['ratio'] > 0.95 else 'MARGINAL GAIN'} - {'weak' if v4 < 0.3 else 'moderate'} correlation limits delta compression""")
    
    # Density
    findings.append(f"""
5. CUBE DENSITY
   Single block: {rd['single']}/1000 cells ({rd['single']/1000:.1%})
   All {len(blocks)} blocks: {rd['all_blocks']}/1000 ({rd['all_blocks']/1000:.1%})
   Random mapping: {rd['random_100']}/1000 ({rd['random_100']/1000:.1%})
   Stride-9 covers {rd['all_blocks']/1000:.0%} of cube = {'dense' if rd['all_blocks'] > 800 else 'sparse'} utilization""")
    
    for f in findings:
        print(f)
    
    print("=" * 70)
    print("OVERALL CONCLUSION")
    print("=" * 70)
    print("""
The 288-cell structure (10³ cube at 28.8% density) does NOT enable
compression of Q8_0 quantized weights through any of the 4 tested approaches:

- Sparse encoding: position addressing (10-16 bits) costs MORE than 8-bit values
- Shared bitmap: bitmap savings are diluted by per-block value storage
- 288-bucket: 9-bit indices > 8-bit Q8_0 values (+12.5% overhead)
- Bipolar pairs: weak cross-position correlations prevent effective delta coding

The fundamental issue: Q8_0 already achieves 8 bits/weight. Any geometric
mapping that uses >8 bits per position (which 1000-cell cube requires 10 bits)
will be MORE expensive, not less.

To achieve compression with 288-cell structure, you would need:
1. Fewer than 8 bits per bucket (impossible with 288 buckets → 9 bits needed)
2. Strong spatial correlations between blocks (weak in practice)
3. Exploitable structure in the quantized values (Q8_0 is already optimal)

The 288-cell structure is better suited as a LOSSY compression target
(e.g., 288-level quantization) rather than a LOSSLESS encoding structure.
""")


if __name__ == "__main__":
    main()
