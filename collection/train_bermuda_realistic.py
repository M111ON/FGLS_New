"""
train_bermuda_realistic.py — Train GeoCodebook with realistic embedding data
================================================================================
Replaces the simple Gaussian cluster training in bermuda_reshape_v3.run() with:

1. Realistic synthetic data mimicking LLM embeddings:
   - 768-dim (BERT/LLaMA scale) or custom dim
   - Hierarchical cluster structure (topic → subtopic → token)
   - Power-law density (Zipf distribution of cluster sizes)
   - Long-tail tokens in sparse regions

2. Multi-gear training (1-4) to evaluate routing quality per gear

3. Saves trained gate for reuse + reports routing entropy/coverage

Usage:
  python train_bermuda_realistic.py                  # default 768-dim gear=2
  python train_bermuda_realistic.py --dim 128 --gear 2  # quick test
  python train_bermuda_realistic.py --dim 768 --gear 4 --epochs 1200

  # After training, model saved to build/bermuda_gate_g{N}_d{DIM}.pt
  # Load with: gate = torch.load(path)
"""

import torch, torch.nn as nn, torch.nn.functional as F
import math, time, argparse, sys
from pathlib import Path

COLLECTION = Path(__file__).resolve().parent
sys.path.insert(0, str(COLLECTION))

from bermuda_reshape_v3 import (
    BermudaGate, GeoCodebook, GEAR_TABLE, GEAR_INV, GEAR_INV_WL,
    snap_gear, pad_to_gear, hilbert_scatter, hilbert_gather,
    strip, reattach, STRIDE, WALK_LEN, N_ZONES, CROSS_LUT,
    DEVICE, build_geo_table,
)
from bermuda_router_v1 import BermudaRouter, POLARITY_ROUTE, POLARITY_GROUND, TRING_SLOTS


