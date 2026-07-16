#!/usr/bin/env python3
"""
E2E Test: Full pipeline verification

Tests: Raw data → geo_field → encode → decode → verify byte-for-byte
"""
import os, sys, time, hashlib

sys.path.insert(0, 'I:/FGLS_new/tools')
from geopixel_pipeline import pipeline_encode, pipeline_decode

# ══════════════════════════════════════════════════════════════
# Data generators
# ══════════════════════════════════════════════════════════════

def gen_random(size):
    """Truly random data."""
    return os.urandom(size)

def gen_image_like(size):
    """Image-like: smooth gradients with local coherence."""
    data = bytearray()
    side = int(size ** 0.5) + 1
    import math
    for y in range(side):
        for x in range(side):
            if len(data) >= size:
                break
            r = int(128 + 127 * math.sin(x * 0.1))
            g = int(128 + 127 * math.cos(y * 0.1))
            b = int(128 + 127 * math.sin((x + y) * 0.05))
            data.extend([r & 0xFF, g & 0xFF, b & 0xFF])
    return bytes(data[:size])

def gen_text_patterns(size):
    """Text with repetitive patterns."""
    lines = []
    while len('\n'.join(lines)) < size:
        lines.append('<path d="M0,0 L10,10" fill="#ff0000" stroke="#00ff00" stroke-width="2"/>')
    return '\n'.join(lines)[:size].encode()

def gen_quantized_weights(size):
    """Simulated quantized weights: small integers."""
    import random
    random.seed(42)
    data = bytearray()
    for i in range(size):
        base = random.randint(0, 15)
        noise = random.randint(-2, 2)
        data.append(max(0, min(15, base + noise)))
    return bytes(data)

# ══════════════════════════════════════════════════════════════
# E2E Test
# ══════════════════════════════════════════════════════════════

def test_e2e(data, label):
    """Full E2E pipeline test."""
    print(f"\n{'='*60}")
    print(f"  {label} ({len(data):,} bytes)")
    print(f"{'='*60}")
    
    original_hash = hashlib.sha256(data).hexdigest()[:16]
    print(f"  Original hash: {original_hash}")
    
    # Encode
    print(f"\n  --- Encode ---")
    t0 = time.time()
    result = pipeline_encode(data)
    t_enc = time.time() - t0
    
    encoded = result.encoded if hasattr(result, 'encoded') else result.stats.get('encoded', b'')
    ratio = result.ratio if hasattr(result, 'ratio') else result.stats.get('ratio', 0)
    stats = result.stats
    
    print(f"  Encoded size: {len(encoded):,} bytes")
    print(f"  Compression ratio: {ratio:.2f}x")
    print(f"  Chunks: {stats['n_chunks']}")
    print(f"  Cube side: {stats['cube_side']}")
    print(f"  Mode: {stats.get('mode', 'unknown')}")
    print(f"  Fallback: {stats.get('fallback', False)}")
    print(f"  Encode time: {t_enc*1000:.1f}ms")
    
    # Decode
    print(f"\n  --- Decode ---")
    t0 = time.time()
    dec = pipeline_decode(encoded)
    t_dec = time.time() - t0
    
    decoded = dec['data']
    verified = dec.get('verified', 0)
    failed = dec.get('failed', 0)
    n_chunks = dec.get('n_chunks', 0)
    
    print(f"  Decoded size: {len(decoded):,} bytes")
    print(f"  Verified: {verified}/{n_chunks}")
    print(f"  Failed: {failed}")
    print(f"  Decode time: {t_dec*1000:.1f}ms")
    
    # Verify
    print(f"\n  --- Verify ---")
    # Trim decoded to original size (pipeline pads to chunk boundary)
    decoded_trimmed = decoded[:len(data)]
    decoded_hash = hashlib.sha256(decoded_trimmed).hexdigest()[:16]
    
    print(f"  Decoded hash: {decoded_hash}")
    
    # Byte-by-byte comparison
    if len(decoded_trimmed) != len(data):
        print(f"  Size mismatch: decoded={len(decoded_trimmed)}, original={len(data)}")
        match = False
    elif decoded_trimmed != data:
        # Find first mismatch
        for i in range(min(len(decoded_trimmed), len(data))):
            if decoded_trimmed[i] != data[i]:
                print(f"  First mismatch at byte {i}: decoded=0x{decoded_trimmed[i]:02x}, original=0x{data[i]:02x}")
                break
        match = False
    else:
        match = True
    
    print(f"  Roundtrip: {'PASS ✓' if match else 'FAIL ✗'}")
    
    # Summary
    print(f"\n  --- Summary ---")
    print(f"  Input:      {len(data):>10,} bytes")
    print(f"  Output:     {len(encoded):>10,} bytes")
    print(f"  Ratio:      {ratio:>10.2f}x")
    print(f"  Roundtrip:  {'PASS ✓' if match else 'FAIL ✗'}")
    
    return {
        'label': label,
        'input_size': len(data),
        'output_size': len(encoded),
        'ratio': ratio,
        'roundtrip': match,
        'encode_time': t_enc,
        'decode_time': t_dec,
    }

# ══════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════

if __name__ == '__main__':
    print("=" * 60)
    print("  E2E Pipeline Test: geo_field → encode → decode → verify")
    print("=" * 60)
    
    results = []
    
    # Test with different data types and sizes
    sizes = [1024, 10240, 102400]  # 1KB, 10KB, 100KB
    
    for size in sizes:
        # Random data
        data = gen_random(size)
        r = test_e2e(data, f"Random {size//1024}KB")
        results.append(r)
    
    # Image-like data
    data = gen_image_like(102400)
    r = test_e2e(data, "Image-like 100KB")
    results.append(r)
    
    # Text patterns
    data = gen_text_patterns(102400)
    r = test_e2e(data, "Text patterns 100KB")
    results.append(r)
    
    # Quantized weights
    data = gen_quantized_weights(102400)
    r = test_e2e(data, "Quantized weights 100KB")
    results.append(r)
    
    # Summary
    print("\n" + "=" * 60)
    print("  FINAL SUMMARY")
    print("=" * 60)
    print(f"  {'Test':<25} {'Input':<12} {'Output':<12} {'Ratio':<10} {'Roundtrip':<10}")
    print("-" * 60)
    for r in results:
        print(f"  {r['label']:<25} {r['input_size']:<12,} {r['output_size']:<12,} {r['ratio']:<10.2f} {'PASS' if r['roundtrip'] else 'FAIL'}")
    
    print("\n" + "=" * 60)
    print("  CONCLUSION")
    print("=" * 60)
    all_pass = all(r['roundtrip'] for r in results)
    if all_pass:
        print("  ✓ ALL TESTS PASSED")
        print("  ✓ Pipeline achieves lossless compression")
        print("  ✓ Data is preserved byte-for-byte")
    else:
        print("  ✗ SOME TESTS FAILED")
        print("  ✗ Pipeline does NOT achieve lossless compression")
        print("  ✗ Data is NOT preserved")
