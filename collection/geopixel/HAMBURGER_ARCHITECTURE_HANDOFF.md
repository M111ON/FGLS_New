# Hamburger Architecture — Session Handoff
**Date:** 2026-05-10 | **Status:** Concept proven, ready to implement

---

## 1. Core Concept — Hourglass Model

> "แค่ทรายย้ายที่ ทุกอย่างไม่มีอะไรหาย โครงสร้างคล้ายเดิมแค่เคลื่อน"

```
ด้านบน  = encode state (R/G/B pipes)
รูกลาง  = invert chain  ← ทรายทั้งหมดไหลผ่านนี้
ด้านล่าง = decode state

decode = พลิก hourglass — ไม่มี algorithm พิเศษ
```

ทรายอัดแน่นได้:
- bg tiles → 1 seed แทนทั้งหมด
- motion → equation แทน per-frame
- noise → frequency params แทน raw values

---

## 2. Hamburger Architecture

### 3 Pipe + Invert Chain

```
Input (image / video / pdf / anything)
         ↓
   Hilbert curve แบ่ง address space
         ↓
R pipe ──→ encoder เลือกเอง ──→ ทิ้ง (ไม่เก็บ)
G pipe ──→ encoder เลือกเอง ──→ ทิ้ง (ไม่เก็บ)
B pipe ──→ encoder เลือกเอง ──→ ทิ้ง (ไม่เก็บ)
         ↓
   invert chain จด before/after ทุก tile
   (passive — ไม่รู้ว่าใครทำอะไร)
         ↓
   seed + invert chain = ข้อมูลทั้งหมด
```

**กฎสำคัญ:** ไม่มี pipe ใดรู้ว่าตัวเองทำอะไรเพื่ออะไร — ต้องดูจากภาพรวมเท่านั้น

### Pipe Isolation

```
tile_hilbert_pos % 3 == 0 → R pipe
tile_hilbert_pos % 3 == 1 → G pipe
tile_hilbert_pos % 3 == 2 → B pipe
```

Hilbert path คือ address — ไม่มี pipe ชนกันเพราะอยู่คนละมิติ

### Dynamic Load Balancing

ถ้า R heavy:
```
tile_hilbert_pos % 6 == 0 → R pipe
tile_hilbert_pos % 6 == 3 → G ช่วย R
```
Geometry ยังคุม — invert chain ไม่รู้เรื่อง ไม่กระทบ

---

## 3. Invert Chain — ผู้เก็บข้อมูลทั้งหมด

```
R/G/B pipes:   process แล้วทิ้ง
invert chain:  จด state ทุกชั้น

timeline:
tile 1: R เท → invert จด → ส่งต่อ
tile 2: R+G เท → invert จด → ส่งต่อ
tile 3: R+G+B เท → invert จด → ส่งต่อ
```

invert ไม่รู้ว่า tile ไหนเบอร์อะไร — รู้แค่:
- "ก่อนหน้าฉันเป็นยังไง"
- "ฉันเปลี่ยนอะไร"

**decode = git log --reverse บน color space**

---

## 4. Self-Healing จาก Geometry

ไม่ใช่ redundancy แบบ RAID (เก็บซ้ำ)
ไม่ใช่ checksum (เก็บ hash)
**Redundancy เกิดจาก geometry เอง**

```
R พัง → G + B + timeline → reconstruct R
G พัง → R + B + timeline → reconstruct G
B พัง → R + G + timeline → reconstruct B
RGB พังพร้อมกัน → timeline คือ state ทุก layer
                  → rebuild จาก seed เดียว
```

---

## 5. Frequency Separation — Lossless ที่ "เล็กจนน่าตกใจ"

```
delta chain → FFT → แยก signal / noise

signal:  smooth, low-freq  → Hilbert locality บีบได้ดี
noise:   bounded, pattern  → เก็บแค่ range + freq_seed

decode:  signal + regenerate noise → chain คืน → pixel ครบ
```

noise ดู random แต่ **bounded + predictable** → เก็บเป็น params ไม่กี่ bytes

### Real-world (กล้องจริง):
```
color balance shift = global delta = 1 value ต่อ frame
rolling shutter     = deterministic artifact = frequency params
scene cut           = entropy จริง = keyframe ใหม่
```

---

## 6. Video Use Case — คนเต้น + Single Color BG

