"""
fusion_feature.py — Dashboard API endpoints for cross-model fusion.
"""
from fastapi import APIRouter, HTTPException
from pathlib import Path

_here = Path(__file__).resolve().parent
_regpath = _here.parent / "coord_real_registry.json"


def register(router: APIRouter):
    @router.get("/fusion/stats")
    def fusion_stats():
        from fusion import get_fusion
        return get_fusion().stats()

    @router.get("/fusion/pairs")
    def fusion_pairs():
        from fusion import get_fusion
        ft = get_fusion()
        pairs = []
        for (z, s), entries in ft._table.items():
            for owner_key, entry in entries.items():
                pairs.append({
                    "zone": z,
                    "shape": s,
                    "owner": owner_key,
                    "peer": entry.peer_key,
                    "similarity": round(entry.similarity, 3),
                    "route_decision": entry.route_decision,
                })
        return {"pairs": pairs, "count": len(pairs)}

    @router.get("/fusion/similarity-test")
    def fusion_similarity_test(a_type: int = 1, a_ent: int = 25,
                                b_type: int = 1, b_ent: int = 30):
        from zone_card import ZoneCard
        from fusion import card_similarity
        a = ZoneCard(id=0, card_type=a_type, entropy=a_ent, pattern=0,
                     locality=128, stability=240)
        b = ZoneCard(id=0, card_type=b_type, entropy=b_ent, pattern=0,
                     locality=128, stability=240)
        sim = card_similarity(a, b)
        return {
            "card_a": {"type": a_type, "entropy": a_ent},
            "card_b": {"type": b_type, "entropy": b_ent},
            "similarity": round(sim, 4),
            "above_threshold": sim >= 0.75,
            "state_share_ready": sim >= 0.90,
        }

    @router.get("/fusion/reset")
    def fusion_reset():
        from fusion import reset_fusion
        reset_fusion()
        return {"status": "reset"}
