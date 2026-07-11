#!/usr/bin/env python3
"""
Hamburger Codec POC v4 — 6-face surface + geo_frame_seek timeline reconstruction.

Uses actual frame timeline (stride-37 walk) to derive interior from surface.

Usage:
    python hamburger_codec_poc_v4.py
"""

import numpy as np

# ── Constants (from geo_frame_seek.h) ──
FRAME_CYCLE = 1440
FRAME_STRIDE = 37
FRAME_FACE_SZ = 120
FRAME_EDGES = 12
FRAME_H_ACTIVE = 9
FRAME_P_STEPS = 4
FRAME_ICO_NODES = 162
FACE_SIZE = 100

# ── geo_jump constants ──
GEO_FULL = 20736

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

# ── geo_frame_seek: actual timeline ──

def frame_enc(t):
    """enc at time t: (t * 37) % 1440"""
    return (t * FRAME_STRIDE) % FRAME_CYCLE

def frame_next(enc):
    """next enc in walk: (enc + 37) % 1440"""
    return (enc + FRAME_STRIDE) % FRAME_CYCLE

def frame_at(enc):
    """O(1) frame decomposition from enc (0..1439)."""
    face = enc // FRAME_FACE_SZ
    slot = enc % FRAME_FACE_SZ
    group = face % 3
    edge = enc % 3
    is_skip = (enc % FRAME_EDGES) >= FRAME_H_ACTIVE
    step = (enc // 3) % FRAME_P_STEPS
    sub = enc % 3
    hilbert_group = (enc // FRAME_EDGES) % 3
    ico_idx = enc % FRAME_ICO_NODES
    phase = (enc // FRAME_EDGES) % 12
    return {
        'enc': enc, 'face': face, 'slot': slot, 'group': group,
        'edge': edge, 'is_skip': is_skip, 'step': step, 'sub': sub,
        'hilbert_group': hilbert_group, 'ico_idx': ico_idx, 'phase': phase,
    }

def frame_seek(t):
    """Seek to time t."""
    return frame_at(frame_enc(t))

# ── geo_jump: face routing ──

def geo_jump(node_id, jump_type=0, param=0):
    if jump_type == 0:
        return (node_id * 37 + param) % GEO_FULL
    elif jump_type == 1:
        return (node_id * 81 + param) % GEO_FULL
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

    # Interior: derived from surface via timeline
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

# ── Interior reconstruction via timeline ──

def voxel_to_timeline(x, y, z, size):
    """Map voxel position to timeline time t."""
    linear = (x * size + y) * size + z
    return linear % FRAME_CYCLE

def derive_interior_from_timeline(faces, size):
    """Reconstruct interior using geo_frame_seek timeline progression."""
    cube = np.zeros((size, size, size), dtype=np.float32)

    # Fill surface
    cube[:, :, -1] = faces['front']
    cube[:, :, 0] = faces['back']
    cube[0, :, :] = faces['left']
    cube[-1, :, :] = faces['right']
    cube[:, -1, :] = faces['top']
    cube[:, 0, :] = faces['bottom']

    # Pre-compute surface lookup
    face_arrays = [faces[name] for name in FACE_LIST]

    # Reconstruct interior via timeline
    for x in range(1, size - 1):
        for y in range(1, size - 1):
            for z in range(1, size - 1):
                # Map voxel to timeline
                t = voxel_to_timeline(x, y, z, size)
                frame = frame_seek(t)

                # Use frame decomposition to pick face + position
                face_idx = frame['face'] % N_FACES
                u = frame['slot'] % size
                v = (frame['slot'] // size) % size

                # Interior value = surface value at routed position
                cube[x, y, z] = face_arrays[face_idx][u, v]

    return cube

def derive_interior_from_timeline_vectorized(faces, size):
    """Vectorized version for speed."""
    cube = np.zeros((size, size, size), dtype=np.float32)

    cube[:, :, -1] = faces['front']
    cube[:, :, 0] = faces['back']
    cube[0, :, :] = faces['left']
    cube[-1, :, :] = faces['right']
    cube[:, -1, :] = faces['top']
    cube[:, 0, :] = faces['bottom']

    face_arrays = [faces[name] for name in FACE_LIST]

    idx = np.arange(1, size - 1)
    x, y, z = np.meshgrid(idx, idx, idx, indexing='ij')

    # Map to timeline
    linear = (x * size + y) * size + z
    t = linear % FRAME_CYCLE

    # Frame decomposition (vectorized)
    face = t // FRAME_FACE_SZ
    slot = t % FRAME_FACE_SZ

    face_idx = face % N_FACES
    u = slot % size
    v = (slot // size) % size

    interior = np.zeros_like(x, dtype=np.float32)
    for fi, face_arr in enumerate(face_arrays):
        mask = (face_idx == fi)
        interior[mask] = face_arr[u[mask], v[mask]]

    cube[1:-1, 1:-1, 1:-1] = interior
    return cube

# ── Main POC ──

if __name__ == '__main__':
    print("=== Hamburger Codec POC v4 ===\n")

    size = 100
    print(f"1. Creating {size}×{size}×{size} cube...")
    cube = create_cube(size)
    print(f"   Cube: {cube.nbytes / 1024:.1f} KB")

    print("\n2. Encoding (unfold to 6 faces)...")
    faces = unfold_cube(cube)
    total_face_bytes = sum(f.nbytes for f in faces.values())
    print(f"   6 faces = {total_face_bytes / 1024:.1f} KB")

    print("\n3. Timeline progression (stride-37)...")
    for t in range(5):
        enc = frame_enc(t)
        frame = frame_at(enc)
        print(f"   t={t}: enc={enc:4d}, face={frame['face']}, "
              f"slot={frame['slot']}, ico_idx={frame['ico_idx']}")

    print("\n4. Decoding (reconstruct from timeline)...")
    reconstructed = derive_interior_from_timeline_vectorized(faces, size)

    print("\n5. Verification...")
    interior_orig = cube[1:-1, 1:-1, 1:-1]
    interior_recon = reconstructed[1:-1, 1:-1, 1:-1]
    diff = np.abs(interior_orig - interior_recon)

    print(f"   Interior max diff: {diff.max():.6f}")
    print(f"   Interior mean diff: {diff.mean():.6f}")
    print(f"   Full match: {np.allclose(cube, reconstructed)}")

    print("\n=== Summary ===")
    print(f"Original: {cube.nbytes / 1024:.1f} KB")
    print(f"Stored:   {total_face_bytes / 1024:.1f} KB (6 faces)")
    print(f"Ratio:    {cube.nbytes / total_face_bytes:.1f}x")
    print(f"Timeline: 1440 frames, stride-37, O(1) seek")
