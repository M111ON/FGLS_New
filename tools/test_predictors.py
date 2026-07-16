#!/usr/bin/env python3
"""
Test different predictors for delta compression.
Goal: find predictor that makes delta small for arbitrary data.
"""
import os, sys, time, hashlib, zlib, struct
import numpy as np

# ══════════════════════════════════════════════════════════════
# Predictors
# ══════════════════════════════════════════════════════════════

class Predictor:
    """Base predictor class."""
    def predict(self, data):
        """Return predicted array same shape as data."""
        raise NotImplementedError
    
    def name(self):
        return "base"

class TimelinePredictor(Predictor):
    """Original timeline predictor (baseline)."""
    def __init__(self, side):
        self.side = side
        sys.path.insert(0, 'I:/FGLS_new/tools')
        from geopixel_pipeline import frame_enc, frame_at, FRAME_CYCLE
        self.frame_enc = frame_enc
        self.frame_at = frame_at
        self.FRAME_CYCLE = FRAME_CYCLE
    
    def predict(self, data):
        side = self.side
        predicted = np.zeros((side, side, side), dtype=np.uint8)
        for x in range(side):
            for y in range(side):
                for z in range(side):
                    linear = (x * side + y) * side + z
                    t = linear % self.FRAME_CYCLE
                    enc = self.frame_enc(t)
                    frame = self.frame_at(enc)
                    predicted[x, y, z] = (frame['face'] * 100 + frame['slot'] * 10 + frame['ico_idx']) & 0xFF
        return predicted
    
    def name(self):
        return "Timeline"

class ZeroPredictor(Predictor):
    """Predict all zeros (baseline)."""
    def predict(self, data):
        return np.zeros_like(data)
    
    def name(self):
        return "Zero"

class PrevBytePredictor(Predictor):
    """Predict each byte = previous byte (LZ-style)."""
    def predict(self, data):
        predicted = np.zeros_like(data)
        flat = data.flatten()
        for i in range(1, len(flat)):
            flat[i] = flat[i-1]
        return predicted
    
    def name(self):
        return "PrevByte"

class MeanPredictor(Predictor):
    """Predict each byte = mean of neighbors."""
    def predict(self, data):
        predicted = np.zeros_like(data)
        shape = data.shape
        for x in range(shape[0]):
            for y in range(shape[1]):
                for z in range(shape[2]):
                    neighbors = []
                    for dx in [-1, 0, 1]:
                        for dy in [-1, 0, 1]:
                            for dz in [-1, 0, 1]:
                                if dx == 0 and dy == 0 and dz == 0:
                                    continue
                                nx, ny, nz = x+dx, y+dy, z+dz
                                if 0 <= nx < shape[0] and 0 <= ny < shape[1] and 0 <= nz < shape[2]:
                                    neighbors.append(data[nx, ny, nz])
                    if neighbors:
                        predicted[x, y, z] = np.mean(neighbors)
        return predicted
    
    def name(self):
        return "Mean"

class RunLengthPredictor(Predictor):
    """Predict = last seen value (run-length encoding style)."""
    def predict(self, data):
        predicted = np.zeros_like(data)
        flat_pred = predicted.flatten()
        flat_data = data.flatten()
        if len(flat_data) > 0:
            flat_pred[0] = flat_data[0]
            for i in range(1, len(flat_data)):
                flat_pred[i] = flat_data[i-1]
        return predicted
    
    def name(self):
        return "RunLength"

class HashPredictor(Predictor):
    """Predict based on position hash (deterministic but data-independent)."""
    def __init__(self, seed=42):
        self.seed = seed
    
    def predict(self, data):
        predicted = np.zeros_like(data)
        shape = data.shape
        for x in range(shape[0]):
            for y in range(shape[1]):
                for z in range(shape[2]):
                    h = (x * 374761393 + y * 668265263 + z * 1274126177 + self.seed) & 0xFFFFFFFF
                    predicted[x, y, z] = (h ^ (h >> 16)) & 0xFF
        return predicted
    
    def name(self):
        return "Hash"

class FrequencyPredictor(Predictor):
    """Predict = most frequent byte in data (global prediction)."""
    def predict(self, data):
        # Find most frequent byte
        values, counts = np.unique(data, return_counts=True)
        most_frequent = values[np.argmax(counts)]
        return np.full_like(data, most_frequent)
    
    def name(self):
        return "Frequency"

class MedianPredictor(Predictor):
    """Predict = median of neighbors."""
    def predict(self, data):
        predicted = np.zeros_like(data)
        shape = data.shape
        for x in range(shape[0]):
            for y in range(shape[1]):
                for z in range(shape[2]):
                    neighbors = []
                    for dx in [-1, 0, 1]:
                        for dy in [-1, 0, 1]:
                            for dz in [-1, 0, 1]:
                                if dx == 0 and dy == 0 and dz == 0:
                                    continue
                                nx, ny, nz = x+dx, y+dy, z+dz
                                if 0 <= nx < shape[0] and 0 <= ny < shape[1] and 0 <= nz < shape[2]:
                                    neighbors.append(data[nx, ny, nz])
                    if neighbors:
                        predicted[x, y, z] = np.median(neighbors)
        return predicted
    
    def name(self):
        return "Median"

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

