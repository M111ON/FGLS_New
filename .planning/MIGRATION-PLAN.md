# Migration Plan: 12-face/1440 TRing → Y-triangle node_id (20736)

## Overview
Complete the migration from the old `tring_pos` (0..1439) / 12-face rotation system
to the new Y-triangle `node_id` (0..20735) / capo routing system.

The core architecture layer is already migrated (geo_compound_cfg, tw_face_bridge main API,
capture_pipeline, zone_card_sid). This plan covers the remaining SID-dependent files.

## Dependency Order

Migration must proceed bottom-up to avoid broken intermediate states:

```
Phase 1: tri_hex_tess.h          (THCoord — foundation struct)
Phase 2: sid.h                    (main bottleneck — SIDCoord, capture, summon)
Phase 3: tw_bridge.h             (TWBridgeResult — remove tring_pos field)
Phase 4: tw_rewind_bridge.h      (bridge 1440 → 20736)
Phase 5: runner/sid_cache.h      (SID_TRING_SLOTS 1440→20736, TWFaceRewindSid→node_id)
Phase 6: runner/th_grid.h        (THCoord consumer)
Phase 7: runner/goldberg_sid.h   (THCoord consumer)
Phase 8: collection/goldberg_sid.h (THCoord consumer)
Phase 9: Legacy section in tw_face_bridge.h (lines 372-608) — REMOVE
Phase 10: Test files (10+ files)
Phase 11: Runner comments (llama_pogls_runner_sid.c)
```

---

## Phase 1: `tri_hex_tess.h` — THCoord struct

**Current:**
```c
typedef struct {
    uint8_t  face;        /* 0..11 */
    uint16_t tring_pos;   /* 0..1439 = face*120 + is_tri*60 + sector*6 + slot */
} THCoord;               /* 3B total */
```

**New:**
```c
typedef struct {
    uint32_t node_id;     /* 0..20735 Y-triangle node_id */
} THCoord;               /* 4B total */
```

**Functions to update:**
- `th_local(tring_pos)` → removed (node_id encodes face natively)
- `th_unpack(local, ...)` → replaced with `geo_pentagon_id(node_id)`, `geo_shell_level(node_id)`, `geo_clock_tick(node_id)` decomposition
- `th_snap(face, vx, vy, g)` → `th_snap(vx, vy, g)` — uses `tw_to_node()` then capo for face spreading
- `th_steps(a, b)` → geodesic step count using `geo_pentagon_id()` difference
- `th_bond_strength(a, b)` → same algorithm, decompose node_ids
- `th_is_always_warm(norm, weight)` → same logic with node_id decomposition

**Key decision:** `THCoord` becomes 4B (just `node_id`). The face is derivable via `geo_pentagon_id(node_id) - 1`. The local position within face is derivable via `node_id % (GEO_FULL / GEO_PENTAGONS)`.

**Dependents affected:** `runner/th_grid.h`, `runner/goldberg_sid.h`, `collection/goldberg_sid.h`

---

## Phase 2: `sid.h` — Main Bottleneck

**Current SIDCoord (line 49-59):**
```c
typedef struct {
    uint8_t  face;
    uint8_t  zone;
    uint8_t  slot;
    int64_t  resid_x;
    int64_t  resid_y;
    uint16_t tring_pos;   /* 0..1439 */
    uint8_t  is_tri;
    uint8_t  drain;
    uint8_t  pad[2];
} SIDCoord;
```

**New SIDCoord:**
```c
typedef struct {
    uint32_t node_id;     /* 0..20735 Y-triangle node_id */
    int64_t  resid_x;     /* residual X (TW_SCALE units) */
    int64_t  resid_y;     /* residual Y (TW_SCALE units) */
    uint8_t  drain;       /* 1 if near sector boundary */
    uint8_t  pad[3];
} SIDCoord;
```

**Functions to update:**
- `sid_capture_legacy()` → `tw_capture_to_node()` + resid extraction
- `sid_summon_legacy()` → `geo_jump.h` node_id → position via `tw_reconstruct_int()`
- `sid_capture_with_config()` → `tw_capture_capo_all()` + pick best resid
- `sid_capture_priority()` → simplified: single capture + capo ×12
- `sid_capture_legacy_24()` → removed (redundant with capo ×12 which covers all 12 pentagons)
- `sid_verify_roundtrip_legacy()` → adapted to new SIDCoord
- `sid_verify_priority()` → adapted

**SIDTwidxEntry (file format):** Update to store `node_id` instead of `face/zone/slot/tring_pos/is_tri`.
This is a **breaking change** for .twidx files — bump `SID_TWIDX_VER` to 2.

