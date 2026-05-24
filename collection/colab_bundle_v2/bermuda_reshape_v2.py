"""
bermuda_reshape_v2.py — Tensor Bermuda Triangle + Shadow Protocol
==================================================================
Fibo clock backbone: 1440 ticks/cycle, 24 zones.

Metal detector pattern:
  STRIP   → snapshot shape + offset + shadow (residual)
  PASS    → core tensor ผ่าน geometry gate (clock-driven)
  REATTACH → สวม shadow + offset กลับ

CROSS self-inverse fix: cpair(cpair(x)) = x (frozen invariant)

Colab: !pip install torch && python bermuda_reshape_v2.py
"""

import torch
import torch.nn as nn
import torch.nn.functional as F
from dataclasses import dataclass
from typing import Tuple
import time

# ── Fibo clock — everything ties to this ──────────────────
from fibo_clock import (
    FiboClock, TICKS_PER_CYCLE, TICKS_PER_ZONE,
    N_CODES, N_ZONES,
    idx_to_tick, tick_to_idx, build_geo_table,
    cycle_phase, encode_addr, decode_addr,
)

OBS_DIM   = 8
N_ACTIONS = 4
DEVICE    = torch.device("cuda" if torch.cuda.is_available() else "cpu")

GEO_TABLE = build_geo_table(N_CODES).to(DEVICE)

# CROSS self-inverse (12-zone in 720-walk space)
# cpair(cpair(x)) = x  ✓
CROSS_LUT_12 = [9,10,11,6,7,8,3,4,5,0,1,2]

