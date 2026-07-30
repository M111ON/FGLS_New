"""
test_opposite_cancel.py — Test if opposite faces really cancel
Tests: do A↔B, C↔D, E↔F cancel? Or is this just a concern?
"""

import random

def test_opposite_cancel():
    print("=" * 60)
    print("Test: Do opposite faces cancel?")
    print("=" * 60)
    
    # Generate random weights for 6 faces
    random.seed(42)
    N = 100  # samples
    
    # 6 faces: A, B, C, D, E, F
    # Each face has N random weights
    faces = {
        'A': [random.randint(0, 255) for _ in range(N)],
        'B': [random.randint(0, 255) for _ in range(N)],
        'C': [random.randint(0, 255) for _ in range(N)],
        'D': [random.randint(0, 255) for _ in range(N)],
        'E': [random.randint(0, 255) for _ in range(N)],
        'F': [random.randint(0, 255) for _ in range(N)],
    }
    
    print("\n[1] Raw values (first 10 samples):")
    print(f"  {'Sample':<8} {'A':<6} {'B':<6} {'C':<6} {'D':<6} {'E':<6} {'F':<6}")
    print("  " + "-" * 40)
    for i in range(10):
        print(f"  {i:<8} {faces['A'][i]:<6} {faces['B'][i]:<6} {faces['C'][i]:<6} {faces['D'][i]:<6} {faces['E'][i]:<6} {faces['F'][i]:<6}")
    
    # Test 1: Do opposite pairs (A-B, C-D, E-F) have correlation?
    print("\n[2] Opposite Pairs: A↔B, C↔D, E↔F")
    print("  Correlation (should be ~0 if independent):")
    
    pairs_opp = [('A', 'B'), ('C', 'D'), ('E', 'F')]
    for x, y in pairs_opp:
        vals_x = faces[x]
        vals_y = faces[y]
        
        # Pearson correlation
        n = len(vals_x)
        sum_x = sum(vals_x)
        sum_y = sum(vals_y)
        sum_xy = sum(a * b for a, b in zip(vals_x, vals_y))
        sum_x2 = sum(a * a for a in vals_x)
        sum_y2 = sum(b * b for b in vals_y)
        
        num = n * sum_xy - sum_x * sum_y
        den = ((n * sum_x2 - sum_x**2) * (n * sum_y2 - sum_y**2)) ** 0.5
        corr = num / den if den > 0 else 0
        
        # Sum check
        sum_diff = sum(a - b for a, b in zip(vals_x, vals_y))
        sum_ratio = sum(a + b for a, b in zip(vals_x, vals_y))
        
        print(f"  {x}↔{y}: corr={corr:.4f}, sum_diff={sum_diff}, sum_ratio={sum_ratio}")
    
    # Test 2: Non-opposite pairs (ac, ad, af, bc, etc.)
    print("\n[3] Non-Opposite Pairs: ac, ad, af, bc, bd, bf")
    print("  (Should have similar correlation to opposite)")
    
    pairs_non = [('A', 'C'), ('A', 'D'), ('A', 'F'), ('B', 'C'), ('B', 'D'), ('B', 'F')]
    for x, y in pairs_non:
        vals_x = faces[x]
        vals_y = faces[y]
        
        n = len(vals_x)
        sum_x = sum(vals_x)
        sum_y = sum(vals_y)
        sum_xy = sum(a * b for a, b in zip(vals_x, vals_y))
        sum_x2 = sum(a * a for a in vals_x)
        sum_y2 = sum(b * b for b in vals_y)
        
        num = n * sum_xy - sum_x * sum_y
        den = ((n * sum_x2 - sum_x**2) * (n * sum_y2 - sum_y**2)) ** 0.5
        corr = num / den if den > 0 else 0
        
        print(f"  {x.lower()}{y.lower()}: corr={corr:.4f}")
    
    # Test 3: What if we SUBTRACT opposite faces?
    print("\n[4] Subtraction Test (what 'cancel' would mean):")
    print("  If A-B ≈ 0, then they cancel. If A-B ≠ 0, they don't.")
    
    for x, y in pairs_opp:
        diff = [a - b for a, b in zip(faces[x], faces[y])]
        avg_diff = sum(abs(d) for d in diff) / len(diff)
        max_diff = max(abs(d) for d in diff)
        pct_nonzero = sum(1 for d in diff if d != 0) / len(diff) * 100
        
        print(f"  {x}-{y}: avg_abs_diff={avg_diff:.1f}, max={max_diff}, nonzero={pct_nonzero:.1f}%")
    
    # Test 4: What if we ADD non-opposite faces?
    print("\n[5] Addition Test (non-opposite):")
    print("  If A+C preserves information, good. If A+C loses info, bad.")
    
    for x, y in pairs_non[:3]:
        added = [a + b for a, b in zip(faces[x], faces[y])]
        avg_val = sum(added) / len(added)
        max_val = max(added)
        min_val = min(added)
        
        print(f"  {x}+{y}: avg={avg_val:.1f}, min={min_val}, max={max_val}")
    
    # Test 5: Information loss?
    print("\n[6] Information Loss Test:")
    print("  If we keep only A-B (difference), can we recover A and B?")
    
    for x, y in pairs_opp[:1]:
        diff = [a - b for a, b in zip(faces[x], faces[y])]
        
        # Try to recover: if we know diff and one face, can we get the other?
        # A - B = diff → B = A - diff
        recovered_b = [faces[x][i] - diff[i] for i in range(N)]
        exact_match = sum(1 for i in range(N) if recovered_b[i] == faces[y][i])
        
        print(f"  {x}-{y}: recovered B from A-diff: {exact_match}/{N} exact match ({exact_match/N*100:.1f}%)")
    
    # Verdict
    print("\n" + "=" * 60)
    print("VERDICT")
    print("=" * 60)
    
    # Check if any pair has correlation close to 0
    all_corrs = []
    for x, y in pairs_opp + pairs_non:
        vals_x = faces[x]
        vals_y = faces[y]
        n = len(vals_x)
        sum_x = sum(vals_x)
        sum_y = sum(vals_y)
        sum_xy = sum(a * b for a, b in zip(vals_x, vals_y))
        sum_x2 = sum(a * a for a in vals_x)
        sum_y2 = sum(b * b for b in vals_y)
        num = n * sum_xy - sum_x * sum_y
        den = ((n * sum_x2 - sum_x**2) * (n * sum_y2 - sum_y**2)) ** 0.5
        corr = num / den if den > 0 else 0
        all_corrs.append((x, y, corr))
    
    avg_opp = sum(c for _, _, c in all_corrs[:3]) / 3
    avg_non = sum(c for _, _, c in all_corrs[3:]) / 6
    
    print(f"  Avg correlation opposite pairs: {avg_opp:.4f}")
    print(f"  Avg correlation non-opposite:   {avg_non:.4f}")
    print(f"  Both ≈ 0 → faces are INDEPENDENT (random data)")
    print()
    
    if abs(avg_opp) < 0.1 and abs(avg_non) < 0.1:
        print("  ✓ Opposite pairs do NOT cancel — they're independent")
        print("  ✓ You can use ANY pair (ab, ac, ad, etc.)")
        print("  ✓ No special treatment needed for opposite faces")
    else:
        print("  ⚠ Some pairs show correlation — needs investigation")

if __name__ == "__main__":
    test_opposite_cancel()