**Old backward compat functions:** Remove `sid_capture_legacy`, `sid_summon_legacy`,
`sid_verify_roundtrip_legacy`. Replace with Y-triangle equivalents.

---

## Phase 3: `tw_bridge.h` — TWBridgeResult

**Current (line 302-314):**
```c
typedef struct {
    uint32_t       node;
    uint32_t       drain_node;
    TantrixTile    tile;
    TantrixTile    drain_tile;
    uint32_t       shell_id;
    uint8_t        ring_state;
    uint8_t        face;
    uint16_t       tring_pos;     /* REMOVE */
    uint8_t        frozen;
    uint32_t       freeze_addr;
    RingClassified ring;
} TWBridgeResult;
```

**Change:** Remove `tring_pos` field. The `node` field (0..20735) replaces it.
Update `tw_bridge()` function: remove `r.tring_pos = tw_to_tring_pos(...)`.

**Also update:**
- `tw_to_tring_pos()` — mark as deprecated or remove (callers should use `node_id` instead)

---

## Phase 4: `tw_rewind_bridge.h` — SID ↔ geo_rewind bridge

**Current:** Uses `tring_pos` (0..1439) for TWFaceRewind indexing and GEO_WALK enc mapping.

**New:** Uses `node_id` (0..20735) for TWFaceRewind indexing.

**Key changes:**
- `tw_sid_tring_to_enc()` → `tw_sid_node_to_enc(node_id)` — map node_id to GEO_WALK enc
- `tw_enc_to_sid_tring()` → `tw_enc_to_sid_node(enc)` — map enc to node_id
- `TWBRewindResult` — replace `tring_pos` references with `node_id`
- `tw_bridge_rewind_find(tw_rb, geo_rb, sid_tring)` → `tw_bridge_rewind_find(tw_rb, geo_rb, node_id)`
- `tw_cap_pack_chunk()` — pack with node_id key instead of tring-based key
- `tw_chunk_unpack_cap()` — adapt to new key format

**Note:** `TWFaceCapture` struct itself is legacy. The new system should use
`tw_node_pack_key()` / `tw_node_unpack_key()` from tw_face_bridge.h main API.

---

## Phase 5: `runner/sid_cache.h`

**Current:**
```c
#define SID_TRING_SLOTS 1440
typedef struct {
    uint64_t keys[SID_TRING_SLOTS];
    uint32_t stored;
} TWFaceRewindSid;
```

**New:**
```c
#define SID_NODE_SLOTS  GEO_FULL  /* 20736 */
typedef struct {
    uint64_t keys[SID_NODE_SLOTS];
    uint32_t stored;
} TWFaceRewindSid;
```

**Functions to update:**
- `tw_rewind_sid_store(rb, key, tring)` → `tw_rewind_sid_store(rb, key, node_id)`
- `tw_rewind_sid_find(rb, tring)` → `tw_rewind_sid_find(rb, node_id)`
- `tw_rewind_sid_has(rb, tring)` → `tw_rewind_sid_has(rb, node_id)`
- `tw_rewind_sid_evict(rb, tring)` → `tw_rewind_sid_evict(rb, node_id)`
- `sid_cache_get_by_tring()` → `sid_cache_get_by_node()`
- `sid_cache_put()` — update `SID_PACK_KEY` and indexing to use `node_id`
- `SIDCacheEntry` — replace `tring_pos` with `node_id`

**Memory impact:** `keys` array grows from 1440×8=11.5KB to 20736×8=162KB (acceptable).

---

## Phase 6: `runner/th_grid.h`

**Current:** Uses `THCoord.tring_pos` for coordinate output.

**Changes:**
- `th_from_name()` — return `THCoord` with `node_id` instead of `tring_pos`
- `th_print_coords()` — decompose `node_id` for display (face via `geo_pentagon_id()`)
- All `tring_pos` references → `node_id`

---

## Phase 7-8: `runner/goldberg_sid.h` + `collection/goldberg_sid.h`

**Both use `THCoord.tring_pos`** for goldberg sphere mapping.

**Changes:**
- `goldberg_from_thcoord(c, ...)` — decompose `c.node_id` into face + local position
- `goldberg_geodesic(a, b)` — same algorithm, decompose node_ids
- `goldberg_discover_bonds()` — use node_id-based THCoord
- `goldberg_print_coords()` — decompose node_id for display

---

## Phase 9: Remove Legacy Section in `tw_face_bridge.h`

