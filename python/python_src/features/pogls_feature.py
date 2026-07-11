from __future__ import annotations
import sys, os, logging, base64, struct
from pathlib import Path
from typing import Optional
from fastapi import APIRouter, HTTPException, Query, Body
from fastapi.responses import HTMLResponse
from pydantic import BaseModel
from .base import EngineFeature

logger = logging.getLogger("engine.features.pogls")

_PATHS = [
    Path(__file__).resolve().parent.parent.parent.parent / "collection" / "geopixel",
    Path(__file__).resolve().parent.parent.parent.parent / "core" / "pogls_engine",
    Path(__file__).resolve().parent.parent.parent.parent / "core" / "pogls_engine" / "TPOGLS_s11" / "TPOGLS_s11",
    Path(__file__).resolve().parent.parent.parent.parent / "collection" / "geopixel" / "wallet",
    Path(__file__).resolve().parent.parent.parent.parent / "collection" / "core" / "pogls_engine",
    Path(__file__).resolve().parent.parent.parent.parent / "collection" / "core" / "pogls_engine" / "twin_core",
    Path(__file__).resolve().parent.parent.parent.parent / "core" / "pogls_engine" / "core",
    Path(__file__).resolve().parent.parent.parent.parent / "collection",
]
for p in _PATHS:
    sp = str(p.resolve())
    if sp not in sys.path:
        sys.path.insert(0, sp)

PG_WORKSPACE = Path(__file__).resolve().parent.parent.parent.parent

try:
    import pogls_addr_codec
    ADDR_CODEC = pogls_addr_codec
except Exception as e:
    ADDR_CODEC = None
    logger.warning(f"pogls_addr_codec import: {e}")

try:
    import pogls_visual
    VISUAL = pogls_visual
except Exception as e:
    VISUAL = None
    logger.warning(f"pogls_visual import: {e}")

try:
    from pogls_wallet_py import WalletReader
    WALLET_READER = WalletReader
except Exception as e:
    WALLET_READER = None
    logger.warning(f"WalletReader import: {e}")

try:
    from pogls_wallet_py import chunk_checksum, verify_chunk
    WALLET_UTIL = {"chunk_checksum": chunk_checksum, "verify_chunk": verify_chunk}
except Exception as e:
    WALLET_UTIL = None

_avail = {}
if ADDR_CODEC: _avail["addr_codec"] = True
if VISUAL: _avail["visual"] = True
if WALLET_READER: _avail["wallet"] = True
if WALLET_UTIL: _avail["wallet_util"] = True

MODULES = _avail


def _list_files(ext: str) -> list[dict]:
    results = []
    for root in [PG_WORKSPACE / "runner", PG_WORKSPACE]:
        if not root.exists():
            continue
        for f in sorted(root.rglob(f"*{ext}")):
            if f.is_file():
                results.append({
                    "name": f.name,
                    "path": str(f.relative_to(PG_WORKSPACE)),
                    "full_path": str(f.resolve()),
                    "size": f.stat().st_size,
                })
    return results


class EncodeRequest(BaseModel):
    addr: int = 0


class DecodeRequest(BaseModel):
    encoded: str = ""


class DecomposeRequest(BaseModel):
    addr: int = 0


class VisualCardRequest(BaseModel):
    addr: int = 0


class VisualGridRequest(BaseModel):
    addrs: list[int] = []
    cols: int = 4


