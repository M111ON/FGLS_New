from __future__ import annotations
import sys, logging
from pathlib import Path
from fastapi import APIRouter
try:
    from .base import EngineFeature
except ImportError:
    # Standalone execution or features/ not a package yet — define minimal base
    class EngineFeature:
        name: str = ""
        description: str = ""
        icon: str = ""
        version: str = "0.1"
        enabled: bool = True
        error: str = ""

        def register_routes(self, app): pass

        def status(self) -> dict:
            return {"name": self.name, "enabled": self.enabled,
                    "description": self.description, "error": self.error}

logger = logging.getLogger("engine.features.geometry")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
if str(_COLLECTION) not in sys.path:
    sys.path.insert(0, str(_COLLECTION))
if str(_HERE.parent) not in sys.path:
    sys.path.insert(0, str(_HERE.parent))

_GEO_MODULE = _COLLECTION / "geo_jump_module" / "python"
if str(_GEO_MODULE) not in sys.path:
    sys.path.insert(0, str(_GEO_MODULE))

_GEO_CACHE = {}


def _get_router():
    if "_router" in _GEO_CACHE:
        return _GEO_CACHE["_router"]
    try:
        from geo_router import GeoRouter
        r = GeoRouter()
        _GEO_CACHE["_router"] = r
        return r
    except Exception:
        return None


def _constants():
    return {
        "GEO_BLOCK": 48,
        "GEO_TOWER": 144,
        "GEO_FULL": 20736,
        "GEO_PENTAGONS": 12,
        "GEO_FIBO_CLOCK": 1440,
        "GEO_SHELL_TICK": 12,
        "GEO_INCIRCLE": 0,
        "GEO_MIDDLE": 1,
        "GEO_BETWEEN": 2,
        "GEO_OUTSIDE": 3,
    }


