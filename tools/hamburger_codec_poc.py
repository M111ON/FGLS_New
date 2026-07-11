#!/usr/bin/env python3
"""
Hamburger Codec POC — 6-face cube unfold + geo_frame_seek progression.

Concept:
  1. 3D cube (100×100×100) → 6 faces (100×100 each) = 60KB
  2. geo_frame_seek: deterministic frame progression (stride-37, 1440 frames)
  3. geo_jump: routing between faces (20736 nodes)

This POC demonstrates:
  - 6-face unfold/reconstruct
  - Deterministic frame progression
  - Face routing via geo_jump

Usage:
    python hamburger_codec_poc.py
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

# ── geo_frame_seek: deterministic frame progression ──

def frame_at(enc):
    """O(1) frame decomposition from enc (0..1439)."""
    face = enc // FRAME_FACE_SZ  # 0..11
    group = face % 3
    edge = enc % 3
    is_skip = (enc % 4 == 3)
    
    # Peano steps (on invert positions)
    step = (enc // 3) % 4
    sub = enc % 3
    hilbert_group = (enc // 12) % 3
    
    # icosphere address
    ico_idx = enc % 162
    
    return {
        'enc': enc,
        'face': face,
        'group': group,
        'edge': edge,
        'is_skip': is_skip,
        'step': step,
        'sub': sub,
        'hilbert_group': hilbert_group,
        'ico_idx': ico_idx,
    }

def next_frame(enc):
    """Next frame in timeline: (enc + 37) % 1440."""
    return (enc + FRAME_STRIDE) % FRAME_CYCLE

def seek_frame(target_enc):
    """O(1) seek to target enc."""
    return frame_at(target_enc)

# ── geo_jump: face routing ──

def geo_jump(node_id, jump_type=0, param=0):
    """Simplified geo_jump: route node to new position."""
    # JUMP_HILBERT = 0
    # For POC, just do modular arithmetic
    if jump_type == 0:  # Hilbert
        return (node_id * 37 + param) % GEO_FULL
    elif jump_type == 1:  # Peano
        return (node_id * 81 + param) % GEO_FULL
    elif jump_type == 2:  # Pentagon
        return (node_id * 12 + param) % GEO_FULL
    else:
        return node_id

# ── 6-face cube operations ──

def create_cube(size=100):
    """Create a 3D cube with unique values per face."""
    cube = np.zeros((size, size, size), dtype=np.float32)
    
    # Fill each face with unique values
    for face_name, face_idx in FACES.items():
        value = (face_idx + 1) * 1000  # Unique per face
        
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
            cube[0, :, :] = value + np.arange(size)[:, None] + np.arange(size)[None, :]
    
    return cube

def unfold_cube(cube):
    """Unfold 3D cube to 6 faces (100×100 each)."""
    size = cube.shape[0]
    faces = {}
    
    # Front face (Z = max)
    faces['front'] = cube[:, :, -1]
    
    # Back face (Z = 0)
    faces['back'] = cube[:, :, 0]
    
    # Left face (X = 0)
    faces['left'] = cube[0, :, :]
    
    # Right face (X = max)
    faces['right'] = cube[-1, :, :]
    
    # Top face (Y = max)
    faces['top'] = cube[:, -1, :]
    
    # Bottom face (Y = 0)
    faces['bottom'] = cube[:, 0, :]
    
    return faces

def reconstruct_cube(faces, size=100):
    """Reconstruct 3D cube from 6 faces."""
    cube = np.zeros((size, size, size), dtype=np.float32)
    
    # Front face (Z = max)
    cube[:, :, -1] = faces['front']
    
    # Back face (Z = 0)
    cube[:, :, 0] = faces['back']
    
    # Left face (X = 0)
    cube[0, :, :] = faces['left']
    
    # Right face (X = max)
    cube[-1, :, :] = faces['right']
    
    # Top face (Y = max)
    cube[:, -1, :] = faces['top']
    
    # Bottom face (Y = 0)
    cube[:, 0, :] = faces['bottom']
    
    return cube

# ── Hamburger Codec Pipeline ──

def hamburger_encode(cube):
    """Encode 3D cube as 6 faces + frame metadata."""
    faces = unfold_cube(cube)
    
    # Store frame metadata for each face
    frame_metadata = {}
    for face_name, face_idx in FACES.items():
        enc = face_idx * FRAME_FACE_SZ  # Frame enc for this face
        frame = frame_at(enc)
        frame_metadata[face_name] = frame
    
    return faces, frame_metadata

def hamburger_decode(faces, frame_metadata, size=100):
    """Reconstruct 3D cube from 6 faces + frame metadata."""
    # Frame metadata can be used for progressive detail
    # For POC, we just reconstruct from faces
    return reconstruct_cube(faces, size)

# ── Main POC ──

if __name__ == '__main__':
    print("=== Hamburger Codec POC ===\n")
    
    # 1. Create test cube
    size = 100
    print(f"1. Creating {size}×{size}×{size} cube...")
    cube = create_cube(size)
    print(f"   Cube shape: {cube.shape}")
    print(f"   Cube size: {cube.nbytes / 1024:.1f} KB")
    
    # 2. Encode (unfold to 6 faces)
    print("\n2. Encoding (unfold to 6 faces)...")
    faces, frame_metadata = hamburger_encode(cube)
    total_face_bytes = sum(f.nbytes for f in faces.values())
    print(f"   6 faces × {size}×{size} = {total_face_bytes / 1024:.1f} KB")
    print(f"   Compression: {cube.nbytes / total_face_bytes:.1f}x")
    
    # 3. Show frame progression
    print("\n3. Frame progression (geo_frame_seek)...")
    enc = 0
    for i in range(5):
        frame = frame_at(enc)
        print(f"   enc={enc:4d}: face={frame['face']:2d}, "
              f"group={frame['group']}, edge={frame['edge']}, "
              f"ico_idx={frame['ico_idx']}")
        enc = next_frame(enc)
    
    # 4. Show geo_jump routing
    print("\n4. geo_jump routing...")
    for node in [0, 1000, 5000, 10000, 20000]:
        routed = geo_jump(node, jump_type=0, param=42)
        print(f"   node={node:5d} → routed={routed:5d}")
    
    # 5. Decode (reconstruct cube)
    print("\n5. Decoding (reconstruct cube)...")
    reconstructed = hamburger_decode(faces, frame_metadata, size)
    
    # 6. Verify
    print("\n6. Verification...")
    match = np.allclose(cube, reconstructed)
    print(f"   Original == Reconstructed: {match}")
    
    # 7. Summary
    print("\n=== Summary ===")
    print(f"Original: {cube.nbytes / 1024:.1f} KB (3D)")
    print(f"Encoded:  {total_face_bytes / 1024:.1f} KB (6 faces)")
    print(f"Ratio:    {cube.nbytes / total_face_bytes:.1f}x")
    print(f"Frame:    1440 frames, stride-37, O(1) seek")
    print(f"Route:    20736 nodes, O(1) jump")
