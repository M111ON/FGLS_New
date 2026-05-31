"""
pogls_bridge.py
─────────────────────────────────────────────────────────────────────
C↔Python zero-copy bridge for pogls_bond.so

Design principles:
  - ctypes Structures mirror exact C packed layout (25B PoglsPiece,
    58B PoglsSlot) — no serialization, no copies
  - All operations return Python-native types (int, bool, dict)
  - Library resolution: env POGLS_SO_PATH > adjacent .so/.dylib/.dll
  - Thread-safe nonce: session nonce set once at startup via set_nonce()
  - Lazy load: .so not opened until first call

Usage:
    from pogls_bridge import PoglsBridge

    br = PoglsBridge()                     # auto-finds .so
    br.set_nonce(0xDEAD000000000001)       # session nonce (optional)

    piece = br.make_piece(seed, axis=1)    # returns PieceView
    key   = br.bond_key(piece)
    valid, bk = br.bond_verify(piece_a, piece_b)

    slot  = br.make_slot(seed, axis=3, agent_id=1)
    br.plug_connect(slot_a, FACE_E, slot_b, FACE_W, ttl=64)
    br.reroute(slot, FAULT_OVERFLOW)
─────────────────────────────────────────────────────────────────────
"""

import ctypes
import hashlib
import os
import platform
from pathlib import Path
from typing import Optional, Tuple


# ── Face / Fault constants (mirror pogls_bond.h) ──────────────────
FACE_N = 0
FACE_S = 1
FACE_E = 2
FACE_W = 3

FAULT_OK        = 0
FAULT_OVERFLOW  = 1
FAULT_FAULT     = 2
FAULT_UPSTREAM  = 3
FAULT_RETRY     = 4

SHAPE_I = ord('I')
SHAPE_O = ord('O')
SHAPE_T = ord('T')
SHAPE_S = ord('S')
SHAPE_Z = ord('Z')
SHAPE_L = ord('L')
SHAPE_J = ord('J')

SHAPE_NAME = {
    SHAPE_I: 'I', SHAPE_O: 'O', SHAPE_T: 'T', SHAPE_S: 'S',
    SHAPE_Z: 'Z', SHAPE_L: 'L', SHAPE_J: 'J',
}


# ── ctypes struct mirrors (packed, matches C layout exactly) ───────

class _PoglsPiece(ctypes.Structure):
    """25-byte packed struct: geo_key(8) + shape(1) + bond_L(8) + bond_R(8)"""
    _pack_ = 1
    _fields_ = [
        ("geo_key", ctypes.c_uint64),
        ("shape",   ctypes.c_uint8),
        ("bond_L",  ctypes.c_uint64),
        ("bond_R",  ctypes.c_uint64),
    ]

class _PoglsPlug(ctypes.Structure):
    """
    12 bytes actual (C: target_id=0, face=4, ttl=6, active=8, pad to 12)
    NOTE: _pack_=1 on Piece only; Plug uses natural alignment → size=12
    """
    _fields_ = [
        ("target_id", ctypes.c_uint32),   # offset 0
        ("face",      ctypes.c_uint8),    # offset 4
        ("_pad1",     ctypes.c_uint8),    # offset 5  (alignment padding before ttl)
        ("ttl",       ctypes.c_uint16),   # offset 6
        ("active",    ctypes.c_uint8),    # offset 8
        ("_pad2",     ctypes.c_uint8 * 3),# offset 9-11 (pad to 12)
    ]

class _PoglsSlot(ctypes.Structure):
    """
    88 bytes actual:
      piece(25) + _pad(3) = 28  [plugs offset=28]
      plugs[4](12×4=48)   = 48  [agent_id offset=76]
      agent_id(4)              [token_cap offset=80]
      token_cap(4)             [rerouted offset=84]
      rerouted(1) + pad(3) = 4
      total = 88
    """
    _fields_ = [
        ("piece",      _PoglsPiece),       # 25B
        ("_pad_piece", ctypes.c_uint8 * 3),# 3B padding (align plugs to 4)
        ("plugs",      _PoglsPlug * 4),    # 48B (offset=28)
        ("agent_id",   ctypes.c_uint32),   # 4B  (offset=76)
        ("token_cap",  ctypes.c_uint32),   # 4B  (offset=80)
        ("rerouted",   ctypes.c_uint8),    # 1B  (offset=84)
        ("_pad_end",   ctypes.c_uint8 * 3),# 3B  (pad to 88)
    ]


# ── View wrappers (Python-friendly read access) ────────────────────

