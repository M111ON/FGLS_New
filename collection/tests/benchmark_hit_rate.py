"""
Benchmark: RouteRuleCache hit rate with expanded rules.
Lightweight: creates ZoneCards directly (no numpy overhead).
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python_src"))

from route_cache import RouteRuleCache
from zone_card import ZoneCard, CARD_SPARSE, CARD_BATCH, CARD_LZ
import numpy as np

# ── Coverage matrix: every meaningful (type, ent, st, loc) bucket ──
# 3 types x 3 ent x 3 st x 3 loc = 81 combos
ENT_BUCKETS  = [(10, "low"), (80, "moderate"), (200, "high")]
ST_BUCKETS   = [(30, "unstable"), (150, "moderate"), (240, "stable")]
LOC_BUCKETS  = [(30, "remote"), (128, "mid"), (240, "local")]
TYPES        = [(CARD_SPARSE, "sparse"), (CARD_BATCH, "batch"), (CARD_LZ, "lz")]

# ── Edge combos from real data distribution ──
EDGE_CASES = [
    # entropy cliff
    ZoneCard(id=0, card_type=CARD_LZ,     entropy=250, pattern=0, locality=128, stability=5),
    ZoneCard(id=1, card_type=CARD_SPARSE,  entropy=250, pattern=0, locality=128, stability=5),
    ZoneCard(id=2, card_type=CARD_BATCH,   entropy=250, pattern=0, locality=128, stability=5),
    # batch gap cases (formerly misses)
    ZoneCard(id=3, card_type=CARD_BATCH,   entropy=25,  pattern=0, locality=128, stability=150),
    ZoneCard(id=4, card_type=CARD_BATCH,   entropy=25,  pattern=0, locality=128, stability=50),
    ZoneCard(id=5, card_type=CARD_BATCH,   entropy=25,  pattern=0, locality=60,  stability=120),
    # sparse with noise
    ZoneCard(id=6, card_type=CARD_SPARSE,  entropy=10,  pattern=0, locality=128, stability=50),
    ZoneCard(id=7, card_type=CARD_SPARSE,  entropy=80,  pattern=0, locality=128, stability=240),
    ZoneCard(id=8, card_type=CARD_SPARSE,  entropy=80,  pattern=0, locality=128, stability=80),
    # LZ boundary cases
    ZoneCard(id=9, card_type=CARD_LZ,      entropy=200, pattern=0, locality=128, stability=240),
    ZoneCard(id=10, card_type=CARD_LZ,     entropy=150, pattern=0, locality=128, stability=200),
    ZoneCard(id=11, card_type=CARD_LZ,     entropy=150, pattern=0, locality=128, stability=80),
    ZoneCard(id=12, card_type=CARD_LZ,     entropy=80,  pattern=0, locality=40,  stability=128),
    # high locality (cascade trigger)
    ZoneCard(id=13, card_type=CARD_SPARSE, entropy=50,  pattern=0, locality=240, stability=128),
    ZoneCard(id=14, card_type=CARD_BATCH,  entropy=50,  pattern=0, locality=240, stability=128),
    ZoneCard(id=15, card_type=CARD_LZ,     entropy=50,  pattern=0, locality=240, stability=128),
]

# ── Sequence edge cases ──
SEQ_CASES = [
    (CARD_SPARSE, CARD_BATCH, CARD_SPARSE),           # skip-middle
    (CARD_SPARSE, CARD_LZ, CARD_SPARSE),              # skip-middle
    (CARD_BATCH, CARD_SPARSE, CARD_BATCH),            # skip-middle
    (CARD_LZ, CARD_SPARSE, CARD_LZ),                  # skip-middle
    (CARD_SPARSE, CARD_SPARSE),                       # skip-both
    (CARD_LZ, CARD_LZ),                               # lz-pair
    (CARD_BATCH, CARD_BATCH),                         # batch-merge
    (CARD_SPARSE, CARD_SPARSE, CARD_SPARSE),          # skip-all
    (CARD_BATCH, CARD_BATCH, CARD_BATCH),             # batch-chain
    (CARD_LZ, CARD_LZ, CARD_LZ),                      # lz-chain
    (CARD_SPARSE, CARD_BATCH),                         # sparse-fill
    (CARD_BATCH, CARD_SPARSE),                         # sparse-drain
    (CARD_SPARSE, CARD_LZ),                            # lz-entrance
    (CARD_LZ, CARD_SPARSE),                            # lz-exit
    (CARD_BATCH, CARD_LZ),                             # b2l
    (CARD_LZ, CARD_BATCH),                             # l2b
    (CARD_SPARSE, CARD_SPARSE, CARD_BATCH),           # batch-entrance
    (CARD_BATCH, CARD_SPARSE, CARD_SPARSE),           # batch-exit
]


def make_card_at(t, ct: int, ent: int, st: int, loc: int):
    return ZoneCard(id=0, card_type=ct, entropy=ent, pattern=0,
                    locality=loc, stability=st)


def main():
    print("=" * 60)
    print("RouteRuleCache Hit Rate — Full Coverage Matrix")
    print("=" * 60)

    from zone_card import ZoneSequence

    cache = RouteRuleCache()
    single_hits = 0
    single_total = 0

    # 1. Full combinatorial coverage: 81 combos
    for ct, ct_name in TYPES:
        for ent, ent_name in ENT_BUCKETS:
            for st, st_name in ST_BUCKETS:
                for loc, loc_name in LOC_BUCKETS:
                    card = make_card_at(0, ct, ent, st, loc)
                    r = cache.resolve(card)
                    single_total += 1
                    if r["cache"] == "rule" or r["cache"] == "exact":
                        single_hits += 1
                    else:
                        print(f"  MISS: {ct_name} {ent_name}({ent}) "
                              f"{st_name}({st}) {loc_name}({loc}) → {r['decision']}")

    # 2. Edge cases
    for card in EDGE_CASES:
        r = cache.resolve(card)
        single_total += 1
        if r["cache"] == "miss":
            print(f"  MISS edge: type={card.card_type} ent={card.entropy} "
                  f"st={card.stability} loc={card.locality} → {r['decision']}")
        else:
            single_hits += 1

    print(f"")
    print(f"  Single-card coverage: {single_hits}/{single_total} "
          f"({single_hits/single_total*100:.1f}%)")
    s = cache.stats()
    print(f"    Rules: {s['n_rules']}")
    print(f"    Internal hit/miss: {s['single_hits']}/{s['single_misses']}")
    print(f"")

    # 3. Sequence patterns
    seq_hits = 0
    seq_total = len(SEQ_CASES)
    missed_seqs = []
    for i, types in enumerate(SEQ_CASES):
        cards = [make_card_at(i, ct, ent=50, st=240, loc=128) for ct in types]
        seq = ZoneSequence(cards)
        routes = cache.resolve_sequence(seq)
        if routes[0]["cache"] != "seq_rule":
            missed_seqs.append((types, routes[0]["decision"]))

    print(f"  Sequence coverage: {seq_total - len(missed_seqs)}/{seq_total}")
    for types, decision in missed_seqs:
        print(f"    MISS seq: {types} → {decision}")

    # 4. Summary
    combined_hits = single_hits + (seq_total - len(missed_seqs))
    combined_total = single_total + seq_total
    print(f"")
    print(f"  {'='*50}")
    print(f"  OVERALL HIT RATE: {combined_hits}/{combined_total} "
          f"({combined_hits/combined_total*100:.1f}%)")
    print(f"  {'='*50}")

    # 5. Compare with old rule set (simulated: just report current)
    print(f"")
    print(f"  Legacy comparison (estimated):")
    print(f"    Old (~80%):  800/1000 rule hits")
    print(f"    New:         {combined_hits}/{combined_total} "
          f"({combined_hits/combined_total*100:.1f}%)")

    # 6. Projection to production
    print(f"")
    print(f"  Projected per 1,000 decisions:")
    rate = combined_hits / combined_total
    print(f"    Cache hits:        {int(1000 * rate)}  (no LLM)")
    print(f"    Cache misses:      {int(1000 * (1 - rate))}  (LLM fallback)")
    print(f"    LLM reduction vs old (80%): {(rate - 0.80) * 1000:.0f} fewer calls per 1K")


if __name__ == "__main__":
    main()
