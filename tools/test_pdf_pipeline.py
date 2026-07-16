#!/usr/bin/env python3
"""
Test: PDF files through pipeline — arbitrary data test
"""
import os, sys, time, hashlib

sys.path.insert(0, 'I:/FGLS_new/tools')
from geopixel_pipeline import pipeline_encode, pipeline_decode

def test_file(filepath):
    """Test a file through the pipeline."""
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        return None
    
    data = open(filepath, 'rb').read()
    filename = os.path.basename(filepath)
    
    print(f"\n{'='*60}")
    print(f"  {filename} ({len(data):,} bytes)")
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
    decoded_trimmed = decoded[:len(data)]
    decoded_hash = hashlib.sha256(decoded_trimmed).hexdigest()[:16]
    
    print(f"  Decoded hash: {decoded_hash}")
    
    if len(decoded_trimmed) != len(data):
        print(f"  Size mismatch: decoded={len(decoded_trimmed)}, original={len(data)}")
        match = False
    elif decoded_trimmed != data:
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
        'filename': filename,
        'input_size': len(data),
        'output_size': len(encoded),
        'ratio': ratio,
        'roundtrip': match,
    }

if __name__ == '__main__':
    print("=" * 60)
    print("  PDF Pipeline Test — Arbitrary Data")
    print("=" * 60)
    
    pdf_files = [
        'I:/FGLS_new/docs/POGLS_MASTER_SCHEMATIC.pdf',
        'I:/FGLS_new/collection/docs/Metatron\'s Routing Built in LUT with no pointer.pdf',
    ]
    
    results = []
    for pdf_path in pdf_files:
        r = test_file(pdf_path)
        if r:
            results.append(r)
    
    # Summary
    print("\n" + "=" * 60)
    print("  FINAL SUMMARY")
    print("=" * 60)
    print(f"  {'File':<40} {'Input':<12} {'Output':<12} {'Ratio':<10} {'Roundtrip':<10}")
    print("-" * 60)
    for r in results:
        print(f"  {r['filename']:<40} {r['input_size']:<12,} {r['output_size']:<12,} {r['ratio']:<10.2f} {'PASS' if r['roundtrip'] else 'FAIL'}")
    
    print("\n" + "=" * 60)
    print("  CONCLUSION")
    print("=" * 60)
    all_pass = all(r['roundtrip'] for r in results)
    if all_pass:
        print("  ✓ ALL TESTS PASSED")
        print("  ✓ Pipeline works with arbitrary data (PDF)")
        print("  ✓ Lossless roundtrip verified")
    else:
        print("  ✗ SOME TESTS FAILED")
        print("  ✗ Pipeline does NOT work with arbitrary data")