class PieceView:
    """Thin Python view over _PoglsPiece. Immutable snapshot."""
    __slots__ = ('geo_key', 'shape', 'bond_L', 'bond_R', '_raw')

    def __init__(self, raw: _PoglsPiece):
        self.geo_key = raw.geo_key
        self.shape   = raw.shape
        self.bond_L  = raw.bond_L
        self.bond_R  = raw.bond_R
        self._raw    = raw   # keep alive for pointer ops

    @property
    def shape_char(self) -> str:
        return SHAPE_NAME.get(self.shape, '?')

    @property
    def bond_key(self) -> int:
        return self.bond_L ^ self.bond_R

    def to_dict(self) -> dict:
        return {
            "geo_key":  f"{self.geo_key:016x}",
            "shape":    self.shape_char,
            "bond_L":   f"{self.bond_L:016x}",
            "bond_R":   f"{self.bond_R:016x}",
            "bond_key": f"{self.bond_key:016x}",
        }

    def __repr__(self):
        return (f"PieceView(shape={self.shape_char} "
                f"geo={self.geo_key:016x} bond_key={self.bond_key:016x})")


class SlotView:
    """Mutable view over _PoglsSlot. _raw is the live C struct."""
    __slots__ = ('_raw',)

    def __init__(self, raw: _PoglsSlot):
        self._raw = raw

    # ── piece proxy (read-only) ─────────────────────────────────
    @property
    def piece(self) -> PieceView:
        return PieceView(self._raw.piece)

    @property
    def agent_id(self) -> int:
        return self._raw.agent_id

    @property
    def token_cap(self) -> int:
        return self._raw.token_cap

    @property
    def rerouted(self) -> int:
        return self._raw.rerouted

    @property
    def shape_char(self) -> str:
        return SHAPE_NAME.get(self._raw.piece.shape, '?')

    def plug(self, face: int) -> dict:
        p = self._raw.plugs[face]
        return {
            "active":    bool(p.active),
            "target_id": p.target_id,
            "face":      p.face,
            "ttl":       p.ttl,
        }

    def to_dict(self) -> dict:
        face_names = {FACE_N:'N', FACE_S:'S', FACE_E:'E', FACE_W:'W'}
        return {
            "agent_id":  self.agent_id,
            "shape":     self.shape_char,
            "rerouted":  self.rerouted,
            "piece":     self.piece.to_dict(),
            "plugs": {
                face_names[f]: self.plug(f)
                for f in (FACE_N, FACE_S, FACE_E, FACE_W)
                if self._raw.plugs[f].active
            },
        }

    def __repr__(self):
        return (f"SlotView(agent={self.agent_id} shape={self.shape_char} "
                f"rerouted={self.rerouted})")


# ── Library loader ─────────────────────────────────────────────────

def _find_so() -> Path:
    """
    Locate pogls_bond shared library.
    Priority: POGLS_SO_PATH env > same dir as this file > cwd
    """
    env = os.environ.get("POGLS_SO_PATH")
    if env:
        p = Path(env)
        if p.exists():
            return p
        raise FileNotFoundError(f"POGLS_SO_PATH set but not found: {env}")

    sys_name = platform.system()
    names = {
        "Linux":  "pogls_bond.so",
        "Darwin": "pogls_bond.dylib",
        "Windows":"pogls_bond.dll",
    }.get(sys_name, "pogls_bond.so")

    candidates = [
        Path(__file__).resolve().parent / names,
        Path.cwd() / names,
    ]
    for c in candidates:
        if c.exists():
            return c

    raise FileNotFoundError(
        f"pogls_bond shared library not found. "
        f"Compile with:\n"
        f"  Linux/macOS: gcc -O2 -shared -fPIC -I. -o {names} pogls_bond_export.c\n"
        f"  Set POGLS_SO_PATH=/path/to/{names} to override search."
    )


