from __future__ import annotations
import sys, logging
from pathlib import Path
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from typing import Optional
from .base import EngineFeature

logger = logging.getLogger("engine.features.pipeline")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
if str(_COLLECTION) not in sys.path:
    sys.path.insert(0, str(_COLLECTION))
if str(_HERE.parent) not in sys.path:
    sys.path.insert(0, str(_HERE.parent))

_BRIDGE_INSTANCE = None


def _get_bridge():
    global _BRIDGE_INSTANCE
    if _BRIDGE_INSTANCE is not None:
        return _BRIDGE_INSTANCE
    try:
        from bermuda_bridge import BermudaBridge
        _BRIDGE_INSTANCE = BermudaBridge()
        _BRIDGE_INSTANCE.init()
        logger.info("C bridge initialized")
        return _BRIDGE_INSTANCE
    except Exception as e:
        logger.warning(f"Bridge init failed: {e}")
        return None


class BatchRouteRequest(BaseModel):
    indices: list[int]
    gear: int = 2
    mode: int = 0


class PipelineFeature(EngineFeature):
    name = "C Pipeline"
    description = "POGLS Bermuda C bridge — DLL, Hilbert, traverse, batch routing"
    icon = "pipeline"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/pipeline", tags=["pipeline"])

        @router.get("/status")
        def pipeline_status():
            bridge = _get_bridge()
            if bridge is None:
                return {"status": "unavailable", "error": "pogls_bermuda.dll not found or init failed"}
            try:
                bijection = {}
                for g in [1, 2, 3, 4]:
                    try:
                        bijection[str(g)] = bridge.verify_bijection(g)
                    except Exception:
                        bijection[str(g)] = False
                return {
                    "status": "online",
                    "dll_loaded": True,
                    "bijection": bijection,
                }
            except Exception as e:
                return {"status": "error", "error": str(e)}

        @router.post("/batch-route")
        def batch_route(req: BatchRouteRequest):
            bridge = _get_bridge()
            if bridge is None:
                raise HTTPException(503, "Bridge unavailable")
            try:
                entries = bridge.route_batch(req.indices, req.gear, req.mode)
                return {
                    "n": len(entries),
                    "entries": [
                        {
                            "idx_in": e.idx_in,
                            "idx_out": e.idx_out,
                            "zone": e.zone,
                            "shape": chr(e.shape),
                            "polarity": "ROUTE" if e.polarity == 0 else "GROUND",
                            "tring_slot": e.tring_slot,
                        }
                        for e in entries
                    ],
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.get("/hilbert-test")
        def hilbert_test():
            bridge = _get_bridge()
            if bridge is None:
                raise HTTPException(503, "Bridge unavailable")
            results = []
            for gear in [1, 2]:
                for pos in range(10):
                    enc = bridge.hilbert_encode(pos, gear)
                    dec = bridge.hilbert_decode(enc, gear)
                    results.append({
                        "gear": gear,
                        "position": pos,
                        "encoded": enc,
                        "decoded": dec,
                        "match": dec == pos,
                    })
            return {"results": results}

        @router.get("/traverse-test")
        def traverse_test():
            bridge = _get_bridge()
            if bridge is None:
                raise HTTPException(503, "Bridge unavailable")
            modes = ["ORBITAL", "CHIRAL", "CROSS", "HUB"]
            results = []
            for mode in range(4):
                entries = []
                for idx in [0, 50, 100, 200, 500]:
                    out = bridge.traverse(idx, 2, mode)
                    z = bridge.zone(idx, 2)
                    entries.append({
                        "idx_in": idx, "idx_out": out,
                        "zone": z,
                    })
                results.append({"mode": modes[mode], "entries": entries})
            return {"results": results}

        app.include_router(router)
