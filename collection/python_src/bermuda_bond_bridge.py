"""
bermuda_bond_bridge.py — Unified ctypes bridge for Bermuda + Bond C DLLs
========================================================================
Zero-copy access to:
  - bermuda_export.h: gear snap, Hilbert, traverse, zone, route_batch
  - pogls_bond_export.h: fibo_addr, make_piece, bond_verify,
    make_slot, plug_connect

Usage:
  from bermuda_bond_bridge import BermudaBondBridge
  bb = BermudaBondBridge()
  bb.bermuda_init()
  gear = bb.snap_gear(64)
  piece = bb.make_piece(seed=0x424D0001, axis=1)
  entries = bb.route_batch([0,1,2], gear=2, mode=0)
  bond = bb.bond_verify(piece, piece2)
"""

import ctypes, os
from pathlib import Path
from typing import List, Optional, Tuple

COLLECTION = Path(__file__).resolve().parent.parent
BUILD_DIR  = COLLECTION / "build"

# ═══════════════════════════════════════════════════════════════
# BOND STRUCTS (mirrors pogls_bond.h packed layout)
# ═══════════════════════════════════════════════════════════════

class PoglsPiece(ctypes.Structure):
    """25B packed — mirrors C __attribute__((packed))"""
    _pack_ = 1
    _fields_ = [
        ("geo_key", ctypes.c_uint64),   # 0-7
        ("shape",   ctypes.c_uint8),     # 8
        ("bond_L",  ctypes.c_uint64),    # 9-16
        ("bond_R",  ctypes.c_uint64),    # 17-24
    ]  # total 25

    def __repr__(self):
        return (f"PoglsPiece(geo=0x{self.geo_key:016x} "
                f"shape={chr(self.shape)} "
                f"L=0x{self.bond_L:016x} R=0x{self.bond_R:016x})")


class PoglsPlug(ctypes.Structure):
    """12B natural alignment (C does NOT pack this struct)"""
    _fields_ = [
        ("target_id", ctypes.c_uint32),  # 0-3
        ("face",      ctypes.c_uint8),   # 4
        ("ttl",       ctypes.c_uint16),  # 6-7 (pad at 5)
        ("active",    ctypes.c_uint8),   # 8
        # pad bytes 9-11 → total 12
    ]


class PoglsSlot(ctypes.Structure):
    """88B — mirrors C natural layout (PoglsPiece is packed, slot has padding)"""
    _pack_ = 1
    _fields_ = [
        ("piece",     PoglsPiece),       # 0-24
        ("_pad1",     ctypes.c_uint8 * 3),  # 25-27 (align plugs to 4)
        ("plugs",     PoglsPlug * 4),    # 28-75 (4×12)
        ("agent_id",  ctypes.c_uint32),  # 76-79
        ("token_cap", ctypes.c_uint32),  # 80-83
        ("rerouted",  ctypes.c_uint8),   # 84
        ("_pad2",     ctypes.c_uint8 * 3),  # 85-87 (pad to 88)
    ]  # total 88

    def __repr__(self):
        active = [(p.target_id, p.face, p.active) for p in self.plugs if p.active]
        return (f"PoglsSlot(agent={self.agent_id} "
                f"shape={chr(self.piece.shape)} "
                f"rerouted={self.rerouted} "
                f"plugs={active})")


# ═══════════════════════════════════════════════════════════════
# BERMUDA ROUTE ENTRY (mirrors bermuda_export.h)
# ═══════════════════════════════════════════════════════════════

class BermudaRouteEntry(ctypes.Structure):
    _fields_ = [
        ("idx_in",     ctypes.c_uint16),
        ("idx_out",    ctypes.c_uint16),
        ("zone",       ctypes.c_uint8),
        ("pole",       ctypes.c_uint8),
        ("shape",      ctypes.c_uint8),
        ("polarity",   ctypes.c_uint8),
        ("tring_slot", ctypes.c_uint16),
    ]

    def __repr__(self):
        return (f"BermudaRoute(idx_in={self.idx_in} idx_out={self.idx_out} "
                f"zone={self.zone} "
                f"shape={chr(self.shape)} "
                f"polarity={'ROUTE' if self.polarity==0 else 'GROUND'} "
                f"tring={self.tring_slot})")


# ═══════════════════════════════════════════════════════════════
# UNIFIED BRIDGE
# ═══════════════════════════════════════════════════════════════

