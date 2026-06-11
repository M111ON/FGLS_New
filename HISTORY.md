# HISTORY.md — Archived Session Logs

Historical session entries moved from AGENTS.md for reference.

---

## LLM Geometry Routing — Vulkan Phase (dual GTX 1050 Ti)

### tl;dr
Concept of geometry-routed LLM inference is **proven in Python** (random router maps different inputs to different Bermuda geometry). C runner (`llama_pogls_runner_v3.exe`) rebuilt with **Vulkan backend** — both GTX 1050 Ti detected, 25/25 layers offloaded, **38.68 t/s** (2.6× CPU speedup). Single-model only (no dispatch). RawBridge + GeoField store loading confirmed working but embedding extraction still returns zeros.

### What Works
- **Vulkan build**: `load_backend: loaded Vulkan backend from ggml-vulkan.dll` — ✓
- **Dual GPU**: Vulkan0 + Vulkan1 (GTX 1050 Ti, 3617 MiB each) — ✓
- **`--ngl 28`**: 25/25 layers offloaded (12 per GPU + output) — ✓
- **KV cache on GPU**: `llama_kv_cache: layer N: dev = Vulkan0/1` — ✓
- **Inference**: 38.68 t/s (vs CPU 14.76 t/s, 2.6× faster) — ✓
- **Build command**: `gcc ... llama.c -o runner.exe llama.dll ggml.dll ggml-base.dll -lm` (direct DLL link) — ✓
- **RawBridge** (`geom_raw_bridge.h`): serves 196 quantized tensors (446 MB) — ✓
- **GeoField store** (`qwen3_06b_geom.gsidx/.gsdat`): 54 keys, 1.76 GB — ✓
- **C gate forward** (`test_gate_forward.c`): encoder + codebook + geotable, matches Python — ✓
- **Python random router demo** (`demo_routing.py`): 5 prompts → 5 different zone/shape — ✓
- **BermudaGate training** (`bermuda_reshape_v3.py`): codebook EMA, Hilbert-gear pipeline — ✓ (but collapse)

### What's Broken / Blocked (historical)
1. **`llama_get_embeddings*` returns zeros (Qwen3-Embedding)** — confirmed open issue #20085 in llama.cpp. M-RoPE position buffer mismatch.
2. **BermudaGate codebook collapse** — training on repetitive generated tokens makes encoder output uniform → all map to code 0.
3. **Dual-model in-process: KV cache conflict on re-use** — alternating contexts corrupts KV.

### To Resume (historical)
1. Fix embedding extraction for real routing: use smollm2 (arch=`llama`) or qwen25 (`qwen2`), not Qwen3-Embedding
2. Train gate on diverse prompt embeddings (not generated tokens)
3. Rebuild llama.cpp from source with newer commit for potential Qwen3-Embedding fix
4. Add more model slots to registry (code/translation/reasoning experts)

### Resource Note
Vulkan build loads `ggml-vulkan.dll` + shader compilation ~5s cold start. 38.68 t/s benchmark on Qwen2.5-0.5B Q8_0 with `--ngl 28`. llama-bench pure Vulkan = 69.98 t/s (no embedding callback overhead).

---

## Session May 26 — Dual-model dispatch test
- **Added SmolLM2-360M-Instruct Q8_0** (arch=`llama`, 32 layers, 960-dim, 15 heads, 8192 ctx) — no M-RoPE bug
- **Built raw tensor store** with `build_smollm2_store.py`: 290 tensors → `.qdat` + `.gsidx/.gsdat`
- **Dual-model test results** (`test_dual_dispatch.exe`):
  - Both models load in one process: qwen25 580ms, smollm2 220ms (CPU)
  - Switch gap **~0 us** (just pointer swap, no I/O)
  - Each model runs independently: qwen25 14.6 t/s, smollm2 18.7 t/s (CPU)
  - KV cache conflict on rapid alternating — not an issue for single-dispatch routing

---

