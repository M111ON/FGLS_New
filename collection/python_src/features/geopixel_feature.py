from __future__ import annotations
import sys, logging, random, base64, os, struct
from pathlib import Path
from functools import lru_cache
from concurrent.futures import ThreadPoolExecutor
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from typing import Optional, List
from .base import EngineFeature

logger = logging.getLogger("engine.features.geopixel")

_HERE = Path(__file__).resolve().parent
_COLLECTION = _HERE.parent.parent
if str(_COLLECTION) not in sys.path:
    sys.path.insert(0, str(_COLLECTION))
if str(_HERE.parent) not in sys.path:
    sys.path.insert(0, str(_HERE.parent))

# ── Constants ───────────────────────────────────────────
GP_TRIT_MOD   = 27
GP_SPOKE_MOD  = 6
GP_COSET_MOD  = 9
GP_LETTER_MOD = 26
GP_FIBO_MOD   = 144
GP_GRID_W     = 27
BGP_STRIPE_W     = 27
BGP_STRIPE_BYTES = 25
BGP_STRIPE_V2_PX = 9
BGP_STRIPE_V2_BUF = 27
GP_RESIDUAL_MAGIC = b"GPR1"
GP_RESIDUAL_VERSION = 1
GP_RESIDUAL_MAX_CHUNK = 64
GPR1_HEADER_SZ = 17  # magic(4) + ver(1) + cs(2) + nc(2) + total_len(8)

# ── Input limiter guards ──────────────────────────────
GEOPIXEL_MAX_INPUT_BYTES = 64 * 1024 * 1024    # 64MB decoded cap
GEOPIXEL_MAX_BLOB_BYTES  = 128 * 1024 * 1024   # 128MB encoded cap
GEOPIXEL_MAX_PIXEL_ARRAY = 9999                # max pixels in stripe decode


def _reject_oversized(label: str, size: int, limit: int):
    if size > limit:
        raise HTTPException(413,
            f"{label} too large: {size} bytes (max {limit})")


def _validate_stripe_pixels(pixels: list, min_count: int, label: str):
    if not isinstance(pixels, list):
        raise HTTPException(400, f"{label}: must be a list")
    if len(pixels) > GEOPIXEL_MAX_PIXEL_ARRAY:
        raise HTTPException(413, f"{label}: too many pixels ({len(pixels)})")
    if len(pixels) < min_count:
        raise HTTPException(400,
            f"{label}: need at least {min_count} pixels, got {len(pixels)}")
    for i, p in enumerate(pixels):
        if not isinstance(p, dict) or not all(k in p for k in ("r", "g", "b")):
            raise HTTPException(400,
                f"{label}[{i}]: missing r/g/b fields")


def geo_pixel_encode(idx: int, W: int) -> tuple[int, int, int]:
    idx_mod = idx % W
    r = ((idx_mod % GP_TRIT_MOD) << 3) | (idx_mod % GP_SPOKE_MOD)
    g = ((idx_mod % GP_COSET_MOD) << 4) | (idx_mod % GP_LETTER_MOD & 0xF)
    b = idx_mod % GP_FIBO_MOD
    return (r & 0xFF, g & 0xFF, b & 0xFF)


def geo_pixel_decode(r: int, g: int, b: int) -> dict:
    return {
        "trit":  r >> 3,
        "spoke": r & 0x7,
        "coset": g >> 4,
        "letter": g & 0xF,
        "fibo":  b & 0xFF,
    }


def _bgp_crt_byte(trit: int, fibo: int) -> int:
    for x in range(trit, 256, GP_TRIT_MOD):
        if x % GP_FIBO_MOD == fibo:
            return x
    return trit


def bond_piece_fingerprint(geo_key: int, shape: str,
                            bond_L: int, bond_R: int) -> tuple[int, int, int]:
    bk = bond_L ^ bond_R
    shape_idx = ord(shape) - ord('A') if shape and 'A' <= shape <= 'Z' else 0
    r = ((shape_idx & 0x1F) << 3) | (geo_key & 0x7)
    g = ((((geo_key >> 3) & 0xF) << 4) | (bk & 0xF))
    b = (((geo_key >> 16) ^ (bk >> 8)) % GP_FIBO_MOD)
    return (r & 0xFF, g & 0xFF, b & 0xFF)


def bond_piece_to_stripe(geo_key: int, shape: str,
                          bond_L: int, bond_R: int) -> List[dict]:
    buf = bytearray(BGP_STRIPE_BYTES)
    for i in range(8):
        buf[i] = (geo_key >> (i * 8)) & 0xFF
    buf[8] = ord(shape) if shape else ord('I')
    for i in range(8):
        buf[9 + i] = (bond_L >> (i * 8)) & 0xFF
    for i in range(8):
        buf[17 + i] = (bond_R >> (i * 8)) & 0xFF

    stripe = []
    for i in range(BGP_STRIPE_BYTES):
        r, g, b = geo_pixel_encode(buf[i], 256)
        stripe.append({"index": i, "byte": buf[i],
                        "rgb": "#%02x%02x%02x" % (r, g, b),
                        "r": r, "g": g, "b": b})

    bk = bond_L ^ bond_R
    r25, g25, b25 = geo_pixel_encode(bk & 0xFF, 256)
    stripe.append({"index": 25, "byte": bk & 0xFF,
                    "rgb": "#%02x%02x%02x" % (r25, g25, b25),
                    "r": r25, "g": g25, "b": b25})
    r26, g26, b26 = geo_pixel_encode((bk >> 8) & 0xFF, 256)
    stripe.append({"index": 26, "byte": (bk >> 8) & 0xFF,
                    "rgb": "#%02x%02x%02x" % (r26, g26, b26),
                    "r": r26, "g": g26, "b": b26})
    return stripe


