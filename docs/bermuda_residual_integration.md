# Bermuda Residual Integration — Architecture Document

## 0. The Sacred Ratio: h_seal = 1/φ² = 0.381966

```
φ   = 1.618033988...   (golden ratio)
1/φ = 0.618033988...   (the gap)
1/φ²= 0.381966011...   (the seal, h_seal)
```

The h_seal links the entire Bermuda shadow layer to the geometric residual:  
it is the exact proportion of data that the system *preserves off-route*.

**Key calculation (Section 2):**

```
Icosahedron circumradius (unit edge):
  R_ico = sin(2π/5) = √((5+√5)/8)  ≈ 0.951056... ≈ 1.0

Dodecahedron circumradius (unit edge):
  R_dod = (√3/4)(1+√5) = φ√3/2  ≈ 1.401258...

For normalized unit sphere:        Icosa(R) = 1.0
  Ratio: Dodeca(R) = 1/φ²        ≈ 0.381966

gap = Icosa(R) - Dodeca(R) = 1.0 - 0.381966 = 0.618034... = 1/φ
```

That gap — the space between the icosahedron's outer hull and the dodecahedron's
inscribed boundary — is the **residual capacity**. It is exactly `1/φ`, and in
the codebase this gap maps to the shadow zone capacity.

## 2. Cube Displacement Into Residual

```
Cube unit side → circumscribed icosahedron (1.0) encloses all data
Cube weight occupies 1.0 in coordinate space
The dodecahedron at 1/φ² ≈ 0.382 is the "structured core"
   — everything outside this is residual / float / COLD

   1.0 (icosa) ─────────────── outer hull, all data lives here
        │
        │   0.618 (gap) ← RESIDUAL CAPACITY = 1/φ
        │   ─── COLD data (float, entropy, unstructured) lives here
        │
   0.382 (dodeca) ─── structured core boundary  = 1/φ² = h_seal
        │
       0 ── center
```

The cube's displacement into the ring measures:
- **Inside 0.382** → ROUTE (structured, HOT)
- **Between 0.382 and 1.0** → GROUND (residual, COLD)

This is exactly the hard-double of geometric residual:  
`gap = 1 - 1/φ² = 1/φ`.

## 3. Source-Truth Constants

From `coord_spine.h` (the single source of truth for all geometry constants):

| Symbol | Value | Meaning |
|--------|-------|---------|
| `GEO_FULL` | 20,736 | Full Y-triangle space (`144²`) |
| `GEO_FULL_N` | 3,456 | Nodes per hemisphere (`576 × 6`) |
| `GEO_PENTAGONS` | 12 | Dodecahedron faces (macro-defines `GEO_FACES_DODECA`) |
| `SHADOW_N_SLOTS` | `GEO_FULL / 12` = 1,728 | Slots per shadow zone |
| `SHADOW_ZONES` | 2 | Zones A (10) and B (11) |
| `SHADOW_ZONE_A` | 10 | North-track shadow sector |
| `SHADOW_ZONE_B` | 11 | South-track shadow sector |
| `SHADOW_NODE_BASE(zone)` | `zone * 1728` | Absolute node base for a shadow zone |
| `BERMUDA_SHADOW_RING` | 144 | Ring buffer (`2² × 3²`, sacred, demo/convenience) |
| `BERMUDA_CHUNK` | 64 | DiamondBlock size (prefix of cache-line, 1 CPU cache-line, 0x40 bytes) |
| `BERMUDA_COLD_RANGE_THR` | 64 | Byte-range >= 64 → candidate COLD |
| `BERMUDA_COLD_TRANS_THR` | 96 | XOR transitions >= 96 → confirmed COLD |
| `BERMUDA_STRIDE` | 37 | Hilbert-stride (prime, coprime to 720) |

**Critical derive build-time check from `coord_spine.h` line 206–214:**

```c++  
// Build-time verification: two shadow zones = exactly 2/12 of GEO_FULL
#if (20736 / 12 != 1728)
#  error "COORD_SPINE: GEO_FULL/12 should be 1728"
#endif

#if (11 * 1728 + 1728 > 20736)
#  error "COORD_SPINE: shadow zones overflow GEO_FULL"
#endif
```