def _dna_timeline(head_tick: int, layer: int) -> dict:
    """O(1) field-based layer position from head_tick."""
    c = _constants()
    if head_tick >= c["GEO_FIBO_CLOCK"] or layer >= c["GEO_SHELL_TICK"]:
        return {"error": "out of range"}
    face = head_tick // (c["GEO_FULL"] // c["GEO_PENTAGONS"])
    base = face * (c["GEO_FULL"] // c["GEO_PENTAGONS"])
    cell = (head_tick - base) % c["GEO_TOWER"]
    node = base + layer * c["GEO_TOWER"] + cell
    return {"layer": layer, "node": node, "face": face, "cell": cell}


def _container_zone(anchor_id: int, node: int) -> dict:
    """Classify a node relative to an anchor centroid."""
    c = _constants()
    anchor_id %= 24
    centroid = anchor_id * (c["GEO_FULL"] // 24) + (c["GEO_FULL"] // 48)
    inner_r, outer_r = 24, c["GEO_TOWER"]
    d = abs(centroid - node)
    if d < inner_r:
        zone, name = c["GEO_INCIRCLE"], "INCIRCLE"
    elif d < outer_r:
        zone, name = c["GEO_MIDDLE"], "MIDDLE"
    elif d < outer_r * 3:
        zone, name = c["GEO_BETWEEN"], "BETWEEN"
    else:
        zone, name = c["GEO_OUTSIDE"], "OUTSIDE"
    return {"anchor_id": anchor_id, "centroid": centroid, "node": node,
            "distance": d, "zone": zone, "zone_name": name,
            "inner_r": inner_r, "outer_r": outer_r}


FIELD_LAYOUT = {
    "faces": 12,
    "layers_per_face": 12,
    "blocks_per_layer": 3,
    "cells_per_block": {"cols": 4, "rows": 4, "floors": 3, "total": 48},
    "address_layout": "12 faces × 12 layers × 3 blocks × 48 cells = 20736",
    "constants": _constants(),
}


class GeometryCoreFeature(EngineFeature):
    name = "Geometry Core"
    description = "GeoDna timeline + CentroidContainer + field geometry (O(1))"
    icon = "geometry"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/geo", tags=["geometry"])

        @router.get("/constants")
        def geo_constants():
            return _constants()

        @router.get("/field")
        def geo_field():
            return FIELD_LAYOUT

        @router.get("/dna/timeline")
        def dna_timeline(head: int = 0, layer: int = 0):
            if layer >= 12:
                return {"error": "layer must be 0-11"}
            return _dna_timeline(head, layer)

        @router.get("/dna/timeline-all")
        def dna_timeline_all(head: int = 0):
            out = []
            for l in range(12):
                out.append(_dna_timeline(head, l))
            return {"head": head, "layers": out}

        @router.get("/container/zone")
        def container_zone(anchor_id: int = 0, node: int = 0):
            return _container_zone(anchor_id, node)

        @router.get("/container/centroid")
        def container_centroid(anchor_id: int = 0):
            c = _constants()
            anchor_id %= 24
            centroid = anchor_id * (c["GEO_FULL"] // 24) + (c["GEO_FULL"] // 48)
            return {"anchor_id": anchor_id, "centroid": centroid}

        @router.get("/capo/{node}/{key}")
        def geo_capo(node: int = 0, key: int = 1):
            result = (node + key * 144) % 20736
            return {"node": node, "key": key, "result": result}

        @router.get("/climate/{node}")
        def geo_climate(node: int = 0, anchor: int = 0):
            c = _constants()
            anchor %= 24
            centroid = anchor * (c["GEO_FULL"] // 24) + (c["GEO_FULL"] // 48)
            d = abs(centroid - node)
            if d < 24:
                zone, climate = "INCIRCLE", "TROPICAL"
            elif d < 144:
                zone, climate = "MIDDLE", "TEMPERATE"
            elif d < 432:
                zone, climate = "BETWEEN", "BOREAL"
            else:
                zone, climate = "OUTSIDE", "TUNDRA"
            return {"node": node, "anchor": anchor, "centroid": centroid,
                    "distance": d, "zone": zone, "climate": climate}

        @router.get("/hilbert/{col}/{row}/{floor}")
        def geo_hilbert(col: int = 1, row: int = 1, floor: int = 1):
            r = _get_router()
            if not r:
                return {"error": "router unavailable"}
            return {"from_node": 0, "to_node": r.jump_hilbert(0, col, row, floor)}

        @router.get("/peano/{col}/{row}/{floor}")
        def geo_peano(col: int = 1, row: int = 1, floor: int = 1):
            r = _get_router()
            if not r:
                return {"error": "router unavailable"}
            return {"from_node": 0, "to_node": r.jump_peano(0, col, row, floor)}

        @router.get("/pentagon/{node}/{layer}")
        def geo_pentagon(node: int = 0, layer: int = 0):
            r = _get_router()
            if not r:
                return {"error": "router unavailable"}
            return {"from_node": node, "to_node": r.jump_pentagon(node, layer),
                    "face": r.pentagon_id(node), "layer": layer}

        @router.get("/dodeca/adj")
        def geo_dodeca_adj(globe: int = 0, face: int = 0, edge: int = 0):
            r = _get_router()
            if not r: return {"error": "router unavailable"}
            return r.dodeca_adj(globe, face, edge)

        @router.get("/dodeca/dist")
        def geo_dodeca_dist(f0: int = 0, f1: int = 0):
            r = _get_router()
            if not r: return {"error": "router unavailable"}
            return {"distance": r.dodeca_face_dist(f0, f1)}

        @router.get("/dodeca/walk")
        def geo_dodeca_walk(face: int = 0, edge: int = 0, hops: int = 1, globe: int = 0):
            r = _get_router()
            if not r: return {"error": "router unavailable"}
            return r.dodeca_walk(face, edge, hops, globe)

        @router.get("/ring/step")
        def geo_ring_step(face: int = 0, ring: int = 0, edge: int = 0):
            r = _get_router()
            if not r: return {"error": "router unavailable"}
            return r.ring_step(face, ring, edge)

        @router.get("/ring/hex")
        def geo_ring_hex(face: int = 0, vert_slot: int = 0):
            r = _get_router()
            if not r: return {"error": "router unavailable"}
            v = r.ring_to_hex(face, vert_slot)
            f = r.hex_to_face(v, 0)
            return {"vertex": v, "faces": [r.hex_to_face(v, i) for i in range(3)]}

        @router.get("/ring/hot-path")
        def geo_ring_hot(pent_id: int = 0, layer: int = 0, globe: int = 0):
            r = _get_router()
            if not r: return {"error": "router unavailable"}
            return {"node": r.ring_hot_path(pent_id, layer, globe)}

        @router.get("/fold/live")
        def geo_fold_live(layer: int = 0, tick: int = 0):
            r = _get_router()
            if not r: return {"error": "router unavailable"}
            return {"layer": layer, "tick": tick, "live": r.shell_layer_live(layer, tick)}

        @router.get("/fold/nearest")
        def geo_fold_nearest(layer: int = 0, tick: int = 0, pent_axis: int = 0):
            r = _get_router()
            if not r: return {"error": "router unavailable"}
            return {"layer": layer, "tick": tick,
                    "nearest": r.shell_fold_nearest(layer, tick, pent_axis)}

        @router.get("/climate/init")
        def geo_climate_init(seed: int = 0, key: int = 0, modality: int = 0):
            from geo_router import _ClimateField
            cf = _ClimateField(seed, key, modality)
            return cf.to_dict()

        @router.get("/climate/demo")
        def geo_climate_demo():
            from geo_router import _ClimateField
            cf = _ClimateField(seed=1000, modality=0)
            cf.add(500, 64)
            cf.add(2000, 128)
            cf.add(3500, 32)
            base = cf.to_dict()
            cf.capo(3)
            capo = cf.to_dict()
            return {"base": base, "after_capo_3": capo}

        app.include_router(router)
