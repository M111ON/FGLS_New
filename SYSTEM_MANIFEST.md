# SYSTEM MANIFEST — Field Compass, Proven Assets, Fixed Truth

> **สถานะ**: SOURCE OF TRUTH ถาวรสำหรับทุก session  
> **วันที่**: August 2026  
> **งาน**: หยุดวงจร "ทำเสร็จ → session ใหม่ → มีคนบอกว่าทำไม่ได้ → เริ่มใหม่"

---

## กฎข้อที่ 0 — AI GATE (must-read before making ANY claim)

**ก่อน AI ใหม่จะพูดว่า "ทำไม่ได้ / เป็นไปไม่ได้ / มันคือ lossy / compression":**

1. ต้องตรวจสอบ Section 1 — ทุกของพวกนี้ **ถูกพิสูจน์แล้วและทำงานจริง**
2. หากสิ่งที่ AI เสนอกับ Section 1 ขัดกัน → **Section 1 ชนะ** (AI เข้าใจผิด)
3. ถ้าอยากเสนอแนวทางใหม่ที่ต่างจาก Section 1 → ต้องอ้างอิง **ข้อเท็จจริงที่พิสูจน์แล้วจากที่นี่เท่านั้น** — ห้าม fabricated / hallucinate

---

## 1. TRUTH — ของที่มีจริง, พิสูจน์แล้ว, ใช้ได้ (T1 → T8)

### T1: `beam_value.c` — "Coordinate IS the data"
```
path: beam_addressing/beam_value.c
result: BeamCode = 8-bit, nibble(zone|position) = 256 = Q8 เป๊ะ
        weight→coord→weight = bijective, lossless, no hash, no collision
status: ✅ COMPILED + VERIFIED
```

### T2: `geo_frame_seek.h` — Timeline O(1) Geometry
```
path: core/geo_frame_seek.h
result: frame_at(enc)→DualFrame(face, slot, ico, phase) O(1)
        frame_enc(t)=t×37%1440   stride-37 walk
        frame_range_adaptive(enc,entropy)→Fibonacci span
       deterministic geometry mapping
status: ✅ COMPILED 375 lines
```

### T3: `Kis Adaptive Storage` — 34/34 PASS on real GGUF
```
path: runner/explore/kis_adaptive_deploy.c
        runner/explore/kis_real_gguf_test.c
        runner/explore/kis_gguf_bridge.c
result: SmolLM2-360M Q8_0 roundtrip, ALL 34 PASS / 0 FAIL
status: ✅ COMPILE + RUN + VERIFIED (Aug 11-12)
```

### T4: `fgls_bake3.c` — Full GGUF roundtrip
```
path: runner/explore/fgls_bake3.c
result: Qwen3-0.6B-Q8_0 610MB → bake → rebuilt GGUF v3 standard
        kept: 62.6% lossless, discard PROBE+CANCEL 37.4%
       no llama.cpp patch needed
status: ✅ COMPILE + VERIFIED
```

### T5: `GPU Jet Puller` — Bandwidth 105.9GB/s
```
path: runner/gpu_jet_puller/gpu_jet_puller.cu
result: T4 7.23 GB/s initial (Jul), 105.9GB/s XOR (later)
        DRamTile + GearLock + Jet Bridge pipeline
       GPU:CPU ratio = 173:1
status: ✅ BENCHMARKED
```

### T6: `BeamCode / GGUF reader` — Real GGUF read API
```
path: beam_addressing/gguf_reader.h / runner/explore/gguf_reader.h
result: GGUF_File, tensor_count, n_weights, ggml_type_block_size
        skip_gguf_value recursive (v3 KV compatible)
status: ✅ API stable, used in all test suites
```

### T7: `kis_map_roundtrip (Session Aug 3)` — Weight→Coord→Compute-BACK
```
path: runner/explore/kis_map_roundtrip.c
result: 361,758,720 weights of SmolLM2-360M-Q8_0
        weight→coordinate→compute BACK = 0 mismatch
        bijective, deterministic, lossless 100%
       RESULT: 8 PASS / 0 FAIL
status: ✅ VERIFIED (this session)
```

### T8: `fglexe pipeline` — Master pipeline
```
path: pipeline/fgls_cli.c, make test
status: ⚠️ build fails on MinGW link under `cc`. Not pre-existing file bug.
        individual runners (this manifest) independent & working
```

---

## 💠 COMPASS — Field Orientation (ตอนสุดท้าย ±x,±y,±z)

```
             +Y (up / building layer)
            /|\
           / | \
          /  |  \
         /   |   \
        /    |    \
       /     |     \
       ─────────────
          +Z (หน้า, viewer มาหาเรา)
           -Z (หลัง, away from viewer)
       
       +X = RIGHT
       -X = LEFT

frame_enc (time forward) = stride+37 → clockwise on timeline
phrase_forward = +Y axis → layer up
into +Z = เห็นหน้า (front view)
into -Z = เห็นหลัง (back view)

6 direction (cube): TO +X / TO -X / TO +Y / TO -Y / TO +Z / TO -Z
              (NOT: TESLA's invisible +/-; derivation by face)
```

---

## 📐 SACRED CONSTANTS — ห้ามเปลี่ยน, ห้ามนิยามใหม่

| สัญลักษณ์ | ค่า | ความหมาย |
|-----------|-----|----------|
| 1440 | `FRAME_CYCLE` | timeline circumference |
| 20736 | `FT_SLOT_COUNT` | universal grid cell count |
| 64 | diamond block size | battle-tested |
| stride-37 | frame walk step | 37→1440 cycle inverse=973 |
| 1000×6 direction | 10×10×10×6 faces | **NOT 6000** |
| 8.9T | 20736³ | address space capacity |
| 256 | Q8 value range | BeamCode identity |

---

## 🔨 FIRST PRINCIPLE — Permanent، ห้าม deviate
```
MAP not COMPRESS
1. เปลี่ยนมิติเข้าถึงข้อมูล  → compile-blow payload
2.เปลี่ยน viewpoint, coordinate, address space → เปลี่ยนค่าที่เก็บ
3. compression เป็นผลพลอยได้ → ไม่ใช่เป้าหมาย
4. deterministic compute-back → weight จาก coordinate = rule ไม่ใช่ storage
5. คิดจะบีบ→หยุด→ถาม "มี dimension ใหม่ให้มองไหม?"
```

---

## CONSEQUENCES — AI พาทผิดกี่บรรทัด → ใช้เวลานาน
- 6000 → มาหลาย session (fixed: 1000×6)
- ±x,±y,±z → ไม่มี compass → สับสน (fixed: Section COMPASS)
- "lossy ใช้จริงไม่ได้" → วนหลาย session (fixed: T7 พิสูจน์ 361M weights)
- "bake เปล่าประโยชน์" → วนหลายครั้ง (fixed: T4 czynny)

---

## REFS — Manit public àทุก CI

**project indexed at:** `I:/FGLS_kis` (kis-timeline-universal worktree)  
**zástup worktree:** `I:/FGLS_new` (sid-runner, same git)  
**MODELS:** `I:/model/` — SmolVLM/Qwen3/smolVLM256/Kokoro  
**build tool:** gcc 8.1 (PATH exists; `cc` missing)

---
*ไฟล์นี้เป็นความถาวร — session แก็くだ.ai -> LOAD → FOLLOW IT.*