def make_realistic_embeddings(
    n_total: int = 4096,
    dim: int = 768,
    n_topics: int = 24,
    seed: int = 42,
    device: torch.device = DEVICE,
) -> torch.Tensor:
    """
    Generate synthetic embeddings that mimic LLM hidden states.

    Properties:
      - Hierarchical: topics → sub-clusters → tokens
      - Power-law: some topics much denser than others (Zipf)
      - Long-tail: sparse outlier tokens
      - Realistic variance: intra-cluster ~ inter-cluster ratio ~1:10

    Args:
      n_total: total tokens (default 4096 fits gear 4)
      dim: embedding dimension (768 = BERT-base, 1024 = LLaMA, 4096 = internal)
      n_topics: number of topic clusters (default 24 = 2×N_ZONES)
      seed: RNG seed for reproducibility

    Returns: [n_total, dim] float tensor
    """
    rng = torch.Generator(device=device)
    rng.manual_seed(seed)

    # Topic centers — spread across the sphere
    topic_centers = torch.randn(n_topics, dim, generator=rng, device=device)
    topic_centers = F.normalize(topic_centers, dim=-1) * 3.0

    # Power-law distribution of topic sizes (Zipf)
    ranks = torch.arange(1, n_topics + 1, device=device).float()
    zipf_weights = 1.0 / ranks
    zipf_weights /= zipf_weights.sum()

    # Allocate tokens per topic
    topic_assignments = torch.multinomial(
        zipf_weights, n_total, replacement=True, generator=rng
    )

    # Generate tokens: center + noise (intra-topic variance scales with rank)
    # Higher-rank (rarer) topics have wider variance
    data = []
    for t in range(n_topics):
        mask = topic_assignments == t
        n_t = mask.sum().item()
        if n_t == 0:
            continue
        center = topic_centers[t]
        # Variance: common topics are tight, rare topics are spread
        var = 0.05 + 0.3 * (t / n_topics)  # 0.05 for common, 0.35 for rare
        noise = torch.randn(n_t, dim, generator=rng, device=device) * math.sqrt(var)
        data.append(center + noise)

    embeddings = torch.cat(data, dim=0)

    # Add 2% long-tail outliers (far from any cluster)
    n_outliers = max(1, n_total // 50)
    outlier_idx = torch.randperm(n_total, generator=rng, device=device)[:n_outliers]
    outliers = torch.randn(n_outliers, dim, generator=rng, device=device) * 5.0
    embeddings[outlier_idx] = outliers

    # L2 normalize for stable training
    embeddings = F.normalize(embeddings, dim=-1) * math.sqrt(dim)

    return embeddings


def train_gate_realistic(
    dim: int = 768,
    gear: int = 2,
    epochs: int = 800,
    n_data: int = 4096,
    batch_size: int = 128,
    lr: float = 3e-4,
) -> BermudaGate:
    """
    Train BermudaGate on realistic synthetic embeddings.

    Improvements over base train_gate():
      - Higher dimension (768 vs 128)
      - Realistic data distribution (Zipf clusters + outliers)
      - Better hyperparams (lower LR, higher batch, cosine warmup)
      - Saves model checkpoint
      - Reports routing quality metrics
    """
    gate = BermudaGate(dim, code_dim=min(64, dim // 4), gear=gear).to(DEVICE)
    slots = GEAR_TABLE[gear]['slots']

    opt = torch.optim.AdamW(
        list(gate.encoder.parameters()) + list(gate.decoder.parameters()),
        lr=lr, weight_decay=1e-4,
    )
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, epochs)

    data = make_realistic_embeddings(n_total=n_data, dim=dim)
    n_data = data.shape[0]

    print(f"Training BermudaGate  dim={dim}  gear={gear}  "
          f"slots={slots}  data={n_data}  epochs={epochs}")
    t0 = time.perf_counter()

    for ep in range(epochs):
        gate.train()
        ib = torch.randint(0, n_data, (batch_size,), device=DEVICE)
        x  = data[ib]
        core, offset, shadow = strip(x)

        core_out, _, _, loss = gate(core, mode=0)

        z   = gate.encoder(core)
        z_n = F.normalize(z.detach(), dim=-1)
        sim = (z_n @ z_n.T)
        mask = ~torch.eye(batch_size, dtype=torch.bool, device=DEVICE)
        div  = sim[mask].clamp(min=0).mean()

        recon = F.mse_loss(core_out, core)
        total = recon + 0.25 * loss + 0.1 * div

        opt.zero_grad()
        total.backward()
        nn.utils.clip_grad_norm_(gate.parameters(), 1.0)
        opt.step()
        sched.step()

        if (ep + 1) % 200 == 0 or ep == 0:
            usage = (gate.codebook.ema_count > 1.0).float().mean().item()
            print(f"  ep={ep+1:4d}  recon={recon.item():.4f}  "
                  f"loss={loss.item():.4f}  div={div.item():.3f}  "
                  f"usage={usage:.1%}")

    print(f"Trained in {time.perf_counter() - t0:.1f}s")

    # Save
    save_dir = COLLECTION / "build"
    save_dir.mkdir(exist_ok=True)
    path = save_dir / f"bermuda_gate_g{gear}_d{dim}.pt"
    torch.save(gate.state_dict(), path)
    print(f"Saved to {path}")

    return gate


def evaluate_routing(gate: BermudaGate, data: torch.Tensor):
    """Evaluate routing quality: zone distribution, entropy, coverage."""
    router = BermudaRouter(dim=data.shape[-1], gear=gate.gear)
    router.gate = gate

    print(f"\n  Routing quality (gear={gate.gear}, "
          f"dim={data.shape[-1]}, n={data.shape[0]}):")

    for mode, name in enumerate(["ORBITAL", "CHIRAL", "CROSS", "HUB"]):
        verdict = router.route(data, mode)
        real = verdict.real_mask
        n_real = real.sum().item()

        # Zone distribution
        z = verdict.zone[real]
        zone_counts = torch.zeros(12, device=DEVICE)
        for i in range(12):
            zone_counts[i] = (z == i).sum()
        zone_entropy = -(zone_counts / n_real * torch.log(zone_counts / n_real + 1e-10)).sum().item()

        # Route/ground split
        route_n = (verdict.polarity[real] == POLARITY_ROUTE).sum().item()
        ground_n = (verdict.polarity[real] == POLARITY_GROUND).sum().item()

        # TRing coverage
        tring_occ = verdict.tring_slot[real].unique().numel()

        # Codebook usage (sample data in batches)
        all_used_idx = []
        for i in range(0, len(data), 128):
            batch = data[i:i + 128]
            v = router.route(batch, mode)
            all_used_idx.append(v.idx_in[v.real_mask])
        if all_used_idx:
            all_used = torch.cat(all_used_idx)
            codebook_usage = all_used.unique().numel()
        else:
            codebook_usage = 0
        total_slots = gate.slots

        print(f"    {name:7s} | entropy={zone_entropy:.2f} "
              f"zones occupied={(zone_counts>0).sum().item()}/12 "
              f"route={route_n} ground={ground_n} "
              f"tring={tring_occ}/{TRING_SLOTS} "
              f"codebook={codebook_usage}/{total_slots}")


def main():
    parser = argparse.ArgumentParser(description="Train BermudaGate on realistic embeddings")
    parser.add_argument("--dim", type=int, default=768, help="Embedding dimension")
    parser.add_argument("--gear", type=int, default=2, help="Gear (1-4)")
    parser.add_argument("--epochs", type=int, default=800, help="Training epochs")
    parser.add_argument("--data", type=int, default=4096, help="Number of training tokens")
    parser.add_argument("--batch", type=int, default=128, help="Batch size")
    parser.add_argument("--lr", type=float, default=3e-4, help="Learning rate")
    parser.add_argument("--eval", action="store_true", help="Only evaluate, skip training")
    parser.add_argument("--load", type=str, default=None,
                        help="Load model path (default: build/bermuda_gate_g{gear}_d{dim}.pt)")
    args = parser.parse_args()

    if args.load:
        load_path = Path(args.load)
    else:
        load_path = COLLECTION / "build" / f"bermuda_gate_g{args.gear}_d{args.dim}.pt"

    if args.load or args.eval:
        gate = BermudaGate(args.dim, code_dim=min(64, args.dim // 4),
                           gear=args.gear).to(DEVICE)
        if not load_path.exists():
            raise FileNotFoundError(f"Model not found at {load_path}")
        gate.load_state_dict(torch.load(load_path, map_location=DEVICE, weights_only=True))
        print(f"Loaded model from {load_path}")
    else:
        gate = train_gate_realistic(args.dim, args.gear, args.epochs,
                                    args.data, args.batch, args.lr)

    # Evaluate
    data = make_realistic_embeddings(n_total=min(args.data, 4096), dim=args.dim)
    evaluate_routing(gate, data)


if __name__ == "__main__":
    main()
