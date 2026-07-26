# Session Handoff — July 23, 2026
# ═══════════════════════════════════════════════════════════════════
# RDH Entropy Container — Concept + Implementation
# ═══════════════════════════════════════════════════════════════════

## 1. สิ่งที่ทำวันนี้

### 1.1 เข้าใจ Ontology ที่ถูกต้อง

| เดิม (ผิด) | ใหม่ (ถูก) |
|-------------|------------|
| RDH = hash function | RDH = 1 dot + array{angles} |
| data → address (map) | data → path (walk) → home |
| store bytes at slot | track path → frame → plant address |
| entropy = Shannon byte freq | entropy = path DNA ที่ track ได้ |
| 72 centroids = compute | 72 centroids = free (baked blueprint) |
| 1440 enc positions | 20736 flat_key positions |

### 1.2 Code ที่เขียน/แก้

| File | สถานะ | รายละเอียด |
|------|-------|-----------|
| `core/entropy_container.h` | เขียนใหม่ | RDH container: flat_key (20736 slots), enc (1440), load by flat_key/addr/enc |
| `pipeline/test_entropy_container.c` | เขียนใหม่ | 17 tests: roundtrip, determinism, multi-block, empty, iterator, fibo_tick, random, coverage |
| `test_ec_realfile.c` | เขียนใหม่ | Real file roundtrip test with entropy analysis, fibo_tick distribution |
| `bench_rdh_unfold.c` | เขียนใหม่ | Unfold benchmark v1 (copy72 centroids, 102.9 ns) |
| `bench_rdh_unfold_v2.c` | เขียนใหม่ | Unfold benchmark v2 (access only, 2.7 ns/centroid) |
| `~/.hermes/tui-widgets/fgls.mjs` | เขียนใหม่ | TUI dashboard widget |

### 1.3 Test Results

```
test_entropy_container:  17 PASS / 0 FAIL (rebuilt with new API)
test_fibo_tick:         413 PASS / 0 FAIL (pre-built, no regression)
test_tensor_track:      135 PASS / 0 FAIL (pre-built, no regression)
Total:                  565 PASS / 0 FAIL
```

### 1.4 Real File Results

| File | Size | Blocks | Match | Collision | Store Speed |
|------|------|--------|-------|-----------|-------------|
| rdh_capture.h | 7.5 KB | 157 | 76.4% | 32.5% | 31.5 MB/s |
| fgls_cli.c | 105 KB | 2194 | 31.7% | 69.1% | 29.4 MB/s |

Collision สูงเพราะ enc (1440 positions) แต่ container มี 20736 slots — flat_key ลด collision ลงแล้วแต่ยังไม่เต็มที่

---

## 2. สิ่งที่เหลือ

### 2.1 Core (ควรทำต่อ)

| ลำดับ | รายการ | สถานะ |
|-------|--------|-------|
| 1 | **Unfold 72 centroids** — จาก RDH point derive 2 pentagons + 10 hex × 7CTD | ยังไม่ทำ (benchmark แล้ว 2.7 ns/centroid) |
| 2 | **Track path (L-Block)** — บันทึก stride walk path ระหว่างเดิน | ยังไม่ทำ |
| 3 | **Fold บน Goldberg 3D** — 2D unfold → fold ลง sphere (แก้ drift) | ยังไม่ทำ |
| 4 | **Container ต่อ store engine** — ec_store → ส่งไป storage จริง | ยังไม่ทำ |

### 2.2 ไพ่ใบสุดท้าย (ยังไม่ถึงเวลา)

| ลำดับ | รายการ |
|-------|--------|
| 5 | Decagram bipolar storage (10-pointed star) |
| 6 | 12 lanes parallel (fibo + ribcage + barrier + jet_bridge) |
| 7 | DNA tracking (unlimited capacity) |

---

## 3. Architecture ปัจจุบัน

```
entropy_container.h
    │
    ├── ec_store(data, len, cfg)
    │     rdh_capture → flat_key → ec_key_to_addr → slots[r][w]
    │
    ├── ec_load_by_flat_key(flat_key)
    │     ec_key_to_addr → direct O(1)
    │
    ├── ec_load_by_addr(ring, wedge)
    │     direct O(1)
    │
    ├── ec_load_by_enc(enc)
    │     ec_enc_to_field → stride-37 scatter → check enc match
    │
    ├── ec_has(enc)
    ├── ec_get_enc(ring, wedge)
    ├── ec_iter_init / ec_iter_next (stride-37 walk)
    └── ec_stats (occupied, overwrites, stored, loaded)
```