## Session May 28 — Fix unicode stray bytes + integrate hex tile codecs + lc_tantrix
- Fixed unicode EM DASH / box-drawing chars in 12 source files (`geo_tring_fec.h` ×5, `lc_tantrix.h` ×2, `geo_rewind_wang.h` ×2, `geo_frame_seek_wang.h` ×2, `lc_twin_gate.h` ×1) — GCC stray byte errors resolved
- **`lc_tantrix.h`**: 256-state routing layer. Tests: `test_tantrix.c` (11 tests). Wired via `lc_twin_gate.h`
- **`hex_tile.h`**: 7-cell hex codec (FLAT/TRIPLET_FLAT/GRADIENT/EDGE). Tests: `test_hex_tile.c` (24 tests). Added `GPX5_CODEC_HEX` (0x07) to all gpx5_container.h copies
- **`hex_codec.h`** v3: XOR dual predictor, 8B non-FLAT (SMOOTH/GRADIENT/EDGE). Tests: `test_hex_codec.c` (28 tests). Added `GPX5_CODEC_L2` (0x08)
- **`geo_hex_layer.h`**: hex_codec → GPX4 GeoAddr.sub bridge. Tests: `test_geo_hex_layer.c` (23 tests). Added `GPX4_LAYER_GEO` (0x06)
- All 3 test suites: **86/86 PASS**

### Architecture: Codec Stack (historical)
```
GPX5 pipeline (carrier-based, per-tile)
  └─ GPX5_CODEC_HEX (0x07) — hex_tile.h static inline, 7B→2/9B (v1: triplet/median)
Standalone batch
  └─ GPX5_CODEC_L2 (0x08) — hex_codec.h + impl.c, 49B batch (v3: XOR dual, 2/8B)
```
**Note**: hex_tile.h (v1, TRIPLET_FLAT/median, 9B) and hex_codec.h (v3, SMOOTH/XOR dual, 8B) use **incompatible** formats.

### New Files (May 28)
| File | Purpose |
|------|---------|
| `collection/hex_tile.h` | 7-cell hex tile codec (static inline) — wired as GPX5_CODEC_HEX |
| `collection/hex_codec.h` + `src/hex_codec_impl.c` | v3: XOR dual predictor, 8B non-FLAT, center-first scan |
| `collection/tests/test_tantrix.c` | 11 tests for lc_tantrix.h |
| `collection/tests/test_hex_tile.c` | 24 tests for hex_tile.h |
| `collection/tests/test_hex_codec.c` | 28 tests for hex_codec.h v3 |
| `collection/geo_hex_layer.h` | hex_codec → GPX4 GeoAddr.sub bridge |
| `collection/tests/test_geo_hex_layer.c` | 23 tests for geo_hex_layer.h |

### Updated Files (May 28)
| File | Change |
|------|--------|
| `lc_tantrix.h` ×2 | Added `#include <stdbool.h>` |
| `gpx5_container.h` ×4 | Added `GPX5_CODEC_HEX` + `GPX5_CODEC_L2` |
| `gpx4_container.h` ×3 | Added `GPX4_LAYER_GEO`, `GPX4_GEO_ADDR_SZ`, `GPX4_GEO_PENT`, `GPX4_GEO_HILBERT` |
| `hamburger_encode.h` ×2 | Added `#include "../hex_tile.h"` + CODEC_HEX branches |

---

## Prompt Engineering Expert Skill (integrated from `skills/prompt-engineering-expert/`)

### Trigger Conditions
Must use when user asks to analyze/review/improve a prompt, create system prompts, reports prompt issues, or asks about prompt engineering techniques.

### Debugging Workflow
1. **Identify**: What's not working?
2. **Analyze**: Is objective clear? Instructions specific?
3. **Test**: Try more context, specificity, examples
4. **Fix**: Update prompt, verify with multiple inputs
5. **Validate**: Does it generalize?

---

## Session June 1 — Gate trained on REAL weight embeddings, store rebuilt
- **Problem**: Synthetic-trained gate mapped ALL real weight rows to zones 4&10
- **Fix**: `train_gate_on_real_weights.py` — extracts 171K pooled embeddings from `.f32` tensors, trains gate on real distribution
- **Result**: 12/12 zones active (8.2-8.4% each), recon=0.0001, 500 epochs in 8.6s
- **Store rebuilt**: `qwen35_geom.gsidx/.gsdat` — 48 keys, 171,264 rows, 1.52 GB data
- **New files**: `train_gate_on_real_weights.py`, `build_geom_store_from_f32.py`
- **Modified**: `bermuda_reshape_v3.py` — added export_gate(), code_dim=32

---

## Session June 3 — Chord predictor inference in demo + C bridge integration
- `TrainedChordRouter` class added to `demo_music_routing.py` — loads all 17 files from `build/chord_export/`
- Fixed weight transpose bug: PyTorch `(out_features, in_features)` requires `.T` for numpy
- Full demo: 12 sections, all working (heuristic router + C bridge + trained predictor + store pipeline)

---

