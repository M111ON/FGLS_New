# Fishbone + Ribcage — Architecture Handbook

### ระบบ DATA ที่ไม่มี DATA — Measurement System, ไม่ใช่ Storage Engine

**Date:** 2026-08-01
**Language:** ไทย + English

---

## Core Principle

```
MAP ไม่ใช่ COMPRESS — คิดจะบีบ = ผิดทางทันที
นี้คือระบบวัด ไม่ใช่ระบบเก็บ
```

---

## Layer Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                  LAYER 0: FISHBONE (KIS-SEAL)                   │
│  Structure ก่อนจักรวาล — ไม่ได้สร้าง, มันมีอยู่แล้ว               │
│                                                                │
│  Icosa (n=0)  ←kis/seal→  Dodeca (n=1)  ←kis/seal→  Icosa (n=2) → ...
│       R=1.0                   R=0.382                   R=1.382
│                                                                │
│  ratio คงที่ทุกชั้น = 1/φ² = 0.381966                        │
│  h_seal = 0.882113 (คงที่ทั้ง 12 vertex, error 1e-16)       │
│  R_n = R_0(1+α)^n — bijective, invertible, no origin      │
│                                                                │
│  Section: .hermes/desktop-attachments/Icosa-Dodeca_Recursive_∗  │
│  Code:   none — pure geometry                             │
├──────────────────────────────────────────────────────────────┤
│                  LAYER 1: SKELETON (ADDR → GEOMETRY)           │
│ Skeleton — ไม่ใช่ data, ไม่ใช่ structure — คือ BRIDGE             │
│                                                                │
│ skeleton_lookup(addr) → O(1), 7 ops, 0 branch               │
│   zone = 12 pentagon sectors (dodecahedron routing)          │
│   pair = 6 bipolar channels (cube face mapping)              │
│   pole = CHIRAL/CROSS (Metatron)                             │
│   enc  = 0..719 walk (stride-37, bijective)                  │
│                                                                │
│ skel_decide(chunk) → P0→P5 short-circuit:                    │
│   IDENTITY (1B) → RAW (64B) → FLAT (1B) → DIFF (10+nB)     │
│   → BREF (2B) → GEOM (~20B) → RAW fallback (64B)           │
│                                                                │
│ Files: core/skeleton_index.h (canonical, 227 lines)          │
│   Also: collection/reshape_nbond/, dgls/, geopixel/, colab/  │
│   Python: tools/geopixel_pipeline.py                         │
│   CUDA: skel_decide_kernel in pipeline                       │
│   Tests: test_skel.py, test_full.py, src/pipeline.c          │
├──────────────────────────────────────────────────────────────┤
│                  LAYER 2: RIBCAGE (CUBE DATA STORE)            │
│ 数据 — สิ่งที่เกาะตามซี่โครง                               │
│                                              จาก          │
│  Cube 10×10×10 × 6 faces (A,B,C,D,E,F) → contour mask         │
│  Cube 10×10×10 × 12 faces (dodecahedron) → 12-FACE option     │
│                                                                │
│  Displacement model: weight → position from 0, lossless       │
│  XOR(pos, 0) = pos = weight — O(1) decode                     │
│  Pins = visible through mask holes — contour = shape          │
│                                                                │
│  Files: runner/explore/contour_mask_v2.c, contour_mask.c      │
│         runner/explore/silk_screen_encoder.c                 │
│         runner/explore/test_opposite_cancel.py              │
├──────────────────────────────────────────────────────────────┤
│                  LAYER 3: POINTERS (ACCESS PATTERNS)           │
│ ตัวชี้ — f(time, rib_id) → cube weight                        │
│                                                                │
│  Access strategies:                                        │
│    linear_step     → rib0→rib1→... sequential pass-by          │
│    angular_rotation → cosine projection angle→rib ID         │
│    direct_array    → O(1) precomputed LUT (fastest)         │
│    cosine_computed → compute cos on-the-fly                 │
│    rotational_forecast → tide curve + subpair recursive       │
│                                                                │
│  Files: experiments/rib_cube_access.py                        │
├──────────────────────────────────────────────────────────────┤
│                  LAYER 4: RESIDUAL (SHADOW SPACE)              │
│ เงา — ช่องว่างที่ไม่ว่างเปล่า                                         │
│                                                                │
│  GAP = Icosa(R=1.0) - Dodeca(R=0.382) = 0.618 = 1/φ         │
│  บน crush: Bermuda shadow ring (capacity=144)                 │
│  Bermuda: HOT/COLD classify → route (used) or shadow (ring)    │
│  Shadow Zone: no clock, no aging, no timeline                    │
│                                                                │
│  Files: collection/bermuda_shadow.h, collection/shadow_zone.h│
│         collection/include/p5h_ribcage.h                     │
│         collection/bermuda_export.h                           │
│         collection/geom_shadow_pipe.h                         │
└──────────────────────────────────────────────────────────────┘
```

---

## Fishbone — The Structure Before Universe (Kis-Seal)

```
  O = Icosahedron เริ่มต้น (n=0)
  D = Dodecahedron (n=1) — derived from ico face normals
  O = Icosahedron (n=2) — derived from dodeca face normals
  ... infinite

  scale ratio: R_n+1 / R_n = 1/φ² ≈ 0.381966
  bijective: n = log(R_n) / log(1+α) — invertible with error < 1e-16

  What Seal means:
    "vertex เดิมหายไปเมื่อ spike ปิด" — shape transitions มี deterministic
    เกิดทุก vertex พร้อมกัน เพราะสมมาตรเต็ม icosahedral group (order 60)
