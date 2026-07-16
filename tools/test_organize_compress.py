#!/usr/bin/env python3
"""
Test "organize before compress" approaches from exp_geo_compress.py
Applied to arbitrary data (PDF, random, text).

Key ideas:
1. Byte frequency separation — group similar bytes
2. Row deltas — reshape matrix, compute inter-row diffs
3. Hilbert reordering — spatial locality
4. Scale/value separation (generalized from Q8_0)
"""
import os, sys, time, zlib, hashlib
import numpy as np

def row_delta_compress(data, row_size):
    """Reshape to matrix, compute row deltas, compress."""
    n = len(data)
    padded = n + (row_size - n % row_size) % row_size
    mat = np.frombuffer(data + b'\x00' * (padded - n), dtype=np.uint8).reshape(-1, row_size)
    
    # Compute delta between consecutive rows
    delta = np.zeros_like(mat)
    delta[0] = mat[0]
    for i in range(1, mat.shape[0]):
        delta[i] = ((mat[i].astype(np.int16) - mat[i-1].astype(np.int16)) % 256).astype(np.uint8)
    
    compressed = zlib.compress(delta.tobytes(), 9)
    return compressed, len(mat), row_size

def byte_frequency_separation(data):
    """Group bytes by frequency: high-freq bytes first, then low-freq."""
    values, counts = np.unique(np.frombuffer(data, dtype=np.uint8), return_counts=True)
    # Sort by frequency (most common first)
    order = np.argsort(-counts)
    sorted_values = values[order]
    
    # Create mapping: byte -> rank
    mapping = np.zeros(256, dtype=np.uint8)
    for rank, val in enumerate(sorted_values):
        mapping[val] = rank
    
    # Transform data
    arr = np.frombuffer(data, dtype=np.uint8)
    transformed = mapping[arr]
    
    compressed = zlib.compress(transformed.tobytes(), 9)
    
    # Store mapping (256 bytes) + compressed
    mapping_bytes = sorted_values.tobytes()
    result = mapping_bytes + compressed
    return result, len(mapping_bytes)

def sort_bytes(data):
    """Sort all bytes by value. Creates long runs of identical bytes."""
    arr = np.frombuffer(data, dtype=np.uint8)
    sorted_arr = np.sort(arr)
    compressed = zlib.compress(sorted_arr.tobytes(), 9)
    return compressed

def scale_value_separation(data, block_size=64):
    """Generalized Q8_0 approach: separate 'head' bytes from 'body' bytes in blocks."""
    n = len(data)
    padded = n + (block_size - n % block_size) % block_size
    padded_data = data + b'\x00' * (padded - n)
    
    arr = np.frombuffer(padded_data, dtype=np.uint8).reshape(-1, block_size)
    
    # Take first byte of each block as "head" (analogous to f32 scale)
    heads = arr[:, 0]
    bodies = arr[:, 1:].flatten()
    
    # Compress separately
    head_compressed = zlib.compress(heads.tobytes(), 9)
    body_compressed = zlib.compress(bodies.tobytes(), 9)
    
    return head_compressed + body_compressed, len(heads), len(bodies)

def hilbert_reorder(data):
    """Reorder bytes using Hilbert curve mapping for spatial locality."""
    n = len(data)
    # Find smallest 2^k x 2^k grid that fits
    side = 1
    while side * side < n:
        side *= 2
    
    # Simple Hilbert curve index mapping
    def hilbert_index(x, y, order):
        """Convert (x,y) to Hilbert curve index."""
        d = 0
        for s in range(order - 1, -1, -1):
            rx = (x >> s) & 1
            ry = (y >> s) & 1
            d += (3 * rx) ^ ry
            if ry == 0:
                if rx == 1:
                    x = (2**order - 1) - x
                    y = (2**order - 1) - y
                x, y = y, x
        return d
    
    order = side.bit_length() - 1
    
    # Create Hilbert order mapping
    hilbert_map = {}
    for y in range(side):
        for x in range(side):
            idx = hilbert_index(x, y, order)
            if idx < n:
                hilbert_map[idx] = y * side + x
    
    # Reorder data
    arr = np.frombuffer(data, dtype=np.uint8)
    reordered = np.zeros(side * side, dtype=np.uint8)
    for hilbert_pos, linear_pos in hilbert_map.items():
        if linear_pos < n:
            reordered[hilbert_pos] = arr[linear_pos]
    
    compressed = zlib.compress(reordered.tobytes(), 9)
    return compressed