**Lines 372-608** — the entire LEGACY BACKWARD-COMPAT section:
- `TWFaceCapture` struct
- `tw_face_to_tring()`
- `tw_capture_int_to_face()`
- `tw_capture_face()`
- `TWFaceIter24` + `tw_iterate_faces_24()` + `tw_best_of_24()`
- `tw_face_pack_key()` / `tw_face_unpack_key()` — replaced by `tw_node_pack_key()` / `tw_node_unpack_key()`
- `tw_face_rewind_store()` — replaced by `tw_rewind_store()` with node_id key
- `_tw_centroid_to_face0()`
- `tw_tring_to_frame_enc()` / `tw_tring_to_world_b()` / `tw_tring_to_face_zone_slot()`
- All legacy constants (`TW_FACES`, `TW_FACE_SLOTS`, `TW_TRING_1440`, etc.)
- `_TW_ROT_COS`, `_TW_ROT_SIN`, `TW_FACE_PRIORITY`

**This is the payoff** — removing ~240 lines of legacy code once all dependents are migrated.

---

## Phase 10: Test Files

10+ test files use `tring_pos` / `1440`. After core migration:

| Test File | Change |
|---|---|
| `test_tw_rewind_bridge.c` | Use `node_id` instead of `tring_pos`, update constants |
| `test_tw_bridge.c` | Remove `test_tring_pos()`, update TWBridgeResult assertions |
| `test_tring_predictor.c` | Replace `tring_pos` with `node_id` |
| `test_gguf_capture.c` | Update histogram from 1440→20736 slots |
| `test_geo_summon.c` | Use `node_id` decomposition |
| `test_cross_arch_v2.c` | Replace `tring_pos` stats with `node_id` stats |
| `test_tring_walk.c` | Update loop bounds to GEO_FULL |
| `test_tgw_stream.c` | Replace `tring_pos` references |
| `test_seam2.c` | Replace `old_tring_pos()` with node_id |
| `test_p6.c` | Replace `_p4_tring_pos()` with node_id |

Many of these are in `collection/tests/`, `collection/test/`, and `collection/geopixel/` directories.
Some are in archived checkpoints — those may not need updating.

---

## Phase 11: Runner Comments

`runner/llama_pogls_runner_sid.c` line 36 — update comment from "1440-slot cache" to "20736-slot cache".

---

## GEO_WALK Mapping Decision

**GEO_WALK[720]**: walk position (0..719) → enc value. Used by RewindBuffer (972 slots).

**Old mapping:** `tring_pos (0..1439)` → `walk_pos = tring_pos % 720` → `enc = GEO_WALK[walk_pos]`

**New mapping:** `node_id (0..20735)` → `local = node_id % 1728` (position within pentagon)
→ `walk_pos = local % 720` → `enc = GEO_WALK[walk_pos]`

**Dual system is clean:**
- **TWFaceRewind** (new): 20736 slots, indexed by `node_id`, uses `tw_node_pack_key()`
- **RewindBuffer** (legacy): 972 slots, indexed by `enc`, unchanged
- **tw_rewind_bridge.h**: converts between them via `node_id % 1728 % 720 → GEO_WALK → enc`

The 1728 mod is `GEO_FULL / GEO_PENTAGONS` (one pentagon's node space).
The 720 mod maps pentagon-local position to the hex-only walk cycle.
This preserves GEO_WALK bijectivity: each pentagon's local 720 positions map uniquely to GEO_WALK entries.

---

## Risk Assessment

| Risk | Mitigation |
|---|---|
| .twidx format break | Bump SID_TWIDX_VER to 2. User confirmed: clean break, no backward compat. |
| GEO_WALK enc mapping | node_id % 1728 % 720 → GEO_WALK → enc. Dual system (node_id + GEO_WALK enc) is clean. |
| test_tring_walk.c relies on GEO_FIBO_CLOCK=1440 | GEO_FIBO_CLOCK is a geo_jump.h constant, NOT related to SID tring; leave untouched. |
| tw_capture_int_combined() still returns zone/slot | Keep TWCaptureInt as-is; convert to node_id via tw_to_node() at boundary. |
| Some tests use tring_pos as a hash/index | Convert to node_id as index. |
| Tri centroids dropped | User confirmed: hex-only is sufficient. capo ×12 hex covers all pentagons. |
| ~240 lines of legacy code removed | This is the payoff — no mitigation needed, it's the goal. |

## Success Criteria

- [ ] `tri_hex_tess.h` THCoord uses `node_id`
- [ ] `sid.h` SIDCoord uses `node_id`, no `TWFaceCapture` dependency
- [ ] `tw_bridge.h` TWBridgeResult has no `tring_pos` field
- [ ] `tw_rewind_bridge.h` uses `node_id` indexing
- [ ] `runner/sid_cache.h` SID_NODE_SLOTS = GEO_FULL
- [ ] Legacy section in tw_face_bridge.h fully removed
- [ ] All 10+ test files compile with new system
- [ ] `sid_capture_priority()` works end-to-end with Y-triangle
- [ ] Roundtrip test passes (capture → store → summon → verify)
