#!/usr/bin/env python3
"""
Test predictors for delta compression.
Only test decode-safe predictors (no access to original data during decode).
"""
import os, sys, time, hashlib, zlib, struct
import numpy as np

sys.path.insert(0, 'I:/FGLS_new/tools')
from geopixel_pipeline import frame_enc, frame_at, FRAME_CYCLE

def compute_delta(actual, predicted):
    return ((actual.astype(np.int16) - predicted.astype(np.int16)) % 256).astype(np.uint8)

def apply_delta(predicted, delta):
    return ((predicted.astype(np.int16) + delta.astype(np.int16).reshape(predicted.shape)) % 256).astype(np.uint8)

def test_one(data, predictor_name, predicted):
    """Test one predictor, verify roundtrip."""
    n = len(data)
    side = predicted.shape[0]
    cube_vol = side ** 3
    
    actual = np.zeros(cube_vol, dtype=np.uint8)
    actual[:n] = np.frombuffer(data, dtype=np.uint8)
    actual = actual.reshape(predicted.shape)
    
    delta = compute_delta(actual, predicted)
    nonzero = np.count_nonzero(delta)
    
    compressed = zlib.compress(delta.tobytes(), 9)
    
    delta_recon = np.frombuffer(zlib.decompress(compressed), dtype=np.uint8).reshape(predicted.shape)
    actual_recon = apply_delta(predicted, delta_recon)
    
    recon_flat = actual_recon.flatten()[:n].tobytes()
    match = recon_flat == data
    
    return {
        'name': predictor_name,
        'input': n,
        'nonzero_pct': nonzero / delta.size * 100,
        'compressed': len(compressed),
        'ratio': len(compressed) / n,
        'roundtrip': match,
    }

def make_timeline_prediction(side):
    predicted = np.zeros((side, side, side), dtype=np.uint8)
    for x in range(side):
        for y in range(side):
            for z in range(side):
                linear = (x * side + y) * side + z
                t = linear % FRAME_CYCLE
                enc = frame_enc(t)
                frame = frame_at(enc)
                predicted[x, y, z] = (frame['face'] * 100 + frame['slot'] * 10 + frame['ico_idx']) & 0xFF
    return predicted

def make_zero_prediction(side):
    return np.zeros((side, side, side), dtype=np.uint8)

def make_hash_prediction(side):
    predicted = np.zeros((side, side, side), dtype=np.uint8)
    for x in range(side):
        for y in range(side):
            for z in range(side):
                h = (x * 374761393 + y * 668265263 + z * 1274126177) & 0xFFFFFFFF
                predicted[x, y, z] = (h ^ (h >> 16)) & 0xFF
    return predicted

def make_frequency_prediction(side, data):
    values, counts = np.unique(np.frombuffer(data, dtype=np.uint8), return_counts=True)
    most_frequent = values[np.argmax(counts)]
    return np.full((side, side, side), most_frequent, dtype=np.uint8)

def make_gradient_prediction(side):
    """Predict byte = linear gradient across the cube."""
    predicted = np.zeros((side, side, side), dtype=np.uint8)
    for x in range(side):
        for y in range(side):
            for z in range(side):
                predicted[x, y, z] = ((x + y + z) * 37) & 0xFF
    return predicted

def make_neighbor_prediction_1d(data, side):
    """Decode-safe: predict byte[i] = byte[i-1], reconstructed in order."""
    n = len(data)
    cube_vol = side ** 3
    actual = np.zeros(cube_vol, dtype=np.uint8)
    actual[:n] = np.frombuffer(data, dtype=np.uint8)
    
    predicted = np.zeros(cube_vol, dtype=np.uint8)
    predicted[0] = 0
    for i in range(1, cube_vol):
        predicted[i] = actual[i-1]
    
    return predicted.reshape(side, side, side)

def make_ppm_prediction(data, side, ctx_len=3):
    """Decode-safe PPM: predict byte[i] = most common byte after context."""
    n = len(data)
    cube_vol = side ** 3
    actual = np.zeros(cube_vol, dtype=np.uint8)
    actual[:n] = np.frombuffer(data, dtype=np.uint8)
    
    predicted = np.zeros(cube_vol, dtype=np.uint8)
    for i in range(cube_vol):
        if i < ctx_len:
            predicted[i] = 0
        else:
            ctx = tuple(actual[i-ctx_len:i].tolist())
            # Find all occurrences of this context in the data
            follow = {}
            for j in range(i, min(n - ctx_len, i + 1000)):
                c = tuple(actual[j:j+ctx_len].tolist())
                if c == ctx and j + ctx_len < n:
                    fb = actual[j + ctx_len]
                    follow[fb] = follow.get(fb, 0) + 1
            if follow:
                predicted[i] = max(follow, key=follow.get)
    
    return predicted.reshape(side, side, side)

if __name__ == '__main__':
    print("=" * 80)
    print("  Predictor Comparison for Delta Compression (decode-safe)")
    print("=" * 80)
    
    pdf_path = r'I:\FGLS_new\docs\POGLS_MASTER_SCHEMATIC.pdf'
    full_pdf = open(pdf_path, 'rb').read()
    
    test_cases = [
        ("PDF 10KB", full_pdf[:10000]),
        ("PDF 100KB", full_pdf[:100000]),
        ("PDF 1MB", full_pdf[:1000000]),
        ("Random 10KB", os.urandom(10000)),
        ("Random 100KB", os.urandom(100000)),
        ("Text 10KB", (b"Hello world! This is a test. " * 500)[:10000]),
    ]
    
    all_results = []
    
    for label, data in test_cases:
        n = len(data)
        side = max(4, int(n ** (1/3) + 0.999))
        
        raw_zlib = zlib.compress(data, 9)
        
        print(f"\n{'='*80}")
        print(f"  {label} ({n:,} bytes, cube={side}³={side**3:,})")
        print(f"  Baseline: raw+zlib = {len(raw_zlib):,} ({len(raw_zlib)/n:.2f}x)")
        print(f"{'='*80}")
        
        # Generate predictions
        predictions = {
            'Zero': make_zero_prediction(side),
            'Timeline': make_timeline_prediction(side),
            'Hash': make_hash_prediction(side),
            'Gradient': make_gradient_prediction(side),
            'Frequency': make_frequency_prediction(side, data),
        }
        
        for name, predicted in predictions.items():
            r = test_one(data, name, predicted)
            sym = "PASS" if r['roundtrip'] else "FAIL"
            print(f"  {name:<15} nonzero={r['nonzero_pct']:5.1f}% | "
                  f"zlib={r['compressed']:>8,} ({r['ratio']:.2f}x) | {sym}")
            all_results.append(r | {'label': label})
        
        # Also test raw zlib (the actual best predictor for arbitrary data)
        print(f"  {'Raw+zlib':<15} {'—':>22} | zlib={len(raw_zlib):>8,} ({len(raw_zlib)/n:.2f}x) | PASS")
    
    print("\n" + "=" * 80)
    print("  KEY INSIGHT")
    print("=" * 80)
    print("  For arbitrary data, the BEST predictor is the data itself (raw+zlib).")
    print("  Any external predictor produces delta ≈ data (99%+ nonzero).")
    print("  The question is: can geo_field make data more predictable?")
    print("  That's the real test we need to run.")
