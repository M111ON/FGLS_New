"""
contour_mask_displacement.py — Contour Mask as displacement model
6000 units move ± based on weight values
Read: diff XOR 0 → recover weight
"""

import random

def test_displacement():
    print("=" * 70)
    print("Contour Mask — Displacement Model")
    print("=" * 70)
    
    # === Structure ===
    CUBES = 10
    FACES = 6
    GRID = 10  # 10×10 per face
    UNITS = CUBES * FACES * GRID * GRID  # 6000 units
    
    print(f"\n[Structure]")
    print(f"  Units: {CUBES} cubes × {FACES} faces × {GRID}×{GRID} = {UNITS}")
    print(f"  Each unit: a 'pin' that moves ± based on weight")
    
    # === Displacement Model ===
    print(f"\n[Displacement Model]")
    print(f"  Encode: weight W → unit moves W positions from center (0)")
    print(f"  Decode: read displacement from 0 → weight W")
    print()
    print(f"  Example:")
    print(f"    Weight = +50 → unit moves 50 right from center")
    print(f"    Weight = -30 → unit moves 30 left from center")
    print(f"    Weight = 0 → unit stays at center")
    
    # === Demo: Single Unit ===
    print(f"\n[Demo: Single Unit]")
    print(f"  Starting position: 0 (center)")
    
    # Encode weight
    weight = 42
    position = weight  # displacement = weight
    
    print(f"  Weight: {weight}")
    print(f"  After encode: position = {position}")
    
    # Decode: XOR with 0
    decoded = position ^ 0  # XOR with 0 = position itself
    print(f"  Decode (XOR 0): {decoded}")
    print(f"  Match: {decoded == weight}")
    
    # === Demo: Multiple Units ===
    print(f"\n[Demo: Multiple Units (first 10)]")
    
    # Generate random weights
    random.seed(42)
    weights = [random.randint(-128, 127) for _ in range(10)]
    
    # Encode: displacement = weight
    positions = weights.copy()  # position = weight from center
    
    # Decode: XOR with 0
    decoded = [p ^ 0 for p in positions]
    
    print(f"  {'Weight':<10} {'Position':<10} {'XOR 0':<10} {'Match':<10}")
    print(f"  {'-'*40}")
    for i in range(10):
        match = decoded[i] == weights[i]
        print(f"  {weights[i]:<10} {positions[i]:<10} {decoded[i]:<10} {match}")
    
    # === Demo: XOR Reading ===
    print(f"\n[Demo: XOR Reading (diff against 0)]")
    
    # Two units with weights
    w1, w2 = 50, -30
    pos1, pos2 = w1, w2
    
    # XOR to compare
    xor_result = pos1 ^ pos2  # diff between two positions
    
    print(f"  Unit 1: weight={w1}, position={pos1}")
    print(f"  Unit 2: weight={w2}, position={pos2}")
    print(f"  XOR(pos1, pos2) = {xor_result}")
    print(f"  This is the DIFF between positions")
    
    # Recover weights from XOR + one known position
    print(f"\n  If we know pos1=50:")
    print(f"    pos2 = XOR(pos1, xor_result) = {pos1 ^ xor_result}")
    print(f"    weight2 = pos2 = {pos1 ^ xor_result}")
    
    # === Demo: 15 Pairs ===
    print(f"\n[Demo: 15 Pairs (C(6,2))]")
    
    # 6 faces, each with 10×10 grid
    faces = ['A', 'B', 'C', 'D', 'E', 'F']
    
    # Generate weights for each face (100 units per face)
    face_weights = {}
    for f in faces:
        face_weights[f] = [random.randint(-128, 127) for _ in range(100)]
    
    # Encode: position = weight
    face_positions = {}
    for f in faces:
        face_positions[f] = face_weights[f].copy()
    
    # 15 pairs
    pairs = []
    for i in range(len(faces)):
        for j in range(i+1, len(faces)):
            pairs.append((faces[i], faces[j]))
    
    print(f"  {len(pairs)} pairs: {', '.join(f'{a.lower()}{b.lower()}' for a,b in pairs)}")
    print()
    
    # Show first 3 pairs
    for a, b in pairs[:3]:
        # XOR between faces
        xor_vals = [face_positions[a][i] ^ face_positions[b][i] for i in range(10)]
        avg_xor = sum(abs(x) for x in xor_vals) / len(xor_vals)
        print(f"  Pair {a.lower()}{b.lower()}: avg XOR = {avg_xor:.1f}")
    
    # === Summary ===
    print(f"\n{'='*70}")
    print(f"SUMMARY")
    print(f"{'='*70}")
    print(f"  Contour Mask = displacement model")
    print(f"  {UNITS} units (pins)")
    print(f"  Encode: weight → displacement from 0")
    print(f"  Decode: XOR with 0 → weight")
    print()
    print(f"  Key insight: units are NOT static")
    print(f"  Weight values PUSH units ± based on magnitude")
    print(f"  Reading: diff/XOR against 0 = weight")
    print()
    print(f"  = physical model of weight measurement")
    print(f"  = contour gauge / profile gauge")

if __name__ == "__main__":
    test_displacement()
