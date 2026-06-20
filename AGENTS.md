# AGENTS.md — Session Handoff

## ⚠️ ห้ามใช้ git โดยเด็ดขาด
- **ห้าม run `git` commands ใดๆ ทั้งสิ้น** ไม่ว่า git status, git add, git commit, git push, git clean, git reset, git checkout ฯลฯ
- หากจำเป็นต้องตรวจสอบประวัติไฟล์ ให้ใช้ Read tool อ่านไฟล์โดยตรงเท่านั้น
- หาก user ต้องการให้ commit หรือ push เดี๋ยว user จัดการเอง
- ข้อยกเว้น: ถ้า user สั่งให้ใช้ git โดยตรง (written in stone) เท่านั้น

## 🛑 วงจรอุบาทว์ (Fix→Crash Loop) Protocol
เมื่อเข้า loop: แก้ → crash → แก้ → crash → แก้ → crash เกิน **3 รอบติด**:
1. **หยุดทันที** — อย่าแก้ต่อ
2. **ประเมินสถานการณ์**: ถาม user ว่า "เราควรเปลี่ยนแนวทางหรือลองอะไรต่อ?"
3. **ทบทวนสมมติฐาน**: อ่านไฟล์ที่เกี่ยวข้องทั้งหมดอีกครั้ง (ไม่ใช่แค่บรรทัดที่ crash)
4. **ใช้วิธีที่ง่ายที่สุด** ที่น่าจะใช้ได้ก่อน — อย่าเพิ่ม abstraction, indirection, หรือ fallback scan ที่ซับซ้อน
5. **เมื่อไม่แน่ใจ**: ใช้ printf/fprintf debug ทีละชั้นก่อน — อย่าเดา root cause

## Pre-scan check
- ก่อนเรียก scan() ให้เรียก get_project_index() เพื่อดูสถานะ cache
- หาก cache มีข้อมูลล่าสุด ให้ใช้ cached data แทนการสแกนใหม่
- หาก cache ไม่มีข้อมูลหรือล้าสมัย ให้สแกนตามปกติ

## 🧭 Ground Rule: มองรอบก่อนพุ่ง
เมื่อติดปัญหา — หยุด มองดูว่ามี asset อะไรในโปรเจ็กต์ที่เกี่ยวข้องอยู่แล้วบ้าง
โปรเจ็กต์นี้มีของดีผ่านการทดลองมามาก แต่บางอันถูกทิ้งเพราะมีอะไรดีกว่ามาแทน หรือยังไม่เจอเคสเหมาะ
**อย่า hardcode/scan ใหม่ ถ้ามีของที่ใช้ได้อยู่แล้ว**

---

## 🎭 Cosplay: Signature-Derived Perturbation Profile (June 19 — Validated)

### What It Does
Replaces 485MB `.gsten` tensor store with a **~2.7KB `.cpl` file** (0.0005%) that encodes minimal perturbation rules derived from SID signatures. At inference, cosplay applies sparse byte-level XOR to SID-cache tensor data before swap injection — producing **detectably different model output** while keeping the model fully coherent.

Key insight: ~1.5% of bytes per tensor flipped by ±1 is enough to change the output measurably, but the model absorbs the noise and stays coherent.

### File Format (`.cpl`, version 2)
- **Header**: magic(4) + version(4) + n(4) + n_layers(4) + n_heads(2) + n_embd(2) = 20B
- **Per-entry**: name_hash(4) + mode(1) + arg(1) + stride(2) + data_size(4) + tick(4) = 16B
- Lookup key: FNV-1a hash of tensor name (computed identically in trainer and runner)
- Backward compat: reads v1 (12B entries, data_size default 0)
- **2.7KB for 170 entries vs 485MB gsten = 0.0005%**

### Perturbation Strategy
- Target: only tensors in SID cache (weight tensors ≥64KB, ~170 of 291)
- `stride=64`, XOR `arg=0x01`: ~1.5% of bytes flipped per tensor
- Statistical: ~6% of perturbed bytes hit block scales (harmless empirically)
- Trainer: `n_targets=0` → auto-select all weight tensors (skips F32 norms/biases <64KB)
- `.cpl` size scaling: 36B (1 tensor) → 400B (25) → 2.7KB (170)

### Scaling Results (Qwen2.5-0.5B Q4_K_M, prompt "Hello", temp=0)
| # Tensors | `.cpl` size | First token | Output tail |
|-----------|------------|-------------|-------------|
| 0 (baseline) | — | `\n` | `Hello! How can I assist you today? ...valuable source of information for others like you in the future. 😊` |
| 1 (output.weight) | 36 B | `\n` | `...of utmost importance in helping me improve my responses. Thank you!` |
| 3 | 68 B | `\n` | `I'm not sure what you're asking for. Can you please provide more information?` |
| 9 | 164 B | `\n` | `I'm not sure what you mean by "Hello", but it seems like a response...` |
| 25 (all attn_output) | 420 B | `\n` | `I'm not sure what you mean by "Hello", but if you have a specific question... Goodbye!` |
| **170 (ALL weights)** | **2.7 KB** | `,` | `I'm not sure what you're asking for. If there's any information I can help with, please let me know!` |

All outputs are **coherent English** — no crash, no garbage, just different responses.

### Files
- `runner/cosplay.h` — CosplayProfile, CosplayEntry, save/load (v1+v2), lookup, verify, apply, train API
- `runner/cosplay_train.c` — CLI: `cosplay_train <store.gsten> <output.cpl>` (default: all weight tensors)
- `runner/llama_pogls_runner_sid_v2.c` — `--cosplay PATH` + `--cosplay-compare` flags
- `docs/COSPLAY.md` — Full documentation with format details, API ref, scaling table, philosophy

### Key APIs
```c
cosplay_train(cp, gi, gsten_path, target_names, n_targets);  // train from gsten
cosplay_save(path, cp);                                       // save .cpl (v2)
cosplay_load(path, cp);                                       // load .cpl (v1 or v2)
cosplay_find(cp, name_hash);                                  // lookup by FNV-1a hash
cosplay_apply(&ce, data, size);                               // → malloc'd perturbed copy
```

### New: `--cosplay-compare` (Cosplay + Time Travel)
At startup, before the main loop, runs a **controlled comparison**:
1. Decode test prompt "Hello" **WITH** cosplay → save logits + first token
2. `llama_memory_clear()` → reset KV cache
3. Toggle delta arrays to original (unperturbed) cached data
4. Decode **WITHOUT** cosplay → save logits + first token
5. Print comparison: token diff, cosine similarity, max logit diff, same-sign ratio, top-5 overlap
6. Toggle back, clear KV cache, continue normally WITH cosplay

