# HBV Session 3 Handoff

## Completed This Session

### D — hb_tile_stream multi-cycle
- `hb_manifest.h` — HbManifest (cycle_id → path)
- `hb_tile_stream_mc()` — resolves path via manifest → delegates to hb_tile_stream
- **Bug fixed:** seed_local used `cycle_id * HB_MAX_TILES + tile_id` → changed to `tile_id` only (matches encode)
- **8/8 PASS** (synthetic 2-cycle)

### A — HBHF auto-wire into hamburger_encode_image
- `gpx5_hbhf.h` — `gpx5_append_hbhf()` + `gpx5_read_hbhf()`
- Strategy: 64B trailer at EOF + `GPX5_FLAG_HBHF = 0x04` in flags byte (offset 5)
- `hamburger_encode_image` auto-appends after encode loop
- **11/11 PASS**

### C — calibrate on real BMP + C-fix

**Fix applied:**
- `hamburger_classify.h`: EDGE → `CODEC_ZSTD19` (was RICE3)
- `hamburger_encode.h`: RICE3 fallback bug — raw copy lacked `orig_sz` 2B header → decode misparse, fixed
- zstd must be linked: `-lzstd`

**Results (lossless ∞ PSNR both images):**

| Image | Tile mix | Before | After |
|---|---|---|---|
| high_detail.bmp 768×768 | EDGE 45% NOISE 26% GRAD 29% | 0.38× | **1.18×** |
| test02_768.bmp 768×768 | GRAD 86% EDGE 14% | 0.38× | **1.63×** |

---

## Open Items (Next Session)

| Priority | Item |
|---|---|
| 1 | **FREQ post-ZSTD** — GRAD tiles: FREQ fallback (delta not smaller) → wrap with ZSTD19 pass. Expected gain for GRAD-heavy images. |
| 2 | **Multi-cycle real image test** — `hamburger_encode_image` with image > 4096 tiles; verify `_c000/_c001` path + manifest build |
| 3 | **hb_tile_stream_mc real image** — seek test with actual .gpx5 cycle files (not synthetic) |

---

## File Changes Summary

| File | Status |
|---|---|
| `hamburger_encode.h` | modified — HBHF wire; RICE3 fallback fix; +includes |
| `hamburger_classify.h` | modified — EDGE→ZSTD19 |
| `hb_tile_stream.h` | modified — seed_local fix; +hb_tile_stream_mc |
| `hb_manifest.h` | new |
| `gpx5_hbhf.h` | new |
| `test_multicycle_stream.c` | new — D test |
| `test_hbhf_wire.c` | new — A test |
| `test_calibrate_bmp.c` | new — C calibration |

## Frozen Constants
HB_MAX_TILES=4096, HB_TILE_SZ_MAX=4896, GPX5_TICK_PERIOD=1440
Sacred: 144, 720, 1440, 3456, 6912, 27648
