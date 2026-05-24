"""
poc_bond_standalone.py
─────────────────────────────────────────────────────────────────────
Portable POGLS Bond sandbox — runs on any OS, zero external deps.

Demonstrates the full C↔Python bridge:
  1. fibo_addr determinism + avalanche
  2. Piece factory (axis → shape)
  3. Intrinsic bond: coordinate-bound self-enforcement
  4. topology_fp → seed → piece (wallet bridge simulation)
  5. 4-agent plug chain (A─B─C─D routing topology)
  6. Ω reroute chain (fault → shape substitution)
  7. Session nonce isolation (replay protection)
  8. False-positive rate stress test (N=50000)

Run:
    python poc_bond_standalone.py
    python poc_bond_standalone.py --nonce 0xDEAD000000000001
    POGLS_SO_PATH=/path/to/pogls_bond.so python poc_bond_standalone.py
─────────────────────────────────────────────────────────────────────
"""

import sys
import os
import time
import argparse
from pathlib import Path

# ── ensure we find pogls_bridge next to this file ─────────────────
sys.path.insert(0, str(Path(__file__).resolve().parent))

from pogls_bridge import (
    PoglsBridge,
    FACE_N, FACE_S, FACE_E, FACE_W,
    FAULT_OK, FAULT_OVERFLOW, FAULT_FAULT, FAULT_UPSTREAM, FAULT_RETRY,
    SHAPE_I, SHAPE_O, SHAPE_T, SHAPE_S, SHAPE_Z, SHAPE_L, SHAPE_J,
)

# ── CLI ────────────────────────────────────────────────────────────
parser = argparse.ArgumentParser(description="POGLS Bond standalone sandbox")
parser.add_argument("--nonce", default="0", help="Session nonce (hex or dec)")
parser.add_argument("--so",    default=None, help="Path to pogls_bond.so")
args = parser.parse_args()
SESSION_NONCE = int(args.nonce, 0)

# ── helpers ────────────────────────────────────────────────────────
pass_count = 0
fail_count = 0

def section(n, title):
    print(f"\n{'═'*62}")
    print(f"  [{n}] {title}")
    print(f"{'═'*62}")

def check(cond, msg):
    global pass_count, fail_count
    mark = "✓" if cond else "✗ FAIL"
    print(f"  {mark}  {msg}")
    if cond:
        pass_count += 1
    else:
        fail_count += 1
        # don't abort — collect all failures

def info(msg):
    print(f"      {msg}")


# ══════════════════════════════════════════════════════════════════
# INIT
# ══════════════════════════════════════════════════════════════════
print("═" * 62)
print("  POGLS BOND — standalone C↔Python bridge demo")
print("═" * 62)

br = PoglsBridge(so_path=args.so)
print(f"\n  bridge   : {br}")
print(f"  so path  : {br._path}")

if SESSION_NONCE:
    br.set_nonce(SESSION_NONCE)
    print(f"  nonce    : {SESSION_NONCE:#018x}")
else:
    print(f"  nonce    : 0 (backward-compat mode)")


# ══════════════════════════════════════════════════════════════════
# [1] fibo_addr determinism + avalanche
# ══════════════════════════════════════════════════════════════════
section(1, "fibo_addr determinism + avalanche")

seed = 0xDEADBEEFCAFE0001
a = br.fibo_addr(seed)
b = br.fibo_addr(seed)
check(a == b, f"same seed → same addr: {a:#018x}")

c = br.fibo_addr(seed ^ 1)
check(a != c, f"seed±1 → different addr")

diff   = a ^ c
popcnt = bin(diff).count('1')
info(f"1-bit seed flip → {popcnt}/64 bits changed (avalanche)")
check(popcnt >= 16, f"avalanche ≥ 16 bits changed (got {popcnt})")


# ══════════════════════════════════════════════════════════════════
# [2] Piece factory — all 7 axes
# ══════════════════════════════════════════════════════════════════
section(2, "piece factory — axis → shape")

AXIS_SHAPES = {1: SHAPE_I, 2: SHAPE_O, 3: SHAPE_T, 4: SHAPE_S,
               5: SHAPE_Z, 6: SHAPE_L, 7: SHAPE_J}

base_seed = 0x9009000000000001
for axis, expected_shape in AXIS_SHAPES.items():
    seed_i = br.fibo_addr(base_seed ^ axis)
    p      = br.make_piece(seed_i, axis)
    check(p.shape == expected_shape,
          f"axis={axis} → shape={p.shape_char}  geo={p.geo_key:#018x}")

# axis=0 → 0x00 sentinel
p0 = br.make_piece(base_seed, axis=0)
check(p0.shape == 0x00, f"axis=0 → 0x00 sentinel (unused)")

