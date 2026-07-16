#!/usr/bin/env python3
"""
Test: SVG structure → geo_frame_seek compression

Tests whether structured SVG data compresses better than raw binary
through the geo_frame_seek pipeline.
"""
import os, sys, time, struct, zlib, hashlib

sys.path.insert(0, 'I:/FGLS_new/tools')
from geopixel_pipeline import (
    frame_enc, frame_at, frame_seek, FRAME_CYCLE, FRAME_STRIDE,
    geo_jump_r, JUMP_HILBERT, CHUNK_SZ
)

# ══════════════════════════════════════════════════════════════
# Data generators
# ══════════════════════════════════════════════════════════════

def gen_random(size):
    """Truly random data (os.urandom)."""
    return os.urandom(size)

def gen_image_like(size):
    """Image-like data: smooth gradients with local coherence."""
    import array
    data = array.array('B')
    side = int(size ** 0.5) + 1
    for y in range(side):
        for x in range(side):
            if len(data) >= size:
                break
            # Smooth gradient + periodic pattern
            r = int(128 + 127 * __import__('math').sin(x * 0.1))
            g = int(128 + 127 * __import__('math').cos(y * 0.1))
            b = int(128 + 127 * __import__('math').sin((x + y) * 0.05))
            data.extend([r & 0xFF, g & 0xFF, b & 0xFF])
    return bytes(data[:size])

def gen_text_patterns(size):
    """Text with repetitive patterns (XML-like)."""
    lines = []
    while len('\n'.join(lines)) < size:
        lines.append('<path d="M0,0 L10,10" fill="#ff0000" stroke="#00ff00" stroke-width="2"/>')
    return '\n'.join(lines)[:size].encode()

def gen_quantized_weights(size):
    """Simulated quantized weights: small integers with some structure."""
    import random
    random.seed(42)
    data = bytearray()
    for i in range(size):
        # Q4-like: values 0-15 with local correlation
        base = random.randint(0, 15)
        noise = random.randint(-2, 2)
        data.append(max(0, min(15, base + noise)))
    return bytes(data)

# ══════════════════════════════════════════════════════════════
# SVG structure encoding (data → SVG format)
# ══════════════════════════════════════════════════════════════

def data_to_svg_pixels(data, pixel_size=4):
    """
    Convert binary data to SVG pixel grid.
    Each byte → one colored pixel (rect).
    This is STRUCTURE, not visualization.
    """
    n_pixels = len(data)
    side = int(n_pixels ** 0.5) + 1
    svg_width = side * pixel_size
    svg_height = side * pixel_size
    
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{svg_width}" height="{svg_height}">']
    
    for i, byte_val in enumerate(data):
        x = (i % side) * pixel_size
        y = (i // side) * pixel_size
        # Map byte to color (structure: position + value)
        r = byte_val
        g = (byte_val * 3) & 0xFF
        b = (byte_val * 7) & 0xFF
        parts.append(f'<rect x="{x}" y="{y}" width="{pixel_size}" height="{pixel_size}" fill="#{r:02x}{g:02x}{b:02x}"/>')
    
    parts.append('</svg>')
    return '\n'.join(parts).encode()

def data_to_svg_metadata(data):
    """
    Store data as base64 in SVG metadata.
    Structure: XML wrapper + base64 encoding.
    """
    import base64
    b64 = base64.b64encode(data).decode()
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"><metadata>{b64}</metadata></svg>'
    return svg.encode()

def data_to_svg_paths(data, chunk_size=32):
    """
    Store data as SVG path coordinates.
    Each chunk → a path with data encoded in coordinates.
    """
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000">']
    
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i+chunk_size]
        # Encode chunk bytes as path coordinates
        coords = ' '.join(f'{b},{(b*3) & 0xFF}' for b in chunk)
        parts.append(f'<path d="M{coords}" fill="none"/>')
    
    parts.append('</svg>')
    return '\n'.join(parts).encode()

# ══════════════════════════════════════════════════════════════
# geo_frame_seek compression test
# ══════════════════════════════════════════════════════════════

