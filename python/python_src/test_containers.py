"""
test_containers.py — Roundtrip tests for GPR1 and GPX4 container formats.
"""
from __future__ import annotations
import sys, os, tempfile, struct
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent
for p in [str(_HERE), str(_COLLECTION)]:
    if p not in sys.path:
        sys.path.insert(0, p)

from gpr1_container import (
    gpr1_encode_file,
    gpr1_decode_file,
    gpr1_info,
    gpr1_verify,
    GP_RESIDUAL_MAGIC,
    GPR1_HEADER_SZ,
)
from gpx4_container import (
    Gpx4File,
    Gpx4LayerDef,
    Gpx4TileEntry,
    GPX4_LAYER_META,
    GPX4_LAYER_O4,
    GPX4_LAYER_GEO,
)

PASS = 0
FAIL = 0


def check(label: str, cond: bool):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  OK  {label}")
    else:
        FAIL += 1
        print(f"  FAIL {label}")


def test_gpr1_roundtrip_small():
    print("\n=== GPR1: small data roundtrip ===")
    data = b"Hello GeoPixel Residual Codec! " * 10
    with tempfile.TemporaryDirectory() as tmp:
        in_path = Path(tmp) / "input.bin"
        gpr1_path = Path(tmp) / "test.gpr1"
        out_path = Path(tmp) / "output.bin"

        in_path.write_bytes(data)

        r = gpr1_encode_file(in_path, gpr1_path, chunk_size=16, workers=2)
        check("encode returns result", r["output_bytes"] > 0)
        check("gpr1 file exists", gpr1_path.exists())

        info = gpr1_info(gpr1_path)
        check("info returns total_len", info["total_len"] == len(data))
        check("info returns chunk_size", info["chunk_size"] == 16)
        check("info magic is GPR1", info["magic"] == "GPR1")

        r2 = gpr1_decode_file(gpr1_path, out_path)
        check("decode returns result", r2["output_bytes"] == len(data))
        check("output file exists", out_path.exists())

        v = gpr1_verify(in_path, out_path)
        check("roundtrip match", v["match"])

        gpr1_path.unlink(missing_ok=True)
        out_path.unlink(missing_ok=True)

    # Also test zero-byte edge case
    with tempfile.TemporaryDirectory() as tmp:
        in_path = Path(tmp) / "empty.bin"
        gpr1_path = Path(tmp) / "empty.gpr1"
        out_path = Path(tmp) / "empty_out.bin"
        in_path.write_bytes(b"")
        r = gpr1_encode_file(in_path, gpr1_path, chunk_size=64, workers=1)
        check("empty file encode", r["output_bytes"] > 0)
        r2 = gpr1_decode_file(gpr1_path, out_path)
        check("empty file decode output 0", r2["output_bytes"] == 0)
        v = gpr1_verify(in_path, out_path)
        check("empty roundtrip match", v["match"])


def test_gpr1_larger_data():
    print("\n=== GPR1: larger data (64KB) roundtrip ===")
    data = os.urandom(65536)
    with tempfile.TemporaryDirectory() as tmp:
        in_path = Path(tmp) / "large.bin"
        gpr1_path = Path(tmp) / "large.gpr1"
        out_path = Path(tmp) / "large_out.bin"
        in_path.write_bytes(data)

        r = gpr1_encode_file(in_path, gpr1_path, chunk_size=64, workers=4)
        print(f"  Compression ratio: {r['ratio']:.3f}x ({r['input_bytes']}B -> {r['output_bytes']}B)")

        r2 = gpr1_decode_file(gpr1_path, out_path)
        v = gpr1_verify(in_path, out_path)
        check("64KB roundtrip match", v["match"])

        # Check magic bytes in file
        hdr = gpr1_path.read_bytes()[:19]
        magic = struct.unpack_from("<4s", hdr)[0]
        check("file header magic is GPR1", magic == GP_RESIDUAL_MAGIC)


def test_gpr1_various_chunk_sizes():
    print("\n=== GPR1: various chunk sizes ===")
    data = os.urandom(4096)
    for cs in [1, 4, 16, 32, 64]:
        with tempfile.TemporaryDirectory() as tmp:
            in_path = Path(tmp) / "var.bin"
            gpr1_path = Path(tmp) / "var.gpr1"
            out_path = Path(tmp) / "var_out.bin"
            in_path.write_bytes(data)
            gpr1_encode_file(in_path, gpr1_path, chunk_size=cs, workers=1)
            gpr1_decode_file(gpr1_path, out_path)
            v = gpr1_verify(in_path, out_path)
            check(f"chunk_size={cs} roundtrip", v["match"])


