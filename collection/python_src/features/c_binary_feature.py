from __future__ import annotations
import subprocess, logging
from pathlib import Path
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from typing import Optional
from .base import EngineFeature

logger = logging.getLogger("engine.features.cbin")

_TPOGLS = Path("C:/TPOGLS")
_BUILD = Path(__file__).resolve().parent.parent.parent / "build"

EXE_SOURCES: list[dict] = [
    {"name": "test_gate_forward.exe", "path": _TPOGLS / "test_gate_forward.exe",
     "description": "Gate C verification test — loads .f32 files, runs encoder + codebook L2 argmin",
     "cwd": _BUILD / "gate_export"},
    {"name": "geom_llm_proof.exe", "path": _TPOGLS / "geom_llm_proof.exe",
     "description": "Live LLM→geometry routing PoC — loads Qwen3-0.6B via llama.dll",
     "cwd": str(_TPOGLS)},
    {"name": "test_geometry_store_reader.exe", "path": _BUILD / "test_geometry_store_reader.exe",
     "description": "C test for .gsidx/.gsdat runtime reader",
     "cwd": str(_BUILD)},
    {"name": "pogls_bond_test.exe", "path": _BUILD / "pogls_bond_test.exe",
     "description": "Bond layer C test (make_piece, bond_verify)",
     "cwd": str(_BUILD)},
    {"name": "geo_field_bridge.exe", "path": _BUILD / "geo_field_bridge.exe",
     "description": "GeoField C bridge binary",
     "cwd": str(_BUILD)},
]


class RunRequest(BaseModel):
    binary_name: str
    args: str = ""
    timeout: int = 30


class CBinaryFeature(EngineFeature):
    name = "C Binaries"
    description = "Run C test executables from the dashboard"
    icon = "terminal"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/c", tags=["c_binaries"])

        @router.get("/list")
        def list_binaries():
            available = []
            for src in EXE_SOURCES:
                exists = src["path"].exists()
                available.append({
                    "name": src["name"],
                    "description": src["description"],
                    "available": exists,
                    "path": str(src["path"]),
                })
            return {"binaries": available}

        @router.post("/run")
        def run_binary(req: RunRequest):
            src = None
            for s in EXE_SOURCES:
                if s["name"] == req.binary_name:
                    src = s
                    break
            if src is None:
                raise HTTPException(404, f"Binary '{req.binary_name}' not found")
            if not src["path"].exists():
                raise HTTPException(404, f"File not found: {src['path']}")
            try:
                cmd = [str(src["path"])]
                if req.args:
                    cmd.extend(req.args.split())
                result = subprocess.run(
                    cmd, capture_output=True, text=True,
                    timeout=req.timeout, cwd=str(src.get("cwd", ".")),
                )
                return {
                    "exit_code": result.returncode,
                    "stdout": result.stdout,
                    "stderr": result.stderr,
                    "timed_out": False,
                }
            except subprocess.TimeoutExpired:
                return {"exit_code": -1, "stdout": "", "stderr": "TIMEOUT", "timed_out": True}
            except Exception as e:
                raise HTTPException(500, str(e))

        app.include_router(router)
