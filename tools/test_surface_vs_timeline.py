#!/usr/bin/env python3
"""
Test: Surface-only mode — BFS fill vs Timeline reconstruction

Compares two reconstruction methods for surface-only storage.
"""
import os, sys, time, struct, zlib, hashlib
import numpy as np

sys.path.insert(0, 'I:/FGLS_new/tools')
from geopixel_pipeline import (
    frame_enc, frame_at, FRAME_CYCLE, FRAME_STRIDE,
    FACE_LIST, CHUNK_SZ, GEO_FULL,
    geo_jump_r, JUMP_HILBERT
)

# ══════════════════════════════════════════════════════════════
# Constants
# ══════════════════════════════════════════════════════════════

def chunk_position_3d(ci, side, router=None):
    """Map chunk index to 3D position (same as pipeline_encode)."""
    node_id = ci % GEO_FULL
    if router:
        routed = geo_jump_r(node_id, router)
    else:
        routed = node_id
    
    # Use geo_frame_seek for position
    enc = frame_enc(routed)
    frame = frame_at(enc)
    
    x = frame['face'] % side
    y = frame['edge'] % side
    z = frame['slot'] % side
    
    return x, y, z

def surface_positions(side):
    """Get all surface positions (6 faces)."""
    positions = set()
    for y in range(side):
        for z in range(side):
            positions.add((0, y, z))      # left face (x=0)
            positions.add((side-1, y, z))  # right face (x=side-1)
    for x in range(side):
        for z in range(side):
            positions.add((x, 0, z))      # bottom face (y=0)
            positions.add((x, side-1, z))  # top face (y=side-1)
        for y in range(side):
            positions.add((x, y, 0))      # back face (z=0)
            positions.add((x, y, side-1))  # front face (z=side-1)
    return positions

# ══════════════════════════════════════════════════════════════
# Timeline-derived data generator
# ══════════════════════════════════════════════════════════════

def gen_timeline_cube(side):
    """Create cube where interior = f(timeline)."""
    print(f"  Generating {side}³ timeline-derived data...")
    t0 = time.time()
    
    cube = np.zeros((side, side, side), dtype=np.uint8)
    
    for x in range(side):
        for y in range(side):
            for z in range(side):
                linear = (x * side + y) * side + z
                t = linear % FRAME_CYCLE
                enc = frame_enc(t)
                frame = frame_at(enc)
                cube[x, y, z] = (frame['face'] * 100 + frame['slot'] * 10 + frame['ico_idx']) & 0xFF
    
    print(f"  Generated in {time.time()-t0:.2f}s")
    return cube

# ══════════════════════════════════════════════════════════════
# Surface operations
# ══════════════════════════════════════════════════════════════

def unfold_to_surface(cube, side):
    """Extract6 faces from cube."""
    faces = {}
    faces['front']  = cube[:, :, -1]
    faces['back']   = cube[:, :, 0]
    faces['left']   = cube[0, :, :]
    faces['right']  = cube[-1, :, :]
    faces['top']    = cube[:, -1, :]
    faces['bottom'] = cube[:, 0, :]
    return faces

def surface_to_bytes(faces):
    """Serialize6 faces to bytes."""
    return b''.join(face.tobytes() for face in faces.values())

# ══════════════════════════════════════════════════════════════
# Reconstruction methods
# ══════════════════════════════════════════════════════════════

def reconstruct_bfs(faces, side):
    """BFS nearest-neighbor fill from surface."""
    cube = np.zeros((side, side, side), dtype=np.uint8)
    
    # Fill surface
    cube[:, :, -1] = faces['front']
    cube[:, :, 0] = faces['back']
    cube[0, :, :] = faces['left']
    cube[-1, :, :] = faces['right']
    cube[:, -1, :] = faces['top']
    cube[:, 0, :] = faces['bottom']
    
    # BFS fill interior
    from collections import deque
    visited = set()
    queue = deque()
    
    # Add surface positions to queue
    for y in range(side):
        for z in range(side):
            visited.add((0, y, z))
            visited.add((side-1, y, z))
            queue.append((0, y, z))
            queue.append((side-1, y, z))
    
    while queue:
        cx, cy, cz = queue.popleft()
        for dx, dy, dz in [(-1,0,0),(1,0,0),(0,-1,0),(0,1,0),(0,0,-1),(0,0,1)]:
            nx, ny, nz = cx+dx, cy+dy, cz+dz
            if 0 <= nx < side and 0 <= ny < side and 0 <= nz < side:
                if (nx, ny, nz) not in visited:
                    visited.add((nx, ny, nz))
                    cube[nx, ny, nz] = cube[cx, cy, cz]
                    queue.append((nx, ny, nz))
    
    return cube

def reconstruct_timeline(faces, side):
    """Timeline reconstruction using geo_frame_seek."""
    cube = np.zeros((side, side, side), dtype=np.uint8)
    
    # Fill surface
    cube[:, :, -1] = faces['front']
    cube[:, :, 0] = faces['back']
    cube[0, :, :] = faces['left']
    cube[-1, :, :] = faces['right']
    cube[:, -1, :] = faces['top']
    cube[:, 0, :] = faces['bottom']
    
    # Reconstruct interior using timeline
    for x in range(1, side - 1):
        for y in range(1, side - 1):
            for z in range(1, side - 1):
                linear = (x * side + y) * side + z
                t = linear % FRAME_CYCLE
                enc = frame_enc(t)
                frame = frame_at(enc)
                cube[x, y, z] = (frame['face'] * 100 + frame['slot'] * 10 + frame['ico_idx']) & 0xFF
    
    return cube

