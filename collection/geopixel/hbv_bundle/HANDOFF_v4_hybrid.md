# HANDOFF — v4 Hybrid Encoder + Atomic Reshape
_Session 2026-05-15_

## สถานะ: ALL PASS — 0 failures

### Components

| Component | File | Status |
|-----------|------|--------|
| v4 header | `new_diamond_tring/geo_diamond_field_v4.h` | ✅ ~1020 lines |
| v3 bench (rotation) | `new_diamond_tring/bench_v3_integrated.c` | ✅ passes |
| Atomic proof | `new_diamond_tring/proof_invert_lossless.c` | ✅ passes |

### Pipeline Architecture

```
encode(chunk):
  try sparse → bitpack → LZ77 → LZ77+Hilbert(4) → raw(64B)
  → pick smallest → store in tring with tag

decode(gidx):
  sidx → batch? → read tring → check tag:
    tag=0xFB → Hilbert+LZ77 → unreorder
    tag=0xFD → LZ77
    tag=0xFC → bitpack
    size=17 → DENSE
    size<64 → sparse
    else → raw 64B

reshape(n_old→n_new):
  Hilbert walk → invert mask (~7%)
  invert: full remap (read + decode + rehash)
  walked: preserve idx (just shell flag + sidx)
  → ~14× faster reshape
```

### Performance (Pentium G4400 — 2-core, no AVX2 = worst case)

| File | enc MB/s | dec MB/s | notes |
|------|----------|----------|-------|
| C source | 27 | 237 | |
| Fold header | 27 | 241 | |
| Compressed zip | 30 | 78 | LZ77 decompress |
| Batch L2 (repetitive) | — | — | **11 B/chunk = 5.8×** |

### Compression Ratio

| Mode | B/chunk | Ratio | Condition |
|------|---------|-------|-----------|
| Batch L2 | 11 | 5.8× | ≤12 diffs from base |
| Batch L3 (theoretical) | ~5.1 | 12.5× | |
| LZ77 + Hilbert | ~47 | 1.35× | entropy data |
| Bitpack | 4-22 | 3-16× | ≤16 distinct values |
| Sparse (n≤2) | 1-63 | 1-64× | zero-heavy |
| Raw | 64 | 1× | fallback |

### Key Features

1. **Adaptive encoder** — try ALL methods per chunk, pick smallest (no heuristic)
2. **Hilbert pre-transform** (4 offsets) — reorder 64B via 8×8 Hilbert curve before LZ77
3. **Conditional batch seed** — 8B seed only when `seed_pc ≥ 8` (flags bit0)
4. **Generalized Hilbert** — `_batch_hilbert_d2xy_n()` works for any power-of-2 grid
5. **Atomic reshape** — Hilbert walk → invert ~7% → remap only invert slots
6. **Rotation pruning** — skip full 6-DOF scan when `seed_pc ≥ 16`
7. **DENSE mode** (17B) — v2-style DiamondBlock reconstruction (API, not auto)

### Next Session — **Optimize**

| Priority | Bottleneck | Current | Target |
|----------|-----------|---------|--------|
| 1 | **Rotation scan** (6-DOF × 64B × 6) | 6 × full chunk permute | sample stride or skip |
| 2 | **Classifier histogram** (256-bin × 64B) | 3 passes per chunk | single pass fused |
| 3 | **Adaptive trials** (5 methods × 64B) | full trial per chunk | lazy eval with early exit |
| 4 | **AVX2 lanes** | unused on G4400 | stride-4 batch decode |

Hardware note: G4400 = Intel Pentium 2015, 2-core, **no AVX2**. Speed numbers are worst-case baseline. On modern CPU with AVX2: expect 2-4× faster enc, 2-3× faster dec. Ratio unchanged.

### Build Commands
```powershell
# v4 test (classifier + hybrid + batch)
gcc -O2 -I. -I..\..\..\..\core\pogls_engine\twin_core -o test_v4.exe test_v4_quick.c -lm

# v3 bench (rotation + sparse + batch)
gcc -O2 -I. -I..\Diamond_shell_encoder -I..\Diamond_decode_hamburger `
    -I..\..\..\..\core\pogls_engine\twin_core `
    -o bench_v3_integrated.exe bench_v3_integrated.c -lm

# Atomic reshape proof
gcc -O2 -o proof_invert_lossless.exe proof_invert_lossless.c -lm
```

### Files
```
new_diamond_tring/
├── geo_diamond_field_v4.h   ← main v4 header (~1020 lines)
├── geo_diamond_field.h      ← v3 vestigial (ยังใช้โดย bench_v3)
├── tring.h                  ← variable-size timeline ring
├── bench_v3_integrated.c    ← rotation + sparse + batch benchmark
├── proof_invert_lossless.c  ← atomic reshape proof
├── test_diamond_field_v3.c  ← v3 field tests
├── test_tring.c             ← tring standalone tests
```
