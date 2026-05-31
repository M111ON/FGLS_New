"""
geometry_model_pool.py — Lazy multi-model geometry runtime
==========================================================

Keeps multiple geometry-addressed models reachable by model key while
keeping RAM bounded. Each model is opened on demand and can be primed
independently, so unused models stay cold.
"""

from __future__ import annotations

import threading
from collections import OrderedDict
from dataclasses import dataclass
import sys
from pathlib import Path
from typing import Any, Dict, Optional

import numpy as np

from zero_warmup_engine import ZeroWarmupEngine


@dataclass(frozen=True)
class ModelSpec:
    store_path: str
    gguf_path: Optional[str] = None
    dim: int = 128
    gear: int = 2
    code_dim: int = 32
    gate_path: Optional[str] = None
    meta_path: Optional[str] = None
    force_cpu: bool = False
    fusion_group: str = ""  # cross-model fusion group label


@dataclass(frozen=True)
class GeometryCoord:
    zone: int
    shape: str
    ns: Optional[str] = None


class GeometryModelPool:
    """LRU pool of lazily-warmed geometry models."""

    def __init__(self, max_open: int = 2):
        self.max_open = max(1, int(max_open))
        self._specs: Dict[str, ModelSpec] = {}
        self._coords: Dict[tuple, str] = {}
        self._engines: "OrderedDict[str, ZeroWarmupEngine]" = OrderedDict()
        self._lock = threading.RLock()
        self._fusion_enabled = True

    def register(self, model_key: str, spec: ModelSpec) -> None:
        if not model_key:
            raise ValueError("model_key is required")
        idx_path = Path(spec.store_path + ".gsidx")
        if not idx_path.exists():
            raise FileNotFoundError(f"missing store index: {spec.store_path}.gsidx")
        # Quick format check: peek first 8 bytes for GSIDX magic
        try:
            with open(idx_path, "rb") as f:
                magic = f.read(8)
            from geometry_store import INDEX_MAGIC
            if magic == INDEX_MAGIC:
                pass  # binary format, ok
            else:
                # JSON format — try to parse
                import json
                with open(idx_path, "r", encoding="utf-8") as f:
                    json.load(f)
        except Exception as e:
            raise ValueError(f"store index {idx_path} has unrecognized format: {e}")
        with self._lock:
            self._specs[model_key] = spec

    def register_coord(self, coord: GeometryCoord, model_key: str) -> None:
        key = (int(coord.zone), str(coord.shape), coord.ns or "")
        with self._lock:
            if model_key not in self._specs:
                raise KeyError(f"unknown model key: {model_key}")
            self._coords[key] = model_key

    def resolve_coord(self, coord: GeometryCoord) -> str:
        key = (int(coord.zone), str(coord.shape), coord.ns or "")
        with self._lock:
            model_key = self._coords.get(key)
            if model_key is None:
                raise KeyError(f"unmapped coordinate: {key}")
            return model_key

    def _evict_if_needed(self) -> None:
        while len(self._engines) > self.max_open:
            _, engine = self._engines.popitem(last=False)
            engine.close()

    def _touch(self, model_key: str, engine: ZeroWarmupEngine) -> ZeroWarmupEngine:
        self._engines[model_key] = engine
        self._engines.move_to_end(model_key)
        self._evict_if_needed()
        return engine

    def get(self, model_key: str, warm: bool = False, promote_gpu: bool = False) -> ZeroWarmupEngine:
        with self._lock:
            if model_key in self._engines:
                engine = self._engines[model_key]
                self._engines.move_to_end(model_key)
                if warm:
                    engine.prime(promote_gpu=promote_gpu)
                return engine

            spec = self._specs.get(model_key)
            if spec is None:
                raise KeyError(f"unknown model key: {model_key}")

            engine = ZeroWarmupEngine(
                store_path=spec.store_path,
                dim=spec.dim,
                gear=spec.gear,
                code_dim=spec.code_dim,
                gate_path=spec.gate_path,
                meta_path=spec.meta_path,
                force_cpu=spec.force_cpu,
            )
            if warm:
                engine.prime(promote_gpu=promote_gpu)
            return self._touch(model_key, engine)

    def query(self, model_key: str, zone: int, shape: str, ns: str = None):
        """Direct store access for a specific model coordinate."""
        return self.get(model_key, warm=False).query(zone, shape, ns=ns)

    def query_coord(self, coord: GeometryCoord):
        model_key = self.resolve_coord(coord)
        return self.query(model_key, coord.zone, coord.shape, coord.ns)

    def query_batch(self, model_key: str, keys: list[tuple]):
        return self.get(model_key, warm=False).query_batch(keys)

    def query_and_plan(self, coord: GeometryCoord) -> Optional[dict]:
        """ZoneCard plan: returns card dict with pre-resolved route. None if unknown."""
        try:
            model_key = self.resolve_coord(coord)
        except KeyError:
            return None
        return self.get(model_key, warm=False).plan(coord.zone, coord.shape, coord.ns)

    def plan_and_resolve(self, coord: GeometryCoord) -> dict:
        """Plan + cache resolve with cross-model fusion fallback."""
        try:
            model_key = self.resolve_coord(coord)
        except KeyError:
            return {"card": None, "route": {"decision": "miss"}}

        engine = self.get(model_key, warm=False)
        card = engine.plan(coord.zone, coord.shape, coord.ns)
        if card is None:
            return {"card": None, "route": {"decision": "miss"}}

        r = card.get("route", {})
        if r.get("cache") == "miss" and self._fusion_enabled:
            from fusion import resolve_fused
            card_obj = _dict_to_card_proxy(card)
            fused = resolve_fused(engine._route_cache, card_obj,
                                  coord.zone, coord.shape, model_key,
                                  engine_pool=self)
            if fused["cache"] != "miss":
                card["route"] = fused
                card["fusion_source"] = fused.get("reason", "")

        # Store fusion entry for cross-model matching
        if self._fusion_enabled:
            _store_fusion_for_coord(self, coord, model_key, engine)

        return {"card": card, "route": card.get("route", {})}

    def execute_coord(self, coord: GeometryCoord,
                       decision: str = None) -> Optional[np.ndarray]:
        """Execute route with global exec cache + cross-model shared state."""
        try:
            model_key = self.resolve_coord(coord)
        except KeyError:
            return None

        engine = self.get(model_key, warm=False)

        card_obj = None
        state_hash = 0
        weights = None

        # Build card + fp once; shared by exec cache + fusion
        if self._fusion_enabled and (not decision or decision == "planner"):
            from fusion import compute_fingerprint
            from zone_card import make_card_from_np
            from global_exec_cache import get_exec_cache

            weights = engine.store.query(coord.zone, coord.shape, ns=coord.ns)
            if weights is not None:
                card_obj = make_card_from_np(weights, zone_id=coord.zone)
                state_hash = compute_fingerprint(weights)

                # Step 1: Global exec cache (O(1), no decode/plan)
                cached = get_exec_cache().lookup(card_obj, state_hash)
                if cached is not None:
                    return cached

                # Step 2: Try borrowing shared state from peer
                from fusion import borrow_shared_state
                shared = borrow_shared_state(self, coord.zone, coord.shape,
                                              model_key, card_obj,
                                              fingerprint=state_hash)
                if shared is not None:
                    w = shared["weights"]
                    dim = engine.dim
                    w_trim = w[:dim, :dim] if w.shape[0] >= dim else w
                    x = np.random.randn(dim).astype(np.float32)
                    out = x @ w_trim.T.astype(np.float32)
                    out = out[:dim] if out.shape[0] >= dim \
                        else np.pad(out, (0, dim - out.shape[0]))
                    # Cache the borrowed result
                    get_exec_cache().ensure(card_obj, state_hash, out)
                    return out

        # Step 3: Normal execution
        out = engine.execute(coord.zone, coord.shape, coord.ns,
                              decision=decision)

        # Cache result for future reuse
        if card_obj is not None and state_hash:
            from global_exec_cache import get_exec_cache
            get_exec_cache().ensure(card_obj, state_hash, out)

        return out

    def set_fusion(self, enabled: bool):
        self._fusion_enabled = enabled

    def prime(self, model_key: str, promote_gpu: bool = False) -> None:
        self.get(model_key, warm=True, promote_gpu=promote_gpu)

    def prime_coord(self, coord: GeometryCoord, promote_gpu: bool = False) -> None:
        self.prime(self.resolve_coord(coord), promote_gpu=promote_gpu)

    def forward(self, model_key: str, x, mode: int = 0, n_layers: int = 1,
                warm: bool = False, promote_gpu: bool = False):
        return self.get(model_key, warm=warm, promote_gpu=promote_gpu).forward(x, mode, n_layers)

    def forward_coord(self, coord: GeometryCoord, x, mode: int = 0,
                      n_layers: int = 1, warm: bool = False,
                      promote_gpu: bool = False):
        model_key = self.resolve_coord(coord)
        return self.forward(model_key, x, mode=mode, n_layers=n_layers,
                            warm=warm, promote_gpu=promote_gpu)

    def close(self, model_key: str = None) -> None:
        with self._lock:
            if model_key is None:
                while self._engines:
                    _, engine = self._engines.popitem(last=False)
                    engine.close()
                return

            engine = self._engines.pop(model_key, None)
            if engine is not None:
                engine.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def _demo_cli(argv: list[str]) -> int:
    if len(argv) < 4:
        print("usage: geometry_model_pool.py <query|prime> <store_path> <zone> <shape> [ns]")
        return 1

    cmd = argv[0]
    store_path = argv[1]
    if cmd not in {"query", "prime"}:
        print(f"unknown cmd: {cmd}")
        return 1

    coord = GeometryCoord(
        zone=int(argv[2]),
        shape=argv[3],
        ns=argv[4] if len(argv) > 4 else None,
    )

    pool = GeometryModelPool(max_open=1)
    pool.register("model", ModelSpec(store_path=store_path, force_cpu=True))
    pool.register_coord(coord, "model")

    if cmd == "prime":
        pool.prime_coord(coord)
        print("prime: PASS")
        pool.close()
        return 0

    out = pool.query_coord(coord)
    if out is None:
        print("query: MISS")
        pool.close()
        return 2

    print(f"query: rows={out.shape[0]} cols={out.shape[1]} mean={float(out.mean()):.6f}")
    pool.close()
    return 0