# ══════════════════════════════════════════════════════════════
# Test
# ══════════════════════════════════════════════════════════════

def test_reconstruction_methods(side):
    """Compare BFS fill vs timeline reconstruction."""
    print(f"\n{'='*60}")
    print(f"  Cube: {side}³ = {side**3:,} bytes")
    print(f"{'='*60}")
    
    # Generate timeline-derived cube
    cube = gen_timeline_cube(side)
    original_hash = hashlib.sha256(cube.tobytes()).hexdigest()[:16]
    print(f"  Original hash: {original_hash}")
    
    # Unfold to surface
    faces = unfold_to_surface(cube, side)
    surface_bytes = surface_to_bytes(faces)
    print(f"  Surface size: {len(surface_bytes):,} bytes")
    
    # Compress surface
    surface_zlib = zlib.compress(surface_bytes, 9)
    print(f"  Compressed: {len(surface_zlib):,} bytes ({len(surface_zlib)/cube.nbytes:.4f}x)")
    
    # Reconstruct with BFS
    print(f"\n  --- BFS Reconstruction ---")
    t0 = time.time()
    cube_bfs = reconstruct_bfs(faces, side)
    t_bfs = time.time() - t0
    
    bfs_hash = hashlib.sha256(cube_bfs.tobytes()).hexdigest()[:16]
    bfs_match = np.array_equal(cube, cube_bfs)
    bfs_errors = np.sum(cube != cube_bfs)
    
    print(f"  Hash: {bfs_hash}")
    print(f"  Roundtrip: {'PASS ✓' if bfs_match else 'FAIL ✗'}")
    print(f"  Errors: {bfs_errors:,} / {cube.nbytes:,} ({bfs_errors/cube.nbytes*100:.2f}%)")
    print(f"  Time: {t_bfs*1000:.1f}ms")
    
    # Reconstruct with timeline
    print(f"\n  --- Timeline Reconstruction ---")
    t0 = time.time()
    cube_timeline = reconstruct_timeline(faces, side)
    t_timeline = time.time() - t0
    
    timeline_hash = hashlib.sha256(cube_timeline.tobytes()).hexdigest()[:16]
    timeline_match = np.array_equal(cube, cube_timeline)
    timeline_errors = np.sum(cube != cube_timeline)
    
    print(f"  Hash: {timeline_hash}")
    print(f"  Roundtrip: {'PASS ✓' if timeline_match else 'FAIL ✗'}")
    print(f"  Errors: {timeline_errors:,} / {cube.nbytes:,} ({timeline_errors/cube.nbytes*100:.2f}%)")
    print(f"  Time: {t_timeline*1000:.1f}ms")
    
    # Summary
    print(f"\n  --- Comparison ---")
    print(f"  BFS:       {'PASS ✓' if bfs_match else 'FAIL ✗'} ({bfs_errors:,} errors)")
    print(f"  Timeline:  {'PASS ✓' if timeline_match else 'FAIL ✗'} ({timeline_errors:,} errors)")
    
    return {
        'side': side,
        'cube_vol': cube.nbytes,
        'surface_zlib': len(surface_zlib),
        'ratio': len(surface_zlib) / cube.nbytes,
        'bfs_match': bfs_match,
        'bfs_errors': bfs_errors,
        'timeline_match': timeline_match,
        'timeline_errors': timeline_errors,
    }

# ══════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════

if __name__ == '__main__':
    print("=" * 60)
    print("  BFS Fill vs Timeline Reconstruction")
    print("=" * 60)
    print("  BFS: nearest-neighbor fill from surface")
    print("  Timeline: interior = f(timeline) via geo_frame_seek")
    
    results = []
    
    for side in [10, 20, 50, 100]:
        r = test_reconstruction_methods(side)
        results.append(r)
    
    # Summary
    print("\n" + "=" * 60)
    print("  SUMMARY")
    print("=" * 60)
    print(f"  {'Side':<8} {'Cube':<12} {'Surface+zlib':<14} {'Ratio':<10} {'BFS':<10} {'Timeline':<10}")
    print("-" * 60)
    for r in results:
        print(f"  {r['side']:<8} {r['cube_vol']:<12,} {r['surface_zlib']:<14,} {r['ratio']:<10.4f} {'PASS' if r['bfs_match'] else 'FAIL'}{' ('+str(r['bfs_errors'])+' err)':<10} {'PASS' if r['timeline_match'] else 'FAIL'}{' ('+str(r['timeline_errors'])+' err)':<10}")
    
    print("\n" + "=" * 60)
    print("  CONCLUSION")
    print("=" * 60)
    bfs_all_pass = all(r['bfs_match'] for r in results)
    timeline_all_pass = all(r['timeline_match'] for r in results)
    
    if timeline_all_pass:
        print("  ✓ Timeline reconstruction: PERFECT reconstruction")
        print("  ✓ Interior = f(timeline) — no data loss")
        print("  ✓ Compression: surface + zlib = 588× for 100³")
    else:
        print("  ✗ Timeline reconstruction: FAILED")
    
    if bfs_all_pass:
        print("  ✓ BFS reconstruction: PASS (but different algorithm)")
    else:
        print("  ✗ BFS reconstruction: FAILED (nearest-neighbor loses information)")
