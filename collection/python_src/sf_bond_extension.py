"""
sf_bond_extension.py — Bond layer extension for Smart Folder FastAPI app

Mount onto any FastAPI app:
    from sf_bond_extension import mount_bond_extension
    mount_bond_extension(app)

Or integrate into create_app() in api.py — add this line before return app:
    from sf_bond_extension import mount_bond_extension
    mount_bond_extension(app)

Requires:
  POGLS_SO_PATH — path to pogls_bond.dll/.so/.dylib
  Or the DLL next to this file / in cwd
"""
import os, json
from pathlib import Path
from typing import Optional

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

HERE = Path(__file__).resolve().parent
os.environ.setdefault("POGLS_SO_PATH", str(HERE.parent / "pogls_bond.dll"))
import importlib.util as _iutil
_spec = _iutil.spec_from_file_location("_pogls_bridge_local", str(HERE / "pogls_bridge.py"))
_pb_mod = _iutil.module_from_spec(_spec)
_spec.loader.exec_module(_pb_mod)
try:
    PoglsBridge = _pb_mod.PoglsBridge
    FACE_N = _pb_mod.FACE_N
    FACE_S = _pb_mod.FACE_S
    FACE_E = _pb_mod.FACE_E
    FACE_W = _pb_mod.FACE_W
    _bridge = PoglsBridge()
except Exception as e:
    _bridge = None
    _load_err = str(e)

router = APIRouter(prefix="/bond", tags=["Bond Layer"])

class PieceRequest(BaseModel):
    fp: str = Field(..., description="topology_fp hex string (16 chars)")
    axis: int = Field(default=1, ge=0, le=8)

class VerifyRequest(BaseModel):
    fp_a: str = Field(..., description="topology_fp for agent A")
    axis_a: int = Field(default=1)
    fp_b: str = Field(..., description="topology_fp for agent B")
    axis_b: int = Field(default=3)

class WalletRequest(BaseModel):
    agents: list[dict] = Field(..., description="list of {fp, axis, agent_id}")
    nonce: int = Field(default=0)

@router.get("/info")
def bond_info():
    if not _bridge:
        raise HTTPException(500, f"Bond layer not loaded: {_load_err}")
    return {
        "version": _bridge.version,
        "verify_bits": _bridge.verify_bits,
        "so_path": str(_bridge._path),
    }

@router.post("/create-piece")
def create_piece(req: PieceRequest):
    if not _bridge:
        raise HTTPException(500, f"Bond layer not loaded: {_load_err}")
    seed = _bridge.seed_from_fp(req.fp)
    piece = _bridge.make_piece(seed, req.axis)
    return {
        "fp": req.fp,
        "axis": req.axis,
        "geo_key": f"{piece.geo_key:#018x}",
        "shape": piece.shape_char,
        "bond_L": f"{piece.bond_L:#018x}",
        "bond_R": f"{piece.bond_R:#018x}",
        "bond_key": f"{piece.bond_key:#018x}",
    }

@router.post("/verify")
def verify_bond(req: VerifyRequest):
    if not _bridge:
        raise HTTPException(500, f"Bond layer not loaded: {_load_err}")
    seed_a = _bridge.seed_from_fp(req.fp_a)
    seed_b = _bridge.seed_from_fp(req.fp_b)
    a = _bridge.make_piece(seed_a, req.axis_a)
    b = _bridge.make_piece(seed_b, req.axis_b)
    valid, bond_key = _bridge.bond_verify(a, b)
    return {
        "agent_a": {"fp": req.fp_a, "axis": req.axis_a, "shape": a.shape_char,
                     "geo_key": f"{a.geo_key:#018x}"},
        "agent_b": {"fp": req.fp_b, "axis": req.axis_b, "shape": b.shape_char,
                     "geo_key": f"{b.geo_key:#018x}"},
        "bond_valid": bool(valid),
        "bond_key": f"{bond_key:#018x}",
    }

@router.post("/wallet")
def build_wallet(req: WalletRequest):
    if not _bridge:
        raise HTTPException(500, f"Bond layer not loaded: {_load_err}")
    if req.nonce:
        _bridge.set_nonce(req.nonce)
    agents = []
    for ag in req.agents:
        seed = _bridge.seed_from_fp(ag["fp"])
        piece = _bridge.make_piece(seed, ag.get("axis", 1))
        agents.append({
            "agent_id": ag.get("agent_id", 0),
            "fp": ag["fp"],
            "shape": piece.shape_char,
            "geo_key": f"{piece.geo_key:#018x}",
            "bond_key": f"{piece.bond_key:#018x}",
        })
    return {"nonce": req.nonce, "agents": agents}

@router.get("/health")
def bond_health():
    if not _bridge:
        return {"loaded": False, "error": _load_err}
    return {"loaded": True, "version": _bridge.version, "verify_bits": _bridge.verify_bits}


def mount_bond_extension(app):
    from fastapi.responses import HTMLResponse
    _ui_path = HERE / "bond_ui.html"
    if _ui_path.exists():
        _ui_html = _ui_path.read_text(encoding="utf-8")
        @app.get("/bond-ui", response_class=HTMLResponse, include_in_schema=False)
        def _bond_ui():
            return _ui_html
    app.include_router(router)
    return router
