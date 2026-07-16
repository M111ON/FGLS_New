#!/usr/bin/env python3
"""
Test: geo_pixel_encode with timeline structure (fibo field)

Key insight: geo_pixel_encode embeds fibo clock position (idx % 144) 
in the B channel, creating timeline structure automatically.

Pipeline:
1. Data → geo_pixel_encode → RGB pixels (with timeline)
2. Compress with zlib/zstd
3. Verify roundtrip
"""
import os, sys, time, zlib, hashlib
import numpy as np

sys.path.insert(0, 'I:/FGLS_new/tools')

# ══════════════════════════════════════════════════════════════
# GeoPixel Encoding (from geo_pixel.h)
# ══════════════════════════════════════════════════════════════

GP_TRIT_MOD = 27
GP_SPOKE_MOD = 6
GP_COSET_MOD = 9
GP_LETTER_MOD = 26
GP_FIBO_MOD = 144
GP_GRID_W = 27

def geo_pixel_encode(idx, W=GP_GRID_W):
    """Encode index → GeoPixel (R, G, B) with timeline structure."""
    i = idx % W if W > 0 else idx
    r = ((i % GP_TRIT_MOD) << 3) | (i % GP_SPOKE_MOD)
    g = ((i % GP_COSET_MOD) << 4) | (i % GP_LETTER_MOD & 0xF)
    b = i % GP_FIBO_MOD
    return r, g, b

def geo_pixel_decode(r, g, b):
    """Decode GeoPixel → fields."""
    trit = (r >> 3) % GP_TRIT_MOD
    spoke = r & 0x7
    coset = (g >> 4) % GP_COSET_MOD
    letter = g & 0xF
    fibo = b
    return trit, spoke, coset, letter, fibo

# ══════════════════════════════════════════════════════════════
# O4 Connector (from geo_o4_connector.h)
# ══════════════════════════════════════════════════════════════

O4_CHUNK_BYTES = 3
O4_GRID_W = 27

def o4_encode(data):
    """Encode data → GeoPixel grid (27×N)."""
    n_chunks = (len(data) + O4_CHUNK_BYTES - 1) // O4_CHUNK_BYTES
    
    # Create grid
    grid_h = (n_chunks + O4_GRID_W - 1) // O4_GRID_W
    grid = np.zeros((grid_h, O4_GRID_W, 3), dtype=np.uint8)
    
    for i in range(n_chunks):
        # Read 3-byte chunk
        byte0 = data[i*3] if i*3 < len(data) else 0
        byte1 = data[i*3+1] if i*3+1 < len(data) else 0
        byte2 = data[i*3+2] if i*3+2 < len(data) else 0
        
        # Encode slot_idx as GeoPixel
        r, g, b = geo_pixel_encode(i, O4_GRID_W)
        
        # XOR data into RGB channels
        r ^= byte0
        g ^= byte1
        b ^= byte2
        
        # Place in grid
        row = i // O4_GRID_W
        col = i % O4_GRID_W
        grid[row, col] = [r, g, b]
    
    return grid, n_chunks

def o4_decode(grid, n_chunks, orig_len):
    """Decode GeoPixel grid → data."""
    data = bytearray()
    
    for i in range(n_chunks):
        row = i // O4_GRID_W
        col = i % O4_GRID_W
        
        r, g, b = grid[row, col]
        
        # Recover geometry pixel
        r_geo, g_geo, b_geo = geo_pixel_encode(i, O4_GRID_W)
        
        # XOR back to get original data
        byte0 = r ^ r_geo
        byte1 = g ^ g_geo
        byte2 = b ^ b_geo
        
        data.append(byte0)
        if len(data) < orig_len:
            data.append(byte1)
        if len(data) < orig_len:
            data.append(byte2)
    
    return bytes(data[:orig_len])

# ══════════════════════════════════════════════════════════════
# Test
# ══════════════════════════════════════════════════════════════

