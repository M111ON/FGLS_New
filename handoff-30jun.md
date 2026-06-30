# Handoff — June 30 POGLS + Deep Benchmark

## Goal of Next Session
Fix DRamTile init ordering (VirtualAlloc before GPU buffers) → enable zero-copy mmap path, then wire SID page table to POGLS store.

## State of Play

### Done
- **POGLS store** (`runner/pogls_store.h` + `runner/gguf_to_pogls.py`): Python→C pipeline verified. Qwen3-4B → 394 tensors → 2.3 GB `.pogls` file, 100% roundtrip verified from C via `pogls_store_ptr()`.
- **Deep benchmark** (`runner/bench_deep.ps1`): LFM2.5-8B-A1B on GTX 1050 Ti 4GB×2:

| Config | Status | Prompt | Gen/tok | Total |
|--------|--------|--------|---------|-------|
| CPU baseline | ✅ | 15,662ms | 455ms | **23.1s** |
| Native GPU (--ngl 24) | ❌ DeviceLost | — | — | **crash** |
| SID+GPU+DRamTile | ✅ | **1,360ms** | **37ms** | **34.1s** |

- **nvidia-smi dmon data**: Winner uses GPU0 2,759 MB + GPU1 2,553 MB total (~5.3 GB across 2 cards). CPU baseline uses ~650 MB (compute buf only).
- **SID zero-copy verified**: `apply=0.00ms restore=0.00ms` — page table bit flip, no memcpy on hot path.
- **Crossover math**: GPU wins at >49 tok prompt or >43 gen tok (init 32s amortized).
- `runner/.gitignore`: `*.txt` added.

### Blocking
- **DRamTile `init failed — falling back to heap`**: `VirtualAlloc(5.1GB)` fails because GPU buffer allocations (`ggml_vk_create_device()`) fragment address space first. Fix: allocate DRamTile store BEFORE `llama_model_load_from_file()`. See `runner/dramtile_store.h:dt_store_init()` + `runner/llama_pogls_runner_sid_v2.c` init ordering.
- **Native GPU crashes** for models >4GB on single 1050 Ti (DeviceLost) — DRamTile enables running these anyway (heap fallback works, just slower mmap).

## Open Decisions
1. **DRamTile init reorder**: Move `dt_store_init()` call before `llama_model_load_from_file()` in runner init sequence. Needs to compute model size early (can use GGUF header scan) or default to 5.1GB.
2. **POGLS wire**: Replace `found_tensors[].orig_data` reads from GGUF with `pogls_store_ptr()` reads from `.pogls` file. Requires adding `--pogls-store` flag + `sid_pt_populate_from_pogls()`.
3. **Multi-turn benchmark**: Current single-prompt bench hides GPU init cost. Need chat session benchmark (3-5 turns) to show real win.

## Skills to Use
- `focused-fix` — for DRamTile init ordering fix
- `cross-session-board` — track progress across sessions

## Artifacts
- `runner/pogls_store.h` — POGLS flat tensor store (C API, 125 lines)
- `runner/gguf_to_pogls.py` — GGUF→POGLS converter (Python, 88 lines)
- `runner/bench_deep.ps1` — Deep benchmark script (PowerShell, 130 lines)
- `runner/dramtile_store.h` — DRamTile (VirtualAlloc → heap fallback)
- `runner/sid_page_table.h` — Page Table SID (2592B bitmap)
- `docs/sid-page-table-design.md` — design doc
- `AGENTS.md` — full session history
- `runner/model_qwen3_4b.pogls` — 2.3 GB POGLS store for Qwen3-4B (not in git)
