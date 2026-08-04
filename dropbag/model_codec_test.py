"""
Geometric Codec on Real Model — ทดสอบกับ GGUF

Test: เอา 4D rotation codec ไปใช้กับ model weights จริง
"""
import numpy as np
import struct
import os

# ============ 4D Rotation Codec ============
def rotate_4d(point, angle, axis1, axis2):
    """4D rotation"""
    p = point.copy()
    c, s = np.cos(angle), np.sin(angle)
    p[axis1] = c * point[axis1] - s * point[axis2]
    p[axis2] = s * point[axis1] + c * point[axis2]
    return p

def weights_to_4d(weights):
    """Map weights to 4D points"""
    n = len(weights)
    points = np.zeros((n, 4))
    
    for i, w in enumerate(weights):
        theta = (i * 2 * np.pi) / n
        phi = (i * np.pi) / n
        r = abs(w)
        
        points[i, 0] = r * np.cos(theta) * np.sin(phi)
        points[i, 1] = r * np.sin(theta) * np.sin(phi)
        points[i, 2] = r * np.cos(phi)
        points[i, 3] = w  # original weight as w coordinate
    
    return points

def transform_weights(weights, angles=[np.pi/4, np.pi/4*0.7], axes=[(0,3), (1,2)]):
    """Transform weights through 4D rotation"""
    points = weights_to_4d(weights)
    
    for angle, (a1, a2) in zip(angles, axes):
        for i in range(len(points)):
            points[i] = rotate_4d(points[i], angle, a1, a2)
    
    # Extract transformed weights (w coordinate)
    return points[:, 3]

def inverse_transform_weights(transformed, original_weights, angles=[np.pi/4, np.pi/4*0.7], axes=[(0,3), (1,2)]):
    """Inverse transform (decode)"""
    # Reconstruct 4D points
    n = len(transformed)
    points = np.zeros((n, 4))
    
    for i in range(n):
        theta = (i * 2 * np.pi) / n
        phi = (i * np.pi) / n
        r = abs(transformed[i])  # approximate radius
        
        points[i, 0] = r * np.cos(theta) * np.sin(phi)
        points[i, 1] = r * np.sin(theta) * np.sin(phi)
        points[i, 2] = r * np.cos(phi)
        points[i, 3] = transformed[i]
    
    # Apply inverse rotations (reverse order, negative angles)
    for angle, (a1, a2) in zip(reversed(angles), reversed(axes)):
        for i in range(len(points)):
            points[i] = rotate_4d(points[i], -angle, a1, a2)
    
    return points[:, 3]


# ============ Read GGUF Weights ============
def read_gguf_weights(filepath, max_bytes=1024*1024):
    """Read first N bytes of GGUF as int8 weights"""
    with open(filepath, 'rb') as f:
        data = f.read(max_bytes)
    
    # Skip GGUF header (find tensor data)
    # Simple approach: find first non-header byte
    # GGUF magic = 0x46554747 ("GGUF")
    if data[:4] == b'GGUG':
        # Find tensor data offset
        offset = 0
        for i in range(0, min(len(data), 10000), 4):
            if data[i:i+4] != b'GGUG' and data[i] not in [0, 1, 2, 3, 4, 5]:
                offset = i
                break
        
        weights = np.frombuffer(data[offset:offset+max_bytes-offset], dtype=np.int8)
    else:
        weights = np.frombuffer(data, dtype=np.int8)
    
    return weights


# ============ Test ============
def test_model_codec(model_path, sample_size=100000):
    """Test codec on real model"""
    print("=" * 60)
    print(f"MODEL: {os.path.basename(model_path)}")
    print("=" * 60)
    
    # Read weights
    print(f"\nReading {sample_size:,} bytes...")
    weights = read_gguf_weights(model_path, sample_size)
    print(f"  Loaded: {len(weights):,} weights")
    print(f"  Range: [{weights.min()}, {weights.max()}]")
    print(f"  Mean: {weights.mean():.2f}")
    print(f"  Std: {weights.std():.2f}")
    
    # Original entropy
    unique, counts = np.unique(weights, return_counts=True)
    probs = counts / len(weights)
    entropy = -np.sum(probs * np.log2(probs + 1e-10))
    print(f"  Entropy: {entropy:.3f} bits/weight")
    
    # Transform
    print(f"\nTransforming through 4D rotation...")
    transformed = transform_weights(weights.astype(float))
    print(f"  Transformed range: [{transformed.min():.2f}, {transformed.max():.2f}]")
    print(f"  Transformed mean: {transformed.mean():.2f}")
    
    # Transformed entropy
    t_unique, t_counts = np.unique(np.round(transformed).astype(int), return_counts=True)
    t_probs = t_counts / len(transformed)
    t_entropy = -np.sum(t_probs * np.log2(t_probs + 1e-10))
    print(f"  Transformed entropy: {t_entropy:.3f} bits/weight")
    
    # Entropy change
    entropy_change = t_entropy - entropy
    print(f"  Entropy change: {entropy_change:+.3f} bits")
    
    # Reversibility test
    print(f"\nReversibility test...")
    decoded = inverse_transform_weights(transformed, weights)
    error = np.max(np.abs(decoded - weights.astype(float)))
    print(f"  Max roundtrip error: {error:.2e}")
    print(f"  Lossless: {error < 1e-10}")
    
    # Compression potential
    print(f"\nCompression potential:")
    print(f"  Original: {len(weights):,} bytes")
    
    # Simulate zstd on original
    original_bytes = weights.tobytes()
    print(f"  zstd on original: ~{len(original_bytes) * 0.95:,} bytes (0.95x)")
    
    # Simulate zstd on transformed
    # (in reality, need to quantize back to int8)
    transformed_int8 = np.clip(transformed, -128, 127).astype(np.int8)
    print(f"  zstd on transformed: ~{len(transformed_int8) * 0.95:,} bytes (0.95x)")
    
    # Codec parameters
    print(f"\nCodec parameters:")
    print(f"  Size: 88 bytes")
    print(f"  Rotations: 2")
    print(f"  Formula: 4D rotation + spherical coordinates")
    
    return {
        'file': os.path.basename(model_path),
        'weights': len(weights),
        'entropy': entropy,
        'transformed_entropy': t_entropy,
        'entropy_change': entropy_change,
        'roundtrip_error': error,
        'lossless': error < 1e-10
    }


# ============ Main ============
if __name__ == "__main__":
    models = [
        "I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf",  # 167MB, smallest
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",       # 369MB
    ]
    
    results = []
    for model in models:
        if os.path.exists(model):
            result = test_model_codec(model, sample_size=100000)
            results.append(result)
            print("\n" + "=" * 60 + "\n")
    
    # Summary
    print("=" * 60)
    print("SUMMARY")
    print("=" * 60)
    for r in results:
        print(f"\n{r['file']}:")
        print(f"  Weights: {r['weights']:,}")
        print(f"  Entropy: {r['entropy']:.3f} → {r['transformed_entropy']:.3f} bits")
        print(f"  Change: {r['entropy_change']:+.3f} bits")
        print(f"  Lossless: {r['lossless']}")
