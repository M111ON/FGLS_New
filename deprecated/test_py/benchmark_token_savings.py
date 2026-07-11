"""
Benchmark: token savings of ZoneCard vs raw geometry text.
Uses synthetic data with realistic dimensions (from registry models).
"""
import sys, json
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python_src"))

import numpy as np
import tiktoken
from zone_card import make_card_from_np

TOKENIZER = tiktoken.get_encoding("cl100k_base")
REGISTRY  = Path(__file__).resolve().parent.parent / "coord_real_registry.json"

# Realistic dims: [n_rows, n_cols] per model name
MODEL_DIMS = {
    "qwen25coder": [220, 1536],
    "qwen25":      [220, 896],
    "smollm2":     [220, 960],
    "qwen3_06b":   [220, 2048],
}


def count_tokens(text: str) -> int:
    return len(TOKENIZER.encode(text))


def format_raw_full(arr: np.ndarray, zone: int, shape: str) -> str:
    """Full dump capped at 10 rows × 16 cols."""
    nr, nc = arr.shape
    show_r, show_c = min(nr, 10), min(nc, 16)
    lines = [f"Zone {zone} Shape {shape} [{nr}x{nc}]:", "["]
    for r in range(show_r):
        vals = ", ".join(f"{arr[r,c]:.4f}" for c in range(show_c))
        lines.append(f"  [{vals}]" + ("," if r < show_r - 1 else ""))
    if nr > show_r:
        lines.append(f"  ... ({nr - show_r} more rows)")
    if nc > show_c:
        lines.append(f"  ... ({nc - show_c} more cols)")
    lines.append("]")
    return "\n".join(lines)


def format_compact(arr: np.ndarray, zone: int, shape: str) -> str:
    """Stats-only: ~200 chars."""
    nr, nc = arr.shape
    flat = arr.flatten()
    mn, mx = float(flat.min()), float(flat.max())
    mu, sd = float(flat.mean()), float(flat.std())
    return (
        f"Zone {zone} Shape {shape} [{nr}x{nc}] "
        f"rng=[{mn:.4f},{mx:.4f}] m={mu:.4f} sd={sd:.4f}"
    )


def format_card(card) -> str:
    return (
        f"[z{card.id:2d} {card.type_name:6s} "
        f"ent={card.entropy:3d} loc={card.locality:3d} "
        f"st={card.stability:3d} pat={card.pattern:04x}]"
    )


def run():
    print("=" * 72)
    print("ZoneCard Token Savings Benchmark")
    print("Tokenizer: cl100k_base")
    print("=" * 72)

    # Build coord list from registry
    coords = []
    if REGISTRY.exists():
        reg = json.loads(REGISTRY.read_text())
        for coord in reg.get("coords", []):
            mk = coord["model_key"]
            if mk in reg.get("models", {}):
                dims = MODEL_DIMS.get(mk, [220, 896])
                coords.append((coord["zone"], coord["shape"], mk, dims))
    if not coords:
        coords = [(2, "I", "qwen25coder", [220, 1536]),
                  (4, "O", "qwen25", [220, 896]),
                  (6, "S", "smollm2", [220, 960])]

    np.random.seed(42)
    samples = []
    for zone, shape, mk, (nr, nc) in coords:
        arr = np.random.randn(nr, nc).astype(np.float32)
        samples.append((zone, shape, arr, mk))

    print(f"\n  Coords: {len(samples)}")
    print("-" * 72)
    print(f"{'Model':14s} {'Zone':>4s} {'Shape':5s} {'Dims':>12s}  "
          f"{'Raw':>7s}  {'Compact':>8s}  {'Card':>5s}  "
          f"{'Saved':>7s}  {'%':>5s}")
    print("-" * 72)

    tr, tc, tm = 0, 0, 0
    for z, s, arr, mk in samples:
        nr, nc = arr.shape
        raw_t = count_tokens(format_raw_full(arr, z, s))
        cmp_t = count_tokens(format_compact(arr, z, s))
        c = make_card_from_np(arr, zone_id=z)
        card_t = count_tokens(format_card(c))
        save = raw_t - card_t
        pct = (save / raw_t) * 100
        tr += raw_t
        tc += cmp_t
        tm += card_t
        print(f"  {mk:14s}  z{z:2d}  {s:5s} [{nr:>4d}x{nc:<4d}]  "
              f"{raw_t:>7d}  {cmp_t:>8d}  {card_t:>5d}  "
              f"{save:>7,d}  {pct:>5.1f}%")

    print("-" * 72)
    save_total = tr - tm
    pct_total = (save_total / tr) * 100
    print(f"  {'TOTAL':>56s}  {tr:>7d}  {tc:>8d}  {tm:>5d}  "
          f"{save_total:>7,d}  {pct_total:>5.1f}%")

    n = len(samples)
    avg_raw = tr / n
    avg_card = tm / n
    avg_cmp = tc / n
    hit_rate = 0.80

    print(f"\n{'='*72}")
    print("Projected per 1,000 Route Decisions")
    print('='*72)
    print(f"  Avg per-zone raw (dump): {avg_raw:>7,.0f} tok")
    print(f"  Avg per-zone compact:    {avg_cmp:>7,.0f} tok")
    print(f"  Avg per-zone ZoneCard:   {avg_card:>7,.0f} tok")
    print(f"  RouteRuleCache hit rate: {hit_rate*100:>5.0f}%")
    print()

    old = 1000 * avg_raw                                # old: every call = raw
    new = 1000 * (1 - hit_rate) * avg_card              # new: only 20% = card
    saved_all = old - new
    saved_by_cache = 1000 * hit_rate * avg_raw
    saved_by_card  = 1000 * (1 - hit_rate) * (avg_raw - avg_card)

    print(f"  Old (1K decisions):       {old:>10,.0f} tok")
    print(f"  New (1K decisions):       {new:>10,.0f} tok")
    print(f"  ─────────────────────────────────────")
    print(f"  Saved by cache (80%):     {saved_by_cache:>10,.0f}")
    print(f"  Saved by ZoneCard (20%):  {saved_by_card:>10,.0f}")
    print(f"  TOTAL SAVED:              {saved_all:>10,.0f}  ({saved_all/old*100:.1f}%)")

    for scale in [1_000, 10_000, 100_000, 1_000_000]:
        o = old * (scale / 1000)
        n = new * (scale / 1000)
        print(f"\n  {scale:>7,}/day: old={o:>13,.0f}  new={n:>13,.0f}"
              f"  saved={o-n:>13,.0f} ({(1-n/o)*100:.1f}%)")

    print(f"\n{'='*72}")
    print("Example (first sample):")
    z, s, arr, mk = samples[0]
    c = make_card_from_np(arr, zone_id=z)
    print(f"\n  Raw ({count_tokens(format_raw_full(arr,z,s))} tok):")
    txt = format_raw_full(arr, z, s)
    for line in txt.split("\n")[:6]:
        print(f"    {line}")
    print(f"\n  ZoneCard ({count_tokens(format_card(c))} tok):")
    print(f"    {format_card(c)}")


if __name__ == "__main__":
    run()
