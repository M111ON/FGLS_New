from __future__ import annotations
import sys, json, logging
from pathlib import Path
from typing import Optional
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from .base import EngineFeature

logger = logging.getLogger("engine.features.card")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
if str(_COLLECTION) not in sys.path:
    sys.path.insert(0, str(_COLLECTION))
if str(_HERE.parent) not in sys.path:
    sys.path.insert(0, str(_HERE.parent))

_CARDS_CACHE: list[dict] = []
_CARDS_READY = False
_REGPATH = _COLLECTION / "coord_real_registry.json"


def _load_cards():
    global _CARDS_CACHE, _CARDS_READY
    if _CARDS_READY:
        return _CARDS_CACHE
    try:
        from zone_card import cards_from_registry, make_card_from_np, ZoneCard
        import numpy as np

        rp = _REGPATH
        if not rp.exists():
            logger.warning(f"Registry not found: {rp}")
            return []
        cards = cards_from_registry(str(rp))
        if cards:
            _CARDS_CACHE = [c.to_dict() for c in cards]
            _CARDS_READY = True
        else:
            _CARDS_CACHE = []
            for z in range(12):
                rng = np.random.default_rng(z + 42)
                arr = rng.standard_normal(128).astype("f4")
                c = make_card_from_np(arr, zone_id=z,
                                      neighbor=(z - 1 if z > 0 else None,
                                                z + 1 if z < 11 else None))
                _CARDS_CACHE.append(c.to_dict())
            _CARDS_READY = True
        logger.info(f"Loaded {len(_CARDS_CACHE)} zone cards (v2)")
        return _CARDS_CACHE
    except Exception as e:
        logger.warning(f"Card init failed: {e}")
        return []


def _load_sequences(window: int = 3):
    try:
        from zone_card import build_sequences_from_registry
        rp = _REGPATH
        if not rp.exists():
            return []
        seqs = build_sequences_from_registry(str(rp), window=window)
        return [s.to_summary() for s in seqs]
    except Exception as e:
        logger.warning(f"Sequence load failed: {e}")
        return []


class CardFeature(EngineFeature):
    name = "Zone Cards"
    description = "ZoneCard v2 signatures + multi-sequence routing"
    icon = "card"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/cards", tags=["cards"])

        @router.get("/list")
        def card_list():
            cards = _load_cards()
            return {"n_cards": len(cards), "cards": cards}

        @router.get("/{zone}")
        def card_detail(zone: int):
            cards = _load_cards()
            for c in cards:
                if c["id"] == zone:
                    return c
            raise HTTPException(404, f"Zone {zone} not found")

        @router.get("/summary/stats")
        def card_stats():
            cards = _load_cards()
            if not cards:
                return {"n_cards": 0}
            type_dist = {}
            ent_vals, loc_vals, st_vals = [], [], []
            for c in cards:
                tn = c["type_name"]
                type_dist[tn] = type_dist.get(tn, 0) + 1
                ent_vals.append(c["entropy"])
                loc_vals.append(c.get("locality", 128))
                st_vals.append(c.get("stability", 128))
            return {
                "n_cards": len(cards),
                "type_distribution": type_dist,
                "entropy_range": [min(ent_vals), max(ent_vals)],
                "avg_entropy": round(sum(ent_vals) / len(ent_vals), 1),
                "locality_range": [min(loc_vals), max(loc_vals)],
                "avg_locality": round(sum(loc_vals) / len(loc_vals), 1),
                "stability_range": [min(st_vals), max(st_vals)],
                "avg_stability": round(sum(st_vals) / len(st_vals), 1),
            }

        @router.post("/route-test")
        def route_test(req: dict):
            zone = req.get("zone", 0)
            cards = _load_cards()
            card = next((c for c in cards if c["id"] == zone), None)
            if not card:
                raise HTTPException(404, f"Zone {zone} not found")
            ct, ent, loc, st = (card["card_type"], card["entropy"],
                                card.get("locality", 128),
                                card.get("stability", 128))
            route = {"zone": zone, "card_type": card["type_name"],
                     "entropy": ent, "locality": loc, "stability": st}

            if ct == 0 and ent < 80:
                route.update({"decision": "skip",
                              "reason": "sparse+low-entropy"})
            elif ct == 0:
                route.update({"decision": "sparse-route",
                              "reason": "sparse weights"})
            elif ct == 1 and ent < 50 and st > 200:
                route.update({"decision": "batch-fast",
                              "reason": "uniform+stable: fast path"})
            elif ct == 1 and ent < 50:
                route.update({"decision": "batch",
                              "reason": "uniform weights"})
            elif ct == 1:
                route.update({"decision": "batch-verify",
                              "reason": "batch+verify"})
            elif ct == 2 and st < 80:
                route.update({"decision": "full-decode",
                              "reason": "unstable LZ: full decode"})
            elif ct == 2 and ent > 200:
                route.update({"decision": "full",
                              "reason": "high entropy LZ"})
            elif ct == 2:
                route.update({"decision": "lz-route",
                              "reason": "LZ decompress"})
            else:
                route.update({"decision": "fallback",
                              "reason": "unclassified"})
            return route

        @router.get("/sequences/{window}")
        def sequences(window: int = 3):
            if window < 2 or window > 6:
                raise HTTPException(400, "window 2-6")
            seqs = _load_sequences(window=window)
            return {"n_sequences": len(seqs), "sequences": seqs,
                    "window": window}

        @router.get("/route/stats")
        def route_stats():
            from route_cache import get_cache
            return get_cache().stats()

        @router.get("/route/rules")
        def route_rules():
            from route_cache import get_cache
            return {"n_rules": len(get_cache().rules_summary()),
                    "rules": get_cache().rules_summary()}

        @router.get("/engine/stats")
        def engine_stats():
            """Cache hit/miss from engine instances (accumulated)."""
            cards = _load_cards()
            return {"n_cards": len(cards), "note": "per-engine stats via /api/cards/route/stats"}

        @router.get("/sequence/suggest")
        def sequence_suggest(window: int = 3):
            try:
                from zone_card import build_sequences_from_registry
                rp = _REGPATH
                if not rp.exists():
                    raise HTTPException(404, "no registry")
                seqs = build_sequences_from_registry(str(rp), window=window)
                if not seqs:
                    return {"routes": []}
                return {
                    "routes": seqs[0].suggest_routes(),
                    "summary": seqs[0].to_summary(),
                    "llm_prompt": seqs[0].llm_prompt_block(),
                }
            except Exception as e:
                raise HTTPException(500, str(e))
