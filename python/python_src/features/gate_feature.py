from __future__ import annotations
import sys, torch, logging, numpy as np
from pathlib import Path
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from typing import Optional
from .base import EngineFeature

logger = logging.getLogger("engine.features.gate")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
if str(_COLLECTION) not in sys.path:
    sys.path.insert(0, str(_COLLECTION))
if str(_HERE.parent) not in sys.path:
    sys.path.insert(0, str(_HERE.parent))

_GATE_INSTANCE = None


def _get_gate():
    global _GATE_INSTANCE
    if _GATE_INSTANCE is not None:
        return _GATE_INSTANCE
    try:
        from bermuda_reshape_v3 import BermudaGate
        _GATE_INSTANCE = BermudaGate(128, code_dim=32, gear=2)
        _GATE_INSTANCE.eval()
        logger.info("Gate initialized")
        return _GATE_INSTANCE
    except Exception as e:
        logger.warning(f"Gate init failed: {e}")
        return None


class GateFeature(EngineFeature):
    name = "Bermuda Gate"
    description = "Gate encoder + codebook test console"
    icon = "gate"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/gate", tags=["gate"])

        @router.get("/info")
        def gate_info():
            gate = _get_gate()
            if gate is None:
                return {"status": "unavailable"}
            cb = gate.codebook
            return {
                "status": "online",
                "dim": gate.dim,
                "code_dim": cb.code_dim,
                "n_codes": cb.codes.shape[0],
                "codes_shape": list(cb.codes.shape),
                "geo_table_shape": list(cb.geo_table.shape) if hasattr(cb, 'geo_table') else None,
                "encoder_layers": [
                    {"type": "linear", "in": gate.encoder[0].in_features,
                     "out": gate.encoder[0].out_features},
                    {"type": "layernorm", "dim": gate.encoder[1].normalized_shape[0]},
                    {"type": "gelu"},
                    {"type": "linear", "in": gate.encoder[3].in_features,
                     "out": gate.encoder[3].out_features},
                ],
            }

        @router.post("/forward")
        def gate_forward(body: dict = None):
            gate = _get_gate()
            if gate is None:
                raise HTTPException(503, "Gate unavailable")
            try:
                if body and "input" in body:
                    arr = np.array(body["input"], dtype=np.float32)
                else:
                    arr = np.random.randn(128).astype(np.float32)
                t = torch.from_numpy(arr)
                if t.dim() == 1:
                    t = t.unsqueeze(0)
                with torch.no_grad():
                    idx, z_q, loss = gate.encode_tokens(t)
                idx_val = int(idx[0].item())
                geo = None
                if hasattr(gate.codebook, 'geo_table') and gate.codebook.geo_table is not None:
                    gt = gate.codebook.geo_table
                    if idx_val < gt.shape[0]:
                        geo = gt[idx_val].tolist()
                entry = _classify_entry(idx_val)
                return {
                    "codebook_index": idx_val,
                    "quantize_loss": round(float(loss.item()), 6),
                    "geo_params": geo,
                    "zone": entry["zone"],
                    "shape": entry["shape"],
                    "polarity": entry["polarity"],
                    "tring_slot": entry["tring_slot"],
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.get("/compare-c")
        def gate_compare_c():
            try:
                import subprocess
                gate_export = _COLLECTION / "build" / "gate_export"
                test_exe = Path("C:/TPOGLS/test_gate_forward.exe")
                if not test_exe.exists():
                    return {"status": "unavailable", "error": "C test_gate_forward.exe not found"}
                result = subprocess.run(
                    [str(test_exe)],
                    capture_output=True, text=True, timeout=15,
                    cwd=str(gate_export),
                )
                stdout = result.stdout
                stderr = result.stderr
                code_idx = None
                for line in stdout.split("\n"):
                    if "code_idx" in line or "codebook" in line.lower():
                        import re
                        m = re.search(r'(\d+)', line)
                        if m:
                            code_idx = int(m.group(1))
                return {
                    "status": "done",
                    "exit_code": result.returncode,
                    "stdout": stdout,
                    "stderr": stderr,
                    "parsed_code_idx": code_idx,
                }
            except Exception as e:
                return {"status": "error", "error": str(e)}

        app.include_router(router)


def _classify_entry(idx: int):
    mode_names = ["ORBITAL", "CHIRAL", "CROSS", "HUB"]
    stride = 37
    walk_len = 1024
    face_sz = walk_len // 12
    enc = (idx * stride) % walk_len
    zone = enc // face_sz
    pole = 0 if zone < 6 else 1
    tring = idx % 720
    shapes = {0: 'I', 1: 'O', 2: 'S', 3: 'L'}
    mode = idx % 4
    return {
        "zone": int(zone),
        "shape": shapes.get(mode, 'I'),
        "polarity": "ROUTE" if pole == 0 else "GROUND",
        "tring_slot": int(tring),
    }
