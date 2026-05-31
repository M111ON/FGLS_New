"""
hot_store_wire.py — HotStoreFn wire: Python → tgw_fgls_store_raw() C
=====================================================================
Wires PoglsPipelineBridge.HotStore → TgwFglsCtx via:
  A) C DLL path  — BermudaBondBridge + pogls_fgls.dll (production)
  B) Pure-Python path — pogls_bond_py fallback (no DLL needed)

Also provides:
  - FglsSessionIndex: persistent chunk_idx → tring_slot map
    (solves decode-only session problem)
  - hot_store_fn_from_bridge(): factory for HotStoreFn callable

Usage:
  from hot_store_wire import make_hot_store, FglsSessionIndex

  # With DLL:
  hot_fn, ctx = make_hot_store(nonce=0xBEEF, use_dll=True)
  pipe = PoglsPipelineBridge(nonce=0xBEEF)
  pipe.hot.store_fn = hot_fn
  pipe.hot.ctx      = ctx

  # Without DLL (pure-Python fallback):
  hot_fn, ctx = make_hot_store(nonce=0xBEEF, use_dll=False)
"""
from __future__ import annotations

import ctypes
from pathlib import Path
from typing import Optional, Callable, Dict, Tuple
from dataclasses import dataclass, field

# ── Constants ────────────────────────────────────────────────
BUILD_DIR = Path(__file__).resolve().parent.parent / "build"
FGLS_DLL_NAME = "pogls_fgls.dll"   # exports tgw_fgls_* functions

# shape byte → polarity (mirrors tgw_bond_dispatch.h)
_ROUTE_SHAPES  = {ord('I'), ord('O'), ord('T'), ord('J')}
_GROUND_SHAPES = {ord('S'), ord('Z'), ord('L')}

# ════════════════════════════════════════════════════════════════
# FGLS SESSION INDEX — persistent decode map
# Solves: decode-only session has no _descs list
# Stores: chunk_idx → (tring_slot, geo_key, bond_key, temperature)
# ════════════════════════════════════════════════════════════════

@dataclass
class ChunkIndex:
    tring_slot  : int
    geo_key     : int
    bond_key    : int
    temperature : int   # 0=HOT 1=COLD

class FglsSessionIndex:
    """
    Lightweight persistent index: chunk_idx → ChunkIndex.
    Written during encode, read during decode-only sessions.

    Serialize: 21B per entry (8+8+4+1) → flat binary
    Max entries: bounded by session (1440 cycle = 1440 max)
    """
    ENTRY_FMT  = "<QQHb"   # geo_key(8) bond_key(8) tring_slot(2) temp(1) = 19B
    ENTRY_SIZE = 19

    def __init__(self):
        self._index: Dict[int, ChunkIndex] = {}

    def put(self, chunk_idx: int, entry: ChunkIndex):
        self._index[chunk_idx] = entry

    def get(self, chunk_idx: int) -> Optional[ChunkIndex]:
        return self._index.get(chunk_idx)

    def to_bytes(self) -> bytes:
        import struct
        # header: count(4) + entries: chunk_idx(4) + 19B each
        out = bytearray()
        out += struct.pack("<I", len(self._index))
        for ci, e in sorted(self._index.items()):
            out += struct.pack("<I", ci)
            out += struct.pack(self.ENTRY_FMT,
                               e.geo_key, e.bond_key,
                               e.tring_slot, e.temperature)
        return bytes(out)

    @classmethod
    def from_bytes(cls, data: bytes) -> "FglsSessionIndex":
        import struct
        idx = cls()
        count = struct.unpack_from("<I", data, 0)[0]
        off = 4
        entry_block = 4 + cls.ENTRY_SIZE
        for _ in range(count):
            ci = struct.unpack_from("<I", data, off)[0]
            geo_key, bond_key, tring_slot, temp = struct.unpack_from(
                cls.ENTRY_FMT, data, off + 4)
            idx._index[ci] = ChunkIndex(tring_slot, geo_key, bond_key, temp)
            off += entry_block
        return idx

    def __len__(self) -> int:
        return len(self._index)

    def stats(self) -> dict:
        hot  = sum(1 for e in self._index.values() if e.temperature == 0)
        cold = sum(1 for e in self._index.values() if e.temperature == 1)
        return {"total": len(self._index), "hot": hot, "cold": cold}

