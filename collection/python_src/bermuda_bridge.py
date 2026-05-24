"""
bermuda_bridge.py — ctypes wrapper for pogls_bermuda.dll
========================================================
Zero-copy C bridge for bermuda ops (gear_snap, Hilbert, traverse, zone).

Usage:
  from bermuda_bridge import BermudaBridge
  br = BermudaBridge()
  br.init()
  gear = br.snap_gear(64)          # → 1
  h    = br.hilbert_encode(42, 2)  # → (42*37) % 1024
  z    = br.zone(42, 2)            # → zone 0-11
  idx2 = br.traverse(42, 2, 0)     # ORBITAL: → 43

  # Batch route
  entries = br.route_batch([0,1,2,3], gear=2, mode=0)
  for e in entries:
      print(e.shape, e.polarity, e.tring_slot)
"""

import ctypes, os
from pathlib import Path
from typing import List, Optional

COLLECTION = Path(__file__).resolve().parent.parent
BUILD_DIR  = COLLECTION / "build"

# ── Route entry struct (mirrors BermudaRouteEntry in bermuda_export.h) ──
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
                f"zone={self.zone} pole={self.pole} "
                f"shape={chr(self.shape)} polarity={'ROUTE' if self.polarity==0 else 'GROUND'} "
                f"tring={self.tring_slot})")


class BermudaBridge:
    """ctypes wrapper for pogls_bermuda.dll (bermuda_export.h)"""

    def __init__(self, dll_path: Optional[str] = None):
        if dll_path:
            path = Path(dll_path)
        else:
            path = BUILD_DIR / "pogls_bermuda.dll"

        if not path.exists():
            raise FileNotFoundError(
                f"pogls_bermuda.dll not found at {path}\n"
                f"Compile: gcc -O2 -shared -DBERMUDA_EXPORT_DLL "
                f"-o {path} bermuda_export.c"
            )

        self._lib = ctypes.CDLL(str(path))
        self._setup_prototypes()

    def _setup_prototypes(self):
        lib = self._lib

        # init
        lib.bermuda_init_dll.restype = None
        lib.bermuda_init_dll.argtypes = []

        # snap_gear
        lib.bermuda_snap_gear_dll.restype = ctypes.c_uint8
        lib.bermuda_snap_gear_dll.argtypes = [ctypes.c_uint16]

        # hilbert_encode
        lib.bermuda_hilbert_encode_dll.restype = ctypes.c_uint16
        lib.bermuda_hilbert_encode_dll.argtypes = [ctypes.c_uint16, ctypes.c_uint8]

        # hilbert_decode
        lib.bermuda_hilbert_decode_dll.restype = ctypes.c_uint16
        lib.bermuda_hilbert_decode_dll.argtypes = [ctypes.c_uint16, ctypes.c_uint8]

        # traverse
        lib.bermuda_traverse_dll.restype = ctypes.c_uint16
        lib.bermuda_traverse_dll.argtypes = [ctypes.c_uint16, ctypes.c_uint8, ctypes.c_uint8]

        # zone
        lib.bermuda_zone_dll.restype = ctypes.c_uint8
        lib.bermuda_zone_dll.argtypes = [ctypes.c_uint16, ctypes.c_uint8]

        # route_batch
        lib.bermuda_route_batch.restype = None
        lib.bermuda_route_batch.argtypes = [
            ctypes.POINTER(ctypes.c_uint16),  # idxs
            ctypes.c_uint8,                    # gear
            ctypes.c_uint8,                    # mode
            ctypes.POINTER(BermudaRouteEntry), # out
            ctypes.c_uint32,                   # n
        ]

    def init(self):
        """Initialize C context (precompute gear tables)."""
        self._lib.bermuda_init_dll()

    def snap_gear(self, n_tokens: int) -> int:
        """Snap token count to smallest fitting gear (1-4)."""
        return self._lib.bermuda_snap_gear_dll(ctypes.c_uint16(n_tokens))

    def hilbert_encode(self, position: int, gear: int) -> int:
        """Position → Hilbert index: (pos * 37) % slots"""
        return self._lib.bermuda_hilbert_encode_dll(
            ctypes.c_uint16(position), ctypes.c_uint8(gear))

    def hilbert_decode(self, index: int, gear: int) -> int:
        """Hilbert index → position: (idx * INV37) % slots"""
        return self._lib.bermuda_hilbert_decode_dll(
            ctypes.c_uint16(index), ctypes.c_uint8(gear))

    def traverse(self, idx: int, gear: int, mode: int) -> int:
        """Apply traverse mode (0=ORBITAL 1=CHIRAL 2=CROSS 3=HUB)."""
        return self._lib.bermuda_traverse_dll(
            ctypes.c_uint16(idx), ctypes.c_uint8(gear), ctypes.c_uint8(mode))

    def zone(self, idx: int, gear: int) -> int:
        """Get geometry zone (0-11) from codebook index."""
        return self._lib.bermuda_zone_dll(
            ctypes.c_uint16(idx), ctypes.c_uint8(gear))

    def route_batch(self, idxs: List[int], gear: int, mode: int
                    ) -> List[BermudaRouteEntry]:
        """Batch route: list of indices → list of route entries."""
        n = len(idxs)
        idxs_arr = (ctypes.c_uint16 * n)(*idxs)
        out_arr  = (BermudaRouteEntry * n)()

        self._lib.bermuda_route_batch(
            idxs_arr, ctypes.c_uint8(gear), ctypes.c_uint8(mode),
            out_arr, ctypes.c_uint32(n))

        return list(out_arr)

    def verify_bijection(self, gear: int) -> bool:
        """Verify Hilbert bijection: encode(decode(x)) == x for all slots."""
        slots = {1: 512, 2: 1024, 3: 2048, 4: 4096}[gear]
        for pos in range(slots):
            h = self.hilbert_encode(pos, gear)
            back = self.hilbert_decode(h, gear)
            if back != pos:
                return False
        return True


