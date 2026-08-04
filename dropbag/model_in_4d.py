"""
Model → 4D Structure — Map model in 4D space
Not transform weights — just MAP them into geometry

Test: map weights → 4D position → weights same position (no compression)
"""
import numpy as np
import os
import sys

# ============ 4D rotation ============
def rotate_4d(point, angle, axis1, axis2):
    p = point.copy()
    c, s = np.cos(angle), np.sin(angle)
    p[axis1] = c * point[axis1] - s * point[axis2]
    p[axis2] = s * point[axis1] + c * point[axis2]
    return p

# ============ Map weights to 4D ============
def map_weights_to_4d(weights, angles=[np.pi/4, np.pi/4*0.7], axes=[(0,3), (1,2)]):
    """Map weights to 4D space"""
    n = len(weights)
    points = np.zeros((n, 4))
    
    for i, w in enumerate(weights):
        theta = (i * 2 * np.pi) / n
        phi = (i * np.pi) / n
        r = abs(w) + 1e-10
        points[i, 0] = r * np.cos(theta) * np.sin(phi)
        points[i, 1] = r * np.sin(theta) * np.sin(phi)
        points[i, 2] = r * np.cos(phi)
        points[i, 3] = float(w)
    
    for angle, (a1, a2) in zip(angles, axes):
        for i in range(len(points)):
            points[i] = rotate_4d(points[i], angle, a1, a2)
    
    return points

def map_back_from_4d(points, angles=[np.pi/4, np.pi/4*0.7], axes=[(0,3), (1,2)]):
    """Extract weights from 4D position"""
    result = np.zeros(len(points))
    
    for angle, (a1, a2) in zip(reversed(angles), reversed(axes)):
        for i in range(len(points)):
            points[i] = rotate_4d(points[i], -angle, a1, a2)
    
    for i in range(len(points)):
        result[i] = points[i, 3]
    
    return result

# ============ Main test ============
def test_model_in_4d(model_path, max_samples=500):
    """Map model into 4D structure"""
    print("=" * 60)
    print(f"MODEL in 4D STRUCTURE: {os.path.basename(model_path)}")
    print("=" * 60)
    
    # Read GGUF
    with open(model_path, 'rb') as f:
        raw = f.read(1024 * 1024 * 2)
    
    # Find tensor data
    offset = 0
    if raw[:4] == b'GGUF':
        for i in range(len(raw) - 4):
            if raw[i] not in [0, 1, 2, 3, 4, 5]:
                break
        offset = i
    
    weights = np.frombuffer(raw[offset:offset + max_samples], dtype=np.int8).astype(float)
    print(f"\nWeights: {len(weights)}, range=[{weights.min():.0f}, {weights.max():.0f}]")
    
    # Original entropy
    unique, counts = np.unique(weights, return_counts=True)
    probs = counts / len(weights)
    entropy = -np.sum(probs * np.log2(probs + 1e-10))
    print(f"Entropy: {entropy:.3f} bits/weight")
    
    # Map to 4D
    print(f"\nMapping to 4D...")
    pos_4d = map_weights_to_4d(weights)
    print(f"4D positions: {pos_4d.shape}")
    print(f"Norms range: {np.linalg.norm(pos_4d, axis=1).min():.2f}/{np.linalg.norm(pos_4d, axis=1).max():.2f}")
    print(f"Storage: {pos_4d.nbytes:,} bytes (expand: {pos_4d.nbytes/len(weights):.2f}x)")
    
    # Map back
    print(f"\nExtracting from 4D...")
    decoded = map_back_from_4d(pos_4d.copy())
    
    # Roundtrip
    error = np.max(np.abs(decoded - weights))
    exact = np.allclose(decoded, weights, atol=1e-10)
    print(f"Error: {error:.2e}")
    print(f"Exact: {exact}")
    
    return exact, error


# ============ Run ============
results = []
for model in ["I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf", "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"]:
    if os.path.exists(model):
        exact, error = test_model_in_4d(model)
        results.append({'model': os.path.basename(model), 'exact': exact, 'error': error})
        print()

print("=" * 60)
print("SUMMARY")
print("=" * 60)
for r in results:
    print(f"  {r['model']}: exact={r['exact']}, error={r['error']:.2e}")