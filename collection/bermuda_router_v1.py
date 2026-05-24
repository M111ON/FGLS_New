"""
bermuda_router_v1.py — Float Router via Bermuda Geometry
========================================================
Float tensor → Bermuda gate → geometry idx → traverse → routing verdict

Maps to POGLS pipeline:
  idx_zone/pole → ROUTE (FGLS) / GROUND (LC-GCFS)
  traverse mode → shape (I/O/T/S/Z/L) compatible with tgw_bond_dispatch.h
  tring_slot     → 720-slot TRing position

Usage:
  from bermuda_router_v1 import BermudaRouter
  router = BermudaRouter(dim=128, gear=2)
  verdict = router.route(float_tensor, mode=0)  # ORBITAL
"""

import torch
import torch.nn.functional as F
from dataclasses import dataclass, field
from typing import List, Optional
from bermuda_reshape_v3 import (
    BermudaGate, GeoCodebook, GEAR_TABLE, GEAR_INV, GEAR_INV_WL,
    snap_gear, pad_to_gear, unpad,
    hilbert_encode, hilbert_decode,
    hilbert_scatter, hilbert_gather,
    strip, reattach, BermudaSnapshot,
    STRIDE, WALK_LEN, N_ZONES, CROSS_LUT, DEVICE,
    _gear_walk_len, build_geo_table,
)


# ── Routing constants (mirrors tgw_bond_dispatch.h) ──────────
TRING_SLOTS     = 720
POLARITY_ROUTE  = 0
POLARITY_GROUND = 1


@dataclass
class RoutingVerdict:
    """Result of bermuda routing for one float tensor"""
    idx_in      : torch.Tensor   # [N] codebook indices before traverse
    idx_out     : torch.Tensor   # [N] after traverse
    mode        : int            # 0=ORBITAL 1=CHIRAL 2=CROSS 3=HUB
    shape       : torch.Tensor   # [N] shape byte (I/O/T/S/Z/L) per token
    polarity    : torch.Tensor   # [N] 0=ROUTE 1=GROUND
    tring_slot  : torch.Tensor   # [N] 0-719 slot position
    zone        : torch.Tensor   # [N] 0-11 zone
    pole        : torch.Tensor   # [N] 0=south 1=north
    pair        : torch.Tensor   # [N] 0-5 pair id
    partner_zone: torch.Tensor   # [N] partner zone from CROSS_LUT
    real_mask   : torch.Tensor   # [N] True = real token, False = padding
    gear        : int
    pad_len     : int
    n_tokens    : int
    shadow_norm : float          # residual magnitude after strip

    def summary(self) -> str:
        n = self.n_tokens
        real  = self.real_mask
        r_n   = real.sum().item()
        route_n  = (self.polarity[real] == POLARITY_ROUTE).sum().item()
        ground_n = (self.polarity[real] == POLARITY_GROUND).sum().item()
        shapes = ""
        for s, label in [(73, 'I'), (79, 'O'), (84, 'T'), (83, 'S'), (90, 'Z'), (76, 'L')]:
            c = (self.shape[real] == s).sum().item()
            if c > 0:
                shapes += f"{label}={c} "
        return (f"  real={r_n} padded={self.pad_len} gear={self.gear} "
                f"ROUTE={route_n} GROUND={ground_n} "
                f"shadow={self.shadow_norm:.2f} "
                f"shapes: {shapes.strip()}")


# shape byte constants (ASCII, matches pogls_bond.h)
SHAPE_I = 73  # 'I' — ROUTE linear
SHAPE_O = 79  # 'O' — ROUTE latch
SHAPE_T = 84  # 'T' — ROUTE fanout
SHAPE_S = 83  # 'S' — GROUND cross-swap
SHAPE_Z = 90  # 'Z' — GROUND reverse-swap
SHAPE_L = 76  # 'L' — GROUND fork-left