class BermudaBondBridge:
    """Unified bridge: bermuda geometry ops + bond layer, both in C."""

    def __init__(self, bermuda_dll: str = None, bond_dll: str = None):
        self._load_bermuda(bermuda_dll or str(BUILD_DIR / "pogls_bermuda.dll"))
        self._load_bond(bond_dll or str(BUILD_DIR / "pogls_bond.dll"))

    # ── DLL loaders ───────────────────────────────────────────

    def _load_bermuda(self, path: str):
        p = Path(path)
        if not p.exists():
            raise FileNotFoundError(f"bermuda DLL not found: {p}")
        self._b = ctypes.CDLL(str(p))
        lib = self._b

        lib.bermuda_init_dll.restype = None
        lib.bermuda_init_dll.argtypes = []

        lib.bermuda_snap_gear_dll.restype = ctypes.c_uint8
        lib.bermuda_snap_gear_dll.argtypes = [ctypes.c_uint16]

        lib.bermuda_hilbert_encode_dll.restype = ctypes.c_uint16
        lib.bermuda_hilbert_encode_dll.argtypes = [ctypes.c_uint16, ctypes.c_uint8]

        lib.bermuda_hilbert_decode_dll.restype = ctypes.c_uint16
        lib.bermuda_hilbert_decode_dll.argtypes = [ctypes.c_uint16, ctypes.c_uint8]

        lib.bermuda_traverse_dll.restype = ctypes.c_uint16
        lib.bermuda_traverse_dll.argtypes = [ctypes.c_uint16, ctypes.c_uint8, ctypes.c_uint8]

        lib.bermuda_zone_dll.restype = ctypes.c_uint8
        lib.bermuda_zone_dll.argtypes = [ctypes.c_uint16, ctypes.c_uint8]

        lib.bermuda_route_batch.restype = None
        lib.bermuda_route_batch.argtypes = [
            ctypes.POINTER(ctypes.c_uint16),
            ctypes.c_uint8,
            ctypes.c_uint8,
            ctypes.POINTER(BermudaRouteEntry),
            ctypes.c_uint32,
        ]

    def _load_bond(self, path: str):
        p = Path(path)
        if not p.exists():
            raise FileNotFoundError(f"bond DLL not found: {p}")
        self._bond = ctypes.CDLL(str(p))
        lib = self._bond

        # set_nonce
        lib.poglsex_set_nonce.argtypes = [ctypes.c_uint64]
        lib.poglsex_set_nonce.restype = None
        lib.poglsex_get_nonce.restype = ctypes.c_uint64

        # piece factory
        lib.poglsex_make_piece.argtypes = [
            ctypes.c_uint64, ctypes.c_uint8,
            ctypes.POINTER(PoglsPiece),
        ]
        lib.poglsex_make_piece.restype = None

        # bond ops
        lib.poglsex_bond_key.argtypes = [ctypes.POINTER(PoglsPiece)]
        lib.poglsex_bond_key.restype = ctypes.c_uint64

        lib.poglsex_bond_verify.argtypes = [
            ctypes.POINTER(PoglsPiece), ctypes.POINTER(PoglsPiece),
            ctypes.POINTER(ctypes.c_uint64),
        ]
        lib.poglsex_bond_verify.restype = ctypes.c_uint8

        # slot ops
        lib.poglsex_make_slot.argtypes = [
            ctypes.c_uint64, ctypes.c_uint8,
            ctypes.c_uint32, ctypes.c_uint32,
            ctypes.POINTER(PoglsSlot),
        ]
        lib.poglsex_make_slot.restype = None

        lib.poglsex_plug_connect.argtypes = [
            ctypes.POINTER(PoglsSlot), ctypes.c_uint8,
            ctypes.POINTER(PoglsSlot), ctypes.c_uint8,
            ctypes.c_uint16,
        ]
        lib.poglsex_plug_connect.restype = None

        # fibo_addr
        lib.poglsex_fibo_addr.argtypes = [ctypes.c_uint64]
        lib.poglsex_fibo_addr.restype = ctypes.c_uint64

        # version
        lib.poglsex_version.restype = ctypes.c_char_p

    # ── Bermuda ops ───────────────────────────────────────────

    def bermuda_init(self):
        self._b.bermuda_init_dll()

    def snap_gear(self, n_tokens: int) -> int:
        return self._b.bermuda_snap_gear_dll(ctypes.c_uint16(n_tokens))

    def hilbert_encode(self, pos: int, gear: int) -> int:
        return self._b.bermuda_hilbert_encode_dll(
            ctypes.c_uint16(pos), ctypes.c_uint8(gear))

    def hilbert_decode(self, idx: int, gear: int) -> int:
        return self._b.bermuda_hilbert_decode_dll(
            ctypes.c_uint16(idx), ctypes.c_uint8(gear))

    def traverse(self, idx: int, gear: int, mode: int) -> int:
        return self._b.bermuda_traverse_dll(
            ctypes.c_uint16(idx), ctypes.c_uint8(gear), ctypes.c_uint8(mode))

    def zone(self, idx: int, gear: int) -> int:
        return self._b.bermuda_zone_dll(
            ctypes.c_uint16(idx), ctypes.c_uint8(gear))

    def route_batch(self, idxs: List[int], gear: int, mode: int
                    ) -> List[BermudaRouteEntry]:
        n = len(idxs)
        arr = (ctypes.c_uint16 * n)(*idxs)
        out = (BermudaRouteEntry * n)()
        self._b.bermuda_route_batch(
            arr, ctypes.c_uint8(gear), ctypes.c_uint8(mode),
            out, ctypes.c_uint32(n))
        return list(out)

    # ── Bond ops ──────────────────────────────────────────────

    def set_nonce(self, nonce: int):
        self._bond.poglsex_set_nonce(ctypes.c_uint64(nonce))

    def fibo_addr(self, seed: int) -> int:
        return self._bond.poglsex_fibo_addr(ctypes.c_uint64(seed))

    def make_piece(self, seed: int, axis: int) -> PoglsPiece:
        p = PoglsPiece()
        self._bond.poglsex_make_piece(
            ctypes.c_uint64(seed), ctypes.c_uint8(axis),
            ctypes.byref(p))
        return p

    def bond_key(self, piece: PoglsPiece) -> int:
        return self._bond.poglsex_bond_key(ctypes.byref(piece))

    def bond_verify(self, a: PoglsPiece, b: PoglsPiece) -> dict:
        bk = ctypes.c_uint64(0)
        valid = self._bond.poglsex_bond_verify(
            ctypes.byref(a), ctypes.byref(b), ctypes.byref(bk))
        return {"bond_key": bk.value, "valid": bool(valid)}

    def make_slot(self, seed: int, axis: int,
                  agent_id: int = 0, token_cap: int = 300) -> PoglsSlot:
        s = PoglsSlot()
        self._bond.poglsex_make_slot(
            ctypes.c_uint64(seed), ctypes.c_uint8(axis),
            ctypes.c_uint32(agent_id), ctypes.c_uint32(token_cap),
            ctypes.byref(s))
        return s

    def plug_connect(self, a: PoglsSlot, face_a: int,
                     b: PoglsSlot, face_b: int, ttl: int = 64):
        self._bond.poglsex_plug_connect(
            ctypes.byref(a), ctypes.c_uint8(face_a),
            ctypes.byref(b), ctypes.c_uint8(face_b),
            ctypes.c_uint16(ttl))

    def version(self) -> str:
        return self._bond.poglsex_version().decode()


