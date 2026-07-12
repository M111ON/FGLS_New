# geofield — GeoField Topology & Routing

Goldberg polyhedron coordinate system for geometric data addressing.
Maps byte streams onto an icosahedral sphere via GpAddr, routes chunks
through Metatron's Cube, and packs into FrustumBlock/DiamondBlock containers.

## Architecture

```
byte stream → [chunker] → 64B chunks → [GpAddr{tile_id, dim}] → [Metatron route]
  → ORBITAL/CHIRAL/CROSS/HUB → [FrustumBlock] → [DiamondBlock × 54] → serialized
```

## Files in `collection/geopixel/geofield/`

### Goldberg Sphere — Address Space

| File | Role |
|---|---|
| `geo_goldberg_sphere.h` | **GpAddr** struct: `{tile_id, dim}`. `gp_face_count(n) = 10n² + 2`. Sector/pentagon mapping. 12 pentagons, invariants. |
| `skeleton_index.h` | O(1) addr → geometry skeleton lookup index. |

### Frustum — Container Layer

| File | Role |
|---|---|
| `frustum_layout_v2.h` | **FrustumBlock container** (136 lines). 4896B = 17B header + 3456B data (54×64B DiamondBlocks) + 1440B meta. |
| `frustum_slot64.h` | **DiamondBlock 64B storage** (148 lines). 54 slots × 64B = 3456B = GEO_FULL_N. Slot: core[4] merkle roots, reserved_mask, write_count, slope_lo. |
| `frustum_trit.h` | **Trit decomposition** (96 lines). `trit = (addr ^ value) % 27`. Extracts coset, face, level, letter, slope. |
| `frustum_gcfs.h` | GCFS (Geometric Canonical Form System). |

### Metatron — Routing Layer

| File | Role |
|---|---|
| `geo_metatron_route.h` | **Metatron routing** (324 lines). 4 route types: ORBITAL (same face, slot+1), CHIRAL (face↔face+6), CROSS (inter-ring bijection), HUB (center node). Sacred constants: TRING_FACES=12, TRING_FACE_SZ=60, TRING_CYCLE=720, META_COND_MOD=36. |
| `tring.h` | **TRing** — perpendicular fiber channel between dimension layers. |
| `geo_tring_walk.h` | TRing walk encoder: stride-37 walk for position 0..719. |

### GeoField Core — Encode/Decode

| File | Role |
|---|---|
| `geo_field_core.h` | **Main unified GeoField** (780 lines). Integrates GpSphere, FrustumBlock, Metatron routing, Trit, Flow chunking. Complete encode/decode loop. |
| `geo_field_core_2.h` | Variant with alternative routing/layout. |
| `geo_field_core_3.h` | Variant with heptagon fence + atomic reshape support (797 lines). |
| `geo_field_main.c` | Demo: 4 demos (complete loop, scale, shape, file roundtrip). |
| `geo_field_bridge.c` | C bridge: reads file → geo_field roundtrip → JSON stats. |

### Bridge & Integration

| File | Role |
|---|---|
| `geo_gp_frustum_bridge.h` | GpSphere ↔ FrustumBlock bridge (195 lines). O(1), stateless, no malloc. Chunk → GpAddr → FrustumBlock slot → Diamond data. |
| `pogls_fold.h` | POGLS fold operations for geometric compression. |
| `pogls_atomic_reshape.h` | Atomic reshape for seamless topology transitions. |
| `pogls_rotation.h` | Rotation transforms in Goldberg sphere coordinates. |
| `pogls_1440.h` | 1440B meta zone constants and helpers. |
| `geo_reshape_junction.h` | Junction 30 routing: reshape operations for geometry topology. |
| `geo_temporal_lut.h` | Temporal LUT for geometric time-based addressing. |

### Pipeline & Fabric

| File | Role |
|---|---|
| `geo_flow_chunker_v8.h` | Content-driven boundary detection chunker. |
| `fabric_wire.h` | Switch Gate, classification, fabric wire infrastructure. |
| `fabric_wire_drain.h` | Drain fabric wire: pentagon drain activation. |
| `goldberg_shutter.h` | Ring confidence system for Goldberg sphere addressing. |
| `heptagon_fence.h` | Boundary isolation heptagon fence. |
| `geometry_store_reader.h` | Reader for loading `.gsten` tile data. |

## Key Constants

| Constant | Value | Meaning |
|---|---|---|
| TRING_FACES | 12 | Pentagon count |
| TRING_FACE_SZ | 60 | Slots per face |
| TRING_CYCLE | 720 | Total slots (12×60) |
| FGLS_DIAMOND_COUNT | 54 | DiamondBlocks per FrustumBlock |
| FGLS_CLOCK_TICKS | 144 | Clock ticks per container |
| GEO_FULL_N | 3456 | Total data bytes (54×64) |

## Routing Modes

| Mode | Behavior |
|---|---|
| **ORBITAL** | Same face, slot+1 (sequential within face) |
| **CHIRAL** | Face ↔ face+6 via TRING_CPAIR (mirror) |
| **CROSS** | Inter-ring bijection (between layers) |
| **HUB** | Center node routing (radial) |

## Build

```bash
cd collection/geopixel/geofield && make
./geo_field_main          # run 4 demos
./geo_field_bridge FILE   # encode a file
```

## Related

- `geopixel-codec` (standalone `geopixel/`) — tile encoding/decoding
- `pogls_geopixel` (runner integration) — v3 FrameStore block compression
- `docs/geo_field and geopixel Subsystems.md` — full pipeline analysis
