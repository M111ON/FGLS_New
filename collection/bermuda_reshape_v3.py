"""
bermuda_reshape_v3.py — Gear + Hilbert-native Tensor Reshape
=============================================================
Pipeline:
  [B, S, D] → snap gear → pad → hilbert_index (stride-37) → traverse
            → hilbert⁻¹ → unpad → [B', S', D]

Gear table (128*n, 2^k aligned):
  Gear 1: n=4  →  512 slots
  Gear 2: n=8  → 1024 slots  ANCHOR (gate_18 clean)
  Gear 3: n=16 → 2048 slots
  Gear 4: n=32 → 4096 slots

Hilbert = stride-37 bijective walk (from skeleton_index.h)
  gcd(37, gear_size) = 1 → full bijection guaranteed
  position → hilbert_index: (pos * 37) % gear_size
  hilbert_index → position: (idx * INV37) % gear_size
  INV37_per_gear computed at init

Shadow Protocol (from v2): strip/reattach unchanged
CROSS self-inverse: cpair(cpair(x)) = x ✓

Colab: !pip install torch && python bermuda_reshape_v3.py
"""

import torch
import torch.nn as nn
import torch.nn.functional as F
from dataclasses import dataclass
from typing import Tuple, Optional
import math, time

DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

# ── Geometry constants ───────────────────────────────────
N_ZONES   = 12
OBS_DIM   =   8
STRIDE    = 37      # bijective walk stride (from skeleton_index.h)
WALK_LEN  = 720     # base walk cycle (kept for geo_table build)

def _gear_walk_len(slots: int) -> int:
    """Smallest multiple of 12 >= slots, coprime with 37"""
    from math import gcd as _gcd
    wl = slots
    while wl % 12 != 0 or _gcd(37, wl) != 1:
        wl += 1
    return wl

CROSS_LUT = torch.tensor([9,10,11,6,7,8,3,4,5,0,1,2], dtype=torch.long)

def build_geo_table(n_codes: int) -> torch.Tensor:
    """Build geometry coordinate table for n_codes positions"""
    positions = [(i * STRIDE) % WALK_LEN for i in range(n_codes)]
    table = []
    for enc in positions:
        zone    = enc // 60
        pair    = [0,1,2,3,4,5,0,1,2,3,4,5][zone % 12]
        pole    = [0,0,0,0,0,0,1,1,1,1,1,1][zone % 12]
        partner = CROSS_LUT[zone % 12].item()
        frame   = enc % 32
        cell    = enc % 10
        table.append([zone/12., pair/6., float(pole),
                      enc/WALK_LEN, frame/32., cell/10.,
                      partner/12., ((enc*STRIDE)%WALK_LEN)/WALK_LEN])
    return torch.tensor(table, dtype=torch.float32)

# ── Gear System ──────────────────────────────────────────
# 128*n slots, all 2^k-friendly
GEAR_TABLE = {
    1: {'n': 4,  'slots': 512,  'walk_len': None},
    2: {'n': 8,  'slots': 1024, 'walk_len': None},   # ANCHOR
    3: {'n': 16, 'slots': 2048, 'walk_len': None},
    4: {'n': 32, 'slots': 4096, 'walk_len': None},
}

def modinv(a: int, m: int) -> int:
    """Extended Euclidean — modular inverse of a mod m"""
    g, x = m, 0
    a0, x0 = a, 1
    while a0 != 0:
        q = g // a0
        g, a0 = a0, g - q * a0
        x, x0 = x0, x - q * x0
    return x % m

# Precompute INV_STRIDE per gear (stride^-1 mod slots)
# Fill per-gear walk_len
for _g in GEAR_TABLE:
    GEAR_TABLE[_g]['walk_len'] = _gear_walk_len(GEAR_TABLE[_g]['slots'])

GEAR_INV     = {g: modinv(STRIDE, info['slots'])          for g, info in GEAR_TABLE.items()}
GEAR_INV_WL  = {g: modinv(STRIDE, info['walk_len'])        for g, info in GEAR_TABLE.items()}

def snap_gear(n_tokens: int) -> int:
    """Snap n_tokens to smallest gear that fits"""
    for gear, info in GEAR_TABLE.items():
        if n_tokens <= info['slots']:
            return gear
    return max(GEAR_TABLE.keys())   # largest gear if overflow

