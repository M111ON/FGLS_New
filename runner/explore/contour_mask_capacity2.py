"""
contour_mask_capacity2.py — Correct capacity analysis
"""

def analyze():
    print("=" * 70)
    print("Contour Mask — Capacity (Corrected)")
    print("=" * 70)
    
    # Current
    W, H, L = 10, 10, 10
    CUBES = 10
    FACES = 6
    UNITS = W * H * L * CUBES * FACES
    
    print(f"\n[Current Structure]")
    print(f"  W×H×L×cubes×faces = {W}×{H}×{L}×{CUBES}×{FACES} = {UNITS:,} units")
    print(f"  Each unit: int8 = 256 values = 3 digits")
    print(f"  Total unique states: 256^{UNITS:,}")
    
    # What does "10 digits" mean?
    print(f"\n[What is '10 digits'?]")
    print(f"  10 digits = 10^10 = 10,000,000,000 values")
    print(f"  Log2(10^10) = 33.22 bits")
    print(f"  = 5 int8 units needed")
    print()
    print(f"  Current: {UNITS:,} units × 8 bits = {UNITS*8:,} bits")
    print(f"  = {UNITS*8/3.3219:.0f} digits")
    print(f"  >>> Way more than 10 digits!")
    
    # The real question: weight precision
    print(f"\n[Weight Precision]")
    print(f"  Current: int8 = [-128, 127] = 256 values = 3 digits")
    print(f"  If weights need more precision:")
    print()
    print(f"  4 digits: int16 = [-32768, 32767] = 65536 values")
    print(f"  5 digits: int32 = [-2B, 2B] = 4B values")
    print(f"  6 digits: int64 = [-9E18, 9E18]")
    
    # Scaling options
    print(f"\n[Scaling for More Precision]")
    print(f"  To go from int8 (3 digits) to int16 (4 digits):")
    print(f"  Option A: 2× units (double W×H×L×cubes)")
    print(f"  Option B: 2× bits/unit (int16 instead of int8)")
    print(f"  Option C: Scale W.H.L.n")
    
    # W.H.L.n scaling
    print(f"\n[W.H.L.n Scaling]")
    print(f"  W = face width (grid columns)")
    print(f"  H = face height (grid rows)")
    print(f"  L = depth (layers)")
    print(f"  n = number of cubes")
    print()
    print(f"  Total units = W × H × L × n × 6 (faces)")
    print(f"  Precision = log2(W × H × L × n × 6 × 256) bits")
    
    # Example
    print(f"\n[Example: 30B model weights]")
    print(f"  30B weights × 8 bits = 240B bits")
    print(f"  Current capacity: {UNITS*8:,} bits = {UNITS*8/1024/1024/1024:.2f} GB")
    print(f"  Need: 240 GB")
    print(f"  Scale factor: {240/(UNITS*8/1024/1024/1024):.0f}x")
    print()
    print(f"  Options:")
    print(f"  1. W×H: 10→77 each (77×77 face)")
    print(f"  2. L: 10→6000 (6000 layers)")
    print(f"  3. n: 10→6000 (6000 cubes)")
    print(f"  4. Combined: W×H×L×n each ×8.7")
    
    # Verify
    scale = (240 / (UNITS*8/1024/1024/1024)) ** 0.25
    print(f"\n  Combined scaling: each dimension ×{scale:.1f}")
    print(f"  New: {W*scale:.0f}×{H*scale:.0f}×{L*scale:.0f}×{CUBES*scale:.0f}")
    new_units = W*scale * H*scale * L*scale * CUBES*scale * FACES
    print(f"  Units: {new_units:,.0f}")
    print(f"  Storage: {new_units*8/1024/1024/1024:.1f} GB")

if __name__ == "__main__":
    analyze()