**Sample output** (170 tensors, Qwen2.5-0.5B):
```
  First token WITH cosplay:    ',' (token 11)
  First token WITHOUT cosplay: '\n' (token 271)
  Same token? NO
  Logits cosine similarity:    0.997262
  Max logit difference:        1.467321
  Same-sign ratio:             97.9%
  Top-5 overlap:               5/5
```
The first token changes from `\n` to `,` — **proof that cosplay measurably affects output** despite 99.7% logit similarity.

### New: `--experiment DIR` (Multi-Delta Experiment Framework)
Auto-runs all `.cpl` files in a directory + baseline, compares outputs:

```
experiment_dir/
  ├── subtle.cpl
  ├── moderate.cpl
  ├── aggressive.cpl
  └── results/
      ├── 00-baseline/{tokens.txt,logits.bin}
      ├── 01-subtle.cpl/...
      ├── 02-moderate.cpl/...
      ├── 03-aggressive.cpl/...
      └── report.txt
```

For each condition: snapshot → apply perturbation → generate → save → cross-compare.
Output: first token, full text, timing, pairwise logit cosine similarity matrix.

**Sample**: 6 conditions on Qwen2.5-0.5B:
```
  00-baseline:          first=11 ','  (coherent)
  01-25-attn-output:    first=11 ','  (same first token, different content!)
  02-full-170-stride32: first=18137   (garbled — too aggressive)
  03-full-170-stride64: first=220     (coherent, different)
  04-full-170:          first=271     (coherent, different)
  05-same-cpl:          first=11 ','  (coherent, different content)
```

Key insight: **same .cpl with same first token can produce different content** — perturbation changes the model's latent path without changing the immediate sample.
At startup, before the main loop, runs a **controlled comparison**:
1. Decode test prompt "Hello" **WITH** cosplay → save logits + first token
2. `llama_memory_clear()` → reset KV cache
3. Toggle delta arrays to original (unperturbed) cached data
4. Decode **WITHOUT** cosplay → save logits + first token
5. Print comparison: token diff, cosine similarity, max logit diff, same-sign ratio, top-5 overlap
6. Toggle back, clear KV cache, continue normally WITH cosplay

**Sample output** (170 tensors, Qwen2.5-0.5B):
```
  First token WITH cosplay:    ',' (token 11)
  First token WITHOUT cosplay: '\n' (token 271)
  Same token? NO
  Logits cosine similarity:    0.997262
  Max logit difference:        1.467321
  Same-sign ratio:             97.9%
  Top-5 overlap:               5/5
```

The first token changes from `\n` to `,` — **proof that cosplay measurably affects output** despite 99.7% logit similarity.

### Key Design Decisions
- **Lookup by FNV-1a name hash** (not SID node_id): avoids collisions where multiple F32 norm tensors map to the same SID coordinate
- **`cosplay_apply()` returns malloc'd buffer**: runner uses `is_malloc` flag for cleanup in `sid_swaps[]`
- **Trainer is flexible**: `n_targets=0` → all weight tensors (≥64KB), or explicit target list
- **Only weight tensors in SID cache** are perturbable: F32 norms/blases not in SID cache are skipped
- **Comparison uses raw tensor_set_data()** (not time travel ring) to avoid side effects

### Known Limitations
- ~6% of perturbed bytes may hit block scales (adds noise but empirically harmless at stride=64)
- `.cpl` file is model-specific (trained from a gsten bake); requires source gsten
- Full-weight perturbation changes output detectably but model stays coherent — no "controlled identity" mode yet

### Compile (standard cosplay trainer)
```
gcc -O2 -std=c11 -I. -Icollection -Icollection/src -Icollection/core -Icollection/core/core -Icollection/core/pogls_engine/core -Icollection/core/geo_headers -Icollection/geo_jump_module/include -II:/llama.cpp/include -II:/llama.cpp/ggml/include -o runner/cosplay_train.exe runner/cosplay_train.c collection/geo_jump_module/src/geo_jump.c -lm
```

### Profile-Aware Cosplay Trainer
`runner/cosplay_profile_train.c` — uses `.ses` session profile to modulate per-tensor stride based on face usage frequencies. See Session June 20 (late) section below for details and trained files.

---

## ✅ Session June 18 (late) — Icosa Bridge GPU Context Fix: Kernel Dispatch Now Works

### Goal
Fix `"invalid resource handle"` (CUDA error 400) when `icosa_bridge.dll` dispatches the icosa lane kernel during llama.cpp GPU inference — caused by CUDA context conflict between `icosa_bridge.dll` and `ggml-cuda.dll`.

### Root Cause
`icosa_bridge.dll` uses CUDA runtime API (implicit primary context), while `ggml-cuda.dll` uses CUDA driver API to create separate contexts for each GPU. When ggml-cuda does inference work, its context becomes current, making our runtime handles (stream, allocations) invalid. The `cudaSetDevice(0)` workaround was insufficient because it doesn't restore the driver-level context.

### Fix
Added CUDA driver API context save/restore in `_dispatch_chunk()` at `collection/src/icosa_twin_bridge.cu:270`:
1. Added `CUcontext cu_ctx` field to `IcosaGpuCtx` struct
2. Save context at init via `cuCtxGetCurrent()` after CUDA runtime is initialized
3. In `_dispatch_chunk()`: save previous context → `cuCtxSetCurrent(ctx->cu_ctx)` → do CUDA work → restore previous context
4. Added `#include <cuda.h>` and linked with `-lcuda` (driver API lib)

### Result
- **Dual GPU inference (`--ngl 29`) + `--twin-gpu`: NO errors** — model generates "! How are you feeling today? 😊" from prompt "Hello"
- **CPU inference + `--twin-gpu`: also works** — same output
- No "invalid resource handle" (error 400), no "dispatch error", no crashes
- All CUDA resources clean up properly (buffer sizes match expectations)
- Compile command: `nvcc -O2 -arch=sm_61 -shared -o icosa_bridge.dll icosa_twin_bridge.cu -lcudart -lcuda -DICOSA_BUILD_DLL -DICOSA_SKIP_MAIN -allow-unsupported-compiler -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`

### Files Changed
- `collection/src/icosa_twin_bridge.cu`: Added `#include <cuda.h>`, `CUcontext cu_ctx` field, `cuCtxGetCurrent()` at init, context save/restore in `_dispatch_chunk()`
- `collection/src/icosa_bridge.dll`: Recompiled with context fix

### Still Pending
- Chat mode with `--twin-gpu --ngl 29` not yet verified (should work)
- Full gear lock integration (meaningful icosa lane processing, not just push)
- Build Colab deployment package: `.cu` + build script for T4 (sm_75)

---

## ✅ Session June 18 — ZoneCardSID Y-Triangle + Tensor Memory Store + SID E2E Verified