def bond_stripe_from_pixels(pixels: List[dict]) -> dict:
    buf = bytearray(BGP_STRIPE_BYTES)
    for i in range(BGP_STRIPE_BYTES):
        p = pixels[i]
        fields = geo_pixel_decode(p["r"], p["g"], p["b"])
        buf[i] = _bgp_crt_byte(fields["trit"], fields["fibo"])

    geo_key = 0
    for i in range(8):
        geo_key |= buf[i] << (i * 8)
    shape = chr(buf[8]) if 32 <= buf[8] <= 126 else '?'
    bond_L = 0
    for i in range(8):
        bond_L |= buf[9 + i] << (i * 8)
    bond_R = 0
    for i in range(8):
        bond_R |= buf[17 + i] << (i * 8)

    f25 = geo_pixel_decode(pixels[25]["r"], pixels[25]["g"], pixels[25]["b"])
    f26 = geo_pixel_decode(pixels[26]["r"], pixels[26]["g"], pixels[26]["b"])
    bk_lo = _bgp_crt_byte(f25["trit"], f25["fibo"])
    bk_hi = _bgp_crt_byte(f26["trit"], f26["fibo"])
    stored_bk = (bk_hi << 8) | bk_lo
    expected_bk = (bond_L ^ bond_R) & 0xFFFF
    bk_match = (stored_bk == expected_bk)

    return {
        "geo_key": geo_key,
        "geo_key_hex": "0x%x" % geo_key,
        "shape": shape,
        "bond_L": bond_L,
        "bond_L_hex": "0x%x" % bond_L,
        "bond_R": bond_R,
        "bond_R_hex": "0x%x" % bond_R,
        "bond_key": bond_L ^ bond_R,
        "stored_bk_low16": stored_bk,
        "expected_bk_low16": expected_bk,
        "bk_match": bk_match,
    }


def bond_addr_to_grid(addr: int) -> List[List[dict]]:
    grid = []
    for y in range(GP_GRID_W):
        row = []
        for x in range(GP_GRID_W):
            mix = addr ^ (y * GP_GRID_W + x)
            idx = ((mix ^ (mix >> 16)) % 65536)
            r, g, b = geo_pixel_encode(idx, GP_GRID_W)
            row.append({"r": r, "g": g, "b": b,
                        "hex": "#%02x%02x%02x" % (r, g, b)})
        grid.append(row)
    return grid


def bond_piece_to_grid(geo_key: int, shape: str,
                        bond_L: int, bond_R: int) -> List[List[dict]]:
    grid = bond_addr_to_grid(geo_key)
    fp_r, fp_g, fp_b = bond_piece_fingerprint(geo_key, shape, bond_L, bond_R)
    fp_hex = "#%02x%02x%02x" % (fp_r, fp_g, fp_b)

    # Overlay row 0 = fingerprint
    for x in range(GP_GRID_W):
        grid[0][x] = {"r": fp_r, "g": fp_g, "b": fp_b, "hex": fp_hex}

    # Overlay column 0 = bond_key gradient
    bk = bond_L ^ bond_R
    for y in range(1, GP_GRID_W):
        mix = (bk >> ((y % 8) * 8)) & 0xFF
        r, g, b = geo_pixel_encode(mix, GP_GRID_W)
        hex_s = "#%02x%02x%02x" % (r, g, b)
        grid[y][0] = {"r": r, "g": g, "b": b, "hex": hex_s}

    # Corner = XOR
    corner_idx = ((geo_key ^ bk) % 65536)
    r, g, b = geo_pixel_encode(corner_idx, GP_GRID_W)
    grid[0][0] = {"r": r, "g": g, "b": b,
                  "hex": "#%02x%02x%02x" % (r, g, b)}
    return grid


