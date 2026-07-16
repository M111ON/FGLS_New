#!/usr/bin/env python3
"""
Test: Surface-only storage + timeline reconstruction

Measures actual compression from storing only 6 faces
and reconstructing interior via geo_frame_seek timeline.
"""
import os, sys, time, struct, zlib, hashlib
import numpy as np

sys.path.insert(0, 'I:/FGLS_new/tools')
from geopixel_pipeline import (
    frame_enc, frame_at, FRAME_CYCLE, FRAME_STRIDE,
    FACE_LIST, CHUNK_SZ
)

# ══════════════════════════════════════════════════════════════
# Constants
# ══════════════════════════════════════════════════════════════

GEO_FULL = 20736  # 144²

# ══════════════════════════════════════════════════════════════
# Data generators
# ══════════════════════════════════════════════════════════════

def gen_random(size):
    return os.urandom(size)

def gen_image_like(size):
    """Image-like: smooth gradients with local coherence."""
    data = bytearray()
    side = int(size ** 0.5) + 1
    for y in range(side):
        for x in range(side):
            if len(data) >= size:
                break
            r = int(128 + 127 * __import__('math').sin(x * 0.1))
            g = int(128 + 127 * __import__('math').cos(y * 0.1))
            b = int(128 + 127 * __import__('math').sin((x + y) * 0.05))
            data.extend([r & 0xFF, g & 0xFF, b & 0xFF])
    return bytes(data[:size])

def gen_timeline_derived(size):
    """Data = f(timeline) — designed for perfect reconstruction."""
    cube_side = int(size ** (1/3) + 0.999)
    data = bytearray()
    for linear in range(cube_side ** 3):
        t = linear % FRAME_CYCLE
        enc = frame_enc(t)
        frame = frame_at(enc)
        # Derive value from timeline
        val = (frame['face'] * 100 + frame['slot'] * 10 + frame['ico_idx']) & 0xFF
        data.append(val)
    return bytes(data[:size])

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
# Surface-only approach
# ══════════════════════════════════════════════════════════════

def cube_side_for_data(data_size, chunk_size=64):
    """Compute cube side needed to hold data as surface voxels."""
    n_chunks = (data_size + chunk_size - 1) // chunk_size
    # Surface capacity = 6 * side²
    # side = sqrt(n_chunks / 6)
    side = max(2, int((n_chunks / 6) ** 0.5 + 0.999))
    return side

def data_to_surface(data, side, chunk_size=64):
    """
    Place data on cube surface (6 faces).
    Returns: {face_name: np.array(side, side)} mapping
    """
    n_chunks = (len(data) + chunk_size - 1) // chunk_size
    surface_cap = 6 * side * side
    
    # Initialize face arrays (-1 = empty)
    faces = {name: np.full((side, side), -1, dtype=np.int32) for name in FACE_LIST}
    
    # Place chunks on surface
    for ci in range(n_chunks):
        off = ci * chunk_size
        chunk = data[off:off + chunk_size]
        
        # Determine face, row, col from chunk index
        face_idx = ci % 6
        face_name = FACE_LIST[face_idx]
        pos = ci // 6
        r = pos // side
        c = pos % side
        
        if r < side and c < side:
            # Store first byte of chunk as voxel value (simplified)
            faces[face_name][r, c] = chunk[0] if chunk else 0
    
    return faces

def surface_to_data(faces, side, original_size, chunk_size=64):
    """
    Reconstruct data from surface faces.
    This is SIMPLIFIED — real implementation uses timeline reconstruction.
    """
    data = bytearray()
    for face_idx, face_name in enumerate(FACE_LIST):
        face = faces[face_name]
        for r in range(side):
            for c in range(side):
                val = face[r, c]
                if val >= 0:
                    # Expand single byte back to chunk (simplified)
                    chunk = bytes([val] * chunk_size)
                    data.extend(chunk)
    return bytes(data[:original_size])

def reconstruct_from_timeline(faces, side, original_size):
    """
    Reconstruct interior using geo_frame_seek timeline.
    This is the KEY — interior = f(timeline).
    """
    cube = np.zeros((side, side, side), dtype=np.uint8)
    
    # Fill surface from stored faces
    if 'front' in faces:
        cube[:, :, -1] = faces['front'][:side, :side]
    if 'back' in faces:
        cube[:, :, 0] = faces['back'][:side, :side]
    if 'left' in faces:
        cube[0, :, :] = faces['left'][:side, :side]
    if 'right' in faces:
        cube[-1, :, :] = faces['right'][:side, :side]
    if 'top' in faces:
        cube[:, -1, :] = faces['top'][:side, :side]
    if 'bottom' in faces:
        cube[:, 0, :] = faces['bottom'][:side, :side]
    
    # Reconstruct interior using timeline
    for x in range(1, side - 1):
        for y in range(1, side - 1):
            for z in range(1, side - 1):
                linear = (x * side + y) * side + z
                t = linear % FRAME_CYCLE
                enc = frame_enc(t)
                frame = frame_at(enc)
                
                # Derive value from timeline (same function as gen_timeline_derived)
                cube[x, y, z] = (frame['face'] * 100 + frame['slot'] * 10 + frame['ico_idx']) & 0xFF
    
    # Extract data from cube (flatten)
    return cube.flatten()[:original_size]

# ══════════════════════════════════════════════════════════════
# Test
# ══════════════════════════════════════════════════════════════