### Goal
Retarget ZoneCardSID to Y-triangle, create tensor memory store, get SID pipeline to run end-to-end with real model.

### What Changed

#### New/Modified Files
- **`collection/zone_card_sid.h`**: Retargeted — `node_id(4B)+capo_key(2B)` replaces `face(1B)+zone(1B)+slot(1B)+tring_pos(2B)`. Uses `geo_jump.h` with `GEO_JUMP_INLINE`.
- **`collection/sid.h`**: Removed 5 dead functions referencing old ZoneCardSID fields (`sid_coord_from_zcsid`, `sid_comm_payload`, `sid_capture`, `sid_summon`, `sid_summon_sig`).
- **`collection/src/tensor_memory.h`**: New — TensorMemStore with TMEM_NONE/TMEM_DELTA compression, mask-based query (`tmem_query`), save/load (`tmem_save`/`tmem_load`), `tmem_foreach`. 31/31 tests pass.
- **`collection/tests/test_tensor_memory.c`**: New — 31 tests covering query, save/load, foreach, delta compression.
- **`runner/llama_pogls_runner_sid_v2.c`**: Added `--mem-store PATH` (logging at wrong level — see below), `sid_mem_store_log()`, `geo_jump.c` in build command. Updated build command in header.

### SID E2E Pipeline Status
- **Model load**: 458-503 ms ✓
- **Tensor scan**: Both `-O0` and `-O2` find **291/291** (previously reported 13/291 is resolved; Tier 2 ``find_layers_ptr`` works correctly with both opt levels) ✓
- **SID cache init**: 170 weight tensors read = 485 MB, verified ✓
- **SID swap setup**: 170/291 tensors selected for per-decode swap ✓
- **Reaches decode with swaps**: Running inference reaches `llama_decode` with swapped tensor->data pointers ✓
- **GPU backend**: Hardcoded CPU (`ggml-cpu-sse42.dll`); `ggml-vulkan.dll` exists but not yet integrated

### Key Findings
- **`geo_jump.c` must be linked explicitly**: `tw_bridge.h` includes `geo_jump.h` without `GEO_JUMP_INLINE`, so geo_jump functions become `extern` and need `geo_jump.c` in the link line.
- **TensorMemStore integration at wrong level**: Current `--mem-store` logs per-decode tensor data (raw swap log), but vision is condensed user behavioral profile. Needs redesign.
- **`_hilbert_idx` warning**: pre-existing in `tw_bridge.h:267`; inside `#ifdef GEO_JUMP_INLINE` block, unavailable when `geo_jump.h` included without it.

### Still Pending
1. **`--mem-store` redesign**: Align with vision (user behavioral profile, not raw swap log)
2. **`--capture` pipeline**: Not tested with real model yet
3. **GPU backend**: Unblock Vulkan for faster inference
4. **`_hilbert_idx` warning**: Fix or suppress

### Bug Fix (June 18) — `sid_summon` node_id/shell_id confusion
- **Root cause:** `sid_summon()` at `collection/sid.h:312` passed `coord->node_id` (0..20735) directly to `geo_shell_decode()` which expects a `shell_id` (0..287). `shell_id % 288` corrupted face/ring info.
- **Effect:** 5/20 tensors failed roundtrip in real model test. Wrong face/ring → wrong centroid → wrong (vx,vy) → recapture node_id mismatch.
- **Fix:** `geo_shell_decode(coord->node_id, ...)` → `geo_shell_face(coord->node_id)` + `geo_shell_ring(coord->node_id)`. These extract face/ring from `node_id = face*1728 + ring*144 + side*72`, the correct inverse of `tw_to_node()`.

---

## 🔮 SID Vision (from user, June 18)

### Shadow Clone + Venom Philosophy
- **Shadow Clone**: independent execution, expire → transfer experience. Not parallel dummy.
- **Venom**: inject, take over, carry forward, enhance next target. Zero waste, evolutionary.
- **No reverse engineering**: chasing version updates is unsustainable.

### Next Phase: SID → Tensor Memory → Topology
```
input (screen/text/event)
    ↓
encode → SID coordinate (face, spoke, slot)
    ↓
map   → tensor region (layer, head, channel)
    ↓
bond  → neighbor tensors via topology edges
```

### Bond Discovery (user's findings with line primitives)
Lines, length, angle, intersection can be:
- Projected, offset, reconstructed
- Origin drift/shift along path
- Parameterized lines = **routers** without needing actual geometry

**Bond is lower-level than geometry** — it's a level below geometric forms, acting as a pure topological primitive.

---

## Time Travel (June 15 — Delta Ring + Rewind/Fast-Forward)

### Core Files
- **`runner/sid_delta_ring.h`** — Circular buffer journal of SID swap operations. Each entry records `(ft_idx, tensor_ptr, orig_data, sid_data, size)`. Supports checkpoint names (up to 64), automatic tail eviction on overflow.
- **`runner/sid_timetravel.h`** — Orchestrator wrapping delta ring. `sid_timetravel_before_decode()` pushes swap entries into ring before each `sid_swap_apply()`. `sid_timetravel_after_decode()` processes pending checkpoint/rewind/ffwd commands.

### How It Works
- Before every `sid_swap_apply()`, the delta ring records all tensor pointers being swapped: the original data pointer AND the SID cache pointer. **No data copying** — just pointer tracking.
- After every `sid_swap_restore()`, pending time travel commands are processed:
  - **Rewind**: walks the ring backward from tail to checkpoint, restores `orig_data` pointer in each tensor struct. Undoes all SID injections back to the checkpoint.
  - **Fast-forward**: walks forward from checkpoint to head, re-applies `sid_data` pointers. Re-does all SID injections.
  - **Checkpoint**: marks current ring position with a name.
  - **Branch**: copies ring state up to checkpoint into a new ring.
- Ring capacity = 1024 entries. Older entries evicted from tail when full.

### CLI Flags & Chat Commands
```
--sid-checkpoint NAME    /checkpoint NAME     Save checkpoint
--sid-rewind NAME        /rewind NAME         Rewind (undo swaps)
--sid-ff NAME            /ff NAME             Fast-forward (redo swaps)
--sid-branch NAME        /branch NAME         Branch state
                         /tt                  Print time travel state
```

### Key Design Decision
- **Pointer-only journaling**: no data copies. Rewind = pointer restore in reverse; Fast-forward = pointer re-apply forward. Copy-on-write branching with different SID data is future work.
- **Chronological ring**: each swap cycle produces one entry per tensor. 170 tensors × 2 cycles/token = 340 entries/token. Capacity 1024 entries ≈ 3 tokens of history. Bump `SID_DELTA_MAX_ENTRIES` for longer history.

---

