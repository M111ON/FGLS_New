import os, sys, hashlib, time
sys.path.insert(0, 'I:/FGLS_new/tools')
from geopixel_pipeline import pipeline_encode, pipeline_decode

pdf_path = r'I:\FGLS_new\docs\POGLS_MASTER_SCHEMATIC.pdf'
full_data = open(pdf_path, 'rb').read()
print(f"Full PDF: {len(full_data):,} bytes")

# Test 1: First 100KB
for sz in [100000, 500000, 1000000]:
    data = full_data[:sz]
    print(f"\n{'='*60}")
    print(f"  PDF test: {sz:,} bytes ({sz/1024:.0f} KB)")
    print(f"{'='*60}")
    print(f"  Hash: {hashlib.sha256(data).hexdigest()[:16]}")

    t0 = time.time()
    result = pipeline_encode(data)
    t_enc = time.time() - t0
    encoded = result.encoded if hasattr(result, 'encoded') else b''
    ratio = len(data) / len(encoded) if len(encoded) > 0 else 0
    print(f"  Encoded: {len(encoded):,} bytes ({ratio:.2f}x) [{t_enc*1000:.0f}ms]")

    t0 = time.time()
    dec = pipeline_decode(encoded)
    t_dec = time.time() - t0
    decoded = dec['data'][:len(data)]
    match = decoded == data
    print(f"  Decoded: {len(decoded):,} bytes [{t_dec*1000:.0f}ms]")
    print(f"  Roundtrip: {'PASS' if match else 'FAIL'}")
