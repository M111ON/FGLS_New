"""Tests: Cross-model fusion (card similarity, FusionTable, FusedRoute, SharedState)."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python_src"))

from fusion import (
    card_similarity, get_fusion, reset_fusion,
    resolve_fused, store_fusion_entry,
    promote_to_rule, compute_fingerprint,
    fingerprint_store, fingerprint_lookup, fingerprint_stats,
    FUSION_THRESHOLD, STATE_SHARE_THRESH,
)
from zone_card import ZoneCard, CARD_SPARSE, CARD_BATCH, CARD_LZ
from route_cache import RouteRuleCache


def t(desc, ok):
    print(f"  {'PASS' if ok else 'FAIL'} {desc}")
    return ok


def setup():
    reset_fusion()


def test_identical_cards():
    setup()
    a = ZoneCard(id=3, card_type=CARD_BATCH, entropy=50, pattern=0xABCD,
                 locality=200, stability=240)
    b = ZoneCard(id=3, card_type=CARD_BATCH, entropy=50, pattern=0xABCD,
                 locality=200, stability=240)
    sim = card_similarity(a, b)
    ok = t("identical cards -> 1.0", abs(sim - 1.0) < 0.01)
    return ok


def test_same_type_different_entropy():
    setup()
    a = ZoneCard(id=1, card_type=CARD_BATCH, entropy=25, pattern=0,
                 locality=128, stability=200)
    b = ZoneCard(id=2, card_type=CARD_BATCH, entropy=75, pattern=0,
                 locality=128, stability=200)
    sim = card_similarity(a, b)
    ok = t("same type+st+loc, ent diff 50", 0.5 < sim < 0.95)
    return ok


def test_different_type():
    setup()
    a = ZoneCard(id=0, card_type=CARD_SPARSE, entropy=5, pattern=0,
                 locality=128, stability=255)
    b = ZoneCard(id=0, card_type=CARD_BATCH, entropy=25, pattern=0,
                 locality=128, stability=255)
    sim = card_similarity(a, b)
    ok = t("different type -> < 0.8", sim < 0.8)
    return ok


def test_fusion_table_store_probe():
    setup()
    ft = get_fusion()
    card_a = ZoneCard(id=0, card_type=CARD_BATCH, entropy=25, pattern=0,
                      locality=128, stability=240)
    card_b = ZoneCard(id=0, card_type=CARD_BATCH, entropy=30, pattern=0,
                      locality=128, stability=235)
    store_fusion_entry(0, "I", "model_a", card_a, "model_b", card_b)
    s = ft.stats()
    ok = t("2 pairs stored (bidirectional)", s["n_pairs"] == 2)

    result = ft.probe(0, "I", "model_b", card_b)
    ok &= t("probe finds peer", result is not None)
    if result:
        ok &= t("peer key is model_a", result.peer_key == "model_a")
        ok &= t("similarity >= threshold", result.similarity >= FUSION_THRESHOLD)
    return ok


def test_fusion_table_below_threshold():
    setup()
    ft = get_fusion()
    card_a = ZoneCard(id=0, card_type=CARD_SPARSE, entropy=5, pattern=0,
                      locality=255, stability=255)
    card_b = ZoneCard(id=0, card_type=CARD_LZ, entropy=200, pattern=0xFFFF,
                      locality=30, stability=30)
    store_fusion_entry(0, "I", "model_a", card_a, "model_b", card_b)
    s = ft.stats()
    ok = t("dissimilar cards -> 0 pairs", s["n_pairs"] == 0)
    return ok


def test_fusion_table_stats():
    setup()
    ft = get_fusion()
    card_a = ZoneCard(id=0, card_type=CARD_BATCH, entropy=25, pattern=0,
                      locality=128, stability=240)
    card_b = ZoneCard(id=0, card_type=CARD_BATCH, entropy=30, pattern=0,
                      locality=128, stability=235)
    store_fusion_entry(0, "I", "model_a", card_a, "model_b", card_b)
    ft.probe(0, "I", "model_b", card_b)  # hit
    ft.probe(1, "I", "model_b", card_b)  # miss
    s = ft.stats()
    ok = t("stats n_pairs == 2", s["n_pairs"] == 2)
    ok &= t("stats probes >= 2", s["probes"] >= 2)
    ok &= t("stats hits >= 1", s["hits"] >= 1)
    ok &= t("hit rate > 0", s["hit_rate"] > 0)
    return ok


def test_fusion_best_peer():
    setup()
    ft = get_fusion()
    card_a = ZoneCard(id=0, card_type=CARD_BATCH, entropy=25, pattern=0,
                      locality=128, stability=240)
    card_b = ZoneCard(id=0, card_type=CARD_BATCH, entropy=30, pattern=0,
                      locality=128, stability=235)
    card_c = ZoneCard(id=0, card_type=CARD_BATCH, entropy=90, pattern=0xFF,
                      locality=50, stability=100)
    store_fusion_entry(0, "I", "model_a", card_a, "model_b", card_b)
    store_fusion_entry(0, "I", "model_a", card_a, "model_c", card_c)
    best = ft.best_peer(0, "I", "model_a")
    ok = t("best peer found", best is not None)
    if best:
        ok &= t("best peer is model_b", best[0] == "model_b")
    return ok


def test_resolve_fused_miss():
    setup()
    cache = RouteRuleCache(rules=[])
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=40, pattern=0x7F,
                    locality=60, stability=120, hash_val=0xCAFE)
    ft = get_fusion()
    peer_card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=42, pattern=0x7F,
                         locality=60, stability=120)
    store_fusion_entry(0, "I", "model_b", peer_card, "model_a", card)
    # Set route decision on entries
    for v in ft._table.get((0, "I"), {}).values():
        v.route_decision = "batch-moderate"
        v.route_action = "batch_route_fresh"

    r = resolve_fused(cache, card, 0, "I", "model_a")
    ok = t("fused decision returned", r["cache"] == "fusion")
    ok &= t("borrowed decision", r["decision"] == "batch-moderate")
    return ok


def test_resolve_fused_hit():
    setup()
    cache = RouteRuleCache()
    card = ZoneCard(id=3, card_type=CARD_SPARSE, entropy=5, pattern=0,
                    locality=50, stability=255)
    r = resolve_fused(cache, card, 3, "S", "model_a")
    ok = t("own cache hit -> rule cache", r["cache"] == "rule")
    return ok


def test_global_singleton():
    setup()
    ft1 = get_fusion()
    ft2 = get_fusion()
    ok = t("singleton returns same instance", ft1 is ft2)
    return ok


def test_card_proxy_fusion():
    """Fusion works with card dicts (not just ZoneCard objects)."""
    setup()
    ft = get_fusion()
    from geometry_model_pool import _dict_to_card_proxy
    proxy_a = _dict_to_card_proxy({
        "card_type": 1, "entropy": 25, "locality": 128,
        "stability": 240, "hash_val": 0,
    })
    card_b = ZoneCard(id=0, card_type=CARD_BATCH, entropy=30, pattern=0,
                      locality=128, stability=235)
    store_fusion_entry(0, "I", "model_a", proxy_a, "model_b", card_b)
    s = ft.stats()
    ok = t("proxy card similarity stored", s["n_pairs"] > 0)
    return ok


# ── Promotion tests ──

def test_promote_creates_rule():
    setup()
    from zone_card import ZoneCard, CARD_BATCH
    from route_cache import get_cache
    cache = get_cache()
    n_before = len(cache.rules_summary())
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0,
                    locality=128, stability=240)
    rule = promote_to_rule(card, decision="batch", action="batch_route",
                           similarity=0.92)
    n_after = len(cache.rules_summary())
    ok = t("rule added to cache", n_after > n_before)
    ok &= t("rule has correct decision", rule.decision == "batch")
    ok &= t("rule has promote reason",
            "promoted" in rule.reason and "0.92" in rule.reason)
    return ok


def test_promoted_rule_hits():
    """After promotion, similar cards hit the rule directly."""
    setup()
    from zone_card import ZoneCard, CARD_BATCH
    from route_cache import get_cache, RouteRuleCache

    # Create a fresh cache with promotion
    cache = RouteRuleCache()
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0,
                    locality=128, stability=240)
    promote_to_rule(card, decision="batch", action="batch_route",
                    similarity=0.92)
    # Same card should hit the promoted rule
    r = cache.resolve(card)
    ok = t("promoted rule hit", r["cache"] == "rule")
    ok &= t("promoted decision", r["decision"] == "batch")
    # Similar card (entropy 55, within ±15 bucket) should also hit
    card2 = ZoneCard(id=0, card_type=CARD_BATCH, entropy=55, pattern=0,
                     locality=128, stability=240)
    r2 = cache.resolve(card2)
    ok &= t("similar card also hits", r2["cache"] == "rule")
    return ok


# ── Fingerprint tests ──

def test_compute_fingerprint():
    import numpy as np
    arr = np.random.randn(100, 100).astype(np.float32)
    fp1 = compute_fingerprint(arr)
    fp2 = compute_fingerprint(arr.copy())
    ok = t("deterministic: same → same", fp1 == fp2)
    arr2 = np.random.randn(100, 100).astype(np.float32)
    fp3 = compute_fingerprint(arr2)
    ok &= t("different → different", fp1 != fp3)
    return ok


def test_fingerprint_store_lookup():
    import numpy as np
    reset_fusion()
    arr = np.random.randn(50, 50).astype(np.float32)
    fp = compute_fingerprint(arr)
    fingerprint_store(fp, arr, "test_model")
    cached = fingerprint_lookup(fp)
    ok = t("lookup finds entry", cached is not None)
    if cached:
        ok &= t("same model_key", cached["model_key"] == "test_model")
        ok &= t("same weights", cached["weights"] is arr)
    ok &= t("wrong fp returns None",
            fingerprint_lookup(0xDEADBEEF) is None)
    s = fingerprint_stats()
    ok &= t("stats has 1 entry", s["n_entries"] >= 1)
    return ok


def test_resolve_fused_promotes():
    """resolve_fused auto-promotes when sim ≥ 0.90."""
    setup()
    from zone_card import ZoneCard, CARD_BATCH
    from route_cache import RouteRuleCache, get_cache

    # Use a fresh local cache for isolation
    local_cache = RouteRuleCache(rules=[])
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=40, pattern=0x7F,
                    locality=60, stability=120, hash_val=0xCAFE)
    peer_card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=42, pattern=0x7F,
                         locality=60, stability=120)
    store_fusion_entry(0, "I", "model_b", peer_card, "model_a", card)
    ft = get_fusion()
    for v in ft._table.get((0, "I"), {}).values():
        v.route_decision = "batch-moderate"
        v.route_action = "batch_route_fresh"

    r = resolve_fused(local_cache, card, 0, "I", "model_a")
    ok = t("fusion returns decision", r["cache"] == "fusion")
    # After promotion, check the global cache (promote_to_rule adds to global)
    gc = get_cache()
    r2 = gc.resolve(card)
    ok &= t("promoted → cache hit (exact or rule)", r2["cache"] in ("exact", "rule"))
    ok &= t("same decision after promotion", r2["decision"] == "batch-moderate")
    return ok


if __name__ == "__main__":
    tests = [
        ("identical_cards",       test_identical_cards),
        ("same_type_diff_ent",    test_same_type_different_entropy),
        ("different_type",        test_different_type),
        ("fusion_store_probe",    test_fusion_table_store_probe),
        ("fusion_below_threshold", test_fusion_table_below_threshold),
        ("fusion_stats",          test_fusion_table_stats),
        ("fusion_best_peer",      test_fusion_best_peer),
        ("resolve_fused_miss",    test_resolve_fused_miss),
        ("resolve_fused_hit",     test_resolve_fused_hit),
        ("global_singleton",      test_global_singleton),
        ("card_proxy_fusion",     test_card_proxy_fusion),
        ("promote_creates_rule",  test_promote_creates_rule),
        ("promoted_rule_hits",    test_promoted_rule_hits),
        ("compute_fingerprint",   test_compute_fingerprint),
        ("fingerprint_store_loop", test_fingerprint_store_lookup),
        ("resolve_fused_promotes", test_resolve_fused_promotes),
    ]
    passed = 0
    failed = 0
    for name, fn in tests:
        print(f"\n-- {name} --")
        try:
            if fn():
                passed += 1
            else:
                failed += 1
        except Exception as e:
            import traceback
            traceback.print_exc()
            failed += 1
    print(f"\n{'='*40}")
    print(f"  {passed}/{passed+failed} PASSED" + ("" if failed == 0 else f"  {failed} FAILED"))
