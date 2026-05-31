# HANDOFF — Frustum / Trit Pipeline (FrustumSlot64 + DiamondBlock)
_Session: 2026-05-07_

---

## Conceptual Role — Frustum ในระบบนี้ไม่ใช่ 3D Culling

Frustum ทำงานเป็น **address decomposition pipeline** และ
**seam geometry สำหรับ hierarchical tiling** — ไม่เกี่ยวกับ
viewport culling เลย

```
addr + value
  → trit   = (addr ^ value) % 27     [3³ geometric address, 0..26]
  → coset  = trit / 3                [GiantCube zone, 0..8]
  → face   = trit % 6                [FrustumSlot64 direction, 0..5]
  → level  = trit % 4                [core[level] depth, 0..3]
  → letter = addr % 26               [LetterPair A..Z]
  → slope  = fibo_seed ^ addr        [apex fingerprint]

  write  → FrustumSlot64.core[level] = merkle_root
  flush  → gcfs_serialize → 4896B file
  delete → reserved_mask bit → coset silenced
```

---

## Trit Encoding — One Value, Three Roles

Single trit (0..26) encodes 3 structural roles simultaneously:

```
trit=0..26:
  coset = trit//3  → 9 zones   (3² GiantCube, 3×3×3)
  face  = trit%6   → 6 dirs    (cube faces)
  level = trit%4   → 4 depths  (2² core)

Pattern cycle: lcm(3,6,4) = 12 → repeats every 12 trits
27 = 2×12 + 3  → two full cycles + one extra coset

Key insight: storing trit alone recovers all three roles
→ no separate fields needed, zero redundancy
```

### Overlap Table (first 12)
```
trit  coset  face  level
  0     0     0     0
  1     0     1     1
  2     0     2     2
  3     1     3     3
  4     1     4     0   ← level wraps
  5     1     5     1
  6     2     0     2   ← face wraps
  7     2     1     3
  8     2     2     0
  9     3     3     1
 10     3     4     2
 11     3     5     3
 [12..23 = repeat pattern, coset continues]
```

---

## DiamondBlock 64 — Binary-to-Ternary Bridge

```
DiamondBlock = 64B = 2⁶
Gear mesh    = 54  = 2×3³  (largest 2×3ⁿ that fits)
Product      = 64 × 54 = 3456 = GEO_FULL_N ✓

Structural meaning:
  54 DiamondBlocks tile entire Lv1 address space exactly
  Each block covers 3456/54 = 64 addresses  (= block size, self-similar)
  block_index = addr // 64
  block_offset = addr % 64

Why 54 is the gear mesh:
  54 = 27 × 2 = trit_space × polarity
  54 = 6 × 9  = cube_faces × giant_cube_cosets
  → DiamondBlock naturally aligns with both trit and face decomposition
```

**Status: defined, NOT YET WIRED into dispatch or trit pipeline**

---

## FrustumSlot64 — File Structure

```
4896B = 2⁵ × 3² × 17

Decomposition:
  3456B = DiamondBlock × 54    (sacred data zone)
  1440B = 4896 - 3456          (metadata + header zone)
  1440  = 2⁵ × 3² × 5         ← has factor 5, outside sacred family
                                  intentional boundary marker

Why 17 in file size:
  17 ∈ FACE_PRIME = {7,11,13,17,19,23}  (Ghost Layer sacred primes)
  4896 = 288 × 17
  → file boundary cannot be brute-forced from pure 2ⁿ×3ᵐ arithmetic
  → security via number theory, not encryption

File layout (proposed):
  [0    .. 3455] = DiamondBlock data zone (3456B = 54 blocks × 64B)
  [3456 .. 4895] = metadata zone (1440B)
    includes: coset_mask(9b), letter_map(26B), slope_fingerprint, merkle_root
```

---

## slope = fibo_seed ^ addr — Apex Fingerprint

```c
slope = fibo_seed ^ addr

Properties:
  slope_A XOR slope_B = addr_A XOR addr_B
  → collision detection: slope equality = same fibo-space position
  → no hash table needed, O(1) comparison
  → "distance from fibo origin" not raw address

Security property:
  without knowing fibo_seed → slope leaks nothing about addr
  with fibo_seed → full addr recovery in 1 XOR op
```

