# Priority Zone — 60 Trapezoid Towers

## Architecture

```
icosa capture (300 keys at f=4)
    ↓ geometric nearest-edge (distance to edge midpoint)
60 trapezoid towers (12 faces × 5 edges)
    ↓ each tower = GEO_BLOCK (48) × GEO_TOWER (144) = 6912 nodes
    ↓ total = 60 × 6912 = 414,720 = 20 × GEO_FULL
    ↓
┌──────────────────────────────────────────────────┐
│  PRIORITY ZONE  (414,720 slots)                  │
│  ┌──────────────┐  ┌──────────────┐              │
│  │ trap 0       │  │ trap 1       │  ...  trap 59│
│  │ 6912 slots   │  │ 6912 slots   │              │
│  └──────┬───────┘  └──────┬───────┘              │
│         │                 │                       │
│         ▼                 ▼                       │
│    shell level 0..11                          │
│    (cold bottom → hot surface)               │
└──────────────────────────────────────────────────┘
    │ capo_key = 0..11  (rotate shell-level assignment)
    │
    ▼
12 different GEO_FULL mappings (20736 slots each)
   ┌─ capo=0: level 0=cold, 1..11=hot (default)
   ├─ capo=1: level 11=cold, 0..10=hot
   ├─ ...
   └─ capo=11: level 1=cold, 0,2..11=hot
```

## Three Redundancy Tiers

### Tier 1 — Capo Rotation (12 views)
Zero data movement — same `(trap, pos)` but different `capo_key` → different GEO_FULL destination.
Equivalent to rehashing without touching data.

### Tier 2 — Fast-Flip Pair (30 pairs)
Each dodeca edge has 2 trapezoids (one per adjacent face). Complete bidirectional pairs proved.
- `icosphere_trap_pair(trap)` → mirrored trap on other face
- `icosphere_trap_fast_flip(trap, pos)` → same tower position, mirrored GEO_FULL address
- RAID-1 style: write to both → read from primary, flip to mirror on corruption

### Tier 3 — Cold Zone (pit bottom)
12 mini pentagons at shell level 0 — 1728 slots. Eviction target when all 360 strategies (30 pairs × 12 capo) still collide.

## Constants

| Name | Value | Meaning |
|---|---|---|
| GEO_BLOCK | 48 | 4×4×3 metatron cells |
| GEO_TOWER | 144 | one pentagon shell level |
| GEO_FULL | 20736 | full globe = 12×1728 |
| GF_PRIORITY_TRAPEZOIDS | 60 | 12 faces × 5 edges |
| GF_PRIORITY_TOWER_NODES | 6912 | 48 × 144 |
| GF_PRIORITY_TOTAL | 414720 | 60 × 6912 |
| GF_PRIORITY_GEO_FULL_RATIO | 20 | 414720 / 20736 |

## Key Functions

### Mapping
- `icosphere_capture_key_to_trapezoid(key)` → 0..59 (geometric nearest-edge)
- `icosphere_capture_key_to_tower_pos(key)` → 0..6911 (barycentric weight → level + intra)
- `icosphere_trapezoid_to_geo_full(trap, pos)` → 0..20735

### Cold Zone
- `icosphere_trapezoid_to_cold(trap, pos)` → shell level 0 GEO_FULL node

### Capo
- `icosphere_trapezoid_to_geo_full_capo(trap, pos, capo_key)` → rotated view
- `icosphere_cold_for_capo(trap, pos, capo_key)` → cold target under capo

### Redundancy
- `icosphere_trap_pair(trap)` → mirrored trap on adjacent face
- `icosphere_trap_fast_flip(trap, pos)` → same pos, mirrored face

## ADJ Table
One entry fixed (July 2, 2026): `ADJ[1][4]={10,3}` → `{10,2}`. Face 1 edge 4 = vertices (19,6) = face 10 edge 2 = vertices (6,19). Bidirectional verification: 60/60 OK after fix.

## Performance (300 keys, f=4)
| Metric | Before fix | After fix |
|---|---|---|
| Shell levels used | 3 (0,2,5) | 11 (near-uniform) |
| GEO_FULL collisions | 593 | 75 |
| Cold zone collisions | 1393 | 80 |

## Limitations
- At f=4, barycentric weights produce only 4 distinct combinations per face → shell/intra formulas use `capture_key` directly for uniform spread. Full geometric depth mapping requires f=8+ (900+ keys).
- 4 empty trapezoids (out of 60) — inherent to geometric nearest-edge mapping at this resolution.
- 1,382 slots per key — designed for f=8 (900 keys) and f=16 (5,460 keys).

## Files
- `collection/geo_jump_module/include/geo_field_icosphere.h` — full implementation
- `collection/geo_jump_module/test_priority_zone.c` — 8-section test, all pass
- `collection/geo_jump_module/include/geo_hidden_pocket.h` — ADJ table + trapezoid geometry
