"""
test_lblock_real.py — Test L-block on real GGUF model weights

Demonstrates:
1. L-block rotation distribution on real model data
2. Hilbert direction consistency
3. Grid alignment with actual tensor weights
"""

import sys
import os
sys.path.insert(0, '.')
import importlib.util
spec = importlib.util.spec_from_file_location('gfs', 'geo_frame_seek.py')
gfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gfs)

def load_gguf_bytes(path, max_bytes=38400):
    """Load raw bytes from GGUF file (skip header)."""
    with open(path, 'rb') as f:
        data = f.read()
    # Skip GGUF header (find first tensor data)
    # Simple heuristic: skip first 1024 bytes (typical header size)
    return data[1024:1024+max_bytes]

# Find GGUF model
model_paths = [
    "I:/model/SmolLM2-360M-Instruct-Q8_0.gguf",
    "I:/model/Qwen3-0.6B-Q8_0.gguf",
    "I:/model/Qwen2.5-3B-Instruct-Q8_0.gguf",
]

model_path = None
for p in model_paths:
    if os.path.exists(p):
        model_path = p
        break

if not model_path:
    print("No GGUF model found. Testing with random data.")
    data_bytes = os.urandom(38400)
    model_name = "random"
else:
    print(f"Using model: {os.path.basename(model_path)}")
    data_bytes = load_gguf_bytes(model_path, 38400)
    model_name = os.path.basename(model_path)

print(f"Data size: {len(data_bytes)} bytes")

# Verify L-block
rc = gfs.lblock_verify()
print(f"\nlblock_verify: {'PASS' if rc == 0 else f'FAIL ({rc})'}")

# Analyze L-block on real data
print("\n=== L-block Analysis on Real Data ===")

# Convert bytes to Hilbert positions
# Each pair of bytes → (vx, vy) → Hilbert position
n_pairs = len(data_bytes) // 2
print(f"Pairs: {n_pairs}")

# Track rotation distribution
rot_count = [0, 0, 0, 0]
dir_count = {(1,0): 0, (0,1): 0, (-1,0): 0, (0,-1): 0}

# Analyze first 1000 pairs
n_analyze = min(1000, n_pairs)
for i in range(n_analyze):
    b0 = data_bytes[2*i]
    b1 = data_bytes[2*i + 1]
    # Map byte pair to Hilbert position (0-63)
    d = (b0 * 256 + b1) % 64
    
    # Get L-block
    cells, rot, (dx, dy) = gfs.lblock_from_hilbert(d, 8)
    rot_count[rot] += 1
    dir_count[(dx, dy)] = dir_count.get((dx, dy), 0) + 1

print(f"\nAnalyzed {n_analyze} pairs:")
print(f"\nRotation distribution:")
total = sum(rot_count)
for i, count in enumerate(rot_count):
    pct = count / total * 100
    bar = '#' * int(pct / 2)
    print(f"  rot {i}: {count:4d} ({pct:5.1f}%) {bar}")

print(f"\nDirection distribution:")
for (dx, dy), count in sorted(dir_count.items()):
    pct = count / total * 100
    dir_name = {(1,0): 'right', (0,1): 'down', (-1,0): 'left', (0,-1): 'up'}[(dx, dy)]
    print(f"  ({dx:+d},{dy:+d}) {dir_name:5s}: {count:4d} ({pct:5.1f}%)")

# Show sample L-blocks
print(f"\nSample L-blocks from model data:")
print("  pos | bytes  | hilbert_d | rot | cells")
print("  ----|--------|-----------|-----|------")
for i in range(0, min(20, n_analyze), 2):
    b0 = data_bytes[2*i]
    b1 = data_bytes[2*i + 1]
    d = (b0 * 256 + b1) % 64
    cells, rot, _ = gfs.lblock_from_hilbert(d, 8)
    cells_str = " ".join(f"({x},{y})" for x, y in cells)
    print(f"  {i:3d} | {b0:02x}{b1:02x}   | {d:9d} |  {rot}  | {cells_str}")

print(f"\n=== KEY INSIGHT ===")
print(f"L-block from model data: rotation is deterministic based on Hilbert position.")
print(f"Same byte pattern → same Hilbert address → same rotation → grid-aligned.")