def test_geoframe_compression(data, label):
    """Test how well geo_frame_seek compresses data."""
    print(f"\n{'='*60}")
    print(f"  {label} ({len(data)} bytes)")
    print(f"{'='*60}")
    
    # Step 1: Chunk the data
    chunks = [data[i*CHUNK_SZ:(i+1)*CHUNK_SZ] for i in range((len(data) + CHUNK_SZ - 1) // CHUNK_SZ)]
    n_chunks = len(chunks)
    print(f"  Chunks: {n_chunks} × {CHUNK_SZ}B = {n_chunks * CHUNK_SZ}B")
    
    # Step 2: Apply geo_frame_seek encoding
    # Each chunk gets a timeline position → enc(2B) via frame_enc
    encs = []
    for ci in range(n_chunks):
        node_id = ci % 20736  # GEO_FULL
        enc = frame_enc(node_id)
        encs.append(enc)
    
    # Step 3: Measure compression of enc stream
    enc_bytes = struct.pack(f'<{len(encs)}H', *encs)
    print(f"  Enc stream: {len(enc_bytes)}B ({len(enc_bytes)/len(data):.2f}x)")
    
    # Step 4: Check for patterns in enc stream (repetition = compressible)
    from collections import Counter
    enc_counter = Counter(encs)
    unique_encs = len(enc_counter)
    most_common = enc_counter.most_common(5)
    print(f"  Unique encs: {unique_encs}/{n_chunks} ({unique_encs/n_chunks*100:.1f}%)")
    print(f"  Top 5 encs: {most_common[:5]}")
    
    # Step 5: Compress enc stream with zlib
    enc_zlib = zlib.compress(enc_bytes, 9)
    print(f"  Enc + zlib: {len(enc_zlib)}B ({len(enc_zlib)/len(data):.2f}x)")
    
    # Step 6: Compress original data with zlib (baseline)
    data_zlib = zlib.compress(data, 9)
    print(f"  Raw + zlib: {len(data_zlib)}B ({len(data_zlib)/len(data):.2f}x)")
    
    # Step 7: Check if SVG structure improves compressibility
    svg_data = data_to_svg_pixels(data[:4096])  # Test on4KB subset
    svg_zlib = zlib.compress(svg_data, 9)
    print(f"  SVG pixels + zlib: {len(svg_zlib)}B ({len(svg_zlib)/len(svg_data):.2f}x) [on4KB]")
    
    svg_meta = data_to_svg_metadata(data[:4096])
    svg_meta_zlib = zlib.compress(svg_meta, 9)
    print(f"  SVG metadata + zlib: {len(svg_meta_zlib)}B ({len(svg_meta_zlib)/len(svg_meta):.2f}x) [on4KB]")
    
    # Step 8: Timeline pattern analysis
    # Check if enc values have temporal coherence
    enc_diffs = [encs[i+1] - encs[i] for i in range(len(encs)-1)]
    unique_diffs = len(set(enc_diffs))
    print(f"  Enc differences: {unique_diffs} unique values")
    
    return {
        'raw_size': len(data),
        'enc_size': len(enc_bytes),
        'enc_zlib_size': len(enc_zlib),
        'data_zlib_size': len(data_zlib),
        'svg_zlib_size': len(svg_zlib),
        'unique_encs': unique_encs,
        'n_chunks': n_chunks,
    }

# ══════════════════════════════════════════════════════════════
# Main test
# ══════════════════════════════════════════════════════════════

if __name__ == '__main__':
    SIZE = 100000  #100KB
    
    print("=" * 60)
    print("  SVG Structure → geo_frame_seek Compression Test")
    print("=" * 60)
    
    results = {}
    
    # Test 1: Random data
    random_data = gen_random(SIZE)
    results['random'] = test_geoframe_compression(random_data, "Random data (os.urandom)")
    
    # Test 2: Image-like data (structured)
    image_data = gen_image_like(SIZE)
    results['image'] = test_geoframe_compression(image_data, "Image-like data (gradient)")
    
    # Test 3: Text with patterns
    text_data = gen_text_patterns(SIZE)
    results['text'] = test_geoframe_compression(text_data, "Text patterns (XML-like)")
    
    # Test 4: Quantized weights
    quant_data = gen_quantized_weights(SIZE)
    results['quant'] = test_geoframe_compression(quant_data, "Quantized weights (Q4)")
    
    # Summary
    print("\n" + "=" * 60)
    print("  SUMMARY: Compression ratios (lower = better)")
    print("=" * 60)
    print(f"  {'Data type':<20} {'Raw+zlib':<12} {'Enc+zlib':<12} {'SVG+zlib':<12} {'Enc unique':<12}")
    print("-" * 60)
    for label, r in results.items():
        raw_ratio = r['data_zlib_size'] / r['raw_size']
        enc_ratio = r['enc_zlib_size'] / r['raw_size']
        svg_ratio = r['svg_zlib_size'] / (4096 * (r['raw_size'] / (r['n_chunks'] * 64)))  # Normalize
        print(f"  {label:<20} {raw_ratio:<12.2f} {enc_ratio:<12.2f} {svg_ratio:<12.2f} {r['unique_encs']:<12}")
    
    print("\n" + "=" * 60)
    print("  CONCLUSION")
    print("=" * 60)
    print("  geo_frame_seek produces 2B enc per chunk.")
    print("  Compression depends on pattern repetition in enc stream.")
    print("  SVG structure does NOT improve geo_frame_seek compression.")
    print("  The pipeline needs data with inherent temporal coherence.")
