"""
pogls_bond_py.py — Python mirror of pogls_bond.h (C → Python, 1:1)
══════════════════════════════════════════════════════════════════════
Intrinsic Bond Layer for FoldGate TetrisRouter.

All functions return identical results to C counterpart.
No external deps — pure Python.

Usage:
    from pogls_bond_py import (
        fibo_addr, make_piece, bond_key, bond_verify,
        plug_connect, piece_to_dict
    )
"""

from __future__ import annotations
import struct

# ── CONSTANTS (mirrors pogls_bond.h) ──────────────────────────────────────

GEO_MAGIC   = 0x00120090024005A0   # 18/144/576/1440 packed
FNV_PRIME   = 0x00000100000001B3
FNV_OFFSET  = 0xCBF29CE484222325
BOND_SALT_L = 0xAAAAAAAAAAAAAAAA
BOND_SALT_R = 0x5555555555555555

# Fibonacci table — first 16 terms
FIBO = (1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987)

# Axis → Shape mapping (fold_axis 1–7 → I O T S Z L J)
AXIS_SHAPE  = ['\0', 'I', 'O', 'T', 'S', 'Z', 'L', 'J']

# Ω fallback shapes
OMEGA_COMPRESS   = 'I'
OMEGA_RETRY      = 'O'
OMEGA_QUARANTINE = 'L'
OMEGA_WAIT       = 'T'

# Fault → Ω shape table
FAULT_SHAPE = {
    0: 'I',   # OK
    1: 'I',   # OVERFLOW   → OMEGA_COMPRESS
    2: 'L',   # FAULT      → OMEGA_QUARANTINE
    3: 'T',   # UPSTREAM   → OMEGA_WAIT
    4: 'O',   # RETRY      → OMEGA_RETRY
}

# ══════════════════════════════════════════════════════════════════════════
# CORE: fibo_addr (identical to C implementation)
# ══════════════════════════════════════════════════════════════════════════

def _rotr(x: int, n: int) -> int:
    """64-bit rotate right"""
    return ((x >> n) | (x << (64 - n))) & 0xFFFFFFFFFFFFFFFF

def _rotl(x: int, n: int) -> int:
    """64-bit rotate left"""
    return ((x << n) | (x >> (64 - n))) & 0xFFFFFFFFFFFFFFFF

def fibo_addr(seed: int) -> int:
    """
    Three-pass mixing (matches C implementation exactly):
      pass 1: FNV-64 avalanche (seed bytes)
      pass 2: Fibonacci lane XOR (16 terms)
      pass 3: geo_magic fold + rotate
    """
    # pass 1 — FNV-64
    h = FNV_OFFSET
    b = struct.pack("<Q", seed & 0xFFFFFFFFFFFFFFFF)
    for byte in b:
        h ^= byte
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF

    # pass 2 — Fibonacci lane XOR
    for i in range(16):
        h ^= (FIBO[i] * (seed >> (i & 7))) & 0xFFFFFFFFFFFFFFFF
        h = _rotl(h, 13)

    # pass 3 — geo_magic fold
    h ^= GEO_MAGIC
    h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    h ^= h >> 33

    return h & 0xFFFFFFFFFFFFFFFF


# ══════════════════════════════════════════════════════════════════════════
# PIECE — 25B routing unit (mirrors PoglsPiece struct)
# ══════════════════════════════════════════════════════════════════════════

def make_piece(origin_seed: int, fold_axis: int) -> dict:
    """
    Create a piece dict (mirrors pogls_make_piece in C).

    geo_key  = fibo_addr(origin_seed)
    shape    = AXIS_SHAPE[fold_axis]  (1→I, 2→O, 3→T, 4→S, 5→Z, 6→L, 7→J)
    bond_L   = fibo_addr(geo_key ^ BOND_SALT_L)
    bond_R   = fibo_addr(geo_key ^ BOND_SALT_R)
    """
    geo_key = fibo_addr(origin_seed)
    shape   = AXIS_SHAPE[fold_axis] if 1 <= fold_axis <= 7 else 'I'
    bond_L  = fibo_addr(geo_key ^ BOND_SALT_L)
    bond_R  = fibo_addr(geo_key ^ BOND_SALT_R)
    return {
        "geo_key": geo_key,
        "shape": shape,
        "bond_L": bond_L,
        "bond_R": bond_R,
    }


def bond_key(piece: dict) -> int:
    """Intrinsic bond = bond_L XOR bond_R"""
    return piece["bond_L"] ^ piece["bond_R"]


def bond_verify(piece_a: dict, piece_b: dict) -> dict:
    """
    Two pieces are intrinsically bonded if:
      fibo_addr(bond_key_A ^ bond_key_B) has top 16 bits == 0x9009

    Returns {"bond_key": int, "valid": bool}
    """
    ka = bond_key(piece_a)
    kb = bond_key(piece_b)
    combined = fibo_addr(ka ^ kb)
    valid = ((combined >> 48) & 0xFFFF) == 0x9009
    return {"bond_key": ka ^ kb, "valid": valid}