# ═══════════════════════════════════════════════════════════════
# TEST
# ═══════════════════════════════════════════════════════════════

def test():
    print("=" * 56)
    print("BermudaBondBridge: Unified C bridge test")
    print("=" * 56)

    bb = BermudaBondBridge()
    bb.bermuda_init()
    print(f"\n  Bond version: {bb.version()}")

    # ── Bermuda ops ───────────────────────────────────────────
    print("\n[1] Bermuda geometry ops")
    g = bb.snap_gear(100)
    h = bb.hilbert_encode(42, 2)
    z = bb.zone(42, 2)
    t = bb.traverse(42, 2, 0)
    print(f"  snap_gear(100)={g}  hilbert(42,g2)={h}  "
          f"zone(42,g2)={z}  traverse(42,g2,0)={t}")

    entries = bb.route_batch([0, 1, 2, 3], gear=2, mode=0)
    for e in entries:
        print(f"  {e}")

    # ── Bond ops ──────────────────────────────────────────────
    print("\n[2] Bond piece ops")
    p1 = bb.make_piece(0x424D0001, 1)
    p2 = bb.make_piece(0x424D0001, 1)  # same seed
    p3 = bb.make_piece(0x424D0002, 3)  # different seed
    print(f"  p1: {p1}")
    print(f"  p2: {p2}")
    print(f"  p3: {p3}")

    bk1 = bb.bond_key(p1)
    bk2 = bb.bond_key(p2)
    bk3 = bb.bond_key(p3)
    print(f"  bond_key p1=0x{bk1:016x} p2=0x{bk2:016x} "
          f"match={bk1==bk2}")
    print(f"  bond_key p3=0x{bk3:016x} != p1={bk1!=bk3}")

    bond_same = bb.bond_verify(p1, p2)
    bond_diff = bb.bond_verify(p1, p3)
    print(f"  same-origin bond: {'BOND' if bond_same['valid'] else 'no-bond'}")
    print(f"  diff-origin bond: {'BOND' if bond_diff['valid'] else 'no-bond'}")

    # ── Slot ops ──────────────────────────────────────────────
    print("\n[3] Slot + plug chain")
    s1 = bb.make_slot(0x424D0001, 1, agent_id=0)
    s2 = bb.make_slot(0x424D0002, 3, agent_id=1)
    bb.plug_connect(s1, 2, s2, 3, 64)  # E→W
    print(f"  s1: agent={s1.agent_id} shape={chr(s1.piece.shape)} "
          f"plugs={[(p.target_id, p.face, p.active) for p in s1.plugs]}")
    print(f"  s2: agent={s2.agent_id} shape={chr(s2.piece.shape)} "
          f"plugs={[(p.target_id, p.face, p.active) for p in s2.plugs]}")

    # ── CROSS self-inverse ────────────────────────────────────
    print("\n[4] CROSS self-inverse (C ABI)")
    ok = all(bb.traverse(bb.traverse(i, 2, 2), 2, 2) == i for i in range(200))
    print(f"  gear 2: 200/200 {'✓' if ok else '✗'}")

    print(f"\n  All tests complete ✓")


if __name__ == "__main__":
    test()
