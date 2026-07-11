#!/usr/bin/env python3
"""
Hamburger Codec POC v3 — 6-face surface + geo_jump interior reconstruction.

Concept:
  1. Store 6 faces (surface) = 60KB
  2. Use geo_jump to route interior voxels to surface addresses
  3. Reconstruct interior from surface via deterministic routing

Usage:
    python hamburger_codec_poc_v3.py
"""

import numpy as np

# ── Constants (from geo_frame_seek.h) ──
FRAME_CYCLE = 1440
FRAME_STRIDE = 37
FRAME_FACE_SZ = 120
FRAME_EDGES = 12
FACE_SIZE = 100

# ── geo_jump constants (from geo_jump.h) ──
GEO_METATRON_COLS = 4
GEO_METATRON_ROWS = 4
GEO_METATRON_FLOORS = 3
GEO_METATRON_CELLS = GEO_METATRON_COLS * GEO_METATRON_ROWS
GEO_BLOCK = GEO_METATRON_CELLS * GEO_METATRON_FLOORS
GEO_TOWER = GEO_BLOCK * GEO_METATRON_FLOORS
GEO_FULL = GEO_TOWER * GEO_TOWER  # 20736

# ── 6-face cube layout ──
FACES = {
    'front':  0,  # +Z
    'back':   1,  # -Z
    'left':   2,  # -X
    'right':  3,  # +X
    'top':    4,  # +Y
    'bottom': 5,  # -Y
}
FACE_LIST = list(FACES.keys())
N_FACES = len(FACE_LIST)

# ── geo_frame_seek: deterministic frame progression ──

