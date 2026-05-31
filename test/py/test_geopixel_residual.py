from __future__ import annotations

import base64
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(r"I:\FGLS_new\collection")
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "python_src"))

from features.geopixel_feature import (  # noqa: E402
    gp_residual_decode_bytes,
    gp_residual_encode_bytes,
    gp_residual_encode_file,
)


def _load_seed_bytes() -> bytes:
    src = ROOT / "geo_frame_seek.h"
    with src.open("rb") as f:
        data = f.read()
    # Build a larger, deterministic corpus from a real file.
    return data * 64


def _assert_roundtrip(label: str, raw: bytes, blob: bytes) -> None:
    decoded = gp_residual_decode_bytes(blob)
    assert decoded == raw, f"{label}: roundtrip mismatch"


def main() -> int:
    raw = _load_seed_bytes()
    print("=== GeoPixel residual codec ===")
    print(f"input: {len(raw)} bytes")

    blob_seq = gp_residual_encode_bytes(raw, chunk_size=64, workers=1)
    blob_par = gp_residual_encode_bytes(raw, chunk_size=64, workers=4)

    print(f"sequential blob: {len(blob_seq)} bytes ({len(blob_seq)/len(raw):.4f}x)")
    print(f"parallel blob:   {len(blob_par)} bytes ({len(blob_par)/len(raw):.4f}x)")

    assert blob_seq == blob_par, "parallel output must be deterministic"
    _assert_roundtrip("sequential", raw, blob_seq)
    _assert_roundtrip("parallel", raw, blob_par)

    # File streaming path: write a temp file and encode without loading whole file.
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "large.bin"
        p.write_bytes(raw)
        blob_file = gp_residual_encode_file(str(p), chunk_size=64, workers=4)
        print(f"file-stream blob: {len(blob_file)} bytes ({len(blob_file)/len(raw):.4f}x)")
        assert blob_file == blob_seq, "file streaming must match memory encoding"
        _assert_roundtrip("file-stream", raw, blob_file)

    # Sanity: base64 transport path.
    b64 = base64.b64encode(blob_seq).decode("ascii")
    assert gp_residual_decode_bytes(base64.b64decode(b64)) == raw

    print("ALL PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