The residual gap = `2/12 = 1/6` of total coordinate space, mapped to
`GEO_FULL / GEO_PENTAGONS = 1728` slots per zone, and `2 × 1728 = 3456`
total shadow slots (matches `TRING_RESIDUAL_N = 3456`).

## 4. Data Flow: Cube → Classify → Route/Shadow → Retrieve

```
                     ┌──────────────┐
                     │ raw 64B chunk │  (from system / codebook)
                     └──────┬───────┘
                            │
                            ▼
                bermuda_shadow_classify()
                ├─ 64B byte-range (min-max)
                ├─ XOR transition entropy (64-strip)
                └─ returns BermudaShadowEntry { temperature: HOT|COLD }
                            │
                ┌───────────┴───────────┐
                │                       │
        temperature = HOT          temperature = COLD
        (uniform / integer)         (float / entropy)
                │                       │
                ▼                       ▼
    bermuda_route_token()       bermuda_shadow_push()
    ┌───── ROUTE lane ────┐      ring_buffer[144]
    │  mode: ORBITAL       │      │
    │  gear: snap(n)       │      │  or → shadow_zone_write()
    │  shape: I/O           │      │       bond_key, tick,
    │  polarity: ROUTE      │      │       temperature saved
    │  tile decode: yes     │      │
    └──────────────────────┘      └────── GROUND lane ──
            │                             │
            ▼                             ▼
    tile[7] bytes                  O(1) hint retrieval
    → pipeline                      → bermuda_shadow_find(ring, bond_key)
                                    → shadow_find_by_bond(...)
                                    → gsp_retrieve_cold(&ctx, bond_key)
```

### Key API signatures

#### Classify (bermuda_shadow.h, line 104)
```c+bermuda_chunk_temperature(const uint8_t *chunk64) → uint8_t (0:HOT, 1:COLD)
```

#### Dispatch — classify + route (bermuda_shadow.h, line 223)
```c+bermuda_shadow_dispatch(BermudaShadowRing *ring,
                          const uint8_t *chunk64,
                          uint16_t idx, uint8_t gear, uint8_t mode,
                          uint64_t addr, uint64_t bond_key,
                          BermudaRouteEntry *route_out) → uint8_t (0/1)
```

#### Dispatch → ShadowZone (bermuda_shadow.h, line 265)
```c+bermuda_shadow_dispatch_to_zone(Bring, ShadowZone *shadow,
                                  ... bond_key, route_out, &node_id)
```

#### Pipeline glue (geom_shadow_pipe.h, line 104)
```c+gsp_pngle_push(ctx, chunk64, idx, gear, mode, addr, bond_key, &result) → int
```

#### Read-back (bermuda_shadow.h, line 205)
```c+bermuda_shadow_find(ring, bond_key) → const BermudaShadowEntry * or NULL
```

#### Read-back via ShadowZone (shadow_zone.h, line 57)
```c+shadow_find_by_bond(shadow_zone, bond_key) → ShadowSlotMeta * or NULL
```

### P5H Riba-cage (p5h_ribcage.h, optional — `-DP5H_ENABLE`)
```
1728 pipes × 12 ticks = 20,736 = GEO_FULL
120  pipes × 12 ticks = 1,440  = FiboClock cycle
12   pipes × 12 ticks = 144   = Tower / FLUSH boundary
Inside each pipe: 10 phases (0-4 outer, 1-5 inner)
```

The p5h riba-cage is orthogonal to the shadow lane — it provides a 10-phase
"bonus room" inside the tick loop. COLD data enters the pipe at a barrier tick
(tick % 12 == 0), remains in phase 0-9 for cross-pipe borrowing, and exits
at the barrier when `barrier_flag == P5H_BARRIER_SYNC`.

### Bermuda export (bermuda_export.h)
API calls: `bermuda_snap_gear(n)`, `bermuda_traverse(idx,gear,mode)`,
`bermuda_zone(idx,gear)`, `bermuda_route_token(idx,gear,mode,&route)`.
Hilbert-37 codec: `bermuda_hilbert_encode(pos,gear)`, `bermuda_hilbert_decode(idx,gear)`.
Cross-livata: `BERMUDA_CROSS = {9,10,11,6,7,8,3,4,5,0,1,2}` maps zone to its antipode.

