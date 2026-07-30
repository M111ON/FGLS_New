"""
contour_mask_unified.py — Silk Screen ≡ Contour Mask (unified)
Contour Mask = measurement tool on 10×10×N cube
"""

import math
import random

def explore_unified():
    print("=" * 70)
    print("Silk Screen ≡ Contour Mask — Unified Model")
    print("=" * 70)
    
    # === Structure ===
    FACE_GRID = 10  # 10×10 face
    N_DEPTH = 10    # 10 depth (round(f(time)))
    CUBES = 10      # number of cubes
    FACES = 6       # 6 faces per cube
    
    # Contour Mask dimensions
    FACE_POINTS = FACE_GRID * FACE_GRID  # 100 points per face
    CUBE_POINTS = FACE_POINTS * N_DEPTH  # 1000 points per cube
    TOTAL_POINTS = CUBE_POINTS * CUBES   # 10000 points total
    
    print(f"\n[Cube Structure]")
    print(f"  Face grid: {FACE_GRID}×{FACE_GRID} = {FACE_POINTS} points/face")
    print(f"  Depth N: {N_DEPTH} (round(f(time)))")
    print(f"  Points per cube: {FACE_POINTS} × {N_DEPTH} = {CUBE_POINTS}")
    print(f"  Total cubes: {CUBES}")
    print(f"  Total points: {CUBE_POINTS} × {CUBES} = {TOTAL_POINTS}")
    
    # === Contour Mask ===
    print(f"\n[Contour Mask = Measurement Tool]")
    print(f"  6 faces: A, B, C, D, E, F")
    print(f"  Each face: {FACE_GRID}×{FACE_GRID} = {FACE_POINTS} measurement points")
    print(f"  Depth: {N_DEPTH} layers (time/frequency)")
    print()
    print(f"  Measurement: beam(dir, face_grid, depth) → weight")
    print(f"  = Silk Screen: filter[box][dir][tick]")
    
    # === Filter Array ===
    print(f"\n[Filter Array]")
    print(f"  filter[cube][face][grid_y][grid_x][depth] = weight")
    print(f"  Dimensions: {CUBES} × {FACES} × {FACE_GRID} × {FACE_GRID} × {N_DEPTH}")
    print(f"  Total slots: {CUBES * FACES * FACE_GRID * FACE_GRID * N_DEPTH}")
    print(f"  Storage: {CUBES * FACES * FACE_GRID * FACE_GRID * N_DEPTH} bytes (int8)")
    
    # === Read Heads ===
    print(f"\n[Read Heads]")
    print(f"  6 faces × {FACE_GRID}×{FACE_GRID} = {FACES * FACE_POINTS} read heads")
    print(f"  Per cube: {FACES * FACE_POINTS}")
    print(f"  Total: {FACES * FACE_POINTS * CUBES}")
    
    # === Contour Mask Operation ===
    print(f"\n[Contour Mask Operation]")
    print(f"  Encode: data → filter[face][grid][depth]")
    print(f"  Decode: filter[face][grid][depth] → data")
    print(f"  Identity: 1:1 copy (lossless)")
    
    # === 15 Pairs ===
    print(f"\n[15 Pairs (C(6,2))]")
    pairs = []
    dirs = ['A', 'B', 'C', 'D', 'E', 'F']
    for i in range(len(dirs)):
        for j in range(i+1, len(dirs)):
            pairs.append(f"{dirs[i].lower()}{dirs[j].lower()}")
    
    print(f"  {len(pairs)} pairs: {', '.join(pairs)}")
    print()
    print(f"  Each pair = 1 view of same cube face")
    print(f"  {len(pairs)} views × {FACE_POINTS} points = {len(pairs) * FACE_POINTS} observations")
    
    # === Time/Frequency Dimension ===
    print(f"\n[Time/Frequency Dimension]")
    print(f"  Depth N = round(f(time))")
    print(f"  This means: cube size varies with observation frequency")
    print()
    print(f"  Low frequency → small N (coarse measurement)")
    print(f"  High frequency → large N (fine measurement)")
    print()
    print(f"  = h-depth variable resolution!")
    print(f"  Low freq: N=4 → 10×10×4 = 400 points")
    print(f"  Mid freq: N=7 → 10×10×7 = 700 points")
    print(f"  High freq: N=10 → 10×10×10 = 1000 points")
    
    # === Demo ===
    print(f"\n[Demo: Contour Mask on 10×10×10 cube]")
    
    # Create a cube
    cube = [[[random.randint(0, 255) for _ in range(N_DEPTH)] 
             for _ in range(FACE_GRID)] 
            for _ in range(FACE_GRID)]
    
    print(f"  Cube created: {FACE_GRID}×{FACE_GRID}×{N_DEPTH}")
    print(f"  Sample values at (5,5,0..9):")
    for d in range(N_DEPTH):
        print(f"    depth={d}: {cube[5][5][d]}")
    
    # Read from face A (z=0 plane)
    print(f"\n  Reading face A (z=0 plane):")
    for y in range(3):
        row = []
        for x in range(3):
            row.append(str(cube[y][x][0]))
        print(f"    row {y}: {' '.join(row)}")
    
    # Read from face B (z=9 plane)
    print(f"\n  Reading face B (z=9 plane):")
    for y in range(3):
        row = []
        for x in range(3):
            row.append(str(cube[y][x][9]))
        print(f"    row {y}: {' '.join(row)}")
    
    # Pair ab: A + B
    print(f"\n  Pair ab (A + B):")
    for y in range(3):
        row = []
        for x in range(3):
            val = cube[y][x][0] + cube[y][x][9]
            row.append(str(val))
        print(f"    row {y}: {' '.join(row)}")
    
    # === Summary ===
    print(f"\n{'='*70}")
    print(f"UNIFIED MODEL SUMMARY")
    print(f"{'='*70}")
    print(f"  Silk Screen ≡ Contour Mask")
    print(f"  Cube: 10×10×N (N = round(f(time)))")
    print(f"  Faces: 6 (A-F)")
    print(f"  Measurement: beam(face, grid, depth) → weight")
    print(f"  Filter: identity (1:1, lossless)")
    print(f"  Pairs: C(6,2) = 15 views")
    print()
    print(f"  Key insight: N varies with time/frequency")
    print(f"  = adaptive resolution based on observation")

if __name__ == "__main__":
    explore_unified()