def run_roundtrip(seed: int) -> dict:
    random.seed(seed)
    geo_key = random.getrandbits(64)
    shapes = "IOTSZJL"
    shape = random.choice(shapes)
    bond_L = random.getrandbits(64)
    bond_R = random.getrandbits(64)

    stripe = bond_piece_to_stripe(geo_key, shape, bond_L, bond_R)
    pixels = [{"r": s["r"], "g": s["g"], "b": s["b"]} for s in stripe]
    decoded = bond_stripe_from_pixels(pixels)

    errors = 0
    if decoded["geo_key"] != geo_key:
        errors += 1
    if decoded["shape"] != shape:
        errors += 1
    if decoded["bond_L"] != bond_L:
        errors += 1
    if decoded["bond_R"] != bond_R:
        errors += 1

    return {
        "seed": seed,
        "original": {
            "geo_key": "0x%x" % geo_key,
            "shape": shape,
            "bond_L": "0x%x" % bond_L,
            "bond_R": "0x%x" % bond_R,
            "bond_key": "0x%x" % (bond_L ^ bond_R),
        },
        "decoded": decoded,
        "errors": errors,
        "pass": errors == 0,
        "stripe": stripe,
    }


# ── V2 (O4-style): 3 bytes/pixel via XOR with geometry ──

@lru_cache(maxsize=64)
def _geo_pixel_encode_slot(slot_idx: int, W: int = 27) -> tuple:
    idx_mod = slot_idx % W
    r = ((idx_mod % GP_TRIT_MOD) << 3) | (idx_mod % GP_SPOKE_MOD)
    g = ((idx_mod % GP_COSET_MOD) << 4) | (idx_mod % GP_LETTER_MOD & 0xF)
    b = idx_mod % GP_FIBO_MOD
    return (r & 0xFF, g & 0xFF, b & 0xFF)


def bond_piece_to_stripe_v2(geo_key: int, shape: str,
                             bond_L: int, bond_R: int) -> List[dict]:
    buf = bytearray(BGP_STRIPE_V2_BUF)
    for i in range(8):
        buf[i] = (geo_key >> (i * 8)) & 0xFF
    buf[8] = ord(shape) if shape else ord('I')
    for i in range(8):
        buf[9 + i] = (bond_L >> (i * 8)) & 0xFF
    for i in range(8):
        buf[17 + i] = (bond_R >> (i * 8)) & 0xFF
    bk = bond_L ^ bond_R
    buf[25] = bk & 0xFF
    buf[26] = (bk >> 8) & 0xFF

    stripe = []
    for i in range(BGP_STRIPE_V2_PX):
        geo_r, geo_g, geo_b = _geo_pixel_encode_slot(i, 27)
        b0 = buf[i * 3 + 0]
        b1 = buf[i * 3 + 1]
        b2 = buf[i * 3 + 2]
        pr = (geo_r ^ b0) & 0xFF
        pg = (geo_g ^ b1) & 0xFF
        pb = (geo_b ^ b2) & 0xFF
        stripe.append({
            "index": i, "slot": i,
            "rgb": "#%02x%02x%02x" % (pr, pg, pb),
            "r": pr, "g": pg, "b": pb,
            "byte0": b0, "byte1": b1, "byte2": b2,
            "geo_r": geo_r, "geo_g": geo_g, "geo_b": geo_b,
        })
    return stripe


def bond_stripe_from_pixels_v2(pixels: List[dict]) -> dict:
    buf = bytearray(BGP_STRIPE_V2_BUF)
    for i in range(BGP_STRIPE_V2_PX):
        geo_r, geo_g, geo_b = _geo_pixel_encode_slot(i, 27)
        buf[i * 3 + 0] = (pixels[i]["r"] ^ geo_r) & 0xFF
        buf[i * 3 + 1] = (pixels[i]["g"] ^ geo_g) & 0xFF
        buf[i * 3 + 2] = (pixels[i]["b"] ^ geo_b) & 0xFF

    geo_key = 0
    for i in range(8):
        geo_key |= buf[i] << (i * 8)
    shape = chr(buf[8]) if 32 <= buf[8] <= 126 else '?'
    bond_L = 0
    for i in range(8):
        bond_L |= buf[9 + i] << (i * 8)
    bond_R = 0
    for i in range(8):
        bond_R |= buf[17 + i] << (i * 8)

    stored_bk = (buf[26] << 8) | buf[25]
    expected_bk = (bond_L ^ bond_R) & 0xFFFF
    bk_match = (stored_bk == expected_bk)

    return {
        "geo_key": geo_key,
        "geo_key_hex": "0x%x" % geo_key,
        "shape": shape,
        "bond_L": bond_L,
        "bond_L_hex": "0x%x" % bond_L,
        "bond_R": bond_R,
        "bond_R_hex": "0x%x" % bond_R,
        "bond_key": bond_L ^ bond_R,
        "stored_bk_low16": stored_bk,
        "expected_bk_low16": expected_bk,
        "bk_match": bk_match,
        "version": 2,
        "pixels": BGP_STRIPE_V2_PX,
    }


# ── V3 (Frame-Predictive): 0.36x ratio — RGB generated on-demand ──

GPV3_SEED_BYTES = 9   # geo_key(8) + fold_axis(1)
GPV3_VIS_PX = 9       # 9 GeoPixel for visualization

POGLS_AXIS_SHAPE = [0, ord('I'), ord('O'), ord('T'), ord('S'), ord('Z'), ord('J'), ord('L')]
POGLS_BOND_SALT_L = 0xAAAAAAAAAAAAAAAA
POGLS_BOND_SALT_R = 0x5555555555555555


