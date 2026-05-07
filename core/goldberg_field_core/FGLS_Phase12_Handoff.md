# FGLS Phase 12 Handoff — Goldberg Scanner Stack
> Status: SEALED ✅
> Builds on: Phase 6–7 (TRing), Phase 11 (GPU kernel)

---

## Files Delivered

| File | Role | Tests |
|---|---|---|
| `bake_goldberg.py` | GP(1,1) geometry baker — run once at build | — |
| `geo_goldberg_lut.h` | AUTO-GEN — pentagon pairs + trigap adjacency | bake verify ✅ |
| `geo_goldberg_scan.h` | Spatial scanner — XOR interference detector | 22/22 ✅ |
| `geo_goldberg_tring_bridge.h` | Spatiotemporal fingerprint bridge | 34/34 ✅ |
| `pogls_goldberg_bridge.h` | `pgb_write()` — replaces twin_bridge_write | 25/25 ✅ |
| `geo_goldberg_tracker.h` | Passive blueprint tracker | 23/23 ✅ |

**Total: 104/104 tests passing**

---

## Architecture — Full Stack

```
Data warps in
    │
    ▼
pgb_write(addr, value)          ← single entry point (pogls_goldberg_bridge.h)
    │
    ├─ addr % 32 → face_id
    ├─ gb_face_write()           ← spatial interference
    ├─ gbt_capture()             ← spatiotemporal fingerprint
    │       ├─ gb_scan()         → spatial: 32 diffs, 60 tensions, 6 circuits
    │       └─ tring_tick()      → temporal: walk pos 0..719
    │
    ├─ on_record(fp, tracker)    ← passive tracker callback
    │       └─ gbt_tracker_record() accumulates 144 fingerprints
    │               └─ at 144 → GBBlueprint extracted (structural shape)
    │
    └─ op_count 144 → gbt_flush() + FIBO_EV_FLUSH
```

---

## GP(1,1) Goldberg Topology (Locked)

```
12 pentagons  = warp gateways (fixed — 6 bipolar pairs)
20 hexagons   = buffer/routing faces
60 tri-gaps   = tetra node connectors (= I-symmetry order 60)
32 frustum rings total

6 pentagon pairs = GEO_SPOKES = 6 independent circuits
  pair[p] = {positive_face, negative_face}  ← from GB_PEN_PAIRS LUT
  triangle gaps: GB_TRIGAP_ADJ[60][3]       ← from bake_goldberg.py
```

---

## GBTRingPrint — Spatiotemporal Fingerprint

```c
typedef struct {
    uint32_t  spatial[32];     // XOR diff per face
    uint32_t  max_tension;     // highest tetra node tension
    uint8_t   max_tension_gap; // gap_id [0..59]
    uint8_t   active_pair;     // bipolar pair [0..5]
    uint8_t   active_pole;     // GB_POLE_POSITIVE or NEGATIVE
    uint16_t  tring_pos;       // temporal walk pos [0..719]
    uint32_t  tring_enc;       // tuple encoding
    uint16_t  tring_pair_pos;  // chiral partner pos
    uint32_t  stamp;           // spatial_fold ^ tring_pos ^ tension
    uint64_t  scan_id;
} GBTRingPrint;
```

---

## GBBlueprint — Flush Window Structural Record

```c
typedef struct {
    uint32_t  spatial_xor;          // XOR fold of all face diffs in window
    uint8_t   top_gaps[5];          // gap_ids with highest accumulated tension
    uint32_t  top_tensions[5];
    uint8_t   circuit_fired;        // bitmask: bit[p]=1 if pair p fired
    uint16_t  tring_start/end/span; // temporal range covered
    uint32_t  stamp_hash;           // XOR chain of all stamps
    uint32_t  event_count;          // = 144 (flush boundary)
    uint64_t  window_id;            // monotonic
} GBBlueprint;
```

---

## Sacred Numbers — All Preserved

```
144  = FIBO_PERIOD_FLUSH = flush_period = events per blueprint window
720  = TRing cycle (6! = 12 compounds × 60 tuples)
3456 = GB_ADDR_BIPOLAR = GEO_FULL_N = bipolar address space
1728 = GB_ADDR_UNIPOLAR = 144 × 12
60   = GB_N_TRIGAP = I-symmetry group order = tetra nodes
12   = GB_N_PENTAGON = N_COMP = dodecahedron faces
6    = GB_N_BIPOLAR_PAIRS = GEO_SPOKES = pentagon opposite pairs
```

---

## Role Separation (DO NOT CONFLATE)

| Component | Role |
|---|---|
| `geo_goldberg_scan.h` | SPATIAL — how data bent the net |
| `geo_temporal_ring.h` | TEMPORAL — where in clock cycle (Phase 7) |
| `geo_goldberg_tring_bridge.h` | COMBINED — spatiotemporal fingerprint |
| `geo_goldberg_tracker.h` | PASSIVE — records blueprint, then forgets |

---

## How to Use

```c
// 1. Init
GBTracker tracker;
PoglsGoldbergBridge bridge;
gbt_tracker_init(&tracker);
pgb_init(&bridge, gbt_tracker_record, &tracker);

// 2. Write loop (replaces twin_bridge_write)
int flush_ev;
GBTRingPrint fp = pgb_write(&bridge, addr, value, &flush_ev);

// 3. Check for blueprint
if (gbt_tracker_ready(&tracker)) {
    GBBlueprint bp = gbt_tracker_extract(&tracker);
    // store bp.stamp_hash, bp.spatial_xor, etc.
    // → write to storage here
    // → then done — no content kept
}
```

---

## Build Order

```bash
# Step 1: bake geometry (once)
python3 bake_goldberg.py   # → geo_goldberg_lut.h

# Step 2: compile
gcc -O2 -I. test_tracker.c -o test_tracker && ./test_tracker
# ALL PASS ✓ — 0 test(s) failed

# Include chain:
# geo_goldberg_tracker.h
#   → pogls_goldberg_bridge.h
#     → geo_goldberg_tring_bridge.h
#       → geo_goldberg_scan.h
#         → geo_goldberg_lut.h
#       → geo_temporal_ring.h
#         → geo_temporal_lut.h  (Phase 7 — AUTO-GEN)
```

---

## What Remains

| Task | Priority |
|---|---|
| Wire `pgb_write` into actual POGLS pipeline (`pipeline_wire_process`) | P1 |
| `dodeca_insert` trigger from `GBBlueprint` on flush | P1 |
| `pgb_twin_raw` → feed `geo_fast_intersect` in Twin Geo | P1 |
| AVX2 `geo_fast_intersect_x4` via `__m256i` (was next from S21) | P2 |
| GPU kernel: wire `GBTRingPrint` into `geo_cube_dispatch` | P2 |

---

## Open Notes

- `bake_goldberg.py` requires `networkx` (system package or venv)
- `geo_goldberg_lut.h` is AUTO-GEN — commit the `.py`, not just the `.h`
- TRing head never resets across flush — temporal is continuous by design
- `pgb_twin_raw(addr, value, stamp_frozen)` = `addr ^ value ^ stamp` — stamp must be captured BEFORE tick (c144 freeze rule preserved)

---

*Sealed: Claude Sonnet 4.6 | FGLS Phase 12 — Goldberg Scanner Stack Complete*