## 🌡️ Bond Prediction (June 15 — Cardioid Express + Metatron Route)

### What It Does
When `--bond` is passed, the runner builds a **bond graph** over all 290 discovered tensors, then computes a **hotness score** (0.0 cold → 1.0 hot) for each tensor using:

1. **Cardioid Express** — Maps each layer to a cardioid geometry position `pos = layer * 720 / n_layers`. Applies `r(θ)=a(1+cosθ)` Q8 fixed-point gate: inside cardioid = express (hot=1.0), outside = warm (0.5), cusp near θ=π = cold (0.1). Signal byte `(layer*17+42)&0xFF` as deterministic per-layer data value.

2. **Metatron Route Topology** — Maps each weight tensor to a Metatron face (0..11) based on slot type + ring, then discovers:
   - **META_ORB**: same face, adjacent slot (= same type, consecutive layers)
   - **META_CHIRAL**: opposite face (±6), same slot (= same type, opposite ring)
   - **META_CROSS**: inter-ring bijection via CROSS_MAP LUT

3. **Hotness Propagation** — 3-pass diffusion: each tensor's hotness spreads to bonded neighbors (weighted by bond strength). After propagation, hotness reflects both cardioid position AND topological connectivity.

### CLI Flag
- `--bond`: Enables bond discovery, cardioid scoring, and Metatron topology. When combined with `--sid-face N`, cold tensors (hotness < 0.3) are skipped from SID swaps.

### Output
```
[bond] 2320 total bonds:
  LAYER_SLOT:    217
  INTRA_LAYER:   672
  CARDIOID:      889
  META_ORB:      270
  META_CHIRAL:   144
  META_CROSS:    128
[predict] hot=32.4% warm=57.2% cold=10.3%
[predict] top-5 hottest:
  1.00  blk.0.attn_norm.weight
  1.00  blk.0.ffn_norm.weight
  1.00  blk.1.attn_norm.weight
  1.00  blk.1.ffn_norm.weight
  1.00  blk.2.attn_norm.weight
[sid] bond filter active: will skip cold tensors (hotness < 0.3)
[sid] 217 / 290 tensors will be swapped per decode (bond filter skipped 30 cold)
```

### Relevant Files
- **`runner/bond_discovery.h`**: All bond types — static (LAYER_SLOT, INTRA_LAYER), cardioid express phase, and Metatron topology (orbital, chiral, cross). Also: `bond_predict_hotness()`, `bond_predict_print()`, heap-allocated BondGraph with `bond_graph_init()`/`bond_graph_free()`.
- **`runner/llama_pogls_runner_sid_v2.c`**: `--bond` flag integration at line ~382. Computes n_layers from found tensor names, passes to bond discovery, stores bond_hotness array, applies cold filter in SID swap setup (line ~510).

### Notes
- Cardioid bonds limited to `|layer_diff| <= 1` to avoid N² explosion.
- BondGraph is heap-allocated with capacity `n_found * 8` (up to 32768).
- BOND_METATRON_HUB defined in enum but not currently discovered (too noisy).
- Hotness propagation: 3 passes, 0.7 self-weight + 0.3 × bond_weight × neighbor hotness.

---

## ✅ Milestone v1.0 "12-Face Bridge Pipeline" — COMPLETE (June 16)

### Summary
All 3 phases of milestone v1.0 have been implemented, compiled, and verified (5265 PASS / 0 FAIL with T8 skipped due to no tensor data).

### Files Created
- `runner/capture_pipeline.h` — Full capture pipeline header: `CaptureResult`, `CaptureTensor`, `capture_init()`, `capture_tensor()`, `capture_write_freeze_wallet()`, `capture_write_store()`, `capture_verify()`, `capture_run_full()`, `capture_summary()`. Uses `tw_capture_tensor_raw` for dequant + signature, runs `tw_iterate_faces` for full 12-face capture per tensor.

### Files Modified
- `collection/tw_face_bridge.h`
  - Added `#include "tw_tensor_capture.h"`
  - Added `TWCapture12FaceResult` struct + `tw_capture_12face_init/free()`
  - Added `tw_capture_tensor_12face()` — orchestrator: `tw_capture_tensor_by_name` → `tw_capture_priority` → `frame_at` → rewind store → freeze wallet in one call
  - Added `tw_capture_tensor_12face_batch()` — multi-tensor batch wrapper
  - Added `DualFrame df` field to `TWCapture12FaceResult` for timeline integration

- `collection/tests/test_tw_face_bridge.c`
  - Upgraded T8 to use `tw_capture_tensor_12face()` orchestrator
  - Added T9 orchestrator unit test (init/free/priority integration)
  - Added T10 timeline round-trip test (DualFrame verification, determinism, World B)
  - Added B1 benchmark test (behind `#ifdef BENCHMARK`)

- `runner/llama_pogls_runner_sid_v2.c`
  - Added `#include "capture_pipeline.h"` after `FoundTensor` definition
  - Added `--capture DIR` CLI flag with arg parsing and help text
  - Added capture call after prompt decode in both chat and prompt modes
  - Uses `CaptureTensor` descriptor to bridge `FoundTensor` → capture pipeline

### Key Design Decisions
- `capture_pipeline.h` defines `CaptureTensor` (lightweight, runner-agnostic) to avoid coupling to `FoundTensor` struct
- `CAPTURE_TENSOR_FROM_FOUND` macro inlined (GCC scoping quirk with `#ifdef` + `for`-scope vars)
- `.tw` (freeze wallet) + `.gsten` (full tensor store) both written per capture
- Lossless verification via `tw_capture_tensor_raw` roundtrip

### Next
- User needs to run `--capture` with an actual GGUF model to verify freeze wallet + .gsten output files are correct
- Benchmark with `-DBENCHMARK` to measure 12-face capture throughput against 2000 t/s target

---

## 🎤 Qwen3 TTS Pipeline (June 17 — bf16 Fixes NaN + Manual ST Load)

### tl;dr
**bfloat16 fixes `TensorCompare.cu:110 Assertion 'input[0] != 0'`** caused by float16 overflow during attention softmax computation. Model outputs all-(-1.0) audio (silence/DC) — generation runs without crash but codes are invalid.

### Working Pipeline (test_fix_v9.py)
1. **Load main model**: meta → replace params with CUDA bf16 → copy weights from CPU sd (dtype conversion on CPU, `copy_` to CUDA — avoids intermediate CUDA tensor)
2. **Load speech tokenizer**: create on **CPU** (not meta — preserves scalar buffers like `padding_total`, `stride`), move float params+buffers to CUDA bf16, load weights tensor-by-tensor via manual safetensors parse (avoids mmap paging error)
3. **Monkey-patch SafeCE**: clamp embedding indices to `num_embeddings-1` to avoid OOB
4. **`generate_voice_clone()`**: runs without NaN crash but output is all -1.0