def test_gpx4_basic_write_read():
    print("\n=== GPX4: basic write/read ===")
    with tempfile.TemporaryDirectory() as tmp:
        gpx4_path = Path(tmp) / "test.gpx4"

        layers = [
            Gpx4LayerDef(
                type=GPX4_LAYER_META,
                name="META",
                data=b"Hello GeoPixel GPX4!",
                tiles=None,
            ),
            Gpx4LayerDef(
                type=GPX4_LAYER_O4,
                name="F000",
                lflags=0x01,  # KEYFRAME
                data=b"\x00\x01\x02\x03" * 64,
                tiles=[
                    Gpx4TileEntry(rows=27, tflags=0x0001, blob_sz=256),
                    Gpx4TileEntry(rows=27, tflags=0x0001, blob_sz=256),
                ],
            ),
            Gpx4LayerDef(
                type=GPX4_LAYER_GEO,
                name="GEOA",
                data=struct.pack(">II", 0x12345678, 0x9ABCDEF0),
                tiles=None,
            ),
        ]

        result = Gpx4File.write(gpx4_path, tw=1, th=2, layers=layers)
        check("write succeeds", result["file_size"] > 0)
        check("n_layers = 3", result["n_layers"] == 3)
        check("n_tiles = 2", result["n_tiles"] == 2)

        # Read back
        gf = Gpx4File.open(gpx4_path)
        check("read n_layers", gf.n_layers == 3)
        check("read n_tiles", gf.n_tiles == 2)
        check("read tw=1 th=2", gf.tw == 1 and gf.th == 2)

        # Layer access
        meta = gf.layer_data_by_name("META")
        check("META layer data", meta == b"Hello GeoPixel GPX4!")

        o4 = gf.layer_data(GPX4_LAYER_O4)
        check("O4 layer data", o4 == b"\x00\x01\x02\x03" * 64)

        geo = gf.geo_addrs()
        check("GEO addrs parsed", geo is not None)
        check("GEO addr count", len(geo) == 2)
        check("GEO addr[0].packed", geo[0].packed == 0x12345678)
        check("GEO addr[0].pent", geo[0].pent == ((0x12345678 >> 28) & 0xF))
        check("GEO addr[0].hilbert", geo[0].hilbert == ((0x12345678 >> 14) & 0x3FFF))
        check("GEO addr[0].sub", geo[0].sub == (0x12345678 & 0x3FFF))

        # Tile table
        tiles = gf.tile_table(GPX4_LAYER_O4)
        check("O4 tile table exists", tiles is not None)
        check("O4 tile count", len(tiles) == 2)

        # Summary
        s = gf.summary()
        check("summary has n_layers", s["n_layers"] == 3)
        check("summary has layers list", len(s["layers"]) == 3)


def test_gpx4_empty_layers():
    print("\n=== GPX4: empty layer edge case ===")
    with tempfile.TemporaryDirectory() as tmp:
        gpx4_path = Path(tmp) / "empty.gpx4"
        layers = [
            Gpx4LayerDef(type=GPX4_LAYER_META, name="META", data=None, tiles=None),
        ]
        result = Gpx4File.write(gpx4_path, tw=0, th=0, layers=layers)
        check("empty layer write", result["file_size"] > 0)

        gf = Gpx4File.open(gpx4_path)
        check("read 1 layer", gf.n_layers == 1)
        check("read 0 tiles", gf.n_tiles == 0)


def test_gpx4_anim_hdr():
    print("\n=== GPX4: animation header ===")
    from gpx4_container import Gpx4AnimHdr
    hdr = Gpx4AnimHdr(n_frames=120, fps_num=24, fps_den=1,
                       keyframe_interval=12, width_px=1920, height_px=1080)
    data = hdr.to_bytes()
    check("anim hdr size", len(data) == 16)
    hdr2 = Gpx4AnimHdr.from_bytes(data)
    check("anim hdr roundtrip n_frames", hdr2.n_frames == 120)
    check("anim hdr roundtrip fps", hdr2.fps_num == 24)
    check("anim hdr roundtrip resolution", hdr2.width_px == 1920)


if __name__ == "__main__":
    test_gpr1_roundtrip_small()
    test_gpr1_larger_data()
    test_gpr1_various_chunk_sizes()
    test_gpx4_basic_write_read()
    test_gpx4_empty_layers()
    test_gpx4_anim_hdr()

    print(f"\n{'='*40}")
    print(f"Results: {PASS} passed, {FAIL} failed")
    sys.exit(0 if FAIL == 0 else 1)
