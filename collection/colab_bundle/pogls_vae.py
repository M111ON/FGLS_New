"""
pogls_vae.py — POGLS Geometry VAE prototype
Tensor-native: encoder maps tensor → geometry addr
Decoder reconstructs from addr via skeleton traversal

Architecture:
  Encoder: tensor [B, D] → addr [B] (uint64 via geometry bottleneck)
  Decoder: addr [B] → tensor [B, D]
  Loss:    reconstruction + geometry spread (replaces KL)

Geometry bottleneck:
  D floats → project to 8 geometry signals (OBS_DIM)
  8 signals → skeleton_lookup compatible encoding
  → 240 discrete positions (soft via Gumbel-softmax)
"""

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import time

# ── Constants (mirror skeleton_index.h) ─────────────────
N_ZONES    = 12      # pentagon sectors
N_PAIRS    =  6      # bipolar pairs
N_ENC      = 720     # walk cycle
N_MOVES    = 240     # unique positions (720/3)
OBS_DIM    =  8      # geometry signal dim
N_ACTIONS  =  4      # ORBITAL/CHIRAL/CROSS/HUB

DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

# ── Geometry position table ──────────────────────────────
# Pre-compute 240 canonical geometry vectors
# Each position = [zone/12, pair/6, pole, enc/720, frame/32,
#                  cell/10, partner/12, walk_pos/720]

def build_geo_table():
    """Build [240, 8] geometry basis vectors"""
    # stride 37 walk, sample 240 positions
    positions = [(i * 37) % 720 for i in range(240)]
    table = []
    for enc in positions:
        zone    = enc // 60
        pair    = [0,1,2,3,4,5, 0,1,2,3,4,5][zone]
        pole    = [0,0,0,0,0,0, 1,1,1,1,1,1][zone]
        partner = [9,10,11,6,7,8, 3,4,5,0,1,2][zone]
        frame   = enc % 32
        cell    = enc % 10
        table.append([
            zone    / 12.0,
            pair    /  6.0,
            float(pole),
            enc     / 720.0,
            frame   / 32.0,
            cell    / 10.0,
            partner / 12.0,
            ((enc * 37) % 720) / 720.0,  # inverse walk
        ])
    return torch.tensor(table, dtype=torch.float32)  # [240, 8]

GEO_TABLE = build_geo_table().to(DEVICE)  # [240, 8]

# ── Encoder ──────────────────────────────────────────────
class GeoEncoder(nn.Module):
    """
    Maps input tensor [B, D] → geometry logits [B, 240]
    Geometry bottleneck: discrete position in 240-space
    Uses Gumbel-softmax for differentiable discrete sampling
    """
    def __init__(self, input_dim, hidden=256, tau=1.0):
        super().__init__()
        self.tau = tau
        self.net = nn.Sequential(
            nn.Linear(input_dim, hidden),  nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,    hidden),  nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,    N_MOVES),  # → 240 logits
        )

    def forward(self, x, hard=False):
        """
        Returns:
          logits:  [B, 240]
          soft_idx: [B, 240] soft one-hot (Gumbel-softmax)
          geo_vec:  [B, 8]   geometry coordinate (weighted sum)
        """
        logits    = self.net(x)                              # [B, 240]
        soft_idx  = F.gumbel_softmax(logits, tau=self.tau, hard=hard)  # [B, 240]
        geo_vec   = soft_idx @ GEO_TABLE                     # [B, 8]
        return logits, soft_idx, geo_vec

    def encode_hard(self, x):
        """Returns discrete position index [B]"""
        logits, _, _ = self.forward(x, hard=True)
        return logits.argmax(dim=-1)

# ── Decoder ──────────────────────────────────────────────
class GeoDecoder(nn.Module):
    """
    Maps geometry coord [B, 8] → reconstructed tensor [B, D]
    Can also take soft_idx [B, 240] for full gradient flow
    """
    def __init__(self, output_dim, hidden=256):
        super().__init__()
        # two paths: from geo_vec (8) or from soft position (240)
        self.from_geo = nn.Sequential(
            nn.Linear(OBS_DIM, hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,  hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,  output_dim),
        )
        # optional reshape path: traverse from position
        self.from_pos = nn.Sequential(
            nn.Linear(N_MOVES, hidden), nn.GELU(),
            nn.Linear(hidden,  output_dim),
        )

    def forward(self, geo_vec, soft_idx=None):
        """
        geo_vec:  [B, 8]   — primary decode path
        soft_idx: [B, 240] — optional traverse path (adds detail)
        """
        out = self.from_geo(geo_vec)
        if soft_idx is not None:
            out = out + 0.1 * self.from_pos(soft_idx)  # residual traverse
        return out