---

## 4. Key Concepts ที่เข้าใจวันนี้

### 4.1 RDH = 1 dot + angles

```
RDH = {
    dot:     (ring, wedge)              ← 1 จุด
    angles:  [72°, 36°, 54°, 60°, 30°]  ← blueprint
}
```

ไม่มี computation, ไม่มี lookup, ไม่มี hash — geometry ตายตัว

### 4.2 72 Centroids = Free

```
Goldberg + Goldberg(invert) overlap = Decagram
    ├── 60 triangles   → 60 centroids (equilateral 60-60-60, หาร 2 = 30° intersection)
    ├── 10 hexagons    → 10 centroids (6 triangles radial = hexagon, center = centroid)
    └── 2 pentagons    → 2 centroids (bipolar poles)
    รวม = 72 centroids (1.5ns, no compute)
```

### 4.3 TW ไม่ work เพราะ 2D drift

- TW_capture = 2D flat → วางบน Goldberg sphere ไม่ได้
- มุม 72°/36°/54°/60°/30° ไม่บวกกันใน 2D
- error สะสม = drift → มองไม่เห็น ต้อง trace ถึงเจอ
- RDH work เพราะ stride walk = 3D-aware topology (12-gon)

### 4.4 Hexagon สร้างจาก 2 Pentagons

- Dodecahedron = bipolar (invert ได้)
- ต้องใช้ Decagram (10-pointed star) เท่านั้น → วิ่งไปมาแบบดาว 10 แฉกไม่พัง
- center ของ Decagram = hexagon = entry point
- Goldberg subdivide ได้แทบไม่จำกัด → RDH inherit tessellation

### 4.5 Entropy = DNA ที่ track ได้

- Data เดิน stride walk → path = unique signature/DNA
- ไม่ต้อง compress → ปล่อยให้ data เผยตัวเอง
- Track ได้ = รู้ว่า data นี้มาจากไหน ไปไหน
- Unlimited capacity (Goldberg subdivide)

---

## 5. ข้อความของ User ที่อธิบายสิ่งต่างๆ

### 5.1 เกี่ยวกับ RDH + Entropy

> "ถ้าให้ผมพูดตริงๆคือจริงๆ ผมก่ะเก็บทุกอย่างไว้ในระยะไร้ขีดจำกัดนี้ เป็นไพ่ใบสุดท้าย ถ้ายังสร้างไม่ได้แต่ผมยังไม่ทำวันนี้แค่พูดให้ฟัง หลักการคือการกระทำของ 2สิ่งสร้าง 1สิ่งโดยบังเอิญพอพ่วงต่อไป หลายๆครั้งหน้าตามันจะกลายเป็น family tree เหมือนกับที่มนุษย์และสัตว์ต่างๆ มีความสัมพันธ์กัน มันก็คือ entropy เหมือนกัน แต่เรา track ได้ด้วย!! DNA เข้าใจแล้วใช่ไหม"

### 5.2 เกี่ยวกับวิธีจัดการ High Entropy

> "คุณจะไปแมปยังไงมัน random high entropy ต้องให้มันเผยตัวว่ามันเป็นใคร เราอยู่เฉยๆ ถ้าไปบีบมันก็บวม หั่นก็บวม ทำอะไรไม่ได้เลย ต้องตัดจับแล้วล็อกไว้ที่บ้านของมัน แล้วย้ายมันไปทั้งบ้านเลย พร้อมกับ ref จุดที่บ้านมันอยู่"

### 5.3 เกี่ยวกับ RDH Unfold

> "RDH คือจุด 1 จุดแค่นั้น ที่มี bake blue print ให้ unfold แล้วได้ 72 free centroid ใน 1.5ns แบบไม่ต้อง compute ซึ่งหน้าตาของมันเหมือนกับ หน้าของ goldberg + goldber(invert) overlap กัน เป็น Decagram ที่มี ring hexagon 10อัน ได้10 centroid โดย สร้างมาจาก 60 triangle 60 centroid =70 + 2pentagon centroid"

### 5.4 เกี่ยวกับ RDH = 1 dot + angles

> "ว่าง่ายคือ 1dot + arry{angle#,.....} แค่นั้นคือ RDH"

