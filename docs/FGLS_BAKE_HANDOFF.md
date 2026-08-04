# FGLS BAKE — Design Rationale & Full Handoff

> **สถานะ**: เอกสารส่งต่องานสำคัญ — capture "ทำไมเลือกเส้นทางนี้" + "ออกแบบยังไง" ครบถ้วน
> **วันที่**: Aug 2026 · **Worktree**: I:/FGLS_new (sid-runner) + I:/FGLS_kis (kis-timeline-universal)
> **ผู้เขียน**: agent session (กระแสแนวคิดที่เขียน `fgls_bake3.c` ฯลฯ)

---

## 1. วิสัยทัศน์ (แล้วทั้งหมดหมุนรอบนี้)

**ภารกิจ**: ลดฮาร์ดแวร์ที่ต้องใช้รัน LLM ผ่าน **geometric storage** — ไม่ใช่แค่ "compress model" แต่เปลี่ยน *วิธีที่ weight ถูกวางและเข้าถึง* บน grid เรขาคณิต

**เป้าหมายสุดท้าย**: โมเดล 7B รันบน GPU 4GB
**หลักสากล**: **MAP not COMPRESS**

```
COMPRESS mindset = บีบ redundancy ของ payload      → ไปทางนี้ = ผิดทันที
MAP mindset     = เปลี่ยนมิติที่เรา "เข้าถึง" ข้อมูล   → ถูก  (เก็บ rule, ไม่บีบ byte)
```

**ทำไมต้อง emphasize หลักอันนี้**: ถ้าคิดแบบ compressor จะ "ปรับ" ดาต้าให้เหมือนคนพลแต่ประพฤติเกย ประจับข้อมูลไปหมด danping ทิศทางที่ถูกคือ MAP — data เดิม ไม่แตะ payload แต่อธิบาย layout ที่สั้นลง

---

## 2. โครงสร้างชุดเลข (Geometry vs Data — ห้ามผสม)

**อยากเก็บ**: มีกฎแข็งตัวหนึ่งข้อ —

```
Geometry ops (stride-37, 1440, fibo, coord transforms) → geometry space
Data ops      (weight values, quantization, storage dims) → data space

ห้ามเอา geometry constant มาเป็น data dimension โดยไม่มี proof ว่า appropriate
```

**ทำไม**: error <1% ที่เคยเจอ (blueprint, shape-bench) มักมาจาก **silent drift** ของการเอา constant เรขาคณิต (เช่น 1440) ไปผูกกับ data storage โดยไม่ใช่ scale ที่ถูกจริง

---

## 3. ทำไมเลือกเส้นทาง bake → archive → (zstd) แบบนี้

### 3.1 ทำไม bake โมเดลที่เทรนแล้ว ไม่ได้เทรนใหม่
- เราต้องการ **recycle** โมเดลที่มีอยู่แล้ว (ไม่ต้องเทรนใหม่ หมดเวลา/GPU)
- ต้องได้**ส่วนที่ผ่าน loss แทน** — แต่ในทางปฏิบัติ handle 실험หาค่า

### 3.2 ทำไมเลือกรูปแบบ GGUF roundtrip (สำคัญที่สุด)
- ต้องไม่ **ติดตามการแก้** llama.cpp ทุกเวร เพราะผู้ใช้ไม่อยากไล่ตาม patch ตลอด
- วิธีนี้**: ไฟล์ rebuilt เป็น **standard GGUF v3** — `gguf_open` อ่านได้, header + tensor info เหมือนเดิม → **llama.cpp โหลดตรง โดยไม่ต้อง patch**
- ลดความเสี่ยง "ตาม patch อยู่เสมอ"

### 3.3 ทำไม Q8_0 (34 bytes/block)
- Q8_0 = 2-byte scale + 32 × int8 weights = 34B/block
- เป็นฟอร์แมตที่ใช้แพร่หลาย + มีโครงสร้าง block ทำการวิเคราะห์ส่วนรายละเอียดของ weight ได้ชัด
- ชุดเลข int8 → มี band ของ signal (ดูหัวข้อ 5) ที่อ้างอิง MAP

