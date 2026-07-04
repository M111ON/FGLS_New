"""
Hierarchical Wedge Addressing System
=====================================
Core idea:
  - Define ONE wedge (A) with a local coordinate equation.
  - Mirror it to get B (same wedge, flipped).
  - Radially repeat (A+B) around center -> covers full disk (1 ring).
  - Stack multiple rings outward -> hierarchical address space.

Any point in the whole structure is addressed by:
  (ring_index, wedge_index, mirror_flag, local_coord)

This replaces brute-force line/intersection search entirely.
No float leaks downstream of vertex generation (same pattern as ngon_intersect.py).
"""

import math
from dataclasses import dataclass

# =========================================================
# CONFIG — edit these
# =========================================================
N_WEDGES        = 24        # number of wedges per ring (360 / N_WEDGES must be integer)
WEDGE_ANGLE_DEG = 360 // N_WEDGES   # must divide evenly -> 15 deg for N=24
N_RINGS         = 6         # number of concentric rings (outward layers)
RING_STEP       = 1000      # radial distance step per ring, integer units
SCALE           = 1000      # base scale for coordinate rounding
POINTS_PER_WEDGE_EDGE = 4   # resolution of local_coord grid inside one wedge (config)
# =========================================================

assert 360 % N_WEDGES == 0, "N_WEDGES must divide 360 evenly"


@dataclass(frozen=True)
class Address:
    ring_index: int      # 0 = innermost ring, N_RINGS-1 = outermost
    wedge_index: int      # 0 .. N_WEDGES-1
    mirror_flag: int      # 0 = A (as-authored), 1 = B (mirrored)
    local_u: int          # local coordinate along wedge radius (integer)
    local_v: int          # local coordinate across wedge angle (integer)

    def key(self):
        """Flatten to a single integer key (compact storage)."""
        return (
            self.ring_index * (N_WEDGES * 2 * POINTS_PER_WEDGE_EDGE * POINTS_PER_WEDGE_EDGE)
            + self.wedge_index * (2 * POINTS_PER_WEDGE_EDGE * POINTS_PER_WEDGE_EDGE)
            + self.mirror_flag * (POINTS_PER_WEDGE_EDGE * POINTS_PER_WEDGE_EDGE)
            + self.local_u * POINTS_PER_WEDGE_EDGE
            + self.local_v
        )


def wedge_A_local_point(u, v, ring_index):
    """
    Define the base wedge (A) local coordinate equation.
    u = radial step index within the wedge (0..POINTS_PER_WEDGE_EDGE-1)
    v = angular step index within the wedge (0..POINTS_PER_WEDGE_EDGE-1)
    ring_index = which ring this wedge belongs to (scales radius outward)

    This is the ONLY place actual geometry math happens.
    Everything else (mirror, radial repeat, ring stacking) is pure
    integer transform of this base definition.
    """
    inner_radius = ring_index * RING_STEP
    outer_radius = (ring_index + 1) * RING_STEP

    # linear interpolation between inner/outer radius, integer-scaled
    radius = inner_radius + (outer_radius - inner_radius) * u // POINTS_PER_WEDGE_EDGE

    # angle within wedge: 0 .. WEDGE_ANGLE_DEG, stepped by v
    local_angle_deg = WEDGE_ANGLE_DEG * v // POINTS_PER_WEDGE_EDGE

    return radius, local_angle_deg


def mirror_point(radius, local_angle_deg):
    """B = mirror of A across the wedge's central axis."""
    mirrored_angle = WEDGE_ANGLE_DEG - local_angle_deg
    return radius, mirrored_angle


def to_global_integer_coord(radius, local_angle_deg, wedge_index, scale=SCALE):
    """
    Convert (radius, local_angle_within_wedge, wedge_index) to a global
    integer (x, y). This is the only place trig happens, and it happens
    once per lookup, not per intersection test -- same principle as the
    N-gon vertex table.
    """
    global_angle_deg = wedge_index * WEDGE_ANGLE_DEG + local_angle_deg
    rad = math.radians(global_angle_deg)
    x = round(radius * math.cos(rad))
    y = round(radius * math.sin(rad))
    return x, y


def build_address(ring_index, wedge_index, mirror_flag, u, v):
    """
    Construct an Address and resolve it to a global integer coordinate.
    This is the main entry point replacing "find intersection" logic.
    """
    assert 0 <= ring_index < N_RINGS
    assert 0 <= wedge_index < N_WEDGES
    assert mirror_flag in (0, 1)
    assert 0 <= u < POINTS_PER_WEDGE_EDGE
    assert 0 <= v < POINTS_PER_WEDGE_EDGE

    radius, angle = wedge_A_local_point(u, v, ring_index)
    if mirror_flag == 1:
        radius, angle = mirror_point(radius, angle)

    addr = Address(ring_index, wedge_index, mirror_flag, u, v)
    coord = to_global_integer_coord(radius, angle, wedge_index)
    return addr, coord


def total_address_space():
    """Total number of addressable points in the whole structure."""
    return N_RINGS * N_WEDGES * 2 * POINTS_PER_WEDGE_EDGE * POINTS_PER_WEDGE_EDGE


def enumerate_all():
    """Generator over every address in the structure -> (Address, key, (x,y))."""
    for ring_index in range(N_RINGS):
        for wedge_index in range(N_WEDGES):
            for mirror_flag in (0, 1):
                for u in range(POINTS_PER_WEDGE_EDGE):
                    for v in range(POINTS_PER_WEDGE_EDGE):
                        addr, coord = build_address(ring_index, wedge_index, mirror_flag, u, v)
                        yield addr, addr.key(), coord


if __name__ == "__main__":
    print(f"N_WEDGES={N_WEDGES} (wedge angle={WEDGE_ANGLE_DEG} deg), N_RINGS={N_RINGS}")
    print(f"Total address space = {total_address_space()} points")

    # sanity: build a few addresses and show resolved coords
    sample = [
        (0, 0, 0, 0, 0),   # innermost ring, wedge 0, A, origin corner
        (0, 0, 1, 0, 0),   # same, but mirrored (B)
        (5, 23, 0, 3, 3),  # outermost ring, last wedge, A, far corner
    ]
    for ring_index, wedge_index, mirror_flag, u, v in sample:
        addr, coord = build_address(ring_index, wedge_index, mirror_flag, u, v)
        print(f"{addr} -> key={addr.key():>8} -> coord={coord}")

    # verify all keys are unique (no collisions in address space)
    keys = [k for _, k, _ in enumerate_all()]
    assert len(keys) == len(set(keys)), "Address key collision detected!"
    print(f"Verified: all {len(keys)} address keys are unique.")