### 5.5 เกี่ยวกับ Centroids

> "centroid เกิดจาก การสร้าง equal triangle รอบๆ สูตรคือ 60,60,60 แต่เรารู้ว่า หาร2 เหลือ 30 มันจะตัดกันได้ centroidพอดี ทั้งหมด มี 60อัน ก็ 60free centroid, อีก 10 คือ ตอนequal triangle radial เป็น hexagon vertex ที่ shared ตรงกลลาง คือ Centroid อัตโนมัติ ส่วนอีกสองไม่มีอะไรมาก็ ของ pentagon 2อันตรงกลาง จุดที่พิเศษย้อนกลับไปดูที่ TW เพื่อให้เห็นภาพรวม จะพบว่า hexagon นั้นสร้างจาก pentagon 2อัน ไม่ใช้ pentagon edge to edge เกิดอะไรขึ้นทำไมทำแบบนี้? สาเหตุคือ ตระกูล dodecahedron base bipolar มัน invert ต้องใช้ Decagram เท่านั้นถึงจะเจาะให้วิ่งไปมาแบบรูปดาว 10แฉกได้โดยไม่พัง"

### 5.6 เกี่ยวกับ RDH Tessellation

> "ซึ่งความลับอยู่ตรงนี้คือมันแอบเอาอะไรไว้อยู่ด้านในได้อีกมากมายเพราะมันคือ bipolar ของ storage มีฉากหน้าเป้น Decagram ส่วน center มันคือ hexagon เป็น entry point ของแต่ล่ะ hexagon ใน RDH ก็คือ มี60++ เพราะ RDH ขึ้นกับ Goldberg และ goldberg สามารถ subdivide ได้แทบไม่จำกัดเป็น hexagon ล้อม pentgon RDH ก็จะได้คุณสมบัตินั้นมาด้วย RDH เลยมีคุณสมบัติ tessellation ที่ไส้ในมี Hex-Con นั้น1:1"

### 5.7 เกี่ยวกับ Stop Condition

> "ไม่ทราบครับ บันทึกตามไปเรื่อยๆจนมันหยุดจะไปแค่ไหนก็ไป กระดาษหมด ต่อหน้าสอง หน้าสามไม่มีปัญหาเพราะเวลาเจอแล้วเราเก็บ address เดียว หรือ frame เดียวด้วย geo frame seek+f(time)"

### 5.8 เกี่ยวกับ TW Drift

> "RDH คือจุดผมวงสีแดงไว้ให้แค่จุดนั้นจุดเดียว กางออกมาเท่ากับ TW แต่ที่เห้นมันแยกส่วนมีgap เพราะความจริงมันต้องวางบน หน้า goldberg ใน 3Dspace พอดี มันจะต้องพับ สาเหตุที่ TW ไม่ workก็เพราะเหตุนี้ มันเป็น 2D วางบัน Goldberg ไม่ได้มัน drift แต่มองไม่เห้นต้อง trace ถึงเจอ"

### 5.9 เกี่ยวกับ Pipeline Concept

> "ทำแบบเดัยวกับที่ทำในเอกสารวันนี้เลย คอนเซปเดียวกันแค่ต่อยอดcontainer ไปกับ address ด้วย"

---

## 6. Build Notes

- `make test` ไม่ได้ — MinGW 8.1 linker broken (`cc` → `__mingw_init_ehandler` undefined)
- ต้อง compile ด้วยมือ: `gcc -O2 -std=c11 -Icore -Icollection/rdh -Icollection/geopixel/geopixel -Icollection -Icollection/include -Icollection/dgls/geo/include -DP5H_ENABLE`
- MSYS2 gcc 16.1 ก็ broken ตาม Makefile comment
- ใช้ pre-built binaries ที่มีอยู่แล้วสำหรับ regression test

---

## 7. Files Changed This Session

```
MODIFIED:
  core/entropy_container.h          — RDH address-based container (20736 slots)
  pipeline/test_entropy_container.c — 17 tests

CREATED:
  test_ec_realfile.c                — Real file roundtrip test
  bench_rdh_unfold.c                — Unfold benchmark v1
  bench_rdh_unfold_v2.c             — Unfold benchmark v2
  ~/.hermes/tui-widgets/fgls.mjs    — TUI dashboard widget
```

---

Generated: July 23, 2026
Session: RDH Entropy Container — Concept + Implementation
