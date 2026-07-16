#!/usr/bin/env python3
"""
Test: Correct geo_frame_seek with cycle counter and proper data placement

Key corrections:
1. Cycle counter (not just position)
2. 1440 = 120 × 12 (adjustable, not fixed)
3. Data on floor layer (not zeros)
4. Metatron reshape structure

Hypothesis: If data is placed on floor layer via geo_pixel_encode,
it becomes timeline-derived and can be compressed.
"""
import os, sys, time, zlib, hashlib, struct
import numpy as np

sys.path.insert(0, 'I:/FGLS_new/tools')

# ══════════════════════════════════════════════════════════════
# Constants from geo_metatron_reshape.h
# ══════════════════════════════════════════════════════════════

TRING_CYCLE = 1440
TRING_STRIDE = 37
META_HALF = 720
PEANO_GRID = 81
HILBERT_GRID = 64
CROP_ACTIVE = 36
SHADOW_COUNT = 28

# ══════════════════════════════════════════════════════════════
# Metatron reshape (from geo_metatron_reshape.h)
# ══════════════════════════════════════════════════════════════

def peano_l1_lut(idx):
    """Peano L1 LUT (9 entries, S-shape)."""
    coords = [
        (0,0), (0,1), (0,2),
        (1,2), (1,1), (1,0),
        (2,0), (2,1), (2,2)
    ]
    return coords[idx]

def peano_l2(idx):
    """Peano L2: idx → (row, col) on 9×9."""
    p_hi = idx // 9
    p_lo = idx % 9
    c1 = peano_l1_lut(p_hi)
    c0 = peano_l1_lut(p_lo)
    
    row = c1[0] * 3 + c0[0]
    if c1[0] & 1:
        col = c1[1] * 3 + (2 - c0[1])
    else:
        col = c1[1] * 3 + c0[1]
    
    return (row, col)

def peano_crop(row, col):
    """Peano crop: (row, col) → (role, cell_id)."""
    if row < 1 or row > 6 or col < 1 or col > 6:
        return ('SHADOW', None)
    
    r = row - 1
    c = col - 1
    cell_id = r * 6 + c
    
    # 4 corners
    if (r == 0 or r == 5) and (c == 0 or c == 5):
        return ('VERTEX', cell_id)
    
    # mid-edge boundary
    if (r == 0 or r == 5) and (c == 2 or c == 3):
        return ('EDGE', cell_id)
    if (c == 0 or c == 5) and (r == 2 or r == 3):
        return ('EDGE', cell_id)
    
    # interior 4×4 = core
    if r >= 1 and r <= 4 and c >= 1 and c <= 4:
        return ('CORE', cell_id)
    
    return ('EDGE', cell_id)

def ico_enc(pole, peano_idx):
    """ico_enc: (pole, peano_idx) → 0..161."""
    return pole * PEANO_GRID + peano_idx

