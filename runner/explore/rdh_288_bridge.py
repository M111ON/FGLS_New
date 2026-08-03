#!/usr/bin/env python3
"""
rdh_288_bridge.py — RDH ↔ 288-Cell Bridge Analysis
═══════════════════════════════════════════════════════

Goal: Find the MISSING STEP between 288-cell geometry and the existing RDH pipeline.

Current RDH flow:
  data → rdh_capture() → flat_key (0..20735)
  → enc = flat_key % 1440 → frame_seek → (face, slot, phase, ico_idx)

288-cell target flow:
  data → 288-cell position → decagram {10/3} walk → 1728 → 20736
  Key: 288 × 6 × 12 = 20736 = 12⁴

The bridge connects these two flows.
"""

from math import gcd
from collections import defaultdict

# ════════════════════════════════════════════════════════════════
# SACRED CONSTANTS (from coord_spine.h, geo_frame_seek.h, rdh_capture.h)
# ════════════════════════════════════════════════════════════════

# RDH address space
RDH_RINGS      = 144     # cfg.n_rings
RDH_WEDGES     = 144     # cfg.n_wedges
RDH_CAPACITY   = RDH_RINGS * RDH_WEDGES  # 20736

# Frame seek
FRAME_CYCLE    = 1440    # = 12 × 120
FRAME_STRIDE   = 37      # prime, gcd(37,1440)=1
FRAME_FACE_SZ  = 120     # slots per face (1440/12)
FRAME_EDGES    = 12      # edges per frame (9H + 3P)
FRAME_ICO_NODES = 162    # icosphere L2 (81×2)

# 288-cell architecture
CELL_288       = 288     # = 576/2 = 2×144 = GEO_BLOCK_BOUNDARY
CELL_DIRS      = 6       # 6-fold symmetry on 12-gon
CELL_PER_FACE  = 1728    # = 288 × 6 = 12³
DODECA_FACES   = 12      # dodecahedron faces
GEO_FULL       = 20736   # = 144² = 12⁴

# Geometry
GEO_SLOTS      = 576     # 24² = 9×64
GEO_FULL_N     = 3456    # = 576×6 = 6×576
GEO_BLOCK      = 48      # Metatron atomic unit

print("=" * 72)
print("RDH ↔ 288-CELL BRIDGE ANALYSIS")
print("=" * 72)

# ════════════════════════════════════════════════════════════════
# 1. NUMBER CHAIN VERIFICATION
# ════════════════════════════════════════════════════════════════
print("\n[1] NUMBER CHAIN — verifying all relationships")
print("-" * 60)

