"""
pogls_vqvae.py — VQ-POGLS: Geometry Codebook VAE
Replaces random VQ-VAE codebook with POGLS 240-position geometry table
- Straight-through estimator (gradient flows through quantize)
- EMA codebook update (stable, no codebook loss needed)
- Random restart: unused codes → reset to random encoder output
- Geometry interpolation: traverse instead of lerp

Colab: pip install torch && python pogls_vqvae.py
"""

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import time

# ── Geometry constants ───────────────────────────────────
N_CODES   = 240   # unique geometry positions
OBS_DIM   =   8   # geometry signal dimension
N_ZONES   =  12
N_ACTIONS =   4   # ORBITAL/CHIRAL/CROSS/HUB

DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

def build_geo_table():
    positions = [(i * 37) % 720 for i in range(N_CODES)]
    table = []
    for enc in positions:
        zone    = enc // 60
        pair    = [0,1,2,3,4,5,0,1,2,3,4,5][zone]
        pole    = [0,0,0,0,0,0,1,1,1,1,1,1][zone]
        partner = [9,10,11,6,7,8,3,4,5,0,1,2][zone]
        frame   = enc % 32
        cell    = enc % 10
        table.append([zone/12., pair/6., float(pole),
                      enc/720., frame/32., cell/10.,
                      partner/12., ((enc*37)%720)/720.])
    return torch.tensor(table, dtype=torch.float32)

GEO_TABLE = build_geo_table().to(DEVICE)   # [240, 8]

# ── Geometry Codebook ────────────────────────────────────
class GeoCodebook(nn.Module):
    """
    240-entry codebook where each entry is a learned vector
    anchored to a geometry position.
    EMA update keeps codebook stable (no codebook loss term).
    Random restart prevents collapse.
    """
    def __init__(self, code_dim, n_codes=N_CODES, decay=0.99, restart_threshold=1.0):
        super().__init__()
        self.n_codes   = n_codes
        self.code_dim  = code_dim
        self.decay     = decay
        self.threshold = restart_threshold

        # codebook: learned vectors [N_CODES, code_dim]
        # init: project geometry table into code_dim space
        with torch.no_grad():
            proj = nn.Linear(OBS_DIM, code_dim, bias=False)
            nn.init.orthogonal_(proj.weight)
            init_codes = proj(GEO_TABLE)
        self.register_buffer('codes',     init_codes.clone())   # [240, D]
        self.register_buffer('ema_count', torch.ones(n_codes))  # usage EMA
        self.register_buffer('ema_sum',   init_codes.clone())   # code sum EMA
        self.register_buffer('geo_idx',   torch.arange(n_codes)) # position map

    def forward(self, z):
        """
        z: [B, code_dim] encoder output
        Returns:
          z_q:     [B, code_dim] quantized (straight-through)
          indices: [B] geometry position indices
          loss:    commitment loss (z tries to match codebook)
          metrics: dict
        """
        B = z.shape[0]
        # distances: [B, N_CODES]
        dist = (z.pow(2).sum(1, keepdim=True)
                - 2 * z @ self.codes.T
                + self.codes.pow(2).sum(1, keepdim=True).T)
        indices = dist.argmin(dim=1)           # [B]
        z_q     = self.codes[indices]          # [B, D]

        # commitment loss: encoder output → codebook (straight-through)
        loss = F.mse_loss(z_q.detach(), z)     # encoder moves toward code
        # straight-through: gradient flows as if z_q == z
        z_q_st = z + (z_q - z).detach()

        # EMA update (only during training)
        if self.training:
            with torch.no_grad():
                one_hot = F.one_hot(indices, self.n_codes).float()  # [B, N]
                count   = one_hot.sum(0)                             # [N]
                sum_z   = one_hot.T @ z                              # [N, D]
                self.ema_count = self.decay * self.ema_count + (1-self.decay) * count
                self.ema_sum   = self.decay * self.ema_sum   + (1-self.decay) * sum_z
                # update codes from EMA
                n = self.ema_count.unsqueeze(1).clamp(min=1e-5)
                self.codes = self.ema_sum / n

                # random restart: unused codes → random encoder output
                dead = (self.ema_count < self.threshold)
                n_dead = dead.sum().item()
                if n_dead > 0:
                    rand_idx = torch.randint(0, B, (int(n_dead),), device=z.device)
                    self.codes[dead] = z[rand_idx].detach()
                    self.ema_sum[dead]   = z[rand_idx].detach()
                    self.ema_count[dead] = 1.0

        usage = (self.ema_count > self.threshold).float().mean().item()
        metrics = {"usage": usage, "n_dead": int((self.ema_count < self.threshold).sum())}
        return z_q_st, indices, loss, metrics

    def traverse(self, indices, mode=0):
        """Move indices to geometry neighbor"""
        if mode == 0: return (indices + 1)   % self.n_codes   # ORBITAL
        if mode == 1: return (indices + 120) % self.n_codes   # CHIRAL
        if mode == 2:  # CROSS: metatron partner
            encs    = (indices * 37) % 720
            zones   = encs // 60
            partner_zones = torch.tensor(
                [9,10,11,6,7,8,3,4,5,0,1,2], device=indices.device)[zones]
            new_encs = partner_zones * 60 + (encs % 60)
            return (new_encs * 37) % self.n_codes
        if mode == 3:  # HUB: go to zone anchor
            encs  = (indices * 37) % 720
            zones = encs // 60
            return (zones * 20) % self.n_codes
        return indices

    def geo_coords(self, indices):
        """indices [B] → geometry coordinates [B, 8]"""
        return GEO_TABLE[indices]

