"""
demo_lblock.py — Demonstrate L-block: deterministic rotation from Hilbert position

L-block is a 4-cell shape that:
1. "Summons" at any Hilbert address (position on 8×8 grid)
2. Unfolds into a deterministic rotation (0°, 90°, 180°, 270°)
3. Aligns with the grid immediately

The rotation is determined by the Hilbert curve's traversal direction at that position.
Same position → same direction → same rotation → deterministic.
"""

import sys
sys.path.insert(0, '.')
import importlib.util
spec = importlib.util.spec_from_file_location('gfs', 'geo_frame_seek.py')
gfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gfs)

def print_grid_with_lblocks(n=8):
    """Print 8×8 grid showing L-blocks for each position."""
    # Build grid
    grid = [[ '.' for _ in range(n)] for _ in range(n)]

    # Place L-blocks for each Hilbert position
    for d in range(n * n):
        cells, rot, (dx, dy) = gfs.lblock_from_hilbert(d, n)
        # Mark cells with rotation number
        for cx, cy in cells:
            if 0 <= cx < n and 0 <= cy < n:
                grid[cy][cx] = str(rot)

    # Print grid
    print("  Grid (rotation labels):")
    print("    " + " ".join(str(i) for i in range(n)))
    for y in range(n):
        print(f"  {y} | " + " ".join(grid[y]))

def print_lblock_ascii(rot):
    """Print L-block shape for given rotation."""
    shapes = {
        0: ["XXX", "  X"],     # right
        1: ["X ", "X ", "XX"], # down
        2: ["X  ", "XXX"],     # left
        3: ["XX", " X", " X"], # up
    }
    return shapes.get(rot, ["??", "??"])

print("=" * 60)
print("L-BLOCK DEMO: Deterministic Rotation from Hilbert Position")
print("=" * 60)

print("\n1. L-block shapes (4 rotations):")
for rot in range(4):
    shape_lines = print_lblock_ascii(rot)
    print(f"\n  Rotation {rot} ({['right','down','left','up'][rot]}):")
    for line in shape_lines:
        print(f"    {line}")

print("\n2. 8×8 grid with L-blocks (rotation labels):")
print_grid_with_lblocks(8)

print("\n3. Sample L-blocks from Hilbert positions:")
print("  d  | dir    | rot | cells")
print("  ---|--------|-----|------")
for d in [0, 1, 5, 10, 21, 42, 63]:
    cells, rot, (dx, dy) = gfs.lblock_from_hilbert(d, 8)
    cells_str = " ".join(f"({x},{y})" for x, y in cells)
    dir_str = f"({dx:+d},{dy:+d})"
    print(f"  {d:2d} | {dir_str:6s} |  {rot}  | {cells_str}")

print("\n4. Rotation distribution across 64 positions:")
rot_count = [0, 0, 0, 0]
for d in range(64):
    _, rot, _ = gfs.lblock_from_hilbert(d, 8)
    rot_count[rot] += 1
total = sum(rot_count)
for i, count in enumerate(rot_count):
    pct = count / total * 100
    bar = '#' * int(pct / 2)
    print(f"  rot {i}: {count:2d} ({pct:5.1f}%) {bar}")

print("\n5. Deterministic verification:")
print("  Same position always gives same rotation:")
for d in [0, 10, 30, 50]:
    _, rot1, _ = gfs.lblock_from_hilbert(d, 8)
    _, rot2, _ = gfs.lblock_from_hilbert(d, 8)
    print(f"  d={d:2d}: rot={rot1} == rot={rot2} → {'PASS' if rot1 == rot2 else 'FAIL'}")

print("\n" + "=" * 60)
print("KEY INSIGHT: Hilbert position → direction → rotation → L-block on grid")
print("Same Hilbert address → same rotation → deterministic alignment")
print("=" * 60)
