from __future__ import annotations
import sys, logging
from pathlib import Path
from typing import Optional
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from .base import EngineFeature

logger = logging.getLogger("engine.features.store")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
if str(_COLLECTION) not in sys.path:
    sys.path.insert(0, str(_COLLECTION))
if str(_HERE.parent) not in sys.path:
    sys.path.insert(0, str(_HERE.parent))

_STORE_INSTANCE = None
_SHAPES = ['I', 'O', 'T', 'S', 'Z', 'L']
_N_ZONES = 12
_NAMESPACE_SHIFT = {'Q': 12, 'K': 24, 'V': 36, 'O': 48, 'G': 60, 'U': 72, 'D': 84}


class QueryRequest(BaseModel):
    zone: int
    shape: str
    ns: Optional[str] = None


def _get_store():
    global _STORE_INSTANCE
    if _STORE_INSTANCE is not None:
        return _STORE_INSTANCE
    try:
        from geometry_store import GeometryStore
        build_dir = _COLLECTION / "build"
        candidates = list(build_dir.glob("*.gsidx"))
        if not candidates:
            return None
        store_path = str(candidates[0]).replace(".gsidx", "")
        _STORE_INSTANCE = GeometryStore(store_path, read_only=True)
        return _STORE_INSTANCE
    except Exception as e:
        logger.warning(f"Store init failed: {e}")
        return None


class StoreFeature(EngineFeature):
    name = "Geometry Store"
    description = "Browse and query the geometry-addressed weight store (.gsidx/.gsdat)"
    icon = "store"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/store", tags=["store"])

        @router.get("/stats")
        def store_stats():
            store = _get_store()
            if store is None:
                return {"status": "unavailable", "error": "No .gsidx found in build/"}
            s = store.stats()
            keys = []
            for (z, si), (off, n_rows, n_cols) in store._index.items():
                ns = ""
                raw_z = z
                for ns_name, shift in _NAMESPACE_SHIFT.items():
                    if z >= shift:
                        raw_z = z - shift
                        ns = ns_name
                keys.append({
                    "zone": raw_z, "shape": _SHAPES[si],
                    "namespace": ns, "rows": n_rows, "cols": n_cols,
                    "offset": off,
                })
            return {
                "status": "online",
                "n_keys": s["n_keys"],
                "total_rows": s["total_rows"],
                "data_kb": s["data_kb"],
                "keys_by_zone": {str(k): v for k, v in s["keys_by_zone"].items()},
                "keys": keys,
            }

        @router.get("/key-coverage")
        def key_coverage():
            store = _get_store()
            if store is None:
                return {"status": "unavailable"}
            matrix = {}
            for (z, si) in store._index:
                raw_z = z % 12
                ns_idx = z // 12
                key = f"z{raw_z}"
                if key not in matrix:
                    matrix[key] = {}
                matrix[key][_SHAPES[si]] = True
            return {"status": "online", "matrix": matrix}

        @router.post("/query")
        def store_query(req: QueryRequest):
            store = _get_store()
            if store is None:
                raise HTTPException(503, "Store unavailable")
            arr = store.query(req.zone, req.shape, ns=req.ns)
            if arr is None:
                return {"found": False}
            # Downsample for heatmap preview
            rows, cols = int(arr.shape[0]), int(arr.shape[1])
            sample_r, sample_c = min(rows, 48), min(cols, 48)
            if rows > sample_r:
                r_idx = [int(rows * i / sample_r) for i in range(sample_r)]
                sampled = arr[r_idx, :]
            else:
                sampled = arr
            if cols > sample_c:
                c_idx = [int(cols * i / sample_c) for i in range(sample_c)]
                sampled = sampled[:, c_idx]
            data_sample = [[float(v) for v in row] for row in sampled.cpu().numpy()]
            return {
                "found": True,
                "shape": [rows, cols],
                "dtype": str(arr.dtype),
                "min": float(arr.min()),
                "max": float(arr.max()),
                "mean": float(arr.mean()),
                "data_sample": data_sample,
            }

        app.include_router(router)
