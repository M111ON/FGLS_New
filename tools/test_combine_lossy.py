#!/usr/bin/env python3
"""
Test: Combine 3 lossy approaches to make lossless compression.

Key insight: 
- FreqSep = bijective byte mapping (lossless, 256 bytes overhead)
- SortBytes = permutation (lossy, but we can store permutation as index)
- Hilbert = spatial reorder (lossy, but we can store mapping)

Question: Can combining them produce better compression than raw+zlib?
"""
import os, sys, time, zlib, hashlib, struct
import numpy as np

# ══════════════════════════════════════════════════════════════
# Approach 1: FreqSep only (lossless)
# ══════════════════════════════════════════════════════════════

def freqsep_compress(data):
    """
    FreqSep: map bytes → frequency ranks (bijective, lossless).
    Stores: [256-byte mapping] + [compressed transformed data]
    """
    arr = np.frombuffer(data, dtype=np.uint8)
    values, counts = np.unique(arr, return_counts=True)
    
    # Create mapping: byte → rank (most frequent = 0)
    order = np.argsort(-counts)
    mapping = np.zeros(256, dtype=np.uint8)
    for rank, val in enumerate(order):
        mapping[val] = rank
    
    # Transform data
    transformed = mapping[arr]
    
    # Compress
    compressed = zlib.compress(transformed.tobytes(), 9)
    
    # Store: mapping (256 bytes) + compressed
    return mapping.tobytes() + compressed

def freqsep_decompress(compressed_data):
    """Decompress FreqSep."""
    # Extract mapping (first 256 bytes)
    mapping = np.frombuffer(compressed_data[:256], dtype=np.uint8)
    
    # Create inverse mapping
    inverse = np.zeros(256, dtype=np.uint8)
    for rank, val in enumerate(mapping):
        inverse[val] = rank
    
    # Decompress data
    decompressed = np.frombuffer(zlib.decompress(compressed_data[256:]), dtype=np.uint8)
    
    # Apply inverse mapping
    return inverse[decompressed].tobytes()

# ══════════════════════════════════════════════════════════════
# Approach 2: FreqSep + Row Delta (lossless)
# ══════════════════════════════════════════════════════════════

def freqsep_rowdelta_compress(data, row_size=64):
    """
    FreqSep + Row Delta: 
    1. FreqSep: map bytes → ranks
    2. Reshape to matrix
    3. Compute row deltas
    4. Compress
    """
    arr = np.frombuffer(data, dtype=np.uint8)
    values, counts = np.unique(arr, return_counts=True)
    
    # FreqSep mapping
    order = np.argsort(-counts)
    mapping = np.zeros(256, dtype=np.uint8)
    for rank, val in enumerate(order):
        mapping[val] = rank
    
    transformed = mapping[arr]
    
    # Pad to row_size multiple
    n = len(transformed)
    padded = n + (row_size - n % row_size) % row_size
    padded_data = np.pad(transformed, (0, padded - n), constant_values=0)
    mat = padded_data.reshape(-1, row_size)
    
    # Row deltas
    delta = np.zeros_like(mat)
    delta[0] = mat[0]
    for i in range(1, mat.shape[0]):
        delta[i] = ((mat[i].astype(np.int16) - mat[i-1].astype(np.int16)) % 256).astype(np.uint8)
    
    # Compress
    compressed = zlib.compress(delta.tobytes(), 9)
    
    # Store: mapping (256B) + row_size (2B) + original length (4B) + compressed
    header = mapping.tobytes() + struct.pack('<HI', row_size, n)
    return header + compressed

def freqsep_rowdelta_decompress(compressed_data):
    """Decompress FreqSep + Row Delta."""
    # Extract mapping
    mapping = np.frombuffer(compressed_data[:256], dtype=np.uint8)
    inverse = np.zeros(256, dtype=np.uint8)
    for rank, val in enumerate(mapping):
        inverse[val] = rank
    
    # Extract row_size and original length
    row_size, orig_len = struct.unpack_from('<HI', compressed_data, 256)
    
    # Decompress
    delta_flat = np.frombuffer(zlib.decompress(compressed_data[262:]), dtype=np.uint8)
    mat = delta_flat.reshape(-1, row_size)
    
    # Reconstruct from deltas
    reconstructed = np.zeros_like(mat)
    reconstructed[0] = mat[0]
    for i in range(1, mat.shape[0]):
        reconstructed[i] = ((reconstructed[i-1].astype(np.int16) + mat[i].astype(np.int16)) % 256).astype(np.uint8)
    
    # Apply inverse mapping
    flat = reconstructed.flatten()[:orig_len]
    return inverse[flat].tobytes()

# ══════════════════════════════════════════════════════════════
# Approach 3: FreqSep + Sort + Store permutation (lossless)
# ══════════════════════════════════════════════════════════════

