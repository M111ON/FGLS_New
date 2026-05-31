# AGENTS.md — Session Handoff

## ก่อนสแกน (Pre-scan check)
- ก่อนเรียก scan() ให้เรียก get_project_index() เพื่อดูสถานะ cache
- หาก cache มีข้อมูลล่าสุด ให้ใช้ cached data แทนการสแกนใหม่
- หาก cache ไม่มีข้อมูลหรือล้าสมัย ให้สแกนตามปกติ

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

### What's Broken / Blocked
1. **`llama_get_embeddings*` returns zeros (Qwen3-Embedding)** — confirmed open issue #20085 in llama.cpp. M-RoPE position buffer mismatch. Bypass: use models with `llama` or `qwen2` arch (no M-RoPE) instead.
2. **BermudaGate codebook collapse** — training on repetitive generated tokens makes encoder output uniform → all map to code 0.
3. **Dual-model in-process: KV cache conflict on re-use** — alternating contexts corrupts KV. OK for single-dispatch (one input → one model), not for per-token round-robin.

### Files
| File | Status |
|------|--------|
| `C:\TPOGLS\vulkan_run\llama_pogls_runner_v3.exe` | Rebuilt with Vulkan DLLs (91 KB) |
| `C:\TPOGLS\vulkan_run\llama.dll` (et al.) | Vulkan builds from `I:\llama\bin\vulkan\` |
| `C:\TPOGLS\Makefile` | Updated with `vulkan-build` target + direct DLL linking |
| `I:\FGLS_new\collection\geom_raw_bridge.h` | Recreated (was missing). Single-header RawBridge |
| `C:\TPOGLS\llama_pogls_runner_v3.c` | Added `#define GEOM_RAW_BRIDGE_IMPLEMENTATION` |
| `collection\build\qwen3_06b_geom_tensors_raw\` | 196 raw quantized tensors (446 MB) |
| `collection\build\smollm2_tensors_raw\` | 290 raw quantized tensors (0.36 GB) — SmolLM2-360M store |
| `C:\TPOGLS\test_dual_dispatch.c` | Dual-model dispatch test program |
| `coord_real_registry.json` | Updated: qwen(z0-3), qwen25(z4-5,9-11), smollm2(z6-8) |

### Session May 26 — Dual-model dispatch test
- **Added SmolLM2-360M-Instruct Q8_0** (arch=`llama`, 32 layers, 960-dim, 15 heads, 8192 ctx) — no M-RoPE bug
- **Built raw tensor store** with `build_smollm2_store.py`: 290 tensors → `.qdat` + `.gsidx/.gsdat`
- **Dual-model test results** (`test_dual_dispatch.exe`):
  - Both models load in one process: qwen25 580ms, smollm2 220ms (CPU)
  - Switch gap **~0 us** (just pointer swap, no I/O)
  - Each model runs independently: qwen25 14.6 t/s, smollm2 18.7 t/s (CPU)
  - KV cache conflict on rapid alternating — not an issue for single-dispatch routing

### To Resume
1. Fix embedding extraction for real routing: use smollm2 (arch=`llama`) or qwen25 (`qwen2`), not Qwen3-Embedding
2. Train gate on diverse prompt embeddings (not generated tokens)
3. Rebuild llama.cpp from source with newer commit for potential Qwen3-Embedding fix
4. Add more model slots to registry (code/translation/reasoning experts)

### ⚠️ Resource Note
Vulkan build loads `ggml-vulkan.dll` + shader compilation ~5s cold start. 38.68 t/s benchmark on Qwen2.5-0.5B Q8_0 with `--ngl 28`. llama-bench pure Vulkan = 69.98 t/s (no embedding callback overhead).

---

## Active: Geometry Engine Dashboard (lightweight, always available)

### Server
`collection/python_src/engine_dashboard.py` — FastAPI on port 8766.

### UI
`engine_dashboard.html` (single-file, Bauhaus Neo-Brutalist design, ~1085 lines)

### 7 Features (auto-discovered from `features/`)
1. **Geometry Store** — stats, coverage matrix, key browser, weight query + heatmap
2. **Bermuda Router** — 4 modes (ORBITAL/CHIRAL/CROSS/HUB), zone radar, shape donut, R/G bar, TRing heatmap
3. **Bermuda Gate** — forward test (Python), compare with C (code_idx=906 match)
4. **C Pipeline** — DLL bridge to `pogls_bermuda.dll`, Hilbert bijection, traverse, batch route
5. **C Binaries** — list/run `.exe` from browser
6. **Geometry Codec** — Hamburger encoder, bond-key tracking, lossless MP4
7. **GeoPixel Codec** — pure-Python GeoPixel encode/decode, 27-pixel stripe, SVG grid, roundtrip verify

### Start
```
cd collection/python_src && python engine_dashboard.py
```

## Active: Container Formats (GPR1 / GPX4)
- **GPR1**: Generic residual container — chunk-based delta storage with 4B CRCs, single-header C codec
- **GPX4**: Multi-layer GeoPixel container — O4 grid layers, delta layers (YCgCo + ZSTD), animation header

## Session May 28 — Fix unicode stray bytes + integrate hex tile codecs + lc_tantrix
- Fixed unicode EM DASH / box-drawing chars in 12 source files (`geo_tring_fec.h` ×5, `lc_tantrix.h` ×2, `geo_rewind_wang.h` ×2, `geo_frame_seek_wang.h` ×2, `lc_twin_gate.h` ×1) — GCC stray byte errors resolved
- **`lc_tantrix.h`**: รัน 256-state routing layer. Wrote `tests/test_tantrix.c` (11 tests). Fixed `bool` type (added `stdbool.h`). Added to `src/` copy. Wired via `lc_twin_gate.h`
- **`hex_tile.h`**: 7-cell hex codec (FLAT/TRIPLET_FLAT/GRADIENT/EDGE). Fixed encode/decode byte-count bug. Wrote `tests/test_hex_tile.c` (24 tests). Added `GPX5_CODEC_HEX` (0x07) to all 4 `gpx5_container.h` copies. Wired into `hamburger_encode.h` ×2 copies
- **`hex_codec.h`** v3: XOR dual predictor, 8B non-FLAT (SMOOTH/GRADIENT/EDGE). Center-first scan order. Created `src/hex_codec_impl.c`. Wrote `tests/test_hex_codec.c` (28 tests). Added `GPX5_CODEC_L2` (0x08) to all 4 gpx5_container.h copies
- **`geo_hex_layer.h`**: hex_codec → GPX4 GeoAddr.sub bridge. 14-bit sub field: [13:12] type, [11:8] xor_diff, [7:0] center. Bulk write/read for GEOA layer. Wrote `tests/test_geo_hex_layer.c` (23 tests). Added `GPX4_LAYER_GEO` (0x06) + `GPX4_GEO_ADDR_SZ`/`GPX4_GEO_PENT`/`GPX4_GEO_HILBERT` to all 3 `gpx4_container.h` copies
- All 3 test suites: 86/86 PASS

### New Files
| File | Purpose |
|------|---------|
| `collection/hex_tile.h` | 7-cell hex tile codec (static inline) — wired as GPX5_CODEC_HEX |
| `collection/hex_codec.h` + `src/hex_codec_impl.c` | v3: XOR dual predictor, 8B non-FLAT, center-first scan |
| `collection/tests/test_tantrix.c` | 11 tests for lc_tantrix.h |
| `collection/tests/test_hex_tile.c` | 24 tests for hex_tile.h |
| `collection/tests/test_hex_codec.c` | 28 tests for hex_codec.h v3 (tile + L2 + stats) |
| `collection/geo_hex_layer.h` | hex_codec → GPX4 GeoAddr.sub bridge |
| `collection/tests/test_geo_hex_layer.c` | 23 tests for geo_hex_layer.h |

### Updated Files
| File | Change |
|------|--------|
| `lc_tantrix.h` ×2 | Added `#include <stdbool.h>` |
| `gpx5_container.h` ×4 | Added `GPX5_CODEC_HEX` + `GPX5_CODEC_L2` |
| `gpx4_container.h` ×3 | Added `GPX4_LAYER_GEO`, `GPX4_GEO_ADDR_SZ`, `GPX4_GEO_PENT`, `GPX4_GEO_HILBERT` |
| `hamburger_encode.h` ×2 | Added `#include "../hex_tile.h"` + CODEC_HEX branches in apply/invert |

