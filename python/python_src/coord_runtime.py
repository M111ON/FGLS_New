"""
coord_runtime.py — coordinate-first runtime entrypoint
======================================================

Loads a registry JSON, maps (zone, shape, ns) to a model key, and lazily
opens the matching geometry store/model on demand.

Registry format:
{
  "max_open": 2,
  "models": {
    "qwen": {"store_path": "build/qwen_geom_v3", "gguf_path": "models/qwen.gguf", "force_cpu": true}
  },
  "coords": [
    {"zone": 2, "shape": "S", "ns": "", "model_key": "qwen"}
  ]
}
"""

from __future__ import annotations

import json
import sys
from dataclasses import asdict
from pathlib import Path

import numpy as np
import torch

from geometry_model_pool import GeometryCoord, GeometryModelPool, ModelSpec
from zero_warmup_engine import _build_router_on_device


def _resolve_registry_path(raw_path: str) -> Path:
    p = Path(raw_path)
    if p.is_file():
        return p

    here = Path(__file__).resolve().parent
    candidates = [
        Path.cwd() / raw_path,
        here / raw_path,
        here.parent / raw_path,
        here.parent.parent / raw_path,
    ]
    for c in candidates:
        if c.is_file():
            return c
    raise FileNotFoundError(f"registry not found: {raw_path}")


