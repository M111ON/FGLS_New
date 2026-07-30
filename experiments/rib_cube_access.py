#!/usr/bin/env python3
"""
rib_cube_access.py — Cube weight retrieval from ribcage positions.

Four access strategies for performance comparison across 20,736 rib slots
in a full ring (0..20736).  Designed as a proof-of-concept: signatures clear,
logic dry, protocol flexible.

--------------------------------------------------------------------------------
Protocol (PrinPkg)
--------------------------------------------------------------------------------
  get_weight(rib_id)   → (int, float)   # (slot, value)
  set_weight(rib_id, value) → None      # (slot, value)

Access strategies
  1) linear_step      — rib0 → rib1 → ... sequential scan
  2) angular_rotation — cosine projection: angle → rib ID
  3) random_access    — three sub-modes:
       a) direct_array    — O(1) via precomputed mapping table
       b) cosine_computed — O(1) cos/floor on the fly
       c) rotational_forecast — tide_curve(wide) → rib_extract (staggered)

Performance
  10 reads per approach × 100 repeats, wall-clock timing.
"""

from __future__ import annotations

import math
import random
import time
from dataclasses import dataclass, field
from typing import Callable, List, Tuple

# ---------------------------------------------------------------------------
# Constants — full ring geometry
# ---------------------------------------------------------------------------
FULL_RING: int = 20736          # 0 .. 20735
RIBS: int = 12                  # ribcage ribs
CUBES_PER_RIB: int = FULL_RING // RIBS   # 1728 per rib
TAU: float = 2.0 * math.pi

# ---------------------------------------------------------------------------
# Core data store — dict keyed by rib_id, each rib holds CUBES_PER_RIB entries
# ---------------------------------------------------------------------------


@dataclass
class RibCubeStore:
    """Sparse dict-backed store. Each rib_id maps to a list of float weights."""

    data: dict[int, list[float]] = field(default_factory=dict)
    cubes_per_rib: int = CUBES_PER_RIB

    def ensure_rib(self, rib_id: int) -> None:
        if rib_id not in self.data:
            self.data[rib_id] = [0.0] * self.cubes_per_rib

    # -- PrinPkg: get_weight / set_weight ------------------------------------

    def get_weight(self, rib_id: int, cube_idx: int = 0) -> float:
        self.ensure_rib(rib_id)
        return self.data[rib_id][cube_idx % self.cubes_per_rib]

    def set_weight(self, rib_id: int, weight: float, cube_idx: int = 0) -> None:
        self.ensure_rib(rib_id)
        self.data[rib_id][cube_idx % self.cubes_per_rib] = weight

    def populate_random(self, seed: int = 42) -> None:
        rng = random.Random(seed)
        for rib in range(RIBS):
            self.data[rib] = [rng.uniform(0.0, 100.0) for _ in range(self.cubes_per_rib)]


# Default global store — all strategies read from / write to this same instance.
store = RibCubeStore()
store.populate_random()

# ---------------------------------------------------------------------------
# 1. Linear step access
#    rib0 → rib1 → ... sequential pass-by
# ---------------------------------------------------------------------------


def linear_step_reads(n: int) -> List[float]:
    """
    Step through ribs 0..(n-1) in strict linear order.
    Returns list of retrieved weights.
    """
    results: List[float] = []
    for rib in range(n):
        results.append(store.get_weight(rib))
    return results


# ---------------------------------------------------------------------------
# 2. Angular rotation access
#    large rotation projection → rib_id via angular allocation
# ---------------------------------------------------------------------------


