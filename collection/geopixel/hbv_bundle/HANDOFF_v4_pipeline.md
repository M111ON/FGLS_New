# v4 Hybrid Geometric Pipeline — Handoff

## File
`collection/geopixel/hbv_bundle/new_diamond_tring/geo_diamond_field_v4.h`

## Changes

### 1. Fast entropy classifier (`_chunk_entropy_class`)
- 16-bin histogram + mean deviation → cheap O(64) estimator
- 0=low: try all 7 methods (sparse+bitpack+LZ+LZ×4 Hilbert)
- 1=medium: skip Hilbert (saves 4× LZ77)
- 2=high: store raw immediately (saves all trials)

### 2. Rotation scan: fast pass + borderline
- `_batch_rotation_fast_score()` — score single rotation
- `_batch_rotation_pruned()` — 1 rot → if score∈[12,20) → full 6-rot scan
- Expected 1.5-2× speedup vs full scan every time

### 3. LZ77 decode: word-aligned copy
- uint64_t copies when offset≥8, byte tail for overlap
- ~80 MB/s → ~120-150 MB/s

### 4. Pre-LZ reshape (`_batch_reshape_for_lz`)
- Hilbert walk → reorder chunks: walked slots first, invert last
- Integrated into `dfield_encode_batch()` — free compression boost from locality

### 5. Flow-aware routing (`dfield_encode_flow`)
- `FlowState`: prev_path, prev_seed, prev_rot, path_streak, locked_path
- Confidence-based: pick path with largest gap to 2nd-best
- Path lock after 4 consecutive same-path chunks
- Flow memory bias (10% boost for similar hash)

### 6. Shell full detection
- `Shell.occupied` counter + `shell_full()`
- Avoids O(cap) probe on full shells

### 7. LZ77 early-exit for Hilbert
- If LZ77 produced no compression → skip Hilbert trials
- Saves 4× Hilbert reorder + 4× LZ77

### 8. ×16 virtual sub-slot mode
- **Not for bulk** — it's a **precision refiner** for small/medium data
- XOR-fold chunk → 4-bit slope (apex wire pattern)
- `base×16+slope` = 16 sub-slots per base slot
- O(16) probe vs O(cap) normal probe
- `SHELL_IDX_BITS` 13→17 (131K idx/level)
- `SlotIndex.tick[]` inline→heap pointer (4.7MB avoids stack overflow)
- Shell flags do NOT track ×16 sub-slots (guard on shell_set/decode/GC)

### 9. Flat streaming mode (`dfield_encode_flat`, `dfield_decode_flat`)
- Bypasses shell/slot entirely
- Uses same adaptive encoder
- No capacity limit (tring capacity = heap)
- Returns sequential tick, caller manages index

## Capacity

| Mode | Max chunks | Max data | Use case |
|---|---|---|---|
| Shell normal | 13,041 | ~0.8 MB | Geometric ratio-optimized |
| Shell ×16 | 208,656 | ~12.7 MB | Precision refiner |
| Flat | unlimited | 2-10 GB+ | Bulk default |

## Constants

```c
ENTROPY_LZ_TAG        = 0xFD
ENTROPY_BITPACK_TAG   = 0xFC
ENTROPY_LZ_HILBERT_TAG = 0xFB
DENSE_NODE_SIZE       = 17
SHELL_MAX_LEVEL       = 8
SHELL_IDX_BITS        = 17   // 131072/level
SHELL_IDX_MASK        = 0x1FFFF
INDEX_SIZE            = 9<<17 = 1,179,648
```

## Tests (all pass)
- `test_v4_quick.c` — seed grouping + rotation pruning
- `proof_invert_lossless.c` — atomic reshape lossless proof
- `bench_entropy_fastpath.c` — entropy classifier speed
- BMP benchmark: 768×768, 28K chunks, all modes lossless
