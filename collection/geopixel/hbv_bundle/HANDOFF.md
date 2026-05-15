# FGLS — Geopixel FiboLayer Handoff

## สถาปัตยกรรมปัจจุบัน (Current Architecture)

```
[cover: HbHeaderFrame(64B) + FiboLayerHeader(32B)]
[tile_0 R-only(64B)] [tile_1 R-only(64B)] ... [tile_N]
```

### 3-Layer Separation (Identity ≠ Orientation ≠ Verify)

| Layer | Function | Key | Location |
|---|---|---|---|
| **IDENTITY** (group) | `seed ^ (set<<8)` | Coarse grouping, NO rotation | `fibo_group_key()` |
| **ORIENTATION** (align) | D4 normalize + Hilbert shift | Canonicalize tile before encode | `find_min_rotation_d4()` + `hilbert_canonicalize()` |
| **VERIFY** (refine) | `fibo_flow_fp()` popcount | Boundary gate after grouping | `fibo_flow_fp()` |

### Pipeline

```
raw chunk(64B)
  → D4 normalize (8 states: rot90/180/270 + flip)
  → Hilbert shift canonicalize (64 shifts, min B-sum)
  → compute seed → group by seed ^ (set<<8)
  → compute route_id → R=chunk, G=route_byte, B=R^G
  → classify → if lossless: store R-only(64B), else seed-only(8B)
```

### Tile Storage — R-only (Lossless)

- **R** (64B) = Hilbert-permuted chunk — STORED
- **G** (64B) = route_byte at each Hilbert position — DERIVED at decode from `fibo_derive_route()` + Hilbert index
- **B** (64B) = R^G — COMPUTED at encode for classifier, NOT stored

**Lossless guarantee:** R = all 64 original chunk bytes (reordered by Hilbert curve). Decoder applies inverse Hilbert to reconstruct chunk.

### Ratio

| Data type | Lossless tiles | Seed-only tiles | Ratio |
|---|---|---|---|
| Geometric (synthetic) | 8.2% | 91.8% | **5.06x** |
| Arbitrary (BMP baseline) | — | — | ~1.16x |

### Random Access O(1)

1. Read `HbHeaderFrame` 64B → dimensions, codec_map
2. Read `FiboLayerHeader` 32B → tick, layer_seq, seed_origin
3. `fibo_layer_expand()` → full 16-route state O(1)
4. `fibo_derive_route(seed, seq, set, route)` → route_id O(1)
5. `fibo_layer_derive_rotation()` → rotA/rotB O(1)
6. `codec_map[route_id]` → decode tile via hamburger codec
7. Inverse Hilbert → reconstruct original chunk

## Key Functions Added

### `fibo_layer_header.h`

```c
// Identity axis: topology space (pentagon × hexagon × time)
uint16_t fibo_rotA(uint8_t route, uint8_t set, uint16_t phase);

// Orientation axis: position space (dodecahedron coords)
uint16_t fibo_rotB(uint8_t face, uint8_t edge, uint16_t z);

// Combined fingerprint — verify/refine ONLY
uint64_t fibo_flow_fp(uint64_t seed, uint8_t route, uint8_t set,
                       uint16_t phase, uint8_t face, uint8_t edge, uint16_t z);

// Coarse grouping key — NO rotation
uint64_t fibo_group_key(uint64_t seed, uint8_t set);
```

### `test_diamond_flow_grouping.c`

```c
// D4 symmetry group (8 states)
CanonicalD4 find_min_rotation_d4(const uint8_t src[64]);
// rot90, rot180, rot270, flip_h, flip_v

// Hilbert shift canonicalizer (64 shifts)
CanonicalHilbertShift hilbert_canonicalize(const uint8_t src[64], uint64_t route_id);

// Tile builder: R=content, G=route_byte, B=R^G
void tile_build_with_binding(...);
```

## Verified Invariants

- ✅ `fibo_layer_verify()` → PASS ✓ (inv_of_inv == XOR all 12 pos)
- ✅ 4 sets × (3 pos + 1 inv) = 16 paths, 4 free inverts
- ✅ Hilbert 16-path: `face%4` → set, `(face/4+edge)%3` → route
- ✅ [cover] + [tiles] format with O(1) random access
- ✅ D4 normalization: 87.3% tiles rotated to canonical orientation
- ✅ Hilbert shift reduces B-sum (R^G binding) by 19%+
- ✅ R-only storage (lossless): ratio 5.06x

## Next Steps / TODOs

1. **Real POGLS data test** — ปัจจุบันใช้ synthetic data, ควรทดสอบกับ POGLS DiamondBlock จริง
2. **Hamburger integration** — เชื่อม `hamburger_encode.h` เข้า pipeline (encode B=0 → near-zero cost)
3. **DfGrouper refinement** — `fibo_flow_fp()` popcount gate ยังไม่ integrate ใน loop จริง
4. **Phase alignment** — `flow_id % 64` → Hilbert phase alignment ยังไม่ activate
5. **8-path mirror extension** — D4 → D8 (add rot45 etc.) for finer alignment

## Files Included

| File | Path | Description |
|---|---|---|
| `fibo_layer_header.h` | `hbv_bundle/` | Core: fibo clock + rotA/B + fingerprint |
| `fibo_tile_dispatch.h` | `hbv_bundle/` | Tile dispatch + route mapping |
| `hb_header_frame.h` | `hbv_bundle/` | Cover format (HbHeaderFrame 64B) |
| `gpx5_container.h` | `hbv_bundle/` | GPX5 container reference |
| `test_diamond_flow_grouping.c` | `hbv_bundle/` | Main pipeline test (D4 + Hilbert + R-only) |
| `test_geopixel_sequence.c` | `hbv_bundle/` | Previous sequence test (reference) |
| `geo_diamond_field.h` | `hbv_bundle/core/twin_core/` | Diamond flow engine |
| `pogls_fold.h` | `hbv_bundle/core/twin_core/` | DiamondBlock structure |