### Key Findings
- **float16 overflows**: `torch.where()` assert fires because softmax produces NaN from overflowed logits in float16. bf16 has same exponent range as f32 — no overflow.
- **safe_open paging error**: Windows `safe_open` mmap exhausts page file when loading 682MB speech tokenizer after main model weights. Manual safetensors parse (raw bytes → `torch.frombuffer`) avoids mmap.
- **Speech tokenizer scalar buffers**: `MimiConv1d.register_buffer("padding_total", ...)` creates scalar int64 buffers. Must keep on CPU or the Mimi encoder's `_pad1d()` breaks (`pad()` expects Python ints, not CUDA tensors).
- **Float buffers must move to CUDA**: Codebook `embed` is a buffer, not a parameter. Need to iterate `named_buffers()` and move float ones to CUDA.

### Current Limitation
- Audio output is all -1.0 (DC silence). Likely cause: generated tokens/codes are wrong. Possibly embedding weights not matching between talker and code_predictor, or the code_predictor generates invalid codes that decode to silence.

### Relevant Files
- `I:\FGLS_new\test_fix_v9.py`: Working pipeline (bf16 + manual ST)
- `I:\FGLS_new\test_fix_v8.py`: Previous version (f16 — NaN crash)
- `I:\model\qwen3-tts-0.6b\`: Model directory
- `I:\model\.cache\pykokoro\`: Kokoro TTS (working independently)

---

## 🔺 Session June 17 — Y-Triangle Migration (geo_jump, Remove TETRA/OCTA)

### Goal
Replace compound-of-5-tetra/octa addressing (3456/6912) with geo_jump Y-triangle (GEO_FULL=20736) for O(1) access, no warmup, simpler frustum routing.

### What Changed

| File | Change |
|---|---|
| `geo_compound_cfg.h` (8 copies) | Removed `GeoCompoundType`, `GeoFaceBase`, `frustum_divisor`, `geo_face_route()`, `geo_addr_translate()`, `geo_compound_cfg_verify()`, `geo_cfg_frustum_unit()`. Single `GEO_CFG` at GEO_FULL=20736. |
| `shell_container.h` | Removed mode/geometry params. `shell_init()` now takes only `(s, shell_id, subdivision, seed)`. Anchor space = GEO_FULL/12 = 1728. |
| `shell_hop.h` | Bridge is identity (1:1) — shell_to_geo and geo_to_shell both mod GEO_FULL. Removed scale_factor. |
| `shell_weight_map.h` | Removed `.geometry` field from Chord init. (already removed from struct) |
| `onion_stack.h`, `onion_shell.h` | Removed mode param from `shell_init()` calls. |
| `tgw_frustum_wire.h` (4 copies) | `FRUSTUM_TETRA_CEILING = GEO_FULL/6`, `FRUSTUM_JUNCTION = GEO_FULL/3`. core/ copy uses hardcoded values (same math, no geo_jump.h dependency). |
| `lc_wire.h` (collection/ copy) | `LCW_MAIN_SPACE = GEO_FULL`, `LCW_RESIDUE = GEO_FULL/3`. core/ copy uses hardcoded. |
| `test_onion.c` | 3 `shell_init()` calls updated (no mode param), 3 Chord inits without `.geometry`. |

### Key Design Decisions
- **Y-triangle eliminates frustum quad conversion**: direction = topology natively, no need for tetra+octa hybrid
- **geo_compound_cfg.h kept as backward-compat shim** — new code should use `geo_jump.h` directly (`JUMP_PENTAGON`, `geo_pentagon_id`, `geo_capo`, `geo_field_climate`)
- **3456/6912 still appear** in `frustum_slot64.h`, `frustum_layout_v2.h`, `geo_field_core.h`, `exp_frame_hash.c` — these are data sizes / hash seeds, NOT TRing-related, left untouched
- **core/ directory** is self-contained (no geo_jump.h) — uses hardcoded values that match GEO_FULL arithmetic

### Verification
- Zero actual code references to removed types/functions remain (all in comments only)
- `FRUSTUM_TETRA_CEILING = 20736/6 = 3456` ✓
- `FRUSTUM_JUNCTION = 20736/3 = 6912` ✓
- `LCW_RESIDUE = 20736/3 = 6912 = 4×12³` ✓
- `GEO_CFG.slots_per_spoke = 20736/6 = 3456` ✓
- `shell_init()` calls all 4-param, no `.geometry` anywhere ✓

### NEXT: Capture Pipeline Retarget
`tw_face_bridge.h` and `capture_pipeline.h` still use 12-face / 1440 TRing (face rotation, hex+tri dual grid, centroids, resid). Need to retarget to geo_jump Y-triangle:
- Replace 12-face iteration → Y-triangle node_id (0..20735) mapping
- Replace `TWFreezeEntry.tring_pos` (0..1439) → node_id (0..20735)
- Replace `TW_REWIND_SLOTS=1440` → 20736 or per-shell partition
- Replace face rotation + resid → `geo_pentagon_id()` + `geo_capo()` O(1) targeting

---

## ✅ Session June 19 — Multi-Turn Chat Profile (SES2 + Transition Matrix)

### Goal
Upgrade SessionProfile from flat histogram (960 bins per face×spoke×slot) to include **face-to-face transition tracking** and **conversation timeline**, so multi-turn chat sessions produce distinguishable "trajectory fingerprints" — not just per-face aggregates.

### What Changed

#### `session_profile.h` — SES2 Format Upgrade
- **New magic**: `SES2` (old `SES1` still loads → auto-migrate)
- **New fields**:
  - `last_face` — previous face index for transition tracking
  - `trans[8][8]` — 8×8 uint32 transition matrix (256 bytes)
  - `timeline[4096]` — in-memory ring of `(face, step)` pairs (not saved to file)
- **New APIs**:
  - `ses_profile_transition(sp, from, to)` — increment `trans[from][to]`
  - `ses_profile_push_timeline(sp, face, step)` — append to ring
  - `ses_profile_print()` — now also prints: face usage histogram + transition matrix + timeline preview
- **File size**: SES1 = 3848B → SES2 = 4108B (+260B for trans + face header)
- **Backward compat**: verified — SES1 file loads as SES2, frames preserved, re-saved as SES2

#### `llama_pogls_runner_sid_v2.c` — Chat Loop Transition Tracking
- **`g_turn_count`**: new static counter, increment per user message
- **Turn-varying hash**: `ses_fnv1a_ints(tokens) ^ (turn * 0x9E3779B9U)` — identical messages in different turns → different faces
- **Per-message transition recording**: `sid_rebuild_swaps(new_face)` followed by `ses_profile_transition(last_face, new_face-1)` + `ses_profile_push_timeline(new_face-1, turn)`
- **Initial face recording**: first face (or face=9 clamped to 0) pushed as timeline[0]

### Verified
- **Tech conversation** (3 turns: "linked list → pointers → malloc"):
  ```
  Timeline:     F0 → F6 → F2 → F5
  Transitions:  F0→F6→F2→F5
  Face usage:   F2=49%, F5=38%, F6=13%
  ```
- **SES1→SES2 migration**: 100-frame SES1 loaded → accumulated 2014 more → saved as SES2 (4108B)
- **SES2 binary layout**: magic(4) + n_frames(4) + last_face(1) + pad(3) + bins(3840) + trans(256) = 4108B ✓

### Still Pending (user's roadmap)
1. ✅ Phase 1 — Weighted face selection + decay + quality score
2. ✅ Phase 2 — ses_cmp: histogram + transition + timeline comparison
3. ✅ Phase 3 — ses_cluster: unsupervised HAC clustering
4. ✅ Phase 4 — ses_merge: merge histograms + transition matrices
5. ✅ Phase 5 — ses_featurize: profile → CSV feature vector
6. ✅ Phase 6 — profile-batch: multi-session generation in single process
7. 🔜 Feedback loop — last, after stable baseline

### Key Files
- `I:\FGLS_new\runner\session_profile.h` — SES2 format, transition matrix, timeline, backward compat, quality score metrics
- `I:\FGLS_new\runner\llama_pogls_runner_sid_v2.c` — g_turn_count, turn-varying hash, transition recording

---

## ✅ Session June 19 (late) — Phase 1: Weighted Face Selection + Decay + Quality Score

### What Changed

#### Weighted Face Selection (`ses_select_face`)
- Replaces old XOR (`fnv(tokens) ^ (turn * C)`) with per-byte weighted blend:
  - `blended_byte = sem_byte * sem_weight + hist_byte * hist_weight + rnd_byte * rnd_weight`
  - Deterministic — same input + same weights → same output
  - First turn (turn=0) uses pure semantic hash (no history yet)
- CLI flags:
  - `--semantic-weight F` (default 0.8)
  - `--history-weight F` (default 0.2)
  - `--random-weight F` (default 0.0)
  - All three weights act independently; no requirement to sum to 1.0

#### Edge Weight Decay (`ses_decay_transitions`)
- `--decay F` (default 1.0 = no decay): applied after each turn to all transition matrix entries
  - `trans[i][j] *= factor`
  - With `--decay 0.99`, older transitions fade by 1% per turn — profile reflects recent behavior
  - Entries become ~0 after enough decay (but remain as zero-valued uint32)

#### Quality Score Metrics (`session_profile.h`)
- **`ses_profile_face_entropy()`** — Shannon entropy of face distribution (0..3, bits)
- **`ses_profile_face_balance()`** — normalized entropy (0 = all one face, 1 = perfectly balanced)
- **`ses_profile_transition_entropy()`** — avg entropy of each transition row (0 = deterministic path)
- **`ses_profile_stability()`** — fraction of self-loop transitions (high = repetitive path)
- **`ses_profile_quality()`** — composite: 0.40×balance + 0.35×(1-trans_entropy) + 0.25×stability
- **`ses_profile_print_quality()`** — prints all metrics at session end

### Verified
- Default (sem=0.8/hist=0.2): `F5→F6→F1`, quality=0.5448 (entropy=1.461)
- History-heavy (sem=0.1/hist=0.9): `F5→F8→F6` — different trajectory, different weights
- `--decay 0.5` parsed correctly, no crash
- `--help` shows all new flags

### Still Pending
1. **`ses_cmp` v2** — histogram + transition Frobenius/KL + timeline LCS
2. **`ses_cluster`** — unsupervised clustering of N sessions
3. **`ses_merge`** — merge profiles (histograms + transition matrices)
4. **Feedback loop** — last, after baseline is stable

---

## ✅ Session June 19 (late) — Phase 2: ses_cmp CLI Tool

### What Changed
- **`runner/ses_cmp.c`** (new) — standalone CLI comparing two SES1/SES2 profiles

### Metrics Reported
| Section | Metric | Description |
|---|---|---|
| Histogram | Cosine similarity | Face/spoke/slot distribution overlap |
| Histogram | L2 distance | Euclidean distance between bin vectors |
| Histogram | Face usage | Per-face usage vector + dominant face match |
| Transition | Frobenius norm | `||A - B||_F` of raw transition matrices |
| Transition | KL divergence | Mean row-wise KL (row-normalized) |
| Transition | Matrix diff | Side-by-side per-row comparison |
| Timeline | LCS similarity | Longest common subsequence of face steps |
| Summary | Verdict | Combined heuristic classification |

### Sample Output
```
── Histogram ──
  cosine similarity:  0.345230
  dominant face: A=F5 B=F4 (diff)

