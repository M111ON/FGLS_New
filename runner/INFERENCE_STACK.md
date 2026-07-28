# FGLS Inference Stack — Quick Reference

## 🚀 Quick Commands

```bash
# One-shot chat (fastest, no server)
./chat.sh "สวัสดีครับ"
./chat.sh q3q8 "1+1 เท่ากับอะไร"  # Q8_0 variant

# Start server (OpenAI-compatible API)
./serve.sh           # Qwen3-0.6B Q4_0 on port 8080
./serve.sh q3q8      # Qwen3-0.6B Q8_0
./serve.sh q25       # Qwen2.5-0.5B Q8_0

# Benchmark all models
./bench.sh           # Full benchmark
./bench.sh q3q4      # Single model

# Call API
curl http://127.0.0.1:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"messages":[{"role":"user","content":"Hello"}],"max_tokens":50}'
```

## 📊 Benchmark Results (GTX 1050 Ti)

| Model | Size | Prompt t/s | Gen t/s | Best for |
|-------|------|-----------|---------|----------|
| Qwen3-0.6B Q4_0 | 359MB | 1,300 | **112** | Speed (recommended) |
| SmolLM2-360M Q8_0 | 367MB | 1,337 | 89 | Smallest, decent speed |
| Qwen3-0.6B Q8_0 | 604MB | 1,035 | 85 | Quality |
| Qwen2.5-0.5B Q8_0 | 639MB | 1,254 | 82 | Quality |

**Key findings:**
- Q4_0 is 32% faster than Q8_0 (112 vs 85 t/s)
- Context size (up to 65K) doesn't affect TG speed
- VRAM usage: ~441MB (Q4_0), ~700MB (Q8_0)
- Thread count doesn't matter (GPU-bound)

## 🧩 Geometry Modules (for llama_pogls_runner_sid_v2)

```
runner/
├── gear_shift.h       — Routing/scheduling layer (stream src→dst)
├── gear_lock.h        — Icosa lane feedback for SID swap scheduling
├── gear2.h            — Pinned memory mirror (single DMA transfer)
├── dramtile_store.h/c — DRamTile: CPU-side mmap weight store
├── vramtile.h         — VRamTile: GPU-side weight promotion
├── sid_cache.h        — SID cache management
├── sid_loader.h       — SID tensor loader
├── sid_page_table.h   — Page table for tensor swapping
├── kv_sid_evict.h     — KV eviction with SID routing
├── kv_remap.h         — KV cache remapping
└── kv_tensor_access.h — Direct KV tensor access
```

### Build Status
- ✅ All headers found
- ✅ Compilation passes (warnings only)
- ❌ Link: needs dramtile_store.c + kv_*.c implementations
- ⚠️ Missing: `pogls_platform.h` only in `collection/core/core/`

## 🔧 System Requirements

```
Hardware: GTX 1050 Ti (4GB VRAM) × 2, 2 CPU cores
Software: llama.cpp b9733 (Vulkan), Python 3.13
Models:   I:/model/*.gguf
```

## ⚠️ GPU Memory Notes

```
GPU 0 (display):  ~1.7GB used by Windows (dwm, Chrome, Hermes)
GPU 1:            Can be used for inference (if Qwen.exe closed)

When VRAM low:
  1. Close Qwen.exe (I:\Qwen\Qwen.exe)
  2. Use ngl=0 for CPU-only (4 t/s with 2 threads)
  3. Use smaller model (SmolLM2-360M)
```