checks = [
    ("144² = 20736",        144**2 == 20736),
    ("12⁴ = 20736",         12**4 == 20736),
    ("288 × 6 × 12 = 20736", 288 * 6 * 12 == 20736),
    ("288 × 72 = 20736",    288 * 72 == 20736),
    ("1728 × 12 = 20736",   1728 * 12 == 20736),
    ("1728 = 288 × 6",      1728 == 288 * 6),
    ("1728 = 12³",          1728 == 12**3),
    ("288 = 576/2",         288 == 576 // 2),
    ("288 = 2×144",         288 == 2 * 144),
    ("1440 = 12 × 120",     1440 == 12 * 120),
    ("1728 = 12 × 144",     1728 == 12 * 144),
    ("gcd(1440, 1728) = 288", gcd(1440, 1728) == 288),
    ("1440/288 = 5",        1440 // 288 == 5),
    ("1728/288 = 6",        1728 // 288 == 6),
    ("gcd(37, 1440) = 1",   gcd(37, 1440) == 1),
    ("gcd(37, 1728) = 1",   gcd(37, 1728) == 1),
    ("gcd(37, 288) = 1",    gcd(37, 288) == 1),
]
for label, ok in checks:
    status = "✓" if ok else "✗ FAIL"
    print(f"  {status}  {label}")

# ════════════════════════════════════════════════════════════════
# 2. CURRENT RDH PIPELINE — trace the flow
# ════════════════════════════════════════════════════════════════
print("\n[2] CURRENT RDH PIPELINE — data flow")
print("-" * 60)

print("""
  rdh_capture(data, len, cfg={144,144,1,1,1})
    │
    │  Walk data bytes: each nibble → direction on 12-gon (0..11)
    │  Accumulate (acc_x, acc_y) with periodic fold every 4096 steps
    │  Final: wedge = acc_x % 144, ring = acc_y % 144
    │
    ▼
  flat_key = ring × 144 + wedge    ∈ [0, 20735]
    │
    │  rdh_capture_to_enc: enc = flat_key % 1440
    │  THIS IS THE LOSSY STEP: 20736 → 1440
    │
    ▼
  enc ∈ [0, 1439]   ← 1440 = 12 faces × 120 slots
    │
    │  frame_seek / frame_at(enc):
    │
    ├──→ face = enc / 120              (0..11,  dodecahedron face)
    ├──→ slot = enc % 120              (0..119, slot within face)
    ├──→ phase = (enc / 12) % 12       (0..11,  iteration phase)
    ├──→ ico_idx = enc % 162           (0..161, icosphere address)
    ├──→ h.group = face % 3            (0..2,   Hilbert group)
    ├──→ h.edge = enc % 3              (0..2,   trit position)
    └──→ h.is_skip = (enc % 12) >= 9   (bool,   invert point)

  stride-37 walk on 1440: enc(t) = (t × 37) % 1440
  Full bijection: gcd(37, 1440) = 1
""")

# Show the lossy mapping
print("  Loss analysis of flat_key % 1440:")
coverage = set()
for fk in range(RDH_CAPACITY):
    enc = fk % FRAME_CYCLE
    coverage.add(enc)
print(f"    flat_key range: 0..{RDH_CAPACITY-1} ({RDH_CAPACITY} values)")
print(f"    enc range:      0..{FRAME_CYCLE-1} ({FRAME_CYCLE} values)")
print(f"    Coverage:       {len(coverage)}/{FRAME_CYCLE} = {len(coverage)/FRAME_CYCLE:.1%}")
print(f"    Information lost: {RDH_CAPACITY - FRAME_CYCLE} flat_keys map to same enc")
print(f"    Compression ratio: {RDH_CAPACITY}/{FRAME_CYCLE} = {RDH_CAPACITY/FRAME_CYCLE:.2f}x")

# ════════════════════════════════════════════════════════════════
# 3. THE GAP: 1440 vs 1728
# ════════════════════════════════════════════════════════════════
print("\n[3] THE GAP — 1440 vs 1728")
print("-" * 60)

print(f"""
  Current frame_seek cycle:  1440 = 12 × 120
  288-cell target cycle:     1728 = 12 × 144

  Difference:  1728 - 1440 = 288  ← THIS IS THE KEY NUMBER

  Decomposition of 1440 by 288:
    1440 = 288 × 5       → 5 "directions" per cell in current system
    1728 = 288 × 6       → 6 directions per cell in 288-cell system

  The MISSING DIRECTION:
    Current system uses 5 out of 6 directions per 288-cell.
    The 6th direction is the gap between 1440 and 1728.

  This is NOT a bug — it's the decagram {10/3} geometry:
    - 12-gon has 12 vertices
    - {10/3} decagram visits 10 vertices with step 3
    - 10 of 12 = 5/6 ratio ← matches 1440/1728 = 5/6!
""")

# ════════════════════════════════════════════════════════════════
# 4. THE BRIDGE: flat_key → 288-cell address
# ════════════════════════════════════════════════════════════════
print("[4] THE BRIDGE — flat_key → 288-cell address")
print("-" * 60)

print("""
  The MISSING STEP is a mixed-radix decomposition of flat_key
  into the 288-cell coordinate system:

  ┌─────────────────────────────────────────────────────────┐
  │  bridge_288(flat_key):                                 │
  │                                                         │
  │    face      = flat_key / 1728     (0..11)              │
  │    rem       = flat_key % 1728                          │
  │    direction = rem / 288           (0..5)               │
  │    cell_pos  = rem % 288           (0..287)             │
  │                                                         │
  │  Verification: 12 × 6 × 288 = 20736 = GEO_FULL ✓      │
  └─────────────────────────────────────────────────────────┘

  This function DOES NOT EXIST in the current codebase.
  It is the missing link between rdh_capture and 288-cell addressing.
""")

# Verify the bridge decomposition
print("  Bridge decomposition verification:")
bridge_coverage = set()
for fk in range(GEO_FULL):
    face = fk // CELL_PER_FACE
    rem = fk % CELL_PER_FACE
    direction = rem // CELL_288
    cell_pos = rem % CELL_288
    assert 0 <= face < 12, f"face out of range: {face}"
    assert 0 <= direction < 6, f"direction out of range: {direction}"
    assert 0 <= cell_pos < 288, f"cell_pos out of range: {cell_pos}"
    addr = (face, direction, cell_pos)
    bridge_coverage.add(addr)

print(f"    Unique (face, dir, cell) tuples: {len(bridge_coverage)}")
print(f"    Expected: 12 × 6 × 288 = {12*6*288}")
print(f"    Bijection: {'✓' if len(bridge_coverage) == 20736 else '✗'}")

# ════════════════════════════════════════════════════════════════
# 5. REVERSE BRIDGE: 288-cell → flat_key
# ════════════════════════════════════════════════════════════════
print("\n[5] REVERSE BRIDGE — 288-cell → flat_key")
print("-" * 60)

print("""
  ┌─────────────────────────────────────────────────────────┐
  │  bridge_288_key(face, direction, cell_pos):             │
  │                                                         │
  │    flat_key = face × 1728 + direction × 288 + cell_pos  │
  │                                                         │
  │  This is the standard mixed-radix encoding.             │
  │  Bijection guaranteed by rdh_addr.h design.             │
  └─────────────────────────────────────────────────────────┘
""")

# Verify round-trip
print("  Round-trip verification:")
roundtrip_ok = True
for fk in range(GEO_FULL):
    face = fk // CELL_PER_FACE
    rem = fk % CELL_PER_FACE
    direction = rem // CELL_288
    cell_pos = rem % CELL_288
    fk_back = face * CELL_PER_FACE + direction * CELL_288 + cell_pos
    if fk_back != fk:
        roundtrip_ok = False
        print(f"    FAIL: fk={fk} → ({face},{direction},{cell_pos}) → {fk_back}")
        break
print(f"    All {GEO_FULL} round-trips: {'✓ PASS' if roundtrip_ok else '✗ FAIL'}")

# ════════════════════════════════════════════════════════════════
# 6. STRIDE-37 ON 1728 — extended frame_seek
# ════════════════════════════════════════════════════════════════
print("\n[6] STRIDE-37 ON 1728 — extended frame_seek")
print("-" * 60)

# Verify stride-37 is a full bijection on 1728
visited = set()
e = 0
for t in range(1728):
    if e in visited:
        print(f"  ✗ Duplicate at t={t}, enc={e}")
        break
    visited.add(e)
    e = (e + 37) % 1728
if e == 0 and len(visited) == 1728:
    print("  ✓ stride-37 full bijection on 1728 verified")
else:
    print(f"  ✗ stride-37 failed: returned to start={e==0}, visited={len(visited)}")

# Compare 1440 vs 1728 walk
print(f"\n  Walk comparison:")
print(f"    stride-37 on 1440: gcd(37,1440) = {gcd(37,1440)} → {'bijection' if gcd(37,1440)==1 else 'NOT bijection'}")
print(f"    stride-37 on 1728: gcd(37,1728) = {gcd(37,1728)} → {'bijection' if gcd(37,1728)==1 else 'NOT bijection'}")
print(f"    stride-37 on 288:  gcd(37,288)  = {gcd(37,288)}  → {'bijection' if gcd(37,288)==1 else 'NOT bijection'}")
print(f"    stride-37 on 20736: gcd(37,20736) = {gcd(37,20736)} → {'bijection' if gcd(37,20736)==1 else 'NOT bijection'}")

# ════════════════════════════════════════════════════════════════
# 7. DECAMGRAM {10/3} WALK ON 288
# ════════════════════════════════════════════════════════════════
print("\n[7] DECAGRAM {10/3} WALK ON 288")
print("-" * 60)

print("""
  The {10/3} decagram on a 12-gon:
    - 12 vertices (0..11)
    - Step size = 3
    - Visits: 0, 3, 6, 9, 0, ...  (cycle of length 4 = 12/gcd(3,12))

  But this is NOT a full decagram — it's a partial cycle.
  The TRUE decagram {10/3} uses 10 vertices with step 3.

  On 288 positions:
    288 = 12 × 24 = 12 × (4 × 6)
    Each 12-gon cycle: 4 vertices (stride-3)
    24 groups of 4 = 96 positions per "layer"
    3 layers × 96 = 288

  The decagram walk on 288 connects the 6 directions:
    direction 0 → direction 3 → direction 0 (on 12-gon: 0→3→6→9→0)
    direction 1 → direction 4 → direction 1
    direction 2 → direction 5 → direction 2
""")

# Simulate decagram {10/3} on 288-cell
print("  Decagram {10/3} walk simulation on 288:")
decagram_order = []
pos = 0
for step in range(288):
    decagram_order.append(pos)
    pos = (pos + 3) % 288  # stride-3 on 288 positions

# Check coverage
print(f"    Walk length: {len(decagram_order)}")
print(f"    Unique positions: {len(set(decagram_order))}")
if len(set(decagram_order)) == 288:
    print(f"    ✓ Full coverage of 288-cell positions")
else:
    print(f"    ✗ Only {len(set(decagram_order))}/288 positions covered")
    # Find the cycle length
    pos = 0
    seen = {}
    for i in range(288):
        if pos in seen:
            print(f"    Cycle length: {i - seen[pos]}")
            break
        seen[pos] = i
        pos = (pos + 3) % 288

# ════════════════════════════════════════════════════════════════
# 8. THE EXACT MISSING STEP
# ════════════════════════════════════════════════════════════════
print("\n[8] THE EXACT MISSING STEP")
print("-" * 60)

print("""
  ┌─────────────────────────────────────────────────────────────────┐
  │                                                                 │
  │  EXISTING RDH PIPELINE:                                         │
  │                                                                 │
  │    data → rdh_capture() → flat_key ∈ [0, 20735]                │
  │    → enc = flat_key % 1440                                      │
  │    → frame_seek(enc) → (face, slot, phase, ico_idx)             │
  │                                                                 │
  │  288-CELL TARGET PIPELINE:                                      │
  │                                                                 │
  │    data → rdh_capture() → flat_key ∈ [0, 20735]                │
  │    → bridge_288(flat_key) → (face, direction, cell_pos)         │
  │    → decagram_walk(cell_pos) → 1728 address                     │
  │    → frame_seek_1728(enc_1728) → (face, slot, phase, ico_idx)   │
  │                                                                 │
  │  ═══════════════════════════════════════════════════════════    │
  │  THE MISSING STEP IS:                                           │
  │                                                                 │
  │    flat_key → bridge_288() → (face, direction, cell_pos)        │
  │                                                                 │
  │  This function decomposes the 144×144 flat_key into:            │
  │    - face (0..11): which dodecahedron face                      │
  │    - direction (0..5): which of 6 symmetry directions           │
  │    - cell_pos (0..287): position within the 288-cell            │
  │                                                                 │
  │  The 6th direction (direction=5) is the "missing" direction     │
  │  that doesn't fit in the current 1440-cycle frame_seek.         │
  │                                                                 │
  │  1440 = 288 × 5 covers only 5 directions.                      │
  │  1728 = 288 × 6 covers all 6 directions.                       │
  │  The gap = 288 = exactly 1 cell cycle of 6 directions.         │
  │                                                                 │
  └─────────────────────────────────────────────────────────────────┘
""")

# ════════════════════════════════════════════════════════════════
# 9. HOW THE 1440 CYCLE RELATES TO 288
# ════════════════════════════════════════════════════════════════
print("[9] HOW 1440 CYCLE RELATES TO 288-CELL")
print("-" * 60)

print("""
  Current system:
    enc ∈ [0, 1439]
    cell_pos = enc % 288     (0..287)  ← extractable from enc
    slot_in_cell = enc / 288 (0..4)    ← only 5 values (not 6!)

  The current frame_seek implicitly assigns:
    - face = enc / 120
    - slot = enc % 120
    But 120 ≠ 144, so the 288-cell structure is NOT exposed.

  To expose it, replace frame_seek with:
    enc_288 = flat_key % 1728           (not % 1440)
    cell_pos = enc_288 % 288            (0..287)
    direction = (enc_288 / 288) % 6     (0..5, all 6 directions)
    face = flat_key / 1728              (0..11)
""")

# Show the 1440 vs 1728 mapping
print("  Mapping flat_key to both systems:")
print(f"    {'flat_key':>8} {'%1440':>6} {'face12':>6} {'slot12':>6} {'%1728':>6} {'face17':>6} {'dir':>4} {'cell':>5}")
print(f"    {'-'*8} {'-'*6} {'-'*6} {'-'*6} {'-'*6} {'-'*6} {'-'*4} {'-'*5}")

for fk in [0, 119, 120, 287, 288, 1439, 1440, 1727, 1728, 20735]:
    enc1440 = fk % 1440
    face12 = enc1440 // 120
    slot12 = enc1440 % 120

    enc1728 = fk % 1728
    face17 = fk // 1728
    direction = (enc1728 // 288) % 6
    cell_pos = enc1728 % 288

    print(f"    {fk:>8} {enc1440:>6} {face12:>6} {slot12:>6} {enc1728:>6} {face17:>6} {direction:>4} {cell_pos:>5}")

# ════════════════════════════════════════════════════════════════
# 10. RDH_TIER0 AND THE 128×162 CONNECTION
# ════════════════════════════════════════════════════════════════
print("\n[10] RDH_TIER0 AND 128×162 CONNECTION")
print("-" * 60)

print("""
  RDH_TIER0 = { 128, 162, 1, 1, 1 }  →  total = 20736

  This maps:
    ring  ∈ [0, 127]  ("CPU world" = 128)
    wedge ∈ [0, 161]  ("icosphere" = 162)

  The 128×162 decomposition is ANOTHER way to fill 20736:
    128 × 162 = 20736 ✓

  But the 288-cell decomposition is:
    288 × 6 × 12 = 20736 ✓

  These are DIFFERENT geometric decompositions of the same space.
  The bridge function connects them:
    rdh_capture uses 144×144 (RDH_CAPTURE_144)
    The 288-cell uses 288×6×12
    The conversion: flat_key (144×144) → bridge_288 → (face, dir, cell)
""")

# Verify 128×162 vs 144×144
print(f"  128 × 162 = {128*162} = {'GEO_FULL' if 128*162 == 20736 else 'NOT GEO_FULL'}")
print(f"  144 × 144 = {144*144} = {'GEO_FULL' if 144*144 == 20736 else 'NOT GEO_FULL'}")
print(f"  Both = {20736} = 12⁴ = 144²")

# ════════════════════════════════════════════════════════════════
# 11. C CODE FOR THE BRIDGE
# ════════════════════════════════════════════════════════════════
print("\n[11] C CODE — rdh_288_bridge.h (THE MISSING HEADER)")
print("-" * 60)

c_code = """
/* rdh_288_bridge.h — Bridge: RDH flat_key ↔ 288-cell address
 * ══════════════════════════════════════════════════════════════
 *
 * THE MISSING STEP between rdh_capture and 288-cell geometry.
 *
 * Current flow (lossy):
 *   flat_key % 1440 → enc → frame_seek → (face, slot, phase, ico_idx)
 *
 * 288-cell flow (full):
 *   flat_key → bridge_288 → (face, direction, cell_pos)
 *   → decagram_walk(cell_pos) → frame_seek_1728 → full decomposition
 *
 * Constants:
 *   CELL_288      = 288    (GEO_BLOCK_BOUNDARY)
 *   CELL_DIRS     = 6      (6-fold symmetry)
 *   CELL_PER_FACE = 1728   (288 × 6 = 12³)
 *   DODECA_FACES  = 12
 *   GEO_FULL      = 20736  (144² = 12⁴)
 *
 * No malloc. No float. O(1). Bijection guaranteed.
 * ══════════════════════════════════════════════════════════════ */

#ifndef RDH_288_BRIDGE_H
#define RDH_288_BRIDGE_H

#include <stdint.h>

/* ── 288-cell address ─────────────────────────────── */
#define CELL_288      288u
#define CELL_DIRS       6u
#define CELL_PER_FACE 1728u
#define DODECA_FACES   12u

typedef struct {
    uint16_t cell_pos;    /* 0..287  — position within 288-cell  */
    uint8_t  direction;   /* 0..5    — 6-fold symmetry direction */
    uint8_t  face;        /* 0..11   — dodecahedron face         */
} Cell288Addr;

/* ── flat_key → 288-cell (THE MISSING STEP) ────────── */
static inline Cell288Addr bridge_288(int64_t flat_key) {
    Cell288Addr a;
    a.face      = (uint8_t)((flat_key / CELL_PER_FACE) % DODECA_FACES);
    a.direction = (uint8_t)((flat_key / CELL_288) % CELL_DIRS);
    a.cell_pos  = (uint16_t)(flat_key % CELL_288);
    return a;
}

/* ── 288-cell → flat_key (reverse) ─────────────────── */
static inline int64_t bridge_288_key(Cell288Addr a) {
    return (int64_t)a.face * CELL_PER_FACE
         + (int64_t)a.direction * CELL_288
         + (int64_t)a.cell_pos;
}

/* ── frame_seek on 1728 (extended cycle) ───────────── */
#define FRAME_1728_CYCLE  1728u
#define FRAME_1728_STRIDE   37u  /* gcd(37,1728)=1, full bijection */

static inline uint16_t frame_1728_enc(uint32_t t) {
    return (uint16_t)((t * FRAME_1728_STRIDE) % FRAME_1728_CYCLE);
}

static inline uint16_t frame_1728_next(uint16_t enc) {
    return (uint16_t)((enc + FRAME_1728_STRIDE) % FRAME_1728_CYCLE);
}

/* ── decode 1728 enc into 288-cell components ──────── */
static inline void frame_1728_decode(uint16_t enc_1728,
                                      uint8_t *face,
                                      uint8_t *direction,
                                      uint16_t *cell_pos) {
    *face      = (uint8_t)(enc_1728 / CELL_PER_FACE);
    *direction = (uint8_t)((enc_1728 / CELL_288) % CELL_DIRS);
    *cell_pos  = (uint16_t)(enc_1728 % CELL_288);
}

/* ── verify: stride-37 full cycle on 1728 ──────────── */
static inline int bridge_288_verify(void) {
    uint8_t visited[1728] = {0};
    uint16_t e = 0;
    for (uint32_t i = 0; i < 1728; i++) {
        if (visited[e]) return -1;
        visited[e] = 1;
        e = frame_1728_next(e);
    }
    return (e == 0) ? 0 : -2;
}

#endif /* RDH_288_BRIDGE_H */
"""
print(c_code)

# ════════════════════════════════════════════════════════════════
# 12. SUMMARY
# ════════════════════════════════════════════════════════════════
print("\n" + "=" * 72)
print("SUMMARY — THE MISSING STEP FOUND")
print("=" * 72)

print("""
  WHAT EXISTS:
    ✓ rdh_capture() — data → flat_key ∈ [0, 20735]
    ✓ frame_seek    — enc(0..1439) → (face, slot, phase, ico_idx)
    ✓ stride-37     — full bijection on 1440
    ✓ GEO_FULL      — 20736 = 144² = 12⁴

  WHAT'S MISSING:
    ✗ bridge_288()  — flat_key → (face, direction, cell_pos)
    ✗ frame_seek_1728 — enc(0..1727) → extended decomposition
    ✗ decagram_walk — cell_pos → 288-cell position traversal

  THE EXACT MISSING STEP:
    bridge_288(flat_key):
      face      = flat_key / 1728
      direction = (flat_key / 288) % 6
      cell_pos  = flat_key % 288

  WHY IT'S MISSING:
    The current pipeline uses flat_key % 1440, which maps
    20736 → 1440 (lossy, 14.4x compression). The 288-cell
    architecture needs 1728 positions per face (288 × 6),
    but frame_seek only provides 120 slots per face.

    The gap: 1440 = 288 × 5, but 1728 = 288 × 6.
    The 6th direction is geometrically real but not in the
    current address decomposition.

  GEOMETRIC MEANING:
    - 288 = GEO_BLOCK_BOUNDARY = half of GEO_SLOTS (576)
    - 6 directions = 6-fold symmetry of 12-gon
    - 12 faces = dodecahedron
    - 288 × 6 × 12 = 20736 = GEO_FULL
    - stride-37 works on ALL cycles: 288, 1440, 1728, 20736

  NEXT STEPS:
    1. Add rdh_288_bridge.h to collection/rdh/
    2. Modify rdh_capture_to_enc to use 1728 instead of 1440
    3. Extend frame_at to handle 1728-cycle encoding
    4. Verify full pipeline: data → flat_key → 288-cell → frame
""")

print("Analysis complete. File: I:/FGLS_kis/runner/explore/rdh_288_bridge.py")
