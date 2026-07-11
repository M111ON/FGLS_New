"""Tests: Global Execution Cache (state_hash O(1) reuse)."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python_src"))

import numpy as np

from fusion import compute_fingerprint
from zone_card import ZoneCard, CARD_BATCH, CARD_SPARSE
from global_exec_cache import (
    get_exec_cache, reset_exec_cache, HOT_THRESHOLD,
)

passed = 0
failed = 0


def t(msg, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"  FAIL {msg}")
    return cond


def setup():
    reset_exec_cache()


# ── Core cache ops ──

def test_miss():
    setup()
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0x7F,
                    locality=128, stability=200)
    state_hash = 0x12345678
    cached = get_exec_cache().lookup(card, state_hash)
    return t("miss returns None", cached is None)


def test_store_and_hit():
    setup()
    gc = get_exec_cache()
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0x7F,
                    locality=128, stability=200)
    state_hash = 0x12345678
    out = np.array([1.0, 2.0, 3.0], dtype=np.float32)
    gc.store(card, state_hash, out)
    cached = gc.lookup(card, state_hash)
    ok = t("hit returns array", cached is not None)
    if cached is not None:
        ok &= t("same values", np.allclose(cached, out))
    return ok


def test_different_hash_miss():
    setup()
    gc = get_exec_cache()
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0x7F,
                    locality=128, stability=200)
    out = np.array([1.0, 2.0, 3.0], dtype=np.float32)
    gc.store(card, 0xAAAA, out)
    cached = gc.lookup(card, 0xBBBB)
    return t("different hash → miss", cached is None)


def test_different_entropy_bucket():
    setup()
    gc = get_exec_cache()
    # entropy 50 → bucket 3
    card_a = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0x7F,
                      locality=128, stability=200)
    # entropy 100 → bucket 6
    card_b = ZoneCard(id=0, card_type=CARD_BATCH, entropy=100, pattern=0x7F,
                      locality=128, stability=200)
    out = np.array([1.0, 2.0, 3.0], dtype=np.float32)
    gc.store(card_a, 0x1234, out)
    cached = gc.lookup(card_b, 0x1234)
    return t("different ent bucket → miss", cached is None)


def test_different_pattern():
    setup()
    gc = get_exec_cache()
    card_a = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0x7F,
                      locality=128, stability=200)
    card_b = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0xAA,
                      locality=128, stability=200)
    out = np.array([1.0, 2.0, 3.0], dtype=np.float32)
    gc.store(card_a, 0x1234, out)
    cached = gc.lookup(card_b, 0x1234)
    return t("different pattern → miss", cached is None)


def test_ensure_no_overwrite():
    setup()
    gc = get_exec_cache()
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0x7F,
                    locality=128, stability=200)
    out_a = np.array([1.0], dtype=np.float32)
    out_b = np.array([9.9], dtype=np.float32)
    gc.ensure(card, 0x1234, out_a)
    gc.ensure(card, 0x1234, out_b)
    cached = gc.lookup(card, 0x1234)
    ok = t("ensure returns first value", cached is not None)
    if cached is not None:
        ok &= t("ensure no overwrite", cached[0] == 1.0)
    return ok


# ── Hot path promotion ──

def test_promote_to_hot():
    setup()
    gc = get_exec_cache()
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0x7F,
                    locality=128, stability=200)
    out = np.array([1.0], dtype=np.float32)
    gc.store(card, 0x1000, out)
    ok = t("not hot after store", not gc.is_hot(card, 0x1000))
    # Hit 3 times (HOT_THRESHOLD)
    for i in range(HOT_THRESHOLD):
        gc.lookup(card, 0x1000)
        if i < HOT_THRESHOLD - 1:
            ok &= t(f"not hot after {i+1} lookups", not gc.is_hot(card, 0x1000))
    ok &= t(f"hot after {HOT_THRESHOLD} lookups", gc.is_hot(card, 0x1000))
    return ok


def test_hot_in_stats():
    setup()
    gc = get_exec_cache()
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0x7F,
                    locality=128, stability=200)
    out = np.array([1.0], dtype=np.float32)
    gc.store(card, 0x2000, out)
    for _ in range(HOT_THRESHOLD):
        gc.lookup(card, 0x2000)
    s = gc.stats()
    ok = t("hot_keys >= 1", s["hot_keys"] >= 1)
    ok &= t("hit_rate > 0", s["hit_rate"] > 0)
    ok &= t("size >= 1", s["size"] >= 1)
    return ok


# ── Compute fingerprint integration ──

def test_compute_fingerprint_integration():
    setup()
    arr = np.random.randn(64, 64).astype(np.float32)
    fp = compute_fingerprint(arr)
    ok = t("fingerprint is int", isinstance(fp, int))
    ok &= t("fingerprint non-zero", fp != 0)
    # Same key as exec cache expects
    card = ZoneCard(id=0, card_type=CARD_BATCH, entropy=50, pattern=0x7F,
                    locality=128, stability=200)
    gc = get_exec_cache()
    gc.store(card, fp, arr)
    cached = gc.lookup(card, fp)
    ok &= t("fingerprint-keyed lookup works", cached is not None)
    return ok


def test_sparse_card_type():
    setup()
    gc = get_exec_cache()
    card = ZoneCard(id=0, card_type=CARD_SPARSE, entropy=10, pattern=0x00,
                    locality=30, stability=200)
    out = np.array([5.0, 6.0], dtype=np.float32)
    gc.store(card, 0xABCD, out)
    cached = gc.lookup(card, 0xABCD)
    ok = t("sparse card stores/retrieves", cached is not None)
    # Different pattern but same ent bucket + hash → miss
    card2 = ZoneCard(id=0, card_type=CARD_SPARSE, entropy=10, pattern=0xFF,
                     locality=30, stability=200)
    cached2 = gc.lookup(card2, 0xABCD)
    ok &= t("sparse diff pattern → miss", cached2 is None)
    return ok


if __name__ == "__main__":
    tests = [
        ("miss", test_miss),
        ("store_and_hit", test_store_and_hit),
        ("different_hash_miss", test_different_hash_miss),
        ("different_entropy_bucket", test_different_entropy_bucket),
        ("different_pattern", test_different_pattern),
        ("ensure_no_overwrite", test_ensure_no_overwrite),
        ("promote_to_hot", test_promote_to_hot),
        ("hot_in_stats", test_hot_in_stats),
        ("fingerprint_integration", test_compute_fingerprint_integration),
        ("sparse_card_type", test_sparse_card_type),
    ]

    for name, fn in tests:
        print(f"\n-- {name} --")
        try:
            fn()
        except Exception as e:
            import traceback
            traceback.print_exc()
            failed += 1
    print(f"\n{'='*40}")
    print(f"  {passed}/{passed+failed} PASSED" + ("" if failed == 0 else f"  {failed} FAILED"))