# ── Fusion helpers ──

class _CardProxy:
    """Minimal card-like object from a dict for fusion resolve."""
    def __init__(self, d: dict):
        self.card_type = d.get("card_type", -1)
        self.entropy   = d.get("entropy", 128)
        self.locality  = d.get("locality", 128)
        self.stability = d.get("stability", 128)
        self.hash_val  = d.get("hash_val", 0)


def _dict_to_card_proxy(card_dict: dict) -> _CardProxy:
    return _CardProxy(card_dict)


def _store_fusion_for_coord(pool, coord, model_key, engine):
    """Compute fusion entry for this coord's card vs all peer models."""
    from fusion import store_fusion_entry, card_similarity
    from zone_card import make_card_from_np

    z = coord.zone
    s = coord.shape
    weights = engine.store.query(z, s, ns=coord.ns)
    if weights is None:
        return
    my_card = make_card_from_np(weights, zone_id=z)

    # Check all peer models at same (zone, shape)
    for peer_key, spec in pool._specs.items():
        if peer_key == model_key:
            continue
        if spec.fusion_group:
            my_group = pool._specs.get(model_key)
            if not my_group or my_group.fusion_group != spec.fusion_group:
                continue
        try:
            peer_engine = pool.get(peer_key, warm=False)
            peer_w = peer_engine.store.query(z, s, ns=coord.ns)
            if peer_w is None:
                continue
            peer_card = make_card_from_np(peer_w, zone_id=z)
            sim = card_similarity(my_card, peer_card)
            if sim >= 0.75:
                store_fusion_entry(z, s, model_key, my_card,
                                   peer_key, peer_card)
        except (KeyError, Exception):
            continue


if __name__ == "__main__":
    raise SystemExit(_demo_cli(sys.argv[1:]))