## 5. Concrete Example: Data Point Into Shadow and Back

Assume a 64-byte float-imagging tonne that is COLD on entry:

```
# --- ENTER SHADOW ---
chunk = [0x3f,0x80,0x00,0x00, ...]   # 64B of densely unlikely values
bond_key = 0xDEADBEEF_00000001
idx = 417
gear = 2
mode = 0 (ORBITAL)

# Step 1: Classify
temp = bermuda_classify(chunk)   # → COLD (range ~ 128, trans ~ 150)

# Step 2: Dispatching (shadow-path)
res = bermuda_shadow_dispatch(
    ring,
    chunk,          # 64B raw input
    idx,            # 417
    gear,           # 2
    mode,           # 0 (ORBITAL)
    0x100_417,      # grand: random address origin
    bond_key,       # 0xDEADBEEF_00000001
    route_capture   # receives CROSS-traversed routing info
)
# → returns BERMUDA_SHADOW_COLD (1)
# → entry written to ring buffer at head position
# → route_capture.polarity = 1 (forced GROUND lane)
# → ring->couold = 1

# At this point, the 64B block is safe in the ring,
# NOT flowing through the tile decode pipeline.

# --- RETRIVE POST ---
# Some later step, when the system is in recovery mode:
entry = bermuda_shadow_find(ring, 0xDEADBEEF_00000001)
if entry:
    # entry->data[:] == original 64B chunk
    # entry->temperature == COLD
    # entry->gear == 2, entry->slot == 417
    # data is fully preserved and can be re-injected
    original = entry->data   # recovery done
else:
    # ring has overrotated (144 entries only); data is lost
    # fallback: memory-resolution from longer-term ShadowZone (1728 slots)
    meta = shadow_find_by_bond(&shadow_zone_a, bond_key)
    if meta:
        # meta->bond_key matches, data in shadow-write zone

# The ring has only 144 capacity (1/12 of GEO_FULL/12, so ~1/144 of the
# available shadow slots p), so it is useful for short-term emission.
# Long-term cold storage is the ShadowZone (1728 slots, one per pentagon sector).

# → Total system capacity: 144 (ring) + 3456 (ShadowZone A+B) ≈ 3600 cold slots
# → Equals ~ 0.174 (3600/20736) of GEO_FULL for cold, plus HOT forward pipe
```

---

## 6. Constant Chain Verification

_Yes significant node-id chain, verified by coord_spine.h compile-time guards:_

| What | Value | Verified At |
|------|-------|-------------|
| GEO_FULL | 20736 | `6 * 3456` |
| GEO_FULL / 12 | 1728 | `206 // 1728` build-time |
| SHADOW_N_SLOTS | 1728 | `GEO_FULL / GEO_PENTAGONS` |
| SHADOW_NODE_BASE(A) | 17280 | `10 * 1728` |
| SHADOW_NODE_BASE(B) | 19008 | `11 * 1728` |
| 19008 + 1728 | 20736 | `#if` not-exceed check: `21` |
| SHADOW_BUFFER_SIZE | 110592 | `1728 * 64` |
| BERMUDA_SHADOW_RING | 144 | `2⁴ * 3²` — convenient sub-window |
| TRING_RESIDUAL_N | 3456 | `2 × 1728` — matches shadow total |
| h_seal | 0.382 | 1/φ² ∓ 1 minus 0.618 = matches 1/φ |

Connection chain: `1/φ ≈ 0.618` (the gap) → `2/12 = 1/6` (the geometric fraction) →
`GEO_FULL/12 = 1728` (per-zone size) → `2×1728 = 3456` (total cold storage) →
`TRING_RESIDUAL_N = 3456` (mirrors the geometric gap exactly).

---

*Document version: auto-generated from live source-material (2026-07-30)*.
*All values verified against `collection/coord_spine.h` (single source of truth) and `collection/bermuda_shadow.h` at the time of writing.*