def freqsep_sort_compress(data):
    """
    FreqSep + Sort:
    1. FreqSep: map bytes → ranks
    2. Sort the ranks (create long runs)
    3. Store permutation as bit-packed indices
    4. Compress everything
    """
    arr = np.frombuffer(data, dtype=np.uint8)
    values, counts = np.unique(arr, return_counts=True)
    
    # FreqSep mapping
    order = np.argsort(-counts)
    mapping = np.zeros(256, dtype=np.uint8)
    for rank, val in enumerate(order):
        mapping[val] = rank
    
    transformed = mapping[arr]
    
    # Sort and get permutation
    sorted_idx = np.argsort(transformed)
    perm = np.argsort(sorted_idx)  # inverse permutation
    
    # Compress sorted data (should have long runs)
    sorted_data = transformed[sorted_idx]
    sorted_compressed = zlib.compress(sorted_data.tobytes(), 9)
    
    # Compress permutation (bit-pack: N * ceil(log2(N)) bits)
    n = len(data)
    bits_needed = max(1, int(np.ceil(np.log2(n + 1))))
    
    # Pack permutation into bit array
    bit_array = bytearray((n * bits_needed + 7) // 8)
    for i, p in enumerate(perm):
        for b in range(bits_needed):
            if (p >> b) & 1:
                byte_idx = (i * bits_needed + b) // 8
                bit_idx = (i * bits_needed + b) % 8
                bit_array[byte_idx] |= (1 << bit_idx)
    
    perm_compressed = zlib.compress(bytes(bit_array), 9)
    
    # Store: mapping (256B) + bits_needed (1B) + orig_len (4B) + perm_compressed + sorted_compressed
    header = mapping.tobytes() + struct.pack('<BI', bits_needed, n)
    return header + perm_compressed + sorted_compressed

def freqsep_sort_decompress(compressed_data):
    """Decompress FreqSep + Sort."""
    # Extract mapping
    mapping = np.frombuffer(compressed_data[:256], dtype=np.uint8)
    inverse = np.zeros(256, dtype=np.uint8)
    for rank, val in enumerate(mapping):
        inverse[val] = rank
    
    # Extract bits_needed and orig_len
    bits_needed, orig_len = struct.unpack_from('<BI', compressed_data, 256)
    
    # Find split point (we need to know where perm_compressed ends)
    # Try different split points
    for split in range(260, len(compressed_data)):
        try:
            perm_data = zlib.decompress(compressed_data[260:split])
            sorted_data = zlib.decompress(compressed_data[split:])
            
            # Unpack permutation
            perm = np.zeros(orig_len, dtype=np.int64)
            for i in range(orig_len):
                val = 0
                for b in range(bits_needed):
                    byte_idx = (i * bits_needed + b) // 8
                    bit_idx = (i * bits_needed + b) % 8
                    if perm_data[byte_idx] & (1 << bit_idx):
                        val |= (1 << b)
                perm[i] = val
            
            # Reconstruct
            sorted_ranks = np.frombuffer(sorted_data, dtype=np.uint8)[:orig_len]
            original_ranks = np.zeros(orig_len, dtype=np.uint8)
            original_ranks[perm.astype(int)] = sorted_ranks
            
            return inverse[original_ranks].tobytes()
        except:
            continue
    
    raise ValueError("Failed to decompress")

# ══════════════════════════════════════════════════════════════
# Approach 4: FreqSep + Hilbert + Delta (lossless)
# ══════════════════════════════════════════════════════════════

def freqsep_hilbert_compress(data):
    """
    FreqSep + Hilbert + Delta:
    1. FreqSep: map bytes → ranks
    2. Hilbert reorder
    3. Delta between neighbors
    4. Compress
    """
    arr = np.frombuffer(data, dtype=np.uint8)
    values, counts = np.unique(arr, return_counts=True)
    
    # FreqSep mapping
    order = np.argsort(-counts)
    mapping = np.zeros(256, dtype=np.uint8)
    for rank, val in enumerate(order):
        mapping[val] = rank
    
    transformed = mapping[arr]
    
    # Hilbert reorder
    n = len(data)
    side = 1
    while side * side < n:
        side *= 2
    
    order_h = side.bit_length() - 1
    
    def hilbert_index(x, y, order):
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
    
    # Build Hilbert mapping
    hilbert_to_linear = {}
    linear_to_hilbert = {}
    for y in range(side):
        for x in range(side):
            linear = y * side + x
            hilbert = hilbert_index(x, y, order_h)
            if linear < n:
                hilbert_to_linear[hilbert] = linear
                linear_to_hilbert[linear] = hilbert
    
    # Reorder
    reordered = np.zeros(n, dtype=np.uint8)
    for hilbert_pos in range(min(n, len(hilbert_to_linear))):
        linear_pos = hilbert_to_linear.get(hilbert_pos, hilbert_pos)
        if linear_pos < n:
            reordered[hilbert_pos] = transformed[linear_pos]
    
    # Delta between consecutive positions
    delta = np.zeros(n, dtype=np.uint8)
    delta[0] = reordered[0]
    for i in range(1, n):
        delta[i] = ((reordered[i].astype(np.int16) - reordered[i-1].astype(np.int16)) % 256).astype(np.uint8)
    
    # Compress
    compressed = zlib.compress(delta.tobytes(), 9)
    
    # Store: mapping (256B) + compressed
    return mapping.tobytes() + compressed

def freqsep_hilbert_decompress(compressed_data):
    """Decompress FreqSep + Hilbert + Delta."""
    # Extract mapping (first 256 bytes)
    mapping = np.frombuffer(compressed_data[:256], dtype=np.uint8)
    inverse = np.zeros(256, dtype=np.uint8)
    for rank, val in enumerate(mapping):
        inverse[val] = rank
    
    # Decompress delta (starts at byte 256)
    delta = np.frombuffer(zlib.decompress(compressed_data[256:]), dtype=np.uint8)
    n = len(delta)
    
    # Reconstruct from deltas
    hilbert_order = np.zeros(n, dtype=np.uint8)
    hilbert_order[0] = delta[0]
    for i in range(1, n):
        hilbert_order[i] = ((hilbert_order[i-1].astype(np.int16) + delta[i].astype(np.int16)) % 256).astype(np.uint8)
    
    # Reverse Hilbert reorder (simplified - use inverse mapping)
    side = 1
    while side * side < n:
        side *= 2
    
    order_h = side.bit_length() - 1
    
    def hilbert_index(x, y, order):
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
    
    # Build linear → hilbert mapping
    linear_to_hilbert = {}
    for y in range(side):
        for x in range(side):
            linear = y * side + x
            hilbert = hilbert_index(x, y, order_h)
            if linear < n:
                linear_to_hilbert[linear] = hilbert
    
    # Reverse: hilbert_order → original order
    original = np.zeros(n, dtype=np.uint8)
    for linear_pos in range(n):
        hilbert_pos = linear_to_hilbert.get(linear_pos, linear_pos)
        if hilbert_pos < n:
            original[linear_pos] = hilbert_order[hilbert_pos]
    
    # Apply inverse FreqSep
    return inverse[original].tobytes()

# ══════════════════════════════════════════════════════════════
# Test
# ══════════════════════════════════════════════════════════════

def test_approach(data, name, compress_fn, decompress_fn):
    """Test one approach with roundtrip verification."""
    t0 = time.time()
    compressed = compress_fn(data)
    t_comp = time.time() - t0
    
    t0 = time.time()
    try:
        decompressed = decompress_fn(compressed)
        t_decomp = time.time() - t0
        
        match = decompressed == data
        return {
            'name': name,
            'input': len(data),
            'compressed': len(compressed),
            'ratio': len(compressed) / len(data),
            'roundtrip': match,
            'comp_ms': t_comp * 1000,
            'decomp_ms': t_decomp * 1000,
        }
    except Exception as e:
        return {
            'name': name,
            'input': len(data),
            'compressed': len(compressed),
            'ratio': len(compressed) / len(data),
            'roundtrip': False,
            'error': str(e),
            'comp_ms': t_comp * 1000,
        }

if __name__ == '__main__':
    print("=" * 80)
    print("  Combining 3 Lossy Approaches → Lossless Compression")
    print("=" * 80)
    
    pdf_path = r'I:\FGLS_new\docs\POGLS_MASTER_SCHEMATIC.pdf'
    full_pdf = open(pdf_path, 'rb').read()
    
    test_cases = [
        ("PDF 10KB", full_pdf[:10000]),
        ("PDF 100KB", full_pdf[:100000]),
        ("Random 10KB", os.urandom(10000)),
        ("Random 100KB", os.urandom(100000)),
        ("Text 10KB", (b"Hello world! " * 1000)[:10000]),
    ]
    
    approaches = [
        ("Raw+zlib", lambda d: zlib.compress(d, 9), lambda c: zlib.decompress(c)),
        ("FreqSep", freqsep_compress, freqsep_decompress),
        ("FreqSep+RowDelta", freqsep_rowdelta_compress, freqsep_rowdelta_decompress),
        ("FreqSep+Sort", freqsep_sort_compress, freqsep_sort_decompress),
        ("FreqSep+Hilbert+Delta", freqsep_hilbert_compress, freqsep_hilbert_decompress),
    ]
    
    for label, data in test_cases:
        print(f"\n{'='*80}")
        print(f"  {label} ({len(data):,} bytes)")
        print(f"{'='*80}")
        
        results = []
        for name, comp_fn, decomp_fn in approaches:
            try:
                r = test_approach(data, name, comp_fn, decomp_fn)
                results.append(r)
            except Exception as e:
                print(f"  {name:<25} ERROR: {e}")
        
        # Sort by ratio
        results.sort(key=lambda r: r['ratio'])
        
        for r in results:
            sym = "PASS" if r.get('roundtrip') else "FAIL"
            err = f" [{r.get('error', '')}]" if 'error' in r else ""
            print(f"  {r['name']:<25} {r['compressed']:>8,} bytes ({r['ratio']:.3f}x) | {sym}{err} | {r.get('comp_ms',0):.0f}ms")
        
        best = results[0]
        raw = next(r for r in results if r['name'] == 'Raw+zlib')
        if best['ratio'] < raw['ratio']:
            print(f"\n  >>> WINNER: {best['name']} ({best['ratio']:.3f}x vs raw {raw['ratio']:.3f}x)")
        else:
            print(f"\n  >>> Raw+zlib still best ({raw['ratio']:.3f}x)")
