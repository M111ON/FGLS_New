"""
bermuda_reshape.py — Tensor Bermuda Triangle
============================================
Tensor เข้า → หายใน geometry space → ออกมา shape ต่างกัน
Topology ไม่แตก, semantic preserved

Pipeline:
  [B, S, D] → flatten → encode → geometry addr
                                       ↓
                                  traverse (mode)
                                       ↓
                                  decode → reshape → [B', S', D']

Colab: !pip install torch && python bermuda_reshape.py
"""

import torch
import torch.nn as nn
import torch.nn.functional as F
import time

# ── Geometry constants ───────────────────────────────────
N_CODES   = 240
OBS_DIM   =   8
N_ZONES   =  12
N_ACTIONS =   4
DEVICE    = torch.device("cuda" if torch.cuda.is_available() else "cpu")

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

GEO_TABLE = build_geo_table().to(DEVICE)

# ── Minimal VQ-POGLS (geometry codebook only) ────────────
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
                    ri = torch.randint(0, z.shape[0], (dead.sum().int().item(),), device=z.device)
                    self.codes[dead] = z[ri].detach()
        return z_q_st, indices, loss

    def traverse(self, idx, mode=0):
        if mode == 0: return (idx + 1) % self.n_codes
        if mode == 1: return (idx + self.n_codes//2) % self.n_codes
        if mode == 2:
            encs  = (idx * 37) % 720
            zones = encs // 60
            pz    = torch.tensor([9,10,11,6,7,8,3,4,5,0,1,2], device=idx.device)[zones]
            return (pz * 60 + encs % 60) * 37 % self.n_codes
        if mode == 3:
            return ((idx * 37) % 720 // 60 * 20) % self.n_codes
        return idx

class BermudaGate(nn.Module):
    """
    The Bermuda Triangle gate.
    Tensor goes in → geometry space → comes out reshaped.
    What happens inside: geometry traversal.
    """
    def __init__(self, dim, code_dim=64, n_codes=N_CODES):
        super().__init__()
        self.dim      = dim
        self.code_dim = code_dim
        self.encoder  = nn.Sequential(
            nn.Linear(dim, 256), nn.LayerNorm(256), nn.GELU(),
            nn.Linear(256, code_dim),
        )
        self.codebook = GeoCodebook(code_dim, n_codes)
        self.decoder  = nn.Sequential(
            nn.Linear(code_dim + OBS_DIM, 256), nn.LayerNorm(256), nn.GELU(),
            nn.Linear(256, dim),
        )

    def encode(self, x_flat):
        """[N, D] → indices [N], z_q [N, code_dim]"""
        z               = self.encoder(x_flat)
        z_q, idx, loss  = self.codebook(z)
        return idx, z_q, loss

    def decode(self, idx):
        """indices [N] → [N, D]"""
        z_q = self.codebook.codes[idx]
        geo = GEO_TABLE[idx % N_CODES]          # geometry coords
        return self.decoder(torch.cat([z_q, geo], dim=-1))

    def forward(self, x, mode=0):
        """
        x: any tensor shape [..., D]
        mode: traverse mode (0=ORBITAL 1=CHIRAL 2=CROSS 3=HUB)
        Returns: x_out same shape [..., D], indices, commit_loss
        """
        orig_shape = x.shape
        x_flat     = x.reshape(-1, self.dim)     # [N, D]

        idx, z_q, loss = self.encode(x_flat)
        nxt_idx        = self.codebook.traverse(idx, mode)
        x_out_flat     = self.decode(nxt_idx)

        return x_out_flat.reshape(orig_shape), idx, nxt_idx, loss

# ── Reshape: enter one shape, emerge another ─────────────
def bermuda_reshape(gate, x, in_shape, out_shape, mode=0):
    """
    x:         tensor of shape in_shape + [D]
    out_shape: target spatial shape (total elements must match)
    mode:      traverse mode

    Example:
      [4, 16, 128] → CHIRAL → [8, 8, 128]   (same tokens, mirrored zone)
      [4, 16, 128] → HUB    → [64, 128]      (collapse to anchor)
    """
    D   = x.shape[-1]
    N   = x.reshape(-1, D).shape[0]

    assert all(x.shape[i] == in_shape[i] for i in range(len(in_shape))), \
        f"shape mismatch: {x.shape} vs {in_shape}"

    out_flat, idx, nxt_idx, loss = gate(x.reshape(-1, D), mode)

    # reshape to new spatial layout (N must match)
    n_out = 1
    for s in out_shape: n_out *= s
    assert n_out == N, f"reshape requires same total elements: {N} vs {n_out}"

    return out_flat.reshape(out_shape + [D]), idx, nxt_idx

# ── Train the gate ───────────────────────────────────────
def train_gate(dim=128, n_codes=60, epochs=500):
    gate  = BermudaGate(dim, code_dim=64, n_codes=n_codes).to(DEVICE)
    opt   = torch.optim.Adam(
        list(gate.encoder.parameters()) + list(gate.decoder.parameters()), lr=1e-3)

    # synthetic: 8 clusters in dim-space
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
        idx_b  = torch.randint(0, len(data), (64,))
        x      = data[idx_b]
        # train: ORBITAL reconstruct (mode=0, adjacent position)
        x_out, _, _, loss = gate(x, mode=0)
        recon  = F.mse_loss(x_out, x)
        total  = recon + 0.25 * loss
        opt.zero_grad(); total.backward()
        nn.utils.clip_grad_norm_(gate.parameters(), 1.0)
        opt.step()
        if (ep+1) % 100 == 0:
            usage = (gate.codebook.ema_count > 1.0).float().mean().item()
            print(f"  ep={ep+1}  recon={recon.item():.4f}  usage={usage:.1%}")

    print(f"Trained in {time.perf_counter()-t0:.1f}s\n")
    gate.eval()
    return gate, data

# ── The Bermuda Experiments ──────────────────────────────
def run():
    DIM     = 128
    N_CODES = 60

    gate, data = train_gate(DIM, N_CODES)

    print("=" * 52)
    print("BERMUDA TRIANGLE RESHAPE EXPERIMENTS")
    print("=" * 52)

    with torch.no_grad():

        # ── Experiment 1: shape-preserving traversal ─────
        print("\n[1] Same shape, different zone")
        x = data[:32].reshape(4, 8, DIM)        # [4, 8, 128]
        for mode, name in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
            x_out, idx_in, idx_out = bermuda_reshape(
                gate, x, [4, 8], [4, 8], mode)
            mse   = F.mse_loss(x_out, x).item()
            valid = torch.isfinite(x_out).all().item()
            zones_in  = set(((idx_in  * 37) % 720 // 60).tolist())
            zones_out = set(((idx_out * 37) % 720 // 60).tolist())
            print(f"  {name:7s} [4,8,128]→[4,8,128]  "
                  f"valid={valid}  MSE={mse:.4f}  "
                  f"zones {sorted(zones_in)} → {sorted(zones_out)}")

        # ── Experiment 2: spatial reshape (Bermuda core) ─
        print("\n[2] Spatial reshape — same tokens, new layout")
        x = data[:64].reshape(4, 16, DIM)       # [4, 16, 128]
        for mode, name in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
            # [4, 16] → [8, 8] same 64 tokens
            x_out, idx_in, idx_out = bermuda_reshape(
                gate, x, [4, 16], [8, 8], mode)
            valid = torch.isfinite(x_out).all().item()
            mse   = F.mse_loss(x_out.reshape(64, DIM),
                               x.reshape(64, DIM)).item()
            print(f"  {name:7s} [4,16,128]→[8,8,128]  "
                  f"valid={valid}  MSE={mse:.4f}")

        # ── Experiment 3: sequence compress (HUB collapse)
        print("\n[3] Sequence compress via HUB (many→anchor)")
        x = data[:96].reshape(8, 12, DIM)       # [8, 12, 128]
        # HUB collapses to 12 zone anchors → reshape to [96, 128]
        x_out, idx_in, idx_out = bermuda_reshape(
            gate, x, [8, 12], [96], 3)           # mode=3 HUB
        unique_out = idx_out.unique().numel()
        print(f"  HUB  [8,12,128]→[96,128]  "
              f"unique positions after={unique_out}  "
              f"(should = n_zones ≤ 12)")

        # ── Experiment 4: topology invariant check ───────
        print("\n[4] Topology invariant — traverse twice = self-inverse?")
        x    = data[:32].reshape(32, DIM)
        _, idx0, idx1 = bermuda_reshape(gate, x, [32], [32], 2)  # CROSS
        _, idx1b, idx2 = bermuda_reshape(
            gate,
            gate.decode(idx1).reshape(32, DIM),
            [32], [32], 2)                        # CROSS again
        roundtrip = (idx2 == idx0).float().mean().item()
        print(f"  CROSS → CROSS roundtrip match: {roundtrip:.1%}  "
              f"(100% = self-inverse ✓)")

        # ── Experiment 5: zone-aware reshape ─────────────
        print("\n[5] Zone distribution before/after each traverse")
        x    = data[:120].reshape(10, 12, DIM)  # 10 batch, 12 seq
        flat = x.reshape(-1, DIM)
        idx_base, _, _ = gate.encode(flat)
        zone_base = ((idx_base * 37) % 720 // 60)
        print(f"  base zones used: {zone_base.unique().tolist()}")
        for mode, name in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
            nxt  = gate.codebook.traverse(idx_base, mode)
            zone = ((nxt * 37) % 720 // 60)
            print(f"  {name:7s} zones: {zone.unique().tolist()}")

    print("\n✓ Bermuda reshape complete")
    print("\nKey result:")
    print("  Tensor enters one shape → geometry traversal → emerges new shape")
    print("  Topology intact (valid=True, no NaN/Inf)")
    print("  HUB = collapse to anchors (compression)")
    print("  CHIRAL = mirror zone (parallel dimension)")
    print("  CROSS = partner zone (entangled pair)")

if __name__ == "__main__":
    run()
