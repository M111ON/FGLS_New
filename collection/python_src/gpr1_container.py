"""
gpr1_container.py — GeoPixel Residual container (.gpr1)

File-level read/write, info, and convenience wrappers around
the in-memory residual codec from geopixel_feature.py.

File layout:
  [FILE_HEADER]     19B   magic(4) + ver(1) + chunk_size(2) + n_chunks(2) + total_len(8)
  [CHUNK_RECORDS]   variable
    slot_idx(4) + orig_len(4) + mode(2) + pay_len(2) + payload(variable)
"""
from __future__ import annotations
import struct, os, logging
from pathlib import Path
from typing import Optional
from features.geopixel_feature import (
    gp_residual_encode_bytes,
    gp_residual_decode_bytes,
    GP_RESIDUAL_MAGIC,
    GP_RESIDUAL_VERSION,
    GP_RESIDUAL_MAX_CHUNK,
    GPR1_HEADER_SZ,
)

logger = logging.getLogger("engine.gpr1")


def gpr1_info(path: str | Path) -> dict:
    """Read GPR1 header metadata from a .gpr1 file."""
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(f"GPR1 file not found: {path}")
    with open(path, "rb") as f:
        hdr = f.read(GPR1_HEADER_SZ)
    if len(hdr) < GPR1_HEADER_SZ:
        raise ValueError(f"Truncated GPR1 header: got {len(hdr)} bytes, need {GPR1_HEADER_SZ}")
    magic, version, chunk_size, n_chunks, total_len = struct.unpack_from(
        "<4sBHHQ", hdr, 0)
    if magic != GP_RESIDUAL_MAGIC:
        raise ValueError(f"Bad GPR1 magic: {magic!r}")
    if version != GP_RESIDUAL_VERSION:
        raise ValueError(f"Unsupported GPR1 version: {version}")
    file_size = path.stat().st_size
    ratio = file_size / total_len if total_len else 0.0
    return {
        "path": str(path),
        "file_size": file_size,
        "magic": magic.decode("ascii"),
        "version": version,
        "chunk_size": chunk_size,
        "n_chunks": n_chunks,
        "total_len": total_len,
        "ratio": ratio,
        "overhead": file_size - total_len,
    }


def gpr1_encode_file(
    input_path: str | Path,
    output_path: str | Path,
    chunk_size: int = 64,
    workers: int = 1,
) -> dict:
    """Encode a file to .gpr1 format."""
    input_path = Path(input_path)
    output_path = Path(output_path)
    if not input_path.exists():
        raise FileNotFoundError(f"Input file not found: {input_path}")

    data = input_path.read_bytes()
    blob = gp_residual_encode_bytes(data, chunk_size=chunk_size, workers=workers)
    output_path.write_bytes(blob)

    ratio = len(blob) / len(data) if data else 0.0
    logger.info("GPR1 encode %s → %s (%.3fx, %dB→%dB)",
                input_path.name, output_path.name, ratio, len(data), len(blob))
    return {
        "input_path": str(input_path),
        "output_path": str(output_path),
        "input_bytes": len(data),
        "output_bytes": len(blob),
        "ratio": ratio,
        "chunk_size": chunk_size,
        "workers": workers,
    }


def gpr1_decode_file(
    input_path: str | Path,
    output_path: str | Path,
) -> dict:
    """Decode a .gpr1 file back to original."""
    input_path = Path(input_path)
    output_path = Path(output_path)
    if not input_path.exists():
        raise FileNotFoundError(f"GPR1 file not found: {input_path}")

    blob = input_path.read_bytes()
    data = gp_residual_decode_bytes(blob)
    output_path.write_bytes(data)

    logger.info("GPR1 decode %s → %s (%dB→%dB)",
                input_path.name, output_path.name, len(blob), len(data))
    return {
        "input_path": str(input_path),
        "output_path": str(output_path),
        "input_bytes": len(blob),
        "output_bytes": len(data),
    }


def gpr1_verify(
    original_path: str | Path,
    decoded_path: str | Path,
) -> dict:
    """Verify that decoded file matches original."""
    original = Path(original_path).read_bytes()
    decoded = Path(decoded_path).read_bytes()
    match = (original == decoded)
    return {
        "original_path": str(original_path),
        "decoded_path": str(decoded_path),
        "original_bytes": len(original),
        "decoded_bytes": len(decoded),
        "match": match,
    }
