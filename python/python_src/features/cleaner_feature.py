from __future__ import annotations

import logging
import os
import threading
import time
from pathlib import Path
from typing import Optional

import sys
# Must come first to shadow the .pyw file in python_src
_STORAGE_CLEANER = r"I:\storage-cleaner\src"
if _STORAGE_CLEANER in sys.path:
    sys.path.remove(_STORAGE_CLEANER)
sys.path.insert(0, _STORAGE_CLEANER)
_ZGLS = r"I:\ZGLS"
if _ZGLS in sys.path:
    sys.path.remove(_ZGLS)
sys.path.insert(0, _ZGLS)

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

from storage_cleaner.core import (
    scan_directory,
    send2recycle,
    fmt_bytes,
    _prefer_keep,
    ScanResult,
)
from .base import EngineFeature

logger = logging.getLogger("engine.features.cleaner")

_scans: dict[str, ScanResult] = {}
_scan_progress: dict[str, dict] = {}
_lock = threading.Lock()


class ScanRequest(BaseModel):
    dirs: list[str]
    exclude_patterns: Optional[list[str]] = None
    min_size: int = 256


class CleanRequest(BaseModel):
    action: str = "send2recycle"
    dupe_group_indices: Optional[list[int]] = None
    trash_indices: Optional[list[int]] = None


class FileOut(BaseModel):
    path: str
    size: int
    ext: str
    category: str
    size_str: str

    @classmethod
    def from_info(cls, f):
        return cls(
            path=f.path, size=f.size, ext=f.ext,
            category=f.category, size_str=fmt_bytes(f.size),
        )


class DupeGroupOut(BaseModel):
    size: int
    count: int
    wasted: int
    size_str: str
    wasted_str: str
    files: list[FileOut]
    keep_index: int

    @classmethod
    def from_group(cls, g):
        return cls(
            size=g.size, count=len(g.files), wasted=g.wasted,
            size_str=fmt_bytes(g.size),
            wasted_str=fmt_bytes(g.wasted),
            files=[FileOut.from_info(f) for f in g.files],
            keep_index=_prefer_keep(g.files),
        )


class ScanResultOut(BaseModel):
    scan_id: str
    status: str
    total_files: int
    total_size: int
    total_size_str: str
    wasted_bytes: int
    wasted_str: str
    dupe_group_count: int
    trash_count: int
    cache_count: int
    duration_ms: float
    dupe_groups: list[DupeGroupOut]
    trash_files: list[FileOut]
    cache_files: list[FileOut]


def _run_scan(scan_id: str, req: ScanRequest):
    def progress_cb(phase, current, total):
        with _lock:
            _scan_progress[scan_id] = {"phase": phase, "current": current, "total": total}
    try:
        result = scan_directory(
            dirs=req.dirs,
            exclude_patterns=req.exclude_patterns,
            min_size=req.min_size,
            progress_cb=progress_cb,
        )
        with _lock:
            _scans[scan_id] = result
            _scan_progress[scan_id] = {"phase": "done", "current": 0, "total": 0}
    except Exception as e:
        with _lock:
            _scan_progress[scan_id] = {"phase": "error", "error": str(e)}


def _build_out(scan_id: str, r: ScanResult) -> ScanResultOut:
    return ScanResultOut(
        scan_id=scan_id, status="completed",
        total_files=r.total_files,
        total_size=r.total_size,
        total_size_str=fmt_bytes(r.total_size),
        wasted_bytes=r.wasted_bytes,
        wasted_str=fmt_bytes(r.wasted_bytes),
        dupe_group_count=len(r.dupe_groups),
        trash_count=len(r.trash_files),
        cache_count=len(r.cache_files),
        duration_ms=r.duration_ms,
        dupe_groups=[DupeGroupOut.from_group(g) for g in r.dupe_groups],
        trash_files=[FileOut.from_info(f) for f in r.trash_files],
        cache_files=[FileOut.from_info(f) for f in r.cache_files],
    )


class CleanerFeature(EngineFeature):
    name = "Storage Cleaner"
    description = "Scan directories for duplicate, trash, and cache files"
    icon = "cleaner"
    version = "1.0.0"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/cleaner", tags=["cleaner"])

        @router.get("/")
        def root():
            return {
                "name": self.name,
                "description": self.description,
                "icon": self.icon,
                "version": self.version,
            }

        @router.post("/scan", response_model=ScanResultOut)
        def start_scan(req: ScanRequest):
            scan_id = f"clean_{int(time.time())}_{threading.get_ident()}"
            with _lock:
                _scan_progress[scan_id] = {"phase": "scanning", "current": 0, "total": 0}
            t = threading.Thread(target=_run_scan, args=(scan_id, req), daemon=True)
            t.start()
            t.join()
            with _lock:
                result = _scans.get(scan_id)
            if not result:
                raise HTTPException(500, "Scan failed")
            return _build_out(scan_id, result)

        @router.post("/scan/async")
        def start_scan_async(req: ScanRequest):
            scan_id = f"clean_{int(time.time())}_{threading.get_ident()}"
            with _lock:
                _scan_progress[scan_id] = {"phase": "scanning", "current": 0, "total": 0}
            t = threading.Thread(target=_run_scan, args=(scan_id, req), daemon=True)
            t.start()
            return {"scan_id": scan_id, "status": "started"}

        @router.get("/scan/{scan_id}/status")
        def get_status(scan_id: str):
            with _lock:
                if scan_id in _scan_progress:
                    return {"scan_id": scan_id, **_scan_progress[scan_id]}
                if scan_id in _scans:
                    return {"scan_id": scan_id, "phase": "done"}
            raise HTTPException(404, "Scan not found")

        @router.get("/scan/{scan_id}/results")
        def get_results(scan_id: str):
            with _lock:
                result = _scans.get(scan_id)
            if not result:
                raise HTTPException(404, "Scan not found")
            return _build_out(scan_id, result)

        @router.post("/scan/{scan_id}/clean")
        def clean(scan_id: str, req: CleanRequest):
            with _lock:
                result = _scans.get(scan_id)
            if not result:
                raise HTTPException(404, "Scan not found")
            removed = 0
            errors = 0
            if req.dupe_group_indices is not None:
                for gi in req.dupe_group_indices:
                    if gi < 0 or gi >= len(result.dupe_groups):
                        continue
                    g = result.dupe_groups[gi]
                    keep_i = _prefer_keep(g.files)
                    for fi, f in enumerate(g.files):
                        if fi == keep_i:
                            continue
                        if send2recycle(f.path):
                            removed += 1
                        else:
                            errors += 1
            if req.trash_indices is not None:
                for ti in req.trash_indices:
                    if ti < 0 or ti >= len(result.trash_files):
                        continue
                    if send2recycle(result.trash_files[ti].path):
                        removed += 1
                    else:
                        errors += 1
            return {"scan_id": scan_id, "removed": removed, "errors": errors}

        app.include_router(router)