## Session June 3 — Weight as Bond Block (geometry-addressed frame storage)
- **`weight_gbond.h`** (single-header C): fixed-frame bond block format, O(1) random access
- **Results on Qwen3.5-0.8B (89 tensors, 1.55 GB)**: Raw → Bond = 0.9962×, lossless 100%
- **New files**: `weight_gbond.h`, `weight_gbond_test.py`, `tests/test_gbond.c`, `experiment_weight_video_frames.py`

---

## Session June 3 — Codec audit (lossless shrink options)
- Raw ZSTD: **2.79×** (best ratio)
- Seekable ZSTD: **2.46×** (keeps random access)
- gbond: 0.996× (best seekable, no compression)
- GPR1: needs format redesign (uint16 overflow)
- weight_bond_codec.h: ~1e-3 error (not lossless-safe)
- **Recommendation**: gbond for random-access + ZSTD underneath for size

---

## Session June 3 — Topology-aware zone router prototype
- `topology_zone_router.py`: extracts topology fingerprints, recommends codec per tensor
- `qwen/gemma/nemotron` → `recipe+segZSTD` (triplet-heavy)
- `moondream2` → `rawZSTD` (edge-heavy)

---

## Session June 3 — End-to-end zone codec test
- `end_to_end_zone_codec_test.py`: tensor → fingerprint → router → encode → decode → verify
- Sample result (4 tensors): 69.21 MB → 26.42 MB (**2.6191×**), all OK

---

## Session June 3 — Score-based routing refinement
- `topology_zone_router.py` now uses scored router (dominant zone, entropy, cardioid ratios, hex ratios)
- Tools: `rawZSTD`, `recipe+segZSTD`, `gbond`, `segZSTD`, `cusp-special`

---

## Session June 3 — Lossless storage routing (compression optional)
- `lossless_topology_router.py`: storage-shape selector (raw / gbond / recipe / segmented)
- Compression treated as optional hint

---

## Session June 3 — Learned topology router
- `train_topology_router.py`: softmax router on tensor fingerprints
- Saved model: `build/topology_router_weights.npz`
- Training: 98 samples, 8 features, 5 classes, 100% accuracy

---

## Experiment: Codec on Real Weights

### Setup
- **Model**: Qwen2.5-0.5B (Q8_0) + Qwen3.5-0.8B (.f32)
- **Comparison**: hex_tile (.gsten), C1 delta, raw .qdat / .f32

### Results
| Codec | Input | Raw Size | Encoded | Ratio | Note |
|-------|-------|----------|---------|-------|------|
| hex_tile | f32 tensor | 24 MB | 31 MB | 0.78× (expand) | EDGE tiles dominate |
| hex_tile | Q8_0 tensor | 833 KB | 1,785 KB (C) | 0.47× (expand) | +6B/tile overhead = 2.14× |
| C1 delta | Q8_0 tensor | 833 KB | 846 KB | 0.98× (expand) | Flat window |

### Key Finding
**hex_tile / .gsten ไม่เหมาะกับ weight data** — Weight bytes มี local correlation ต่ำ → 99% tiles เป็น EDGE → expansion เสมอ

### Seed-Based Geometry Path (core paradigm)
เปลี่ยนจาก "keep value" → **"keep recipe that produces value"**:
- Test multiple geometry paths (Hilbert/Wang/Peano/etc.)
- Pick seed that reconstructs correctly
- Store seed + lossless residual (if needed)
- Decode: run geometry path from seed → reconstruct
- Per-zone codec, container tracks codec ID

### Proven Approaches
1. **Geometry store path**: f32 → mean-pool 1024→128 → gate → .gsidx/.gsdat
2. **Chord model**: `train_chord_on_real_weights.py` → 61.7% acc
3. **Seed path**: ต้องออกแบบต่อ — geometry path ที่ lossless กับ weight distribution จริง

---

## Session June 4 — PWC V2 C decoder fix: squareish shape bug
- **Critical bug**: PWC `int32_tile_row_delta64` encoded with squareish shape but C decoder used original shape for inverse cumsum → NaN/inf
- **Fix**: `_inv_int32_tile_row_delta` now uses `e.srows`/`e.scols` instead of `e.orows`/`e.ocols`
- **segZSTD decoder** rewritten, **rawZSTD decoder** fixed (skip 3B magic)
- **Result**: C test decodes all **290 tensors → 0 errors, no NaN**
- **Files changed**: `pogls_weight_container.h`, `tests/test_pwc.c`, `tests/test_pwc_debug.c`, `tests/test_pwc_simple.c`
