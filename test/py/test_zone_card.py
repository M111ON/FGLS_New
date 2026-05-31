"""Tests: ZoneCard v2 (locality, stability, sequences)."""
import sys, json, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python_src"))

import numpy as np
from zone_card import (
    ZoneCard, make_card, make_card_from_np, card_from_store,
    cards_from_registry, build_sequences_from_registry, ZoneSequence,
    CARD_SPARSE, CARD_BATCH, CARD_LZ,
    CARD_PACK_SZ, CARD_EXT_SZ, CARD_V1_SZ,
)
from geometry_store import GeometryStore


def t(desc: str, ok: bool):
    print(f"  {'PASS' if ok else 'FAIL'} {desc}")
    return ok


def test_make_card_v2():
    """ZoneCard v2: locality + stability present."""
    rng = np.random.default_rng(42)
    arr = rng.standard_normal(5120).astype("f4")
    card = make_card(arr.tobytes(), zone_id=3, neighbor=(2, 4))
    ok = True
    ok &= t("zone id", card.id == 3)
    ok &= t("locality 0-255", 0 <= card.locality <= 255)
    ok &= t("stability 0-255", 0 <= card.stability <= 255)
    ok &= t("entropy 0-255", 0 <= card.entropy <= 255)
    ok &= t("hash non-zero", card.hash_val != 0)
    return ok


def test_stability():
    """Stability: same rows=255, random rows=low."""
    # All identical rows → max stability
    same = np.tile(np.linspace(-1, 1, 128), (8, 1)).astype("f4")
    card = make_card_from_np(same, zone_id=0)
    ok = t("identical rows → stability=255", card.stability >= 240)

    # Chaotic rows → low stability
    rng = np.random.default_rng(99)
    chaos = rng.standard_normal((16, 128)).astype("f4")
    card2 = make_card_from_np(chaos, zone_id=1)
    ok &= t("random rows → stability<200", card2.stability < 200)

    # All zeros → max stability
    zeros = np.zeros((8, 128), dtype="f4")
    card3 = make_card_from_np(zeros, zone_id=2)
    ok &= t("all zeros → stability=255", card3.stability == 255)
    return ok


def test_locality():
    """Locality: same neighbors=high, different=low."""
    rng = np.random.default_rng(42)
    base = rng.standard_normal(1280).astype("f4")
    # Same data → high locality
    card_same = make_card_from_np(base, zone_id=0, neighbor_arr=base)
    ok = t("identical neighbor → locality>200", card_same.locality > 200)
    # Different data → lower locality
    other = rng.standard_normal(1280).astype("f4") * 100
    card_diff = make_card_from_np(base, zone_id=0, neighbor_arr=other)
    ok &= t("different neighbor → locality<200", card_diff.locality < 200)
    return ok


def test_packed_format_v2():
    """V2 packed format (12B/20B) round-trip."""
    card = ZoneCard(id=8, card_type=1, entropy=127, pattern=0xA13F,
                    locality=200, stability=50, neighbor=(7, 9),
                    hash_val=0xDEADBEEFCAFE)
    ok = True
    packed = card.to_bytes(extended=False)
    ok &= t("v2 pack size 12B", len(packed) == CARD_PACK_SZ)
    u = ZoneCard.from_bytes(packed)
    ok &= t("id rt", u.id == 8)
    ok &= t("locality rt", u.locality == 200)
    ok &= t("stability rt", u.stability == 50)
    ok &= t("pattern rt", u.pattern == 0xA13F)
    ok &= t("neighbor rt", u.neighbor == (7, 9))
    ext = card.to_bytes(extended=True)
    ok &= t("v2 ext size 20B", len(ext) == CARD_EXT_SZ)
    u2 = ZoneCard.from_bytes(ext)
    ok &= t("ext hash rt", u2.hash_val == 0xDEADBEEFCAFE)
    ok &= t("ext locality rt", u2.locality == 200)
    return ok


def test_backward_compat_v1():
    """Can read old v1 (10B) format."""
    old_packed = struct.pack('<HBBHHH', 3, 0, 100, 0x1F, 2, 4)
    card = ZoneCard.from_bytes(old_packed)
    ok = True
    ok &= t("v1 id", card.id == 3)
    ok &= t("v1 type", card.card_type == 0)
    ok &= t("v1 entropy", card.entropy == 100)
    ok &= t("v1 locality default 128", card.locality == 128)
    ok &= t("v1 stability default 128", card.stability == 128)
    return ok


def test_to_dict_v2():
    """to_dict includes new fields."""
    card = ZoneCard(id=2, card_type=0, entropy=64, pattern=0x1F,
                    locality=180, stability=200, neighbor=(1, 3))
    d = card.to_dict()
    ok = True
    ok &= t("dict has locality", d["locality"] == 180)
    ok &= t("dict has stability", d["stability"] == 200)
    ok &= t("dict has pattern hex", d["pattern"] == "0x001F")
    return ok


