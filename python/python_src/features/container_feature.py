"""
container_feature.py — Container format endpoints for .gpr1 and .gpx4

Provides file upload/download, encode/decode, and inspection via the dashboard.
"""
from __future__ import annotations
import sys, os, logging, tempfile, shutil, uuid
from pathlib import Path
from fastapi import APIRouter, HTTPException, UploadFile, File, Form
from fastapi.responses import FileResponse
from pydantic import BaseModel
from typing import Optional

from .base import EngineFeature

logger = logging.getLogger("engine.features.container")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
if str(_COLLECTION) not in sys.path:
    sys.path.insert(0, str(_COLLECTION))
if str(_HERE.parent) not in sys.path:
    sys.path.insert(0, str(_HERE.parent))

from gpr1_container import (
    gpr1_encode_file,
    gpr1_decode_file,
    gpr1_info,
    gpr1_verify,
)
from gpx4_container import (
    Gpx4File,
    gpx4_info,
    gpx4_extract_layer,
    gpx4_timeline,
    gpx4_decode_o4_layer,
    o4_grid_to_svg,
)

# Temp directory for uploaded/generated files
_TMP = Path(tempfile.gettempdir()) / "geopixel_containers"
_TMP.mkdir(parents=True, exist_ok=True)


# ── Pydantic models ─────────────────────────────────────

class Gpr1EncodeRequest(BaseModel):
    chunk_size: int = 64
    workers: int = 1

class Gpr1InfoRequest(BaseModel):
    path: str

class Gpx4InfoRequest(BaseModel):
    path: str

class Gpx4ExtractRequest(BaseModel):
    path: str
    layer_name: str
    output_name: Optional[str] = None


# ════════════════════════════════════════════════════════
# FEATURE CLASS
# ════════════════════════════════════════════════════════

