# HANDOFF — Diamond Field v3: Classifier v2 + Sparse + Batch + Rotation
_Session 2026-05-15_

## สถานะ: ALL PASS — 0 failures

### Test Results

| Component | Tests | Result |
|-----------|-------|--------|
| `tring.h` standalone | 10/10 | ✅ variable-size, gc_bitmap, gc_scan, stats |
| `geo_diamond_field.h` v3 | 10/10 | ✅ classifier, sparse, shell, encode/decode, delete, GC, reshape, multi |
| v3 + Rotation bench | 18/18 | ✅ 5 real files lossless + delete/GC + reshape + level distribution |
| **Batch mode (new)** | **4/4** | ✅ L1 roundtrip + L2 6.06 B/chunk + ratio + batch GC |
| **Total** | **42/42** | **ALL PASS — 0 failures** |

### Batch Compression Results

| Scenario | Batch Size | B/chunk | vs baseline |
|----------|-----------|---------|-------------|
| L1 synthetic (8 similar chunks) | 108B | 13.50 | 4.7× |
| L2 synthetic (64 similar chunks) | 388B | **6.06** | **10.5×** |
| L3 theoretical (512 chunks) | 4+64+512×5=2628B | **5.13** | **12.5×** |
| Real file (C source, L1 batches) | avg 108B | 13.50 | 4.7× |

### Architecture

#### Files

| File | Path | Lines | Role |
|------|------|-------|------|
| `geo_diamond_field.h` | `new_diamond_tring/` | ~450 | Shell + Slot + **Classifier v2** + **Sparse** + **Batch** — tring.h based |
| `tring.h` | `new_diamond_tring/` | 226 | Variable-size timeline ring |
| `bench_v3_integrated.c` | `new_diamond_tring/` | ~430 | Integrated bench: rotation + sparse + batch |
| `diamond_shell_v2.h` | `Diamond_shell_encoder/` | 414 | 6-DOF rotation scan, ShellBatch, `shell_batch_flush_v2` |
| `diamond_shell_codec.h` | `Diamond_decode_hamburger/` | 188 | Inverse rotation |
| `pogls_fold.h` | `core/pogls_engine/twin_core/` | 362 | DiamondBlock, fold_fibo_intersect |

#### Batch Wire Format

```
TringNode for batch:
  [header:4B]     — layer(1) + best_rot(1) + count(1) + flags(1)
  [base_chunk:64B] — byte-wise median of all K chunks
  [diff0:5B]      — diff for chunk 0
  [diff1:5B]      — diff for chunk 1
  ...
  [diff_{K-1}:5B] — diff for chunk K-1

5B diff encoding: [pos0:1B][val0:1B][pos1:1B][val1:1B][flags:1B]
  bit0=apply diff0, bit1=apply diff1
  Captures up to 2 differing bytes from base (lossless if ≤2 differ)

Total = 4 + 64 + K×5 = 68 + 5K bytes
Average = 5 + 68/K B/chunk
  L1(8):  13.50 B/chunk  (4.7× vs 64)
  L2(64): 6.06 B/chunk   (10.5× vs 64)
  L3(512): 5.13 B/chunk  (12.5× vs 64)
```

#### sidx packing for batch

```c
Non-batch: sidx[gidx] = tick              (bit31 = 0, fits in < 2^31)
Batch:     sidx[gidx] = bit31 | tick<<8 | pos  (bit31 = 1 marker)
```

### Data Flow

```
encode_batch:
  K chunks → compute base (byte-wise median)
          → for each chunk: compute 5B diff from base
          → build wire: header + base + K×5B diffs
          → tring_push(wire, 68+5K)
          → for each chunk: probe slot → shell_set + sidx_set(packed)

decode:
  gidx → sidx_get → if batch_flag: extract batch_tick + position
       → tring_read(batch_tick) → base + diff[pos]
       → batch_decode_diff(out, base, diff) → memcpy(base) + apply 1-2 byte overrides

delete/gc:
  dfield_delete → shell_clr() O(1) — same as non-batch
  dfield_gc → sidx_is_batch → sidx_batch_tick → compare with target tick
            → all batch gidxs point to same tick → freed when all shell flags cleared
```

### Remaining Tasks

| Priority | Task | Why |
|----------|------|-----|
| 1 | **Batch diff overflow** — if chunk differs in >2 bytes from base, store individually | ทำให้ batch lossless สำหรับ data จริง |
| 2 | **Rotated batch** — integrate `shell_batch_flush_v2` rotation into `dfield_encode_batch` | ปัจจุบันใช้ best_rot=0, ต้องใช้ rotation scan เพื่อหา orientation ที่ดีที่สุด |
| 3 | **Batch multi-level** — chunks in batch may fit different shell levels | ปัจจุบันใช้ level ของ chunk แรกเท่านั้น, ควร probe per chunk |
| 4 | **Tune score thresholds** — empirical tuning | ปรับ distribution ให้ optimal |
| 5 | **Optimize classifier speed** — single-pass score computation | enc MB/s ต่ำจาก 3-pass histogram+variance |

### Build Commands
```powershell
# test tring.h standalone
gcc -O2 -I. -o test_tring.exe test_tring.c -lm

# test geo_diamond_field.h (classifier + sparse + batch)
gcc -O2 -I. -I..\..\..\..\core\pogls_engine\twin_core `
    -o test_diamond_field_v3.exe test_diamond_field_v3.c -lm

# integrated bench (rotation + sparse + batch)
gcc -O2 -I. -I..\Diamond_shell_encoder -I..\Diamond_decode_hamburger `
    -I..\..\..\..\core\pogls_engine\twin_core `
    -o bench_v3_integrated.exe bench_v3_integrated.c -lm
```