### Architecture: Codec Stack
```
GPX5 pipeline (carrier-based, per-tile)
  └─ GPX5_CODEC_HEX (0x07) — hex_tile.h static inline, 7B→2/9B (v1: triplet/median)
Standalone batch
  └─ GPX5_CODEC_L2 (0x08) — hex_codec.h + impl.c, 49B batch (v3: XOR dual, 2/8B)
```
**Note**: hex_tile.h (v1, TRIPLET_FLAT/median, 9B) and hex_codec.h (v3, SMOOTH/XOR dual, 8B) use **incompatible** formats — separate codec stacks.

### Integrations
- **geo_hex_layer.h**: bridges hex_codec v3 into GPX4 GEO layer. `gpx4_geo_hex_write()` packs tiles → 32-bit GeoAddr array; `gpx4_geo_hex_read()` unpacks. `GPX4_LAYER_GEO` (0x06) added to all 3 `gpx4_container.h` copies.

---

## Prompt Engineering Expert Skill (integrated from `skills/prompt-engineering-expert/`)

### Trigger Conditions
Must use this skill when user:
- Asks to analyze, review, or improve a prompt
- Wants to create system prompt / custom instructions for an agent or skill
- Reports prompt issues (inconsistent outputs, hallucinations, vague responses, wrong format)
- Asks about prompt engineering techniques (CoT, few-shot, XML tags, role-based, prefilling, chaining)
- Wants to design a testing/evaluation framework for prompts
- Asks about anti-patterns or best practices

### Rules
- Always check if this skill applies before acting on prompt-related requests
- If it applies, it MUST be used
- Read `skills/prompt-engineering-expert/CLAUDE.md` for core instructions
- Reference `docs/` for detailed techniques, best practices, and troubleshooting
- Do not skip the debugging workflow: Identify → Analyze → Test → Fix → Validate

### Debugging Workflow (for prompt issues)
1. **Identify**: What's not working? How does it fail?
2. **Analyze**: Is objective clear? Instructions specific? Context sufficient? Format specified?
3. **Test**: Try more context, specificity, examples, format changes
4. **Fix**: Update prompt, verify with multiple inputs
5. **Validate**: Does it generalize? Is it efficient?

### Quick Anti-Pattern Reference
| Issue | Fix |
|-------|-----|
| Inconsistent | Add format spec + examples |
| Hallucinations | Ask for sources + confidence levels |
| Vague | Add specific details + examples |
| Wrong format | Show exact format example |
| Doesn't generalize | Use variables, handle variations |