class BermudaRouter:
    """
    Float → geometry idx → routing verdict
    
    Architecture:
      Float [B,S,D]
        → snap_gear → pad → Hilbert scatter → strip
        → gate.encode_tokens() → idx (geometry address)
        → traverse(mode) → idx_out
        → classify() → shape, polarity, tring_slot
        → RoutingVerdict
    
    Integration:
      verdict.polarity  → TGW dispatch ROUTE/GROUND
      verdict.shape     → matches pogls_bond.h shape bytes
      verdict.tring_slot→ 720-slot TRing position
      verdict.idx_out   → store in codebook for decode
    """

    def __init__(self, dim: int = 128, gear: int = 2, code_dim: int = 64):
        self.dim   = dim
        self.gear  = gear
        self.gate  = BermudaGate(dim, code_dim=code_dim, gear=gear).to(DEVICE)
        self._gate_cache = {gear: self.gate}
        self.slots = GEAR_TABLE[gear]['slots']
        self.walk_len = GEAR_TABLE[gear]['walk_len']
        self.face_sz  = self.walk_len // 12

    def _get_gate(self, gear: int) -> BermudaGate:
        gate = self._gate_cache.get(gear)
        if gate is None:
            gate = BermudaGate(self.dim, code_dim=self.gate.codebook.code_dim, gear=gear).to(DEVICE)
            self._gate_cache[gear] = gate
        return gate

    # ── Classification: idx → verdict ────────────────────────
    def classify_idx(self, idx: torch.Tensor, mode: int, gear: int | None = None) -> RoutingVerdict:
        """
        Single classification: given codebook indices, produce routing verdict.
        No forward pass through gate (indices already known).
        """
        N = idx.shape[0]
        device = idx.device
        gear = self.gear if gear is None else gear
        gate = self._get_gate(gear)
        walk_len = GEAR_TABLE[gear]['walk_len']
        face_sz = walk_len // 12

        # decode geometry from idx
        encs   = (idx * STRIDE) % walk_len
        zones  = encs // face_sz
        pairs  = torch.tensor([0,1,2,3,4,5,0,1,2,3,4,5], device=device)[zones % 12]
        poles  = torch.tensor([0,0,0,0,0,0,1,1,1,1,1,1], device=device)[zones % 12]
        partners = CROSS_LUT.to(device)[zones % 12]

        # apply traverse
        idx_out = gate.codebook.traverse(idx, mode)

        # shape + polarity per mode (mirrors tgw_bond_dispatch.h semantics)
        if mode == 0:  # ORBITAL — local step → ROUTE forward
            shape    = torch.where(poles == 0, SHAPE_I, SHAPE_O)
            polarity = poles.clone()  # pole=0 ROUTE, pole=1 GROUND
        elif mode == 1:  # CHIRAL — mirror/backup → always GROUND
            shape    = torch.full((N,), SHAPE_O, dtype=torch.long, device=device)
            polarity = torch.ones(N, dtype=torch.long, device=device)
        elif mode == 2:  # CROSS — cross-face routing → always ROUTE
            shape    = torch.full((N,), SHAPE_S, dtype=torch.long, device=device)
            polarity = torch.zeros(N, dtype=torch.long, device=device)
        elif mode == 3:  # HUB — collapse to anchor → always GROUND
            shape    = torch.full((N,), SHAPE_L, dtype=torch.long, device=device)
            polarity = torch.ones(N, dtype=torch.long, device=device)

        # tring slot: idx mapped to 720-slot ring
        tring_slot = (idx % TRING_SLOTS).to(torch.int)
        # GROUND forces odd slot
        odd_mask = (polarity == POLARITY_GROUND) & (tring_slot % 2 == 0)
        tring_slot = torch.where(odd_mask, (tring_slot + 1) % TRING_SLOTS, tring_slot)

        real_mask = torch.ones(N, dtype=torch.bool, device=device)
        return RoutingVerdict(
            idx_in=idx, idx_out=idx_out, mode=mode,
            shape=shape, polarity=polarity, tring_slot=tring_slot,
            zone=zones, pole=poles, pair=pairs,
            partner_zone=partners, real_mask=real_mask,
            gear=gear, pad_len=0, n_tokens=N,
            shadow_norm=0.0,
        )

    # ── Full route: float → verdict ──────────────────────────
    @torch.no_grad()
    def route(self, x: torch.Tensor, mode: int = 0) -> RoutingVerdict:
        """
        Full float → routing pipeline.
        
        x: [B, S, D] or [N, D] float tensor
        mode: 0=ORBITAL 1=CHIRAL 2=CROSS 3=HUB
        """
        D = x.shape[-1]
        x_flat = x.reshape(-1, D)
        N = x_flat.shape[0]

        # snap gear
        gear = snap_gear(N)
        gate_g = self._get_gate(gear)

        # pad, Hilbert scatter, strip
        x_pad, pad_len = pad_to_gear(x_flat, gear)
        x_h = hilbert_scatter(x_pad, gear)
        core, offset, shadow = strip(x_h)

        # encode through gate
        idx, z_q, loss = gate_g.encode_tokens(core)

        # classify
        verdict = self.classify_idx(idx, mode, gear=gear)
        verdict.pad_len = pad_len
        verdict.n_tokens = N
        verdict.shadow_norm = shadow.norm().item()
        # mark padding tokens (always at end after pad_to_gear)
        if pad_len > 0:
            verdict.real_mask = torch.cat([
                torch.ones(N, dtype=torch.bool, device=x_flat.device),
                torch.zeros(pad_len, dtype=torch.bool, device=x_flat.device),
            ])

        return verdict

    # ── Route batch: multiple modes ──────────────────────────
    @torch.no_grad()
    def route_all_modes(self, x: torch.Tensor) -> List[RoutingVerdict]:
        """Test all 4 traverse modes, return list of verdicts"""
        return [self.route(x, mode) for mode in range(4)]

    # ── Simulate dispatch (mirrors tgw_bond_dispatch.h) ──────
    def dispatch_report(self, verdict: RoutingVerdict) -> str:
        """Format verdict like tgw_fgls_connector output"""
        real = verdict.real_mask
        route_n  = (verdict.polarity[real] == POLARITY_ROUTE).sum().item()
        ground_n = (verdict.polarity[real] == POLARITY_GROUND).sum().item()
        mode_names = ["ORBITAL", "CHIRAL", "CROSS", "HUB"]
        mode_str = mode_names[verdict.mode]

        lines = []
        lines.append(f"[BermudaRouter] mode={mode_str}")
        lines.append(f"  tokens={verdict.n_tokens}  padded={verdict.pad_len}  "
                     f"gear={verdict.gear}")
        lines.append(f"  ROUTE={route_n}  GROUND={ground_n}  "
                     f"shadow_norm={verdict.shadow_norm:.2f}")

        # zone distribution (real tokens only)
        z_real = verdict.zone[real]
        for z in range(12):
            cnt = (z_real == z).sum().item()
            if cnt > 0:
                pole_flag = "N" if z >= 6 else "S"
                lines.append(f"    zone={z:2d}({pole_flag}) count={cnt}")

        # TRing slot occupancy (real tokens only)
        occupied = verdict.tring_slot[real].unique().numel()
        lines.append(f"  TRing slots occupied: {occupied}/{TRING_SLOTS}")

        # shape breakdown (real tokens only)
        for s, label in [(SHAPE_I, 'I'), (SHAPE_O, 'O'), (SHAPE_T, 'T'),
                         (SHAPE_S, 'S'), (SHAPE_Z, 'Z'), (SHAPE_L, 'L')]:
            cnt = (verdict.shape[real] == s).sum().item()
            if cnt > 0:
                lines.append(f"    shape {label}: {cnt} tokens")

        # CROSS self-inverse check
        if verdict.mode == 2:
            idx2 = self.gate.codebook.traverse(verdict.idx_out, mode=2)
            match = (idx2 == verdict.idx_in).float().mean().item()
            lines.append(f"  CROSS self-inverse: {match:.1%}")

        return "\n".join(lines)