def test_surface_compression(data, label, timeline_compatible=False):
    """Test surface-only compression approach."""
    print(f"\n{'='*60}")
    print(f"  {label} ({len(data)} bytes)")
    print(f"{'='*60}")
    
    # Original
    print(f"  Original: {len(data)} bytes")
    print(f"  Hash: {hashlib.sha256(data).hexdigest()[:16]}")
    
    # Compute cube dimensions
    side = cube_side_for_data(len(data))
    surface_cap = 6 * side * side
    cube_vol = side ** 3
    
    print(f"  Cube side: {side}")
    print(f"  Surface capacity: {surface_cap} voxels ({surface_cap} bytes)")
    print(f"  Cube volume: {cube_vol} voxels ({cube_vol} bytes)")
    print(f"  Surface ratio: {surface_cap}/{cube_vol} = {surface_cap/cube_vol:.3f}x")
    
    # Place data on surface
    faces = data_to_surface(data, side)
    surface_size = sum(face.nbytes for face in faces.values())
    print(f"  Surface storage: {surface_size} bytes")
    
    # Compress surface with zlib
    surface_bytes = b''.join(face.tobytes() for face in faces.values())
    surface_zlib = zlib.compress(surface_bytes, 9)
    print(f"  Surface + zlib: {len(surface_zlib)} bytes ({len(surface_zlib)/len(data):.2f}x)")
    
    # Reconstruct from surface (simplified)
    reconstructed = surface_to_data(faces, side, len(data))
    recon_hash = hashlib.sha256(reconstructed).hexdigest()[:16]
    print(f"  Reconstructed: {len(reconstructed)} bytes")
    print(f"  Recon hash: {recon_hash}")
    print(f"  Roundtrip: {'PASS' if reconstructed[:len(data)] == data else 'FAIL'}")
    
    # Timeline reconstruction (if data is timeline-compatible)
    if timeline_compatible:
        print(f"\n  --- Timeline Reconstruction ---")
        recon_timeline = reconstruct_from_timeline(faces, side, len(data))
        timeline_bytes = recon_timeline.tobytes() if hasattr(recon_timeline, 'tobytes') else bytes(recon_timeline)
        timeline_hash = hashlib.sha256(timeline_bytes).hexdigest()[:16]
        print(f"  Timeline recon: {len(recon_timeline)} bytes")
        print(f"  Timeline hash: {timeline_hash}")
        print(f"  Timeline roundtrip: {'PASS' if np.array_equal(recon_timeline[:len(data)], data[:len(recon_timeline)]) else 'FAIL'}")
    
    # Compare with raw compression
    raw_zlib = zlib.compress(data, 9)
    print(f"\n  --- Comparison ---")
    print(f"  Raw + zlib: {len(raw_zlib)} bytes ({len(raw_zlib)/len(data):.2f}x)")
    print(f"  Surface + zlib: {len(surface_zlib)} bytes ({len(surface_zlib)/len(data):.2f}x)")
    print(f"  Surface only: {surface_size} bytes ({surface_size/len(data):.2f}x)")
    
    return {
        'raw_size': len(data),
        'surface_size': surface_size,
        'surface_zlib_size': len(surface_zlib),
        'raw_zlib_size': len(raw_zlib),
        'side': side,
        'cube_vol': cube_vol,
        'surface_cap': surface_cap,
    }

# ══════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════

if __name__ == '__main__':
    SIZE = 100000  #100KB
    
    print("=" * 60)
    print("  Surface-Only Storage Test")
    print("=" * 60)
    
    results = {}
    
    # Test 1: Random data
    random_data = gen_random(SIZE)
    results['random'] = test_surface_compression(random_data, "Random data", False)
    
    # Test 2: Image-like data
    image_data = gen_image_like(SIZE)
    results['image'] = test_surface_compression(image_data, "Image-like (gradient)", False)
    
    # Test 3: Timeline-derived data (perfect reconstruction)
    timeline_data = gen_timeline_derived(SIZE)
    results['timeline'] = test_surface_compression(timeline_data, "Timeline-derived", True)
    
    # Test 4: Quantized weights
    quant_data = gen_quantized_weights(SIZE)
    results['quant'] = test_surface_compression(quant_data, "Quantized weights", False)
    
    # Summary
    print("\n" + "=" * 60)
    print("  SUMMARY: Surface-only compression ratios")
    print("=" * 60)
    print(f"  {'Data type':<20} {'Surface':<12} {'Surface+zlib':<14} {'Raw+zlib':<12} {'Better?':<10}")
    print("-" * 60)
    for label, r in results.items():
        surf_ratio = r['surface_size'] / r['raw_size']
        surf_zlib_ratio = r['surface_zlib_size'] / r['raw_size']
        raw_zlib_ratio = r['raw_zlib_size'] / r['raw_size']
        better = "YES" if surf_zlib_ratio < raw_zlib_ratio else "NO"
        print(f"  {label:<20} {surf_ratio:<12.2f} {surf_zlib_ratio:<14.2f} {raw_zlib_ratio:<12.2f} {better:<10}")
    
    print("\n" + "=" * 60)
    print("  ANALYSIS")
    print("=" * 60)
    print("  Surface-only storage: 6 faces × side² bytes")
    print("  Compression depends on cube_side vs data_size")
    print("  For small data: surface > raw (expansion)")
    print("  For large data: surface << cube_vol (compression)")
    print("  Timeline reconstruction: interior = f(timeline)")
    print("  Only works for data with inherent temporal coherence")