def hilbert_encode(positions: torch.Tensor, gear: int) -> torch.Tensor:
    """position → hilbert index: (pos * STRIDE) % slots"""
    slots = GEAR_TABLE[gear]['slots']
    return (positions * STRIDE) % slots

def hilbert_decode(indices: torch.Tensor, gear: int) -> torch.Tensor:
    """hilbert index → position: (idx * INV_STRIDE) % slots"""
    slots  = GEAR_TABLE[gear]['slots']
    inv    = GEAR_INV[gear]
    return (indices * inv) % slots

# ── Geometry Codebook (per-gear) ─────────────────────────
class GeoCodebook(nn.Module):
    def __init__(self, code_dim: int, gear: int, decay: float = 0.95):
        super().__init__()
        self.gear      = gear
        self.n_codes   = GEAR_TABLE[gear]['slots']
        self.code_dim  = code_dim
        geo_table = build_geo_table(self.n_codes)
        with torch.no_grad():
            proj = nn.Linear(OBS_DIM, code_dim, bias=False)
            nn.init.orthogonal_(proj.weight)
            init_codes = proj(geo_table)
        self.register_buffer('codes',     init_codes.clone())
        self.register_buffer('ema_count', torch.full((self.n_codes,), 2.0))
        self.register_buffer('ema_sum',   init_codes.clone())
        self.register_buffer('geo_table', geo_table)
        self.decay = decay

    def quantize(self, z: torch.Tensor):
        """z [N, D] → z_q, indices, commit_loss"""
        dist    = (z.pow(2).sum(1, keepdim=True)
                   - 2 * z @ self.codes.T
                   + self.codes.pow(2).sum(1, keepdim=True).T)
        idx     = dist.argmin(dim=1)
        z_q     = self.codes[idx]
        loss    = F.mse_loss(z_q.detach(), z)
        z_q_st  = z + (z_q - z).detach()
        if self.training:
            with torch.no_grad():
                oh  = F.one_hot(idx, self.n_codes).float()
                cnt = oh.sum(0)
                self.ema_count = self.decay*self.ema_count + (1-self.decay)*cnt
                self.ema_sum   = self.decay*self.ema_sum   + (1-self.decay)*(oh.T @ z)
                n = self.ema_count.unsqueeze(1).clamp(min=1e-5)
                self.codes = self.ema_sum / n
                dead = self.ema_count < 1.0
                if dead.any():
                    ri = torch.randint(0, z.shape[0], (int(dead.sum()),), device=z.device)
                    self.codes[dead]     = z[ri].detach()
                    self.ema_sum[dead]   = z[ri].detach()
                    self.ema_count[dead] = 2.0
        return z_q_st, idx, loss

    def traverse(self, idx: torch.Tensor, mode: int = 0) -> torch.Tensor:
        N        = self.n_codes
        WL       = GEAR_TABLE[self.gear]['walk_len']   # per-gear walk_len
        FACE_SZ  = WL // 12
        if mode == 0: return (idx + 1) % N
        if mode == 1: return (idx + N // 2) % N
        if mode == 2:
            # CROSS self-inverse: invert in WL-space → clip to slots
            INV_WL = GEAR_INV_WL[self.gear]
            encs   = (idx * STRIDE) % WL
            zones  = encs // FACE_SZ
            pz     = CROSS_LUT.to(idx.device)[zones % 12]
            ne     = pz * FACE_SZ + encs % FACE_SZ
            return (ne * INV_WL) % WL % N
        if mode == 3:
            encs  = (idx * STRIDE) % WL
            zones = encs // FACE_SZ
            return (zones * (N // N_ZONES)) % N
        return idx

# ── Bermuda Gate (per-gear) ──────────────────────────────
class BermudaGate(nn.Module):
    def __init__(self, dim: int, code_dim: int = 64, gear: int = 2):
        super().__init__()
        self.dim      = dim
        self.gear     = gear
        self.slots    = GEAR_TABLE[gear]['slots']
        self.codebook = GeoCodebook(code_dim, gear)
        self.encoder  = nn.Sequential(
            nn.Linear(dim, 256), nn.LayerNorm(256), nn.GELU(),
            nn.Linear(256, code_dim),
        )
        self.decoder  = nn.Sequential(
            nn.Linear(code_dim + OBS_DIM, 256), nn.LayerNorm(256), nn.GELU(),
            nn.Linear(256, dim),
        )

    def encode_tokens(self, x: torch.Tensor):
        """x [N, D] → geo_idx [N], z_q [N, C], loss"""
        z              = self.encoder(x)
        z_q, idx, loss = self.codebook.quantize(z)
        return idx, z_q, loss

    def decode_tokens(self, idx: torch.Tensor) -> torch.Tensor:
        """geo_idx [N] → x_hat [N, D]"""
        z_q = self.codebook.codes[idx]
        geo = self.codebook.geo_table[idx]
        return self.decoder(torch.cat([z_q, geo], dim=-1))

    def forward(self, x: torch.Tensor, mode: int = 0):
        idx, z_q, loss = self.encode_tokens(x)
        nxt            = self.codebook.traverse(idx, mode)
        x_out          = self.decode_tokens(nxt)
        return x_out, idx, nxt, loss

# ── Shadow Protocol (from v2, unchanged) ─────────────────
@dataclass
class BermudaSnapshot:
    offset     : torch.Tensor   # [N, 1]  per-token mean
    shadow     : torch.Tensor   # [N, D]  residual
    idx_in     : torch.Tensor   # [N]     geo addr before traverse
    orig_shape : tuple
    out_shape  : tuple
    gear       : int
    mode       : int
    pad_len    : int            # how many tokens were padding

def strip(x_flat: torch.Tensor):
    offset = x_flat.mean(dim=-1, keepdim=True)
    core   = x_flat - offset
    shadow = core - core.mean(dim=0, keepdim=True)
    core   = core - core.mean(dim=0, keepdim=True)
    return core, offset, shadow

def reattach(core_out: torch.Tensor, snap: BermudaSnapshot) -> torch.Tensor:
    N, D     = core_out.shape
    shadow_r = snap.shadow.reshape(N, D)
    offset_r = snap.offset.reshape(N, 1)
    return core_out + shadow_r + offset_r

# ── Gear Pad / Unpad ─────────────────────────────────────
def pad_to_gear(x_flat: torch.Tensor, gear: int) -> Tuple[torch.Tensor, int]:
    """Pad token sequence to gear slot count. Returns (padded, pad_len)"""
    N, D     = x_flat.shape
    slots    = GEAR_TABLE[gear]['slots']
    pad_len  = slots - N
    if pad_len == 0:
        return x_flat, 0
    # padding reserve = zeros (shadow-neutral)
    pad      = torch.zeros(pad_len, D, device=x_flat.device, dtype=x_flat.dtype)
    return torch.cat([x_flat, pad], dim=0), pad_len

def unpad(x_flat: torch.Tensor, pad_len: int) -> torch.Tensor:
    if pad_len == 0:
        return x_flat
    return x_flat[:-pad_len]

# ── Hilbert Reorder / Restore ────────────────────────────
def hilbert_scatter(x_flat: torch.Tensor, gear: int) -> torch.Tensor:
    """Reorder tokens by Hilbert index (stride-37 walk)"""
    N     = x_flat.shape[0]
    slots = GEAR_TABLE[gear]['slots']
    pos   = torch.arange(N, device=x_flat.device)
    h_idx = hilbert_encode(pos, gear)[:N]      # [N] hilbert positions
    out   = torch.zeros_like(x_flat)
    out[h_idx % N] = x_flat                    # scatter into hilbert order
    return out

def hilbert_gather(x_flat: torch.Tensor, gear: int) -> torch.Tensor:
    """Restore original order from Hilbert-reordered tensor"""
    N     = x_flat.shape[0]
    pos   = torch.arange(N, device=x_flat.device)
    h_idx = hilbert_encode(pos, gear)[:N]
    out   = torch.zeros_like(x_flat)
    out[pos] = x_flat[h_idx % N]               # gather back
    return out

# ── Full Bermuda Pass v3 ─────────────────────────────────
def bermuda_pass_v3(gate: BermudaGate,
                    x: torch.Tensor,
                    out_shape: tuple,
                    mode: int = 0) -> Tuple[torch.Tensor, BermudaSnapshot]:
    """
    Full pipeline:
      1. snap gear
      2. pad to gear slots (padding reserve)
      3. hilbert scatter (stride-37 reorder)
      4. strip shadow
      5. core → Bermuda gate (geometry traverse)
      6. reattach shadow
      7. hilbert gather (restore order)
      8. unpad
      9. reshape to out_shape

    x        : [..., D]
    out_shape: target spatial dims
    """
    D          = x.shape[-1]
    orig_shape = tuple(x.shape[:-1])
    x_flat     = x.reshape(-1, D)
    N          = x_flat.shape[0]

    n_out = 1
    for s in out_shape: n_out *= s
    assert n_out == N, f"N mismatch: {N} vs {n_out}"

    # ── 1. Snap gear ─────────────────────────────────────
    gear = snap_gear(N)
    gate_g = gate if gate.gear == gear else gate   # use provided gate

    # ── 2. Pad ───────────────────────────────────────────
    x_padded, pad_len = pad_to_gear(x_flat, gear)

    # ── 3. Hilbert scatter ───────────────────────────────
    x_h = hilbert_scatter(x_padded, gear)

    # ── 4. Strip ─────────────────────────────────────────
    core, offset, shadow = strip(x_h)

    # ── 5. Gate ──────────────────────────────────────────
    gate_g.eval()
    with torch.no_grad():
        core_out, idx_in, idx_out, _ = gate_g(core, mode)

    snap = BermudaSnapshot(
        offset=offset, shadow=shadow, idx_in=idx_in,
        orig_shape=orig_shape, out_shape=out_shape,
        gear=gear, mode=mode, pad_len=pad_len)

    # ── 6. Reattach ──────────────────────────────────────
    x_out_h = reattach(core_out, snap)

    # ── 7. Hilbert gather ────────────────────────────────
    x_out_padded = hilbert_gather(x_out_h, gear)

    # ── 8. Unpad ─────────────────────────────────────────
    x_out_flat = unpad(x_out_padded, pad_len)

    # ── 9. Reshape ───────────────────────────────────────
    return x_out_flat.reshape(list(out_shape) + [D]), snap

# ── Training ─────────────────────────────────────────────
def train_gate(dim=128, gear=2, epochs=600):
    gate  = BermudaGate(dim, code_dim=64, gear=gear).to(DEVICE)
    slots = GEAR_TABLE[gear]['slots']
    opt   = torch.optim.Adam(
        list(gate.encoder.parameters()) + list(gate.decoder.parameters()), lr=1e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, epochs)

    def make_data(n=1024):
        pts = []
        for c in range(8):
            ctr = torch.randn(dim) * 3
            pts.append(ctr + torch.randn(n//8, dim) * 0.3)
        return torch.cat(pts).to(DEVICE)

    data = make_data()
    print(f"Training BermudaGate  dim={dim}  gear={gear}  "
          f"slots={slots}  epochs={epochs}")
    t0 = time.perf_counter()
    for ep in range(epochs):
        gate.train()
        ib   = torch.randint(0, len(data), (64,))
        x    = data[ib]
        core, offset, shadow = strip(x)
        core_out, _, _, loss = gate(core, mode=0)
        z    = gate.encoder(core)
        z_n  = F.normalize(z.detach(), dim=-1)
        sim  = (z_n @ z_n.T)
        mask = ~torch.eye(64, dtype=torch.bool, device=DEVICE)
        div  = sim[mask].clamp(min=0).mean()
        recon = F.mse_loss(core_out, core)
        total = recon + 0.25*loss + 0.1*div
        opt.zero_grad(); total.backward()
        nn.utils.clip_grad_norm_(gate.parameters(), 1.0)
        opt.step(); sched.step()
        if (ep+1) % 150 == 0:
            usage = (gate.codebook.ema_count > 1.0).float().mean().item()
            print(f"  ep={ep+1}  recon={recon.item():.4f}  "
                  f"div={div.item():.3f}  usage={usage:.1%}")

    print(f"Trained in {time.perf_counter()-t0:.1f}s\n")
    return gate, data

# ── Verify bijection ─────────────────────────────────────
def verify_hilbert_bijection():
    for gear in GEAR_TABLE:
        slots = GEAR_TABLE[gear]['slots']
        pos   = torch.arange(slots)
        h     = hilbert_encode(pos, gear)
        back  = hilbert_decode(h, gear)
        assert (back == pos).all(), f"Gear {gear} bijection FAIL"
        assert h.unique().numel() == slots, f"Gear {gear} not surjective"
    print("Hilbert bijection: ALL GEARS PASS ✓")

def verify_cross_inverse():
    for z in range(12):
        assert CROSS_LUT[CROSS_LUT[z]] == z
    print("CROSS self-inverse: PASS ✓")

# ── Experiments ──────────────────────────────────────────
def run():
    verify_hilbert_bijection()
    verify_cross_inverse()

    DIM  = 128
    GEAR = 2       # ANCHOR: 1024 slots
    gate, data = train_gate(DIM, GEAR)

    print("=" * 56)
    print("BERMUDA v3 — GEAR + HILBERT-NATIVE RESHAPE")
    print("=" * 56)

    with torch.no_grad():

        # ── 1. Gear snap test ────────────────────────────
        print("\n[1] Gear snap — adaptive to input size")
        for n in [50, 200, 500, 1000, 2000]:
            g = snap_gear(n)
            slots = GEAR_TABLE[g]['slots']
            pad   = slots - n
            print(f"  N={n:>4} → Gear {g}  slots={slots}  "
                  f"pad_reserve={pad}  "
                  f"n_shell_n={GEAR_TABLE[g]['n']}")

        # ── 2. Hilbert reorder verify ────────────────────
        print("\n[2] Hilbert scatter/gather roundtrip")
        x_test = torch.randn(64, DIM, device=DEVICE)
        x_h    = hilbert_scatter(x_test, GEAR)
        x_back = hilbert_gather(x_h, GEAR)
        err    = (x_back - x_test).abs().max().item()
        print(f"  scatter → gather max_err={err:.2e}  "
              f"{'✓' if err < 1e-5 else '✗'}")

        # ── 3. Spatial reshape ───────────────────────────
        print("\n[3] Spatial reshape [4,16,D]→[8,8,D]")
        x = data[:64].reshape(4, 16, DIM)
        for mode, name in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
            x_out, snap = bermuda_pass_v3(gate, x, [8, 8], mode)
            valid = torch.isfinite(x_out).all().item()
            mse   = F.mse_loss(x_out.reshape(64, DIM),
                               x.reshape(64, DIM)).item()
            print(f"  {name:7s} valid={valid}  MSE={mse:.4f}  "
                  f"gear={snap.gear}  pad={snap.pad_len}")

        # ── 4. CROSS self-inverse ────────────────────────
        print("\n[4] CROSS self-inverse via gear codebook")
        x_flat = data[:64]
        idx0, _, _ = gate.encode_tokens(x_flat)
        idx1       = gate.codebook.traverse(idx0, mode=2)
        idx2       = gate.codebook.traverse(idx1, mode=2)
        match      = (idx2 == idx0).float().mean().item()
        print(f"  CROSS→CROSS match: {match:.1%}  "
              f"{'✓' if match > 0.9 else '✗'}")

        # ── 5. Gear upgrade: small→large ────────────────
        print("\n[5] Gear upgrade path")
        for N, label in [(50,'small'), (500,'mid'), (1000,'large')]:
            x_n   = data[:N]
            g     = snap_gear(N)
            slots = GEAR_TABLE[g]['slots']
            x_pad, pl = pad_to_gear(x_n, g)
            x_h   = hilbert_scatter(x_pad, g)
            x_b   = hilbert_gather(x_h, g)
            x_up  = unpad(x_b, pl)
            err   = (x_up - x_n).abs().max().item()
            print(f"  {label:6s} N={N}  gear={g}  slots={slots}  "
                  f"pad={pl}  roundtrip_err={err:.2e}")

        # ── 6. Full 2-gate pipeline ──────────────────────
        print("\n[6] 2-gate pipeline [6,8,D]→CHIRAL→[4,12,D]→CROSS→[6,8,D]")
        x_orig = data[:48].reshape(6, 8, DIM)
        x_mid, snap1 = bermuda_pass_v3(gate, x_orig, [4, 12], mode=1)
        x_back, snap2 = bermuda_pass_v3(gate, x_mid,  [6,  8], mode=2)
        valid = torch.isfinite(x_back).all().item()
        mse   = F.mse_loss(x_back, x_orig).item()
        print(f"  valid={valid}  MSE={mse:.4f}")
        print(f"  gear path: {snap1.gear} → {snap2.gear}")
        print(f"  shadow preserved: {snap2.shadow.norm():.3f}")

    print("\n✓ Bermuda v3 complete")
    print("\nKey insight:")
    print("  Hilbert (stride-37) = math guarantee, no neural topology needed")
    print("  Gear = discrete steps, predictable size, padding reserve built-in")
    print("  Shadow = residual rides outside gate, reattach restores fidelity")
    print("  Neural net = semantic transformation only")

if __name__ == "__main__":
    run()