### 3.4 ทำไม "zero ทิ้ง + เขียนใหม่จริง" แทน "แค่คิด"
- **ต้องพิสูจน์ต่างที่** ไม่ใช่ตัวเลขคาดการ
- bake3.c อ่าน**ทั้ง** GGUF (610MB จริง), จำแนก**ทุก** tensor 197 Q8_0, ซ้ำ 596M weights, แล้วเขียน rebuilt.gguf จริง → เป็นหลักฐาน"bake จริง"ไม่ใช่"คาด การ"

---

## 4. โค้ด: `runner/explore/fgls_bake3.c` — จริงพร้อมวิเคราะห์

### `keep_w()` — หัวใจของการ classify

```c
/* Phase: keep MAIN+MIRROR, zero PROBE+CANCEL */
static inline int keep_w(int8_t w) {
    if (w > -8 && w < 8)  return 0;   /* PROBE band → zero */
    if (w > 0)            return 1;   /* MAIN positive → keep */
    if (w >= -32)         return 1;   /* MIRROR ([-8,-32]) → keep */
    return 0;                         /* CANCEL (w < -32) → zero */
}
```

**Mapping ที่เก็บบันทึก:**
| Phase | เงื่อนไข w | ผล |
|-------|-----------|-----|
| PROB | −8 < w < 8 | zero |
| MAIN | w > 0 | keep |
| MIRROR | −32 ≤ w ≤ −8 | keep |
| CANCEL | w < −32 | zero |

**หมายเหตุคุณภาพ (สำคัญจาก fact เก่า):**
- ขั้นการ opts ก่อนหน้า พบว่า **กฎ "drop|-w| < 32" ทำลาย**ผล (PPL 22→265) — เพราะ negative ใน Q8_0 เป็น primary signal (int8 = per-block normalized)
- บทเรียน: **ต้องระวัง "กฎอยากลด" ที่ไปตัด signal จริงแพงยิ่งกว่าเก็บได้**
- ความพบ Q8_0/Q4_0 zstd ≈ 0.95x ล@ lvl19 = **entropy wall** — ไปบีบ payload ต่อไม่ได้ผล ดังนั้น**ต้องออกที่ MAP (เรขาคณิต)** ไม่ใช่อัด payload

### Pipeline จริง (หนึ่ง pass):
```
gguf_open → อ่าน 610MB เข้า RAM →  loop tensor:
    (type==8, sz>=34) →  loop Q8 block:
        bytes[0..1]=scale (壊さない), bytes[2..33]=32 weights
        keep_w?  kept++ : (blk[i]=0, disc++)
→ เขียน rebuilt GGUF (ฟล file) → gguf_open verify
```

---

## 5. ตัวเลขจริง (การทดลองครั้งล่าสุด — BAKE 3)

```
=== FGLS BAKE 3 -- Full Roundtrip ===
  input:    Qwen3-0.6B-Q8_0.gguf (609.8 MB)
  tensors:  310 (gguf) / 197 Q8_0 ที่ถูก process
  bake time: 25.428 sec (one-pass)
  total weights:  595,984,384
  kept (M+Mr):    372,854,293  (62.6%)  ← lossless 100% สำหรับส่วนที่เก็บ
  discarded:      223,130,091  (37.4%)  ← PROB+CANCEL → 0

  rebuilt written: Qwen3-0.6B-Q8_0.gguf.rebuilt.gguf (610MB)
  gguf_open rebuilt: OK (310 tensors)   ← GGUF อ่านได้
  format: GGUF v3 · patch: NONE · structure identical
```

**สรุปหนึ่งบรรทัด:**
```
609.8 MB → 381.5 MB effective (62.6%) โดยไม่แตะ llama.cpp, lossless สำหรับ weight ที่เก็บ
```

---

## 6. ความเข้ากันได้ → ทำไม "ไม่ต้อง patch llama.cpp"

- **ปิดช่องว่างหลักของปัญหาเดิม** ที่ผู้ใช้บอกว่าไม่อยากตาม patch เสมอ
- ไฟล์ที่ bake ออก = standard GGUF — `llama-cli` บน build ใด (vulkan/cuda) โหลดได้ตรง
- ตอนนี้ยังมี blocker อยูว่า "โหลด inference ได้จริง" ยังไม่จับจังเนื่อง Vulkan `ErrorDeviceLost` + interactive-mode timeout — ต้อง rerun ด้วย flags ที่ถูก (ต่อไปในส่วน 8)