def compress_delta(delta):
    """Compress delta with zlib."""
    return zlib.compress(delta.tobytes(), 9)

def decompress_delta(compressed, shape):
    """Decompress delta."""
    data = zlib.decompress(compressed)
    return np.frombuffer(data, dtype=np.uint8).reshape(shape)

# ══════════════════════════════════════════════════════════════
# Test
# ══════════════════════════════════════════════════════════════

def test_predictor(data, predictor, label):
    """Test a predictor with given data."""
    side = max(4, int(len(data) ** (1/3) + 0.999))
    
    # Reshape data to cube
    cube = np.zeros((side, side, side), dtype=np.uint8)
    data_arr = np.frombuffer(data, dtype=np.uint8)
    flat = cube.flatten()
    flat[:len(data_arr)] = data_arr
    
    # Get prediction
    t0 = time.time()
    predicted = predictor.predict(cube)
    t_pred = time.time() - t0
    
    # Compute delta
    delta = compute_delta(cube, predicted)
    
    # Stats
    non_zero = np.count_nonzero(delta)
    delta_ratio = non_zero / delta.size
    
    # Compress
    compressed = compress_delta(delta)
    
    # Verify roundtrip
    delta_recon = decompress_delta(compressed, cube.shape)
    actual_recon = apply_delta(predicted, delta_recon)
    data_recon = actual_recon.flatten()[:len(data)]
    match = data_recon.tobytes() == data
    
    return {
        'name': predictor.name(),
        'label': label,
        'input_size': len(data),
        'delta_nonzero': non_zero,
        'delta_ratio': delta_ratio,
        'compressed_size': len(compressed),
        'compression_ratio': len(compressed) / len(data),
        'roundtrip': match,
        'pred_time': t_pred * 1000,
    }

# ══════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════

if __name__ == '__main__':
    print("=" * 80)
    print("  Predictor Comparison for Delta Compression")
    print("=" * 80)
    
    # Test data
    pdf_path = r'I:\FGLS_new\docs\POGLS_MASTER_SCHEMATIC.pdf'
    full_pdf = open(pdf_path, 'rb').read()
    
    test_cases = [
        ("PDF 10KB", full_pdf[:10000]),
        ("PDF 100KB", full_pdf[:100000]),
        ("Random 10KB", os.urandom(10000)),
        ("Random 100KB", os.urandom(100000)),
    ]
    
    predictors = [
        ZeroPredictor(),
        PrevBytePredictor(),
        FrequencyPredictor(),
        HashPredictor(),
        MedianPredictor(),
        TimelinePredictor(max(4, int(100000 ** (1/3) + 0.999))),
    ]
    
    all_results = []
    
    for label, data in test_cases:
        print(f"\n{'='*80}")
        print(f"  {label} ({len(data):,} bytes)")
        print(f"{'='*80}")
        
        # Baseline: raw zlib
        raw_zlib = zlib.compress(data, 9)
        print(f"  Baseline: raw+zlib = {len(raw_zlib):,} bytes ({len(raw_zlib)/len(data):.2f}x)")
        print()
        
        results = []
        for pred in predictors:
            try:
                r = test_predictor(data, pred, label)
                results.append(r)
                symbol = "✓" if r['roundtrip'] else "✗"
                print(f"  {r['name']:<15} delta={r['delta_ratio']*100:5.1f}% nonzero | "
                      f"compressed={r['compressed_size']:>8,} bytes ({r['compression_ratio']:.2f}x) | "
                      f"roundtrip {symbol} | {r['pred_time']:.0f}ms")
            except Exception as e:
                print(f"  {pred.name():<15} ERROR: {e}")
        
        # Find best
        valid = [r for r in results if r['roundtrip']]
        if valid:
            best = min(valid, key=lambda r: r['compressed_size'])
            print(f"\n  Best: {best['name']} ({best['compression_ratio']:.2f}x)")
        
        all_results.extend(results)
    
    # Summary
    print("\n" + "=" * 80)
    print("  SUMMARY")
    print("=" * 80)
    print(f"  {'Predictor':<15} {'PDF 10KB':<14} {'PDF 100KB':<14} {'Random 10KB':<14} {'Random 100KB':<14}")
    print("-" * 80)
    
    for pred in predictors:
        row = f"  {pred.name():<15}"
        for label, data in test_cases:
            matching = [r for r in all_results if r['name'] == pred.name() and r['label'] == label]
            if matching:
                r = matching[0]
                row += f"{r['compression_ratio']:.2f}x{'✓' if r['roundtrip'] else '✗':<3}     "
            else:
                row += f"{'N/A':<14}"
        print(row)
    
    print("\n" + "=" * 80)
    print("  RAW ZLIB BASELINE")
    print("=" * 80)
    for label, data in test_cases:
        raw_zlib = zlib.compress(data, 9)
        print(f"  {label:<20} raw+zlib = {len(raw_zlib)/len(data):.2f}x")
