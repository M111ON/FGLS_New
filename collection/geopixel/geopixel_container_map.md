# GeoPixel Container — Architecture Map
> Session handoff document — อ่านก่อนเริ่ม session ใหม่ทุกครั้ง

---

## สถานะปัจจุบัน (v21 / O25)

**มีแล้ว:**
- `geopixel_v18+.c` — encode/decode อิสระ, classify tile เป็น FLAT/GRADIENT/EDGE/NOISE ตาม avg_var
- Goldberg sphere geometry, TRing address encoder
- Blob dedup via stamp hash

**ยังไม่มี:**
- Dashboard frame (LUT)
- Container format ที่รวม dashboard + data layers
- Invert Hilbert recorder
- Transmitter (ฝั่ง input — ทำทีหลัง ต้อง match spec นี้)

---

## Core Concept

```
ไฟล์ทั้งหมด = 1 seed + encoder-per-lane + delta-per-layer

seed      = root กลาง (ไม่เปลี่ยน)
lane      = characteristic type ของ tile นั้น (ไม่ใช่ RGB จริงๆ — เป็น data type label)
encoder   = เลือกตาม lane ของ tile นั้น (tile คนละที่อาจได้ encoder เดียวกันโดยบังเอิญ แต่ไม่รู้จักกัน)
core area = ส่วนที่เหมือนกันทุก layer → บีบได้ทันที
boundary  = delta ต่อ layer → เก็บแค่ความต่าง
decode    = วิ่งออกจากกลาง + ใส่เปลือกกลับ (เหมือน git diff: core=base commit, boundary=patch)
```

**Key invariant:** encoder ไม่ข้ามชั้น, lane ใครlane มัน, isolate โดย design

---

## Container Structure

```
[GPX File]
├── Frame 0 : Dashboard (LUT)
│   ├── Header  : magic, version, tile_count, layer_count, seed
│   └── Entries : tile_id × N
│       ├── char_type      (FLAT/GRADIENT/EDGE/NOISE — from existing classify)
│       ├── encoder_id     (which encoder to use for this tile)
│       ├── seed_local     (derived from global seed + tile geometry)
│       └── hilbert_entry  (start point ใน invert hilbert สำหรับ tile นี้)
│
└── Frame 1..N : Data Layers
    ├── Core area  : shared identity ทุก layer (compress ร่วมกัน)
    └── Boundary   : delta per layer (displacement map / bumpmap)
```

---

## Dashboard — Two Sides

```
FRONT (LUT side):
  tile_id → { char_type, encoder_id, seed_local, hilbert_entry }
  GeoPixel อ่านตรงนี้ก่อน → รู้ทุกอย่าง → random access ได้เลย ไม่ต้อง decode ทั้งไฟล์

BACK (Chain side):
  tile_id → hilbert_entry → seed_local → encoder_id
  derive จาก geometry ล้วนๆ → ไม่มี index แยก → deterministic + reversible
```

**ความสัมพันธ์:**
```
global_seed + tile_geometry → seed_local   (derive, ไม่ store)
seed_local  + tile_position → hilbert_entry (derive, ไม่ store)
char_type   → encoder_id                   (lookup, store ใน dashboard)
```

---

## Tile Classification (มีแล้วใน v18)

```c
avg_var < 16    → TTYPE_FLAT      → BMODE_NONE
avg_var < threshold → TTYPE_GRADIENT → BMODE_GRAD9 (NORMAL/LOOSE ตาม n_circuits)
avg_var < 1000  → TTYPE_EDGE     → BMODE_DELTA
else            → TTYPE_NOISE    → BMODE_DELTA
```

Dashboard ใช้ output ของ classify นี้เป็น char_type — ไม่ต้องสร้างใหม่

---

## Invert Hilbert Recorder + fibo_clock

```
แนวคิด: เก็บ "เส้นที่ไม่เดิน" แทน "เส้นที่เดิน"
→ ถ้า tile มี entropy ต่ำ (สีเดียว) hilbert ไม่เดิน → delta ≈ 0 → free compression
→ inverted = XOR กับ full grid → ไม่ต้องคำนวนใหม่
→ 1 plane grid 16×16 (256 positions) → ratio 1:3 ในกรณีที่ดี

role: บันทึก "สนาม" ของแต่ละ tile
      เป็น entry point ของ chain (back side ของ dashboard)
      seam โผล่เองจาก geometry — ไม่ต้อง hardcode (proof จาก Goldberg experiment)
```

**fibo_clock แทน index:**
```
ไม่เก็บ coordinate ของ "ที่ไม่เดิน"
→ ใช้ fibo_clock tick บอกว่า tick นี้ เดิน(1) หรือไม่เดิน(0)
→ hilbert position = derive จาก tick (deterministic, ไม่ store)
→ output = pure bit stream ตาม clock

tick N → hilbert_pos(N)  ← derive เสมอ ไม่ store
bit  N → 1/0             ← store แค่นี้

ไม่มี float, ไม่มี coordinate, ไม่มี invert HB #1,2,3 แยกกัน
fibo_clock wire เข้าตรงนี้ได้เลย — มีอยู่แล้วในระบบ POGLS
```