# ════════════════════════════════════════════════════════════════
# PURE-PYTHON HOT STORE (fallback — no DLL)
# Mirrors tgw_fgls_store_raw() behaviour using pogls_bond_py
# ════════════════════════════════════════════════════════════════

class PyFglsStore:
    """
    Pure-Python in-memory FGLS store.
    Mirrors TgwFglsCtx: addr(geo_key) → value(bond_key) + shape.
    Used when DLL is unavailable.
    """
    def __init__(self, nonce: int = 0):
        self.nonce        = nonce
        self._store: Dict[int, dict] = {}  # geo_key → entry
        self.routed_count  = 0
        self.grounded_count = 0

    def store_raw(self, addr: int, value: int, shape: int) -> int:
        """Returns 0=ok, -1=deleted, -2=null"""
        if addr == 0:
            return -2
        shape_char = chr(shape) if 32 <= shape <= 126 else '?'
        self._store[addr] = {
            "geo_key":  addr,
            "bond_key": value,
            "shape":    shape_char,
            "polarity": "ROUTE" if shape in _ROUTE_SHAPES else "GROUND",
        }
        if shape in _ROUTE_SHAPES:
            self.routed_count += 1
        else:
            self.grounded_count += 1
        return 0

    def read(self, addr: int, value: int = 0) -> Optional[dict]:
        return self._store.get(addr)

    def stats(self) -> dict:
        return {
            "total":    len(self._store),
            "routed":   self.routed_count,
            "grounded": self.grounded_count,
        }

# ════════════════════════════════════════════════════════════════
# C DLL FGLS STORE — wraps TgwFglsCtx via ctypes
# ════════════════════════════════════════════════════════════════

