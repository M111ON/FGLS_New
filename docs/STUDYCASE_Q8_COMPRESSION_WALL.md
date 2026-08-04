# STUDY CASE — "บีบ Q8_0 ได้ 0.59x / 1,818x" เป็นเทพนิยาย 3 ชั้น

> **วันที่:** 2026-08-04
> **ผู้จัดทำ:** Session ตรวจ KIS v4 / beam prototypes / huffman_test
> **เป้าหมาย:** บันทึกข้อผิดพลาดเชิง方法论 เพื่อไม่ให้ session หน้าเชื่อตัวเลขที่ไม่ผ่านการพิสูจน์อีก
> **สถานะ:** ✅ ข้อสรุปพิสูจน์แล้วด้วยตัวเลขจริงบน SmolLM2-360M Q8_0 (386MB)

---

## 1. ภาพรวม — เราเคยเชื่ออะไร

ตลอดหลาย session มีตัวเลข "compression ratio" ที่น่าตื่นเต้นถูกบันทึกไว้:
- **0.59x** — KIS Codec v4 บีบ Q8_0 real model
- **~542B สำหรับโมเดลทุกขนาด** — แผน kis_codec_v3 (codebook + beam formula)
- **1,818x** — แผน geometric position reconstruction

ตัวเลขเหล่านี้ถูกใช้เป็นเหตุผลต่อยอดงานต่อๆ มา **โดยไม่มีใคร decode กลับมาพิสูจน์ lossless จริง**

---

## 2. ชั้นที่ 1 — "0.59x" คือ expansion ที่ print กลับด้าน

**หลักฐานจริง (รัน `kis_codec_v4_test.exe` บน SmolLM2 Q8_0, 2026-08-04):**

```
Tensor: token_embd.weight (47,185,920 weights)
Codec:  80,298,691 bytes
Raw:    47,185,920 bytes
Ratio:  0.59x          ← (double)n / enc = 47,185,920 / 80,298,691 = 0.587
Mismatches: 0
```

**รากของปัญหา:** โค้ด print `n/enc` (raw/codec) มาตลอด แต่คนอ่านตีความว่า "บีบเหลือ 59%"
- `1/0.587 = 1.703` = **codec ใหญ่กว่า raw 1.7 เท่า = EXPANSION**
- Lossless ผ่าน 100% (0 mismatches) แต่ **ผ่านไม่ได้แปลว่าบีบได้**

**บทเรียน:** ratio ที่พิมพ์ออกมา < 1 ต้องถามเสมอว่าสูตรคือ `n/enc` หรือ `enc/n` — และต้อง decode กลับมาเทียบทุกตัวทุกตำแหน่ง (ไม่ใช่แค่ "decode แล้วค่าตรงกัน" บนข้อมูลที่ encode ผิดตำแหน่ง)

---

## 3. ชั้นที่ 2 — แผน "542B กู้คืนทั้งโมเดล" ผิดเชิงคณิตศาสตร์

**แผน kis_codec_v3 กล่าวว่า:**
> เก็บ codebook (~542B) + beam_formula (value→cell O(1)) + classify bond + bridge_288 → กู้คืนทั้งโมเดล, ratio = N×1B / 542B = linear compression

**ทำไมเป็นไปไม่ได้ (พิสูจน์เชิงคณิต):**

1. **หลุมนกพิราบ (Pigeonhole):** SmolLM2 มี 47,185,920 weights แต่ address space มีแค่ 20,736 cells (20736 = 144²) → 47 ล้าน weights ต้องชนกันใน cell แน่นอน — ไม่มีทางให้แต่ละ weight ได้ address ต่างกัน

2. **ข้อมูลลำดับ (ordering information):** histogram เก็บได้แค่ "ค่า v มี n_v ตัว" แต่โมเดลต้องรู้ "ตัวที่ index 5176 คือค่าไหน" — ข้อมูลนี้ต้องการ ~6-7 bits/weight และ **ไม่มีสูตรใดสร้างข้อมูลที่ไม่ได้เก็บขึ้นมาได้** (Shannon entropy wall)

3. **ฟังก์ชัน deterministic:** `value→cell` ให้ผลเหมือนกันทุก occurrence ของค่าเดียวกัน → decode ไม่รู้ว่าตัวที่ i คือ occurrence #1 หรือ #5000 → กู้คืนลำดับไม่ได้

**บทเรียน:** ข้ออ้าง "compression จาก address space" ที่ไม่ระบุว่า *ข้อมูลตำแหน่ง 47 ล้านตัวถูกเก็บ/สร้างที่ไหน* = ต้องสงสัยทันที

---

## 4. ชั้นที่ 3 — 5 ต้นแบบ beam/zone/angular วัด "existence" ไม่ใช่ "position+count"

**ผล audit (Claude, 2026-08-04) ของ 5 ไฟล์ต้นแบบ:**

| ไฟล์ | สถานะ | ปัญหาจริง |
|---|---|---|
| `beam_projection_codec.c` | print "LOSSLESS!" | reconstruction check เจอ error จริง 170/32000 cells (0.5%) — ไม่ lossless; "Ghost: 0 bits" เป็นค่า hardcode |
| `beam_formula_test.c` | รันได้ | วัดแค่ ghost/active ของ activation bitmap — ไม่เคย reconstruct ค่าเดิมกลับมาเทียบ |
| `beam_angular_v2.c` | Crash | `qsort()` ส่ง comparator เป็น NULL → segfault ทันที |
| `beam_angular_test.c` | รันได้ | ratio 1.59x (gaussian) / 0.82x (blocky) — ไม่เคยต่ำกว่า raw แบบมีนัยสำคัญ |
| `beam_field_measure.c` | รันได้ | เหมือน angular_test + printf bug `%I64d` (Windows-only) |