# ── POGLS VAE ────────────────────────────────────────────
class PoglsVAE(nn.Module):
    """
    Full POGLS VAE:
      encode:  x → geometry position (discrete, 240 codes)
      decode:  position → x_hat
      loss:    reconstruction + geometry spread (no KL needed)

    Geometry spread loss:
      Encourages encoder to use all 240 positions
      (entropy of position distribution)
      Replaces KL divergence — derived from geometry not statistics
    """
    def __init__(self, dim, hidden=256, tau=1.0):
        super().__init__()
        self.encoder = GeoEncoder(dim, hidden, tau)
        self.decoder = GeoDecoder(dim, hidden)
        self.dim     = dim

    def forward(self, x):
        logits, soft_idx, geo_vec = self.encoder(x)
        x_hat = self.decoder(geo_vec, soft_idx)
        return x_hat, logits, soft_idx, geo_vec

    def loss(self, x, x_hat, logits, beta=0.1):
        """
        recon_loss:  MSE reconstruction
        spread_loss: entropy of position distribution (geometry KL)
                     max entropy = all 240 positions used equally
                     = log(240) ≈ 5.48 nats
        """
        recon = F.mse_loss(x_hat, x)

        # spread: maximize entropy → use all geometry positions
        probs       = F.softmax(logits, dim=-1)             # [B, 240]
        avg_probs   = probs.mean(dim=0)                     # [240]
        spread_loss = -(avg_probs * (avg_probs + 1e-8).log()).sum()  # entropy
        max_entropy = torch.tensor(N_MOVES).float().log()
        spread_norm = 1.0 - spread_loss / max_entropy       # 0=uniform, 1=collapsed

        loss = recon + beta * spread_norm
        return loss, recon.item(), spread_norm.item()

    def encode(self, x):
        """Returns geometry position index [B] and geo_vec [B, 8]"""
        with torch.no_grad():
            logits, soft_idx, geo_vec = self.encoder(x)
            idx = logits.argmax(dim=-1)
        return idx, geo_vec

    def decode(self, idx):
        """Decode from discrete position index [B] → x_hat [B, D]"""
        with torch.no_grad():
            # lookup geometry vector from table
            geo_vec = GEO_TABLE[idx]                        # [B, 8]
            x_hat   = self.decoder(geo_vec)
        return x_hat

    def traverse(self, idx, mode=0):
        """
        Move to adjacent geometry position
        mode: 0=ORBITAL(+1) 1=CHIRAL(+120) 2=CROSS(partner) 3=HUB(anchor)
        Returns new idx [B]
        """
        if mode == 0:   return (idx + 1)   % N_MOVES
        if mode == 1:   return (idx + 120) % N_MOVES   # half-cycle jump
        if mode == 2:                                   # cross: mirror in table
            enc     = (idx * 37) % 720
            zone    = enc // 60
            partner_zone = [9,10,11,6,7,8, 3,4,5,0,1,2][zone]
            new_enc = partner_zone * 60 + (enc % 60)
            return torch.tensor([(new_enc * 37) % N_MOVES
                                  if isinstance(idx, int)
                                  else 0], dtype=torch.long)
        if mode == 3:                                   # hub: go to zone anchor
            enc  = (idx * 37) % 720
            zone = enc // 60
            return torch.full_like(idx, zone * 20)      # zone anchor position
        return idx