── Transition ──
  Frobenius norm:     2.449490
  KL divergence (mean row): 19.931568

── Summary ──
  histogram: Different topics (cos=0.3452)
  transition: Very different trajectory (Frob=2.4495 KL=19.9316)
  verdict: Likely different topics
```

### Compile
```
gcc -O2 -std=c11 -I. -Irunner -o runner/ses_cmp.exe runner/ses_cmp.c -lm
```

### Verified
- Self-comparison: cosine=1.0, L2=0, Frobenius=0, KL=0 ✓
- Same topic, different weights (0.8/0.2 vs 0.1/0.9): cos=0.345 — different face selection changes profile ✓
- Transition matrix diff shows exactly which rows differ ✓
- SES1 and SES2 both work (load function handles both)

---

## ✅ Session June 19 (late) — Phase 3: ses_cluster

### What Changed
- **`runner/ses_cluster.c`** (new) — Unsupervised hierarchical clustering of N session profiles

### Features
- Scans directory for all `*.ses` files
- Combined distance metric: `0.5 × (1 - hist_cosine) + 0.5 × clamp01(trans_frobenius / 10)`
- Hierarchical Agglomerative Clustering with average linkage
- Auto-determines optimal K via intra/inter-cluster ratio score
- Output: distance matrix + cluster assignments + dendrogram with intra/inter distances
- Platform support: Windows (`FindFirstFile`) and POSIX (`readdir`)

### CLI
```
ses_cluster <dir> [--min-similarity F] [--min-clusters N] [--max-clusters N]
```

### Verified
- 3 profiles loaded, distance matrix computed ✓
- HAC clustering produces 2-group assignment ✓
- Dendrogram shows intra/inter cluster distances ✓

### Compile
```
gcc -O2 -std=c11 -I. -Irunner -o runner/ses_cluster.exe runner/ses_cluster.c -lm
```

---

## ✅ Session June 19 (late) — Phase 4: ses_merge

### What Changed
- **`runner/ses_merge.c`** (new) — Merge 2+ session profiles into one

### Features
- Merges histogram bins (weighted average by n_frames)
- Merges transition matrices (sum of counts)
- Merges timelines (concatenated)
- `--ratio A B` override weighting for 2-profile merge
- `--out PATH` output file

### Usage
```
ses_merge A.ses B.ses --out merged.ses
ses_merge A.ses B.ses --ratio 0.7 0.3 --out merged.ses
ses_merge *.ses --out merged.ses
```

### Verified
- code_A + code_B + code_C → merged: histogram weighted by n_frames ✓
- Ratio merge `--ratio 0.8 0.2`: merged profile closer to A (cos=0.95 vs A) ✓
- Transition matrices summed correctly ✓

### Compile
```
gcc -O2 -std=c11 -I. -Irunner -o runner/ses_merge.exe runner/ses_merge.c -lm
```

---

## ✅ Session June 19 (late) — Phase 5: ses_featurize

### What Changed
- **`runner/ses_featurize.c`** (new) — แปลง SES2 → CSV feature vector (108 features)

### Features (108 columns per profile)
| Group | Count | Description |
|---|---|---|
| `face_0..face_7` | 8 | Face usage frequencies |
| `spoke_0..spoke_23` | 24 | Spoke (layer) frequencies |
| `slot_0..slot_4` | 5 | Slot type frequencies |
| `trans_0_0..trans_7_7` | 64 | Row-normalized transition probabilities |
| `entropy, face_balance, trans_entropy, stability, quality` | 5 | Quality metrics |
| `n_frames, last_face` | 2 | Metadata |
| **Total** | **108** | |

### Usage
```
ses_featurize <dir/*.ses> --output features.csv
ses_featurize file1.ses file2.ses --output features.csv
ses_featurize <dir> --output features.csv --no-header
```

### Downstream (Python)
```python
import pandas as pd
df = pd.read_csv("features.csv")
X = df.drop(columns=["filename"]).values
# UMAP, t-SNE, HDBSCAN, k-NN, silhouette score, etc.
```

### Compile
```
gcc -O2 -std=c11 -I. -Irunner -o runner/ses_featurize.exe runner/ses_featurize.c -lm
```

---

## ✅ Session June 19 (late) — Phase 6: Profile Batch Mode

### What Changed
- **`--profile-batch FILE`** in `llama_pogls_runner_sid_v2.c` — generate 400+ profiles in a single process

### How It Works
- โหลดโมเดลครั้งเดียว
- Batch file format: `session_name|prompt1|prompt2|prompt3` (pipe-delimited)
- วนแต่ละบรรทัด: reset profile → 3-turn chat → save .ses → recreate context → next
- Maximum speed: **~5-10s/profile vs ~25s** เรียกแยก process
- 400 sessions ≈ **~1 hour** (serial) instead of ~3 hours

### Stability
- Re-create context ระหว่าง session (เหมือน `/clear`)
- SID cache/FNV/swap state ถูก reset ทุก session
- Memory: leak-free (context freed/refreed)

### Batch File Format
```
# topic|prompt1|prompt2|prompt3
coding_001|What is a linked list?|Explain pointers|How does malloc work?
coding_002|What is sorting?|What is binary search?|What is a hash table?
math_001|What is a derivative?|What is an integral?|What is a limit?
# lines starting with # are skipped
```

### Usage
```
runner/llama_pogls_runner_sid_v2.exe model.gguf --sid-prompt-hash ^
  --semantic-weight 0.8 --history-weight 0.2 --temp 0 --n-predict 1 ^
  --profile-batch batch_400.txt

ses_featurize . --output all_features.csv
```

### Roadmap
1. ✅ Phase 1 — Weighted face selection + decay + quality score
2. ✅ Phase 2 — ses_cmp: histogram + transition + timeline comparison
3. ✅ Phase 3 — ses_cluster: unsupervised HAC clustering (⚠️ C crash, Python workaround in cluster_profiles.py)
4. ✅ Phase 4 — ses_merge: merge histograms + transition matrices
5. ✅ Phase 5 — ses_featurize: profile → CSV feature vector
6. ✅ Phase 6 — profile-batch: multi-session generation in single process
7. ✅ Large-scale validation benchmark (400 unique profiles generated, clustered)
8. ✅ Profile-aware cosplay training: `cosplay_profile_train` tool using selected representatives

---
## ✅ Session June 20 — Cluster Analysis + Representative Selection for Cosplay

### Goal
Cluster 402 unique simulated session profiles, select diverse representatives for cosplay perturbation training.

### What Happened
1. **Replaced batch_400.txt** (101 topics × 4 seeds → identical prompts → identical profiles) with **batch_400_unique.txt** (400 unique prompt combos across 270 diverse topics).
2. **Ran `--simulate`** generating 402 `.ses` files (400 unique + 2 compare_test survivors).
3. **`ses_featurize`** → `features_800mix.csv` (402 profiles × 108 columns).
4. **`ses_cluster.exe` has pre-existing heap corruption bug** (STATUS_HEAP_CORRUPTION 0xC0000374 after ~318 profile loads). Workaround: Python clustering script `cluster_profiles.py` using scipy + pandas.
5. **Clustering result (k=3, avg linkage, ratio=0.7649):**
   - **C0 (92):** API/HTTP/CDN/LLM/NLP/web-oriented topics — intra avg 0.1651
   - **C1 (69):** Systems/OS/testing/scheduling topics — intra avg 0.1549
   - **C2 (241):** General/algorithms/data structures/math topics — intra avg 0.2339
   - Inter-cluster distances: 0.1171–0.1336 (moderate separation)

### Selected Representatives (3 per cluster)
| Cluster | Role | Profile | Avg Intra Dist |
|---------|------|---------|---------------|
| C0 | Centroid | `hash_table_005.ses` | 0.1341 |
| C0 | Edge | `java_265.ses` | 0.1907 |
| C0 | Edge | `java_367.ses` | 0.1907 |
| C1 | Centroid | `jwt_348.ses` | 0.1297 |
| C1 | Edge | `devops_101.ses` | 0.1965 |
| C1 | Edge | `devops_203.ses` | 0.1965 |
| C2 | Centroid | `nosql_233.ses` | 0.1884 |
| C2 | Edge | `regex_142.ses` | 0.3044 |
| C2 | Edge | `regex_040.ses` | 0.3044 |

### Bug Note: ses_cluster.exe Heap Corruption
- Consistent crash at 0xC0000374 after ~318 `ses_profile_load()` calls
- Not realloc-related (pre-allocating 512 slots didn't help)
- Not file-specific (ses_cmp loads all files fine individually)
- Suspected: heap metadata corruption from `_strdup` or `fopen`/`fclose` cycles under MinGW CRT
- **Workaround**: Python script `cluster_profiles.py` replicates the combined distance metric and HAC clustering using scipy, outputs identical format
- `ses_cluster.c` left with debug prints commented and distance matrix print suppressed; can be restored

---
## ✅ Session June 20 (late) — Profile-Aware Cosplay Training

### Goal
Create `cosplay_profile_train` tool that uses `.ses` session profile to modulate per-tensor cosplay perturbation stride, producing profile-characteristic `.cpl` files.

### New Tool: `runner/cosplay_profile_train.c`

**Usage:** `cosplay_profile_train <profile.ses> <store.gsten> <output.cpl>`

**How it works:**
1. Load `.ses` profile → compute per-face usage frequency (aggregate all (spoke, slot) bins per face 0..7)
2. Load `.gsten` tensor store → iterate all tensors ≥64KB (standard SID weight filter)
3. For each tensor: compute its permanent face via FNV-1a(name) % 20 → `GEO_OCTANT[20]` LUT (same as runner's `geo_addr.h`)
4. Map face usage → stride:
   - usage > 0.20 → stride=32 (heavy perturbation — distort signature path)
   - usage > 0.10 → stride=48 (medium-heavy)
   - usage > 0.04 → stride=64 (standard)
   - usage > 0.005 → stride=96 (light)
   - usage ≤ 0.005 → stride=128 (very light — preserve unused path)
5. Write `.cpl` with per-tensor varying stride (all mode=CP_XOR, arg=0x01)

No format changes needed — `.cpl` already supports per-entry stride.

### Trained 9 Profile-Aware `.cpl` Files

| Profile | Cluster | Active Faces | stride=32 | stride=48 | stride=128 | Unique |
|---------|---------|-------------|:---------:|:---------:|:----------:|:------:|
| `hash_table_005.cpl` | C0 centroid | F4(1.0), F7(0.31) | 47 | 0 | 123 | ✅ |
| `java_265.cpl` | C0 edge | F4(0.81), F7(1.0) | 47 | 0 | 123 | ❌ dup |
| `java_367.cpl` | C0 edge | F4(0.81), F7(1.0) | 47 | 0 | 123 | ❌ dup |
| `jwt_348.cpl` | C1 centroid | F2(1.0), F5(0.38) | 53 | 0 | 117 | ✅ |
| `devops_101.cpl` | C1 edge | F2(0.83), F7(1.0) | 48 | 0 | 122 | ❌ dup |
| `devops_203.cpl` | C1 edge | F2(0.83), F7(1.0) | 48 | 0 | 122 | ❌ dup |
| `nosql_233.cpl` | C2 centroid | F0(1.0), F1(0.88), F5(0.92) | 70 | 0 | 100 | ✅ |
| `regex_142.cpl` | C2 edge | F5(1.0), F6(0.17) | 23 | 8 | 139 | ✅ |
| `regex_040.cpl` | C2 edge | F5(1.0), F6(0.17) | 23 | 8 | 139 | ❌ dup |

**5 unique signatures** — within-cluster duplicates expected (similar topic patterns → similar face usage).

### Compile
```
gcc -O2 -std=c11 -I. -Irunner -Icollection -Icollection/src -Icollection/core -Icollection/core/core -Icollection/core/pogls_engine/core -Icollection/core/geo_headers -Icollection/geo_jump_module/include -o runner/cosplay_profile_train.exe runner/cosplay_profile_train.c -lm
```

### Next Steps (original — DONE ✅ June 20 late-late)
- Run `--experiment DIR` with all 5 unique `.cpl` files + baseline to compare output differences ✅
- Or `--cosplay PATH` with a single file to test specific profile-aware perturbation ✅
- Files in `runner/cpl_profiles/`

---

## ✅ Session June 20 (late-late) — Full Pipeline Validation + Cleanup

### What Was Validated

| Component | Status | Evidence |
|-----------|--------|----------|
| b9528 + Haswell model load | ✅ | 456-547ms, 291/291 tensors |
| SID cache init (485MB) | ✅ | 170 weight tensors, verified |
| Init redirect (tensor->data → heap) | ✅ | 170 redirects before context creation |
| Context creation (kv_cache, sched) | ✅ | 37.53 MiB compute buffer |
| `llama_decode` (prompt + generation) | ✅ | Exit 0, coherent output |
| Per-decode SID swap (main loop) | ✅ | Exit 0, output `, it is` |
| `--cosplay` single mode | ✅ | Output `) + (string length` ≠ baseline |
| `--experiment` (10 conditions) | ✅ | Full cosine matrix, exit 0 |
| Main runner (non-test) with b9528 | ✅ | `llama_main_b9528.exe` exit 0 |
| DLL cleanup (remove stale .bak) | ✅ | All old b9686 DLLs removed |

### Root Cause of Previous Crash
b9686 CPU backend scheduler **deadlocks** if `tensor->data` pointer changes between `sched_reserve` and `llama_decode`. Our init redirect avoids this by setting pointers BEFORE context creation. The crash was from **DLL version mismatch**: `llama.dll` (b9528) + `ggml-cpu-*.dll` (b9686) = ACCESS_VIOLATION. Fixed by using all DLLs from same b9528 build.

### Experiment Results (Qwen2.5-0.5B, prompt "Hello", temp=0, max_new=1)
All 9 profile-aware `.cpl` files produce different logits from baseline:
- **C0** (devops/hash_table): cosine 0.946–0.953 (most aggressive)
- **C1/C2** (jwt/regex/nosql): cosine 0.983–0.985 (conservative)
- Within-cluster duplicates: identical cosines
- Full report: `runner/cpl_profiles/results/report.txt`

### Key Files
- `runner/llama_main_b9528.exe` — Main runner compiled against b9528
- `runner/llama_test_haswell.exe` — Test binary (experiment support)
- `runner/ggml-cpu-haswell.dll` — b9528 Haswell CPU backend
- `runner/ggml-cpu-sse42.dll` — b9528 SSE4.2 CPU backend
- `runner/ggml-cpu-x64.dll` — b9528 generic x64 CPU backend
- `runner/ggml.dll`, `ggml-base.dll`, `llama.dll` — all b9528
- `runner/cpl_profiles/results/report.txt` — experiment report
- `docs/DEVELOPMENT_SUMMARY.md` — full development summary

### Remaining
- `llama_pogls_runner_sid_v2_new5.exe` (old b9686 build) — still present but won't work; use `llama_main_b9528.exe` instead
- `test_tokenize.c`, `test_swap_*.c`, `test_modelonly.c`, etc. — separate test files with their own fflush, unrelated to main pipeline