def ico_decompose(ico_idx):
    """ico_decompose: ico_idx → (pole, peano_idx)."""
    return (ico_idx // PEANO_GRID, ico_idx % PEANO_GRID)

def ico_cpair(ico_idx):
    """ico_cpair: north↔south flip within icosphere."""
    return (ico_idx + PEANO_GRID) % (PEANO_GRID * 2)

def ico_meta_cpair(enc):
    """ico_meta_cpair: (enc + 720) % 1440 — diameter line."""
    return (enc + META_HALF) % TRING_CYCLE

# ══════════════════════════════════════════════════════════════
# geo_pixel_encode (from geopixel_pipeline.py)
# ══════════════════════════════════════════════════════════════

def geo_pixel_encode(idx, W=256):
    """geo_pixel_encode: idx → RGB with trit/spoke/coset/letter/fibo fields."""
    fibo = idx % 144  # fibo clock position
    
    # Trit (base-3 representation)
    trit = [0, 0, 0]
    v = fibo
    for i in range(3):
        trit[i] = v % 3
        v //= 3
    
    # Spoke (which of 12 edges)
    spoke = fibo % 12
    
    # Coset (which of 12 faces)
    coset = fibo // 12
    
    # Letter (which of 4 letters)
    letter = fibo % 4
    
    # RGB encoding
    R = (spoke * 21) % W  # 12 spokes × 21 = 252
    G = (coset * 21) % W  # 12 cosets × 21 = 252
    B = (letter * 64) % W  # 4 letters × 64 = 256
    
    return (R, G, B)

# ══════════════════════════════════════════════════════════════
# Test: Data on floor layer with cycle counter
# ══════════════════════════════════════════════════════════════

def test_floor_layer_compress(data, label):
    """Test compression with data on floor layer."""
    print(f"\n{'='*70}")
    print(f"  {label} ({len(data):,} bytes)")
    print(f"{'='*70}")
    
    orig_hash = hashlib.sha256(data).hexdigest()[:16]
    print(f"  Original hash: {orig_hash}")
    
    # Split data into 64-byte chunks
    chunk_sz = 64
    n_chunks = (len(data) + chunk_sz - 1) // chunk_sz
    
    # How many cycles? (each cycle = 1440 chunks)
    n_cycles = (n_chunks + TRING_CYCLE - 1) // TRING_CYCLE
    
    print(f"  Chunks: {n_chunks}")
    print(f"  Cycles: {n_cycles} (each = {TRING_CYCLE} chunks)")
    print(f"  Total chunks needed: {n_cycles * TRING_CYCLE}")
    
    # Place data on floor layer via geo_pixel_encode
    # Each chunk is encoded to a position on the floor layer
    floor_data = []
    for i in range(n_cycles * TRING_CYCLE):
        if i < n_chunks:
            # Get chunk data
            start = i * chunk_sz
            end = min(start + chunk_sz, len(data))
            chunk = data[start:end]
            # Pad to chunk_sz
            chunk = chunk + b'\x00' * (chunk_sz - len(chunk))
        else:
            # Empty slot — encode position as data (NOT zeros!)
            pos = geo_pixel_encode(i)
            chunk = bytes(pos) + b'\x00' * (chunk_sz - 3)
        
        floor_data.append(chunk)
    
    # Convert to bytes
    floor_bytes = b''.join(floor_data)
    
    print(f"  Floor data size: {len(floor_bytes):,} bytes")
    
    # Compress
    compressed = zlib.compress(floor_bytes, 9)
    print(f"  Compressed: {len(compressed):,} bytes")
    ratio = len(compressed) / len(data)
    print(f"  Ratio (compressed / original): {ratio:.3f}x")
    
    # Compare with raw zlib
    raw_zlib = zlib.compress(data, 9)
    print(f"\n  --- Comparison ---")
    print(f"  Raw+zlib:      {len(raw_zlib):>8,} bytes ({len(raw_zlib)/len(data):.3f}x)")
    print(f"  Floor+zlib:    {len(compressed):>8,} bytes ({ratio:.3f}x)")
    
    if ratio < len(raw_zlib)/len(data):
        print(f"  Floor is {(len(raw_zlib)/len(data))/ratio:.2f}x better")
    else:
        print(f"  Raw+zlib is better")
    
    # Analyze floor data structure
    print(f"\n  --- Floor Data Analysis ---")
    n_nonzero = sum(1 for c in floor_bytes if c != 0)
    print(f"  Non-zero bytes: {n_nonzero:,} ({n_nonzero/len(floor_bytes)*100:.1f}%)")
    
    # Check if data has timeline structure
    # Timeline structure = each chunk derivable from previous chunk
    derivable = 0
    for i in range(1, min(n_chunks, TRING_CYCLE)):
        chunk_prev = floor_data[i-1]
        chunk_curr = floor_data[i]
        # Simple test: is chunk_curr derivable from chunk_prev?
        # In real pipeline, this uses timeline function
        # Here we just check if there's any pattern
        if any(chunk_curr[j] != chunk_prev[j] for j in range(chunk_sz)):
            derivable += 1
    
    print(f"  Chunks with change from previous: {derivable}/{min(n_chunks-1, TRING_CYCLE-1)}")
    
    return {
        'label': label,
        'input': len(data),
        'floor_size': len(floor_bytes),
        'compressed': len(compressed),
        'raw_zlib': len(raw_zlib),
        'ratio': ratio,
        'raw_ratio': len(raw_zlib)/len(data),
        'n_chunks': n_chunks,
        'n_cycles': n_cycles,
        'nonzero_pct': n_nonzero/len(floor_bytes)*100,
    }

if __name__ == '__main__':
    print("=" * 70)
    print("  Floor Layer Compression Test")
    print("=" * 70)
    print("  Key: Data on floor layer (NOT zeros) + cycle counter")
    print("  geo_pixel_encode: idx → RGB with trit/spoke/coset/letter/fibo")
    print("  Metatron reshape: World A (64) + World B (36) = DiamondBlock")
    
    pdf_path = r'I:\FGLS_new\docs\POGLS_MASTER_SCHEMATIC.pdf'
    full_pdf = open(pdf_path, 'rb').read()
    
    test_cases = [
        ("PDF 10KB", full_pdf[:10000]),
        ("PDF 100KB", full_pdf[:100000]),
        ("Random 10KB", os.urandom(10000)),
        ("Random 100KB", os.urandom(100000)),
        ("Text 10KB", (b"Hello world! " * 1000)[:10000]),
    ]
    
    results = []
    for label, data in test_cases:
        r = test_floor_layer_compress(data, label)
        results.append(r)
    
    # Summary
    print("\n" + "=" * 70)
    print("  SUMMARY")
    print("=" * 70)
    print(f"  {'Data':<15} {'Input':<10} {'Cycles':<8} {'Floor':<10} {'Compressed':<12} {'Raw+zlib':<12}")
    print("-" * 70)
    
    for r in results:
        print(f"  {r['label']:<15} {r['input']:<10,} {r['n_cycles']:<8} {r['floor_size']:<10,} {r['compressed']:<12,} {r['raw_zlib']:<12,}")
    
    print("\n" + "=" * 70)
    print("  KEY INSIGHT")
    print("=" * 70)
    print("  Floor layer = data + geo_pixel_encode positions (NOT zeros)")
    print("  Each position encodes: trit, spoke, coset, letter, fibo")
    print("  Empty slots = position encoding (meaningful, not zeros)")
    print("  If data is timeline-derived → reconstruction possible")
    print("  If data is arbitrary → reconstruction impossible")