def angular_rotation_read(angles: List[float]) -> List[float]:
    """
    Map each angle (radians) → rib_id via angular slice allocation.
    Each rib covers TAU/RIBS radians.
    """
    slice_width = TAU / RIBS
    results: List[float] = []
    for angle in angles:
        rib_id = int((angle % TAU) // slice_width)
        results.append(store.get_weight(rib_id))
    return results


# ---------------------------------------------------------------------------
# 3. Random access — three sub-strategies
#    Rib ID mapping: cruise F(time, rib_id) through sphere
# ---------------------------------------------------------------------------

# 3a. direct_array — O(1) precomputed lookup table
# ---------------------------------------------------
# Build once; the mapping table answers time/cube → rib_id without computation.


def _build_direct_map(ring: int = FULL_RING, ribs: int = RIBS) -> List[int]:
    """Precompute: every full-ring slot → which rib it lands in."""
    per: int = ring // ribs
    return [i // per for i in range(ring)]


_direct_map: List[int] = _build_direct_map()


def direct_array_read(indices: List[int]) -> List[float]:
    """O(1) lookup: index into the precomputed map, then read store."""
    return [store.get_weight(_direct_map[i % FULL_RING]) for i in indices]


# 3b. 360° cosine compute — O(1) on the fly
# -----------------------------------------------------------


def cos_360_to_rib(angle_deg: float) -> int:
    """
    Map 360-degree angle to rib_id via cosine projection.
    Normalise to [0,1], scale by RIBS.
    """
    val = (math.cos(math.radians(angle_deg % 360.0)) + 1.0) * 0.5  # [0, 1]
    return int(val * (RIBS - 0.5))  # clip to 0..RIBS-1


def cosine_computed_access(angles_deg: List[float]) -> List[float]:
    """Compute rib_id on the fly via cos projection, then read store."""
    return [store.get_weight(cos_360_to_rib(a)) for a in angles_deg]


# 3c. Rotational forecast — tide_curve(wide) pipe
# ---------------------------------------------------------


def tide_curve(t: float, amplitude: float = 1.0, period: float = 360.0) -> float:
    """
    Tide curve: sin wave mapped 0..1.
    t in [0..FULL_RING), maps to rib_extract.
    """
    return (math.sin(2.0 * math.pi * t / period) + 1.0) * 0.5


def subpair_recursive(rib_id: int, depth: int = 2) -> List[int]:
    """
    rib_id → subpair recursive: parent pipe mode → list of actual rib positions.

    Each level yields siblings: the rib itself ± a perturbation that narrows
    with depth (stagger spread).
    """
    spread = max(1, RIBS // (2 ** (depth + 1)))
    siblings: List[int] = [rib_id]
    for offset in range(-spread, spread + 1):
        cand = (rib_id + offset) % RIBS
        if cand not in siblings:
            siblings.append(cand)
    # Recursive deepen
    if depth > 1:
        deeper: List[int] = []
        for sib in siblings:
            deeper.extend(subpair_recursive(sib, depth - 1))
        siblings = list(dict.fromkeys(deeper))  # dedup preserve order
    return siblings


def rotational_forecast_access(rib_ids: List[int], depth: int = 2) -> List[Tuple[int, float]]:
    """
    tide_curve(wide) → rib_extract → subpair_recursive → store reads.

    Returns flat list of (rib_slot, weight) — each original rib yields
    a staggered fan-out, like a tidal spread through the sphere.
    """
    results: List[Tuple[int, float]] = []
    for rib in rib_ids:
        families = subpair_recursive(rib, depth)
        for f in families:
            results.append((f, store.get_weight(f)))
    return results


# ---------------------------------------------------------------------------
# 4. Performance harness
# ---------------------------------------------------------------------------

ACCESS_SAMPLES: int = 10       # reads per approach
REPEATS: int = 100             # how many times each run is repeated


def _timeit(label: str, fn: Callable[[], object]) -> Tuple[str, float]:
    t0 = time.perf_counter()
    fn()
    elapsed = time.perf_counter() - t0
    return label, elapsed


def run_benchmark() -> None:
    # Prefabricate inputs
    linear_n = min(ACCESS_SAMPLES, RIBS)
    angles_rad = [random.uniform(0, TAU) for _ in range(ACCESS_SAMPLES)]
    angles_deg = [random.uniform(0, 360) for _ in range(ACCESS_SAMPLES)]
    random_indices = [random.randint(0, FULL_RING - 1) for _ in range(ACCESS_SAMPLES)]
    random_ribs = [random.randint(0, RIBS - 1) for _ in range(ACCESS_SAMPLES)]

    strategies: List[Tuple[str, Callable[[], object]]] = [
        ("linear_step", lambda: linear_step_reads(linear_n)),
        ("angular_rotation", lambda: angular_rotation_read(angles_rad)),
        ("direct_array", lambda: direct_array_read(random_indices)),
        ("cosine_computed", lambda: cosine_computed_access(angles_deg)),
        ("rotational_forecast", lambda: rotational_forecast_access(random_ribs)),
    ]

    print("=" * 68)
    print("rib_cube_access — strategy performance (10 reads × 100 reps)")
    print("=" * 68)
    print(f"  FULL_RING={FULL_RING}  RIBS={RIBS}  CUBES_PER_RIB={CUBES_PER_RIB}")
    print()

    totals: dict[str, float] = {}

    for name, fn in strategies:
        times: List[float] = []
        for _ in range(REPEATS):
            label_n, dt = _timeit(name, fn)
            times.append(dt)
            totals[name] = sum(times)

    for name, total in sorted(totals.items(), key=lambda x: x[1]):
        per_read = (total / REPEATS) / ACCESS_SAMPLES
        print(f"  {name:<22s}  total={total:8.4f}s   per_read={per_read*1e6:7.1f} µs")

    print()
    best = min(totals, key=totals.get)
    print(f"  Fastest: {best}")
    print("=" * 68)


# ---------------------------------------------------------------------------
# Interactive / direct-run entry points
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    # Quick sanity
    print("Sanity check — one read from each strategy...")
    s = RibCubeStore()
    s.set_weight(0, 42.0)

    print(f"  linear_step[0]              = {linear_step_reads(1)[0]}")
    print(f"  angular_rotation[0]         = {angular_rotation_read([0.0])[0]}")
    print(f"  direct_array[0]             = {direct_array_read([0])[0]}")
    print(f"  cosine_computed[0]          = {cosine_computed_access([0.0])[0]}")
    print(f"  rotational_forecast[0,:]    = {rotational_forecast_access([0], depth=1)[:3]}")
    print()

    run_benchmark()