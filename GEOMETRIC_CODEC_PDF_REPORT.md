# Geometric Codec × PDF Compression — Test Report

**Date:** 2026-08-05  
**Test files:** 4 PDFs from `I:/DWGLS/dropbag/PDF_sample/` (11-20 MB each)  
**Sample size:** 4 MB per file  
**Method:** Apply geometric transforms to raw PDF bytes, compress with zstd, measure ratio

---

## Results Summary

| Transform | Avg z@1 | Avg z@6 | Avg z@19 | Δ vs raw@6 | Size Change |
|-----------|---------|---------|----------|------------|-------------|
| RAW baseline | 1.001 | 1.001 | 1.001 | — | 1.0x |
| **4D Rotation (XY+ZW)** | 1.030 | 1.030 | 1.030 | **+0.030** | 1.0x |
| **Cardioid mapping** | 1.060 | 1.060 | 1.063 | **+0.060** | 1.0x |
| Bit-plane interleave | 0.658 | 0.737 | 0.916 | -0.264 | 8.0x |
| Delta encoding | 1.001 | 1.001 | 1.001 | -0.000 | 1.0x |
| Rot→Bitplane | 0.657 | 0.733 | 0.943 | -0.267 | 8.0x |
| Rot→Bitplane→Delta | 0.615 | 0.677 | 0.906 | -0.324 | 8.0x |

**Ratio = original_size / compressed_size (higher = better)**

---

## Key Findings

### 1. PDFs are already well-compressed internally
- Raw zstd ratio ≈ 1.001-1.002x (essentially incompressible)
- PDF internal compression (FlateDecode, etc.) already removes most redundancy
- Byte entropy ≈ 8.0 bits/byte (maximum for byte data)

### 2. Cardioid mapping is the best pure geometric transform
- **+0.060 ratio improvement** (6% better compression than raw)
- Polar coordinate mapping (r, θ) → cardioid r = k(1+cos(θ)) creates byte-level regularity
- Entropy drops from 8.0 to ~7.51 bits/byte
- No size expansion (1:1 input/output)

### 3. 4D Rotation (XY+ZW) is second best
- **+0.030 ratio improvement** (3% better compression)
- Rotation matrices in 4D space (XY plane at 45°, ZW plane at 30°)
- Entropy drops from 8.0 to ~7.73 bits/byte
- No size expansion

### 4. Bit-plane interleave is misleading
- Creates 8x data expansion (each byte → 8 separate bit-planes)
- Individual bit-planes have low entropy (~1 bit/byte) but the 8x expansion means total compressed output is **larger** than raw
- Ratio < 1.0 means transform+compress is worse than raw

### 5. Combined pipelines don't help
- Each transform step adds overhead
- Rot→Bitplane→Delta is the worst performer (-0.324)
- Chaining transforms compounds the expansion penalty

---

## Why geometric transforms work (marginally)

The 4D rotation and cardioid mapping work because they:

1. **Exploit inter-byte correlations** — PDF data has structure at byte-pair and byte-quad level that geometric transforms can regularize
2. **Preserve data size** — No expansion means compression gains are pure improvement
3. **Create smoother gradients** — Rotation maps distant bytes closer together in value space, reducing zstd's literal costs

---

## Limitations & Next Steps

- These transforms show ~3-6% improvement on **already-compressed** PDFs
- Would likely show much larger gains on:
  - Raw sensor data / telemetry
  - Uncompressed images (BMP, raw TIFF, PPM)
  - Text before any compression
  - Random-access databases
- Could combine with zstd's dictionary mode for structured data
- The cardioid mapping is the most promising candidate for further development

---

## Files Created

- `I:/FGLS_new/geometric_pdf_compression_test.py` — Original test (had bugs, fixed)
- `I:/FGLS_new/full_file_test.py` — Full-file test (too slow, abandoned)
- `I:/FGLS_new/optimized_test.py` — Final working test script
- `I:/FGLS_new/GEOMETRIC_CODEC_PDF_REPORT.md` — This report
