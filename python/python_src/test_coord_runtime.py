"""Focused test for coordinate-first runtime registry dispatch."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

import numpy as np

from geometry_store import GeometryStore
from coord_runtime import CoordRuntime


def _build_store(base: str) -> None:
    with GeometryStore(base, read_only=False) as store:
        store.index(2, 'S', np.array([[1.0, 2.0, 3.0]], dtype=np.float32))
        store.flush()


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        store_base = str(tmp / "model_a")
        _build_store(store_base)

        registry = {
            "max_open": 1,
            "models": {
                "model-a": {"store_path": store_base, "force_cpu": True},
            },
            "coords": [
                {"zone": 2, "shape": "S", "ns": None, "model_key": "model-a"},
            ],
        }
        registry_path = tmp / "registry.json"
        registry_path.write_text(json.dumps(registry), encoding="utf-8")

        with CoordRuntime(str(registry_path)) as runtime:
            out = runtime.query(2, 'S')
            assert out is not None and out.shape == (1, 3)
            assert np.allclose(out[0], np.array([1.0, 2.0, 3.0], dtype=np.float32))

            runtime.prime(2, 'S')
            assert runtime.pool.get("model-a")._cpu_router is not None

    print("coord_runtime: PASS")


if __name__ == "__main__":
    main()
