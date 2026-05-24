"""
fibo_clock.py — 1440-tick fibo clock backbone.
Everything in the system ties to this clock: address, codebook, traverse.

Clock: tick 0..1439, cycle auto-increments on wrap.
Codebook: 240 codes (1 code per 6 ticks).
Address = (cycle, tick) — continuous sequence encoding.
"""

import torch

TICKS_PER_CYCLE = 1440
N_CODES = 240  # 1 code per 6 ticks

class FiboClock:
    def __init__(self, tick=0, cycle=0):
        self.tick = tick
        self.cycle = cycle

    def advance(self, step: int):
        """Advance by step ticks. Returns new (tick, cycle)."""
        new_tick = self.tick + step
        cycle_add = new_tick // TICKS_PER_CYCLE
        new_tick = new_tick % TICKS_PER_CYCLE
        if cycle_add > 0:
            self.cycle += cycle_add
        self.tick = new_tick
        return self.tick, self.cycle

    def rebase(self, tick: int, cycle: int = 0):
        self.tick = tick % TICKS_PER_CYCLE
        self.cycle = cycle + (tick // TICKS_PER_CYCLE)

    def clone(self):
        return FiboClock(self.tick, self.cycle)

    def __repr__(self):
        return f"FiboClock(tick={self.tick}, cycle={self.cycle})"


# ── Index ↔ Tick conversion ──────────────────────────────
# 37 is coprime to 1440 → full-cycle permutation generator
# tick = (idx * 37) % 1440
# idx  = (tick * 973) % 1440   (37*973 = 36001 ≡ 1 mod 1440)
_COPRIME = 37
_INV = pow(37, -1, TICKS_PER_CYCLE)  # 973

def idx_to_tick(idx: torch.Tensor) -> torch.Tensor:
    """Convert codebook index → fibo tick position."""
    return (idx * _COPRIME) % TICKS_PER_CYCLE

def tick_to_idx(tick: torch.Tensor) -> torch.Tensor:
    """Convert fibo tick position → codebook index."""
    return (tick * _INV) % TICKS_PER_CYCLE

# ── Cycle counter — continuous sequence encoding ──────────
# Address = cycle * TICKS_PER_CYCLE + tick
# Phase = golden-ratio hash of cycle → deterministic permutation
_PHI32 = 2654435761  # golden ratio for 32-bit (Knuth multiplicative hash)

def cycle_phase(cycle: torch.Tensor, n_codes: int = N_CODES) -> torch.Tensor:
    """Deterministic phase offset for cycle counter.
    Maps cycle to a pseudo-random offset in [0, n_codes).
    Golden-ratio multiplication gives maximal spread with no collisions.
    """
    return (cycle * _PHI32) % n_codes

def encode_addr(cycle: int, tick: int) -> int:
    """Combine cycle + tick into a single continuous address."""
    return cycle * TICKS_PER_CYCLE + tick

def decode_addr(addr: int):
    """Split address into (cycle, tick)."""
    return addr // TICKS_PER_CYCLE, addr % TICKS_PER_CYCLE


# ── Zone system for 1440-tick clock ──────────────────────
# 24 zones × 60 ticks each = 1440
# Zone pairs: (0,12), (1,13), ..., (11,23) — self-inverse
TICKS_PER_ZONE = 60
N_ZONES = TICKS_PER_CYCLE // TICKS_PER_ZONE  # 24

def tick_to_zone(tick: torch.Tensor) -> torch.Tensor:
    return tick // TICKS_PER_ZONE

def zone_to_tick(zone: int) -> int:
    return zone * TICKS_PER_ZONE

# Self-inverse: CROSS_LUT[CROSS_LUT[z]] == z
CROSS_LUT = torch.tensor([(i + 12) % 24 for i in range(24)], dtype=torch.long)


def build_geo_table(n_codes: int = TICKS_PER_CYCLE) -> torch.Tensor:
    """Build 8-dim geo table for 1440-tick clock.
    Each entry: [zone/24, pair_id/12, pole, tick/1440,
                 frame/60, cell/10, partner/24, ((tick*37)%1440)/1440]
    """
    table = []
    for i in range(n_codes):
        tick = idx_to_tick(torch.tensor(i)).item()
        zone = tick // TICKS_PER_ZONE
        pair_id = zone % 12
        pole = 0.0 if zone < 12 else 1.0
        partner = (zone + 12) % 24
        frame = tick % TICKS_PER_ZONE
        cell = tick % 10
        table.append([
            zone / 24.0,
            pair_id / 12.0,
            pole,
            tick / float(TICKS_PER_CYCLE),
            frame / float(TICKS_PER_ZONE),
            cell / 10.0,
            partner / 24.0,
            ((tick * _COPRIME) % TICKS_PER_CYCLE) / float(TICKS_PER_CYCLE),
        ])
    return torch.tensor(table, dtype=torch.float32)