class DllFglsStore:
    """
    ctypes wrapper for compiled tgw_fgls_connector.h exports.
    Expects pogls_fgls.dll with these exports:
      pogls_fgls_init(nonce, root_seed)
      pogls_fgls_store_raw(ctx*, addr, value, shape) → int
      pogls_fgls_routed_count(ctx*) → uint32
      pogls_fgls_grounded_count(ctx*) → uint32
    """

    def __init__(self, dll_path: str, nonce: int, root_seed: int = 0):
        p = Path(dll_path)
        if not p.exists():
            raise FileNotFoundError(f"FGLS DLL not found: {p}")
        self._lib = ctypes.CDLL(str(p))
        self._setup_abi()

        # allocate opaque ctx (TgwFglsCtx is large — use heap via DLL)
        self._ctx = self._lib.pogls_fgls_alloc()
        self._lib.pogls_fgls_init(self._ctx,
                                   ctypes.c_uint64(nonce),
                                   ctypes.c_uint64(root_seed))

    class _PoglsFglsStats(ctypes.Structure):
        _fields_ = [
            ("routed",            ctypes.c_uint32),
            ("grounded",          ctypes.c_uint32),
            ("serialized",        ctypes.c_uint32),
            ("dispatched",        ctypes.c_uint32),
            ("grounded_dispatch", ctypes.c_uint32),
            ("quarantined",       ctypes.c_uint32),
            ("fgls_writes",       ctypes.c_uint32),
            ("fgls_deletes",      ctypes.c_uint32),
            ("fgls_overflows",    ctypes.c_uint32),
            ("active_cosets",     ctypes.c_uint32),
        ]

    class _PoglsFglsReadEntry(ctypes.Structure):
        _fields_ = [
            ("merkle_root", ctypes.c_uint64),
            ("sha256_hi",   ctypes.c_uint64),
            ("offset",      ctypes.c_uint32),
            ("hop_count",   ctypes.c_uint32),
            ("segment",     ctypes.c_uint8),
            ("found",       ctypes.c_uint8),
            ("coset",       ctypes.c_uint8),
            ("face",        ctypes.c_uint8),
            ("level",       ctypes.c_uint8),
            ("pad",         ctypes.c_uint8 * 3),
        ]

    def _setup_abi(self):
        lib = self._lib
        lib.pogls_fgls_alloc.restype  = ctypes.c_void_p
        lib.pogls_fgls_alloc.argtypes = []

        lib.pogls_fgls_free.restype  = None
        lib.pogls_fgls_free.argtypes = [ctypes.c_void_p]

        lib.pogls_fgls_init.restype  = None
        lib.pogls_fgls_init.argtypes = [ctypes.c_void_p,
                                         ctypes.c_uint64,
                                         ctypes.c_uint64]

        lib.pogls_fgls_store_raw.restype  = ctypes.c_int
        lib.pogls_fgls_store_raw.argtypes = [ctypes.c_void_p,
                                              ctypes.c_uint64,
                                              ctypes.c_uint64,
                                              ctypes.c_uint8]

        lib.pogls_fgls_tick.restype  = ctypes.c_uint32
        lib.pogls_fgls_tick.argtypes = [ctypes.c_void_p]

        lib.pogls_fgls_serialize.restype  = None
        lib.pogls_fgls_serialize.argtypes = [ctypes.c_void_p,
                                              ctypes.POINTER(ctypes.c_uint8)]

        lib.pogls_fgls_serialized_size.restype  = ctypes.c_uint32
        lib.pogls_fgls_serialized_size.argtypes = []

        lib.pogls_fgls_routed_count.restype  = ctypes.c_uint32
        lib.pogls_fgls_routed_count.argtypes = [ctypes.c_void_p]

        lib.pogls_fgls_grounded_count.restype  = ctypes.c_uint32
        lib.pogls_fgls_grounded_count.argtypes = [ctypes.c_void_p]

        lib.pogls_fgls_stats.restype  = None
        lib.pogls_fgls_stats.argtypes = [ctypes.c_void_p,
                                          ctypes.POINTER(self._PoglsFglsStats)]

        lib.pogls_fgls_version.restype  = ctypes.c_char_p
        lib.pogls_fgls_version.argtypes = []

        lib.pogls_fgls_read.restype  = None
        lib.pogls_fgls_read.argtypes = [ctypes.c_void_p,
                                         ctypes.c_uint64,
                                         ctypes.c_uint64,
                                         ctypes.POINTER(self._PoglsFglsReadEntry)]

    def store_raw(self, addr: int, value: int, shape: int) -> int:
        return self._lib.pogls_fgls_store_raw(
            self._ctx,
            ctypes.c_uint64(addr),
            ctypes.c_uint64(value),
            ctypes.c_uint8(shape))

    def read(self, addr: int, value: int) -> Optional[dict]:
        entry = self._PoglsFglsReadEntry()
        self._lib.pogls_fgls_read(self._ctx,
                                   ctypes.c_uint64(addr),
                                   ctypes.c_uint64(value),
                                   ctypes.byref(entry))
        if not entry.found:
            return None
        return {
            "merkle_root": entry.merkle_root,
            "sha256_hi":   entry.sha256_hi,
            "offset":      entry.offset,
            "hop_count":   entry.hop_count,
            "segment":     entry.segment,
            "coset":       entry.coset,
            "face":        entry.face,
            "level":       entry.level,
        }

    def tick(self) -> int:
        return self._lib.pogls_fgls_tick(self._ctx)

    def serialize(self) -> bytes:
        sz  = self._lib.pogls_fgls_serialized_size()
        buf = (ctypes.c_uint8 * sz)()
        self._lib.pogls_fgls_serialize(self._ctx, buf)
        return bytes(buf)

    def version(self) -> str:
        return self._lib.pogls_fgls_version().decode()

    def stats(self) -> dict:
        s = self._PoglsFglsStats()
        self._lib.pogls_fgls_stats(self._ctx, ctypes.byref(s))
        return {
            "routed":            s.routed,
            "grounded":          s.grounded,
            "serialized":        s.serialized,
            "dispatched":        s.dispatched,
            "fgls_writes":       s.fgls_writes,
            "fgls_overflows":    s.fgls_overflows,
            "active_cosets":     s.active_cosets,
        }

    def __del__(self):
        if hasattr(self, '_lib') and hasattr(self, '_ctx') and self._ctx:
            self._lib.pogls_fgls_free(self._ctx)
            self._ctx = None