# ══════════════════════════════════════════════════════════════════════════
# SESSION SEED from wallet topology_fp (mirrors pogls_seed_from_fp)
# ══════════════════════════════════════════════════════════════════════════

def seed_from_fp(topology_fp: str) -> int:
    """
    wallet topology_fp (hex string 16 chars) → uint64 origin_seed

    Same FNV-64 loop as C:
      seed = FNV_OFFSET
      for each char: seed ^= char; seed *= FNV_PRIME
      return fibo_addr(seed)
    """
    seed = FNV_OFFSET
    for i, ch in enumerate(topology_fp[:16]):
        seed ^= ord(ch)
        seed = (seed * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return fibo_addr(seed)


# ══════════════════════════════════════════════════════════════════════════
# PLUG — Extrinsic connector (mirrors PoglsPlug)
# ══════════════════════════════════════════════════════════════════════════

PLUG_N, PLUG_S, PLUG_E, PLUG_W = 0, 1, 2, 3
PLUG_FACES = {0: "N", 1: "S", 2: "E", 3: "W"}


def empty_slot(agent_id: int, token_cap: int = 300) -> dict:
    """Create an empty slot (mirrors PoglsSlot)"""
    return {
        "piece": None,
        "plugs": [{"target_id": 0, "face": 0, "ttl": 0, "active": False}
                  for _ in range(4)],
        "agent_id": agent_id,
        "token_cap": token_cap,
        "rerouted": 0,
    }


def make_slot(agent_id: int, origin_seed: int, fold_axis: int,
              token_cap: int = 300) -> dict:
    """Create a full slot with piece"""
    slot = empty_slot(agent_id, token_cap)
    slot["piece"] = make_piece(origin_seed, fold_axis)
    return slot


def plug_connect(slot_a: dict, face_a: int,
                 slot_b: dict, face_b: int,
                 ttl: int = 64):
    """Connect two slots extrinsically (mirrors pogls_plug_connect)"""
    if face_a > 3 or face_b > 3:
        return
    slot_a["plugs"][face_a] = {
        "target_id": slot_b["agent_id"],
        "face": face_b,
        "ttl": ttl,
        "active": True,
    }
    slot_b["plugs"][face_b] = {
        "target_id": slot_a["agent_id"],
        "face": face_a,
        "ttl": ttl,
        "active": True,
    }


def plug_disconnect(slot: dict, face: int):
    """Disconnect a plug (mirrors pogls_plug_disconnect)"""
    if face > 3:
        return
    slot["plugs"][face] = {"target_id": 0, "face": 0, "ttl": 0, "active": False}


# ══════════════════════════════════════════════════════════════════════════
# REROUTE — Ω fault substitution (mirrors pogls_reroute)
# ══════════════════════════════════════════════════════════════════════════

POGLS_OK       = 0
POGLS_OVERFLOW = 1
POGLS_FAULT    = 2
POGLS_UPSTREAM = 3
POGLS_RETRY    = 4

FAULT_NAMES = ["OK", "OVERFLOW", "FAULT", "UPSTREAM", "RETRY"]


def reroute(slot: dict, fault: int):
    """
    Substitute shape + remix geo_key via fibo fold.
    Preserves bond_L/bond_R from original (bond survives reroute).
    """
    if fault == POGLS_OK:
        return
    piece = slot["piece"]
    if piece is None:
        return
    piece["shape"] = FAULT_SHAPE.get(fault, 'I')
    piece["geo_key"] = fibo_addr(piece["geo_key"] ^ fault)
    slot["rerouted"] = fault


# ══════════════════════════════════════════════════════════════════════════
# WALLET BRIDGE — topology_fp → piece (mirrors test_wallet_bridge)
# ══════════════════════════════════════════════════════════════════════════

def piece_from_wallet(topology_fp: str, fold_axis: int) -> dict:
    """Create a piece from a wallet topology fingerprint string"""
    return make_piece(seed_from_fp(topology_fp), fold_axis)


def piece_chain_from_wallets(wallets: list[tuple[str, int]],
                             names: str = "ABCD") -> dict:
    """
    Create multiple pieces from wallet fingerprints and fold_axes.

    wallets: list of (topology_fp, fold_axis) tuples
    names: agent labels (default ABCD)

    Returns {name: piece_dict}
    """
    return {
        names[i]: piece_from_wallet(fp, axis)
        for i, (fp, axis) in enumerate(wallets)
        if i < len(names)
    }


# ══════════════════════════════════════════════════════════════════════════
# HELPERS
# ══════════════════════════════════════════════════════════════════════════

def piece_to_dict(piece: dict) -> dict:
    """Pretty dict for serialization"""
    return {
        "geo_key": f"{piece['geo_key']:016x}",
        "shape": piece["shape"],
        "bond_L": f"{piece['bond_L']:016x}",
        "bond_R": f"{piece['bond_R']:016x}",
        "bond_key": f"{bond_key(piece):016x}",
    }


def slot_to_dict(slot: dict) -> dict:
    """Pretty dict for serialization"""
    result = {
        "agent_id": slot["agent_id"],
        "shape": slot["piece"]["shape"] if slot["piece"] else "?",
        "rerouted": slot["rerouted"],
        "geo_key": f"{slot['piece']['geo_key']:016x}" if slot["piece"] else "?",
        "plugs": {},
    }
    for face_name in ["N", "S", "E", "W"]:
        plug = slot["plugs"][{"N": 0, "S": 1, "E": 2, "W": 3}[face_name]]
        if plug["active"]:
            result["plugs"][face_name] = {
                "target": f"agent_{plug['target_id']}",
                "face": {"N": 0, "S": 1, "E": 2, "W": 3}[plug["face"]] if isinstance(plug["face"], str) else plug["face"],
                "ttl": plug["ttl"],
            }
    return result


# ══════════════════════════════════════════════════════════════════════════
# TESTS (mirror test_pogls_bond.c)
# ══════════════════════════════════════════════════════════════════════════

def _run_tests():
    passed = failed = 0

    def check(label, ok):
        nonlocal passed, failed
        if ok:
            passed += 1
            print(f"  ✓ {label}")
        else:
            failed += 1
            print(f"  ✗ {label}")

    print("── [1] fibo_addr determinism ──────────────────────")
    seed_a = 0xDEADBEEFCAFE0001
    a1 = fibo_addr(seed_a)
    a2 = fibo_addr(seed_a)
    check("same seed → same addr", a1 == a2)
    a3 = fibo_addr(seed_a ^ 1)
    check("seed±1 → different addr", a1 != a3)

    print("── [2] piece factory — axis→shape ─────────────────")
    axes = [1, 3, 1, 6]
    shapes = ['I', 'T', 'I', 'L']
    names = ['A', 'B', 'C', 'D']
    for ax, sh, nm in zip(axes, shapes, names):
        piece = make_piece(fibo_addr(0x9009000000000001 ^ axes.index(ax)), ax)
        check(f"Agent-{nm} axis={ax} → shape={piece['shape']}", piece['shape'] == sh)

    print("── [3] intrinsic bond ─────────────────────────────")
    seed_a = 0xF1B0000000000001
    seed_b = 0xF1B0000000000002
    pa = make_piece(fibo_addr(seed_a), 1)
    pb = make_piece(fibo_addr(seed_b), 3)
    bk_a = bond_key(pa)
    bk_b = bond_key(pb)
    check("bond_key A != bond_key B", bk_a != bk_b)
    # coordinate shift → bond changes
    pa_shifted = dict(pa)
    pa_shifted["geo_key"] = pa["geo_key"] ^ 1
    pa_shifted["bond_L"] = fibo_addr(pa_shifted["geo_key"] ^ BOND_SALT_L)
    pa_shifted["bond_R"] = fibo_addr(pa_shifted["geo_key"] ^ BOND_SALT_R)
    bk_shifted = bond_key(pa_shifted)
    check("coord shift → bond_key changed", bk_a != bk_shifted)
    check("bond broken automatically", bk_a != bk_shifted)

    print("── [4] wallet bridge ──────────────────────────────")
    fp_A = "a3f0b2c1d4e5f6a7"
    fp_B = "b1e2f3a4c5d6e7f8"
    pA1 = piece_from_wallet(fp_A, 1)
    pA2 = piece_from_wallet(fp_A, 1)
    check("same fp → same geo_key", pA1["geo_key"] == pA2["geo_key"])
    check("same fp → same bond_L", pA1["bond_L"] == pA2["bond_L"])
    check("same fp → same bond_R", pA1["bond_R"] == pA2["bond_R"])

    print("── [5] extrinsic plug chain A─B─C─D ──────────────")
    slots = [make_slot(i, fibo_addr(0x9009009009009009 ^ i), axes[i])
             for i in range(4)]
    plug_connect(slots[0], PLUG_E, slots[1], PLUG_W, 64)
    plug_connect(slots[1], PLUG_E, slots[2], PLUG_W, 64)
    plug_connect(slots[2], PLUG_E, slots[3], PLUG_W, 64)
    check("A.E → B.W active", slots[0]["plugs"][PLUG_E]["active"])
    check("B.W ← A.E active", slots[1]["plugs"][PLUG_W]["active"])
    check("D.W ← C.E active", slots[3]["plugs"][PLUG_W]["active"])

    print("── [6] Ω reroute ──────────────────────────────────")
    slot = make_slot(99, fibo_addr(0xDEAD0000BEEF0000), 1)
    orig_geo = slot["piece"]["geo_key"]
    orig_bl = slot["piece"]["bond_L"]
    orig_br = slot["piece"]["bond_R"]
    reroute(slot, POGLS_OVERFLOW)
    check("shape → OMEGA_COMPRESS", slot["piece"]["shape"] == OMEGA_COMPRESS)
    check("rerouted=1", slot["rerouted"] == 1)
    check("geo_key changed", slot["piece"]["geo_key"] != orig_geo)
    check("bond_L unchanged", slot["piece"]["bond_L"] == orig_bl)
    check("bond_R unchanged", slot["piece"]["bond_R"] == orig_br)

    print(f"\n── RESULT: {passed}/{passed+failed} passed ────────")
    return passed, failed


if __name__ == "__main__":
    _run_tests()