# ── Test ─────────────────────────────────────────────────────
def test():
    print("=" * 56)
    print("BermudaBridge: C ctypes test")
    print("=" * 56)

    br = BermudaBridge()
    br.init()

    # ── 1. snap_gear ────────────────────────────────────────
    print("\n[1] snap_gear")
    for n in [50, 500, 1000, 3000]:
        g = br.snap_gear(n)
        print(f"  N={n} → gear {g}")

    # ── 2. Hilbert bijection ────────────────────────────────
    print("\n[2] Hilbert bijection")
    for gear in [1, 2, 3, 4]:
        ok = br.verify_bijection(gear)
        print(f"  gear {gear}: {'✓' if ok else '✗'}")

    # ── 3. Traverse modes ───────────────────────────────────
    print("\n[3] Traverse modes (gear=2, idx=100)")
    for mode, name in enumerate(["ORBITAL", "CHIRAL", "CROSS", "HUB"]):
        idx_out = br.traverse(100, 2, mode)
        z = br.zone(100, 2)
        print(f"  {name:7s}: {100} → {idx_out:4d}  zone={z}")

    # ── 4. Batch route ──────────────────────────────────────
    print("\n[4] Batch route (gear=2, mode=0, 8 tokens)")
    entries = br.route_batch(list(range(50, 58)), gear=2, mode=0)
    for e in entries:
        print(f"  {e}")

    # ── 5. Cross-mode consistency ───────────────────────────
    print("\n[5] CROSS self-inverse")
    n_ok = 0
    for idx in range(200):
        idx1 = br.traverse(idx, 2, 2)
        idx2 = br.traverse(idx1, 2, 2)
        if idx2 == idx:
            n_ok += 1
    print(f"  {n_ok}/200 ✓ ({100*n_ok/200:.0f}%)")

    # ── 6. C vs Python consistency ──────────────────────────
    print("\n[6] C vs Python consistency")
    import sys
    sys.path.insert(0, str(COLLECTION))
    from bermuda_reshape_v3 import hilbert_encode as py_encode, \
        hilbert_decode as py_decode, GEAR_TABLE, snap_gear as py_snap

    for gear in [1, 2]:
        for pos in range(100):
            c_h = br.hilbert_encode(pos, gear)
            py_h = py_encode(torch.tensor([pos]), gear).item()
            if c_h != py_h:
                print(f"  ✗ gear={gear} pos={pos}: C={c_h} Py={py_h}")
                break
        else:
            print(f"  Hilbert encode gear {gear}: C == Python ✓")

    print(f"\n  All tests complete ✓")


if __name__ == "__main__":
    import torch
    test()