class ContainerFormatFeature(EngineFeature):
    name = "Container Format"
    description = "GPR1 (.gpr1) and GPX4 (.gpx4) container read/write/inspect"
    icon = "container"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/container", tags=["container"])

        # ── GPR1 file upload / encode ──────────────────

        @router.post("/gpr1/encode")
        async def gpr1_upload_encode(
            file: UploadFile = File(...),
            chunk_size: int = Form(64),
            workers: int = Form(1),
        ):
            """Upload a file, encode to GPR1, return download URL."""
            try:
                tag = uuid.uuid4().hex[:12]
                in_path = _TMP / f"{tag}_input.bin"
                out_path = _TMP / f"{tag}.gpr1"

                content = await file.read()
                in_path.write_bytes(content)

                result = gpr1_encode_file(in_path, out_path,
                                          chunk_size=chunk_size, workers=workers)
                result["download_url"] = f"/api/container/download/{out_path.name}"
                return result
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/gpr1/decode")
        async def gpr1_upload_decode(
            file: UploadFile = File(...),
        ):
            """Upload a .gpr1 file, decode to original, return download URL."""
            try:
                tag = uuid.uuid4().hex[:12]
                in_path = _TMP / f"{tag}.gpr1"
                out_path = _TMP / f"{tag}_decoded.bin"

                content = await file.read()
                in_path.write_bytes(content)

                result = gpr1_decode_file(in_path, out_path)
                result["download_url"] = f"/api/container/download/{out_path.name}"
                return result
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/gpr1/info")
        def gpr1_info_endpoint(body: Gpr1InfoRequest):
            """Inspect a .gpr1 file on disk."""
            try:
                info = gpr1_info(body.path)
                return info
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/gpr1/verify")
        async def gpr1_verify_endpoint(
            original: UploadFile = File(...),
            decoded: UploadFile = File(...),
        ):
            """Upload original and decoded files to verify roundtrip."""
            try:
                tag = uuid.uuid4().hex[:12]
                orig_path = _TMP / f"{tag}_orig.bin"
                dec_path = _TMP / f"{tag}_dec.bin"
                orig_path.write_bytes(await original.read())
                dec_path.write_bytes(await decoded.read())
                result = gpr1_verify(orig_path, dec_path)
                # cleanup
                orig_path.unlink(missing_ok=True)
                dec_path.unlink(missing_ok=True)
                return result
            except Exception as e:
                raise HTTPException(500, str(e))

        # ── GPX4 endpoints ─────────────────────────────

        @router.post("/gpx4/info")
        def gpx4_info_endpoint(body: Gpx4InfoRequest):
            """Inspect a .gpx4 file on disk."""
            try:
                info = gpx4_info(body.path)
                return info
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/gpx4/extract")
        def gpx4_extract_endpoint(body: Gpx4ExtractRequest):
            """Extract a layer from a .gpx4 file."""
            try:
                out_name = body.output_name or f"layer_{body.layer_name}.bin"
                out_path = _TMP / out_name
                result = gpx4_extract_layer(body.path, body.layer_name, out_path)
                result["download_url"] = f"/api/container/download/{out_path.name}"
                return result
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/gpx4/inspect-upload")
        async def gpx4_inspect_upload(file: UploadFile = File(...)):
            """Upload a .gpx4 file and return its structure."""
            try:
                tag = uuid.uuid4().hex[:12]
                in_path = _TMP / f"{tag}.gpx4"
                content = await file.read()
                in_path.write_bytes(content)
                gf = Gpx4File.open(in_path)
                info = gf.summary()
                info["path"] = str(in_path)
                info["file_size"] = len(content)
                # cleanup
                in_path.unlink(missing_ok=True)
                return info
            except Exception as e:
                raise HTTPException(500, str(e))

        # ── GPX4 Timeline / Preview ────────────────────

        @router.post("/gpx4/timeline")
        def gpx4_timeline_endpoint(body: Gpx4InfoRequest):
            """Get frame timeline + SVG preview for a GPX4 file."""
            try:
                tl = gpx4_timeline(body.path)
                return tl
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/gpx4/preview-o4")
        def gpx4_preview_o4(body: Gpx4InfoRequest):
            """Decode O4 layer to SVG preview."""
            try:
                gf = Gpx4File.open(body.path)
                # Find first O4 layer
                for li in gf.layers:
                    if li.type == 0x04:  # GPX4_LAYER_O4
                        data = gf.layer_data_by_name(li.name)
                        tiles = gf.tile_table_by_name(li.name)
                        result = gpx4_decode_o4_layer(data, len(tiles) if tiles else 0)
                        result["layer_name"] = li.name.strip()
                        return result
                raise HTTPException(404, "No O4 layer found in file")
            except HTTPException:
                raise
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/gpx4/build-demo")
        def gpx4_build_demo():
            """Build a minimal GPX4 demo file with synthetic O4 grid data."""
            import struct
            try:
                tag = uuid.uuid4().hex[:12]
                out_path = _TMP / f"{tag}_demo.gpx4"
                # Grid: 27×3 pixels of synthetic data
                grid_h = 3
                pixel_data = bytearray()
                for y in range(grid_h):
                    for x in range(27):
                        r = (x * 10 + y * 30) & 0xFF
                        g = (x * 20 + y * 10) & 0xFF
                        b = (x * 30 + y * 20) & 0xFF
                        pixel_data.extend([r, g, b])
                layer_data = bytes(pixel_data)
                with open(out_path, "wb") as f:
                    # File header: n_tiles=0 so no tile table is written
                    f.write(struct.pack(">4sBBHHHI", b"GPX4", 2, 0, 1, 1, 1, 0))
                    # Layer table: 1 O4 layer
                    lay_off = 16 + 14  # header + 1 layer entry
                    f.write(struct.pack(">BB4sII", 4, 0, b"O4_D", lay_off, len(layer_data)))
                    # Layer data (raw RGB grid)
                    f.write(layer_data)

                return {
                    "path": str(out_path),
                    "download_url": f"/api/container/download/{out_path.name}",
                    "size": out_path.stat().st_size,
                    "note": "Synthetic demo GPX4 with O4 grid 27×3",
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        # ── Generic file download ──────────────────────

        @router.get("/download/{filename}")
        def container_download(filename: str):
            """Download a generated file."""
            file_path = _TMP / filename
            if not file_path.exists():
                raise HTTPException(404, f"File not found: {filename}")
            media_type = "application/octet-stream"
            if filename.endswith(".gpr1"):
                media_type = "application/geopixel-residual"
            elif filename.endswith(".gpx4"):
                media_type = "application/geopixel-gpx4"
            return FileResponse(str(file_path), media_type=media_type,
                                filename=filename)

        app.include_router(router)
