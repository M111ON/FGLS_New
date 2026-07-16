#!/usr/bin/env python3
"""
Test: Delta compression with timeline prediction

Concept:
1. Use timeline as model to predict data
2. Store delta = actual - predicted
3. If prediction good, delta is small → compression
"""
import os, sys, time, hashlib, zlib, struct
import numpy as np

sys.path.insert(0, 'I:/FGLS_new/tools')
from geopixel_pipeline import (
    frame_enc, frame_at, FRAME_CYCLE, FRAME_STRIDE,
    FACE_LIST, CHUNK_SZ
)

# ══════════════════════════════════════════════════════════════
# Timeline prediction
# ══════════════════════════════════════════════════════════════

def timeline_predict(side):
    """
    Generate predicted data from timeline.
    Returns predicted cube where interior = f(timeline).
    """
    cube = np.zeros((side, side, side), dtype=np.uint8)
    
    for x in range(side):
        for y in range(side):
            for z in range(side):
                linear = (x * side + y) * side + z
                t = linear % FRAME_CYCLE
                enc = frame_enc(t)
                frame = frame_at(enc)
                cube[x, y, z] = (frame['face'] * 100 + frame['slot'] * 10 + frame['ico_idx']) & 0xFF
    
    return cube

def data_to_cube(data, side):
    """Place arbitrary data into cube."""
    cube = np.zeros((side, side, side), dtype=np.uint8)
    for i in range(min(len(data), side**3)):
        x = i // (side * side)
        y = (i // side) % side
        z = i % side
        cube[x, y, z] = data[i]
    return cube

def cube_to_data(cube, size):
    """Extract data from cube."""
    data = bytearray()
    for x in range(cube.shape[0]):
        for y in range(cube.shape[1]):
            for z in range(cube.shape[2]):
                data.append(cube[x, y, z])
                if len(data) >= size:
                    return bytes(data)
    return bytes(data)

# ══════════════════════════════════════════════════════════════
# Delta compression
# ══════════════════════════════════════════════════════════════

def compute_delta(actual, predicted):
    """Compute delta = actual - predicted (mod 256)."""
    return ((actual.astype(np.int16) - predicted.astype(np.int16)) % 256).astype(np.uint8)

def apply_delta(predicted, delta):
    """Reconstruct actual = predicted + delta (mod 256)."""
    delta_reshaped = delta.reshape(predicted.shape)
    return ((predicted.astype(np.int16) + delta_reshaped.astype(np.int16)) % 256).astype(np.uint8)

def store_delta(delta):
    """Store delta efficiently (sparse encoding)."""
    # Count non-zero elements
    non_zero = np.count_nonzero(delta)
    total = delta.size
    
    # If delta is sparse, store only non-zero positions
    if non_zero < total * 0.5:
        # Sparse encoding: [count][positions][values]
        positions = np.where(delta != 0)[0]
        values = delta[delta != 0]
        
        buf = bytearray()
        buf += struct.pack('<I', len(positions))  # count
        buf += positions.tobytes()  # positions (4 bytes each)
        buf += values.tobytes()  # values (1 byte each)
        return bytes(buf), True
    else:
        # Dense encoding: store full delta
        return delta.tobytes(), False

def load_delta(buf, total_size, is_sparse):
    """Load delta from stored format."""
    if is_sparse:
        count = struct.unpack_from('<I', buf, 0)[0]
        positions = np.frombuffer(buf[4:4+count*4], dtype=np.int32)
        values = buf[4+count*4:4+count*4+count]
        
        delta = np.zeros(total_size, dtype=np.uint8)
        delta[positions] = np.frombuffer(values, dtype=np.uint8)
        return delta
    else:
        return np.frombuffer(buf, dtype=np.uint8)

# ══════════════════════════════════════════════════════════════
# Test
# ══════════════════════════════════════════════════════════════

def test_delta_compression(data, label):
    """Test delta compression with timeline prediction."""
    print(f"\n{'='*60}")
    print(f"  {label} ({len(data):,} bytes)")
    print(f"{'='*60}")
    
    original_hash = hashlib.sha256(data).hexdigest()[:16]
    print(f"  Original hash: {original_hash}")
    
    # Compute cube side
    side = max(4, int(len(data) ** (1/3) + 0.999))
    cube_vol = side ** 3
    print(f"  Cube side: {side}")
    print(f"  Cube volume: {cube_vol:,} bytes")
    
    # Place data in cube
    actual_cube = data_to_cube(data, side)
    
    # Generate timeline prediction
    print(f"\n  --- Timeline Prediction ---")
    t0 = time.time()
    predicted_cube = timeline_predict(side)
    t_pred = time.time() - t0
    print(f"  Prediction time: {t_pred*1000:.1f}ms")
    
    # Compute delta
    print(f"\n  --- Delta Computation ---")
    t0 = time.time()
    delta = compute_delta(actual_cube, predicted_cube)
    t_delta = time.time() - t0
    
    non_zero = np.count_nonzero(delta)
    delta_ratio = non_zero / delta.size
    print(f"  Non-zero delta: {non_zero:,} / {delta.size:,} ({delta_ratio*100:.1f}%)")
    print(f"  Delta time: {t_delta*1000:.1f}ms")
    
    # Store delta (sparse or dense)
    print(f"\n  --- Delta Storage ---")
    t0 = time.time()
    delta_bytes, is_sparse = store_delta(delta)
    t_store = time.time() - t0
    print(f"  Storage format: {'sparse' if is_sparse else 'dense'}")
    print(f"  Delta size: {len(delta_bytes):,} bytes")
    print(f"  Delta ratio: {len(delta_bytes)/len(data):.2f}x")
    print(f"  Store time: {t_store*1000:.1f}ms")
    
    # Compress delta with zlib
    delta_zlib = zlib.compress(delta_bytes, 9)
    print(f"  Delta + zlib: {len(delta_zlib):,} bytes ({len(delta_zlib)/len(data):.2f}x)")
    
    # Reconstruct
    print(f"\n  --- Reconstruction ---")
    t0 = time.time()
    delta_recon = load_delta(delta_bytes, delta.size, is_sparse)
    actual_recon = apply_delta(predicted_cube, delta_recon)
    data_recon = cube_to_data(actual_recon, len(data))
    t_recon = time.time() - t0
    
    recon_hash = hashlib.sha256(data_recon).hexdigest()[:16]
    match = data_recon == data
    print(f"  Recon hash: {recon_hash}")
    print(f"  Roundtrip: {'PASS' if match else 'FAIL'}")
    print(f"  Recon time: {t_recon*1000:.1f}ms")
    
    # Compare with raw compression
    raw_zlib = zlib.compress(data, 9)
    print(f"\n  --- Comparison ---")
    print(f"  Raw + zlib: {len(raw_zlib):,} bytes ({len(raw_zlib)/len(data):.2f}x)")
    print(f"  Delta + zlib: {len(delta_zlib):,} bytes ({len(delta_zlib)/len(data):.2f}x)")
    
    improvement = len(raw_zlib) / len(delta_zlib) if len(delta_zlib) > 0 else 0
    print(f"  Delta is {improvement:.2f}x {'smaller' if improvement > 1 else 'larger'} than raw+zlib")
    
    return {
        'label': label,
        'input_size': len(data),
        'delta_size': len(delta_bytes),
        'delta_zlib_size': len(delta_zlib),
        'raw_zlib_size': len(raw_zlib),
        'delta_ratio': len(delta_zlib) / len(data),
        'roundtrip': match,
        'non_zero_ratio': delta_ratio,
    }

# ══════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════

if __name__ == '__main__':
    print("=" * 60)
    print("  Delta Compression with Timeline Prediction")
    print("=" * 60)
    print("  Concept: actual = predicted + delta")
    print("  If prediction good, delta small → compression")
    
    results = []
    
    # Test with PDF
    pdf_path = r'I:\FGLS_new\docs\POGLS_MASTER_SCHEMATIC.pdf'
    if os.path.exists(pdf_path):
        full_pdf = open(pdf_path, 'rb').read()
        
        for sz in [10000, 50000, 100000, 500000]:
            data = full_pdf[:sz]
            r = test_delta_compression(data, f"PDF {sz//1024}KB")
            results.append(r)
    
    # Test with random data
    for sz in [10000, 50000, 100000]:
        data = os.urandom(sz)
        r = test_delta_compression(data, f"Random {sz//1024}KB")
        results.append(r)
    
    # Summary
    print("\n" + "=" * 60)
    print("  SUMMARY")
    print("=" * 60)
    print(f"  {'Data':<20} {'Input':<12} {'Delta+zlib':<14} {'Raw+zlib':<14} {'Better?':<10}")
    print("-" * 60)
    for r in results:
        better = "YES" if r['delta_ratio'] < r['raw_zlib_size']/r['input_size'] else "NO"
        print(f"  {r['label']:<20} {r['input_size']:<12,} {r['delta_zlib_size']:<14,} {r['raw_zlib_size']:<14,} {better:<10}")
    
    print("\n" + "=" * 60)
    print("  CONCLUSION")
    print("=" * 60)
    print("  Delta compression: actual = predicted + delta")
    print("  Prediction from timeline model")
    print("  Delta stores residual")
    print("  If prediction good → compression")
