# Handoff: Debug DiamondBlock Integration

## สถานะปัจจุบัน

### ✅ Verified: Pure pipeline (D4 → Hilbert → seed-index → decode)

**ไฟล์:** `test_integrity_mini.c`  
**Result:** N=1721 → **pass=1721/1721 (100%)** ratio **8.0x**

Pipeline ที่พิสูจน์แล้วว่าถูกต้อง:
```
raw 64B → D4 canon (permutation tables) → Hilbert forward
  → FNV-64 hash → seed index (dedup by hash collision)
  → store 8B (seed pointer, no 64B payload)
  → decode: seed lookup → Hilbert inverse → D4 inverse → original
```

D4 inverse ใช้ **permutation table** (`D4_FWD[8][64]` + `D4_INV[8][64]`) — compose ได้ 100%.

### ❌ Buggy: DiamondBlock integration (encode_all/decode_all)

**ไฟล์:** `test_integrity.c`  
**Symptoms:**
- Batch N=1..100 → **pass** (encode_all + decode_all ถูกต้อง)
- Batch N=1721 → **0/1721 FAIL** (ทุก chunk decode ผิด)
- ไม่ใช่ stack overflow (เปลี่ยน stack → heap ก็ยัง fail)
- ไม่ใช่ hash collision (SIDX_CAP=65536, entries=1721, load=2.6%)

**Hypothesis:** Memory corruption ใน `encode_all()` ที่ N สูง  
** suspect #1:** `fold_block_init()` + `fold_build_quad_mirror()` + `diamond_flow_end()` — stack หรือ heap เขียนทับข้อมูลข้างเคียง  
** suspect #2:** `diamond_baseline()` — มี global static state ที่ corrode ระหว่าง iteration  
** suspect #3:** `DiamondBlock` padding/alignment mismatch (aligned(64)) กับ struct layout ปกติ

### Pipeline Comparison (ก่อน debug)

| Pipeline | seed-only | Ratio | Byte-exact |
|---|---|---|---|
| Content-hash D4+FNV (3-tier) | 14.7% | 1.547x | ✅ |
| Geometry DiamondBlock | **99.9%** | **7.963x** | ❌ (bug) |
| Pure D4+Hilbert+seed-index | ~99.9% | **8.0x** | ✅ (verified) |

## 3 Red Flag Tests (ต้อง fix ก่อนสรุป)

1. **Byte-level diff**: original vs decoded → 100% (Pure ✅, Geometry ❌)
2. **Shuffle test**: random chunk order → ratio ต้อง drop
3. **Cross-file test**: train A → test B → ratio != train ratio

## ไฟล์ที่เกี่ยวข้อง

| File | Status | Description |
|---|---|---|
| `test_integrity_mini.c` | ✅ verified | Pure pipeline 1721/1721 |
| `test_integrity.c` | ❌ buggy | DiamondBlock integration + 3 tests |
| `bench_roundtrip_diamond.c` | ⚠️ needs review | Content vs Geometry comparison |
| `bench_roundtrip_full.c` | ✅ verified | Standalone D4+Hilbert+seed-index |
| `fibo_tile_dispatch.h` | ✅ fixed | R stores full byte (lossless) |
| `fibo_layer_header.h` | ✅ | rotA/rotB/flow_fp/group_key |

## แนวทาง debug

1. **Minimize**: ลบ DiamondBlock ออกจาก `encode_all` ไล่เพิ่มทีละขั้น
2. **Address sanitizer**: `gcc -fsanitize=address` ถ้า toolchain support
3. **Valgrind** (ถ้า Linux): `valgrind ./test_integrity`
4. **Heap profiling**: ตรวจ `malloc`/`calloc` size กับ `sizeof(Encoded)` จริง
5. **Binary search**: หา N แรกที่ fail (ตอนนี้รู้แค่ว่า >100 fail)