```

**Pitfall:** Hardcoded coordinates from different conventions = 40% correct, 60% wrong silently. ต้อง derive dual จาก face-normal โดยตรง

## RibCage — The Cube Data Stores (Weight Cube)

```
  Each *rib* position = cube weight measurement:

  10×10×10 grid × 6 faces = 6,000 unit cells (per rib)
  10×10×10 × 12 faces = 12,000 cells (per rib)

  720 ribs in new projection = ±rib_count → 6000 × 720 = 4.3M cells

  4.3M อย่างละ independent reading via displacement:

    measurement(face, x, y, z) → DISTANCE_X from (0,0,0)
    XOR(resulting_positions) = weight → lossless forever
```

**Does NOT store weights directly — stores displacement ship position:**

```
The cube doesn't store 0x45 — it stores (e.g.) position 45 inside 3D space
  Data = "where" ≠ "what"
  Question → position → answer (compute, no lookup)
```

## Pointers — Access from Anywhere

```
Access formula: pointer = f(time, rib_id)

    time = n (free) × interpretation (step/frame/cycle/layer)
       n = 42, mode=STEP → position within 12-tick cycle = 6
       n = 42, mode=FRAME → frame at [42/12] % 1728 = 3
       n = 42, mode=FULL → absolute position = 42 (in 0..20735)

    rib_id = which angle/slice/direction + sublocality within rib
    → linear scan: rib0→rib1→...
→ real-time: angular ⊂ sphere → cosine → rib_id*
→ precomputed: direct_array[hash] O(1) reads for lookup
```

**Access System Built On:**
- `beam_addressing/beam_codec_*.c` — 1.22B ops/sec, weight codec
- `collection/geo_frame_seek.h` — O(1) rib index (enc n ×37 %1440) → DualFrame (12 edges × face/slot/phase)

---

## Lineage — From Existing Systems (genes)

```
DRamTile  = rib storage + hold the cube weights (mmap, page-locked)
GearShift = rib streaming scheduler (IDLE→STREAMING→DONE)
GeoFrameSeek = threading knob → time → coordinate
FiboSpine    = 1728 pipes × 12 ticks = 20736 = the main field
P5H       = pipe domain — bonus room with barrier sync
Jet Bridge    = cross-rib hopping at tick 11
```

---

## Anti-patterns — What NOT to Build

```
- outdated_compress  → turns into spiral  →  STOP (don't compress)
- copy / mirror     → normalization → only origin
- eternal origin    → no collapse 20736 rot testrot ← just read cyclic
- shape_matching    → "looks right" = danger uniform single-flow
```

---

## /// Cross-Codex List File References

| Component | Files | Tested |
|-----------|-------|--------|
| Cube capture | runner/explore/contour_mask_v2.c | ✅ 2/2 PASS |
| Encoder (silk) | runner/explore/silk_screen_encoder.c | ✅ 2/2 PASS |
| 12-face storage | runner/explore/geo_12axis_storage.c | ✅ 9/9 PASS |
| Geo frame seek | collection/geo_frame_seek.h | ✅ 384× reduction |
| Bermuda shadow | collection/bermuda_shadow.h | ✅ HOT/COLD classify |
| P5H ribcage | collection/include/p5h_ribcage.h | ✅ 10-phase window |
| GearShift | runner/gear_shift.h | ✅ stream/router |
| DRamTile | runner/dramtile_store.h | ✅ mmap zero-copy |
| Gen free unit | experiments/gen_free_unit.py | ✅ 23/23 PASS |
| Access methods | experiments/rib_cube_access.py | ✅ 5 strategies |
| Kis-Seal proof | .hermes/desktop-attachments/Icosa-Dodeca_Recursive_* | ✅ 1e-16 error |

---

## Summary — Ground Truths

1. **MAP not COMPRESS** — คิดจะบีบ = ผิดทาง
2. **Gen(n) = free** — เหมือน cm, mm; ไม่ผูกกับหมายความใด
3. **h_seal = fixed** — difference = ก๊าp = shadow/version space
4. **Cube weights = displacements** — not in storage, only measured on demand
5. **FiboSpine = ribs** — P5H = cross-rib access; GearShift = routing
5. 🔴 **Silent drift** — check when 2D↔3D swaps: shape similar but wrong
   — always validate roundtrip data: go out ↔ come back in same spot
6. Ground priority: rib → rib ID direct → skip compute when unnecessary

---

*Master architecture reference — version 1.0 — 2026-08-01*