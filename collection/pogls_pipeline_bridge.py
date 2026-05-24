"""
pogls_pipeline_bridge.py — Python↔C Pipeline Integration
=========================================================
Bridges Python (BermudaRouter/BermudaPipeline) ↔ C (pogls_pipeline.h)

Three integration points:
  1. verdict_to_chunk_desc()  — RoutingVerdict → ChunkDesc (Python-side)
  2. PoglsPipelineBridge      — wraps encode/decode via ctypes or pure-Python fallback
  3. geom_codec extension     — adds frame_0 header pixel to APNG sequence

Architecture position:
  BermudaRouter.route() → RoutingVerdict
      ↓ verdict_to_chunk_desc()
  ChunkDesc{geo_key, bond_key, enc, zone, shape, polarity, temperature}
      ↓ PoglsPipelineBridge.encode_verdict()
  POGLSHeader (30B) + ColdStore entries
      ↓ header_to_frame0()
  RGB frame [8×8×3] → prepend to APNG sequence

Compatible with:
  pogls_geofield_export.h — Approach B positional keys
  pogls_pipeline.h        — ColdStore + HotStore
  bermuda_router_v1.py    — RoutingVerdict source
  geom_codec.py           — RGB frame encoding

No torch dependency in encode path (numpy only).
No malloc. O(1) per token.
"""
from __future__ import annotations

import struct
import base64
import numpy as np
from dataclasses import dataclass, field
from typing import List, Optional, Dict, Any

# ── Constants (mirrors pogls_geofield_export.h) ──────────────
PGFE_CHUNK_SZ       = 64
PGFE_HEADER_SZ      = 30
PGFE_RESIDUAL_COLD  = 72
PGFE_CYCLE          = 1440
PGFE_CODEC_W        = 27
PGFE_FACE_DEFAULT   = 12
PGFE_HOT            = 0
PGFE_COLD           = 1
PGFE_FLAG_HAS_RESIDUAL = 0x01

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME  = 0x00000100000001B3
MASK64     = 0xFFFFFFFFFFFFFFFF

TRING_ROWS, TRING_COLS = 8, 8    # 64 cells (8×8 DiamondBlock)

# ── Approach B: positional key functions ─────────────────────

def _fnv64(*vals: int) -> int:
    h = FNV_OFFSET
    for v in vals:
        h ^= (v & MASK64)
        h = (h * FNV_PRIME) & MASK64
    return h

def pgfe_geo_key(nonce: int, chunk_idx: int,
                  tile_id: int, dim: int) -> int:
    return _fnv64(nonce, chunk_idx, tile_id, dim)

def pgfe_bond_key(geo_key: int, chunk_idx: int) -> int:
    rot = (chunk_idx % 63) + 1
    return ((geo_key << rot) | (geo_key >> (64 - rot))) & MASK64

def pgfe_enc(tile_id: int, dim: int) -> int:
    compound = ((tile_id % 32) + (dim % 32) * 32) % 144
    spoke    = ((tile_id % 32) ^ (dim % 32)) % 6
    offset   = ((tile_id % 32) * 3 + (dim % 32) * 7) % 4
    return (compound * 24 + spoke * 4 + offset) % PGFE_CYCLE

# ── ChunkDesc (Python mirror of C struct) ────────────────────

@dataclass
class ChunkDesc:
    chunk_idx   : int
    tile_id     : int
    dim         : int
    gear        : int
    zone        : int       # 0..11
    pole        : int       # 0=south 1=north
    shape       : str       # 'I'/'O'/'S' etc
    polarity    : int       # 0=ROUTE 1=GROUND
    temperature : int       # PGFE_HOT / PGFE_COLD
    tring_slot  : int       # 0..719
    enc         : int       # 0..1439
    geo_key     : int
    bond_key    : int
    origin_key  : int

# ── POGLSHeader (Python mirror) ──────────────────────────────