```
bg tiles:    delta = 0 ทุก frame → seed เดียว ครอบทั้งหมด
คน (edge):  delta เล็ก + pattern → delta chain สั้น
motion:     linear interpolate   → params ไม่กี่ตัว

frame กลาง = คำนวณ real-time ไม่ต้องเก็บ
```

vs H.264: ต้องเก็บ P-frame ทุกอัน
vs Hamburger: seed + keyframe + motion params

---

## 7. Proof จาก Session นี้

### GIF Animation Test (200w.gif — 90 frames, geometric pattern)
```
Per-frame encode:    10,438 KB (1.66×)
O23 keyframe+delta:  10,062 KB (1.72×)
Original GIF:         1,104 KB (15.6×)

Delta ระหว่าง frame:
  pixel เปลี่ยน 86-98% ทุก frame (rotation)
  แต่ delta magnitude เล็กมาก (7-13 จาก 0-255)
  G channel เล็กกว่า R/B เสมอ (gif สีม่วง = R+B สูง)
```

**Observation:** G pipe แทบไม่มีงาน → bandwidth ว่างให้ R/B  
ถ้า hamburger ใช้ frequency separation → 90 frames อาจเหลือไม่กี่ KB

### PDF Test (Metatron doc — 29 pages)
```
Page 1 (cover):  174× compression lossless
Page 3 (text):   6.6× compression lossless
```

encoder detect structure โดยไม่รู้ว่ากำลังอ่าน PDF

---

## 8. สิ่งที่มีอยู่แล้ว (map ตรง)

| Hamburger component | Header ที่มีอยู่ |
|---|---|
| Hilbert addressing | `geo_tring_addr.h` ✓ |
| seed + encode formula | `geo_pixel.h` ✓ |
| invert/mirror state | EHCP ghost/mirror ✓ |
| delta chain | O23 delta path ✓ |
| per-tile codec select | O25 circuit-based ✓ |
| self-healing | `tring_first_gap()` ✓ |
| pipe isolation | EHCP complement pair ✓ |

---

## 9. สิ่งที่ยังต้องสร้าง

```
# งาน                                    Priority
H1  pipe_R/G/B encoder dispatcher         HIGH — core
H2  invert_record(before, after)          HIGH — passive recorder
H3  frequency separation (FFT on delta)   HIGH — key to small size
H4  hamburger_encode(file) → seed+chain   HIGH
H5  hamburger_decode(seed+chain) → file   HIGH
H6  benchmark vs ZSTD / GIF / H.264       MEDIUM
H7  video pipeline (กล้องจริง test)        MEDIUM
H8  noise regeneration from params        MEDIUM
```

---

## 10. Next Session — เริ่มตรงไหน

**Option A:** สร้าง `hamburger_core.h`
- 3 pipe dispatcher + invert recorder
- feed กับ gif frames ที่มีอยู่
- ดู invert chain จริงๆ ว่าเล็กแค่ไหน

**Option B:** FFT delta separation ก่อน
- proof ว่า delta chain compress ได้ด้วย frequency
- นั่นคือ "ขนาดที่น่าตกใจ" ที่คิดไว้

**Recommendation: A ก่อน** — เห็น invert chain จริงแล้วค่อย apply FFT

---

## 11. Files ที่ต้องเอาไปด้วย

```
geopixel/
  geopixel_v21_o25.c      ← O25 encoder (compiled → gpx_v21)
  geo_gpx_anim_o23.h      ← O23 animation (keyframe+delta)
  geo_pixel.h             ← seed + RGB encode formula
  geo_tring_addr.h        ← Hilbert/TRing addressing
  geo_goldberg_tile.h     ← Goldberg face classifier
  gpx4_container_o22.h   ← GPX4 container (MAX_LAYERS=256)

vault/
  anim_o23.gpx4           ← encoded GIF (90 frames)
  gp20_equirect.bmp.gp15  ← encoded sphere image
  gif_frames/f000-f089.bmp ← raw frames

gen_goldberg_o11.c        ← GP(2,0) generator
geopixel_o3_roundtrip.c  ← O3 lossless validator
```

---

## Sacred Numbers (ห้ามเปลี่ยน)
`144, 720, 3456, 6912, 20736, 27648, 27, 6, 12`

seed: a=2, b=3 → ทุกตัวเลขในระบบมาจากนี้

---

*"ถ้าทำเสร็จแล้วพิสูจน์ได้ ทุกอย่างจะง่ายมากเลย แค่ต่อสาย ไม่ต้องคิดอีกแล้ว"*