---

## Transmitter (ยังไม่สร้าง)

```
ต้นทางยังไม่ได้เลือก
requirement เดียว: output ต้องตรง spec container นี้
→ scaffold receiver ก่อน แล้ว transmitter มาเจอกัน
input จริงๆ: compressed binary / vector data (ไม่ใช่ natural image)
→ มี pattern แต่คาดเดาไม่ได้
→ เหมือน QR แต่เป็น geometric data
```

---

## Block Structure — 17³ Fence

```
block  = 4896 bytes  (actual data)
fence  = 17 bytes    (drain gap — ไม่ store)
total  = 4913 = 17³  (geometric boundary)

ไฟล์ใหญ่ = block เพิ่ม, fibo_clock เดินต่อ
ไม่มีจุดเชื่อมพิเศษ — clock tick ถัดไป = block ถัดไป
```



```
17³ = 4913  ← theoretical full cube (ไม่ store)
17 × 2 × 144 = 4896  ← actual storage (geometry fit)
4913 - 4896 = 17  ← drain gap = switch gate = ไม่มีจริง

8  = before state
9  = after state  
17 = during (atomic boundary — ไม่มีจริง = ไม่ store)
```

**Frustum connection:**
```
frustum = cone ที่ถูก clip
→ มี top (before) + bottom (after)
→ ส่วนกลาง = 17 = รอยตัด = drain gap = ไม่ store
→ เหมือน fibo_clock tick ที่ไม่เดิน
```

**ทำไมสำคัญกับ container:**
```
tile size → ผูกกับ 4896 หรือ factor ของมัน
fibo_clock + frustum drain → sync กันเองโดยธรรมชาติ
seam โผล่เองจาก geometry (proof จาก Goldberg experiment)
ไม่ต้อง hardcode boundary ใดๆ
```

**Sacred numbers ที่ใช้:**
```
4896 = file/tile size anchor
17   = drain gap (switch gate)
144  = bridge (Fibo ↔ Geometry)
54   = lane count (Rubik stickers)
12   = traverse operator / pentagon drain count
```



```
input data
    ↓
classify (char_type)
    ↓
encoder ที่เหมาะสมกับ lane
    ↓
hilbert traverse (driven by fibo_clock)
    ↓
bit stream: เดิน(1) / ไม่เดิน(0) ต่อ tick
    ↓
[ negative space bit stream = final storage ]

decode = invert กลับ → รู้ว่าเดินตรงไหน → reconstruct
storage size ∝ density ของ pattern (sparse → เล็กมาก, dense → ใกล้ original แต่ไม่เกิน)
```

 (Priority Order)

| # | Task | Detail | Blocker |
|---|------|---------|---------|
| 1 | `gpx_container.h` | magic bytes, header struct, frame layout | none |
| 2 | Dashboard encoder | เขียน LUT จาก classify output | container done |
| 3 | Invert Hilbert | recorder per tile, entry point derive | dashboard done |
| 4 | Layer packer | core area separation + boundary delta | hilbert done |
| 5 | Random access API | GeoPixel อ่าน dashboard → seek ตรง | layer done |
| 6 | Transmitter spec | define input format ให้ต้นทาง | design stable |

---

## Open Questions

| คำถาม | Priority |
|--------|----------|
| tile size: fixed หรือ variable? ถ้า variable → index เก็บยังไง | HIGH |
| seed_local derive formula: `hash(global_seed ^ tile_id)` หรือ geometry-based? | HIGH |
| core area boundary: กำหนดยังไงว่าส่วนไหน "เหมือนกัน" across layers | MEDIUM |
| hilbert grid size per tile: 16×16? หรือ adaptive ตาม tile size | MEDIUM |
| container extension: `.gpx` หรือ versioned `.gp{N}` | LOW |

---

## Files ที่เกี่ยวข้อง

```
geopixel_v21_o25.c      ← encoder/decoder หลัก (v18 content)
gen_goldberg_o11.c      ← Goldberg sphere geometry generator
o11_seam_analysis.html  ← proof: seam โผล่เองจาก geometry
entropy_comparison.html ← tile entropy characteristic test
pogls_coord_wallet.h    ← pattern อ้างอิง: coordinate-only storage
```

---

---

## Handoff — เปิด Session ถัดไป

```
สถานะ: design phase เสร็จ, พร้อม implement
งานแรก: gpx_container.h — header struct + magic bytes + block layout
anchor: block=4896, fence=17, total=4913=17³
clock: fibo_clock (integer only, ไม่มี float)
scaling: ไฟล์ใหญ่ = block เพิ่ม, clock เดินต่อ ไม่มีจุดเชื่อมพิเศษ
ref file: geopixel_v21_o25.c (encoder/decoder หลัก — classify มีอยู่แล้ว)
```