# ── Encoder / Decoder ────────────────────────────────────
class Encoder(nn.Module):
    def __init__(self, input_dim, code_dim, hidden=256):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,    hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,    code_dim),
        )
    def forward(self, x): return self.net(x)

class Decoder(nn.Module):
    def __init__(self, output_dim, code_dim, hidden=256):
        super().__init__()
        # decode from both quantized code and geometry coords
        self.from_code = nn.Sequential(
            nn.Linear(code_dim, hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,   hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,   output_dim),
        )
        self.from_geo = nn.Sequential(
            nn.Linear(OBS_DIM, hidden // 2), nn.GELU(),
            nn.Linear(hidden // 2, output_dim),
        )
    def forward(self, z_q, geo_coords):
        return self.from_code(z_q) + 0.1 * self.from_geo(geo_coords)

# ── VQ-POGLS ─────────────────────────────────────────────
class VqPogls(nn.Module):
    """
    Full VQ-POGLS:
      encode:    x → z → quantize → geometry position
      decode:    position → x_hat
      loss:      recon + commitment (no spread loss needed —
                 EMA + random restart handles codebook usage)
    """
    def __init__(self, dim, code_dim=64, hidden=256):
        super().__init__()
        self.encoder  = Encoder(dim, code_dim, hidden)
        self.codebook = GeoCodebook(code_dim)
        self.decoder  = Decoder(dim, code_dim, hidden)
        self.dim      = dim
        self.code_dim = code_dim

    def forward(self, x):
        z               = self.encoder(x)
        z_q, idx, cl, m = self.codebook(z)
        geo             = GEO_TABLE[idx]
        x_hat           = self.decoder(z_q, geo)
        return x_hat, idx, cl, m

    def loss(self, x, x_hat, commit_loss, beta=0.25):
        recon = F.mse_loss(x_hat, x)
        return recon + beta * commit_loss, recon.item(), commit_loss.item()

    def encode(self, x):
        with torch.no_grad():
            z = self.encoder(x)
            _, idx, _, _ = self.codebook(z)
        return idx

    def decode(self, idx):
        with torch.no_grad():
            z_q = self.codebook.codes[idx]
            geo = GEO_TABLE[idx]
            return self.decoder(z_q, geo)

    def traverse(self, idx, mode=0):
        return self.codebook.traverse(idx, mode)

# ── Training ─────────────────────────────────────────────
def train():
    print(f"device: {DEVICE}")
    DIM      = 128
    CODE_DIM = 64
    BATCH    = 256
    EPOCHS   = 400
    LR       = 1e-3

    def make_data(n=2000):
        pts = []
        for c in range(8):   # 8 clusters → should map to 8+ geometry positions
            ctr = torch.randn(DIM) * 3
            pts.append(ctr + torch.randn(n//8, DIM) * 0.2)
        return torch.cat(pts)

    data  = make_data().to(DEVICE)
    model = VqPogls(DIM, CODE_DIM).to(DEVICE)
    opt   = torch.optim.Adam(
        list(model.encoder.parameters()) + list(model.decoder.parameters()),
        lr=LR)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, EPOCHS)

    params = sum(p.numel() for p in model.parameters())
    print(f"params={params:,}  dim={DIM}  code_dim={CODE_DIM}  "
          f"codebook={N_CODES} positions  clusters=8\n")

    t0 = time.perf_counter()
    for ep in range(EPOCHS):
        idx_b = torch.randint(0, len(data), (BATCH,))
        x     = data[idx_b]
        x_hat, idx, cl, metrics = model(x)
        loss, recon, commit      = model.loss(x, x_hat, cl)
        opt.zero_grad(); loss.backward()
        nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step(); sched.step()

        if (ep+1) % 80 == 0:
            usage   = metrics["usage"]
            n_dead  = metrics["n_dead"]
            print(f"  ep={ep+1:>3}  loss={loss.item():.4f}  "
                  f"recon={recon:.4f}  commit={commit:.4f}  "
                  f"usage={usage:.1%}  dead={n_dead}")

    dt = time.perf_counter() - t0
    print(f"\ntrained in {dt:.1f}s")

    # ── Evaluation ───────────────────────────────────────
    model.eval()
    print("\n=== Geometry Distribution ===")
    with torch.no_grad():
        all_idx = model.encode(data)

    unique = all_idx.unique().numel()
    print(f"unique positions: {unique}/{N_CODES}  ({100*unique/N_CODES:.1f}% coverage)")

    zone_counts = torch.zeros(N_ZONES, dtype=torch.long)
    for i in all_idx:
        enc  = int((i.item() * 37) % 720)
        zone = enc // 60
        zone_counts[zone] += 1
    print(f"zone distribution: {zone_counts.tolist()}")

    # MSE
    recon_err = F.mse_loss(model.decode(all_idx), data).item()
    print(f"encode→decode MSE: {recon_err:.4f}")

    # ── Traverse test ─────────────────────────────────────
    print("\n=== Traverse: decode at each move ===")
    test_x   = data[:4]
    test_idx = model.encode(test_x)
    ref_mse  = F.mse_loss(model.decode(test_idx), test_x).item()

    for mode, name in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
        nxt = model.traverse(test_idx, mode)
        dec = model.decode(nxt)
        mse = F.mse_loss(dec, test_x).item()
        print(f"  {name:7s}  idx {test_idx[:3].tolist()} → {nxt[:3].tolist()}"
              f"  MSE={mse:.4f}")
    print(f"  (reference MSE: {ref_mse:.4f})")

    # ── Reshape: shift all positions, topology intact ─────
    print("\n=== Reshape Test ===")
    for mode, name in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
        shifted = model.traverse(all_idx, mode)
        dec     = model.decode(shifted)
        valid   = torch.isfinite(dec).all()
        uniq    = shifted.unique().numel()
        print(f"  {name:7s}  valid={valid.item()}  "
              f"unique positions after={uniq}")

    print("\n✓ VQ-POGLS complete")
    return model

if __name__ == "__main__":
    model = train()
