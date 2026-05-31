from __future__ import annotations
import sys, torch, logging, numpy as np
from pathlib import Path
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from typing import Optional
from .base import EngineFeature

logger = logging.getLogger("engine.features.router")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
if str(_COLLECTION) not in sys.path:
    sys.path.insert(0, str(_COLLECTION))
if str(_HERE.parent) not in sys.path:
    sys.path.insert(0, str(_HERE.parent))

_ROUTER_INSTANCE = None
MODE_NAMES = ["ORBITAL", "CHIRAL", "CROSS", "HUB"]
SHAPE_LABELS = {73: 'I', 79: 'O', 84: 'T', 83: 'S', 90: 'Z', 76: 'L'}


class RouteRequest(BaseModel):
    dim: int = 128
    n_tokens: int = 64
    mode: int = 0
    seed: int = 42


class BatchRouteRequest(BaseModel):
    dims: list[list[float]]


def _get_router():
    global _ROUTER_INSTANCE
    if _ROUTER_INSTANCE is not None:
        return _ROUTER_INSTANCE
    try:
        from bermuda_router_v1 import BermudaRouter
        _ROUTER_INSTANCE = BermudaRouter(dim=128, gear=2)
        _ROUTER_INSTANCE.gate.eval()
        logger.info("Router initialized")
        return _ROUTER_INSTANCE
    except Exception as e:
        logger.warning(f"Router init failed: {e}")
        return None


class RouterFeature(EngineFeature):
    name = "Bermuda Router"
    description = "Route float tensors through geometry and visualize verdicts"
    icon = "router"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/router", tags=["router"])

        @router.post("/route")
        def do_route(req: RouteRequest):
            r = _get_router()
            if r is None:
                raise HTTPException(503, "Router unavailable (torch/bermuda needed)")
            device = next(r.gate.parameters()).device
            torch.manual_seed(req.seed)
            x = torch.randn(req.n_tokens, req.dim, device=device)
            verdict = r.route(x, req.mode)
            real = verdict.real_mask
            zone_dist = {}
            for z in range(12):
                cnt = int((verdict.zone[real] == z).sum().item())
                if cnt > 0:
                    zone_dist[str(z)] = cnt
            shape_dist = {}
            for byte_val, label in SHAPE_LABELS.items():
                cnt = int((verdict.shape[real] == byte_val).sum().item())
                if cnt > 0:
                    shape_dist[label] = cnt
            route_n = int((verdict.polarity[real] == 0).sum().item())
            ground_n = int((verdict.polarity[real] == 1).sum().item())
            tring_occ = verdict.tring_slot[real].tolist()
            tring_set = sorted(set(tring_occ))
            tring_freq = {}
            for s in tring_occ:
                tring_freq[s] = tring_freq.get(s, 0) + 1
            sample_tokens = []
            for i in range(min(20, real.sum().item())):
                sample_tokens.append({
                    "zone": int(verdict.zone[real][i].item()),
                    "shape": chr(int(verdict.shape[real][i].item())),
                    "polarity": "ROUTE" if verdict.polarity[real][i].item() == 0 else "GROUND",
                    "tring_slot": int(verdict.tring_slot[real][i].item()),
                })
            return {
                "mode": MODE_NAMES[req.mode],
                "mode_idx": req.mode,
                "n_tokens": verdict.n_tokens,
                "n_real": int(real.sum().item()),
                "gear": verdict.gear,
                "shadow_norm": round(verdict.shadow_norm, 4),
                "zone_distribution": zone_dist,
                "shape_distribution": shape_dist,
                "route_count": route_n,
                "ground_count": ground_n,
                "tring_occupied": len(tring_set),
                "tring_total": 720,
                "tring_slots": tring_set,
                "tring_freq": {str(k): v for k, v in tring_freq.items()},
                "sample_tokens": sample_tokens,
            }

        @router.post("/route-all")
        def route_all(req: RouteRequest):
            r = _get_router()
            if r is None:
                raise HTTPException(503, "Router unavailable")
            device = next(r.gate.parameters()).device
            torch.manual_seed(req.seed)
            x = torch.randn(req.n_tokens, req.dim, device=device)
            results = []
            for mode in range(4):
                v = r.route(x, mode)
                real = v.real_mask
                zone_dist = {}
                for z in range(12):
                    cnt = int((v.zone[real] == z).sum().item())
                    if cnt > 0:
                        zone_dist[str(z)] = cnt
                shape_dist = {}
                for byte_val, label in SHAPE_LABELS.items():
                    cnt = int((v.shape[real] == byte_val).sum().item())
                    if cnt > 0:
                        shape_dist[label] = cnt
                route_n = int((v.polarity[real] == 0).sum().item())
                ground_n = int((v.polarity[real] == 1).sum().item())
                to = v.tring_slot[real].tolist()
                results.append({
                    "mode": MODE_NAMES[mode],
                    "mode_idx": mode,
                    "n_real": int(real.sum().item()),
                    "gear": v.gear,
                    "shadow_norm": round(v.shadow_norm, 4),
                    "zone_distribution": zone_dist,
                    "shape_distribution": shape_dist,
                    "route_count": route_n,
                    "ground_count": ground_n,
                    "tring_slots": sorted(set(to)),
                    "tring_occupied": len(set(to)),
                })
            return {"results": results}

        @router.post("/classify-idx")
        def classify_idx(body: dict):
            r = _get_router()
            if r is None:
                raise HTTPException(503, "Router unavailable")
            idx = torch.tensor(body.get("indices", [0]), dtype=torch.long)
            mode = body.get("mode", 0)
            gear = body.get("gear", 2)
            v = r.classify_idx(idx, mode, gear=gear)
            entries = []
            for i in range(len(idx)):
                entries.append({
                    "idx_in": int(v.idx_in[i].item()),
                    "idx_out": int(v.idx_out[i].item()),
                    "zone": int(v.zone[i].item()),
                    "shape": chr(int(v.shape[i].item())),
                    "polarity": "ROUTE" if v.polarity[i].item() == 0 else "GROUND",
                    "tring_slot": int(v.tring_slot[i].item()),
                })
            return {"mode": MODE_NAMES[mode], "entries": entries}

        app.include_router(router)
