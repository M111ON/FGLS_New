from __future__ import annotations
import sys, torch, logging, numpy as np, base64
from pathlib import Path
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from typing import Optional
from .base import EngineFeature

logger = logging.getLogger("engine.features.geom_codec")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
for p in [str(_COLLECTION), str(_HERE.parent)]:
    if p not in sys.path:
        sys.path.insert(0, p)

_ROUTER_INSTANCE = None
SHAPE_LABELS = {73: 'I', 79: 'O', 84: 'T', 83: 'S', 90: 'Z', 76: 'L'}

def _get_router():
    global _ROUTER_INSTANCE
    if _ROUTER_INSTANCE is not None:
        return _ROUTER_INSTANCE
    try:
        from bermuda_router_v1 import BermudaRouter
        _ROUTER_INSTANCE = BermudaRouter(dim=128, gear=2)
        _ROUTER_INSTANCE.gate.eval()
        return _ROUTER_INSTANCE
    except Exception as e:
        logger.warning(f"Router unavailable: {e}")
        return None


class CaptureRequest(BaseModel):
    n_ticks: int = 16
    dim: int = 128
    n_tokens: int = 64
    mode: int = 0
    seed: int = 42
    highlight: str = ""


def _capture_tick(router, seed: int, mode: int, n_tokens: int, dim: int) -> dict:
    torch.manual_seed(seed)
    device = next(router.gate.parameters()).device
    x = torch.randn(n_tokens, dim, device=device)
    verdict = router.route(x, mode)
    real = verdict.real_mask
    tokens = []
    for i in range(real.sum().item()):
        tokens.append({
            'zone': int(verdict.zone[real][i].item()),
            'shape': chr(int(verdict.shape[real][i].item())),
            'polarity': 'ROUTE' if verdict.polarity[real][i].item() == 0 else 'GROUND',
            'tring_slot': int(verdict.tring_slot[real][i].item()),
        })
    return {
        'tring_slots': sorted(set(int(x) for x in verdict.tring_slot[real].tolist())),
        'sample_tokens': tokens,
        'n_real': int(real.sum().item()),
    }


class GeomCodecFeature(EngineFeature):
    name = "Geometry Codec"
    description = "Encode routing verdicts → RGB layer APNG (8×8 × 3ch) with connection tracking"
    icon = "codec"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/codec", tags=["codec"])

        @router.get("/status")
        def codec_status():
            r = _get_router()
            return {
                "router_online": r is not None,
                "frame_shape": [8, 8, 3],
                "cells": 64,
                "slots_per_cell": 720 // 64,
                "layers": {"R": "zone", "G": "shape", "B": "polarity"},
                "max_levels_per_cycle": 1440,
            }

        @router.post("/capture")
        def codec_capture(req: CaptureRequest):
            r = _get_router()
            if r is None:
                raise HTTPException(503, "Router unavailable")
            from geom_codec import encode_sequence, encode_level_gif, track_connections
            from pogls_pipeline_bridge import encode_sequence_with_header

            levels = []
            for tick in range(req.n_ticks):
                lv = _capture_tick(r, req.seed + tick, req.mode, req.n_tokens, req.dim)
                levels.append(lv)

            conns = track_connections(levels)

            if req.highlight in ("enter", "leave", "persist"):
                apng_b64 = base64.b64encode(
                    encode_level_gif(levels, fps=8, marker=req.highlight)
                ).decode()
                pipeline_info = None
            else:
                apng_bytes, header, pipeline_info = encode_sequence_with_header(levels, fps=8)
                apng_b64 = base64.b64encode(apng_bytes).decode()

            n_enter = sum(c['n_enter'] for c in conns)
            n_leave = sum(c['n_leave'] for c in conns)
            n_persist = sum(c['n_persist'] for c in conns)

            result = {
                "n_levels": len(levels),
                "apng_b64": apng_b64,
                "connections": conns,
                "total_enter": int(n_enter),
                "total_leave": int(n_leave),
                "total_persist": int(n_persist),
                "has_header_frame": req.highlight not in ("enter", "leave", "persist"),
                "levels_raw": [{
                    "occupied": lv['tring_slots'],
                    "n_tokens": lv['n_real'],
                } for lv in levels],
            }
            if pipeline_info:
                result["pipeline"] = pipeline_info
            return result

        @router.post("/video")
        def codec_video(req: CaptureRequest):
            r = _get_router()
            if r is None:
                raise HTTPException(503, "Router unavailable")
            from geom_codec import encode_level_mp4
            from fastapi.responses import Response

            levels = []
            for tick in range(req.n_ticks):
                lv = _capture_tick(r, req.seed + tick, req.mode, req.n_tokens, req.dim)
                levels.append(lv)

            mp4 = encode_level_mp4(levels, fps=8)
            return Response(content=mp4, media_type="video/mp4",
                            headers={"Content-Disposition": "inline; filename=geom_lossless.mp4"})

        @router.post("/flow")
        def codec_flow(req: CaptureRequest):
            r = _get_router()
            if r is None:
                raise HTTPException(503, "Router unavailable")
            from geom_codec import connection_flow, track_connections

            levels = []
            for tick in range(req.n_ticks):
                lv = _capture_tick(r, req.seed + tick, req.mode, req.n_tokens, req.dim)
                levels.append(lv)

            flow = connection_flow(levels)
            conns = track_connections(levels)
            nz = np.nonzero(flow)
            sparse = [{"from": int(a), "to": int(b), "count": int(flow[a, b])}
                      for a, b in zip(nz[0], nz[1])]

            return {
                "n_edges": len(sparse),
                "edges": sparse[:200],
                "total_flow": int(flow.sum()),
                "connections": conns,
            }

        app.include_router(router)