# ── Quick prototype test ─────────────────────────────────
def run_prototype():
    print(f"device: {DEVICE}")
    print(f"GEO_TABLE: {GEO_TABLE.shape}  (240 positions × 8 signals)")

    DIM    = 128    # input dimension (e.g. embedding size)
    BATCH  = 256
    EPOCHS = 200
    LR     = 1e-3
    BETA   = 0.5    # geometry spread weight

    # synthetic data: 4 clusters (simulate 4 data types)
    def make_data(n=1000):
        clusters = []
        for c in range(4):
            center = torch.randn(DIM) * 2
            pts    = center + torch.randn(n//4, DIM) * 0.3
            clusters.append(pts)
        return torch.cat(clusters, dim=0)

    data  = make_data(1000).to(DEVICE)
    model = PoglsVAE(dim=DIM, hidden=256, tau=1.0).to(DEVICE)
    opt   = torch.optim.Adam(model.parameters(), lr=LR)

    print(f"\ntraining {sum(p.numel() for p in model.parameters()):,} params")
    print(f"input_dim={DIM}  batch={BATCH}  epochs={EPOCHS}")

    t0 = time.perf_counter()
    for ep in range(EPOCHS):
        idx_b = torch.randint(0, len(data), (BATCH,))
        x     = data[idx_b]

        x_hat, logits, soft_idx, geo_vec = model(x)
        loss, recon, spread = model.loss(x, x_hat, logits, beta=BETA)

        opt.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()

        # anneal tau: start diffuse, end sharp
        model.encoder.tau = max(0.1, 1.0 - ep / EPOCHS * 0.9)

        if (ep+1) % 40 == 0:
            print(f"  ep={ep+1:>3}  loss={loss.item():.4f}  "
                  f"recon={recon:.4f}  spread={spread:.3f}  "
                  f"tau={model.encoder.tau:.2f}")

    dt = time.perf_counter() - t0
    print(f"\ntrained in {dt:.1f}s")

    # ── Evaluate geometry distribution ──────────────────
    print("\n=== Geometry Distribution ===")
    with torch.no_grad():
        idxs, geo_vecs = model.encode(data)

    unique_pos = idxs.unique().numel()
    print(f"unique positions used: {unique_pos} / {N_MOVES}  "
          f"({100*unique_pos/N_MOVES:.1f}% coverage)")

    # zone distribution
    zone_counts = torch.zeros(N_ZONES)
    for i in idxs:
        enc  = int((i.item() * 37) % 720)
        zone = enc // 60
        zone_counts[zone] += 1
    print(f"zone distribution: {zone_counts.int().tolist()}")

    # ── Traverse test ───────────────────────────────────
    print("\n=== Traverse Test ===")
    test_x   = data[:8]
    test_idx, test_geo = model.encode(test_x)
    recon    = model.decode(test_idx)
    recon_err = F.mse_loss(recon, test_x).item()
    print(f"encode→decode MSE: {recon_err:.4f}")

    # traverse ORBITAL then decode
    next_idx  = model.traverse(test_idx, mode=0)  # ORBITAL
    recon_orb = model.decode(next_idx)
    print(f"positions: {test_idx[:4].tolist()} → orbital → {next_idx[:4].tolist()}")

    # traverse CHIRAL then decode
    chir_idx  = model.traverse(test_idx, mode=1)  # CHIRAL
    print(f"positions: {test_idx[:4].tolist()} → chiral  → {chir_idx[:4].tolist()}")

    # ── Reshape test: same data, different topology ──────
    print("\n=== Reshape Test (no topology break) ===")
    # encode all data → geometry
    all_idx, all_geo = model.encode(data)
    # traverse entire dataset by ORBITAL (shift all positions +1)
    shifted_idx  = model.traverse(all_idx, mode=0)
    shifted_recon = model.decode(shifted_idx)
    # check: shifted decode should still be valid (no NaN/Inf)
    valid = torch.isfinite(shifted_recon).all()
    print(f"reshape (orbital shift all): valid={valid.item()} ✓")
    print(f"original recon MSE:  {F.mse_loss(model.decode(all_idx), data):.4f}")
    print(f"shifted  recon MSE:  {F.mse_loss(shifted_recon, data):.4f}  "
          f"(should differ — different position)")

    print("\n✓ POGLS VAE prototype complete")
    return model

if __name__ == "__main__":
    model = run_prototype()

def run_prototype_v2():
    """v2: stronger spread + per-cluster geometry separation"""
    print("\n=== Prototype v2 — stronger geometry spread ===")
    DIM, BATCH, EPOCHS = 128, 256, 300
    data = run_prototype.__globals__['make_data'] if False else None

    def make_data(n=1000):
        clusters = []
        for c in range(4):
            center = torch.randn(DIM) * 2
            pts    = center + torch.randn(n//4, DIM) * 0.3
            clusters.append(pts)
        return torch.cat(clusters, dim=0)

    data  = make_data(1000).to(DEVICE)
    # higher tau decay → stays exploratory longer
    model = PoglsVAE(dim=DIM, hidden=256, tau=2.0).to(DEVICE)
    opt   = torch.optim.Adam(model.parameters(), lr=3e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, EPOCHS)

    for ep in range(EPOCHS):
        idx_b = torch.randint(0, len(data), (BATCH,))
        x     = data[idx_b]
        x_hat, logits, soft_idx, geo_vec = model(x)

        recon = F.mse_loss(x_hat, x)
        # stronger spread: use per-sample entropy not just batch avg
        probs       = F.softmax(logits, dim=-1)
        avg_probs   = probs.mean(0)
        batch_ent   = -(avg_probs * (avg_probs+1e-8).log()).sum()
        max_ent     = torch.tensor(float(N_MOVES)).log()
        spread      = 1.0 - batch_ent / max_ent
        # diversity: push different samples to different positions
        # cosine similarity between geo_vecs should be low
        gn   = F.normalize(geo_vec, dim=-1)
        sim  = (gn @ gn.T).abs().mean()
        loss = recon + 0.5 * spread + 0.1 * sim

        opt.zero_grad(); loss.backward()
        nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step(); sched.step()
        model.encoder.tau = max(0.1, 2.0 - ep/EPOCHS * 1.9)

        if (ep+1) % 60 == 0:
            print(f"  ep={ep+1:>3}  loss={loss.item():.4f}  "
                  f"recon={recon.item():.4f}  spread={spread.item():.3f}  "
                  f"sim={sim.item():.3f}  tau={model.encoder.tau:.2f}")

    with torch.no_grad():
        idxs, _ = model.encode(data)
    unique = idxs.unique().numel()
    zone_counts = torch.zeros(N_ZONES)
    for i in idxs:
        enc  = int((i.item() * 37) % 720)
        zone = enc // 60
        zone_counts[zone] += 1
    print(f"unique positions: {unique}/{N_MOVES}  ({100*unique/N_MOVES:.1f}%)")
    print(f"zone distribution: {zone_counts.int().tolist()}")
    recon_err = F.mse_loss(model.decode(idxs), data).item()
    print(f"encode→decode MSE: {recon_err:.4f}")
    print("✓ v2 complete")
    return model

if __name__ == "__main__":
    import torch.nn as nn
    run_prototype_v2()