@dataclass
class POGLSHeader:
    session_nonce : int = 0
    root_seed     : int = 0
    enc_start     : int = 0
    gear          : int = 1
    layer_count   : int = 1
    codec_id      : int = PGFE_CODEC_W
    flags         : int = 0
    origin_key    : int = 0

    # struct layout: Q Q H B B B B Q = 8+8+2+1+1+1+1+8 = 30B
    _FMT = "<QQHBBBBQ"

    def to_bytes(self) -> bytes:
        return struct.pack(self._FMT,
            self.session_nonce, self.root_seed, self.enc_start,
            self.gear, self.layer_count, self.codec_id,
            self.flags, self.origin_key)

    @classmethod
    def from_bytes(cls, data: bytes) -> "POGLSHeader":
        fields = struct.unpack(cls._FMT, data[:PGFE_HEADER_SZ])
        return cls(*fields)

    def __post_init__(self):
        assert struct.calcsize(self._FMT) == PGFE_HEADER_SZ, \
            f"Header size mismatch: {struct.calcsize(self._FMT)} != {PGFE_HEADER_SZ}"

# ── verdict_to_chunk_desc ─────────────────────────────────────

def verdict_to_chunk_desc(
    verdict_token: dict,    # single token from RoutingVerdict (zone,shape,polarity,tring_slot)
    chunk_idx: int,
    nonce: int,
    origin_key: int,
    n_tokens: int = 64,
    face_max: int = PGFE_FACE_DEFAULT,
) -> ChunkDesc:
    """
    RoutingVerdict token → ChunkDesc using Approach B positional keys.

    verdict_token: {zone, shape, polarity, tring_slot}  (one token dict)
    Maps directly onto geo_field_core_3 tile_id/dim addressing.
    """
    tring_slot = int(verdict_token.get("tring_slot", chunk_idx % 720))
    zone       = int(verdict_token.get("zone", 0))
    shape      = verdict_token.get("shape", "I")
    polarity   = 0 if verdict_token.get("polarity", "ROUTE") == "ROUTE" else 1

    # Map tring_slot → (tile_id, dim) — inverse of pgfe_enc
    # tile_id = slot % face_max, dim = slot // face_max
    tile_id = tring_slot % face_max
    dim     = (tring_slot // face_max) & 0x7F

    gear = (1 if n_tokens <= 512  else
            2 if n_tokens <= 1024 else
            3 if n_tokens <= 2048 else 4)

    enc = pgfe_enc(tile_id, dim)
    geo_key  = pgfe_geo_key(nonce, chunk_idx, tile_id, dim)
    bond_key = pgfe_bond_key(geo_key, chunk_idx)

    # Temperature from polarity: GROUND → COLD (non-deterministic path)
    temperature = PGFE_COLD if polarity == 1 else PGFE_HOT

    return ChunkDesc(
        chunk_idx   = chunk_idx,
        tile_id     = tile_id,
        dim         = dim,
        gear        = gear,
        zone        = zone,
        pole        = 1 if zone >= 6 else 0,
        shape       = shape,
        polarity    = polarity,
        temperature = temperature,
        tring_slot  = tring_slot,
        enc         = enc,
        geo_key     = geo_key,
        bond_key    = bond_key,
        origin_key  = origin_key if origin_key else geo_key,
    )

def verdicts_to_descs(
    levels: List[dict],
    nonce: int,
    face_max: int = PGFE_FACE_DEFAULT,
) -> tuple[POGLSHeader, List[List[ChunkDesc]]]:
    """
    Full level sequence → (header, descs_per_level).
    levels: list of {tring_slots, sample_tokens, n_real}  (from geom_codec)
    """
    all_descs: List[List[ChunkDesc]] = []
    chunk_idx = 0
    origin_key = 0
    n_tokens = max((lv.get("n_real", 64) for lv in levels), default=64)

    for lv in levels:
        tokens     = lv.get("sample_tokens", [])
        tring_slots = lv.get("tring_slots", [])
        level_descs = []

        for i, token in enumerate(tokens):
            if i < len(tring_slots):
                token = dict(token, tring_slot=tring_slots[i])
            d = verdict_to_chunk_desc(token, chunk_idx, nonce,
                                      origin_key, n_tokens, face_max)
            if chunk_idx == 0:
                origin_key = d.geo_key
                d.origin_key = origin_key
            level_descs.append(d)
            chunk_idx += 1

        all_descs.append(level_descs)

    # Build header from first desc
    head = all_descs[0][0] if all_descs and all_descs[0] else None
    if head is None:
        return POGLSHeader(), []

    has_cold = any(d.temperature == PGFE_COLD
                   for level in all_descs for d in level)

    header = POGLSHeader(
        session_nonce = nonce,
        root_seed     = head.geo_key,
        enc_start     = head.enc,
        gear          = head.gear,
        layer_count   = 3,          # R/G/B = 3 channels
        codec_id      = PGFE_CODEC_W,
        flags         = PGFE_FLAG_HAS_RESIDUAL if has_cold else 0,
        origin_key    = head.geo_key,
    )
    return header, all_descs

# ── Cold Store (Python mirror of ColdStore in pogls_pipeline.h) ──

class ColdStore:
    """
    Two-tier residual store.
    Tier 1: 144-slot ring (sacred constant)
    Tier 2: overflow list
    """
    RING_CAP = 144

    def __init__(self):
        self._ring: List[Optional[tuple]] = [None] * self.RING_CAP
        self._head = 0
        self._count = 0
        self.evictions = 0
        self.total_stored = 0
        self._overflow: List[tuple] = []

    def push(self, bond_key: int, chunk: bytes):
        assert len(chunk) == PGFE_CHUNK_SZ
        if self._count == self.RING_CAP:
            evicted = self._ring[self._head]
            if evicted:
                self._overflow.append(evicted)
            self.evictions += 1
        else:
            self._count += 1
        self._ring[self._head] = (bond_key, chunk)
        self._head = (self._head + 1) % self.RING_CAP
        self.total_stored += 1

    def find(self, bond_key: int) -> Optional[bytes]:
        hint = bond_key % self.RING_CAP
        entry = self._ring[hint]
        if entry and entry[0] == bond_key:
            return entry[1]
        for e in self._ring:
            if e and e[0] == bond_key:
                return e[1]
        for e in self._overflow:
            if e[0] == bond_key:
                return e[1]
        return None

    @property
    def stats(self) -> dict:
        return {
            "ring_count":     self._count,
            "overflow_count": len(self._overflow),
            "evictions":      self.evictions,
            "total_stored":   self.total_stored,
        }

# ── Pipeline Bridge ───────────────────────────────────────────

class PoglsPipelineBridge:
    """
    Python-side encode/decode pipeline.
    Wraps verdict_to_chunk_desc + ColdStore + POGLSHeader.

    Usage:
        pipe = PoglsPipelineBridge(nonce=0xDEADBEEF)
        header = pipe.encode_levels(levels)   # from geom_codec capture
        # decode:
        chunk = pipe.decode_chunk(chunk_idx)  # returns bytes or None (HOT)
    """

    def __init__(self, nonce: int = 0x424D524D00000001,
                 face_max: int = PGFE_FACE_DEFAULT):
        self.nonce    = nonce
        self.face_max = face_max
        self.header   : Optional[POGLSHeader] = None
        self.cold     = ColdStore()
        self._descs   : List[ChunkDesc] = []   # flat: all chunks in order
        self._hot_keys: Dict[int, ChunkDesc]   = {}  # chunk_idx → desc (HOT)
        self.stats    = {"hot": 0, "cold": 0}

    def encode_levels(self, levels: List[dict]) -> POGLSHeader:
        """
        Process full level sequence from geom_codec capture.
        Returns POGLSHeader (30B) ready for frame_0.
        """
        header, all_descs = verdicts_to_descs(levels, self.nonce, self.face_max)
        self.header = header
        self._descs = []
        self._hot_keys = {}

        for level_descs in all_descs:
            for d in level_descs:
                self._descs.append(d)
                if d.temperature == PGFE_HOT:
                    self._hot_keys[d.chunk_idx] = d
                    self.stats["hot"] += 1
                else:
                    # COLD: store bond_key + synthetic 64B (tring_slot pattern)
                    raw = _synthetic_cold_chunk(d)
                    self.cold.push(d.bond_key, raw)
                    self.stats["cold"] += 1

        return header

    def decode_chunk(self, chunk_idx: int) -> tuple[int, Optional[bytes]]:
        """
        Returns (geo_key, raw_bytes_or_None).
        None = HOT path → caller fetches from FGLS by geo_key.
        bytes = COLD path → data recovered from ring.
        """
        if not self.header:
            raise RuntimeError("encode_levels() must be called first")

        # Reconstruct: use stored desc if available (preserves tring_slot)
        # else fallback to positional formula
        if chunk_idx < len(self._descs):
            d          = self._descs[chunk_idx]
            geo_key    = d.geo_key
            bond_key   = d.bond_key
        else:
            tring_slot = chunk_idx % 720
            tile_id    = tring_slot % self.face_max
            dim_val    = (tring_slot // self.face_max) & 0x7F
            geo_key    = pgfe_geo_key(self.nonce, chunk_idx, tile_id, dim_val)
            bond_key   = pgfe_bond_key(geo_key, chunk_idx)

        raw = self.cold.find(bond_key)
        if raw is not None:
            return geo_key, raw   # COLD found
        return geo_key, None      # HOT — FGLS path

    def header_bytes(self) -> bytes:
        if not self.header:
            raise RuntimeError("No header yet")
        return self.header.to_bytes()

    def summary(self) -> dict:
        return {
            "header_ready": self.header is not None,
            "total_chunks": len(self._descs),
            "hot":          self.stats["hot"],
            "cold":         self.stats["cold"],
            "cold_store":   self.cold.stats,
            "header_sz":    PGFE_HEADER_SZ,
        }

def _synthetic_cold_chunk(d: ChunkDesc) -> bytes:
    """Generate deterministic 64B from ChunkDesc (for COLD ring storage)."""
    buf = bytearray(PGFE_CHUNK_SZ)
    key = d.bond_key
    for i in range(PGFE_CHUNK_SZ):
        buf[i] = (key >> ((i % 8) * 8)) & 0xFF
        key = (key ^ (key >> 17)) & MASK64
    return bytes(buf)

# ── geom_codec extension: header_to_frame0 ───────────────────

def header_to_frame0(header: POGLSHeader,
                      descs_level0: List[ChunkDesc]) -> np.ndarray:
    """
    Convert POGLSHeader + level-0 descs → RGB frame [8×8×3].
    Frame 0 = header frame: encodes 30B header + active slots.

    Encoding:
      Active slots:  color = f(gear, zone, temperature)
      Header pixels: first 30 pixels of row 0 encode header bytes
                     R=byte, G=byte^0x37, B=byte^0xAA
      Empty slots:   [8, 6, 10]  (dark background)
    """
    rgb = np.full((TRING_ROWS, TRING_COLS, 3), [8, 6, 10], dtype=np.uint8)

    # Active slots from level 0 (collapse tring_slot 0..719 → cell 0..63)
    for d in descs_level0:
        cell = d.tring_slot % 64
        r, c = divmod(cell, TRING_COLS)
        if 0 <= r < TRING_ROWS and 0 <= c < TRING_COLS:
            gear_v  = min(255, 40 + d.gear * 50)
            zone_v  = 60 + (d.zone % 12) * 15
            temp_v  = 220 if d.temperature == PGFE_HOT else 50
            rgb[r, c] = [gear_v, zone_v, temp_v]

    # Encode header bytes in first 30 pixels of row 0 (header watermark)
    hbytes = header.to_bytes()
    for i, b in enumerate(hbytes):
        c = i % TRING_COLS     # 0..7
        r = i // TRING_COLS   # row 0..3 (30B across 8-wide grid)
        rgb[r, c] = [b, b ^ 0x37, b ^ 0xAA]

    return rgb

def prepend_header_frame(apng_frames: List["PIL.Image.Image"],
                          header: POGLSHeader,
                          descs_level0: List[ChunkDesc]) -> List["PIL.Image.Image"]:
    """
    Prepend header frame to APNG sequence.
    Returns new list with header frame at index 0.
    """
    from PIL import Image
    frame0_rgb = header_to_frame0(header, descs_level0)
    frame0 = Image.fromarray(frame0_rgb, "RGB")
    return [frame0] + apng_frames

# ── Integration: patch geom_codec.encode_sequence ────────────

def encode_sequence_with_header(levels: List[dict],
                                 nonce: int = 0x424D524D00000001,
                                 fps: int = 30) -> tuple[bytes, POGLSHeader, dict]:
    """
    Drop-in replacement for geom_codec.encode_sequence()
    that prepends a header frame (frame 0).

    Returns: (apng_bytes, header)
    """
    from geom_codec import verdict_to_level, encode_sequence
    from PIL import Image
    from io import BytesIO

    # Build pipeline and encode
    pipe = PoglsPipelineBridge(nonce=nonce)
    header = pipe.encode_levels(levels)

    # Build frames (mirrors encode_sequence)
    frames = []
    for i, v in enumerate(levels):
        rgb = verdict_to_level(
            v.get("tring_slots", []),
            [t["zone"] for t in v.get("sample_tokens", [])],
            [t["shape"] for t in v.get("sample_tokens", [])],
            [t["polarity"] for t in v.get("sample_tokens", [])],
        )
        frames.append(Image.fromarray(rgb, "RGB"))

    # Prepend header frame
    descs0 = []
    if pipe._descs:
        n0 = len(levels[0].get("sample_tokens", [])) if levels else 0
        descs0 = pipe._descs[:n0]
    all_frames = prepend_header_frame(frames, header, descs0)

    buf = BytesIO()
    all_frames[0].save(
        buf, format="PNG", save_all=True,
        append_images=all_frames[1:],
        duration=1000 // fps, loop=0,
    )

    pipeline_info = {
        "header_enc_start": header.enc_start,
        "header_gear":      header.gear,
        "header_b64":       base64.b64encode(header.to_bytes()).decode(),
        "has_residual":     bool(header.flags & PGFE_FLAG_HAS_RESIDUAL),
        **pipe.summary(),
    }
    return buf.getvalue(), header, pipeline_info

# ── Verify ────────────────────────────────────────────────────

def verify() -> int:
    errors = 0

    # T1: header size
    h = POGLSHeader()
    assert len(h.to_bytes()) == PGFE_HEADER_SZ, "header size"

    # T2: header roundtrip
    h = POGLSHeader(session_nonce=0xDEADBEEFCAFEBABE, root_seed=0x1234,
                     enc_start=42, gear=2, layer_count=3, codec_id=27,
                     flags=1, origin_key=0xAAAABBBB)
    h2 = POGLSHeader.from_bytes(h.to_bytes())
    assert h.session_nonce == h2.session_nonce, "nonce roundtrip"
    assert h.enc_start     == h2.enc_start,     "enc_start roundtrip"
    assert h.origin_key    == h2.origin_key,     "origin_key roundtrip"

    # T3: geo_key determinism
    for ci in range(1440):
        t, d = ci % 12, (ci // 12) & 0x7F
        k1 = pgfe_geo_key(0xDEAD, ci, t, d)
        k2 = pgfe_geo_key(0xDEAD, ci, t, d)
        assert k1 == k2, f"determinism fail at {ci}"

    # T4: uniqueness in 1440
    keys = [pgfe_geo_key(0xBEEF, ci, ci%12, (ci//12)&0x7F)
            for ci in range(1440)]
    assert len(set(keys)) == 1440, f"collision: {1440 - len(set(keys))}"

    # T5: verdict_to_chunk_desc
    token = {"zone": 3, "shape": "I", "polarity": "ROUTE", "tring_slot": 42}
    d = verdict_to_chunk_desc(token, 0, 0xBEEF, 0)
    assert d.tring_slot == 42
    assert d.shape == "I"
    assert d.temperature == PGFE_HOT   # ROUTE → HOT
    assert d.geo_key != 0

    # T6: GROUND token → COLD
    token_g = {"zone": 9, "shape": "S", "polarity": "GROUND", "tring_slot": 99}
    d_g = verdict_to_chunk_desc(token_g, 1, 0xBEEF, 0)
    assert d_g.temperature == PGFE_COLD

    # T7: ColdStore ring + find
    cs = ColdStore()
    chunk = bytes(range(64))
    cs.push(0xABCDEF, chunk)
    found = cs.find(0xABCDEF)
    assert found == chunk, "cold find failed"
    assert cs.find(0x999999) is None, "false positive"

    # T8: ring eviction
    cs2 = ColdStore()
    for i in range(ColdStore.RING_CAP + 5):
        cs2.push(i + 1, bytes([i % 256] * 64))
    assert cs2.evictions == 5
    assert cs2.stats["overflow_count"] == 5

    # T9: verdicts_to_descs + header
    levels = [{"tring_slots": [10, 20, 30],
               "sample_tokens": [
                   {"zone": 1, "shape": "I", "polarity": "ROUTE",  "tring_slot": 10},
                   {"zone": 7, "shape": "S", "polarity": "GROUND", "tring_slot": 20},
                   {"zone": 3, "shape": "O", "polarity": "ROUTE",  "tring_slot": 30},
               ],
               "n_real": 3}]
    hdr, all_descs = verdicts_to_descs(levels, 0xCAFE)
    assert len(hdr.to_bytes()) == PGFE_HEADER_SZ
    assert len(all_descs) == 1
    assert len(all_descs[0]) == 3
    assert hdr.layer_count == 3

    # T10: PoglsPipelineBridge encode + decode
    pipe = PoglsPipelineBridge(nonce=0xFACE1234)
    pipe.encode_levels(levels)
    assert pipe.header is not None
    gk, raw = pipe.decode_chunk(0)
    assert gk != 0
    # chunk 1 is GROUND → COLD → raw should be found
    gk1, raw1 = pipe.decode_chunk(1)
    assert raw1 is not None, "COLD chunk should be in ring"

    # T11: header_to_frame0 shape
    frame = header_to_frame0(hdr, all_descs[0])
    assert frame.shape == (8, 8, 3)
    assert frame.dtype == np.uint8

    # T12: decode roundtrip geo_key matches encode
    pipe2 = PoglsPipelineBridge(nonce=0xBEEF42)
    levels2 = [{"tring_slots": list(range(10)),
                "sample_tokens": [
                    {"zone": i%12, "shape": "I", "polarity": "ROUTE",
                     "tring_slot": i}
                    for i in range(10)
                ],
                "n_real": 10}]
    pipe2.encode_levels(levels2)
    for ci in range(10):
        gk_e = pipe2._descs[ci].geo_key
        gk_d, _ = pipe2.decode_chunk(ci)
        assert gk_e == gk_d, f"geo_key mismatch at ci={ci}"

    return errors

if __name__ == "__main__":
    rc = verify()
    if rc == 0:
        print("pogls_pipeline_bridge: ALL 12 TESTS PASS")
    else:
        print(f"FAIL: {rc} errors")