def _pogls_fibo_addr(seed: int) -> int:
    """Simplified fibo_addr — matches C implementation."""
    h = seed
    h ^= h >> 33; h = (h * 0xff51afd7ed558ccd) & 0xFFFFFFFFFFFFFFFF
    h ^= h >> 33; h = (h * 0xc4ceb9fe1a85ec53) & 0xFFFFFFFFFFFFFFFF
    h ^= h >> 33
    return h & 0xFFFFFFFFFFFFFFFF


def bond_piece_compress_v3(geo_key: int, shape: str) -> dict:
    fold_axis = 1  # default I
    for a, s in enumerate(POGLS_AXIS_SHAPE):
        if shape and s == ord(shape):
            fold_axis = a
            break
    return {
        "geo_key": geo_key,
        "geo_key_hex": "0x%x" % geo_key,
        "fold_axis": fold_axis,
        "shape": shape,
        "compressed_bytes": GPV3_SEED_BYTES,
        "original_bytes": 25,
        "ratio": GPV3_SEED_BYTES / 25,
        "_seed": geo_key.to_bytes(8, 'little') + bytes([fold_axis]),
    }


def bond_piece_decompress_v3(seed_bytes: bytes) -> dict:
    if len(seed_bytes) < GPV3_SEED_BYTES:
        raise ValueError(f"seed_bytes must be >= {GPV3_SEED_BYTES}")
    geo_key = int.from_bytes(seed_bytes[:8], 'little')
    axis = seed_bytes[8]
    shape = chr(POGLS_AXIS_SHAPE[axis]) if axis < len(POGLS_AXIS_SHAPE) and POGLS_AXIS_SHAPE[axis] else 'I'
    bond_L = _pogls_fibo_addr(geo_key ^ POGLS_BOND_SALT_L)
    bond_R = _pogls_fibo_addr(geo_key ^ POGLS_BOND_SALT_R)
    return {
        "geo_key": geo_key,
        "geo_key_hex": "0x%x" % geo_key,
        "shape": shape,
        "bond_L": bond_L,
        "bond_L_hex": "0x%x" % bond_L,
        "bond_R": bond_R,
        "bond_R_hex": "0x%x" % bond_R,
        "bond_key": bond_L ^ bond_R,
        "version": 3,
        "compressed_bytes": GPV3_SEED_BYTES,
    }


def bond_piece_visualize_v3(geo_key: int, shape: str, bond_L: int, bond_R: int) -> List[dict]:
    """Generate GeoPixel RGB on-demand — NEVER stored."""
    buf = bytearray(27)
    for i in range(8): buf[i] = (geo_key >> (i * 8)) & 0xFF
    buf[8] = ord(shape) if shape else ord('I')
    for i in range(8): buf[9 + i] = (bond_L >> (i * 8)) & 0xFF
    for i in range(8): buf[17 + i] = (bond_R >> (i * 8)) & 0xFF
    bk = bond_L ^ bond_R
    buf[25] = bk & 0xFF; buf[26] = (bk >> 8) & 0xFF

    stripe = []
    for i in range(GPV3_VIS_PX):
        geo_r, geo_g, geo_b = _geo_pixel_encode_slot(i, 27)
        stripe.append({
            "index": i,
            "rgb": "#%02x%02x%02x" % ((geo_r ^ buf[i*3]) & 0xFF,
                                       (geo_g ^ buf[i*3+1]) & 0xFF,
                                       (geo_b ^ buf[i*3+2]) & 0xFF),
            "r": (geo_r ^ buf[i*3]) & 0xFF,
            "g": (geo_g ^ buf[i*3+1]) & 0xFF,
            "b": (geo_b ^ buf[i*3+2]) & 0xFF,
        })
    return stripe


# ── Sparse residual codec + streaming/parallel batch ──