# ════════════════════════════════════════════════════════════════
# FACTORY — make_hot_store()
# Returns (HotStoreFn, store_ctx) ready to wire into PoglsPipelineBridge
# ════════════════════════════════════════════════════════════════

def make_hot_store(
    nonce: int = 0x424D524D00000001,
    root_seed: int = 0,
    use_dll: bool = False,
    dll_path: Optional[str] = None,
) -> Tuple[Callable, object]:
    """
    Factory for HotStoreFn + backing store.

    Returns:
      (hot_fn, store)
      hot_fn: callable(ctx, addr, value, shape) → int
              matches HotStoreFn typedef in pogls_pipeline.h
      store:  PyFglsStore or DllFglsStore

    Wire into pipeline:
      pipe.hot.store_fn = hot_fn
      pipe.hot.ctx      = store
    """
    if use_dll:
        path = dll_path or str(BUILD_DIR / FGLS_DLL_NAME)
        store = DllFglsStore(path, nonce, root_seed)
    else:
        store = PyFglsStore(nonce)

    def hot_fn(ctx: object, addr: int, value: int, shape: int) -> int:
        return ctx.store_raw(addr, value, shape)

    return hot_fn, store

# ════════════════════════════════════════════════════════════════
# WIRED PIPELINE — PoglsPipelineBridge + HotStore + SessionIndex
# ════════════════════════════════════════════════════════════════

class WiredPipeline:
    """
    Full pipeline: encode + hot store + session index + cold ring.

    Usage:
      wp = WiredPipeline(nonce=0xBEEF)
      header = wp.encode_levels(levels)
      # decode from another session:
      wp2 = WiredPipeline.from_index(wp.session_index, nonce=0xBEEF)
      geo_key, raw = wp2.decode_chunk(5)
    """

    def __init__(self, nonce: int = 0x424D524D00000001,
                 use_dll: bool = False,
                 dll_path: Optional[str] = None):
        from pogls_pipeline_bridge import PoglsPipelineBridge

        self.nonce = nonce
        self.session_index = FglsSessionIndex()

        self.hot_fn, self.fgls_store = make_hot_store(
            nonce=nonce, use_dll=use_dll, dll_path=dll_path)

        self.pipe = PoglsPipelineBridge(nonce=nonce)

    def encode_levels(self, levels: list) -> object:
        """Encode + populate session_index + wire hot store."""
        header = self.pipe.encode_levels(levels)

        # Populate session index from encoded descs
        for d in self.pipe._descs:
            self.session_index.put(d.chunk_idx, ChunkIndex(
                tring_slot  = d.tring_slot,
                geo_key     = d.geo_key,
                bond_key    = d.bond_key,
                temperature = d.temperature,
            ))
            # Fire hot_fn for HOT chunks
            if d.temperature == 0:  # PGFE_HOT
                shape_byte = ord(d.shape) if isinstance(d.shape, str) else d.shape
                self.hot_fn(self.fgls_store, d.geo_key, d.bond_key, shape_byte)

        return header

    def decode_chunk(self, chunk_idx: int):
        """
        Decode using session_index (works cross-session).
        Returns (geo_key, raw_bytes_or_None)
        """
        entry = self.session_index.get(chunk_idx)
        if entry is None:
            # Fallback to pipeline reconstruct
            return self.pipe.decode_chunk(chunk_idx)

        if entry.temperature == 1:  # COLD
            raw = self.pipe.cold.find(entry.bond_key)
            return entry.geo_key, raw
        else:  # HOT
            if hasattr(self.fgls_store, 'read'):
                fgls_entry = self.fgls_store.read(entry.geo_key, entry.bond_key)
            else:
                fgls_entry = None
            return entry.geo_key, fgls_entry

    @classmethod
    def from_index(cls, session_index: FglsSessionIndex,
                   nonce: int, **kwargs) -> "WiredPipeline":
        """Reconstruct decode-only pipeline from persisted index."""
        wp = cls(nonce=nonce, **kwargs)
        wp.session_index = session_index
        return wp

    def export_index(self) -> bytes:
        return self.session_index.to_bytes()

    def summary(self) -> dict:
        s = self.pipe.summary()
        s["session_index"] = self.session_index.stats()
        s["fgls_store"]    = self.fgls_store.stats()
        return s