def _load_lib(path: Path) -> ctypes.CDLL:
    lib = ctypes.CDLL(str(path))

    # fibo_addr
    lib.poglsex_fibo_addr.restype  = ctypes.c_uint64
    lib.poglsex_fibo_addr.argtypes = [ctypes.c_uint64]

    # seed_from_fp
    lib.poglsex_seed_from_fp.restype  = ctypes.c_uint64
    lib.poglsex_seed_from_fp.argtypes = [ctypes.c_char_p]

    # make_piece
    lib.poglsex_make_piece.restype  = None
    lib.poglsex_make_piece.argtypes = [ctypes.c_uint64, ctypes.c_uint8,
                                        ctypes.POINTER(_PoglsPiece)]

    # bond_key
    lib.poglsex_bond_key.restype  = ctypes.c_uint64
    lib.poglsex_bond_key.argtypes = [ctypes.POINTER(_PoglsPiece)]

    # bond_verify
    lib.poglsex_bond_verify.restype  = ctypes.c_uint8
    lib.poglsex_bond_verify.argtypes = [ctypes.POINTER(_PoglsPiece),
                                         ctypes.POINTER(_PoglsPiece),
                                         ctypes.POINTER(ctypes.c_uint64)]

    # make_slot
    lib.poglsex_make_slot.restype  = None
    lib.poglsex_make_slot.argtypes = [ctypes.c_uint64, ctypes.c_uint8,
                                       ctypes.c_uint32, ctypes.c_uint32,
                                       ctypes.POINTER(_PoglsSlot)]

    # plug_connect
    lib.poglsex_plug_connect.restype  = None
    lib.poglsex_plug_connect.argtypes = [ctypes.POINTER(_PoglsSlot), ctypes.c_uint8,
                                          ctypes.POINTER(_PoglsSlot), ctypes.c_uint8,
                                          ctypes.c_uint16]

    # plug_disconnect
    lib.poglsex_plug_disconnect.restype  = None
    lib.poglsex_plug_disconnect.argtypes = [ctypes.POINTER(_PoglsSlot), ctypes.c_uint8]

    # reroute
    lib.poglsex_reroute.restype  = None
    lib.poglsex_reroute.argtypes = [ctypes.POINTER(_PoglsSlot), ctypes.c_uint8]

    # nonce
    lib.poglsex_set_nonce.restype  = None
    lib.poglsex_set_nonce.argtypes = [ctypes.c_uint64]
    lib.poglsex_get_nonce.restype  = ctypes.c_uint64
    lib.poglsex_get_nonce.argtypes = []

    # version / config
    lib.poglsex_version.restype  = ctypes.c_char_p
    lib.poglsex_version.argtypes = []
    lib.poglsex_verify_bits.restype  = ctypes.c_int
    lib.poglsex_verify_bits.argtypes = []

    return lib


# ── Main bridge class ──────────────────────────────────────────────

class PoglsBridge:
    """
    Zero-copy Python bridge to pogls_bond.so.

    All structs live in C memory allocated here.
    No subprocess, no JSON serialization — direct pointer passing.
    """

    def __init__(self, so_path: Optional[str] = None):
        path = Path(so_path) if so_path else _find_so()
        self._lib  = _load_lib(path)
        self._path = path

    # ── info ────────────────────────────────────────────────────
    @property
    def version(self) -> str:
        return self._lib.poglsex_version().decode()

    @property
    def verify_bits(self) -> int:
        return self._lib.poglsex_verify_bits()

    # ── session nonce ───────────────────────────────────────────
    def set_nonce(self, nonce: int):
        """Set session nonce. Call once at startup for replay protection."""
        self._lib.poglsex_set_nonce(ctypes.c_uint64(nonce))

    def get_nonce(self) -> int:
        return self._lib.poglsex_get_nonce()

    # ── hash primitives ─────────────────────────────────────────
    def fibo_addr(self, seed: int) -> int:
        return self._lib.poglsex_fibo_addr(ctypes.c_uint64(seed))

    def seed_from_fp(self, topology_fp: str) -> int:
        return self._lib.poglsex_seed_from_fp(topology_fp.encode())

    # ── piece ───────────────────────────────────────────────────
    def make_piece(self, origin_seed: int, axis: int = 1) -> PieceView:
        raw = _PoglsPiece()
        self._lib.poglsex_make_piece(
            ctypes.c_uint64(origin_seed),
            ctypes.c_uint8(axis),
            ctypes.byref(raw)
        )
        return PieceView(raw)

    def bond_key(self, piece: PieceView) -> int:
        return self._lib.poglsex_bond_key(ctypes.byref(piece._raw))

    def bond_verify(self, a: PieceView, b: PieceView) -> Tuple[bool, int]:
        """
        Returns (valid: bool, bond_key: int).
        bond_key is the raw XOR — stable across nonces, safe for indexing.
        """
        bk = ctypes.c_uint64(0)
        valid = self._lib.poglsex_bond_verify(
            ctypes.byref(a._raw),
            ctypes.byref(b._raw),
            ctypes.byref(bk)
        )
        return bool(valid), bk.value

    # ── slot ────────────────────────────────────────────────────
    def make_slot(self, origin_seed: int, axis: int = 1,
                  agent_id: int = 0, token_cap: int = 300) -> SlotView:
        raw = _PoglsSlot()
        self._lib.poglsex_make_slot(
            ctypes.c_uint64(origin_seed),
            ctypes.c_uint8(axis),
            ctypes.c_uint32(agent_id),
            ctypes.c_uint32(token_cap),
            ctypes.byref(raw)
        )
        return SlotView(raw)

    def plug_connect(self, a: SlotView, face_a: int,
                     b: SlotView, face_b: int, ttl: int = 64):
        self._lib.poglsex_plug_connect(
            ctypes.byref(a._raw), ctypes.c_uint8(face_a),
            ctypes.byref(b._raw), ctypes.c_uint8(face_b),
            ctypes.c_uint16(ttl)
        )

    def plug_disconnect(self, slot: SlotView, face: int):
        self._lib.poglsex_plug_disconnect(
            ctypes.byref(slot._raw), ctypes.c_uint8(face)
        )

    def reroute(self, slot: SlotView, fault: int):
        self._lib.poglsex_reroute(
            ctypes.byref(slot._raw), ctypes.c_uint8(fault)
        )

    def __repr__(self):
        return (f"PoglsBridge(so={self._path.name} "
                f"v{self.version} verify_bits={self.verify_bits})")


