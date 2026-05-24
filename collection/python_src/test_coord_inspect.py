"""Focused test for coord inspection CLI/runtime."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

import numpy as np

from geometry_store import GeometryStore, SHAPES
from coord_runtime import CoordRuntime, _text_to_probe


def _build_store(base: str) -> None:
    with GeometryStore(base, read_only=False) as store:
        store.index(2, 'S', np.array([[1.0, 2.0, 3.0]], dtype=np.float32))
        store.flush()


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        store_base = str(tmp / "model_a")
        _build_store(store_base)

        coords = []
        for z in range(12):
            for s in SHAPES:
                coords.append({"zone": z, "shape": s, "ns": None, "model_key": "model-a"})

        registry = {
            "max_open": 1,
            "models": {
                "model-a": {"store_path": store_base, "gguf_path": "/tmp/model-a.gguf", "force_cpu": True, "dim": 128, "gear": 2, "code_dim": 32},
            },
            "coords": coords,
        }
        registry_path = tmp / "registry.json"
        registry_path.write_text(json.dumps(registry), encoding="utf-8")

        with CoordRuntime(str(registry_path)) as runtime:
            probe = _text_to_probe("inspect-me", 128)
            info = runtime.inspect("model-a", probe)
            assert info["model_key"] == "model-a"
            assert info["resolved_model_key"] == "model-a"
            assert info["shape"] in SHAPES
            assert 0 <= info["zone"] < 12

    print("coord_inspect: PASS")


if __name__ == "__main__":
    main()