def combined_approach(data):
    """Combine multiple organize-before-compress strategies."""
    arr = np.frombuffer(data, dtype=np.uint8).copy()
    
    # Step 1: Row delta at multiple row sizes
    best = zlib.compress(data, 9)
    
    for row_size in [16, 32, 64, 128, 256]:
        compressed, _, _ = row_delta_compress(data, row_size)
        if len(compressed) < len(best):
            best = compressed
    
    # Step 2: Scale/value separation at multiple block sizes
    for block_size in [16, 32, 64, 128]:
        compressed, _, _ = scale_value_separation(data, block_size)
        if len(compressed) < len(best):
            best = compressed
    
    # Step 3: Hilbert reorder
    hilbert = hilbert_reorder(data)
    if len(hilbert) < len(best):
        best = hilbert
    
    # Step 4: Sort + compress
    sorted_comp = sort_bytes(data)
    if len(sorted_comp) < len(best):
        best = sorted_comp
    
    return best

def test_approach(data, name, approach_fn, label):
    """Test one approach."""
    t0 = time.time()
    if name == "ScaleValue":
        compressed, heads, bodies = approach_fn(data)
        detail = f"heads={heads}, bodies={bodies}"
    elif name == "RowDelta":
        compressed, rows, cols = approach_fn(data)
        detail = f"rows={rows}, cols={cols}"
    else:
        compressed = approach_fn(data)
        detail = ""
    
    t = time.time() - t0
    ratio = len(compressed) / len(data)
    return {
        'name': name,
        'label': label,
        'input': len(data),
        'compressed': len(compressed),
        'ratio': ratio,
        'time_ms': t * 1000,
        'detail': detail,
    }

if __name__ == '__main__':
    print("=" * 80)
    print("  Organize Before Compress — Applied to Arbitrary Data")
    print("=" * 80)
    
    pdf_path = r'I:\FGLS_new\docs\POGLS_MASTER_SCHEMATIC.pdf'
    full_pdf = open(pdf_path, 'rb').read()
    
    test_cases = [
        ("PDF 10KB", full_pdf[:10000]),
        ("PDF 100KB", full_pdf[:100000]),
        ("PDF 1MB", full_pdf[:1000000]),
        ("Random 10KB", os.urandom(10000)),
        ("Random 100KB", os.urandom(100000)),
        ("Text 10KB", (b"Hello world! This is a test of organize-before-compress. " * 500)[:10000]),
        ("Text 100KB", (b"Hello world! This is a test of organize-before-compress. " * 5000)[:100000]),
    ]
    
    approaches = [
        ("Raw+zlib", lambda d: zlib.compress(d, 9)),
        ("FreqSep", byte_frequency_separation),
        ("SortBytes", sort_bytes),
        ("RowDelta16", lambda d: row_delta_compress(d, 16)[0]),
        ("RowDelta64", lambda d: row_delta_compress(d, 64)[0]),
        ("RowDelta256", lambda d: row_delta_compress(d, 256)[0]),
        ("ScaleVal16", lambda d: scale_value_separation(d, 16)[0]),
        ("ScaleVal64", lambda d: scale_value_separation(d, 64)[0]),
        ("Hilbert", hilbert_reorder),
        ("Combined", combined_approach),
    ]
    
    all_results = []
    
    for label, data in test_cases:
        print(f"\n{'='*80}")
        print(f"  {label} ({len(data):,} bytes)")
        print(f"{'='*80}")
        
        results = []
        for name, fn in approaches:
            try:
                r = test_approach(data, name, fn, label)
                results.append(r)
            except Exception as e:
                print(f"  {name:<15} ERROR: {e}")
        
        # Sort by ratio
        results.sort(key=lambda r: r['ratio'])
        best = results[0]
        
        for r in results:
            marker = " <-- BEST" if r == best else ""
            print(f"  {r['name']:<15} {r['compressed']:>8,} bytes ({r['ratio']:.3f}x) | {r['time_ms']:.0f}ms{marker}")
        
        all_results.extend(results)
    
    # Summary
    print("\n" + "=" * 80)
    print("  SUMMARY — Best approach per data type")
    print("=" * 80)
    
    for label, data in test_cases:
        matching = [r for r in all_results if r['label'] == label]
        matching.sort(key=lambda r: r['ratio'])
        best = matching[0]
        raw = next(r for r in matching if r['name'] == 'Raw+zlib')
        improvement = raw['ratio'] / best['ratio'] if best['ratio'] > 0 else 0
        print(f"  {label:<20} Best: {best['name']:<15} {best['ratio']:.3f}x (vs raw {raw['ratio']:.3f}x, {improvement:.2f}x better)")