def _is_printable_ratio(data: bytes) -> float:
    if not data:
        return 0.0
    printable = sum(1 for b in data if b in (9, 10, 13) or 32 <= b <= 126)
    return printable / len(data)


def _guess_content_type(data: bytes) -> str:
    if not data:
        return "binary"
    if data[:4] == b"GGUF":
        return "gguf"
    if _is_printable_ratio(data) >= 0.9:
        txt = data[:4096].decode("utf-8", errors="ignore").lstrip()
        if txt.startswith("{") or txt.startswith("["):
            return "json"
        return "text"
    return "binary"


def _fib_set(limit: int) -> set[int]:
    fibs = {1, 2}
    a, b = 1, 2
    while b <= limit:
        a, b = b, a + b
        fibs.add(b)
    return fibs


def topology_scan_multiscale(data: bytes) -> dict:
    """Classify a blob into a compact topology summary."""
    content_type = _guess_content_type(data)
    scales = (16, 32, 64, 128)
    profile = []
    fibs = _fib_set(max(len(data) // 2, 1))

    for scale in scales:
        if scale <= 0:
            continue
        chunks = [data[i:i + scale] for i in range(0, len(data), scale) if data[i:i + scale]]
        if not chunks:
            profile.append({
                "scale": scale,
                "total_chunks": 0,
                "unique_chunks": 0,
                "intra_dedup": 0.0,
                "phi_ratio": 0.0,
            })
            continue

        seen: dict[bytes, list[int]] = {}
        for idx, chunk in enumerate(chunks):
            seen.setdefault(chunk, []).append(idx)

        total_chunks = len(chunks)
        unique_chunks = len(seen)
        repeated_chunks = total_chunks - unique_chunks

        phi_hits = 0
        phi_total = 0
        for positions in seen.values():
            if len(positions) < 2:
                continue
            for a, b in zip(positions, positions[1:]):
                gap = b - a
                phi_total += 1
                if gap in fibs:
                    phi_hits += 1

        intra_dedup = repeated_chunks / total_chunks if total_chunks else 0.0
        phi_ratio = phi_hits / phi_total if phi_total else 0.0
        profile.append({
            "scale": scale,
            "total_chunks": total_chunks,
            "unique_chunks": unique_chunks,
            "intra_dedup": round(intra_dedup, 4),
            "phi_ratio": round(phi_ratio, 4),
        })

    best = max(profile, key=lambda row: (row["intra_dedup"], row["phi_ratio"], -row["scale"])) if profile else {
        "scale": 32,
        "intra_dedup": 0.0,
        "phi_ratio": 0.0,
    }

    if content_type in {"text", "json", "gguf"}:
        routing_hint = "woven"
        access_mode = "random-access"
    elif best["intra_dedup"] >= 0.35:
        routing_hint = "sequential"
        access_mode = "streaming"
    else:
        routing_hint = "novel"
        access_mode = "store-all"

    topology_fp = hashlib.sha256(data).hexdigest()
    geometry = {
        "topology_fp": topology_fp,
        "content_type": content_type,
        "best_scale": best["scale"],
        "routing_hint": routing_hint,
        "access_mode": access_mode,
        "intra_dedup": best["intra_dedup"],
        "phi_ratio": best["phi_ratio"],
        "scale_profile": profile,
    }

    try:
        br = PoglsBridge()
        seed = br.seed_from_fp(topology_fp)
        piece = br.make_piece(seed, axis=1)
        geometry["geometry"] = piece.to_dict()
        geometry["bond_key"] = f"{piece.bond_key:016x}"
        geometry["shape"] = piece.shape_char
    except Exception:
        geometry["geometry"] = {
            "geo_key": topology_fp[:16],
            "shape": "?",
            "bond_L": "0" * 16,
            "bond_R": "0" * 16,
            "bond_key": "0" * 16,
        }

    return geometry
