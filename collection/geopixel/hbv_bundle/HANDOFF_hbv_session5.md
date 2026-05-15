# HBV Session 4 Handoff (Updated)

## Status Carry-Forward from Session 3

**Codec ratios (lossless ∞ PSNR):**
| Image | Tile mix | Ratio |
|---|---|---|
| high_detail.bmp 768×768 | EDGE 45% NOISE 26% GRAD 29% | 1.18× |
| test02_768.bmp 768×768 | GRAD 86% EDGE 14% | 1.63× |

**Session 3 fixes locked:**
- EDGE → CODEC_ZSTD19 ✓
- RICE3 fallback `orig_sz` 2B header fix ✓
- HBHF auto-wire into `hamburger_encode_image` (11/11 PASS) ✓
- `hb_tile_stream_mc` multi-cycle + seed_local fix (8/8 PASS, synthetic) ✓

---

## NEW THIS SESSION — H64 Pipeline (POGLS → Geopixel bridge)

### Architecture decision locked: Path A (CODEC_SEED only)

Every tile stores `seed32` only (4B). No pixel content. No delta. Pure geometric address.

```
ScanEntry stream
    ↓  pogls_hilbert64_encoder.h   (H64Encoder)
HilbertPacket64  — 64 slots, RGB balanced, invert derived
    ↓  pogls_to_geopixel.h         (h64_to_geopixel)
H64GeopixelBridge — H64TileIn[64] + H64BridgeHeader (16B)
    ↓  hb_encode_seed_only()       ← NEXT SESSION: implement this
.gpx5 output: 4B × 64 tiles = 256B per packet
```

**Theoretical ratio: 15×** (4096B raw → 272B stored per packet)
**Confirmed working:** H64 encoder + bridge compile, RGB balance 12-12-12 ✓, invert derived correctly ✓

### Files added (carry forward):
- `pogls_hilbert64_encoder.h` — H64Encoder, HilbertPacket64, h64_feed/finalize/reconstruct
- `pogls_to_geopixel.h` — H64GeopixelBridge, h64_to_geopixel, h64_pipeline (Path A)

### Design constants (frozen):
```
H64_SLOTS = 64          (12 faces × 4 paths + 16 ghost)
H64_FACE_COUNT = 12     (dodecahedron)
H64_POSITIVE_SLOTS = 48
H64_GHOST_SLOTS = 16
RGB: face%3 → 0=R=Y, 1=G=Cg, 2=B=Co
ghost → Y plane always
invert = XOR(3 positive seeds per face) — never stored
```

### H64TileIn struct (20B):
```c
typedef struct {
    uint64_t seed64;   // epoch-mixed fingerprint
    uint32_t seed32;   // → hamburger CODEC_SEED input (4B stored)
    uint32_t tile_id;  // slot 0..63
    uint8_t  channel;  // 0=Y 1=Cg 2=Co
    uint8_t  face;     // 0..11
    uint8_t  path_id;  // 0..2 positive, 3=invert, 4=ghost
    uint8_t  flags;    // H64T_FLAG_*
} H64TileIn;
```

### H64BridgeHeader (16B — only thing needed to reconstruct):
```c
magic(4) + epoch(4) + chunk_count(4) + face_count(1) + ghost_phase(1) + version(1) + flags(1)
```

---

## Session 5 Work Plan

### Priority 1 — `hb_encode_seed_only()` (CODEC_SEED path)

**What:** New entry point in `hamburger_encode.h` that accepts `H64TileIn[]` instead of `HbTileIn[]`.
Writes `seed32` (4B) per tile with `CODEC_SEED` tag. No pixel processing. No classify.

**Implementation sketch:**
```c
int hb_encode_seed_only(const char        *out_path,
                         HbEncodeCtx       *ctx,
                         const H64TileIn   *tiles,
                         uint32_t           n_tiles);
```
Per tile: write `[tile_id(2B)][CODEC_SEED(1B)][seed32(4B)]` = 7B per tile.
Total per packet: 7B × 64 = 448B + gpx5 frame overhead.

**Decision point:** use existing gpx5 frame format or new H64 frame tag?
Recommend: new frame tag `GPX5_FRAME_H64 = 0x07` to keep decode path separate from image tiles.

### Priority 2 — `hb_decode_seed_only()`

Reverse: read seed32 from .gpx5 → `hb_predict(seed32, i)` → reconstruct 64 pixels per tile.
Must match encode path exactly — same `hb_predict()` formula, same epoch-mix.

### Priority 3 — Benchmark vs raw

Run `h64_pipeline` on real file (e.g. high_detail.bmp as ScanEntry stream).
Measure actual ratio vs theoretical 15×.
Compare with hamburger image path (1.18×) — expected H64 to win significantly
since it bypasses classify + codec entirely for seed-derivable content.

### Priority 4 (carry from S4) — FREQ+ZSTD

Still open: `CODEC_FREQ_ZSTD = 0x07` for GRAD tiles fallback.
**Note:** if GPX5_FRAME_H64 takes 0x07, reassign CODEC_FREQ_ZSTD = 0x08.
Resolve tag conflict before implementing either.

---

## Open Architecture Questions

1. **GPX5 frame tag for H64** — assign `GPX5_FRAME_H64` value before implement
2. **CODEC_FREQ_ZSTD tag** — check conflict with H64 frame tag, assign 0x08 if needed
3. **Epoch source** — `H64Encoder.pkt.epoch` currently zero (not wired to reshape cycle). Wire to `fabric_rotation_state()` or keep manual? Affects seed uniqueness across reshape cycles.
4. **Ghost zone decoder** — ghost tiles in .gpx5 currently have no decode path. Needed for residual zone extract. Defer until Priority 1+2 stable.

---

## Frozen Constants
HB_MAX_TILES=4096, HB_TILE_SZ_MAX=4896, GPX5_TICK_PERIOD=1440
H64: SLOTS=64, FACE_COUNT=12, POSITIVE_SLOTS=48, GHOST_SLOTS=16
Sacred: 144, 720, 1440, 3456, 6912, 27648

## All Files (carry forward)
**HBV core:**
`hamburger_encode.h`, `hamburger_classify.h`, `hamburger_pipe.h`,
`gpx5_container.h`, `gpx5_hbhf.h`, `hb_header_frame.h`, `hb_manifest.h`,
`hb_tile_stream.h`, `fibo_shell_walk.h`, `frustum_coord.h`, `geo_tring_walk.h`

**H64 pipeline (new this session):**
`pogls_hilbert64_encoder.h`, `pogls_to_geopixel.h`

**Tests:**
`test_hbhf_wire.c`, `test_multicycle_stream.c`, `test_calibrate_bmp.c`