class PoglsFeature(EngineFeature):
    name = "POGLS Pipeline"
    description = "POGLS address codec, visual SVG cards, wallet inspection"
    icon = "compass"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/pogls", tags=["pogls"])

        @router.get("/status")
        def pogls_status():
            return {
                "modules": MODULES,
                "addr_codec": ADDR_CODEC is not None,
                "visual": VISUAL is not None,
                "wallet": WALLET_READER is not None,
            }

        @router.get("/files")
        def pogls_files(type: str = Query("pogwallet", description="file extension filter")):
            ext_map = {"pogwallet": ".pogwallet", "gguf": ".gguf", "pogls": ".pogls", "svg": ".svg"}
            ext = ext_map.get(type, f".{type}")
            return {"files": _list_files(ext)}

        @router.post("/addr/encode")
        def pogls_addr_encode(req: EncodeRequest):
            if not ADDR_CODEC:
                raise HTTPException(503, "addr_codec module not loaded")
            enc = ADDR_CODEC.encode(req.addr)
            return {"addr": req.addr, "encoded": enc}

        @router.post("/addr/decode")
        def pogls_addr_decode(req: DecodeRequest):
            if not ADDR_CODEC:
                raise HTTPException(503, "addr_codec module not loaded")
            dec = ADDR_CODEC.decode(req.encoded)
            return {"encoded": req.encoded, "addr": dec}

        @router.post("/addr/decompose")
        def pogls_addr_decompose(req: DecomposeRequest):
            if not VISUAL:
                raise HTTPException(503, "visual module not loaded")
            dec = VISUAL.decompose(req.addr)
            return dec

        @router.post("/visual/card")
        def pogls_visual_card(req: VisualCardRequest):
            if not VISUAL:
                raise HTTPException(503, "visual module not loaded")
            svg = VISUAL.render_svg(req.addr)
            return {"addr": req.addr, "svg": svg}

        @router.post("/visual/grid")
        def pogls_visual_grid(req: VisualGridRequest):
            if not VISUAL:
                raise HTTPException(503, "visual module not loaded")
            svg = VISUAL.render_multi_svg(req.addrs, cols=req.cols)
            return {"addrs": req.addrs, "cols": req.cols, "svg": svg}

        class SvgEncodeRequest(BaseModel):
            path: str = ""

        class SvgDecodeRequest(BaseModel):
            svg: str = ""

        @router.post("/svg-encode")
        def pogls_svg_encode(path: str = Body(..., embed=True)):
            full = (PG_WORKSPACE / path).resolve()
            if not full.exists():
                raise HTTPException(404, f"File not found: {path}")
            try:
                import chunk2svg
                data = full.read_bytes()
                svg = chunk2svg.encode_svg(data)
                original_size = len(data)
                return {"path": path, "svg": svg, "original_size": original_size}
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/svg-decode")
        def pogls_svg_decode(svg: str = Body(..., embed=True)):
            try:
                import svg2chunk
                data, meta = svg2chunk.decode_svg(svg)
                import base64
                b64 = base64.b64encode(data).decode()
                name = meta.get("original_name", "decoded_file")
                return {"data": b64, "original_name": name, "size": len(data)}
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.get("/wallet/info")
        def pogls_wallet_info(path: str = Query("", description="relative path to .pogwallet")):
            if not WALLET_READER:
                raise HTTPException(503, "wallet module not loaded")
            full = (PG_WORKSPACE / path).resolve()
            if not full.exists():
                raise HTTPException(404, f"File not found: {path}")
            try:
                reader = WALLET_READER(str(full))
                info = reader.read_info()
                return {"path": path, "info": info}
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.get("/xxh64")
        def pogls_xxh64(path: str = Query("", description="relative file path")):
            full = (PG_WORKSPACE / path).resolve()
            if not full.exists():
                raise HTTPException(404, f"File not found: {path}")
            try:
                from pogls_wallet_py import chunk_checksum
                data = full.read_bytes()
                h = chunk_checksum(data)
                return {"path": path, "xxh64": hex(h), "size": len(data)}
            except Exception as e:
                raise HTTPException(500, str(e))

        @app.get("/pogls-tool", response_class=HTMLResponse)
        def pogls_tool_page():
            html_path = Path(__file__).resolve().parent.parent / "engine_pogls.html"
            if html_path.exists():
                return HTMLResponse(content=html_path.read_text(encoding="utf-8"))
            return HTMLResponse("<h1>POGLS tool page not found</h1>", status_code=404)

        app.include_router(router)
