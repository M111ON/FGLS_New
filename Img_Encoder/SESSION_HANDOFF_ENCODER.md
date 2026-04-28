# GeoPixel Encoder — Session Handoff
> combined3.c | ColorOffset + Cylinder 144 + Residual

---

## Current Results (test01.bmp 512×512)

| Method | Size | Ratio | PSNR | Notes |
|--------|------|-------|------|-------|
| RAW | 786,432 B | 1.00x | inf | |
| Bitpack 3-tier | ~577K B | 1.36x | inf | lossless |
| **ColorOffset+Cyl144** | **408,176 B** | **1.93x** | **30.90dB** | theoretical |
| PNG | 393,847 B | 2.00x | inf | reference |

- Plane verify: **PASS** (lossless inside cylinder)
- Loss source: ColorOffset clip 5.83% only

---

## Architecture (combined3.c)

```
RGB pixel
  ↓ ColorOffset transform (asymmetric bias-aware)
    R  → stored as-is (8 bit)
    G-R → clamped [-64..+16] → stored as 0..80
    B-R → clamped [-96..+16] → stored as 0..112
  ↓ 3 planes → Cylinder 144 encode
    row delta → (spoke, zone, residual)
    spoke: 0=R- 1=R+ 2=G-R- 3=G-R+ 4=B-R- 5=B-R+
    zone:  0..23 (24 zones per spoke)
    residual: actual_delta - zone_midpoint (max ±5)
  ↓ output: anchor(h) + stream(sn) + residual(sn) per plane
```

### Sacred Numbers
| Value | Role |
|-------|------|
| 144 | 6 spokes × 24 zones = fibo closure |
| 24 | 128→24 fold ladder (256→128→144→24) |
| 6 | GEO_SPOKES |
| 128 | pivot (128 × 162 = 144²) |

### Critical Bug Fixed This Session
```c
// WRONG: clamp (breaks uint8 wrap arithmetic)
plane[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);

// CORRECT: wrap (modular arithmetic, closed ring)
plane[x] = (uint8_t)(v & 0xFF);
```
uint8 delta chain must use wrapping — not clamping
(10 - 200) as uint8 = 66, not -190. Decode must mirror this.

---

## Entropy Analysis

```
[R  ] stream=2.553 bits  residual=3.142 bits  11 unique residuals
[G-R] stream=1.059 bits  residual=2.089 bits  11 unique residuals
[B-R] stream=1.265 bits  residual=2.279 bits  11 unique residuals
```

G-R uses only **14/144 symbols** — color transform collapses channel space dramatically

---

## Next Session Priority

### P0 — Entropy Coder (close the gap to PNG)
theoretical 1.93x → actual requires real entropy coding
Options (easiest to hardest):
```
A. ANS (rANS) on stream symbols (144-symbol alphabet)
B. Huffman on stream + separate Huffman on residual
C. zstd/lz4 as drop-in on top of current byte output
```
Option C fastest to test — just pipe output through zstd

### P1 — Reduce clip (improve PSNR → lossless)
Current clip 5.83% at [-64..+16] / [-96..+16]
Option: add overflow stream for out-of-range pixels
```
if |GR| > range → mark pixel, store exact GR separately
overhead: ~5.83% × 2 bytes = small
result: lossless full image
```

### P2 — 2D prediction (beat PNG convincingly)
Currently row delta only
Add: predict from pixel above → vertical delta often smaller
Combined predictor: min(horizontal, vertical, diagonal)
Expected entropy drop: 2.553 → ~2.0 bits for R channel

### P3 — GeoPixel bridge
```
cylinder address → trit space
fibo clock gear (144) = cylinder address space
spoke (0-5) = GEO_SPOKES
zone (0-23) = trit-compatible (24 = 8×3)
```

---

## File: geopixel_encoder.c
Compile: `gcc -O2 -o encoder geopixel_encoder.c -lm`
Run: `./encoder image.bmp`
Input: 24-bit BMP only (convert webp/png via PIL first)
Output: stats to stdout, no file output yet