class CoordRuntime:
    def __init__(self, registry_path: str):
        self.registry_path = _resolve_registry_path(registry_path)
        with open(self.registry_path, "r", encoding="utf-8") as f:
            raw = json.load(f)

        self.pool = GeometryModelPool(max_open=raw.get("max_open", 2))

        models = raw.get("models", {})
        coords = raw.get("coords", [])
        default_coord = raw.get("default_coord")

        valid_keys = {"store_path", "gguf_path", "dim", "gear", "code_dim", "gate_path", "meta_path", "force_cpu"}
        for model_key, spec in models.items():
            store_path = spec.get("store_path")
            if store_path is None:
                continue
            filtered = {k: v for k, v in spec.items() if k in valid_keys}
            try:
                self.pool.register(model_key, ModelSpec(**filtered))
            except Exception as e:
                print(f"[coord_runtime] skip model '{model_key}': {e}")
                continue

        for item in coords:
            coord = GeometryCoord(
                zone=int(item["zone"]),
                shape=str(item["shape"]),
                ns=item.get("ns") or None,
            )
            self.pool.register_coord(coord, item["model_key"])

        if default_coord is not None:
            self.default_coord = GeometryCoord(
                zone=int(default_coord["zone"]),
                shape=str(default_coord["shape"]),
                ns=default_coord.get("ns") or None,
            )
        elif coords:
            first = coords[0]
            self.default_coord = GeometryCoord(
                zone=int(first["zone"]),
                shape=str(first["shape"]),
                ns=first.get("ns") or None,
            )
        else:
            self.default_coord = None

    def prime(self, zone: int, shape: str, ns: str = None, promote_gpu: bool = False) -> None:
        self.pool.prime_coord(GeometryCoord(zone=zone, shape=shape, ns=ns), promote_gpu=promote_gpu)

    def query(self, zone: int, shape: str, ns: str = None):
        return self.pool.query_coord(GeometryCoord(zone=zone, shape=shape, ns=ns))

    def inspect(self, model_key: str, probe: np.ndarray, mode: int = 0) -> dict:
        spec = self.pool._specs[model_key]
        router = _build_router_on_device(spec.dim, spec.gear, spec.code_dim, spec.gate_path, 'cpu')
        probe = np.asarray(probe, dtype=np.float32).reshape(-1)
        if probe.shape[0] < spec.dim:
            probe = np.pad(probe, (0, spec.dim - probe.shape[0]))
        probe = probe[:spec.dim]
        if probe.size:
            probe = probe - probe.mean()

        with torch.no_grad():
            verdict = router.route(torch.from_numpy(probe[np.newaxis, :]), mode)

        zone = int(verdict.zone[0].item())
        shape = chr(int(verdict.shape[0].item()))
        resolved = None
        try:
            resolved = self.pool.resolve_coord(GeometryCoord(zone=zone, shape=shape, ns=None))
        except KeyError:
            resolved = None

        return {
            "model_key": model_key,
            "resolved_model_key": resolved,
            "zone": zone,
            "shape": shape,
            "polarity": int(verdict.polarity[0].item()),
            "tring_slot": int(verdict.tring_slot[0].item()),
            "mode": mode,
        }

    def resolve(self, zone: int, shape: str, ns: str = None) -> tuple[str, ModelSpec]:
        coord = GeometryCoord(zone=zone, shape=shape, ns=ns)
        model_key = self.pool.resolve_coord(coord)
        return model_key, self.pool._specs[model_key]

    def resolve_default(self) -> tuple[str, ModelSpec, GeometryCoord]:
        if self.default_coord is None:
            raise KeyError("registry has no default_coord or coords")
        model_key = self.pool.resolve_coord(self.default_coord)
        return model_key, self.pool._specs[model_key], self.default_coord

    def plan(self, zone: int, shape: str, ns: str = None) -> Optional[dict]:
        """
        Generate ZoneCard — route pre-resolved via cache (no LLM).
        Returns card dict with pre-resolved route, or None if miss.
        """
        try:
            return self.pool.query_and_plan(GeometryCoord(zone=zone, shape=shape, ns=ns))
        except KeyError:
            return None

    def plan_and_resolve(self, zone: int, shape: str,
                          ns: str = None) -> dict:
        """Plan + resolve route via cache. Returns {card, route}."""
        return self.pool.plan_and_resolve(GeometryCoord(zone=zone, shape=shape, ns=ns))

    def route_from_card(self, card_dict: dict) -> dict:
        """
        Route resolution via RouteRuleCache (no LLM).
        Falls back to planner only on cache miss.

        This replaces the old if-else chain with a proper rule engine.
        """
        from route_cache import get_cache
        # Convert dict back to a simple object for the cache
        class _CardProxy:
            pass
        proxy = _CardProxy()
        proxy.card_type = card_dict.get("card_type", -1)
        proxy.entropy = card_dict.get("entropy", 128)
        proxy.locality = card_dict.get("locality", 128)
        proxy.stability = card_dict.get("stability", 128)
        proxy.hash_val = card_dict.get("hash_val", 0)
        if isinstance(proxy.hash_val, str) and proxy.hash_val.startswith("0x"):
            proxy.hash_val = int(proxy.hash_val, 16)
        return get_cache().resolve(proxy)

    def execute_plan(self, plan: dict, zone: int, shape: str,
                     ns: str = None) -> Optional[np.ndarray]:
        """Execute a route plan. Skips if decision is 'skip'."""
        decision = plan.get("decision", "fallback")
        if decision in ("skip", "skip-all", "skip-middle"):
            return None
        try:
            return self.pool.execute_coord(GeometryCoord(zone=zone, shape=shape, ns=ns),
                                            decision=decision)
        except KeyError:
            return None

    def execute_with_plan(self, zone: int, shape: str,
                          ns: str = None) -> Optional[np.ndarray]:
        """plan + resolve + execute: all-in-one, no LLM anywhere."""
        result = self.plan_and_resolve(zone, shape, ns=ns)
        if result["card"] is None:
            return None
        return self.execute_plan(result["route"], zone, shape, ns=ns)

    def forward(self, zone: int, shape: str, x, ns: str = None, mode: int = 0,
                n_layers: int = 1, warm: bool = False, promote_gpu: bool = False):
        return self.pool.forward_coord(
            GeometryCoord(zone=zone, shape=shape, ns=ns),
            x,
            mode=mode,
            n_layers=n_layers,
            warm=warm,
            promote_gpu=promote_gpu,
        )

    def close(self) -> None:
        self.pool.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def _parse_coord(argv: list[str]) -> GeometryCoord:
    return GeometryCoord(zone=int(argv[0]), shape=argv[1], ns=argv[2] if len(argv) > 2 else None)


def _text_to_probe(text: str, dim: int) -> np.ndarray:
    data = text.encode("utf-8")
    if not data:
        return np.zeros(dim, dtype=np.float32)
    out = np.zeros(dim, dtype=np.float32)
    for i, b in enumerate(data):
        out[i % dim] += (b / 255.0) * (1.0 + (i % 7) * 0.1)
    out -= out.mean()
    return out