def test_zone_sequence():
    """ZoneSequence: multi-card reasoning."""
    cards = [
        ZoneCard(id=2, card_type=1, entropy=25, pattern=0x01,
                 locality=220, stability=240, neighbor=(1, 3)),
        ZoneCard(id=3, card_type=0, entropy=5, pattern=0x00,
                 locality=30, stability=255, neighbor=(2, 4)),
        ZoneCard(id=4, card_type=1, entropy=30, pattern=0x01,
                 locality=210, stability=235, neighbor=(3, 5)),
    ]
    seq = ZoneSequence(cards)
    ok = True
    ok &= t("n zones", seq.n == 3)
    ok &= t("type pattern", seq.type_pattern() == "batch→sparse→batch")
    ok &= t("entropy trend valid",
            seq.entropy_trend() in ("up", "down", "flat", "mixed"))
    ok &= t("stability trend valid",
            seq.stability_trend() in ("stable", "erratic", "increasing", "decreasing"))
    ok &= t("locality bonds", len(seq.locality_bonds()) == 2)
    ok &= t("to_summary has keys", "type_pattern" in seq.to_summary())
    return ok


def test_zone_sequence_suggest():
    """ZoneSequence.suggest_routes() detects skip patterns."""
    cards = [
        ZoneCard(id=1, card_type=0, entropy=10, pattern=0,
                 locality=50, stability=200, neighbor=(None, 2)),
        ZoneCard(id=2, card_type=0, entropy=5, pattern=0,
                 locality=50, stability=255, neighbor=(1, 3)),
        ZoneCard(id=3, card_type=0, entropy=8, pattern=0,
                 locality=50, stability=200, neighbor=(2, None)),
    ]
    seq = ZoneSequence(cards)
    routes = seq.suggest_routes()
    ok = True
    ok &= t("suggest returns 3 routes", len(routes) == 3)
    ok &= t("middle zone skipped",
            any(r["decision"] == "skip" for r in routes))
    return ok


def test_build_sequences():
    """build_sequences_from_registry with temp store."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        store_path = str(tmpd / "test_geom")
        rng = np.random.default_rng(42)
        with GeometryStore(store_path) as store:
            store.index(2, "S", rng.standard_normal((16, 128)).astype("f4"))
            store.index(3, "I", rng.standard_normal((8, 128)).astype("f4"))
            store.index(4, "O", rng.standard_normal((12, 128)).astype("f4"))
            store.flush()
        registry_path = str(tmpd / "reg.json")
        with open(registry_path, "w") as f:
            json.dump({
                "max_open": 1,
                "models": {"test": {"store_path": str(tmpd / "test_geom"),
                                    "dim": 128, "force_cpu": True}},
                "coords": [
                    {"zone": 2, "shape": "S", "model_key": "test"},
                    {"zone": 3, "shape": "I", "model_key": "test"},
                    {"zone": 4, "shape": "O", "model_key": "test"},
                ]
            }, f)
        seqs = build_sequences_from_registry(registry_path, window=3)
        ok = True
        ok &= t("builds sequence", len(seqs) == 1)
        if seqs:
            s = seqs[0]
            ok &= t("3 zones in seq", s.n == 3)
            ok &= t("type pattern valid", "→" in s.type_pattern())
            ok &= t("llm prompt block", len(s.llm_prompt_block()) > 20)
            ok &= t("suggest routes", len(s.suggest_routes()) == 3)
    return ok


def test_llm_prompt():
    """LLM prompt block is readable."""
    cards = [
        ZoneCard(id=2, card_type=1, entropy=25, pattern=0x01,
                 locality=220, stability=240, neighbor=(1, 3)),
        ZoneCard(id=3, card_type=0, entropy=5, pattern=0x00,
                 locality=30, stability=255, neighbor=(2, 4)),
    ]
    seq = ZoneSequence(cards)
    block = seq.llm_prompt_block()
    ok = True
    ok &= t("prompt has zone info", "z 2" in block or "z2" in block)
    ok &= t("prompt has pattern", "pattern" in block)
    ok &= t("prompt has entropy", "entropy" in block)
    print(f"\n  Example LLM prompt:\n{block}")
    return ok


import struct  # for backward compat test

if __name__ == "__main__":
    tests = [
        ("make_card_v2",        test_make_card_v2),
        ("stability",           test_stability),
        ("locality",            test_locality),
        ("packed_format_v2",    test_packed_format_v2),
        ("backward_compat_v1",  test_backward_compat_v1),
        ("to_dict_v2",          test_to_dict_v2),
        ("zone_sequence",       test_zone_sequence),
        ("zone_sequence_suggest", test_zone_sequence_suggest),
        ("build_sequences",     test_build_sequences),
        ("llm_prompt",          test_llm_prompt),
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