# axis>=8 → SHAPE_I fallback
p8 = br.make_piece(base_seed, axis=8)
check(p8.shape == SHAPE_I, f"axis≥8 → SHAPE_I fallback  got={p8.shape_char}")


# ══════════════════════════════════════════════════════════════════
# [3] Intrinsic bond: coordinate enforcement
# ══════════════════════════════════════════════════════════════════
section(3, "intrinsic bond — coordinate-bound self-enforcement")

seed_A = br.fibo_addr(0xF1B0000000000001)
seed_B = br.fibo_addr(0xF1B0000000000002)
A = br.make_piece(seed_A, axis=1)
B = br.make_piece(seed_B, axis=3)

ka_orig = br.bond_key(A)
info(f"A bond_key = {ka_orig:#018x}")

# simulate coordinate shift: rebuild piece with slightly different seed
seed_A_shifted = br.fibo_addr(0xF1B0000000000001 ^ 1)
A_shifted = br.make_piece(seed_A_shifted, axis=1)
ka_shifted = br.bond_key(A_shifted)
check(ka_orig != ka_shifted,
      f"coord shift → bond_key changes: {ka_orig:#018x} → {ka_shifted:#018x}")
check(A.geo_key != A_shifted.geo_key, "geo_key differs after shift")

# all 8 low-order bit flips each produce different bond_key
keys_seen = set()
for bit in range(8):
    s = br.fibo_addr((0xF1B0000000000001 ^ (1 << bit)))
    p = br.make_piece(s, axis=1)
    keys_seen.add(br.bond_key(p))
check(len(keys_seen) == 8, f"8 distinct bond_keys for 8 bit flips (got {len(keys_seen)})")


# ══════════════════════════════════════════════════════════════════
# [4] topology_fp → seed → piece  (wallet bridge simulation)
# ══════════════════════════════════════════════════════════════════
section(4, "topology_fp → seed → piece  (wallet bridge)")

# Simulated topology fingerprints as Python side would produce them
fps = {
    "A": "a3f0b2c1d4e5f6a7",
    "B": "b1e2f3a4c5d6e7f8",
    "C": "c2d3e4f5a6b7c8d9",
}

pieces_by_fp = {}
for label, fp in fps.items():
    s1 = br.seed_from_fp(fp)
    s2 = br.seed_from_fp(fp)
    check(s1 == s2, f"fp[{label}] reproducible: seed={s1:#018x}")
    p1 = br.make_piece(s1, axis=1)
    p2 = br.make_piece(s2, axis=1)
    check(p1.geo_key == p2.geo_key, f"fp[{label}] → same geo_key")
    check(p1.bond_L  == p2.bond_L,  f"fp[{label}] → same bond_L")
    pieces_by_fp[label] = p1

info(f"Pieces from wallet fps: {[f'{l}={p}' for l, p in pieces_by_fp.items()]}")


# ══════════════════════════════════════════════════════════════════
# [5] 4-agent plug chain  A─B─C─D
# ══════════════════════════════════════════════════════════════════
section(5, "4-agent plug chain  A─B─C─D  (routing topology)")

AGENTS = [
    ("A", 1, 100),   # I-pipe,     token_cap=100
    ("B", 3, 200),   # T-splitter, token_cap=200
    ("C", 1, 150),   # I-pipe,     token_cap=150
    ("D", 6, 300),   # L-fork-L,   token_cap=300
]

chain_base = 0x9009009009009009
slots = {}
for label, axis, cap in AGENTS:
    seed_i = br.fibo_addr(chain_base ^ ord(label))
    slots[label] = br.make_slot(seed_i, axis=axis, agent_id=ord(label), token_cap=cap)
    info(f"Agent-{label}: {slots[label]}")

# wire: A.E ↔ B.W ↔ C.W ↔ D.W
br.plug_connect(slots["A"], FACE_E, slots["B"], FACE_W, ttl=64)
br.plug_connect(slots["B"], FACE_E, slots["C"], FACE_W, ttl=64)
br.plug_connect(slots["C"], FACE_E, slots["D"], FACE_W, ttl=64)

check(slots["A"]._raw.plugs[FACE_E].active == 1, "A.E → B.W active")
check(slots["B"]._raw.plugs[FACE_W].active == 1, "B.W ← A.E symmetric")
check(slots["D"]._raw.plugs[FACE_W].active == 1, "D.W ← C.E active")
check(slots["D"]._raw.plugs[FACE_E].active == 0, "D.E not connected (end of chain)")

# TTL is stored correctly
check(slots["A"]._raw.plugs[FACE_E].ttl == 64, "A.E TTL=64")

