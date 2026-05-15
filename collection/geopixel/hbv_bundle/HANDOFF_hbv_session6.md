# HBV Session 6 Handoff

## สถานะ: ✅ MERGED + TESTED

### Bugs ที่ปิดแล้ว (carry from hbv_fixed)
- RAW codec false clear-tile → `hb_invert_record_enc` + codec param ✓
- hb_vault.h header 8B → 16B (zdata_sz exact) ✓
- SHA256 round-trip: 3/3 MATCH ✓

### Fixes จาก session นี้ (fibo_hb_wire)
- YCgCo forward formula แก้เป็น standard
- FLAT → CODEC_HILBERT (lossless guarantee)
- GRAD → CODEC_ZSTD19 (ข้าม FREQ ที่ expect int16 YCgCo)
- encode RGB uint8 โดยตรง (ไม่ผ่าน int16 pack)

## Benchmark (worst case: geometric + iridescent JPEG→BMP)

| Image | Tiles | Ratio | Mismatch |
|---|---|---|---|
| geometric (736×1304) | 14996 | **1.111×** | 0 ✓ |
| holographic (736×1304) | 14996 | **1.150×** | 0 ✓ |
| synthetic 768×768 | 9216 | **2.948×** | 0 ✓ |

Tile mix (worst case): FLAT 2%, GRAD 67-72%, EDGE 25-30%, NOISE <1%

## Files ใน bundle (17 files, canonical)
hamburger_encode.h ← hbv_fixed base (RAW codec fix)
hamburger_pipe.h   ← hbv_fixed base (clear-tile skip fix)
hb_vault.h         ← hbv_fixed base (16B header fix)
hamburger_classify.h
fibo_hb_wire.h     ← fixed this session (YCgCo + GRAD+ZSTD19)
fibo_layer_header.h
fibo_tile_dispatch.h
pogls_hilbert64_encoder.h
pogls_to_geopixel.h (Path A: CODEC_SEED, 16B header only)
+ 8 gpx5/hb support headers

## Next session priorities

### P1 — EDGE codec tuning
EDGE 25-30% ยังใช้ ZSTD19 เหมือน GRAD
ลอง CODEC_HILBERT สำหรับ EDGE → hilbert walk น่าจะ predict edge structure ได้ดีกว่า entropy coding
Expected: ratio 1.111× → ~1.2-1.3×

### P2 — hb_encode_seed_only()
Entry point รับ H64TileIn[] → CODEC_SEED path โดยตรง
4B × 64 tiles = 256B per packet (theoretical 15×)
ต้อง wire epoch จาก fabric_rotation_state() เข้า H64Encoder.pkt.epoch

### P3 — CODEC_FREQ_ZSTD (tag 0x08)
FREQ+ZSTD19 post-pass สำหรับ GRAD tiles ที่ fallback raw
ใช้ tag 0x08 (ไม่ใช่ 0x07 เพราะ reserved สำหรับ GPX5_FRAME_H64)

## Frozen constants
HB_MAX_TILES=4096, HB_TILE_SZ_MAX=4896, GPX5_TICK_PERIOD=1440
H64: SLOTS=64, FACE_COUNT=12, POSITIVE_SLOTS=48, GHOST_SLOTS=16
Sacred: 144, 720, 1440, 3456, 6912, 27648