def _gp_predict_chunk(slot_idx: int, length: int) -> bytes:
    out = bytearray(length)
    for i in range(length):
        r, g, b = _geo_pixel_encode_slot(slot_idx + (i // 3), 27)
        out[i] = (r, g, b)[i % 3]
    return bytes(out)


def _gp_encode_chunk_record(item: tuple[int, bytes]) -> bytes:
    slot_idx, chunk = item
    pred = _gp_predict_chunk(slot_idx, len(chunk))
    mask = 0
    vals = bytearray()
    for i, (a, b) in enumerate(zip(chunk, pred)):
        if a != b:
            mask |= (1 << i)
            vals.append(a ^ b)

    if len(vals) == 0:
        mode = 0  # perfect prediction: only metadata, no payload
        payload = b""
    else:
        sparse_payload = mask.to_bytes(8, "little") + bytes(vals)
        if len(sparse_payload) < len(chunk):
            mode = 1  # sparse residual
            payload = sparse_payload
        else:
            mode = 2  # raw fallback
            payload = bytes(chunk)

    return struct.pack("<IHBH", slot_idx, len(chunk), mode, len(payload)) + payload


def gp_residual_encode_bytes(data: bytes, chunk_size: int = 64, workers: int = 1) -> bytes:
    if chunk_size < 1 or chunk_size > GP_RESIDUAL_MAX_CHUNK:
        raise ValueError(f"chunk_size must be 1..{GP_RESIDUAL_MAX_CHUNK}")
    if workers < 1:
        workers = 1

    n_chunks = (len(data) + chunk_size - 1) // chunk_size
    header = struct.pack(
        "<4sBHHQ",
        GP_RESIDUAL_MAGIC,
        GP_RESIDUAL_VERSION,
        chunk_size,
        n_chunks,
        len(data),
    )

    chunks = [data[i:i + chunk_size] for i in range(0, len(data), chunk_size)]
    out = bytearray(header)

    if workers == 1 or len(chunks) <= 1:
        for slot_idx, chunk in enumerate(chunks):
            out.extend(_gp_encode_chunk_record((slot_idx, chunk)))
        return bytes(out)

    with ThreadPoolExecutor(max_workers=workers) as ex:
        for rec in ex.map(_gp_encode_chunk_record, enumerate(chunks)):
            out.extend(rec)
    return bytes(out)


def gp_residual_encode_file(path: str, chunk_size: int = 64, workers: int = 1) -> bytes:
    if chunk_size < 1 or chunk_size > GP_RESIDUAL_MAX_CHUNK:
        raise ValueError(f"chunk_size must be 1..{GP_RESIDUAL_MAX_CHUNK}")
    total_len = os.path.getsize(path)
    n_chunks = (total_len + chunk_size - 1) // chunk_size
    header = struct.pack(
        "<4sBHHQ",
        GP_RESIDUAL_MAGIC,
        GP_RESIDUAL_VERSION,
        chunk_size,
        n_chunks,
        total_len,
    )
    out = bytearray(header)

    if workers < 1:
        workers = 1

    with open(path, "rb") as f:
        if workers == 1:
            slot_idx = 0
            while True:
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                out.extend(_gp_encode_chunk_record((slot_idx, chunk)))
                slot_idx += 1
            return bytes(out)

        # bounded streaming parallelism: read in a window, encode concurrently
        window = []
        slot_idx = 0
        with ThreadPoolExecutor(max_workers=workers) as ex:
            while True:
                while len(window) < workers * 4:
                    chunk = f.read(chunk_size)
                    if not chunk:
                        break
                    window.append(ex.submit(_gp_encode_chunk_record, (slot_idx, chunk)))
                    slot_idx += 1
                if not window:
                    break
                fut = window.pop(0)
                out.extend(fut.result())
            for fut in window:
                out.extend(fut.result())
    return bytes(out)


def _gp_decode_record(blob: bytes, offset: int) -> tuple[bytes, int]:
    if offset + 9 > len(blob):
        raise ValueError("truncated residual record")
    slot_idx, orig_len, mode, stored_len = struct.unpack_from("<IHBH", blob, offset)
    offset += 9
    payload = blob[offset:offset + stored_len]
    if len(payload) != stored_len:
        raise ValueError("truncated residual payload")
    offset += stored_len

    if orig_len > GP_RESIDUAL_MAX_CHUNK:
        raise ValueError("invalid chunk length")

    pred = _gp_predict_chunk(slot_idx, orig_len)
    if mode == 0:
        return pred, offset
    if mode == 1:
        if stored_len < 8:
            raise ValueError("invalid sparse payload")
        mask = int.from_bytes(payload[:8], "little")
        vals = payload[8:]
        out = bytearray(pred)
        vi = 0
        for i in range(orig_len):
            if mask & (1 << i):
                if vi >= len(vals):
                    raise ValueError("sparse residual underflow")
                out[i] ^= vals[vi]
                vi += 1
        if vi != len(vals):
            raise ValueError("sparse residual overflow")
        return bytes(out), offset
    if mode == 2:
        if stored_len != orig_len:
            raise ValueError("raw payload length mismatch")
        return payload, offset
    raise ValueError(f"unknown residual mode {mode}")


def gp_residual_decode_bytes(blob: bytes) -> bytes:
    if len(blob) < GPR1_HEADER_SZ:
        raise ValueError(f"truncated residual header: {len(blob)} < {GPR1_HEADER_SZ}")
    magic, version, chunk_size, n_chunks, total_len = struct.unpack_from("<4sBHHQ", blob, 0)
    if magic != GP_RESIDUAL_MAGIC:
        raise ValueError("bad residual magic")
    if version != GP_RESIDUAL_VERSION:
        raise ValueError("unsupported residual version")
    if chunk_size < 1 or chunk_size > GP_RESIDUAL_MAX_CHUNK:
        raise ValueError("invalid chunk size")

    out = bytearray()
    off = GPR1_HEADER_SZ  # 17
    for _ in range(n_chunks):
        chunk, off = _gp_decode_record(blob, off)
        out.extend(chunk)
    return bytes(out[:total_len])


# ── API Models ──────────────────────────────────────────

class EncodeRequest(BaseModel):
    idx: int = 42
    W: int = 256

class DecodeRequest(BaseModel):
    r: int = 128
    g: int = 64
    b: int = 32

class PieceRequest(BaseModel):
    geo_key: int = 0xDEADBEEFCAFE1234
    shape: str = "I"
    bond_L: int = 0x1111222233334444
    bond_R: int = 0xAAAA5555BBBB6666

class StripeDecodeRequest(BaseModel):
    pixels: List[dict]

class GridRequest(BaseModel):
    addr: int = 0xDEADBEEF

class RoundtripRequest(BaseModel):
    seed: int = 42
    count: int = 1

class UniquenessRequest(BaseModel):
    W: int = 27

class ResidualEncodeRequest(BaseModel):
    data_b64: str
    chunk_size: int = 64
    workers: int = 1

class ResidualDecodeRequest(BaseModel):
    blob_b64: str


# ════════════════════════════════════════════════════════
# FEATURE CLASS
# ════════════════════════════════════════════════════════

class GeoPixelFeature(EngineFeature):
    name = "GeoPixel Codec"
    description = "GeoPixel encode/decode, piece↔stripe bridge, residual codec, address grid, roundtrip verify"
    icon = "geopixel"

    def register_routes(self, app):
        router = APIRouter(prefix="/api/geopixel", tags=["geopixel"])

        @router.get("/info")
        def geopixel_info():
            # Check if C test binary exists
            test_exe = Path("C:/TPOGLS/test_bond_gp.exe")
            c_available = test_exe.exists()
            return {
                "status": "online",
                "constants": {
                    "GP_TRIT_MOD": GP_TRIT_MOD,
                    "GP_SPOKE_MOD": GP_SPOKE_MOD,
                    "GP_COSET_MOD": GP_COSET_MOD,
                    "GP_LETTER_MOD": GP_LETTER_MOD,
                    "GP_FIBO_MOD": GP_FIBO_MOD,
                    "GP_GRID_W": GP_GRID_W,
                    "BGP_STRIPE_W": BGP_STRIPE_W,
                    "BGP_STRIPE_BYTES": BGP_STRIPE_BYTES,
                    "GP_RESIDUAL_MAX_CHUNK": GP_RESIDUAL_MAX_CHUNK,
                },
                "c_bridge": {
                    "available": c_available,
                    "test_exe": str(test_exe) if c_available else None,
                },
            }

        @router.post("/encode")
        def geopixel_encode_endpoint(body: EncodeRequest):
            try:
                idx, W = body.idx, body.W
                r, g, b = geo_pixel_encode(idx, W)
                fields = geo_pixel_decode(r, g, b)
                return {
                    "input": {"idx": idx, "W": W},
                    "pixel": {"r": r, "g": g, "b": b,
                              "hex": "#%02x%02x%02x" % (r, g, b)},
                    "decoded_fields": fields,
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/decode")
        def geopixel_decode_endpoint(body: DecodeRequest):
            try:
                fields = geo_pixel_decode(body.r, body.g, body.b)
                trit, fibo = fields["trit"], fields["fibo"]
                recovered = _bgp_crt_byte(trit, fibo)
                return {
                    "pixel": {"r": body.r, "g": body.g, "b": body.b},
                    "fields": fields,
                    "crt_recovered_byte": recovered,
                    "crt_recovered_char": chr(recovered) if 32 <= recovered <= 126 else None,
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/fingerprint")
        def geopixel_fingerprint_endpoint(body: PieceRequest):
            try:
                r, g, b = bond_piece_fingerprint(
                    body.geo_key, body.shape, body.bond_L, body.bond_R)
                bk = body.bond_L ^ body.bond_R
                return {
                    "piece": {
                        "geo_key": "0x%x" % body.geo_key,
                        "shape": body.shape,
                        "bond_L": "0x%x" % body.bond_L,
                        "bond_R": "0x%x" % body.bond_R,
                        "bond_key": "0x%x" % bk,
                    },
                    "fingerprint": {
                        "r": r, "g": g, "b": b,
                        "hex": "#%02x%02x%02x" % (r, g, b),
                        "rgb888": (r << 16) | (g << 8) | b,
                    },
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/stripe")
        def geopixel_stripe_endpoint(body: PieceRequest):
            try:
                stripe = bond_piece_to_stripe(
                    body.geo_key, body.shape, body.bond_L, body.bond_R)
                return {
                    "piece": {
                        "geo_key": "0x%x" % body.geo_key,
                        "shape": body.shape,
                        "bond_L": "0x%x" % body.bond_L,
                        "bond_R": "0x%x" % body.bond_R,
                    },
                    "stripe": stripe,
                    "width": BGP_STRIPE_W,
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/stripe-decode")
        def geopixel_stripe_decode_endpoint(body: StripeDecodeRequest):
            try:
                _validate_stripe_pixels(body.pixels, BGP_STRIPE_W, "stripe-decode")
                result = bond_stripe_from_pixels(body.pixels[:BGP_STRIPE_W])
                return result
            except HTTPException:
                raise
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/stripe-v2")
        def geopixel_stripe_v2_endpoint(body: PieceRequest):
            try:
                stripe = bond_piece_to_stripe_v2(
                    body.geo_key, body.shape, body.bond_L, body.bond_R)
                return {
                    "piece": {
                        "geo_key": "0x%x" % body.geo_key,
                        "shape": body.shape,
                        "bond_L": "0x%x" % body.bond_L,
                        "bond_R": "0x%x" % body.bond_R,
                    },
                    "stripe": stripe,
                    "width": BGP_STRIPE_V2_PX,
                    "bytes_per_pixel": 3,
                    "data_bytes": BGP_STRIPE_V2_BUF,
                    "ratio": BGP_STRIPE_V2_BUF / (BGP_STRIPE_V2_PX * 3),
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/stripe-decode-v2")
        def geopixel_stripe_decode_v2_endpoint(body: StripeDecodeRequest):
            try:
                _validate_stripe_pixels(body.pixels, BGP_STRIPE_V2_PX, "stripe-decode-v2")
                result = bond_stripe_from_pixels_v2(body.pixels[:BGP_STRIPE_V2_PX])
                return result
            except HTTPException:
                raise
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/v3-info")
        def geopixel_v3_info():
            return {
                "version": 3,
                "compressed_bytes": GPV3_SEED_BYTES,
                "original_bytes": 25,
                "ratio": GPV3_SEED_BYTES / 25,
                "visualize_pixels": GPV3_VIS_PX,
                "storage": "geo_key(8B) + fold_axis(1B) = 9B",
                "visualization": "on-demand RGB (never stored)",
                "principle": "PoglsPiece is fully deterministic from (geo_key, fold_axis). "
                            "RGB/BMP is a deterministic projection — not actual storage.",
            }

        @router.post("/v3-compress")
        def geopixel_v3_compress(body: PieceRequest):
            try:
                result = bond_piece_compress_v3(body.geo_key, body.shape)
                return result
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/v3-visualize")
        def geopixel_v3_visualize(body: PieceRequest):
            try:
                vis = bond_piece_visualize_v3(
                    body.geo_key, body.shape, body.bond_L, body.bond_R)
                return {
                    "piece": {
                        "geo_key": "0x%x" % body.geo_key,
                        "shape": body.shape,
                    },
                    "visualization": vis,
                    "pixels": GPV3_VIS_PX,
                    "note": "Generated on-demand — NOT stored",
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/v3-roundtrip")
        def geopixel_v3_roundtrip(body: PieceRequest):
            try:
                compressed = bond_piece_compress_v3(body.geo_key, body.shape)
                seed_bytes = compressed["_seed"]
                decompressed = bond_piece_decompress_v3(seed_bytes)
                ok = (decompressed["geo_key"] == body.geo_key and
                      decompressed["shape"] == body.shape)
                return {
                    "original": {
                        "geo_key_hex": "0x%x" % body.geo_key,
                        "shape": body.shape,
                    },
                    "compressed": {
                        "bytes": GPV3_SEED_BYTES,
                        "ratio": GPV3_SEED_BYTES / 25,
                        "fold_axis": compressed["fold_axis"],
                    },
                    "decompressed": decompressed,
                    "roundtrip_ok": ok,
                    "note": "RGB/BMP not involved — pure deterministic reconstruct",
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.get("/residual-info")
        def geopixel_residual_info():
            return {
                "magic": GP_RESIDUAL_MAGIC.decode("ascii"),
                "version": GP_RESIDUAL_VERSION,
                "chunk_size_max": GP_RESIDUAL_MAX_CHUNK,
                "predictor": "deterministic slot-geometry XOR residuals",
                "modes": {
                    "0": "perfect prediction (metadata only)",
                    "1": "sparse residual (mask + XOR bytes)",
                    "2": "raw fallback",
                },
            }

        @router.post("/residual-encode")
        def geopixel_residual_encode(body: ResidualEncodeRequest):
            try:
                raw_b64_size = len(body.data_b64)
                _reject_oversized("residual-encode input (base64)",
                                  raw_b64_size, GEOPIXEL_MAX_BLOB_BYTES)
                data = base64.b64decode(body.data_b64)
                _reject_oversized("residual-encode input (decoded)",
                                  len(data), GEOPIXEL_MAX_INPUT_BYTES)
                blob = gp_residual_encode_bytes(
                    data, chunk_size=body.chunk_size, workers=body.workers)
                return {
                    "input_bytes": len(data),
                    "blob_b64": base64.b64encode(blob).decode("ascii"),
                    "blob_bytes": len(blob),
                    "ratio": (len(blob) / len(data)) if data else 0.0,
                    "chunk_size": body.chunk_size,
                    "workers": body.workers,
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/residual-decode")
        def geopixel_residual_decode(body: ResidualDecodeRequest):
            try:
                raw_b64_size = len(body.blob_b64)
                _reject_oversized("residual-decode blob (base64)",
                                  raw_b64_size, GEOPIXEL_MAX_BLOB_BYTES)
                blob = base64.b64decode(body.blob_b64)
                _reject_oversized("residual-decode blob (decoded)",
                                  len(blob), GEOPIXEL_MAX_BLOB_BYTES)
                data = gp_residual_decode_bytes(blob)
                return {
                    "output_bytes": len(data),
                    "data_b64": base64.b64encode(data).decode("ascii"),
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/grid")
        def geopixel_grid_endpoint(body: GridRequest):
            try:
                grid = bond_addr_to_grid(body.addr)
                return {
                    "addr": "0x%x" % body.addr,
                    "grid": grid,
                    "width": GP_GRID_W,
                    "height": GP_GRID_W,
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/piece-grid")
        def geopixel_piece_grid_endpoint(body: PieceRequest):
            try:
                grid = bond_piece_to_grid(
                    body.geo_key, body.shape, body.bond_L, body.bond_R)
                return {
                    "piece": {
                        "geo_key": "0x%x" % body.geo_key,
                        "shape": body.shape,
                        "bond_L": "0x%x" % body.bond_L,
                        "bond_R": "0x%x" % body.bond_R,
                    },
                    "grid": grid,
                    "width": GP_GRID_W,
                    "height": GP_GRID_W,
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/roundtrip")
        def geopixel_roundtrip_endpoint(body: RoundtripRequest):
            try:
                results = []
                ok = 0
                for i in range(body.count):
                    r = run_roundtrip(body.seed + i)
                    results.append(r)
                    if r["pass"]:
                        ok += 1
                return {
                    "count": body.count,
                    "pass": ok,
                    "fail": body.count - ok,
                    "results": results,
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/uniqueness")
        def geopixel_uniqueness_endpoint(body: UniquenessRequest):
            try:
                W = body.W
                seen = {}
                collisions = []
                for idx in range(W):
                    r, g, b = geo_pixel_encode(idx, W)
                    fields = geo_pixel_decode(r, g, b)
                    key = (fields["trit"], fields["coset"], fields["fibo"])
                    if key in seen:
                        collisions.append({
                            "idx_a": seen[key],
                            "idx_b": idx,
                            "shared": {
                                "trit": fields["trit"],
                                "coset": fields["coset"],
                                "fibo": fields["fibo"],
                            },
                        })
                    else:
                        seen[key] = idx
                return {
                    "W": W,
                    "unique_pixels": len(seen),
                    "collisions": len(collisions),
                    "collision_detail": collisions[:20],
                    "all_unique": len(collisions) == 0,
                }
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/grid-svg")
        def geopixel_grid_svg(body: GridRequest):
            """Return an inline SVG of the 27x27 address grid."""
            try:
                grid = bond_addr_to_grid(body.addr)
                cell_sz = 8
                w = GP_GRID_W * cell_sz
                h = GP_GRID_W * cell_sz
                rects = []
                for y in range(GP_GRID_W):
                    for x in range(GP_GRID_W):
                        px = grid[y][x]
                        rects.append(
                            '<rect x="%d" y="%d" width="%d" height="%d" '
                            'fill="%s" stroke="#1a1a1a" stroke-width="0.5"/>'
                            % (x * cell_sz, y * cell_sz, cell_sz, cell_sz, px["hex"]))
                svg = (
                    '<svg xmlns="http://www.w3.org/2000/svg" '
                    'width="%d" height="%d" shape-rendering="crispEdges">'
                    '<rect width="%%s" height="%%s" fill="#f5f0e8"/>'
                    '%s</svg>' % (w, h, "".join(rects))
                ) % (w, h)
                return {"svg": svg, "addr": "0x%x" % body.addr,
                        "width": w, "height": h}
            except Exception as e:
                raise HTTPException(500, str(e))

        @router.post("/piece-grid-svg")
        def geopixel_piece_grid_svg(body: PieceRequest):
            try:
                grid = bond_piece_to_grid(
                    body.geo_key, body.shape, body.bond_L, body.bond_R)
                cell_sz = 8
                w = GP_GRID_W * cell_sz
                h = GP_GRID_W * cell_sz
                rects = []
                for y in range(GP_GRID_W):
                    for x in range(GP_GRID_W):
                        px = grid[y][x]
                        rects.append(
                            '<rect x="%d" y="%d" width="%d" height="%d" '
                            'fill="%s" stroke="#1a1a1a" stroke-width="0.5"/>'
                            % (x * cell_sz, y * cell_sz, cell_sz, cell_sz, px["hex"]))
                svg = (
                    '<svg xmlns="http://www.w3.org/2000/svg" '
                    'width="%d" height="%d" shape-rendering="crispEdges">'
                    '<rect width="%%s" height="%%s" fill="#f5f0e8"/>'
                    '%s</svg>' % (w, h, "".join(rects))
                ) % (w, h)
                return {"svg": svg, "piece": {
                    "geo_key": "0x%x" % body.geo_key,
                    "shape": body.shape,
                }, "width": w, "height": h}
            except Exception as e:
                raise HTTPException(500, str(e))

        app.include_router(router)
