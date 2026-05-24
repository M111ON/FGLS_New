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

    def register(self, model_key: str, spec: ModelSpec) -> None:
        if not model_key:
            raise ValueError("model_key is required")
        if not Path(spec.store_path + ".gsidx").exists():
            raise FileNotFoundError(f"missing store index: {spec.store_path}.gsidx")
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


if __name__ == "__main__":
    raise SystemExit(_demo_cli(sys.argv[1:]))
