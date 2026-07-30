"""
contour_mask_capacity.py — Capacity analysis for contour mask structure
"""

def analyze_capacity():
    print("=" * 70)
    print("Contour Mask — Capacity Analysis")
    print("=" * 70)
    
    # Current structure
    W = 10  # width (face grid)
    H = 10  # height (face grid)
    L = 10  # depth (layers)
    CUBES = 10  # number of cubes
    FACES = 6   # faces per cube
    
    # Current capacity
    UNITS = W * H * L * CUBES * FACES
    BITS_PER_UNIT = 8  # int8
    TOTAL_BITS = UNITS * BITS_PER_UNIT
    
    print(f"\n[Current Structure]")
    print(f"  W × H × L × cubes × faces = {W}×{H}×{L}×{CUBES}×{FACES}")
    print(f"  = {UNITS} units")
    print(f"  × {BITS_PER_UNIT} bits/unit = {TOTAL_BITS} bits")
    print(f"  = {TOTAL_BITS/10:.0f} digits")
    
    # Target: 10 digits
    TARGET_DIGITS = 10
    TARGET_BITS = TARGET_DIGITS * 10  # 10 digits = 100 bits
    
    print(f"\n[Target: {TARGET_DIGITS} digits]")
    print(f"  Need: {TARGET_BITS} bits")
    print(f"  Have: {TOTAL_BITS} bits")
    print(f"  Ratio: {TARGET_BITS/TOTAL_BITS:.1f}x needed")
    
    # Scale factors
    print(f"\n[Scaling Options]")
    
    # Option 1: Scale W×H (face grid)
    scale_face = (TARGET_BITS / TOTAL_BITS) ** 0.5
    print(f"  1. Scale face grid: {W}→{W*scale_face:.0f}, {H}→{H*scale_face:.0f}")
    print(f"     New units: {W*scale_face:.0f}×{H*scale_face:.0f}×{L}×{CUBES}×{FACES}")
    
    # Option 2: Scale L (depth)
    scale_L = TARGET_BITS / TOTAL_BITS
    print(f"  2. Scale depth: {L}→{L*scale_L:.0f}")
    print(f"     New units: {W}×{H}×{L*scale_L:.0f}×{CUBES}×{FACES}")
    
    # Option 3: Scale cubes
    scale_cubes = TARGET_BITS / TOTAL_BITS
    print(f"  3. Scale cubes: {CUBES}→{CUBES*scale_cubes:.0f}")
    print(f"     New units: {W}×{H}×{L}×{CUBES*scale_cubes:.0f}×{FACES}")
    
    # Option 4: Scale bits per unit
    scale_bits = TARGET_BITS / UNITS
    print(f"  4. Scale bits/unit: {BITS_PER_UNIT}→{scale_bits:.0f}")
    print(f"     Need: {scale_bits:.0f}-bit storage per unit")
    
    # Option 5: Combined scaling
    combined = (TARGET_BITS / TOTAL_BITS) ** 0.25
    print(f"  5. Combined: W×H×L×cubes each ×{combined:.2f}")
    print(f"     New: {W*combined:.0f}×{H*combined:.0f}×{L*combined:.0f}×{CUBES*combined:.0f}×{FACES}")
    
    # Practical recommendation
    print(f"\n[Practical Recommendation]")
    print(f"  For {TARGET_BITS} bits ({TARGET_DIGITS} digits):")
    print(f"  → Scale W×H (face grid) by ~{scale_face:.1f}x each")
    print(f"  → Keep L×cubes×faces constant")
    print(f"  → New face grid: {W*scale_face:.0f}×{H*scale_face:.0f}")
    print(f"  → Units: {UNITS*scale_face*scale_face:.0f}")
    print(f"  → Storage: {UNITS*scale_face*scale_face*BITS_PER_UNIT/1024:.1f} KB")
    
    # Verify
    new_units = W*scale_face * H*scale_face * L * CUBES * FACES
    new_bits = new_units * BITS_PER_UNIT
    print(f"\n  Verification: {new_units:.0f} units × {BITS_PER_UNIT} bits = {new_bits:.0f} bits")
    print(f"  = {new_bits/10:.0f} digits ✓")

if __name__ == "__main__":
    analyze_capacity()