# disconnect mid-chain
br.plug_disconnect(slots["B"], FACE_E)
check(slots["B"]._raw.plugs[FACE_E].active == 0, "B.E disconnected")
check(slots["C"]._raw.plugs[FACE_W].active == 1, "C.W still active (no auto-teardown)")

for label in AGENTS:
    l = label[0]
    d = slots[l].to_dict()
    active_plugs = list(d["plugs"].keys())
    info(f"Agent-{l} active plugs: {active_plugs or '(none)'}")


# ══════════════════════════════════════════════════════════════════
# [6] Ω reroute chain
# ══════════════════════════════════════════════════════════════════
section(6, "Ω reroute chain — geo evolves, bond survives")

seed_r = br.fibo_addr(0xABCD000000000001)
slot_r = br.make_slot(seed_r, axis=1, agent_id=99)

bond_L_orig = slot_r._raw.piece.bond_L
bond_R_orig = slot_r._raw.piece.bond_R
geo_prev    = slot_r._raw.piece.geo_key

FAULTS = [
    (FAULT_OVERFLOW, "OVERFLOW", "I"),
    (FAULT_FAULT,    "FAULT",    "L"),
    (FAULT_UPSTREAM, "UPSTREAM", "T"),
    (FAULT_RETRY,    "RETRY",    "O"),
]
for fault, name, expected_shape in FAULTS:
    br.reroute(slot_r, fault)
    new_geo = slot_r._raw.piece.geo_key
    check(new_geo != geo_prev,
          f"after {name:<10}: geo mutated  → {new_geo:#018x}")
    check(slot_r._raw.piece.bond_L == bond_L_orig, f"{name}: bond_L stable")
    check(slot_r._raw.piece.bond_R == bond_R_orig, f"{name}: bond_R stable")
    check(slot_r.shape_char == expected_shape,
          f"{name}: shape → {slot_r.shape_char} (expected {expected_shape})")
    geo_prev = new_geo


# ══════════════════════════════════════════════════════════════════
# [7] Session nonce isolation
# ══════════════════════════════════════════════════════════════════
section(7, "session nonce isolation — replay protection")

p_x = br.make_piece(br.fibo_addr(0xCAFE000000000001), axis=1)
p_y = br.make_piece(br.fibo_addr(0xCAFE000000000002), axis=3)

br.set_nonce(0xDEAD000000000001)
v1, bk1 = br.bond_verify(p_x, p_y)
info(f"nonce=DEAD...: valid={v1}  bond_key={bk1:#018x}")

br.set_nonce(0xBEEF000000000002)
v2, bk2 = br.bond_verify(p_x, p_y)
info(f"nonce=BEEF...: valid={v2}  bond_key={bk2:#018x}")

check(bk1 == bk2, "bond_key (raw XOR) stable across nonces — safe for indexing")
check(br.fibo_addr(bk1 ^ 0xDEAD000000000001) !=
      br.fibo_addr(bk1 ^ 0xBEEF000000000002),
      "different nonce → different combined hash (replay attack blocked)")

# restore nonce for final test
br.set_nonce(SESSION_NONCE)


# ══════════════════════════════════════════════════════════════════
# [8] False-positive rate stress test
# ══════════════════════════════════════════════════════════════════
section(8, f"false-positive rate stress test (N=50,000 random pairs)")

br.set_nonce(0)   # deterministic run
fp_count = 0
N        = 50_000
K1       = 0x9e3779b97f4a7c15
K2       = 0x6c62272e07bb0142

t0 = time.perf_counter()
for i in range(N):
    sx = br.fibo_addr((i * K1) ^ 0xAAAAAAAA00000001)
    sy = br.fibo_addr((i * K2) ^ 0x5555555500000002)
    px = br.make_piece(sx, axis=1)
    py = br.make_piece(sy, axis=3)
    valid, _ = br.bond_verify(px, py)
    if valid:
        fp_count += 1
elapsed = time.perf_counter() - t0

info(f"false positives : {fp_count} / {N}  ({fp_count/N*100:.6f}%)")
info(f"elapsed         : {elapsed*1000:.1f} ms  ({N/elapsed:.0f} verify/s)")
info(f"verify bits     : {br.verify_bits}")
check(fp_count == 0, f"zero false positives at N={N:,} (32-bit mask)")

br.set_nonce(SESSION_NONCE)


# ══════════════════════════════════════════════════════════════════
# SUMMARY
# ══════════════════════════════════════════════════════════════════
total = pass_count + fail_count
print(f"\n{'═'*62}")
if fail_count == 0:
    print(f"  ALL TESTS PASSED  ({pass_count}/{total})")
else:
    print(f"  FAILED: {fail_count}   PASSED: {pass_count}/{total}")
print(f"{'═'*62}\n")

sys.exit(1 if fail_count else 0)
