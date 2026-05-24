# HANDOFF — Diamond Shell Codec + Metatron Reshape Concept
_Session 2026-05-15_

## สถานะ: PROVEN (ของที่ work จริง)

### ✅ 8 executables ทดสอบผ่านทั้งหมด บนไฟล์จริง

| Executable | Result | สิ่งที่พิสูจน์ |
|-----------|--------|-------------|
| `test_frustum_trit.exe` | 10/10 PASS | Trit decomposition (addr^value)%27 → coset/face/level/slope |
| `test_frustum_slot64.exe` | 7/7 PASS | 54-slot O(1) store, 64B per slot, addr wrapping |
| `test_lossless_minimal.exe` | 64/64 PASS | Cover-only (552B) reconstruct → geometry derive จาก slot index |
| `test_codec_lossless.exe` | 7/7 PASS | Encoder + Decoder roundtrip (synthetic + edge cases) |
| `bench_shell_realfiles.exe` | ✅ lossless | 7 real files, FLAT/DEDUP/FULL, shuffle test |
| `debug_mismatch.exe` | ✅ mismatch found+fixed | Classify bug: fibo_intersect ไม่เหมาะเป็น content classifier |

### ✅ Lossless บนไฟล์จริง (ทุก chunk roundtrip)

| ไฟล์ | N chunks | Ratio | Dedup rate |
|------|----------|-------|-----------|
| C source (`test_diamond_flow_grouping.c`) | 479 | 1.13x | 398/479 |
| C header (`fibo_layer_header.h`) | 182 | 1.01x | 173/182 |
| Markdown (`Pogls_full pipeline.md`) | 566 | 0.97x | 566/566 |
| Markdown (`HANDOFF.md`) | 74 | 0.97x | 74/74 |
| Binary EXE (`debug_check.exe`) | 1043 | 1.05x | 958/1043 |
| Compressed zip (`fgls_handoff_fibolayer.zip`) | 667 | 0.97x | 667/667 |
| Fold header (`pogls_fold.h`) | 324 | **1.21x** | 247/324 |
| Diamond field (`geo_diamond_field.h`) | 462 | 0.99x | 452/462 |

### ✅ Shuffle test: drop ~0% (stateless จริง)

---

## Architecture (ของที่มีอยู่แล้ว)

### 1. Diamond Shell classifier + rotation scan
```
64B chunk → 6× rotate64 → fold_fibo_intersect → pick best_rot
         → classify by actual zero-byte count:
             FLAT(zero_cnt==64)  → 2B
             DEDUP(found in idx) → 10B
             FULL(unique)        → 66B
```

**Files:**
- `Diamond_shell_encoder/diamond_shell_v2.h` — rotation + classify API
- `Diamond_decode_hamburger/diamond_shell_codec.h` — serializer + decoder + inverse_rotate

### 2. Frustum trit addressing (Metatron grid proof)
```
addr + value + fibo_seed → TritAddr
  trit   = (addr ^ value) % 27
  coset  = trit / 3
  face   = trit % 6
  level  = trit % 4
  letter = addr % 26
  slope  = fibo_seed ^ addr  → recover addr = slope ^ fibo_seed
```
**File:** `Diamond_decode_hamburger/frustum_trit.h`

### 3. Frustum slot store (54 × 64B)
```
slot_idx = addr % 54  → O(1) random access
fstore_silence(coset)  → hide data by coset mask
```
**File:** `Diamond_decode_hamburger/frustum_slot64.h`

### 4. Cover-only reconstruction
```
Cover (552B) = FiboLayerHeader(32B) + seeds[64](512B) + pad(8B)
slot_face(i)    = i % 12
slot_channel(i) = slot_face(i) % 3
invert[ch]      = XOR of all seeds in channel
```
**File:** `Diamond_decode_hamburger/test_lossless_minimal.c`

---

## Bugs Found และ Fixed

### Bug 1: Classify โดย fibo_intersect → FLAT false positive
**สาเหตุ:** `_shell_chunk_to_block()` เขียนทับ core.raw ด้วย geometry metadata (face_id, engine_id, vpos, fibo_gear, quad_flags) — **เหลือ data residue แค่ 16 bits** → `fold_fibo_intersect()` ไม่ได้สะท้อน content จริง
**Fix:** classify FLAT/SPARSE/DENSE จาก actual zero-byte count ของ rotated buffer ใช้ fibo_intersect แค่เลือก best rotation

### Bug 2: FNV64 seed collision in dedup
**สาเหตุ:** FNV64 hash ตรง แต่ content ต่าง → decode ได้ chunk ผิด
**Fix:** `sg_verify()` ตรวจทั้ง seed + `memcmp(chunk, 64)` ก่อน dedup

### Bug 3: Flag decode ambiguity
**สาเหตุ:** encode ไม่ distinguish ระหว่าง dedup (10B) vs full (66B) — decode อ่าน flag ไม่ตรง
**Fix:** `0=FLAT, 1=DEDUP(seed), 2=FULL(64B)`

---

## สิ่งที่ยังไม่ Proven (ต้องทำต่อ)

### Priority 1: Sparse / Delta encoding สำหรับ unique chunks
- ปัจจุบัน unique chunk → 66B (แพ้ raw 64B)
- ต้องมี: delta encode (consecutive XOR), zigzag + Rice, หรือ position+value sparse encoding
- ถ้าทำได้ data ที่เป็น unique จะโดน 2-4x แทน 0.97x

### Priority 2: Batch mode (L1/L2/L3)
- v1/v2 มี concept: L1=8chunks, L2=64, L3=512 → 5B/chunk
- ต้อง validate lossless + ratio

### Priority 3: Multi-solid projection (Metatron grid)
- ปัจจุบันใช้แค่ Cube (6 rot)
- ต้อง expand `fold_build_quad_mirror()` จาก 4×byte-rotate → polyhedral projection matrix
- `pogls_atomic_reshape.h` expand จาก 6-state → multi-solid
- `_shell_rotate64` expand จาก 6 orientations → 4/8/12/20 projections

### Priority 4: Decoder สำหรับ batch + sparse mode
- ปัจจุบันมีแค่ FLAT/DEDUP/FULL decoder
- ต้องเพิ่ม: sparse decoder, delta decoder, batch reconstruct

### Priority 5: Reshape = append-only delete
- Concept: 27 visible / 27 dark side (coset silence)
- ต้อง implementation จริง + test กับ FrustumStore

---

## Key Files (for next session)

| File | Path | Lines |
|------|------|-------|
| `diamond_shell_v2.h` | `Diamond_shell_encoder/` | 414 |
| `diamond_shell_codec.h` | `Diamond_decode_hamburger/` | 188 |
| `frustum_trit.h` | `Diamond_decode_hamburger/` | 75 |
| `frustum_slot64.h` | `Diamond_decode_hamburger/` | 74 |
| `bench_shell_realfiles.c` | `Diamond_decode_hamburger/` | 262 |
| `test_codec_lossless.c` | `Diamond_decode_hamburger/` | 70 |
| `test_lossless_minimal.c` | `Diamond_decode_hamburger/` | 68 |
| `pogls_atomic_reshape.h` | `active_updates/` | 287 |
| `pogls_fold.h` | `core/pogls_engine/twin_core/` | (canonical source) |

## Build Command (สำหรับ next session)
```powershell
gcc -O2 -I. -I..\Diamond_shell_encoder ``
    -I..\..\..\..\core\pogls_engine\twin_core ``
    -o bench_shell_realfiles.exe bench_shell_realfiles.c -lm
```