def frame_at(enc):
    face = enc // FRAME_FACE_SZ
    group = face % 3
    edge = enc % 3
    is_skip = (enc % 4 == 3)
    step = (enc // 3) % 4
    sub = enc % 3
    hilbert_group = (enc // 12) % 3
    ico_idx = enc % 162
    return {
        'enc': enc, 'face': face, 'group': group, 'edge': edge,
        'is_skip': is_skip, 'step': step, 'sub': sub,
        'hilbert_group': hilbert_group, 'ico_idx': ico_idx,
    }

def next_frame(enc):
    return (enc + FRAME_STRIDE) % FRAME_CYCLE

# ── geo_jump: face routing ──

def geo_jump(node_id, jump_type=0, param=0):
    if jump_type == 0:
        return (node_id * 37 + param) % GEO_FULL
    elif jump_type == 1:
        return (node_id * 81 + param) % GEO_FULL
    elif jump_type == 2:
        return (node_id * 12 + param) % GEO_FULL
    return node_id

# ── 6-face cube operations ──

def create_cube(size=100):
    cube = np.zeros((size, size, size), dtype=np.float32)

    for face_name, face_idx in FACES.items():
        value = (face_idx + 1) * 1000
        if face_name == 'front':
            cube[:, :, -1] = value + np.arange(size)[:, None] + np.arange(size)[None, :]
        elif face_name == 'back':
            cube[:, :, 0] = value + np.arange(size)[:, None] + np.arange(size)[None, :]
        elif face_name == 'left':
            cube[0, :, :] = value + np.arange(size)[:, None] + np.arange(size)[None, :]
        elif face_name == 'right':
            cube[-1, :, :] = value + np.arange(size)[:, None] + np.arange(size)[None, :]
        elif face_name == 'top':
            cube[:, -1, :] = value + np.arange(size)[:, None] + np.arange(size)[None, :]
        elif face_name == 'bottom':
            cube[:, 0, :] = value + np.arange(size)[:, None] + np.arange(size)[None, :]

    ii, jj, kk = np.meshgrid(
        np.arange(1, size - 1), np.arange(1, size - 1), np.arange(1, size - 1), indexing='ij'
    )
    cube[1:-1, 1:-1, 1:-1] = 500 + (ii * 31 + jj * 17 + kk * 7) % 500
    return cube

def unfold_cube(cube):
    faces = {}
    faces['front'] = cube[:, :, -1]
    faces['back'] = cube[:, :, 0]
    faces['left'] = cube[0, :, :]
    faces['right'] = cube[-1, :, :]
    faces['top'] = cube[:, -1, :]
    faces['bottom'] = cube[:, 0, :]
    return faces

# ── Interior fill via geo_jump (vectorized) ──

def fill_interior_from_faces_vectorized(faces, size):
    cube = np.zeros((size, size, size), dtype=np.float32)

    cube[:, :, -1] = faces['front']
    cube[:, :, 0] = faces['back']
    cube[0, :, :] = faces['left']
    cube[-1, :, :] = faces['right']
    cube[:, -1, :] = faces['top']
    cube[:, 0, :] = faces['bottom']

    idx = np.arange(1, size - 1)
    x, y, z = np.meshgrid(idx, idx, idx, indexing='ij')

    node_id = ((x * size + y) * size + z) % GEO_FULL
    hilbert_route = (node_id * 37 + (x + y + z)) % GEO_FULL
    peano_route = (node_id * 81 + (x * y * z + 1)) % GEO_FULL

    face_idx = hilbert_route % N_FACES
    u = peano_route % size
    v = (peano_route // size) % size

    interior = np.zeros_like(x, dtype=np.float32)
    for fi, face_name in enumerate(FACE_LIST):
        mask = (face_idx == fi)
        face_arr = faces[face_name]
        interior[mask] = face_arr[u[mask], v[mask]]

    cube[1:-1, 1:-1, 1:-1] = interior
    return cube

# ── Main POC ──

if __name__ == '__main__':
    print("=== Hamburger Codec POC v3 ===\n")

    size = 100
    print(f"1. Creating {size}×{size}×{size} cube with interior...")
    cube = create_cube(size)
    print(f"   Cube: {cube.nbytes / 1024:.1f} KB")
    print(f"   Interior voxels: {(size-2)**3}")

    print("\n2. Encoding (unfold to 6 faces)...")
    faces = unfold_cube(cube)
    total_face_bytes = sum(f.nbytes for f in faces.values())
    print(f"   6 faces = {total_face_bytes / 1024:.1f} KB")
    print(f"   Ratio: {cube.nbytes / total_face_bytes:.1f}x")

    print("\n3. Frame progression (geo_frame_seek)...")
    enc = 0
    for i in range(5):
        frame = frame_at(enc)
        print(f"   enc={enc:4d}: face={frame['face']:2d}, "
              f"group={frame['group']}, edge={frame['edge']}, "
              f"ico_idx={frame['ico_idx']}")
        enc = next_frame(enc)

    print("\n4. geo_jump routing...")
    for node in [0, 1000, 5000, 10000, 20000]:
        routed = geo_jump(node, jump_type=0, param=42)
        print(f"   node={node:5d} → routed={routed:5d}")

    print("\n5. Decoding (reconstruct from 6 faces + geo_jump interior)...")
    reconstructed = fill_interior_from_faces_vectorized(faces, size)

    print("\n6. Verification...")
    surface_match = np.allclose(cube[:, :, -1], reconstructed[:, :, -1])
    print(f"   Surface (front): {surface_match}")

    interior_orig = cube[1:-1, 1:-1, 1:-1]
    interior_recon = reconstructed[1:-1, 1:-1, 1:-1]
    interior_match = np.allclose(interior_orig, interior_recon)
    print(f"   Interior match: {interior_match}")

    diff = np.abs(interior_orig - interior_recon)
    print(f"   Interior max diff: {diff.max():.6f}")
    print(f"   Interior mean diff: {diff.mean():.6f}")

    full_match = np.allclose(cube, reconstructed)
    print(f"   Full cube match: {full_match}")

    print("\n=== Summary ===")
    print(f"Original:   {cube.nbytes / 1024:.1f} KB (3D)")
    print(f"Stored:     {total_face_bytes / 1024:.1f} KB (6 faces)")
    print(f"Ratio:      {cube.nbytes / total_face_bytes:.1f}x")
    print(f"Interior:   {(size-2)**3} voxels via geo_jump routing")
    print(f"Full match: {full_match}")