**รากของปัญหา:** ทั้ง 5 ไฟล์วัด **"ค่านี้เคยปรากฏไหม" (existence — 1 bit)** ไม่ใช่วัด **"ได้ weight array กลับมาตรงเดิมทุกตัวทุกตำแหน่งไหม"**
- Projection ghost-check ตอบได้แค่ existence — ไม่บอก **กี่ครั้ง** และ **ตำแหน่งไหน**
- codec จริงต้องคืนค่า n_weights ตัวในลำดับเดิมเป๊ะ

**บทเรียน:** คำว่า "lossless" ต้องนิยามให้ชัด = **decode → compare ทุกค่า ทุกตำแหน่ง (n_weights ตัว)** — ไม่ใช่ "ghost cell = 0" หรือ "ไม่มี collision"

---

## 5. การวัดจริง 3 วิธี (2026-08-04, SmolLM2 Q8_0 จริง)

| วิธี | ไฟล์ | ratio | Lossless | หมายเหตุ |
|---|---|---|---|---|
| KIS v4 permutation (delta+varint) | `kis_codec_v4_test.c` | 1.70x | ✅ | permutation ของข้อมูล quasi-random → delta เฉลี่ย n/2 → ขยายเสมอ |
| Adaptive block (raw/bitmap/grp4/adapt) | `beam_compress_fixed.c` | **0.9896x** | ✅ 128,000/128,000 | ดีที่สุดในกลุ่ม payload-compression — ชนะได้แค่ ~1% |
| Huffman order-0 (canonical) | `huffman_test.c` | 1.059x | ✅ mm=0 ทุกชุด | H=7.495 bit/w บน Q8_0 จริง — เกือบเท่า 8 bit เต็ม |

**ข้อสรุปจาก 3 ทิศทาง (permutation / block / entropy):** Q8_0 redundancy ต่ำมากจน payload-compression ทุกวิธีชนกำแพง ~1.0x — **Shannon entropy เป็นขอบล่างทางทฤษฎี และ Q8_0 อยู่ใกล้ขอบล่างแล้ว**

---

## 6. ข้อผิดพลาดเชิงกระบวนการที่ต้องจำ (Checklist)

1. **Ratio < 1 ต้อง decode พิสูจน์ก่อนเชื่อ** — ทุกครั้ง, ทุก codec, ไม่มีข้อยกเว้น
2. **ตรวจสูตร ratio:** `n/enc` กับ `enc/n` ต่างกัน 1.7 เท่า — อ่านโค้ดก่อนบันทึกตัวเลข
3. **Lossless = compare ทุกค่าทุกตำแหน่ง** — ไม่ใช่ ghost check / existence / collision-free
4. **Q8_0 block = 34B** (2B f16 scale + 32 int8 weights) — ถ้าโค้ดใช้ stride อื่น (เช่น 33) = อ่านข้อมูลเพี้ยน → ตัวเลขทุกอย่างใช้ไม่ได้
5. **"Compression จาก address space" ที่ไม่บอกว่าตำแหน่ง 47 ล้านตัวเก็บที่ไหน** = หลุมนกพิราบ — สงสัยทันที
6. **Entropy wall เป็นเรื่องจริง** — H≈7.5-8 bit/w สำหรับ Q8_0 → อย่าคาดหวัง payload-compression ต่ำกว่า ~0.9x
7. **ถ้าตัวเลขสวยเกินจริง → audit โค้ดการวัดก่อนโค้ด codec** — ผิดที่ measurement มากกว่าที่ algorithm

---

## 7. ทางที่เหลือจริง (MAP not COMPRESS)

- **เป้าหมาย user เดิม:** "Observation, not compression" — geometry เป็น address space สำหรับ access/inference ไม่ใช่ compressor
- **ของที่ใช้ได้จริง:** `bridge_288` (bijection O(1) พิสูจน์แล้ว), `geo_frame_seek_1728` (stride-37), adaptive 0.9896x / zstd 1.04x สำหรับ blob
- **สิ่งที่เลิกเชื่อ:** "0.59x", "542B ทั้งโมเดล", "1,818x", "0.026x ของ projection_codec" — ทั้งหมดไม่ผ่านการพิสูจน์ lossless

---

## 8. ไฟล์ที่เกี่ยวข้อง

- `core/kis_codec_v4.h` — permutation codec (พิสูจน์แล้ว expand 1.7x)
- `core/kis_codec_v3.h` — แผน codebook+beam formula (ผิดเชิงคณิต, ใช้เป็น reference เตือนใจ)
- `core/kis_geom_simple.h` — prototype geometric position (lossy — เก็บแค่ signature แรกต่อค่า)
- `beam_addressing/beam_compress.c` + `beam_compress_fixed.c` — adaptive block codec (fixed = วัดถูกต้อง)
- `huffman_test.c` — entropy codec + verify (แก้ canonical แล้ว lossless 100%)
- `beam_addressing/beam_metatron_maze.c` — 0.97x lossless (33B/block)
- `collection/rdh/rdh_288_bridge.h` — bijection จริง O(1) ใช้ต่อยอดได้