def _resolve_probe_path(raw_path: str) -> Path:
    p = Path(raw_path)
    if p.is_file():
        return p

    here = Path(__file__).resolve().parent
    candidates = [
        Path.cwd() / raw_path,
        here / raw_path,
        here.parent / raw_path,
        here.parent.parent / raw_path,
    ]
    for c in candidates:
        if c.is_file():
            return c
    raise FileNotFoundError(f"probe file not found: {raw_path}")


def _load_probe(path: str) -> np.ndarray:
    p = _resolve_probe_path(path)
    if p.suffix.lower() == ".npy":
        return np.load(str(p)).astype(np.float32).reshape(-1)
    if p.suffix.lower() == ".npz":
        z = np.load(str(p))
        first = z[z.files[0]]
        return np.asarray(first, dtype=np.float32).reshape(-1)
    text = p.read_text(encoding="utf-8")
    nums = [float(x) for x in text.replace(",", " ").split() if x]
    return np.asarray(nums, dtype=np.float32)


def _demo_cli(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: coord_runtime.py <registry.json> <query|prime|resolve|resolve-default|inspect-text|inspect-npy> ...")
        return 1

    registry_path = argv[0]
    cmd = argv[1]

    with CoordRuntime(registry_path) as runtime:
        if cmd == "prime":
            coord = _parse_coord(argv[2:])
            runtime.prime(coord.zone, coord.shape, coord.ns)
            print(f"prime: {asdict(coord)} PASS")
            return 0

        if cmd == "resolve":
            coord = _parse_coord(argv[2:])
            model_key, spec = runtime.resolve(coord.zone, coord.shape, coord.ns)
            print(f"model_key={model_key}")
            print(f"gguf_path={spec.gguf_path or ''}")
            print(f"store_path={spec.store_path}")
            print(f"force_cpu={'1' if spec.force_cpu else '0'}")
            return 0

        if cmd == "resolve-default":
            model_key, spec, coord = runtime.resolve_default()
            print(f"model_key={model_key}")
            print(f"gguf_path={spec.gguf_path or ''}")
            print(f"store_path={spec.store_path}")
            print(f"force_cpu={'1' if spec.force_cpu else '0'}")
            print(f"zone={coord.zone}")
            print(f"shape={coord.shape}")
            print(f"ns={coord.ns or ''}")
            return 0

        if cmd == "query":
            coord = _parse_coord(argv[2:])
            out = runtime.query(coord.zone, coord.shape, coord.ns)
            if out is None:
                print(f"query: {asdict(coord)} MISS")
                return 2
            print(f"query: rows={out.shape[0]} cols={out.shape[1]} mean={float(out.mean()):.6f}")
            return 0

        if cmd == "inspect-text":
            if len(argv) < 4:
                print("usage: coord_runtime.py <registry.json> inspect-text <model_key> <text...>")
                return 1
            model_key = argv[2]
            text = " ".join(argv[3:])
            probe = _text_to_probe(text, runtime.pool._specs[model_key].dim)
            info = runtime.inspect(model_key, probe)
            print(f"model_key={info['model_key']}")
            print(f"resolved_model_key={info['resolved_model_key'] or ''}")
            print(f"zone={info['zone']}")
            print(f"shape={info['shape']}")
            print(f"polarity={info['polarity']}")
            print(f"tring_slot={info['tring_slot']}")
            return 0

        if cmd == "inspect-npy":
            if len(argv) < 4:
                print("usage: coord_runtime.py <registry.json> inspect-npy <model_key> <probe.npy|.npz|.txt>")
                return 1
            model_key = argv[2]
            probe = _load_probe(argv[3])
            info = runtime.inspect(model_key, probe)
            print(f"model_key={info['model_key']}")
            print(f"resolved_model_key={info['resolved_model_key'] or ''}")
            print(f"zone={info['zone']}")
            print(f"shape={info['shape']}")
            print(f"polarity={info['polarity']}")
            print(f"tring_slot={info['tring_slot']}")
            return 0

        print(f"unknown cmd: {cmd}")
        return 1


if __name__ == "__main__":
    raise SystemExit(_demo_cli(sys.argv[1:]))