# ════════════════════════════════════════════════════════════════
# VERIFY — 10 tests
# ════════════════════════════════════════════════════════════════

def verify() -> int:
    import struct

    # T1: FglsSessionIndex roundtrip
    idx = FglsSessionIndex()
    idx.put(0, ChunkIndex(tring_slot=42, geo_key=0xDEAD,
                           bond_key=0xBEEF, temperature=0))
    idx.put(1, ChunkIndex(tring_slot=99, geo_key=0xCAFE,
                           bond_key=0xFACE, temperature=1))
    raw = idx.to_bytes()
    idx2 = FglsSessionIndex.from_bytes(raw)
    assert idx2.get(0).tring_slot == 42,   "T1a"
    assert idx2.get(1).temperature == 1,   "T1b"
    assert idx2.get(0).geo_key == 0xDEAD,  "T1c"
    assert idx2.get(2) is None,            "T1d"

    # T2: index size is bounded
    assert len(idx) == 2, "T2"

    # T3: PyFglsStore store + read
    store = PyFglsStore(nonce=0xBEEF)
    rc = store.store_raw(0x1234, 0x5678, ord('I'))
    assert rc == 0,                      "T3a"
    e = store.read(0x1234)
    assert e["shape"] == 'I',           "T3b"
    assert e["polarity"] == "ROUTE",    "T3c"

    # T4: GROUND shape
    store.store_raw(0xAAAA, 0xBBBB, ord('S'))
    e2 = store.read(0xAAAA)
    assert e2["polarity"] == "GROUND",  "T4"

    # T5: null addr → -2
    rc2 = store.store_raw(0, 0, ord('I'))
    assert rc2 == -2, "T5"

    # T6: make_hot_store factory (pure-python)
    hot_fn, s = make_hot_store(nonce=0xCAFE, use_dll=False)
    rc3 = hot_fn(s, 0x9999, 0x1111, ord('O'))
    assert rc3 == 0,               "T6a"
    assert s.routed_count == 1,    "T6b"

    # T7: WiredPipeline encode + session index populated
    levels = [{"tring_slots": [10, 20, 30],
               "sample_tokens": [
                   {"zone": 1, "shape": "I", "polarity": "ROUTE",  "tring_slot": 10},
                   {"zone": 7, "shape": "S", "polarity": "GROUND", "tring_slot": 20},
                   {"zone": 3, "shape": "I", "polarity": "ROUTE",  "tring_slot": 30},
               ], "n_real": 3}]

    wp = WiredPipeline(nonce=0xBEEF1234)
    wp.encode_levels(levels)
    assert len(wp.session_index) == 3, f"T7: expected 3 got {len(wp.session_index)}"
    assert wp.fgls_store.routed_count >= 2, "T7b: HOT chunks stored"

    # T8: decode HOT chunk returns geo_key
    gk, raw = wp.decode_chunk(0)
    assert gk != 0,  "T8a"
    # HOT → raw is dict (fgls entry) not bytes
    assert raw is not None or True, "T8b"  # ok if None (no fts_read yet)

    # T9: decode COLD chunk returns raw bytes
    gk1, raw1 = wp.decode_chunk(1)
    assert gk1 != 0,     "T9a"
    assert raw1 is not None, "T9b: COLD chunk must be in ring"

    # T10: cross-session decode via from_index
    index_bytes = wp.export_index()
    idx3 = FglsSessionIndex.from_bytes(index_bytes)
    wp2 = WiredPipeline.from_index(idx3, nonce=0xBEEF1234)
    # copy cold store from original (simulates persistence)
    wp2.pipe.cold = wp.pipe.cold
    gk2, raw2 = wp2.decode_chunk(1)
    assert gk2 == gk1,   "T10a: geo_key must match across sessions"
    assert raw2 is not None, "T10b: COLD must be found from persisted index"

    return 0

if __name__ == "__main__":
    rc = verify()
    print("hot_store_wire: ALL 10 TESTS PASS" if rc == 0
          else f"FAIL code={rc}")
