# Experiment: Qwen2.5-Coder-1.5B Steering Integration

## Date
2026-05-28

## Objective
Integrate a dedicated coding model into the geometry routing system (Steering), replacing unusable Qwen3-architecture models with a compatible alternative.

## Hardware
- GPU: 2× NVIDIA GeForce GTX 1050 Ti (3.6 GB VRAM each)
- CPU: Vulkan backend via llama.cpp
- Platform: Windows

## Models Tested & Discarded

### 1. Qwen3-Zro-Cdr-Reason-V2-0.8B-NEO-EX-D_AU-Q8_0.gguf
- **Architecture**: qwen3
- **Size**: 874 MB (Q8_0)
- **Layers**: 42 (merge of two 0.6B models)
- **Embedding dim**: 1024
- **Context**: 40960
- **Problem**: Blue Screen of Death (BSOD) on Vulkan load. The 40960 context length requires ~7 GB KV cache in f16 — exceeds GTX 1050 Ti's 3.6 GB VRAM. Even CPU-only with reduced context hangs indefinitely.
- **Status**: Deleted

### 2. Qwen3-Embedding-0.6B-Q8_0.gguf
- **Architecture**: qwen3
- **Size**: 446 MB
- **Problem**: M-RoPE position buffer bug (llama.cpp issue #20085). `llama_get_embeddings*` returns all zeros. Cannot be used for embedding extraction required by the geometry routing pipeline.
- **Status**: Deleted

### 3. Qwen3 1.7B (Ollama blob)
- **Architecture**: qwen3
- **Layers**: 28
- **Embedding dim**: 2048
- **Context**: 40960
- **Problem**: Same M-RoPE bug. Also large context → high memory usage.
- **Status**: Deleted

**Root Cause**: All Qwen3-architecture models use M-RoPE (Multi-scale Rotary Position Embedding), which has a known incompatibility with llama.cpp's `llama_get_embeddings*` API (open issue #20085). The geometry routing system requires real embedding vectors for classification.

## Model Added: Qwen2.5-Coder-1.5B-Instruct

### Metadata
- **Source**: `Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF` on HuggingFace
- **File**: `qwen2.5-coder-1.5b-instruct-q8_0.gguf` (1.89 GB)
- **Architecture**: qwen2 ✅ (no M-RoPE)
- **Layers**: 28
- **Embedding dim**: 1536
- **Heads**: 12 (Q) / 2 (KV) — Grouped Query Attention
- **Context**: 32768
- **Tokenizer**: gpt2 + qwen2 pre

### Why This Model?
1. **Same architecture** (`qwen2`) as existing Qwen2.5-0.5B — proven compatible with the existing llama.cpp Vulkan build
2. **No M-RoPE** — embedding extraction works correctly for routing
3. **Small enough** for dual GTX 1050 Ti (1.89 GB Q8_0 fits in 3.6 GB VRAM with room for KV cache)
4. **Official GGUF** from Qwen team — stable and well-tested
5. **Coding-specialized** — trained on 5.5 trillion tokens of code and text

## Build Pipeline: Tensor Store

A geometry store was built for the coder model following the same pipeline used for SmolLM2-360M and Qwen2.5-0.5B.

### Output Files
| File | Path | Size |
|------|------|------|
| Raw tensors | `build/qwen25_coder_tensors_raw/*.qdat` | 339 files, 1.76 GB |
| Geometry index | `build/qwen25_coder_geom.gsidx` | JSON, 339 entries |
| Geometry data | `build/qwen25_coder_geom.gsdat` | Binary, 4068 bytes (12B/rec) |

### Geometry Distribution
Tensors are mapped to 12 faces (zones 0-11) via FNV hash + Fibonacci LFSR:
- Face 0: 29 tensors
- Face 1: 28 tensors
- Face 2: 26 tensors
- Face 3: 25 tensors
- Face 4: 27 tensors
- Face 5: 29 tensors
- Face 6: 37 tensors
- Face 7: 32 tensors
- Face 8: 30 tensors
- Face 9: 33 tensors
- Face 10: 20 tensors
- Face 11: 23 tensors

## Registry: coord_real_registry.json

### Current Model Slots
| Model Key | Model | Arch | Dim | Zones | Role |
|-----------|-------|------|-----|-------|------|
| `qwen25coder` | Qwen2.5-Coder-1.5B | qwen2 | 1536 | 0-3 | Coding expert |
| `qwen25` | Qwen2.5-0.5B-Instruct | qwen2 | 896 | 4-5, 9-11 | General |
| `smollm2` | SmolLM2-360M-Instruct | llama | 960 | 6-8 | Chat/lightweight |

### Zone Map
```
Zone 0-3:  qwen25coder (coding)
Zone 4-5:  qwen25      (general)
Zone 6-8:  smollm2     (chat)
Zone 9-11: qwen25      (general)
```

## Steering Verification

The BermudaRouter classifies text prompts to geometry coordinates deterministically. Test results:

| Prompt | Zone | Model | Shape | Tring Slot |
|--------|------|-------|-------|------------|
| "write fibonacci in python" | 8 | smollm2 | O | 11 |
| "how are you today" | 9 | qwen25 | O | 361 |
| "implement a binary search tree in rust" | 10 | qwen25 | O | 375 |
| "what is the meaning of life" | 7 | smollm2 | O | 177 |

Different prompts naturally land in different zones due to the embedding → codebook → stride-37 geometry mapping. No semantic classification is required — the routing emerges from the structure of token embeddings.

## Code Changes

### coord_runtime.py
- Added `valid_keys` filtering to `ModelSpec` construction to ignore extra JSON keys (`tensor_dir`, `arch`, `note`)
- Added `None` check for `store_path` to skip blocked/placeholder models (e.g., qwen35)

### build_qwen25_coder_store.py (new)
- Build script to extract GGUF tensors and create geometry store for the coder model
- Adapted from `build_smollm2_store.py` pattern

## Repository Cleanup
- Deleted: 3 GGUF files (~3.2 GB recovered)
- Deleted: Ollama blobs directory (Qwen3 1.7B metadata)
- Added: 1 GGUF file (coder, 1.89 GB)
- Built: 339 raw tensors + geometry store (1.76 GB)

## Remaining Issues
1. **Steering is deterministic, not semantic** — The router does not "know" coding prompts should go to the coder. It maps based on embedding geometry. Training the BermudaGate encoder on labeled prompt types would enable semantic steering.
2. **KV cache conflict on model switching** — Alternating between models in the same process corrupts KV cache. The system is designed for single-dispatch (one input → one model), not per-token round-robin.
3. **No embedding-extraction model remains** — Qwen3-Embedding was the only pure embedding model. The system now uses internal encoder features for routing rather than external embeddings.

## Files Referenced
- `I:\FGLS_new\collection\coord_real_registry.json`
- `I:\FGLS_new\collection\python_src\coord_runtime.py`
- `I:\FGLS_new\collection\python_src\geometry_model_pool.py`
- `I:\FGLS_new\collection\python_src\zero_warmup_engine.py`
- `I:\FGLS_new\collection\build_qwen25_coder_store.py`
- `I:\FGLS_new\collection\bermuda_router_v1.py`
- `I:\FGLS_new\collection\bermuda_reshape_v3.py`
