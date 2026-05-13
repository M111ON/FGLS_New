# Hamburger Architecture — Handoff Document
**Session end state: 51/51 PASS**

---

## ไฟล์ทั้งหมด (1835 lines total)

| File | Lines | Role |
|------|-------|------|
| `gpx5_container.h` | 486 | Container format, structs, I/O primitives |
| `hamburger_pipe.h` | 408 | H1 dispatch + H2 invert recorder |
| `hamburger_classify.h` | 248 | Tile classifier (from v21), auto_pipes |
| `hamburger_encode.h` | 693 | H4 encode + H5 decode + encode_image |
| `test_hamburger.c` | — | T1–T8, 51 assertions |

Include order (ห้ามสลับ):
```
gpx5_container.h → hamburger_classify.h → hamburger_pipe.h → hamburger_encode.h
```
`hamburger_encode.h` include classify+pipe ให้แล้ว — caller include แค่ encode.h พอ

---

## สิ่งที่แก้ใน session นี้

### Fixes จาก session ก่อน
- `Gpx5PipeEntry.reserved` → `lflags uint16_t` + update pipe_read/write
- `GPX5_CODEC_RAW 0xFF` define หายไป → เพิ่มใน container.h
- `#endif` ลอยใน 3 ไฟล์ (conflict กับ `#pragma once`) → ตัดออก

### Option A: wire ttype เข้า invert stream
- entry format: `[tile_id 2B][ttype 1B][invert_val 4B]` = 7B (เดิม 6B)
- `HB_INVERT_ENTRY_SZ` 6→7
- `HbInvertRecorder` เพิ่ม `ttype[4096]`
- `hb_invert_record()` + `hb_invert_write_stream()` รับ/เขียน ttype
- decode ไม่ต้อง hardcode `TTYPE_FLAT` อีก — อ่านจาก entry[2]

### Option B: hamburger_classify.h (ใหม่)
- `hb_classify_tile()` — standalone integer-only, extracted จาก v21
- threshold calibrated จาก 16×16 tile: GRAD<150 → `15000`, EDGE<2000 → `200000`
- flat guard: `is_flat && avg_var_x100 < THRESH_GRAD` (ป้องกัน checkerboard false flat)
- `hb_ttype_to_ctype/codec()` auto routing
- `hb_auto_pipes()` สร้าง pipe table จาก ttype histogram (sort top-3)
- `hb_classify_batch()` classify ทีเดียวทั้ง grid

### hamburger_encode_image() (ใหม่)
```c
HbImageIn img = {Y, Cg, Co, 640, 480, 16, 16};
int r = hamburger_encode_image("out.gpx5", &img, seed, 0);
```
ทำทุกอย่างใน 1 call: classify → auto_pipes → pack tiles → encode → write

---

## Architecture สรุป

```
hamburger_encode_image()
    ├── hb_classify_batch()      ← classify all tiles (v21 logic)
    ├── hb_auto_pipes()          ← build pipe table from histogram
    ├── [pack YCgCo → uint8 tiles]
    ├── hamburger_encode()
    │     ├── hb_encode_run()    ← 1440-tick warm-up
    │     │     ├── hb_dispatch()          H1
    │     │     ├── hb_codec_apply()
    │     │     └── hb_invert_record()     H2
    │     ├── hb_invert_freeze()
    │     ├── hb_invert_write_stream()
    │     ├── hb_build_lut()
    │     └── [write: hdr/pipe/set/LUT/invert_stream]
    └── hb_encode_free()

hamburger_decode()
    ├── gpx5_open()
    ├── per tile: LUT O(1) → read ttype from entry[2]
    ├── gpx5_seed_local() + gpx5_hilbert_entry() + hb_dispatch()
    └── hb_codec_invert()
```

---

## Codec สถานะ

| Codec | Encode | Decode | Note |
|-------|--------|--------|------|
| SEED | ✅ | ✅ | FLAT tiles: exact 6B sample + orig_sz |
| DELTA | ✅ | ✅ | XOR vs seed prediction |
| RICE3 | ✅ | ✅ | Rice(k=3) bitstream roundtrip |
| HILBERT | ✅ | ⚠️ imprecise | bit→original mapping ต้องรู้ orig_sz |
| FREQ | ✅ | ✅ | predictive int16 delta + LEB128 residual stream |
| ZSTD19 | ✅ | ✅ | optional zstd, raw fallback when unavailable |
| RAW | ✅ | ✅ | fallback |

---

## Next steps

1. **H3 RICE3 decode** — implement rice(k=3) bit decoder (ง่าย, standalone)
2. **H3 HILBERT decode** — ต้องรู้ `orig_sz` → เพิ่มใน invert stream entry หรือ set header
3. **H3 FREQ/ZSTD** — FREQ now uses per-channel predictive delta for exact YCgCo tiles; ZSTD still optional through `zstd.h`
4. **decode → HbImageOut** — reconstruct YCgCo planes กลับ (inverse of encode_image)
5. **Threshold recalibrate** — รัน classify บน real images วัด distribution จริง

### Real-image calibration snapshot

อิงจาก `temp/preview.png` ที่แปลงเป็น BMP แล้วรัน `temp/calibrate.c` บน tile 16x16:

- Threshold ที่ใช้อยู่ตอนนี้:
  - `FLAT = 2000`
  - `GRAD = 10000`
  - `EDGE = 80000`
- Distribution บนภาพ preview:
  - `FLAT = 0`
  - `GRADIENT = 664`
  - `EDGE = 1048`
  - `NOISE = 592`
- ข้อสังเกต:
  - preview นี้แทบไม่มี flat tile ตาม gate ปัจจุบัน
  - การดัน `EDGE` ขึ้นช่วยลดการไหลไป RAW ได้ชัดเจนกว่าเดิม

---

## Sacred constants (ห้ามแก้)
```
GPX5_TICK_PERIOD = 1440   GPX5_BLOCK_SZ = 4896
GPX5_COMPOUNDS   = 144    GPX5_CUBE_SZ  = 4913 (17³)
HB_MAX_TILES     = 4096   HB_TILE_SZ_MAX = 4896
HB_INVERT_ENTRY_SZ = 7    (tile_id 2B + ttype 1B + invert_val 4B)
```

## Known issues / stubs
- `CODEC_HILBERT` decode ยังเป็น compact walk model ไม่ใช่ generic transform
- `hb_tick()` ส่ง `ttype=0u` เสมอ — ใช้ได้เฉพาะ hb_encode_run ที่ส่ง ttype จริง
- `hb_classify_batch` ไม่ clamp tile bounds ถ้า image size ไม่หาร tile size ลงตัว — caller ต้องจัดการ padding เอง
- `ZSTD19` จะวิ่ง raw fallback ถ้า build environment ไม่มี `zstd.h`

### Lossless note

`hamburger_encode_image()` / `hamburger_decode_image()` now store exact signed YCgCo plane samples as 16-bit little-endian bytes per pixel.
This makes the `test02_768.bmp` roundtrip exact again:

- `rgb_mismatch = 0`
- `max_delta = 0`
- `72 PASS / 0 FAIL` still holds on `test_hamburger.exe`

Also, `hb_encode_run()` now records each tile at least once instead of stopping at tick 1440 when the image has more than 1440 tiles.

### Size snapshot

On `temp/test02_768.bmp`:

- previous `gpx5` size: `5,224,158` bytes
- current `gpx5` size: `4,654,239` bytes
- reduction: `569,919` bytes
- roundtrip remains exact: `rgb_mismatch = 0`, `max_delta = 0`