def test_geopixel_compress(data, label):
    """Test geo_pixel_encode + compression."""
    print(f"\n{'='*70}")
    print(f"  {label} ({len(data):,} bytes)")
    print(f"{'='*70}")
    
    orig_hash = hashlib.sha256(data).hexdigest()[:16]
    print(f"  Original hash: {orig_hash}")
    
    # Encode with geo_pixel
    t0 = time.time()
    grid, n_chunks = o4_encode(data)
    t_enc = time.time() - t0
    
    grid_bytes = grid.tobytes()
    print(f"  Grid size: {len(grid_bytes):,} bytes")
    print(f"  Grid shape: {grid.shape}")
    print(f"  Encode time: {t_enc*1000:.1f}ms")
    
    # Compress grid with zlib
    t0 = time.time()
    compressed = zlib.compress(grid_bytes, 9)
    t_comp = time.time() - t0
    
    ratio = len(compressed) / len(data)
    print(f"  Compressed: {len(compressed):,} bytes ({ratio:.3f}x)")
    print(f"  Compress time: {t_comp*1000:.1f}ms")
    
    # Decompress
    t0 = time.time()
    decompressed_grid = np.frombuffer(zlib.decompress(compressed), dtype=np.uint8).reshape(grid.shape)
    t_decomp = time.time() - t0
    
    # Decode
    t0 = time.time()
    decoded = o4_decode(decompressed_grid, n_chunks, len(data))
    t_dec = time.time() - t0
    
    recon_hash = hashlib.sha256(decoded).hexdigest()[:16]
    match = decoded == data
    
    print(f"  Decoded hash: {recon_hash}")
    print(f"  Roundtrip: {'PASS' if match else 'FAIL'}")
    print(f"  Decode time: {t_dec*1000:.1f}ms")
    
    # Compare with raw zlib
    raw_zlib = zlib.compress(data, 9)
    print(f"\n  --- Comparison ---")
    print(f"  Raw+zlib:     {len(raw_zlib):>8,} bytes ({len(raw_zlib)/len(data):.3f}x)")
    print(f"  GeoPixel+zlib: {len(compressed):>8,} bytes ({ratio:.3f}x)")
    
    improvement = len(raw_zlib) / len(compressed) if len(compressed) > 0 else 0
    print(f"  GeoPixel is {improvement:.2f}x {'smaller' if improvement > 1 else 'larger'}")
    
    return {
        'label': label,
        'input': len(data),
        'grid_size': len(grid_bytes),
        'compressed': len(compressed),
        'raw_zlib': len(raw_zlib),
        'ratio': ratio,
        'raw_ratio': len(raw_zlib)/len(data),
        'roundtrip': match,
        'improvement': improvement,
    }

if __name__ == '__main__':
    print("=" * 70)
    print("  GeoPixel Encoding + Compression Test")
    print("=" * 70)
    print("  Key: geo_pixel_encode embeds fibo clock (idx % 144) in B channel")
    print("       This creates timeline structure automatically")
    
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
        r = test_geopixel_compress(data, label)
        results.append(r)
    
    # Summary
    print("\n" + "=" * 70)
    print("  SUMMARY")
    print("=" * 70)
    print(f"  {'Data':<15} {'Input':<10} {'Grid':<10} {'GP+zlib':<12} {'Raw+zlib':<12} {'Better?':<10}")
    print("-" * 70)
    
    for r in results:
        better = "YES" if r['improvement'] > 1 else "NO"
        print(f"  {r['label']:<15} {r['input']:<10,} {r['grid_size']:<10,} {r['compressed']:<12,} {r['raw_zlib']:<12,} {better:<10}")
    
    print("\n" + "=" * 70)
    print("  ANALYSIS")
    print("=" * 70)
    print("  GeoPixel encoding expands data (3 bytes → 3 bytes in grid)")
    print("  But grid has geometric structure (trit, spoke, coset, letter, fibo)")
    print("  zlib compresses the geometric structure")
    print("  Question: does the structure help zlib compress better than raw data?")