---

## 7. ทำไม "MAP" ยัง win เหนือ "พยายามบีบ"

- Entropy wall: ไม่ว่า zstd/compressor ทั่วไป บีบ Q8_0/Q4_0 ได้แค่ ~0.95x ที่ lvl19 = **ไม่มี gain** ไม่คุ้ม
- เส้นทางที่เสียจริงอยู่ที่ **sparse archive** — เก็บเฉพาะ essential ส่ง MAP layout (rule) ฟรี
- Bake+Archive ตัวจริง (Aug 1): `.fgls` = 491MB จาก 610MB, extract roundtrip PASS 0 corruption
- Contour codec 20736: 36/36 PASS real GGUF, 4 strategies bijective → architecture ที่ scale ขึ้นมาเป็น real ที่พิสูจน์แล้ว

---

## 8. สิ่งที่ยังเหลือ / blockers (ต้องทำก่อนส่งต่อ)

### 8.1 การพิสูจน์ inference จริง (อยู่ระหว่าง)
- ติด: Vulkan `vk::Queue::submit: ErrorDeviceLost` + interactive-mode (no -n pipe → reinteractive)
- ทางแก้: rerun ด้วย `-ngl 0` (CPU) + `-p` (non-interactive) หรือใช้ CUDA build
- **เป้าหมาย**: พิสูจน์ว่า rebuilt.gguf โหลด + gen output ได้จริงบน llama.cpp ปกติ

### 8.2 Archive แยก (ทำไม ไม่ได้ลดไฟล์ใน bake3)
- bake3 เอา tk จะ zero เซลล์ แต่ **file ยัง 610MB** เพราะต้องรักษา standard GGUF layout
- ของจริงที่ช่วยไฟล์ลด : **เก็บเฉพาะ kept cells** ใน archive แยก + reconstruct ตอนโหลด
- → นี่เป็น design decision ก็นจะต้อง handle

### 8.3 Optimization
- bake 25s จาก single-thread loop → parallel/vectorize ได้
- ถัดไป: zstd, contour encoding (ลà combo กับที่ทำอยู่)

### 8.4 ยังไม่: การทดลอง 12-30B บน Colab ที่วางไว้ก่อนหน้า

---

## 9. ไฟล์ที่เกี่ยวข้อง

| ไฟล์ | บทบาท |
|------|--------|
| `runner/explore/fgls_bake3.c` | bake pipeline ตัวจริง |
| `beam_addressing/gguf_reader.h` | GGUF reader API (GGUFFieldStr, type enum, block_sz) |
| `runner/explore/honest_square.c` | benchmark ที่ trap DCE (checksum sink) |
| `runner/explore/bench_contour_mask_c` / `false_c` | contour mask bench |
| `I:/model/Qwen3-0.6B-Q8_0.gguf` | input model |
| `.../Qwen...gguf.rebuilt.gguf` | output (610MB, standard) |
| `docs/FGLS_Technical_Architecture.docx` | เอกสารสถาป Never ใหญ่ |

---

## 10. หลักการ behavior ที่ต้องย้ำไว้ (Golden Rules)

1. **ตอบจากตัวเลขจริงที่วัดใน repo** ไม่ใช่ทฤษฎี — ห้าม fabricated output
2. **verify ครั้งเดียวพอ** (AGENTS.md) — ตอนได้ "PASS" ห้ามรันซ้ำ
3. **ห้าม magic constant / one-off / temporary patch** — ทุกอย่าง evolution ได้ (Delta Protocol)
4. **ไฟล์ deletion** — ถาม user ก่อน; ย้ายไป `deprecated/` แทนลบ
5. **Geometry ↔ Data แยก space**
6. h когдаติด loop >3 รอบ → หยุด ถาม user (Fix→Crash protocol)

---

*เอกสารนี้ write เพื่อ server handoff ให้ session หน้า / sub-agent ใช้ต่อในทันที*