# ── Demo ─────────────────────────────────────────────────────
def demo():
    print("=" * 60)
    print("Bermuda Router v1 — Float → Geometry Routing")
    print("=" * 60)

    DIM  = 128
    GEAR = 2

    # create router
    router = BermudaRouter(dim=DIM, gear=GEAR)
    print(f"\nRouter initialized: gear={GEAR} slots={router.slots}")

    # synthetic float data (simulating LLM embedding)
    data = []
    for c in range(8):
        ctr = torch.randn(DIM) * 3
        data.append(ctr + torch.randn(64, DIM) * 0.3)
    data = torch.cat(data).to(DEVICE)

    tests = [
        ("[64, D] small batch",      data[:64]),
        ("[128, D] mid batch",       data[:128]),
        ("[256, D] large batch",     data[:256]),
        ("[4, 16, D] spatial",       data[:64].reshape(4, 16, DIM)),
        ("[8, 8, D] square",         data[:64].reshape(8, 8, DIM)),
    ]

    for label, x in tests:
        print(f"\n─── {label} ───")
        for mode in range(4):
            v = router.route(x, mode)
            print(router.dispatch_report(v))

    # ── Mode analysis ────────────────────────────────────────
    print("\n" + "=" * 60)
    print("Mode Analysis: what each traverse does for routing")
    print("=" * 60)

    x = data[:64]
    for mode, name, desc in [
        (0, "ORBITAL", "step +1 → adjacent slot → ROUTE (forward path)"),
        (1, "CHIRAL",  "half-cycle → mirror pole → GROUND (backup)"),
        (2, "CROSS",   "partner zone → cross-face → ROUTE cross-connect"),
        (3, "HUB",     "zone anchor → collapse → GROUND (aggregate)"),
    ]:
        v = router.route(x, mode)
        real   = v.real_mask
        route_n  = (v.polarity[real] == POLARITY_ROUTE).sum().item()
        ground_n = (v.polarity[real] == POLARITY_GROUND).sum().item()
        z_real = v.zone[real]
        z0 = (z_real < 6).sum().item()
        z6 = (z_real >= 6).sum().item()
        print(f"\n  {name:7s} | {desc}")
        print(f"          | zones S={z0} N={z6}  route={route_n} ground={ground_n}")

    # ── Integration check ────────────────────────────────────
    print("\n" + "=" * 60)
    print("Integration: verdict → tgw_bond_dispatch.h")
    print("=" * 60)
    v = router.route(data[:64], mode=0)
    i = 0  # first real token
    print(f"""
  verdict.shape[{i}]     = {v.shape[i].item():>3d}  → shape byte '{chr(v.shape[i].item())}'
  verdict.polarity[{i}]  = {v.polarity[i].item():>3d}  → {'ROUTE' if v.polarity[i].item()==0 else 'GROUND'}
  verdict.tring_slot[{i}]= {v.tring_slot[i].item():>3d}  → TRing slot (0-719)
  verdict.zone[{i}]      = {v.zone[i].item():>3d}  → dodeca face
  verdict.pole[{i}]      = {v.pole[i].item():>3d}  → hemisphere
  shadow_norm          = {v.shadow_norm:.2f}  → residual magnitude

  → Pass to tgw_dispatch():
     slot_a.piece.shape = verdict.shape[i]
     polarity            = verdict.polarity[i]
     tring_pos           = verdict.tring_slot[i]
  → Bond layer verifies, FGLS stores ROUTE, LC-GCFS stores GROUND
""")


if __name__ == "__main__":
    demo()
