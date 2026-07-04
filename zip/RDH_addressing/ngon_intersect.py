"""
N-gon Line Intersection Address System
Integer-only, config-driven geometry rule engine.
No float, no sin/cos — vertex positions come from precomputed integer lookup.
"""

from itertools import combinations
from math import comb

# =========================================================
# CONFIG — edit these
# =========================================================
N              = 144      # number of vertices on the polygon (must divide 360 cleanly if angle-mapped)
SCALE          = 1000      # integer scale for vertex coordinates (avoids float)
ALLOW_CONCURRENCY = False  # if True, allow 3+ lines to share one intersection point (many-to-1 mapping)
FULL_CONNECT   = True       # True = connect every pair of vertices (C(N,2) lines)
# =========================================================


def gen_vertices(n, scale):
    """
    Integer vertex positions on an n-gon using precomputed unit circle table.
    Only computed once (sin/cos happens here, nowhere else downstream).
    """
    import math
    pts = []
    for i in range(n):
        angle = 2 * math.pi * i / n
        x = round(math.cos(angle) * scale)
        y = round(math.sin(angle) * scale)
        pts.append((x, y))
    return pts


def gen_lines(n, full_connect=True):
    """Return list of vertex-index pairs representing lines."""
    if full_connect:
        return list(combinations(range(n), 2))
    else:
        # default: connect only immediate + skip-1 neighbors (polygon + star)
        lines = [(i, (i + 1) % n) for i in range(n)]
        lines += [(i, (i + 2) % n) for i in range(n)]
        return lines


def cross(o, a, b):
    """Integer cross product (b-o) x (a-o)."""
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])


def sign(v):
    return (v > 0) - (v < 0)


def segments_intersect(p1, p2, p3, p4):
    """Integer-only orientation test. Returns True if segment p1p2 crosses p3p4."""
    d1 = sign(cross(p3, p4, p1))
    d2 = sign(cross(p3, p4, p2))
    d3 = sign(cross(p1, p2, p3))
    d4 = sign(cross(p1, p2, p4))
    return d1 != d2 and d3 != d4


def theoretical_intersection_count(n):
    """
    Max interior intersection points if no 3 lines are concurrent.
    Every set of 4 vertices produces exactly 1 interior intersection
    (from its two diagonals).
    """
    return comb(n, 4)


def find_intersections(vertices, lines):
    """
    Brute-force O(L^2) intersection finder.
    Returns dict: intersection_key -> list of (line_a, line_b) that meet there
    (only non-concurrent geometric detection; concurrency grouping is a
    separate post-pass since exact point coords would need rational arithmetic
    to detect true concurrency without float).
    """
    results = []
    for (a, b) in combinations(lines, 2):
        p1, p2 = vertices[a[0]], vertices[a[1]]
        p3, p4 = vertices[b[0]], vertices[b[1]]
        # skip lines sharing a vertex (not an interior crossing)
        if len(set(a) & set(b)) > 0:
            continue
        if segments_intersect(p1, p2, p3, p4):
            results.append((a, b))
    return results


def intersection_to_tuple(line_a, line_b):
    """
    Map a single intersection (from two lines) back to its unique 4-tuple
    of originating vertices. This is the compressed address key.
    """
    return tuple(sorted(set(line_a) | set(line_b)))


if __name__ == "__main__":
    verts = gen_vertices(N, SCALE)
    lines = gen_lines(N, FULL_CONNECT)

    print(f"N = {N}")
    print(f"Lines generated: {len(lines)}")
    print(f"Theoretical max interior intersections C(N,4) = {theoretical_intersection_count(N)}")

    # NOTE: brute force below is O(L^2) — fine for small N, but for N=144 with
    # full connect (10,296 lines) this is ~53M line pairs. Only run for small N
    # unless you swap in a sweep-line algorithm.
    if len(lines) <= 200:
        hits = find_intersections(verts, lines)
        print(f"Actual detected intersections: {len(hits)}")

        tuples = [intersection_to_tuple(a, b) for a, b in hits]
        unique_tuples = set(tuples)
        print(f"Unique 4-tuples (address keys): {len(unique_tuples)}")

        if not ALLOW_CONCURRENCY:
            assert len(tuples) == len(unique_tuples), "Concurrency detected but ALLOW_CONCURRENCY=False"
    else:
        print("Skipping brute-force pairwise scan (too large for O(L^2) at this N).")
        print("Use theoretical count above, or implement sweep-line for exact scan.")
