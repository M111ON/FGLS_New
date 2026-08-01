# Weight Structure Analysis — Qwen3-0.6B-Q8_0
## Date: 2026-08-01

## Key Findings

### 1. SCALE DISTRIBUTION — The real structure
**33.17% of all blocks** (6.18M/18.6M) have scales in a single bin: log2 ∈ [5.5, 6.0) — scales ≈ 32-64 in linear.

This is NOT noise. It's a structural concentration:
- The model's dominant output-channel magnitudes cluster in a narrow band
- The 50th-95th percentile of |scale×w| all sit in the same narrow range (~32-45)
- 50% of real magnitudes live in log2 range [-3.5, 6.0] (~0.088 to 64)

Scale histogram pattern:
- Log2 ≈ -25: 4.68% (very small scales — near-zero channels)
- Log2 [-16, +5.5]: alternating 1-2% bins (distributed)
- Log2 [5.5, 6.0]: **33.17% spike** (the cluster)

### 2. INT8 VALUES — Near-uniform (7.66 bits)
- Entropy: 7.6619 bits/value (max 8.0)
- Top values: 16 (1.60%), ±127 (1.5% each) — but ±127 spike is FORMAT-FORCED (every block has its max = 127)
- 256/256 distinct values all used
- No meaningful value-level duplicates (by construction: large samples → N/256 per value)
- Sign split: negative 47.96%, positive 51.17%, zero 0.87%

### 3. CONSECUTIVE RUNS — Negligible
- Max run length: 5
- Values in runs: 1.25% of total
- No exploitable run-length structure

### 4. IDENTICAL BLOCKS — Near-zero
- Duplicate blocks: 0.35% (65K/18.6M)
- Adjacent-identical blocks: 0
- Block-level duplication is not a compression lever

### 5. PER-TENSOR ENTROPY — All tensors ~7.6 bits
- Lowest: blk.16.ffn_down.weight (7.602 bits)
- All tensors essentially uniform — no layer-type differentiation

## Implications for Compression

### What works
1. **Scale delta-encoding**: scales vary widely (-26 to +6 in log2) but many cluster in [5.5, 6.0). Delta coding per layer could compress scales from 37MB to maybe 10-15MB.
2. **Real magnitude clustering**: 50% of |scale×w| in range [0.088, 64]. Could store these ranges more efficiently than raw int8.

### What doesn't work
1. **Int8 value redundancy**: 7.66 bits → theoretical floor = 544MB for 596M values. No practical method beats this.
2. **Block-level duplication**: 0.35% — too low to matter.
3. **Run-length encoding**: 1.25% — negligible.
4. **Bake-and-zip**: Degrades quality (PPL 22.6 → 265M with aggressive rules) while only achieving 0.91x at mild settings.

### The entropy wall (reconfirmed)
- Q8_0 weights: 7.66 bits/value → 0.95x lossless ceiling
- Q4_0 nibbles: 3.82 bits → 0.95x ceiling (already packed at 4 bits)
- Quantized formats are essentially entropy-optimal at byte level

## Tools Used
- `w_dup.exe` — full value/scale/magnitude/block analysis
- `q8_hist.exe` — |w| distribution
- `q4_hist.exe` — nibble distribution (Q4_0)
- `bake_sweep.exe` — parametrized bake for PPL testing
- `fgls_archive.exe` v3 — zstd body compression
- `fgls_extract.exe` v3 — decompression
