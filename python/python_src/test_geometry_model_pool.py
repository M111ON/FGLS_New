"""Focused test for lazy multi-model pool behavior."""

from __future__ import annotations

import tempfile
from pathlib import Path

import numpy as np

from geometry_store import GeometryStore
from geometry_model_pool import GeometryModelPool, ModelSpec
from geometry_model_pool import GeometryCoord


def _build_store(base: str) -> None:
    with GeometryStore(base, read_only=False) as store:
        store.index(2, 'S', np.array([[1.0, 2.0, 3.0]], dtype=np.float32))
        store.flush()


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        base = str(Path(tmp) / "m1")
        _build_store(base)

        pool = GeometryModelPool(max_open=1)
        pool.register("model-a", ModelSpec(store_path=base, force_cpu=True))
        pool.register_coord(GeometryCoord(2, 'S'), "model-a")

        out = pool.query_coord(GeometryCoord(2, 'S'))
        assert out is not None and out.shape == (1, 3)
        assert np.allclose(out[0], np.array([1.0, 2.0, 3.0], dtype=np.float32))

        engine = pool.get("model-a")
        assert engine._cpu_router is None
        pool.prime_coord(GeometryCoord(2, 'S'))
        assert pool.get("model-a")._cpu_router is not None

        pool.close()
        print("geometry_model_pool: PASS")


if __name__ == "__main__":
    main()
