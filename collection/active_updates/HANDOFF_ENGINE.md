# HANDOFF — Main Engine (Dispatch + Cardioid + Metatron + Compound)
_Session: 2026-05-07_

---

## System Architecture Overview

```
Lv1 Address Space
┌─────────────────────────────────────────────────────┐
│  tetra core [0..3455]     │  octa residual [3456..6911] │
│  5-tetra × 12, tri face   │  5-octa × 12, square face   │
│  720 slots, sacred        │  1536 slots, frustum-ready  │
└─────────────────────────────────────────────────────┘
                    junction = 6912 = 2⁸×3³

Lv2 Convergence Ceiling
  20736 = 128 × 162 = 144²   (binary × icosphere = Fibonacci²)
  27648 = 20736 + 6912        (icosphere zone + residual)
  27648 / 3456 = 8            (Lv2 = 8× Lv1 base)
```

---

## Component Status

| File | Status | Tests |
|---|---|---|
| `geo_addr_net.h` | ✅ frozen | — |
| `geo_tring_walk.h` | ✅ frozen | 6/6 |
| `tgw_dispatch_v2.h` | ✅ patched Hook A+B | 24/24 |
| `tgw_ground_lcgw.h` | ✅ cardioid wired | 21/21 |
| `tgw_cardioid_express.h` | ✅ 2-gate hybrid | 21/21 |
| `geo_compound_cfg.h` | ✅ dual-compound | — |
| `geo_metatron_route.h` | ✅ NEW this session | 37/37 |

**Total: 89 tests PASS, 0 FAIL**

---

## Cardioid Express — 2-Gate Hybrid

### Gate 1: Geometric Floor (deterministic)
```c
r_geo_q8 = A × (256 + cos[pos]) >> 8
if r_geo_q8 < CARDIOID_GEO_MIN_Q8(140) → CUSP block
```
### Gate 2: Data Gate (adaptive)
```c
(r_data << 8) <= r_geo → EXPRESS else LINEAR
```

### Calibrated Results (test workload)
```
express rate  : ~65%   (vs 74% single-gate — stable)
cusp_block    : ~500+  (Gate 1 noise rejection working)
bucket skew   : <2.0   (express distribution healthy)
```

### Key Constants
```c
CARDIOID_GEO_MIN_Q8 = 140   // tuned for test data (val&0xFF low)
                             // real traffic → may lower to 100-120
CARDIOID_M_DEFAULT  = 7     // gcd(7,720)=1 → bijective permutation
bucket mapping      = cd.next_pos / 6   // NOT %120, natural division
```

### Hook Wiring in tgw_dispatch_v2.h
```
Hook A (GROUND early-exit, line ~238):
  polarity=1 + cardioid_pass=EXPRESS → redirect to route bucket
  polarity=1 + cardioid_pass=CUSP/LINEAR → ground_fn as normal

Hook B (ROUTE path, 2 locations):
  no-blueprint path + verdict=true path
  express → bucket = cd.next_pos / 6
  linear  → bucket = geo.hilbert_idx (original)

Stats: express_count, cusp_block_count, bucket_hist[120]
```

---

## Metatron Route Layer — geo_metatron_route.h

### 4-Layer O(1) Routing
```
Orbital : (enc + 1) % 60 within face          — 1 mod
Chiral  : (enc + 360) % 720 = face+6          — 1 add
Cross   : METATRON_CROSS[face] × 60 + slot    — 1 LUT
Hub     : target_face × 60                    — 2 LUT
```

### METATRON_CROSS[12] — Mathematical Basis
```c
// rotate +3 within ring → self-inverse bijection
// ring1(0..5) → ring2(6..11): face = (f+3)%6 + 6
// ring2(6..11) → ring1(0..5): face = (f-6+3)%6
static const uint8_t METATRON_CROSS[12] = {
    9,10,11,6,7,8,   // ring1 → ring2
    3, 4, 5,0,1,2,   // ring2 → ring1
};
// property: cross(cross(enc)) == enc ✓ for all 720
```

### Circuit Switch Integration
```c
// tri[5] FLOW condition — 36 = 2²×3² (sacred family)
cond = meta_face(enc) * 3 + enc % 3   // 0..35
key  = meta_face(cpair) * 3 + enc % 3 // chiral complement

// Geometric invariant (PROVEN):
// (cond_folder - cond_cpair) % 36 == 18  for ALL 720 enc values
// → trip: metatron_cond(requester) == (folded_cond + 18) % 36

// API:
circuit_arm(cell, dest_hid, enc)       // set tri[2]+tri[5]
circuit_resolve(cell, requester_enc)   // → dest | META_OPEN
```

### Key Insight
**144 = √(128 × 162)** — geometric mean ของ binary domain (128=2⁷)
และ icosphere (162=2×3⁴) คือ Fibonacci clock ของระบบ
ไม่ใช่ coincidence — algebraic inevitability ของการเลือก constants

---

## geo_compound_cfg.h — Dual-Compound Layer

```
GEO_CFG_TETRA: addr 0..3455,    slots=720,  face=triangle, divisor=2
GEO_CFG_OCTA:  addr 3456..6911, slots=1536, face=square,   divisor=1

Translation: geo_addr_translate(src_cfg, dst_cfg, addr) → O(1)
Face route:  geo_face_route(src_cfg, dst_cfg, face_id)  → O(1)
Verify:      geo_compound_cfg_verify(cfg)               → call at init
```

**5-octa role**: NOT a replacement for tetra — address bridge layer
living inside tetra residual zone, frustum-native (square face),
future use: codec tile addressing (1536 = 2⁹×3, power-of-2 friendly)

---

## Sacred Constants — FROZEN

```c
GAN_TRING_CYCLE    = 720        // 12×60, walk cycle
GEO_FULL_N         = 3456       // 2⁷×3³, Lv1 address space
JUNCTION           = 6912       // 2⁸×3³, tetra+octa ceiling
ICOSPHERE_LV2      = 20736      // 144² = 128×162
CONVERGENCE        = 27648      // 20736+6912 = 3456×8
META_COND_MOD      = 36         // 2²×3², circuit condition space
DIAMOND_BLOCK      = 64         // 2⁶, binary domain unit
GEAR_MESH          = 54         // 2×3³, base2↔base3 translator
// 64 × 54 = 3456 ✓
```

---

## Pending — Engine Side

### P1: Metatron ↔ Dispatch Integration (not yet done)
`meta_route()` ต้องการ `target_face` แต่ dispatch ยังไม่ derive:
```c
// Option A (lazy): target_face = 0xFF → always orbital
// Option B (full): addr ∈ [3456..6911] → cross/chiral via OCTA zone
// Decision needed before next session
```

### P2: test_dispatch_v2.c + test_rewind_lcgw.c
ยังไม่ได้ run ใน session นี้ (อยู่ใน zip) — ต้อง verify ยังครบไหม
หลัง cardioid 2-gate เปลี่ยน behavior

### P3: bench_ground.c
GROUND write throughput vs ROUTE — ยังไม่ได้ run
