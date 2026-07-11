"""Tests: RouteRuleCache — kills LLM in hot path."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python_src"))

from route_cache import RouteRuleCache, RouteRule
from zone_card import ZoneCard, CARD_SPARSE, CARD_BATCH, CARD_LZ


def t(desc, ok):
    print(f"  {'PASS' if ok else 'FAIL'} {desc}")
    return ok


def test_hit_sparse_skip():
    cache = RouteRuleCache()
    card = ZoneCard(id=3, card_type=CARD_SPARSE, entropy=5, pattern=0,
                    locality=50, stability=255)
    r = cache.resolve(card)
    ok = t("sparse+stable → skip", r["decision"] == "skip")
    ok &= t("cache hit", r["cache"] == "rule")
    return ok


def test_hit_batch_fast():
    cache = RouteRuleCache()
    card = ZoneCard(id=2, card_type=CARD_BATCH, entropy=25, pattern=0x01,
                    locality=220, stability=240)
    r = cache.resolve(card)
    ok = t("batch+stable+local → batch-chain/reuse/cascade",
           r["decision"] in ("batch-chain", "batch", "cascade-reuse"))
    ok &= t("cache hit", r["cache"] == "rule")
    return ok


def test_hit_batch_low_locality():
    cache = RouteRuleCache()
    card = ZoneCard(id=5, card_type=CARD_BATCH, entropy=60, pattern=0x02,
                    locality=30, stability=180)
    r = cache.resolve(card)
    ok = t("batch+low locality → batch-verify", r["decision"] == "batch-verify")
    return ok


def test_hit_lz_full():
    cache = RouteRuleCache()
    card = ZoneCard(id=7, card_type=CARD_LZ, entropy=220, pattern=0xFF,
                    locality=128, stability=50)
    r = cache.resolve(card)
    ok = t("LZ+unstable → full-decode", r["decision"] == "full-decode")
    return ok


def test_miss_fallback():
    cache = RouteRuleCache(rules=[])  # no rules → everything misses
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=40, pattern=0x7F,
                    locality=60, stability=120)
    r = cache.resolve(card)
    ok = t("no rules → planner", r["decision"] == "planner")
    ok &= t("cache miss", r["cache"] == "miss")
    return ok


def test_exact_cache():
    cache = RouteRuleCache()
    card = ZoneCard(id=0, card_type=CARD_SPARSE, entropy=5, pattern=0,
                    locality=128, stability=255, hash_val=0xABCD)
    # First call → rule hit
    r1 = cache.resolve(card)
    # Store as exact
    cache.store(card, {"decision": "skip", "action": "noop", "reason": "cached"})
    # Second call → exact hit
    r2 = cache.resolve(card)
    ok = t("second call → exact cache", r2["cache"] == "exact")
    ok &= t("exact decision preserved", r2["decision"] == "skip")
    return ok


def test_sequence_skip_middle():
    cache = RouteRuleCache()
    cards = [
        ZoneCard(id=1, card_type=CARD_BATCH, entropy=25, pattern=0,
                 locality=128, stability=240),
        ZoneCard(id=2, card_type=CARD_SPARSE, entropy=5, pattern=0,
                 locality=128, stability=255),
        ZoneCard(id=3, card_type=CARD_BATCH, entropy=30, pattern=0,
                 locality=128, stability=235),
    ]
    from zone_card import ZoneSequence
    seq = ZoneSequence(cards)
    routes = cache.resolve_sequence(seq)
    ok = t("seq returns 3 routes", len(routes) == 3)
    ok &= t("middle zone is skip (seq rule)",
            routes[1]["decision"] == "skip")
    ok &= t("first zone decision from seq rule",
            routes[0]["decision"] in ("skip-middle", "batch"))
    ok &= t("seq cache hit", routes[0]["cache"] == "seq_rule")
    return ok


def test_sequence_all_sparse():
    cache = RouteRuleCache()
    cards = [
        ZoneCard(id=1, card_type=CARD_SPARSE, entropy=5, pattern=0,
                 locality=128, stability=255),
        ZoneCard(id=2, card_type=CARD_SPARSE, entropy=10, pattern=0,
                 locality=128, stability=250),
        ZoneCard(id=3, card_type=CARD_SPARSE, entropy=3, pattern=0,
                 locality=128, stability=255),
    ]
    from zone_card import ZoneSequence
    seq = ZoneSequence(cards)
    routes = cache.resolve_sequence(seq)
    ok = t("all sparse → skip-all", routes[0]["decision"] == "skip")
    return ok


def test_stats():
    cache = RouteRuleCache()
    card = ZoneCard(id=3, card_type=CARD_SPARSE, entropy=5, pattern=0,
                    locality=50, stability=255)
    cache.resolve(card)  # hit (rule: skip)
    # all valid card types now match rules, so 0 misses on default cache
    s = cache.stats()
    ok = True
    ok &= t("has hits", s["single_hits"] >= 1)
    ok &= t("0 misses (all types covered)", s["single_misses"] == 0)
    ok &= t("100% hit rate", s["single_hit_rate"] == 100.0)
    ok &= t("has rules", s["n_rules"] > 5)
    # verify empty-rules cache produces misses
    cache0 = RouteRuleCache(rules=[])
    card2 = ZoneCard(id=0, card_type=CARD_BATCH, entropy=40, pattern=0x7F,
                     locality=60, stability=120)
    cache0.resolve(card2)
    s0 = cache0.stats()
    ok &= t("empty rules → misses", s0["single_misses"] >= 1)
    return ok


# ── New gap-filler tests ──

def test_gap_batch_low_ent_moderate_st():
    """Batch + low ent + moderate st → used to be a miss, now rule 74."""
    cache = RouteRuleCache()
    card = ZoneCard(id=1, card_type=CARD_BATCH, entropy=25, pattern=0,
                    locality=128, stability=120)
    r = cache.resolve(card)
    ok = t("batch+low ent+mod st → batch-moderate",
           r["decision"] == "batch-moderate")
    ok &= t("rule hit", r["cache"] == "rule")
    return ok


def test_gap_batch_low_ent_unstable():
    """Batch + low ent + unstable → used to be a miss, now rule 73."""
    cache = RouteRuleCache()
    card = ZoneCard(id=1, card_type=CARD_BATCH, entropy=25, pattern=0,
                    locality=128, stability=50)
    r = cache.resolve(card)
    ok = t("batch+low ent+unstable → batch-verify-fresh",
           r["decision"] == "batch-verify-fresh")
    ok &= t("rule hit", r["cache"] == "rule")
    return ok


def test_gap_sparse_low_ent_unstable():
    """Sparse + low ent + unstable → used to be a miss, now rule 81."""
    cache = RouteRuleCache()
    card = ZoneCard(id=2, card_type=CARD_SPARSE, entropy=5, pattern=0,
                    locality=128, stability=50)
    r = cache.resolve(card)
    ok = t("sparse+low ent+unstable → sparse-verify",
           r["decision"] == "sparse-verify")
    ok &= t("rule hit", r["cache"] == "rule")
    return ok


def test_entropy_cliff():
    """Any type + ent≥220 + st≤30 → crash-plan."""
    cache = RouteRuleCache()
    for ct, name in [(0, "sparse"), (1, "batch"), (2, "lz")]:
        card = ZoneCard(id=0, card_type=ct, entropy=230, pattern=0,
                        locality=128, stability=10)
        r = cache.resolve(card)
        ok = t(f"{name} entropy cliff → crash-plan", r["decision"] == "crash-plan")
        if not ok:
            return False
    return True


def test_high_locality_cascade():
    """Any type + loc≥220 → cascade-reuse."""
    cache = RouteRuleCache()
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=128, pattern=0,
                    locality=240, stability=128)
    r = cache.resolve(card)
    ok = t("high locality → cascade-reuse",
           r["decision"] == "cascade-reuse")
    ok &= t("rule hit", r["cache"] == "rule")
    return ok


def test_seq_sparse_batch_sparse():
    """(0,1,0) → skip-middle (new rule 104)."""
    cache = RouteRuleCache()
    from zone_card import ZoneSequence
    cards = [
        ZoneCard(id=1, card_type=CARD_SPARSE, entropy=5, pattern=0,
                 locality=128, stability=255),
        ZoneCard(id=2, card_type=CARD_BATCH, entropy=25, pattern=0,
                 locality=128, stability=240),
        ZoneCard(id=3, card_type=CARD_SPARSE, entropy=3, pattern=0,
                 locality=128, stability=255),
    ]
    seq = ZoneSequence(cards)
    routes = cache.resolve_sequence(seq)
    ok = t("3 routes returned", len(routes) == 3)
    ok &= t("middle is skip", routes[1]["decision"] == "skip")
    ok &= t("seq cache hit", routes[0]["cache"] == "seq_rule")
    return ok


def test_seq_skip_both():
    cache = RouteRuleCache()
    from zone_card import ZoneSequence
    cards = [
        ZoneCard(id=1, card_type=CARD_SPARSE, entropy=5, pattern=0,
                 locality=128, stability=255),
        ZoneCard(id=2, card_type=CARD_SPARSE, entropy=10, pattern=0,
                 locality=128, stability=255),
    ]
    seq = ZoneSequence(cards)
    routes = cache.resolve_sequence(seq)
    ok = t("skip-both decision",
           routes[0]["decision"] in ("skip", "skip-both"))
    ok &= t("seq cache hit", routes[0]["cache"] == "seq_rule")
    return ok


def test_seq_lz_pair():
    """(2,2) → lz-pair."""
    cache = RouteRuleCache()
    from zone_card import ZoneSequence
    cards = [
        ZoneCard(id=5, card_type=CARD_LZ, entropy=200, pattern=0,
                 locality=128, stability=200),
        ZoneCard(id=6, card_type=CARD_LZ, entropy=180, pattern=0,
                 locality=128, stability=210),
    ]
    seq = ZoneSequence(cards)
    routes = cache.resolve_sequence(seq)
    ok = t("lz-pair", routes[0]["decision"] == "lz-pair")
    ok &= t("seq cache hit", routes[0]["cache"] == "seq_rule")
    return ok


if __name__ == "__main__":
    tests = [
        ("sparse_skip",              test_hit_sparse_skip),
        ("batch_fast",               test_hit_batch_fast),
        ("batch_low_locality",       test_hit_batch_low_locality),
        ("lz_full",                  test_hit_lz_full),
        ("miss_fallback",            test_miss_fallback),
        ("exact_cache",              test_exact_cache),
        ("seq_skip_middle",          test_sequence_skip_middle),
        ("seq_all_sparse",           test_sequence_all_sparse),
        ("stats",                    test_stats),
        ("gap_batch_low_ent_mod_st", test_gap_batch_low_ent_moderate_st),
        ("gap_batch_low_ent_unst",   test_gap_batch_low_ent_unstable),
        ("gap_sparse_low_ent_unst",  test_gap_sparse_low_ent_unstable),
        ("entropy_cliff",            test_entropy_cliff),
        ("high_locality_cascade",    test_high_locality_cascade),
        ("seq_sparse_batch_sparse",  test_seq_sparse_batch_sparse),
        ("seq_skip_both",            test_seq_skip_both),
        ("seq_lz_pair",              test_seq_lz_pair),
    ]
    passed = 0
    failed = 0
    for name, fn in tests:
        print(f"\n── {name} ──")
        try:
            if fn():
                passed += 1
            else:
                failed += 1
        except Exception as e:
            print(f"  EXCEPTION: {e}")
            failed += 1
    print(f"\n{'='*40}")
    print(f"  {passed}/{passed + failed} PASSED" + ("" if failed == 0 else f"  {failed} FAILED"))
    raise SystemExit(0 if failed == 0 else 1)
