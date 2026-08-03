#!/usr/bin/env python3
"""
decode_288_struct_compress.py — 288-Cell Structural Compression on Real GGUF

Tests geometric structure compression on Q8_0 weights from SmolLM2-360M:
  1. Read 1000 Q8_0 blocks (32000 values)
  2. Build codebook of distinct sorted values
  3. Map sorted indices to 288-cell geometry via stride-37 decagram walk
  4. Encode: [sort_permutation][sparse_bitmap][values]
  5. Test 288-bucket quantization after sorting
"""

import struct
import numpy as np
import math
import time
from gguf import GGUFReader

# ============================================================
# Configuration
# ============================================================
GGUF_PATH = r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf"
N_BLOCKS = 1000
BLOCK_SIZE = 34  # 2-byte scale + 32 int8 weights

# 288-cell geometry constants
N_CELLS = 288
STRIDE = 37  # stride-37 walk on decagram
CUBE_SIZE = 10  # 10³ cube for 288 cells

# ============================================================
# Section 1: Read Q8_0 Blocks from GGUF
# ============================================================
def read_q8_blocks(path, n_blocks):
    """Read n_blocks of Q8_0 from GGUF file using proper library."""
    reader = GGUFReader(path, 'r')
    
    # Find first Q8_0 tensor (type 8 = Q8_0 in gguf)
    tensor = reader.tensors[0]  # token_embd.weight
    
    print(f"  Tensor: {tensor.name} (type={tensor.tensor_type})")
    print(f"  n_elements: {tensor.n_elements}, n_bytes: {tensor.n_bytes}")
    
    # Flatten the 2D memmap to 1D
    raw = bytes(tensor.data.flatten())
    
    # Parse Q8_0 blocks from raw bytes
    scales = []
    all_weights = []
    
    for i in range(n_blocks):
        base = i * BLOCK_SIZE
        if base + BLOCK_SIZE > len(raw):
            print(f"  Warning: only {i} blocks available")
            break
        
        # Read scale as float16
        scale = float(np.frombuffer(raw[base:base+2], dtype=np.float16)[0])
        
        # Read 32 int8 weights
        weights = list(struct.unpack('<32b', raw[base+2:base+BLOCK_SIZE]))
        
        scales.append(scale)
        all_weights.append(weights)
    
    return scales, all_weights

def dequantize(scales, all_weights):
    """Dequantize Q8_0: float32 = scale * int8 / 128"""
    all_floats = []
    for scale, weights in zip(scales, all_weights):
        for w in weights:
            all_floats.append(scale * w / 128.0)
    return np.array(all_floats, dtype=np.float32)

# ============================================================
# Section 2: Codebook from Distinct Values
# ============================================================
def build_codebook(floats):
    """Build codebook of distinct values."""
    sorted_unique = np.sort(np.unique(floats))
    codebook = {v: i for i, v in enumerate(sorted_unique)}
    indices = np.array([codebook[v] for v in floats])
    return sorted_unique, codebook, indices

