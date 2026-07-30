"""
mask_6vs12.py — Compare 6-face vs 12-face contour mask
"""

def compare():
    print("=" * 70)
    print("Contour Mask: 6 faces vs 12 faces")
    print("=" * 70)
    
    # === Structure ===
    W, H, L = 10, 10, 10
    CUBES = 10
    
    # 6 faces (cube)
    F6 = 6
    UNITS6 = W * H * L * CUBES * F6
    PAIRS6 = F6 * (F6 - 1) // 2  # C(6,2) = 15
    DIRS6 = F6
    
    # 12 faces (cuboctahedron / dodecahedron)
    F12 = 12
    UNITS12 = W * H * L * CUBES * F12
    PAIRS12 = F12 * (F12 - 1) // 2  # C(12,2) = 66
    DIRS12 = F12
    
    print(f"\n[Structure]")
    print(f"                    6 faces      12 faces     Ratio")
    print(f"  Faces:            {F6:<12} {F12:<12} {F12/F6:.1f}x")
    print(f"  Units:            {UNITS6:,}      {UNITS12:,}      {UNITS12/UNITS6:.1f}x")
    print(f"  Pairs (C(n,2)):   {PAIRS6:<12} {PAIRS12:<12} {PAIRS12/PAIRS6:.1f}x")
    print(f"  Directions:       {DIRS6:<12} {DIRS12:<12} {DIRS12/DIRS6:.1f}x")
    
    # === Storage ===
    print(f"\n[Storage]")
    storage6 = UNITS6 * 1  # int8
    storage12 = UNITS12 * 1
    print(f"  6 faces:  {storage6:,} bytes = {storage6/1024:.1f} KB")
    print(f"  12 faces: {storage12:,} bytes = {storage12/1024:.1f} KB")
    print(f"  Overhead: {storage12/storage6:.1f}x")
    
    # === Observation Power ===
    print(f"\n[Observation Power]")
    print(f"  6 faces:  {PAIRS6} viewpoints")
    print(f"  12 faces: {PAIRS12} viewpoints")
    print(f"  Gain: {PAIRS12/PAIRS6:.1f}x more perspectives")
    
    # === Parallel Reads ===
    print(f"\n[Parallel Reads]")
    reads6 = DIRS6 * W * H
    reads12 = DIRS12 * W * H
    print(f"  6 faces:  {reads6} read heads per cube")
    print(f"  12 faces: {reads12} read heads per cube")
    print(f"  Gain: {reads12/reads6:.1f}x more parallel reads")
    
    # === Displacement Directions ===
    print(f"\n[Displacement Directions]")
    print(f"  6 faces:  ±X, ±Y, ±Z (6 directions)")
    print(f"  12 faces: ±X, ±Y, ±Z, ±XY, ±XZ, ±YZ (12 directions)")
    print()
    print(f"  12-face gives DIAGONAL displacement!")
    print(f"  = weight can push unit in 12 directions, not just 6")
    
    # === Capacity with h-depth ===
    print(f"\n[Capacity with h-depth]")
    SLOTS_LOW = 64
    SLOTS_MID = 128
    SLOTS_HIGH = 256
    
    cap6_low = UNITS6 * SLOTS_LOW
    cap6_high = UNITS6 * SLOTS_HIGH
    cap12_low = UNITS12 * SLOTS_LOW
    cap12_high = UNITS12 * SLOTS_HIGH
    
    print(f"  6 faces, low res:  {cap6_low:,} slots = {cap6_low*8/1024/1024:.1f} Mbit")
    print(f"  6 faces, high res: {cap6_high:,} slots = {cap6_high*8/1024/1024:.1f} Mbit")
    print(f"  12 faces, low res: {cap12_low:,} slots = {cap12_low*8/1024/1024:.1f} Mbit")
    print(f"  12 faces, high res: {cap12_high:,} slots = {cap12_high*8/1024/1024:.1f} Mbit")
    
    # === Summary ===
    print(f"\n{'='*70}")
    print(f"SUMMARY: What 12 faces gives you")
    print(f"{'='*70}")
    print(f"  ✓ 2x more displacement directions (6→12)")
    print(f"  ✓ 4.4x more viewpoint pairs (15→66)")
    print(f"  ✓ Diagonal displacement (XY, XZ, YZ)")
    print(f"  ✓ 2x more parallel reads")
    print(f"  ✓ 2x more storage capacity")
    print(f"  ✓ Finer resolution observation")
    print()
    print(f"  Cost: 2x memory (6KB → 12KB)")
    print(f"  Benefit: 4.4x more viewpoints")
    print(f"  Ratio: 4.4x benefit / 2x cost = 2.2x ROI")

if __name__ == "__main__":
    compare()