# ── Geometry Codebook ────────────────────────────────────
class GeoCodebook(nn.Module):
    def __init__(self, code_dim, n_codes=N_CODES, decay=0.95):
        super().__init__()
        self.n_codes  = n_codes
        self.code_dim = code_dim
        with torch.no_grad():
            proj = nn.Linear(OBS_DIM, code_dim, bias=False)
            nn.init.orthogonal_(proj.weight)
            init_codes = proj(GEO_TABLE.cpu())[:n_codes]
        self.register_buffer('codes',     init_codes.clone())
        self.register_buffer('ema_count', torch.full((n_codes,), 2.0))
        self.register_buffer('ema_sum',   init_codes.clone())

    def forward(self, z):
        dist    = (z.pow(2).sum(1, keepdim=True)
                   - 2 * z @ self.codes.T
                   + self.codes.pow(2).sum(1, keepdim=True).T)
        indices = dist.argmin(dim=1)
        z_q     = self.codes[indices]
        loss    = F.mse_loss(z_q.detach(), z)
        z_q_st  = z + (z_q - z).detach()
        if self.training:
            with torch.no_grad():
                one_hot = F.one_hot(indices, self.n_codes).float()
                self.ema_count = 0.95*self.ema_count + 0.05*one_hot.sum(0)
                self.ema_sum   = 0.95*self.ema_sum   + 0.05*(one_hot.T @ z)
                n = self.ema_count.unsqueeze(1).clamp(min=1e-5)
                self.codes = self.ema_sum / n
                dead = self.ema_count < 1.0
                if dead.any():
                    ri = torch.randint(0, z.shape[0], (int(dead.sum()),), device=z.device)
                    self.codes[dead] = z[ri].detach()
                    self.ema_sum[dead]   = z[ri].detach()
                    self.ema_count[dead] = 2.0
        return z_q_st, indices, loss

    def traverse(self, idx, mode=0, step=1, cycle=None):
        """
        All modes work in fibo tick-space (0..1439).
        cycle (optional): injects cycle phase for continuous sequence encoding.

        ORBITAL  tick + step          (advance by step ticks)
        CHIRAL   tick + 720 + step-1  (half-cycle + offset)
        CROSS    zone → partner       (self-inverse ✓)
        HUB      → zone anchor        (collapse to zone's first tick)
        """
        # ── Apply cycle phase before traversal ──────────────
        if cycle is not None:
            phase = cycle_phase(cycle, self.n_codes)
            idx = (idx + phase) % self.n_codes

        tick = idx_to_tick(idx)  # convert to fibo tick-space

        if mode == 0:
            new_tick = (tick + step) % TICKS_PER_CYCLE
            result = tick_to_idx(new_tick)

        elif mode == 1:
            half = self.n_codes // 2
            mirrored = (idx + half) % self.n_codes
            mirror_tick = idx_to_tick(mirrored)
            new_tick = (mirror_tick + step) % TICKS_PER_CYCLE
            idx_full = tick_to_idx(new_tick)
            ticks_per_code = max(1, TICKS_PER_CYCLE // self.n_codes)
            result = (idx_full // ticks_per_code) % self.n_codes

        elif mode == 2:
            encs = (idx * 37) % 720
            zones = encs // 60
            offset_in_zone = encs % 60
            pz = torch.tensor(CROSS_LUT_12, device=idx.device, dtype=torch.long)[zones]
            new_encs = pz * 60 + offset_in_zone
            result = (new_encs * 253) % self.n_codes

        elif mode == 3:
            encs = (idx * 37) % 720
            zones = encs // 60
            result = (zones * (self.n_codes // 12)) % self.n_codes

        else:
            result = idx

        # ── Remove cycle phase after traversal ──────────────
        if cycle is not None:
            result = (result - phase) % self.n_codes

        return result

# ── Bermuda Gate ─────────────────────────────────────────
class BermudaGate(nn.Module):
    def __init__(self, dim, code_dim=64, n_codes=N_CODES):
        super().__init__()
        self.dim      = dim
        self.code_dim = code_dim
        self.n_codes  = n_codes
        self.encoder  = nn.Sequential(
            nn.Linear(dim,  256), nn.LayerNorm(256), nn.GELU(),
            nn.Linear(256,  code_dim),
        )
        self.codebook = GeoCodebook(code_dim, n_codes)
        # Local geo table — same as global GEO_TABLE but can be reordered
        self.register_buffer('geo_table', build_geo_table()[:n_codes].clone())
        self.decoder  = nn.Linear(code_dim + OBS_DIM, dim)

    def reorder_codes(self, spread=2.0):
        """Reorder codebook + geo_table by PC1 projection for spatial locality.
        After reorder: adjacent indices have similar codes → traverse step
        produces meaningful diff scaling.
        spread: multiply PC1 deviation by this factor to amplify gear ramp.
        """
        with torch.no_grad():
            codes = self.codebook.codes  # [n_codes, code_dim]
            c = codes - codes.mean(dim=0, keepdim=True)
            U, S, V = torch.svd(c.float())
            pc1_dir = V[:, 0:1]  # [code_dim, 1]
            pc1_val = c @ pc1_dir   # [n_codes, 1]
            perm = pc1_val.squeeze(1).argsort()
            # Sort codes by PC1
            code_s = codes[perm].contiguous()
            # Expand PC1 component: pc1_new = pc1_old * spread
            code_s_c = code_s - code_s.mean(dim=0, keepdim=True)
            pc1_of_sorted = code_s_c @ pc1_dir  # [n_codes, 1]
            off_pc1 = code_s_c - pc1_of_sorted @ pc1_dir.T
            pc1_exp = pc1_of_sorted * spread
            reconstructed = pc1_exp @ pc1_dir.T + off_pc1 + code_s.mean(dim=0, keepdim=True)
            self.codebook.codes = reconstructed.contiguous()
            # Reorder geo table and EMA buffers
            self.geo_table = self.geo_table[perm].contiguous()
            self.codebook.ema_count = self.codebook.ema_count[perm].contiguous()
            self.codebook.ema_sum = self.codebook.ema_sum[perm].contiguous()
        return perm

    def encode(self, x_flat):
        z              = self.encoder(x_flat)
        z_q, idx, loss = self.codebook(z)
        return idx, z_q, loss

    def decode(self, idx):
        z_q = self.codebook.codes[idx % self.n_codes]
        geo = self.geo_table[idx % self.n_codes]
        return self.decoder(torch.cat([z_q, geo], dim=-1))

    def forward(self, x_flat, mode=0, step=1, cycle=None):
        idx, z_q, loss = self.encode(x_flat)
        nxt_idx        = self.codebook.traverse(idx, mode, step=step, cycle=cycle)
        x_out          = self.decode(nxt_idx)
        return x_out, idx, nxt_idx, loss

# ── Shadow Snapshot ──────────────────────────────────────
@dataclass
class BermudaSnapshot:
    """
    Metal detector ticket — holds everything stripped before the gate
    offset : per-token mean  [N, 1]   (scale/bias)
    shadow : residual        [N, D]   (what geometry can't handle)
    idx_in : geometry addr   [N]      (address before traverse)
    orig_shape : tuple                (original tensor shape)
    out_shape  : tuple                (target reshape)
    mode       : int                  (traverse mode)
    """
    offset     : torch.Tensor
    shadow     : torch.Tensor
    idx_in     : torch.Tensor
    orig_shape : tuple
    out_shape  : tuple
    mode       : int

def strip(x_flat: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """
    Strip offset + shadow from flat tensor.
    core = normalized, low-variance signal geometry can work with.
    shadow = high-freq residual that rides along outside the gate.
    """
    offset = x_flat.mean(dim=-1, keepdim=True)   # [N, 1]
    core   = x_flat - offset                      # [N, D] zero-mean
    # shadow = per-token std deviation pattern (what's beyond mean)
    shadow = core - core.mean(dim=0, keepdim=True) # [N, D] residual vs batch mean
    core   = core - core.mean(dim=0, keepdim=True) # pure core
    return core, offset, shadow

def reattach(core_out: torch.Tensor,
             snap: BermudaSnapshot) -> torch.Tensor:
    """
    Sew shadow + offset back onto core after gate.
    Shadow reshapes to match new spatial layout — topology preserved.
    """
    N, D   = core_out.shape
    # shadow carries through — reshape to new spatial layout
    shadow_r = snap.shadow.reshape(N, D)   # same N, just new spatial context
    offset_r = snap.offset.reshape(N, 1)
    return core_out + shadow_r + offset_r

# ── Full Bermuda Pass ────────────────────────────────────
def bermuda_pass(gate: BermudaGate,
                 x: torch.Tensor,
                 out_shape: tuple,
                 mode: int = 0,
                 step: int = 1) -> Tuple[torch.Tensor, BermudaSnapshot]:
    """
    Full metal-detector pipeline:
      1. snapshot original shape
      2. strip → core / offset / shadow
      3. core → Bermuda gate (geometry traversal)
      4. reattach shadow + offset
      5. reshape to out_shape

    x        : [..., D] any shape
    out_shape: target spatial dims (product must == x[:-1].numel())
    mode     : traverse mode
    """
    D          = x.shape[-1]
    orig_shape = tuple(x.shape[:-1])
    x_flat     = x.reshape(-1, D)
    N          = x_flat.shape[0]

    n_out = 1
    for s in out_shape: n_out *= s
    assert n_out == N, f"N mismatch: {N} → {n_out}"

    # ── STRIP ────────────────────────────────────────────
    core, offset, shadow = strip(x_flat)

    # ── PASS through gate ────────────────────────────────
    gate.eval()
    with torch.no_grad():
        core_out, idx_in, idx_out, _ = gate(core, mode, step=step)

    # ── snapshot ─────────────────────────────────────────
    snap = BermudaSnapshot(
        offset=offset, shadow=shadow, idx_in=idx_in,
        orig_shape=orig_shape, out_shape=out_shape, mode=mode)

    # ── REATTACH ─────────────────────────────────────────
    x_out_flat = reattach(core_out, snap)
    x_out      = x_out_flat.reshape(tuple(out_shape) + (D,))

    return x_out, snap

# ── Training ─────────────────────────────────────────────
def train_gate(dim=128, n_codes=60, epochs=600):
    gate = BermudaGate(dim, code_dim=64, n_codes=n_codes).to(DEVICE)
    opt  = torch.optim.Adam(
        list(gate.encoder.parameters()) + list(gate.decoder.parameters()), lr=1e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, epochs)

    def make_data(n=800):
        pts = []
        for c in range(8):
            ctr = torch.randn(dim) * 3
            pts.append(ctr + torch.randn(n//8, dim) * 0.3)
        return torch.cat(pts).to(DEVICE)

    data = make_data()
    print(f"Training BermudaGate  dim={dim}  n_codes={n_codes}  epochs={epochs}")
    t0 = time.perf_counter()
    for ep in range(epochs):
        gate.train()
        ib     = torch.randint(0, len(data), (64,))
        x      = data[ib]
        core, offset, shadow = strip(x)
        core_out, _, _, loss = gate(core, mode=0)

        # ── Multi-component diversity + ramp loss ───────────────
        # Target: ramp > 2×  (step sensitivity)
        with torch.no_grad():
            z = gate.encoder(core)
        z_n  = F.normalize(z, dim=-1)
        sim  = (z_n @ z_n.T)                            # [B, B]
        mask = ~torch.eye(64, dtype=torch.bool, device=DEVICE)

        # (1) Uniformity: push encoder outputs apart on hypersphere
        pair_sim = sim[mask]
        div_uni  = pair_sim.clamp(min=0).mean()

        # (2) Margin repulsion: penalize similarity above threshold
        margin = 0.1
        div_margin = (pair_sim - margin).clamp(min=0).mean()

        # (3) Codebook dispersion: push all code vectors away from each other
        c_n  = F.normalize(gate.codebook.codes, dim=-1)
        c_sim = c_n @ c_n.T
        c_mask = ~torch.eye(gate.n_codes, dtype=torch.bool, device=DEVICE)
        div_code = c_sim[c_mask].clamp(min=0.05).mean()

        # (4) Usage entropy: encourage full codebook utilization
        usage_p = gate.codebook.ema_count / gate.codebook.ema_count.sum().clamp(min=1e-8)
        entropy = -(usage_p * (usage_p + 1e-10).log()).sum()
        div_ent = -entropy

        # (5) Ramp loss: maximize absolute divergence at large step
        # Simple push: output at step=120 ≠ output at step=1
        c120, _, _, _ = gate(core, mode=0, step=120)
        ramp_loss = -F.mse_loss(c120, core_out)

        div = div_uni + 0.5*div_margin + 0.5*div_code + 0.05*div_ent
        recon = F.mse_loss(core_out, core)
        total = recon + 0.25*loss + 1.0*div + 5.0*ramp_loss
        opt.zero_grad(); total.backward()
        nn.utils.clip_grad_norm_(gate.parameters(), 1.0)
        opt.step(); sched.step()
        if (ep+1) % 150 == 0:
            usage = (gate.codebook.ema_count > 1.0).float().mean().item()
            print(f"  ep={ep+1}  recon={recon.item():.4f}  "
                  f"div_uni={div_uni.item():.3f}  "
                  f"div_margin={div_margin.item():.3f}  "
                  f"div_code={div_code.item():.3f}  "
                  f"ramp={ramp_loss.item():.4f}  "
                  f"entropy={entropy.item():.2f}  "
                  f"usage={usage:.1%}")

    print(f"Trained in {time.perf_counter()-t0:.1f}s\n")
    return gate, data

# ── Verify CROSS_LUT self-inverse ────────────────────────
def verify_cross_lut():
    for z in range(N_ZONES):
        assert CROSS_LUT[CROSS_LUT[z]] == z, f"CROSS_LUT not self-inverse at zone {z}"
    print(f"CROSS_LUT self-inverse ({N_ZONES} zones): PASS ✓")

# ── Experiments ──────────────────────────────────────────
def run():
    verify_cross_lut()
    DIM, N_CODES = 128, 60
    gate, data   = train_gate(DIM, N_CODES)

    print("=" * 54)
    print("BERMUDA TRIANGLE v2 — SHADOW PROTOCOL")
    print("=" * 54)

    with torch.no_grad():

        # ── 1. Shadow roundtrip ──────────────────────────
        print("\n[1] Shadow reattach — topology invariant")
        x = data[:32].reshape(4, 8, DIM)
        for mode, name in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
            x_out, snap = bermuda_pass(gate, x, [4, 8], mode)
            valid = torch.isfinite(x_out).all().item()
            # shadow contribution vs core contribution
            core_mag   = (x - x.mean(dim=-1, keepdim=True)).norm().item()
            shadow_mag = snap.shadow.norm().item()
            print(f"  {name:7s} valid={valid}  "
                  f"core={core_mag:.2f}  shadow={shadow_mag:.2f}  "
                  f"shape={list(x_out.shape)}")

        # ── 2. Spatial reshape ───────────────────────────
        print("\n[2] Spatial reshape [4,16,D]→[8,8,D]")
        x = data[:64].reshape(4, 16, DIM)
        for mode, name in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
            x_out, snap = bermuda_pass(gate, x, [8, 8], mode)
            valid = torch.isfinite(x_out).all().item()
            mse   = F.mse_loss(x_out.reshape(64, DIM), x.reshape(64, DIM)).item()
            zones_in  = (idx_to_tick(snap.idx_in) // TICKS_PER_ZONE).unique().tolist()
            zones_out = (idx_to_tick(gate.codebook.traverse(snap.idx_in, mode)) // TICKS_PER_ZONE).unique().tolist()
            print(f"  {name:7s} valid={valid}  MSE={mse:.4f}  "
                  f"zones {sorted(zones_in)}→{sorted(zones_out)}")

        # ── 3. CROSS self-inverse ────────────────────────
        print("\n[3] CROSS self-inverse: cpair(cpair(x)) = x")
        x_flat = data[:64]
        idx0,  _, _ = gate.encode(x_flat)
        idx1        = gate.codebook.traverse(idx0, mode=2)   # CROSS
        idx2        = gate.codebook.traverse(idx1, mode=2)   # CROSS again
        match       = (idx2 == idx0).float().mean().item()
        print(f"  idx0 → CROSS → idx1 → CROSS → idx2")
        print(f"  idx2 == idx0: {match:.1%}  {'✓' if match > 0.9 else '✗'}")

        # ── 4. HUB compress + snapshot restore ───────────
        print("\n[4] HUB compress [8,12,D]→[96,D] + snapshot")
        x = data[:96].reshape(8, 12, DIM)
        x_out, snap = bermuda_pass(gate, x, [96], mode=3)
        idx_hub     = gate.codebook.traverse(snap.idx_in, 3)
        unique_pos  = idx_hub.unique().numel()
        print(f"  HUB unique positions: {unique_pos}  (≤ n_zones={N_ZONES})")
        print(f"  snapshot: orig={snap.orig_shape} → out={snap.out_shape}  "
              f"mode={snap.mode}")
        print(f"  shadow norm={snap.shadow.norm():.3f}  "
              f"offset norm={snap.offset.norm():.3f}")

        # ── 5. Full pipeline: in → Bermuda → out → restore
        print("\n[5] Full pipeline: enter → traverse → restore shape")
        x_orig = data[:48].reshape(6, 8, DIM)
        # enter: [6,8,D] → CHIRAL → [4,12,D]
        x_mid, snap_mid = bermuda_pass(gate, x_orig, [4, 12], mode=1)
        # traverse again: [4,12,D] → CROSS → [6,8,D]
        x_back, snap_back = bermuda_pass(gate, x_mid, [6, 8], mode=2)
        valid = torch.isfinite(x_back).all().item()
        mse   = F.mse_loss(x_back, x_orig).item()
        print(f"  [6,8,D] →CHIRAL→ [4,12,D] →CROSS→ [6,8,D]")
        print(f"  valid={valid}  MSE={mse:.4f}")
        print(f"  shadow preserved through 2 gates: "
              f"{snap_back.shadow.norm():.3f}")

    print("\n✓ Bermuda v2 complete")
    print("\nShadow Protocol summary:")
    print("  STRIP  : separate core (geometry-passable) from shadow (residual)")
    print("  PASS   : core traverses geometry gate — shape can change")
    print("  REATTACH: shadow + offset sewed back — topology restored")
    print("  Snapshot: ticket that proves the same entity came through")

if __name__ == "__main__":
    run()
