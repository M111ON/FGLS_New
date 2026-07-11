#!/usr/bin/env python3
"""
Hamburger Codec — Real File Test

Test with actual files from the project.

Usage:
    python hamburger_test_real.py <file>
"""

import sys, os, time, hashlib, math
import numpy as np

# ── Constants ──
FRAME_CYCLE = 1440
FRAME_STRIDE = 37
FRAME_FACE_SZ = 120
FRAME_EDGES = 12

# ── geo_frame_seek ──

def frame_enc(t):
    return (t * FRAME_STRIDE) % FRAME_CYCLE

def frame_at(enc):
    face = enc // FRAME_FACE_SZ
    slot = enc % FRAME_FACE_SZ
    ico_idx = enc % 162
    phase = (enc // FRAME_EDGES) % 12
    return face, slot, ico_idx, phase

# ── File → Cube → 6 Faces ──

def file_to_cube(data: bytes, target_size=None):
    """Convert file bytes to 3D cube."""
    # Pad to cube size
    n_bytes = len(data)
    if target_size is None:
        # Find smallest cube that fits
        side = math.ceil(math.pow(n_bytes, 1/3))
        # Round up to multiple of 100 for clean faces
        side = math.ceil(side / 100) * 100
        target_size = side
    
    total_voxels = target_size ** 3
    
    # Pad data to fill cube
    padded = data + b'\x00' * (total_voxels - n_bytes)
    
    # Convert to numpy array
    arr = np.frombuffer(padded, dtype=np.uint8)
    cube = arr.reshape(target_size, target_size, target_size).astype(np.float32)
    
    return cube, n_bytes, target_size

def unfold_cube(cube):
    """Unfold 3D cube to 6 faces."""
    faces = {}
    faces['front'] = cube[:, :, -1]
    faces['back'] = cube[:, :, 0]
    faces['left'] = cube[0, :, :]
    faces['right'] = cube[-1, :, :]
    faces['top'] = cube[:, -1, :]
    faces['bottom'] = cube[:, 0, :]
    return faces

def cube_to_file(cube, original_size):
    """Convert 3D cube back to file bytes."""
    arr = cube.astype(np.uint8).flatten()
    return arr[:original_size].tobytes()

# ── Timeline reconstruction ──

def reconstruct_from_timeline(faces, size):
    """Reconstruct cube from 6 faces via timeline derivation."""
    cube = np.zeros((size, size, size), dtype=np.float32)

    cube[:, :, -1] = faces['front']
    cube[:, :, 0] = faces['back']
    cube[0, :, :] = faces['left']
    cube[-1, :, :] = faces['right']
    cube[:, -1, :] = faces['top']
    cube[:, 0, :] = faces['bottom']

    # Vectorized interior reconstruction
    idx = np.arange(1, size - 1)
    x, y, z = np.meshgrid(idx, idx, idx, indexing='ij')

    linear = (x * size + y) * size + z
    t = linear % FRAME_CYCLE
    enc = (t * FRAME_STRIDE) % FRAME_CYCLE

    face = enc // FRAME_FACE_SZ
    slot = enc % FRAME_FACE_SZ
    ico_idx = enc % 162
    phase = (enc // FRAME_EDGES) % 12

    interior = (
        face * 100 +
        slot * 10 +
        ico_idx +
        phase * 5
    ).astype(np.float32)

    cube[1:-1, 1:-1, 1:-1] = interior
    return cube

# ── Main ──

def test_file(filepath):
    """Test Hamburger Codec with a real file."""
    print(f"\n{'='*60}")
    print(f"Testing: {filepath}")
    print(f"{'='*60}\n")
    
    # Read file
    t0 = time.perf_counter()
    data = open(filepath, 'rb').read()
    t_read = time.perf_counter() - t0
    
    file_size = len(data)
    checksum = hashlib.sha256(data).hexdigest()[:16]
    
    print(f"File size:    {file_size:,} bytes ({file_size/1024:.1f} KB)")
    print(f"Checksum:     {checksum}")
    print(f"Read time:    {t_read*1000:.1f} ms")
    
    # Convert to cube
    t0 = time.perf_counter()
    cube, orig_size, side = file_to_cube(data)
    t_to_cube = time.perf_counter() - t0
    
    print(f"\nCube size:    {side}×{side}×{side}")
    print(f"Cube memory:  {cube.nbytes/1024:.1f} KB")
    print(f"To cube:      {t_to_cube*1000:.1f} ms")
    
    # Encode (unfold)
    t0 = time.perf_counter()
    faces = unfold_cube(cube)
    t_encode = time.perf_counter() - t0
    
    face_bytes = sum(f.nbytes for f in faces.values())
    print(f"\n6 faces:      {face_bytes/1024:.1f} KB")
    print(f"Encode:       {t_encode*1000:.1f} ms")
    
    # Decode (reconstruct)
    t0 = time.perf_counter()
    reconstructed = reconstruct_from_timeline(faces, side)
    t_decode = time.perf_counter() - t0
    
    print(f"Decode:       {t_decode*1000:.1f} ms")
    
    # Verify
    t0 = time.perf_counter()
    match = np.allclose(cube, reconstructed)
    t_verify = time.perf_counter() - t0
    
    print(f"Verify:       {t_verify*1000:.1f} ms")
    print(f"Match:        {match}")
    
    # Summary
    print(f"\n{'─'*60}")
    print(f"Summary:")
    print(f"  Original:   {file_size:,} bytes")
    print(f"  Stored:     {face_bytes:,} bytes (6 faces)")
    print(f"  Ratio:      {cube.nbytes/face_bytes:.1f}x")
    print(f"  Time:       {(t_to_cube+t_encode+t_decode+t_verify)*1000:.1f} ms")
    print(f"  Match:      {match}")
    
    return match

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python hamburger_test_real.py <file>")
        print("\nExample files:")
        print("  python hamburger_test_real.py runner/llama_pogls_runner_sid_v2.c")
        print("  python hamburger_test_real.py tools/geopixel_hilbert.py")
        sys.exit(1)
    
    filepath = sys.argv[1]
    if not os.path.exists(filepath):
        print(f"Error: {filepath} not found")
        sys.exit(1)
    
    test_file(filepath)
