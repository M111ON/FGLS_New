# HBV Session 4 Handoff

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

## Session 4 Work Plan

### Priority 1 — FREQ post-ZSTD (GRAD tiles, ~29% of high_detail)

**Root cause:** FREQ codec writes raw when delta stream is not smaller than input.
For GRAD tiles, this fallback hits ~67% of the time (flat-ish gradient → delta ≈ noise → Rice3 bloats).
Fix: after FREQ decide-raw, attempt ZSTD19 on the raw payload before writing.
This is the same pattern already proven for EDGE.

**Expected impact:** high_detail ratio 1.18× → ~1.4–1.5× (rough estimate: 29% GRAD × 67% fallback × ~0.5 ZSTD gain on raw gradient data).

**Implementation target:** `hamburger_encode.h` — FREQ codec branch, add ZSTD19 post-pass on fallback path.
New codec tag needed: `CODEC_FREQ_ZSTD` or reuse `CODEC_ZSTD19` with type=FREQ in tile header.
**Decision point:** use separate tag vs. unified tag — recommend separate `CODEC_FREQ_ZSTD = 0x07` to keep decode path clean.

**Test:** `test_calibrate_bmp.c` — run both images, confirm ratio improvement, PSNR stays ∞.

---

### Priority 2 — Multi-cycle real image test

`hamburger_encode_image` with image that produces >4096 tiles (e.g. 2048×2048 BMP).
Verify: `_c000.gpx5` + `_c001.gpx5` files written, manifest built correctly, `hb_tile_stream_mc` can seek across cycle boundary without seed mismatch.

**Risk:** seed_local formula `tile_id` only (session 3 fix) — must verify this holds when tile_id resets at cycle boundary (tile_id 0..4095 per cycle, not global). If encode uses global tile_id for seed but stream uses local → mismatch.
**Check:** `hb_tile_stream_mc` seed path vs `hamburger_encode_image` seed path — confirm both use `tile_id % HB_MAX_TILES`.

---

### Priority 3 — hb_tile_stream_mc real .gpx5 seek test

After Priority 2 produces real multi-cycle files, run seek test: random access to tile in cycle 1, verify payload correct.
This closes the synthetic-only gap from session 3.

---

## Open Architecture Questions

1. **CODEC_FREQ_ZSTD tag** — new byte value 0x07 not yet assigned in `hamburger_classify.h`. Assign before implementing decode.
2. **GRAD threshold calibration** — current classify boundary between GRAD and NOISE may be suboptimal for high_detail. After FREQ+ZSTD, check if re-tuning GRAD/NOISE boundary improves ratio further (low cost, high info).
3. **NOISE codec** — still RICE3, 26% of high_detail. Next bottleneck after GRAD is resolved.

---

## Frozen Constants
HB_MAX_TILES=4096, HB_TILE_SZ_MAX=4896, GPX5_TICK_PERIOD=1440
Sacred: 144, 720, 1440, 3456, 6912, 27648

## Files (unchanged from session 3, carry all)
`hamburger_encode.h`, `hamburger_classify.h`, `hamburger_pipe.h`,
`gpx5_container.h`, `gpx5_hbhf.h`, `hb_header_frame.h`, `hb_manifest.h`,
`hb_tile_stream.h`, `fibo_shell_walk.h`, `frustum_coord.h`, `geo_tring_walk.h`,
`test_hbhf_wire.c`, `test_multicycle_stream.c`, `test_calibrate_bmp.c`