---

## Frustum as Lv1→Lv2 Seam

Frustum ใน context ระบบนี้คือ **interface ระหว่าง scale levels**
ไม่ใช่ geometric culling:

```
Goldberg face (hexagon) → ผ่าครึ่ง → trapezoid × 2
trapezoid = frustum cross-section = seam thickness

Lv1 unit = 54  (gear mesh, base transition)
Lv2 scale = 162/54 = 3×  (base-3 drives scaling)

seam thickness = 27648 / 6912 = 4 Lv1-units = 2² (pure binary)

Scaling chain:
  54 slots → ×3 → 162 vertices (icosphere)
  no remainder at any step
```

### Why hexagon splits to trapezoid cleanly
```
hexagon: 6 edges → 6/2 = 3 pairs per half
trapezoid pairs × 4 seam-units = 12 = META_FACES
→ Lv2 seam count = Lv1 face count (self-similar at scale)
```

---

## Convergence Numbers — Full Map

```
       54          128         162
    (gear mesh)  (DiamondBlk) (icosphere)
        │            │            │
        ×64          ×162         ×128
        ↓            ↓            ↓
      3456         20736        20736
   (GEO_FULL_N)   (144²)       (144²)
        │            │
        ×2           │
        ↓            │
      6912 ──────────┤
   (junction)        │
        │            │
        └────────────┤ sum
                     ↓
                   27648  = 3456 × 8
               (Lv2 ceiling)

Clean integer ratios:
  27648 / 3456 = 8
  27648 / 6912 = 4
  20736 / 6912 = 3
  √(128 × 162) = 144  (exact)
```

---

## What Needs to Be Built

### F1: `frustum_trit.h` — Trit Decomposition Engine
```c
typedef struct {
    uint8_t  trit;    // 0..26
    uint8_t  coset;   // 0..8
    uint8_t  face;    // 0..5
    uint8_t  level;   // 0..3
    uint8_t  letter;  // 0..25 (A..Z)
    uint64_t slope;   // fibo_seed ^ addr
} TritAddr;

TritAddr trit_decompose(uint64_t addr, uint32_t value, uint64_t fibo_seed);
```

### F2: `frustum_slot64.h` — DiamondBlock Storage
```c
typedef struct {
    uint32_t core[4];         // level 0..3, each = merkle_root
    uint8_t  reserved_mask;   // coset silence bitmap (9 bits → uint16?)
} FrustumSlot64;              // target: fits in 64B DiamondBlock

// 54 slots × 64B = 3456B = GEO_FULL_N ✓
FrustumSlot64 store[54];
```

### F3: `frustum_gcfs.h` — Serialize to 4896B
```c
int gcfs_serialize(const FrustumSlot64 store[54],
                   const uint8_t letter_map[26],
                   uint64_t slope_fingerprint,
                   uint8_t out[4896]);
```

### F4: Wire into dispatch
```
dispatch addr+value
  → trit_decompose()
  → frustum_slot64 write
  → periodic gcfs_serialize flush
```

---

## Constants — Frustum Domain

```c
TRIT_MOD        = 27    // 3³
COSET_COUNT     = 9     // 3²
FACE_COUNT      = 6     // cube faces
LEVEL_COUNT     = 4     // 2²
LETTER_COUNT    = 26    // A..Z
DIAMOND_BLOCK   = 64    // 2⁶ bytes
GEAR_MESH       = 54    // 2×3³ blocks per Lv1 space
GCFS_FILE_SIZE  = 4896  // 2⁵×3²×17 bytes
// fibo_seed: derive from POGLS PHI constants (PHI_UP=1,696,631)
```

---

## Dependencies

```
frustum_trit.h     → standalone, no deps
frustum_slot64.h   → depends on frustum_trit.h
frustum_gcfs.h     → depends on frustum_slot64.h
wire to dispatch   → depends on tgw_dispatch_v2.h + frustum_slot64.h
```

No dependency on geo_metatron_route.h or cardioid —
frustum pipeline เป็น orthogonal layer ที่ทำงานคู่ขนาน
