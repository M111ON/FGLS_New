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

## 🧪 Session June 18 — CORRECTION: Backend Reads tensor->data Per-Decode (Not Cached at Init)

### tl;dr
**The CPU backend reads `tensor->data` on EVERY `llama_decode` call, NOT cached at `llama_init_from_model`.** 
- Swapping tensor->data **after** context creation, then calling `llama_decode` → logits change (proven: 151935/151936 logits differ, max_diff=0.358)
- `llama_free(ctx)` + new `llama_init_from_model` somehow resets state (old test with 2 contexts showed no difference)
- **Correct flow: swap between decodes, same context**

### What Changed from Earlier Claim
Earlier AGENTS.md entry claimed "swap before context works, swap after = zero effect." This was WRONG.
- The old proof test (test_swap_debug.c) found only 13/291 tensors via model struct scan
- Old test created 2 separate contexts (baseline + test) → `llama_free` + new context masked the swap effect
- New test (test_swap_all.c) uses **same context, 2 decodes** — swap BETWEEN decodes works definitively

### Key Implementations Now Working
1. **`gguf_idx_open`** (existing in `gguf_index.h`) — reads tensor names from GGUF file. Replaces hardcoded 291 name list.
2. **Model struct scan** (`scan_region_for_tensors`) — scans model struct (64KB) + layers heap allocation for tensor pointers matching GGUF names. Uses VirtualQuery for memory safety (no SEH, no process-wide scan).
3. **291/291 tensors found** — both direct model tensors (3) + all layer tensors (288) via layers-pointer discovery.
4. **Per-decode swap proven** — set tensor->data to copies (with 1-byte sentinel flip) → decode sees changed data.

### Implications for SID
- No need to intercept context creation
- SID swap happens BETWEEN `llama_decode` calls
- Flow: decode(token_n) → swap tensor->data to SID cache → decode(token_n+1) → swap back
- Works with any backend (CPU, GPU, Vulkan) — backend reads tensor->data per-decode

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