# ============================================================
# Section 3: 288-Cell Geometric Mapping
# ============================================================
def stride37_decagram_walk(n_cells, stride, cube_size):
    """Generate stride-37 walk on decagram, map to 3D coordinates."""
    positions = []
    pos = 0
    occupied = set()
    
    for _ in range(n_cells):
        x = pos % cube_size
        y = (pos // cube_size) % cube_size
        z = pos // (cube_size * cube_size)
        occupied.add((x, y, z))
        positions.append((x, y, z))
        pos = (pos + stride) % (cube_size ** 3)
    
    return positions, occupied

def analyze_288_geometry(n_cells, stride, cube_size):
    """Measure collision rate, ghost rate, sparse density."""
    positions, occupied = stride37_decagram_walk(n_cells, stride, cube_size)
    
    total_cells = cube_size ** 3
    n_unique = len(occupied)
    n_ghosts = total_cells - n_unique
    collisions = n_cells - n_unique
    
    ghost_rate = n_ghosts / total_cells
    sparse_density = n_unique / total_cells
    
    return {
        'n_cells': n_cells,
        'n_unique': n_unique,
        'collisions': collisions,
        'ghosts': n_ghosts,
        'ghost_rate': ghost_rate,
        'sparse_density': sparse_density,
        'positions': positions
    }

# ============================================================
# Section 4: Encode [sort_permutation][sparse_bitmap][values]
# ============================================================
def delta_encode_permutation(indices):
    """Delta-encode sorted indices → measure bits."""
    sorted_indices = np.sort(indices)
    deltas = np.diff(sorted_indices, prepend=0)
    
    # Bits needed for delta encoding
    max_delta = int(np.max(deltas))
    bits_per_delta = max(1, math.ceil(math.log2(max_delta + 1)))
    total_bits = len(deltas) * bits_per_delta
    
    return deltas, bits_per_delta, total_bits

def create_sparse_bitmap(n_cells, active_count):
    """Create bitmap for 288 cells."""
    # Simple bitmap: 1 byte per 8 cells
    bitmap_bytes = (n_cells + 7) // 8
    # Compressed list: which cells are active
    # Each active cell index needs ceil(log2(288)) = 9 bits
    bits_per_index = math.ceil(math.log2(n_cells + 1))
    list_bits = active_count * bits_per_index
    
    return bitmap_bytes, list_bits, bits_per_index

def encode_sort_codebook(codebook, float_values):
    """Encode the distinct values codebook."""
    n_distinct = len(codebook)
    # Each value is float32 = 32 bits
    codebook_bits = n_distinct * 32
    return codebook_bits, n_distinct

# ============================================================
# Section 5: 288-Bucket Quantization
# ============================================================
def quantize_to_288_buckets(floats):
    """Quantize floats into 288 buckets after sorting."""
    sorted_unique = np.sort(np.unique(floats))
    n_distinct = len(sorted_unique)
    
    # Map to 288 buckets
    bucket_indices = np.round(np.linspace(0, 287, n_distinct)).astype(int)
    quantized = np.array([bucket_indices[np.argwhere(sorted_unique == v)[0, 0]] for v in floats])
    
    # Verify roundtrip
    dequantized = np.zeros_like(floats)
    for i, v in enumerate(floats):
        bucket = quantized[i]
        # Find the representative value for this bucket
        bucket_mask = bucket_indices == bucket
        representative = sorted_unique[bucket_mask][0]  # use min
        dequantized[i] = representative
    
    error = np.abs(floats - dequantized)
    max_error = np.max(error)
    mean_error = np.mean(error)
    
    n_quantized_distinct = len(np.unique(quantized))
    return quantized, max_error, mean_error, n_quantized_distinct

# ============================================================
# Main
# ============================================================
def main():
    print("=" * 72)
    print("288-CELL STRUCTURAL COMPRESSION — Real GGUF Test")
    print("=" * 72)
    
    # Section 1: Read Q8_0 blocks
    print("\n[1] READING Q8_0 BLOCKS")
    print("-" * 72)
    start = time.time()
    scales, all_weights = read_q8_blocks(GGUF_PATH, N_BLOCKS)
    read_time = time.time() - start
    
    total_values = len(scales) * 32
    print(f"  Blocks read:      {len(scales)}")
    print(f"  Total values:     {total_values:,}")
    print(f"  Read time:        {read_time:.3f}s")
    print(f"  Raw Q8_0 size:    {total_values * 8} bits = {total_values} bytes")
    
    # Section 2: Dequantize and build codebook
    print("\n[2] DEQUANTIZE & BUILD CODEBOOK")
    print("-" * 72)
    floats = dequantize(scales, all_weights)
    print(f"  Float range:      [{floats.min():.4f}, {floats.max():.4f}]")
    print(f"  Float mean:       {floats.mean():.4f}")
    print(f"  Float std:        {floats.std():.4f}")
    
    sorted_unique, codebook, indices = build_codebook(floats)
    print(f"  Distinct values:  {len(sorted_unique)}/{total_values} ({100*len(sorted_unique)/total_values:.1f}%)")
    print(f"  Codebook size:    {len(sorted_unique)} × 32 bits = {len(sorted_unique)*32} bits")
    print(f"  Index range:      [0, {len(sorted_unique)-1}]")
    
    # Section 3: 288-cell geometry
    print("\n[3] 288-CELL GEOMETRIC MAPPING")
    print("-" * 72)
    geo = analyze_288_geometry(N_CELLS, STRIDE, CUBE_SIZE)
    print(f"  Cube size:        {CUBE_SIZE}³ = {CUBE_SIZE**3} cells")
    print(f"  288 cells:        {geo['n_unique']} unique positions")
    print(f"  Collisions:       {geo['collisions']}")
    print(f"  Ghosts:           {geo['ghosts']} ({100*geo['ghost_rate']:.1f}%)")
    print(f"  Sparse density:   {100*geo['sparse_density']:.1f}%")
    
    # Section 4: Encoding
    print("\n[4] ENCODING: [sort_permutation][sparse_bitmap][values]")
    print("-" * 72)
    
    # Sort permutation (delta-encoded)
    deltas, bits_per_delta, perm_bits = delta_encode_permutation(indices)
    print(f"  Sort permutation: {len(deltas)} indices × {bits_per_delta} bits = {perm_bits:,} bits")
    print(f"  Max delta:        {int(np.max(deltas))}")
    print(f"  Avg delta:        {np.mean(deltas):.1f}")
    
    # Sparse bitmap
    active_count = len(sorted_unique)
    bitmap_bytes, list_bits, bits_per_idx = create_sparse_bitmap(N_CELLS, active_count)
    print(f"  Sparse bitmap:    {bitmap_bytes} bytes (288 cells)")
    print(f"  Active cells:     {active_count} × {bits_per_idx} bits = {list_bits:,} bits")
    
    # Values codebook
    codebook_bits, n_distinct = encode_sort_codebook(codebook, floats)
    print(f"  Values codebook:  {n_distinct} × 32 bits = {codebook_bits:,} bits")
    
    # Total
    total_bits = perm_bits + list_bits + codebook_bits
    raw_bits = total_values * 8
    ratio = raw_bits / total_bits if total_bits > 0 else float('inf')
    print(f"\n  TOTAL ENCODED:    {total_bits:,} bits")
    print(f"  RAW Q8_0:         {raw_bits:,} bits")
    print(f"  Compression:      {raw_bits/total_bits:.2f}x" if total_bits > 0 else "  Compression:      N/A")
    
    # Section 5: 288-bucket quantization
    print("\n[5] 288-BUCKET QUANTIZATION (post-sort)")
    print("-" * 72)
    quantized, max_err, mean_err, n_q_distinct = quantize_to_288_buckets(floats)
    print(f"  Original distinct: {len(sorted_unique)}")
    print(f"  Quantized buckets: {n_q_distinct}")
    print(f"  Max error:         {max_err:.6f}")
    print(f"  Mean error:        {mean_err:.6f}")
    print(f"  Bucket utilization: {n_q_distinct}/{N_CELLS} ({n_q_distinct/N_CELLS*100:.1f}%)")
    
    # Compression after bucket quantization
    q_sorted, q_codebook, q_indices = build_codebook(quantized.astype(float))
    q_deltas, q_bpd, q_perm_bits = delta_encode_permutation(q_indices)
    q_bitmap_bytes, q_list_bits, q_bpi = create_sparse_bitmap(N_CELLS, len(q_sorted))
    q_codebook_bits = len(q_sorted) * 32
    
    q_total = q_perm_bits + q_list_bits + q_codebook_bits
    print(f"\n  288-Bucket encoding:")
    print(f"    Permutation:     {q_perm_bits:,} bits ({q_bpd} bpi)")
    print(f"    Sparse bitmap:   {q_list_bits:,} bits")
    print(f"    Values codebook: {q_codebook_bits:,} bits")
    print(f"    TOTAL:           {q_total:,} bits")
    print(f"    vs raw Q8_0:     {raw_bits:,} bits")
    print(f"    Ratio:           {raw_bits/q_total:.2f}x" if q_total > 0 else "    Ratio:           N/A")
    
    # Summary
    print("\n" + "=" * 72)
    print("SUMMARY")
    print("=" * 72)
    print(f"  Method                          Bits      Ratio   vs Q8_0")
    print(f"  {'-'*60}")
    print(f"  Raw Q8_0                        {raw_bits:>10,}  1.00x   ---")
    print(f"  288-cell structural              {total_bits:>10,}  {raw_bits/total_bits:.2f}x   {'+' if total_bits < raw_bits else '-'}{abs(raw_bits-total_bits):,} bits")
    print(f"  288-bucket + structural          {q_total:>10,}  {raw_bits/q_total:.2f}x   {'+' if q_total < raw_bits else '-'}{abs(raw_bits-q_total):,} bits")
    
    print(f"\n  Key insight: Q8_0 uses {len(sorted_unique)}/{total_values} distinct values")
    print(f"  288 cells can address {N_CELLS} positions — {'sufficient' if len(sorted_unique) <= N_CELLS else 'insufficient'} for this data")
    print(f"  Geometric value: 288 enables full 12-based addressing chain (288×6×12=20736)")
    
    return {
        'n_distinct': len(sorted_unique),
        'n_total': total_values,
        'raw_bits': raw_bits,
        'struct_bits': total_bits,
        'bucket_bits': q_total,
        'max_err': max_err,
        'mean_err': mean_err
    }

if __name__ == '__main__':
    main()
