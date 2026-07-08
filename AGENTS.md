# AGENTS.md — Session Handoff

## 🐙 Git — ระวัง untracked files ก่อนเสมอ
- ใช้ `git status` เช็คสถานะก่อนทุกครั้ง
- **ระวัง untracked files เป็นอันดับแรก** — เช็คว่ามีไฟล์อะไรใหม่บ้างก่อนทำอะไรกับ git
- **Confirm ทุก action กับ user ก่อนเสมอ**:
  - `git add`, `git commit`, `git push`, `git reset`, `git restore`
  - โดยเฉพาะ `git clean` หรืออะไรก็ตามที่จะลบไฟล์
- **ห้ามลบหรือทำอะไรนอกเหนือจากที่คุยกันไว้**
- `git status` ใช้ได้โดยไม่ต้อง confirm

## 🛑 วงจรอุบาทว์ (Fix→Crash Loop) Protocol
เมื่อเข้า loop: แก้ → crash → แก้ → crash → แก้ → crash เกิน **3 รอบติด**:
1. **หยุดทันที** — อย่าแก้ต่อ
2. **ประเมินสถานการณ์**: ถาม user ว่า "เราควรเปลี่ยนแนวทางหรือลองอะไรต่อ?"
3. **ทบทวนสมมติฐาน**: อ่านไฟล์ที่เกี่ยวข้องทั้งหมดอีกครั้ง (ไม่ใช่แค่บรรทัดที่ crash)
4. **ใช้วิธีที่ง่ายที่สุด** ที่น่าจะใช้ได้ก่อน — อย่าเพิ่ม abstraction, indirection, หรือ fallback scan ที่ซับซ้อน
5. **เมื่อไม่แน่ใจ**: ใช้ printf/fprintf debug ทีละชั้นก่อน — อย่าเดา root cause

## 📋 Cross-Session Board (global skill)
Global skill `cross-session-board` ให้ board + context source tools ทุก workspace:
- `board_post/board_list/board_update/board_handoff` — track progress
- `source_register/source_load/source_unload/source_list` — loadable context
- ข้อมูลแยกตาม workspace อัตโนมัติ — ไม่ปนกัน

เมื่อเริ่ม session ใหม่ ให้ทำตาม **New Session Protocol** (ใน skill) โดยอัตโนมัติ

## Pre-scan check
- ก่อนเรียก scan() ให้เรียก get_project_index() เพื่อดูสถานะ cache
- หาก cache มีข้อมูลล่าสุด ให้ใช้ cached data แทนการสแกนใหม่
- หาก cache ไม่มีข้อมูลหรือล้าสมัย ให้สแกนตามปกติ

## 🧭 Ground Rule: มองรอบก่อนพุ่ง
เมื่อติดปัญหา — หยุด มองดูว่ามี asset อะไรในโปรเจ็กต์ที่เกี่ยวข้องอยู่แล้วบ้าง
โปรเจ็กต์นี้มีของดีผ่านการทดลองมามาก แต่บางอันถูกทิ้งเพราะมีอะไรดีกว่ามาแทน หรือยังไม่เจอเคสเหมาะ
**อย่า hardcode/scan ใหม่ ถ้ามีของที่ใช้ได้อยู่แล้ว**

## 🧠 Behavioral Rules

### 1. File Deletion — Strict Scoping
- **ห้ามลบไฟล์เด็ดขาด** ยกเว้น user สั่งโดยตรงแบบ explicit (written in stone)
- ถ้าต้องลบ ให้ลบเฉพาะที่ตรงกับ prompt ทุกประการ — ไม่เลยเถิดไปลบไฟล์อื่นแม้จะดู "เกี่ยวข้อง"
- เมื่อไม่แน่ใจ ให้ถาม user ก่อนทุกครั้ง

### 2. Loop Detection
- สังเกต pattern การวนซ้ำ: output ต่างกันแค่ space/whitespace, หรือพยายามแก้จุดเดิมซ้ำๆ โดยไม่ progress
- ถ้าเจอ ให้หยุด ถาม user ว่าควรเปลี่ยนแนวทางหรือไม่ (ต่อจาก Fix→Crash Loop Protocol ด้านบน)

### 3. Open Mind — ไม่ยึดติดโครงสร้างเดิม
- อย่าเอาแต่ใช้ pattern หรือ architecture เดิมซ้ำโดยไม่คิด
- มองหาความเป็นไปได้ใหม่ เสนอแนวทางที่แตกต่าง ถ้ามีเหตุผลรองรับ
- "We've always done it this way" ไม่ใช่เหตุผล

### 4. Deprecate Before Delete
- ไฟล์ .c / .h / .py ที่ไม่ได้ใช้แล้ว → ย้ายไป `deprecated/` แทนการลบ
- รักษาโครงสร้างโฟลเดอร์เดิมใน `deprecated/` เพื่อให้ traceability
- ไฟล์ที่ย้ายแล้วให้ update include/import paths หรือแจ้ง user

### 5. Convert Important Notes to Docs
- .txt, log notes, หรือข้อความสำคัญ → แปลงเป็น .md เก็บใน `docs/`
- ตั้งชื่อสื่อความหมาย ไม่ซ้ำซ้อน
- อย่าทิ้งข้อมูลสำคัญไว้ใน raw text/log โดยไม่มีโครงสร้าง

---

## สถานะระบบปัจจุบัน (June 29, 2026)

### ✅ Pipeline หลัก — ทั้งหมดผ่าน
| Component | Status |
|---|---|
| SID cache init + per-decode swap | ✅ |
| Cosplay perturbation (`.cpl`) | ✅ |
| `--cosplay-compare` WITH/WITHOUT | ✅ |
| `--experiment` multi-delta framework | ✅ |
| Session profiles (SES2) | ✅ |
| `ses_cmp/cluster/featurize/merge` tools | ✅ |
| Profile batch mode (`--profile-batch`) | ✅ |
| Profile-aware cosplay training | ✅ |
| KV state perturbation | ✅ |
| **KV SID eviction (pointer swap)** | **✅ (verified: Bob test)** |
| **Incremental pending token-suffix** | **✅ (prefix-match delta decode)** |
| **`--script FILE` chat input** | **✅ (deterministic scripted testing)** |
| Time travel (delta ring + rewind/ffwd) | ✅ |
| Bond prediction (`--bond`) | ✅ |
| Capture pipeline (`--capture`) | ✅ |
| Gear lock feedback (`--gear-lock`) | ✅ |
| Icosa bridge GPU context fix | ✅ |
| Y-triangle migration (geo_jump) | ✅ |
| Vulkan GPU backend | ✅ |
| b9733 + LFM2 support | ✅ (DLLs + runner verified working) |
| Colab deploy script | ✅ (`deploy/colab/build_colab.sh`) |
| `_hilbert_idx` warning | ✅ ไม่มี warning นี้ใน code ปัจจุบันแล้ว |
| **KV Remap (skeleton+delta)** | **✅ adaptive 3-tier system tested** |
| **KV Remap Rail (background verify)** | **✅ layer-based segment chunking** |
| **KV Remap runner integration (`--remap`)** | **✅ integrated into runner + rail freeze/resume** |
| **KV Remap Shadow Zone (`--shadow`)** | **✅ delta metadata heartbeat via shadow_zone.h** |
| **SID+GPU DeviceLost fix** | **✅ (tensor_update_data with ggml_backend_tensor_set)** |
| **SID restore GPU crash fix** | **✅ (delta_orig_data instead of GPU pointer)** |
| **DRamTile benchmark** | **✅ (CPU: -36%, GPU: +38%, 5-face: -23%)** |
| **GearShift (Tier-2 streaming router)** | **✅ generic src→dst, no data storage** |
| **GearLock→GearShift priority sync** | **✅ sync_gearlock_to_gearshift + gs_stream_pending_prioritized** |

### ✅ July 2 — POGLS v2 Compression Benchmark + gguf_to_pogls Upgrade

- **Compression benchmark** (`test_pogls_compress.c`): 33/33 PASS on 512MB synthetic Q4 + 8MB norms + 512KB biases:
  - **Q4 quantized weights: 1.00×** — zstd, binary shell (0.91×), diamond shell (0.97×) ALL fail. Q4 residuals look like random noise at byte level → **store raw, skip compression**.
  - **Dense f32 norms: 1.00×** — uniformly distributed, incompressible.
  - **Sparse biases: 3.30×** (zstd), **2.30×** (binary shell) — compress well.
  - **Strategy**: try zstd per tensor, keep if ratio ≥ 1.1, else RAW. Binary shell/diamond shell never outperform zstd — removed from compression path.
- **`pogls_meta.h` updates**: added `pogls_compress_tensor()` / `pogls_decompress_tensor()` — auto-detect compressibility, zstd or raw. Guarded by `#define POGLS_USE_ZSTD`.
- **`gguf_to_pogls.c` rewrite**: standalone GGUF reader (no llama DLL), writes v2 `.pogls` with metadata + optional `--compress` flag. Fixed `POGLS_MAX_ADDR` double-define, `data_pos`/`src_pos` separate tracking.
- **All tests**: 99/99 PASS (42 meta + 33 compress + 20 priority_dram + 4 gguf_to_pogls build).

### ✅ July 8 — DGFS Drive: WinFsp FUSE + DRamTile = Persistent virtual drive X:\
- **Root cause of write failures**: `dirlist_add()` called `dt_put(DIRLIST_KEY, buf, new_sz)` but `dt_put` rejects size changes for existing entries (`old_sz != new_sz → return NULL`). Fix: `dt_free(&g_store, DIRLIST_KEY)` before every dirlist update.
- **`statfs` block count bug**: `f_blocks = capacity` (bytes) with `f_bsize = 4096` reported 35 TB storage (overflow in 32-bit calc). Fix: divide by 4096: `f_blocks = capacity / 4096`.
- **Delayed write bug**: `open()` only checked `dt_get(key)` which returns NULL for zero-size entries. Fix: `create`/`mknod` now stores a zero-size `dt_put` entry + dirlist entry so `open` finds it.
- **Verified**: Create, write (same-size + resize), overwrite, delete, readback, persistence across unmount/remount — all work on X:\.
- **`dgfs-launcher.ps1`**: PowerShell WinForms GUI for Drive Mode (mount X:\) vs LLM pass-through mode.

- **Face tables fixed**: `TRIPLET_FACE_VERTS`, `POCKET_FACE_VERTS` corrected to use actual plane-equation-derived CCW edge-connected vertex sets (not goldberg_sid.h's region-based grouping). Faces 9-12 had z-plane errors (wrong φ sign) — fixed.
- **POCKET_FACE_CENTERS corrected**: swapped faces 10/11 to match plane equations (z-φx=+φ² at (-1.1708, 0, 0.7236), z-φx=-φ² at (1.1708, 0, -0.7236)).
- **ADJ table rebuilt**: `pocket_cross_edge()` adjacency completely recomputed for the new face numbering, verified bidirectional (ADJ[f][e] → neighbor AND ADJ[neighbor][entry] → f).
- **Pentakis triangle table fixed**: `TRIPLET_PENTAKIS_TRI` reordered to match new CCW face orders.
- **Icosphere f=4 position table**: 162-vertex pre-computed `TRIPLET_ICOSPHERE_POS` table (Python-generated from icosa subdivision, projected to face-center sphere at R≈1.376). Plus `TRIPLET_ICOSA_FACES_TABLE` (20 faces) and `TRIPLET_ICOSA_EDGES` (30 edges) for topology queries. Added `triplet_icosphere_pos()` lookup function.
- **Tests**: 51/51 PASS (was 42, added 9 new icosphere/icosa table tests).
- **Philosophical insight**: Triplet world = geometric address space for zero-copy pipeline (disk→RAM→CPU→GPU); Hidden pocket = persistent alternate-dimension storage accessible within the world. Recorded on session board.

### ✅ (June 30 — Page Table SID: Zero-Copy Indirect Layer + GPU Twin Buffer Swap)
- **Page Table SID** (`runner/sid_page_table.h` + `--sid-pt` flag): 20736-bit indirect layer (2592 bytes) replaces 5.1GB sid_cache + DRamTile preload with zero-copy bit flip
- **CPU zero-copy verified**: `--sid-pt --sid-face 1` → apply=0.01ms decode=469ms restore=0.02ms (no alloc/copy, just bit flip)
- **Baseline benchmark**: `--sid-pt` decode 365ms vs baseline 366ms — zero overhead confirmed
- **GPU twin buffer swap**: pre-uploads 74 face twins (`ggml_backend_buffer` per tensor), atomic pointer swap of `tensor->buffer` + `tensor->data` per decode — no memcpy on hotpath
- **GPU test**: `--sid-pt --sid-face 1 --sid-force --ngl 8` → 74 twins pre-uploaded, 256 tok decoded at 353ms avg, clean exit
- **Bugs fixed**: function ordering, `ggml_init_params.no_alloc=1`, `ggml_backend_tensor_alloc` non-NULL addr, stack overflow `-Wl,--stack,16777216`, AV crash (lazy data load skip)
- **Blocked**: GPU VRAM (GTX 1050 Ti 4GB) — 253 tensors × ~215MB each ≈ 54GB twin VRAM impossible at scale. Needs lighter model or fallback to `ggml_backend_tensor_set` per decode
- **New file**: `runner/sid_page_table.h` (bitmap + journal + GPU fields)
- **Inbox MCP server** — เปิดใช้งานผ่าน `opencode.jsonc` แล้ว (31 tools, vault พร้อม)

## 📁 Key Binaries (ใน `runner/`)
- `llama_pogls_runner_sid_v2.exe` — Main runner (b9528)
- `llama_b9733.dll` + `ggml*.dll` (b9733) — พร้อมใช้งาน
- `cosplay_train.exe`, `cosplay_profile_train.exe`
- `ses_cmp.exe`, `ses_cluster.exe`, `ses_featurize.exe`, `ses_merge.exe`
- `test_kv_remap.exe` — KV Remap test suite (7 tests, all pass)
- 403 `.ses` profiles ใน `runner/ses_profiles/`
- 9 `.cpl` cosplay profiles ใน `runner/cpl_profiles/`
- `bench_sid.ps1` — SID+DRamTile benchmark script

### 🔑 Key Design: Page Table Indirect (June 30)
- **Problem**: SID ใช้ RAM ~15GB + memcpy หนัก — ขัดกับ geometry zero-copy design
- **Root cause**: implementation drift — `sid_cache` + `DRamTile` + `tensor_set_data()` + `ggml_backend_tensor_set()` ทั้งหมด copy data ไม่ใช่ indirect
- **Solution**: 20736-bit page table (2592 bytes) — `bit[N]=0`→orig, `=1`→face. CPU/GPU indirect เดียวกัน
- **GPU twin buffer**: pre-upload `ggml_backend_buffer` per tensor (`ggml_dup_tensor` + `ggml_backend_tensor_alloc` + `ggml_backend_tensor_set`), atomic pointer swap of `tensor->buffer` + `tensor->data` per decode — no memcpy on hotpath
- **สมการแกน**: `128×162 = 144×144 = 20736` — base2×base3 = Fibonacci² — DRamTile/Y-triangle/GPU buffer share address space
- **Reference**: `docs/sid-page-table-design.md`

### ✨ June 28 — VRamTile Integration + GPU Offload Bugfix

- **VRamTile runner integration** (`vramtile.h` + `llama_pogls_runner_sid_v2.c`):
  - `vrt_init_external()`: VRamTile reads from existing `g_dramtile` instead of owning its own store (`external_src` field)
  - `--vram MB` CLI flag, `g_vrt` + `g_gpu_worlds` globals
  - Upload callback `vrt_upload_gpu()` via `g_ibridge.memcpy_h2d()` for real GPU upload
  - `sid_swap_apply_ex()` wired with `vrt_promote()` for each SID swap cycle
  - `twin_gpu_gear_push()` wired with `vrt_evict_gear()` using `g_gpu_worlds` counter
  - `/vrt` chat command for stats
  - Builds with 0 errors
- **Critical bugfix**: DRamTile crashed with `--ngl` (GPU offload) because `tensor->data` points to GPU device memory after `llama_load_model_from_file()`. Fix: when `opt_ngl > 0`, read tensor data from GGUF file directly (using `gidx` offsets) instead of `orig_data` pointer.
- **Verified**: `--dramtile --ngl 24` exit 0, answer "4". `--dramtile --vram 128 --ngl 24` exit 0, answer "4". VRamTile 9/9 tests pass, DRamTile 166/166 tests pass.

### ✨ June 28 — SID+GPU Bugfix + DRamTile Benchmark

- **SID+GPU DeviceLost fix** (`llama_pogls_runner_sid_v2.c`): `tensor_set_data()` changes `tensor->data` → breaks `vk_tensor_offset()` for GPU tensors. **Fix**: `tensor_update_data()` detects GPU tensor via `t->buffer && !ggml_backend_buffer_is_host(t->buffer)` → uses `ggml_backend_tensor_set()` instead of pointer swap. DeviceLost resolved.
- **SID restore GPU crash fix**: `sid_swap_restore_ex()` used `found_tensors[fi].orig_data` (GPU device pointer) directly → `ggml_backend_tensor_set` crashed trying to memcpy from GPU address. **Fix**: passes `delta_orig_data[i]` (DRamTile CPU copy) instead.
- **DRamTile benchmark** (`runner/bench_sid.ps1` — LFM2.5-8B-A1B-Q4_K_M.gguf):
  | Benchmark | Time | vs Baseline |
  |---|---|---|
  | CPU: Baseline (no SID) | 113.64s | — |
  | CPU: SID | 101.32s | -11% |
  | **CPU: SID + DRamTile** | **73.03s** | **-36% 🏆** |
  | GPU: SID | 1.65s | — |
  | GPU: SID + DRamTile | 2.27s | +38% |
  | 5-face CPU: SID | 4.30s | — |
  | **5-face CPU: SID + DRamTile** | **3.32s** | **-23% 🏆** |
  - **CPU: DRamTile เร็วกว่า** — VirtualAlloc/mmap overhead น้อยกว่า heap allocator สำหรับ tensor 5 GB
  - **GPU: DRamTile ช้ากว่า** — ต้อง memcpy DRamTile → GPU buffer (extra copy)
  - **5-face warm cache: DRamTile เร็วกว่า** — cache reuse + allocation efficiency

### ✨ June 28 — DRamTile Cold Migrate + Eviction + KV Compose
- **Phase 10**: `dt_migrate_step()` / `dt_migrate_promote_one()` — promote bond entries from cold back to primary (LRU sort by session_tick)
- **Phase 11**: `dt_evict_step()` — LRU evict oldest cold entries (lowest session_tick)
- **Phase 12**: `dt_cold_make_room()` — auto-evict before cold alloc when full
- **Phase 13**: `kv_compose()` — KV spill to cold when kv_base full, transparent read/write via dt_get/dt_put_kv
- **Bugs fixed**:
  - `dt_store_destroy()`/`destroy_twin()`/`destroy_twinv()`: cold_base cleanup now checks `is_cold_twin` (UnmapViewOfFile+CloseHandle vs VirtualFree)
  - `dt_cold_rebuild_used()` moved before `dt_store_init_cold_twin()` to fix implicit declaration
  - `4UL * 1024 * 1024 * 1024` overflow on Windows (unsigned long = 32-bit) → simplified to use caller's max_bytes directly
- **Tests**: 68 → 97 tests, all pass
- **Docs**: updated `docs/dramtile.md` with cold spill, migrate, eviction, KV compose, 3-tier hierarchy

### 📁 KV Remap Files (สร้างวันนี้)
- `runner/kv_remap.h` — Adaptive skeleton+delta (RLE compressed, self-contained)
- `runner/kv_remap_rail.h` — Rail layer-based segment chunking (per-layer scan/patch)
- `runner/test_kv_remap.c` — Full test suite (7 tests, all pass)
- `docs/remap.md` — User-facing documentation for --remap

---

## Next Scope: GPU Performance + Twin-GPU + VRamTile

### Short-term Remaining Work
1. **Wire `--twin-gpu` with real `icosa_bridge.dll`** — verify `cudaMemcpy` H2D path (currently simulation memcpy)
2. **Fix `-O2` strict aliasing UB in tensor memory scanner** — for consistent optimized builds (currently use `-O0` or `-fno-strict-aliasing`)
3. **Gear 2: Direct GPU buffer write** — bypass `ggml_backend_tensor_set()` for GPU tensors by using saved Vulkan buffer offset + HOST_VISIBLE mapped address

### Key Files
- `runner/vramtile.h` — VRamTile GPU cache (deprecated, kept for compat)
- `runner/gear_shift.h` — Generic streaming router (Tier-2)
- `runner/gear_lock.h` — GearLock state machine (priority/speed control)
- `runner/llama_pogls_runner_sid_v2.c` — Main runner with SID+GPU
- `runner/dramtile_store.h` — DRamTile API (Tier-1 storage)
- `runner/kv_page_store.h` — KV Page Store (scatter/gather patterns)
- `runner/kv_page_gearshift.h` — KV + GearShift integration
- `runner/bench_sid.ps1` — Benchmark script
- `collection/src/icosa_twin_bridge.h`/`.cu` — GPU memory ops (real upload path)

### Model
- `I:\model\LFM2.5-8B-A1B-Q4_K_M.gguf` — 8B Q4 model for testing (5.15 GB)

---

## Session History (สรุปย่อ)

### June 29 — `--sid` Flag Fix + GPU Tensor `delta_orig_data` Crash Fix

- **`--sid` flag fix**: Added `sid_face=1` handler for bare `--sid` flag. Previously only `--sid-face N` worked — `--sid` was silently ignored.
- **GPU tensor crash fix** (`llama_pogls_runner_sid_v2.c:1378`): When `--ngl > 0`, `found_tensors[i].orig_data` points to GPU device memory. `sid_swap_restore_ex()` calls `ggml_backend_tensor_set()` with this pointer via `delta_orig_data` → ACCESS_VIOLATION. **Fix**: detect GPU tensor via `t->buffer && !ggml_backend_buffer_is_host(t->buffer)`, use `cached` (CPU-readable) instead of `orig_data` for restore.
- **DRamTile + `--ngl`**: DRamTile init fails with `--ngl` (VirtualAlloc 5.1 GB fails after GPU buffer allocation). Pre-existing, not fixed this session. SID falls back to lazy GGUF load — works correctly.
- **Verified**: `[sid] 251 / 251 tensors will be swapped per decode`, `[sid] lazy progressive: start 50/251, +50 per decode`. No crash, no ACCESS_VIOLATION, no DeviceLost.
- **Reference doc**: `docs/sid-gpu-integration.md` — detailed root cause, fix logic, DRamTile limitation, and architecture notes for future sessions.

### June 29 — GearLock→GearShift Priority Sync + KV Page GearShift Integration

- **GearLock→GearShift priority wiring** (`gear_shift.h` + `llama_pogls_runner_sid_v2.c`):
  - `gs_stream_pending_prioritized()`: streams pending entries sorted by priority (highest first), using insertion sort on index array
  - `sync_gearlock_to_gearshift()`: reads `gear_lock_score()` per tensor and writes to `GSEntry.priority`
  - Called after `gear_lock_update()` in `twin_gpu_gear_push()` — priorities synced every SID swap cycle
  - `kv_page_gearshift.h`: KV Page Store + GearShift pipelined scatter/gather (3 lanes × 2 layers)
- **Verified**: build clean (0 errors), `test_kv_page_gearshift.exe` 64-page benchmark passes
- **Key insight**: At 64 pages, DRamTile hash lookup is 10x slower than array scan (0.98ms vs 0.10ms). DRamTile wins at scale (1000+ entries), not at small counts.

### June 28 — VRamTile Integration + GPU Offload Bugfix + Benchmark

- **VRamTile runner integration** (`vramtile.h` + `llama_pogls_runner_sid_v2.c`):
  - `vrt_init_external()`: VRamTile reads from existing `g_dramtile` instead of owning its own store (`external_src` field)
  - `--vram MB` CLI flag, `g_vrt` + `g_gpu_worlds` globals
  - Upload callback `vrt_upload_gpu()` via `g_ibridge.memcpy_h2d()` for real GPU upload
  - `sid_swap_apply_ex()` wired with `vrt_promote()` for each SID swap cycle
  - `twin_gpu_gear_push()` wired with `vrt_evict_gear()` using `g_gpu_worlds` counter
  - `/vrt` chat command for stats
  - Builds with 0 errors
- **Critical bugfix**: DRamTile crashed with `--ngl` (GPU offload) because `tensor->data` points to GPU device memory after `llama_load_model_from_file()`. Fix: when `opt_ngl > 0`, read tensor data from GGUF file directly (using `gidx` offsets) instead of `orig_data` pointer.
- **Verified**: `--dramtile --ngl 24` exit 0, answer "4". `--dramtile --vram 128 --ngl 24` exit 0, answer "4". VRamTile 9/9 tests pass, DRamTile 166/166 tests pass.

### June 28 — SID+GPU Bugfix + DRamTile Benchmark

- **SID+GPU DeviceLost fix** (`llama_pogls_runner_sid_v2.c`): `tensor_set_data()` changes `tensor->data` → breaks `vk_tensor_offset()` for GPU tensors. **Fix**: `tensor_update_data()` detects GPU tensor via `t->buffer && !ggml_backend_buffer_is_host(t->buffer)` → uses `ggml_backend_tensor_set()` instead of pointer swap. DeviceLost resolved.
- **SID restore GPU crash fix**: `sid_swap_restore_ex()` used `found_tensors[fi].orig_data` (GPU device pointer) directly → `ggml_backend_tensor_set` crashed trying to memcpy from GPU address. **Fix**: passes `delta_orig_data[i]` (DRamTile CPU copy) instead.
- **DRamTile benchmark** (`runner/bench_sid.ps1` — LFM2.5-8B-A1B-Q4_K_M.gguf):
  | Benchmark | Time | vs Baseline |
  |---|---|---|
  | CPU: Baseline (no SID) | 113.64s | — |
  | CPU: SID | 101.32s | -11% |
  | **CPU: SID + DRamTile** | **73.03s** | **-36% 🏆** |
  | GPU: SID | 1.65s | — |
  | GPU: SID + DRamTile | 2.27s | +38% |
  | 5-face CPU: SID | 4.30s | — |
  | **5-face CPU: SID + DRamTile** | **3.32s** | **-23% 🏆** |
  - **CPU: DRamTile เร็วกว่า** — VirtualAlloc/mmap overhead น้อยกว่า heap allocator สำหรับ tensor 5 GB
  - **GPU: DRamTile ช้ากว่า** — ต้อง memcpy DRamTile → GPU buffer (extra copy)
  - **5-face warm cache: DRamTile เร็วกว่า** — cache reuse + allocation efficiency

### June 28 — DRamTile Dual-Region + KV Ephemeral Fixes + Type-Safe Container Test
- **Bugs fixed in `dramtile_store.h`**:
  - `dt_store_save_dir()`: was using `store->n_stored` (includes KV entries) as entry count → header had wrong count (e.g. 3 for 1 weight + 2 KV), causing directory corruption on reopen. **Fix**: count non-KV entries separately.
  - `dt_store_destroy_twin()`/`destroy_twinv()`: missing `kv_base` cleanup (VirtualFree/munmap). **Fix**: added kv_base unmap after file mapping cleanup.
  - `dt_resolve()`, `dt_getv()`, `dt_view()`: used direct `store->base + offset` (wrong for KV) and exact `dram_addr` comparison (failed for KV entries). **Fix**: use `dt_entry_ptr()` and `(stored & ~DT_KV_FLAG)` mask.
  - `dt_store_foreach()`: iterated KV entries (count=3 instead of 1). **Fix**: added `DT_KV_FLAG` skip.
  - `dt_store_total_bytes()`: counted KV bytes. **Fix**: added `DT_KV_FLAG` skip.
- **New tests** (test_dramtile_twin.c → 49 tests, all pass):
  - Phase 6: Dual-region — write 1 weight + 2 KV entries, verify KV accessible in-session, NOT persisted on reopen, foreach/total_bytes exclude KV. **✅**
  - Phase 7: Type-safe container — `dtc_wrap()`, `dtc_f32_2d()` element access, `dtc_slice()`, `dtc_flatten()`, `dtc_ptr()` with float32 data. **✅**
- **Key design principle**: KV entries use `DT_KV_FLAG` (0x80000000) in `dram_addr` to select `kv_base` vs `base`. All lookup functions must mask the flag. Directory save/foreach/total_bytes explicitly skip KV entries.

### June 26 — KV Remap System (Adaptive Skeleton+Delta + Rail)
- **New system**: Adaptive 3-tier KV cache management:
  - 0-15% change → ENTROPY (XOR + RLE compressed delta)
  - 15-85% change → GEO (byte-offset ranges)
  - 85%+ change → REBUILD (flush + new skeleton)
- **Files created**:
  - `runner/kv_remap.h` — Adaptive skeleton+delta (self-contained, no zstd dependency)
  - `runner/kv_remap_rail.h` — Rail 3-lane background scan (idle-driven, freeze/resume)
  - `runner/test_kv_remap.c` — 7 tests, all pass
- **Key insight**: Quantize ≠ Geometry but Topology is very similar — they complement, not compete
- **Geometry mapping**: 6 attention layers = 6 triangles = 1 hexagon (model unit)
- **Train station metaphor**: Each attention layer = one station, rail = connection between stations
- **Bug fixed**: classify() offset calculation (was assuming [K0,K1,...,V0,V1,...] but actual layout is [K0,V0,K1,V1,...])
- **Bug fixed**: rand() on Windows/MinGW only returns 15-bit values — replaced with xorshift32 for full 32-bit range

### June 26 — KV Remap Runner Integration + Layer-based Segment Chunking
- **Integrated `--remap` into runner** (`llama_pogls_runner_sid_v2.c`):
  - Added `#include "kv_remap.h"` + `"kv_remap_rail.h"`
  - `--remap` flag: enables adaptive skeleton+delta with rail background scan
  - Init: registers KV tensors via `kv_get_cache_tensors`/`kv_get_cache_tensor_ptrs`, sets skeleton baseline
  - Rail freeze/resume around every `llama_decode()` call (chat + prompt paths)
  - Idle step: `kv_remap_rail_step()` + `kv_remap_cycle()` after each generation response
  - Chat commands: `/rstatus` (print remap+rail status), `/rscan` (trigger full scan)
  - Cleanup: `kv_remap_rail_destroy()` + `kv_remap_destroy()` at exit
- **Layer-based segment chunking** (rewrote `kv_remap_rail.h`):
  - Old: 3 lanes split total KV bytes into flat thirds, scan walks byte-by-byte with layer lookup
  - New: 3 lanes assigned to actual layers (`RAIL_LAYERS_PER_LANE=2`), scan walks `layers[l].k_data` → `layers[l].v_data` directly
  - Patch decompresses skeleton and writes back per-layer K/V chunks (no flat offset math)
  - RailLane struct: `layer_start`, `layer_end`, `cur_layer`, `cur_phase` (0=K, 1=V) instead of flat byte ranges
- **Windows compatibility fix**: `POGLS_RAIL_USE_POGTIME` macro resolves conflict between runner's custom `clock_gettime(PoglsTime*)` and rail's `struct timespec`
- **Build**: runner compiles+links clean (0 errors), `test_kv_remap.exe` 7/7 tests pass

### June 27 — Shadow Zone → KV Remap Integration
- **Connected shadow zone to KV remap hotpath** (`kv_remap.h`):
  - Added `#ifdef KV_REMAP_USE_SHADOW` guard with `shadow_zone.h` include
  - `kv_remap_init_shadow()` — init shadow zone A for delta metadata heartbeat
  - `kv_remap_store_delta()`: after heap store, packs delta metadata (type, pct, sizes, ranges) into a 64-byte DIAMOND_BLOCK packet and writes to shadow zone via `shadow_write()` with bond_key
  - `kv_remap_restore()`: checks `shadow_find_by_bond()` for alive delta metadata
  - `kv_remap_rebuild()`: frees shadow bond key on rebuild (`shadow_free()`)
  - `kv_remap_destroy()`: cleanup shadow zone on exit
- **Runner integration** (`llama_pogls_runner_sid_v2.c`):
  - `#define KV_REMAP_USE_SHADOW 1` before `#include "kv_remap.h"`
  - `g_opt_shadow` flag + `--shadow` CLI arg + help text
  - `kv_remap_init_shadow()` called after rail init when `--shadow` is set
  - Shadow reinit in `/clear` handler
- **Verification**: runner compiles clean (0 errors), `test_kv_remap.exe` 7/7 tests pass
- **Key design**: Shadow zone stores only delta metadata (19 bytes → padded to 64), not full delta data. Acts as heartbeat/registry — alive check tells whether delta was evicted. Shadow zone capacity (1728 slots × 64B) naturally manages delta lifetime across context switches.

### June 26 — KV Tensor Access Architecture-Agnostic (Hybrid fix)
- **Root cause**: LFM2 is hybrid architecture (`llama_memory_hybrid`) — `dynamic_cast<llama_kv_cache*>` fails because the actual type is `llama_memory_hybrid` (not `llama_kv_cache`)
- **Fix**: `resolve_kv()` in `kv_tensor_access.cpp` tries multiple memory types:
  1. `dynamic_cast<llama_kv_cache*>` — works for standard transformers (Qwen2.5, etc.)
  2. `dynamic_cast<llama_memory_hybrid*>` → `get_mem_attn()` — works for hybrids (LFM2, Qwen3.5)
  3. `dynamic_cast` to `llama_kv_cache_dsa*` — placeholder for DeepSeekV3
- **Verification**:
  - Qwen2.5-0.5B: `14llama_kv_cache`, 24 layers ✅
  - LFM2.5-1.2B: `19llama_memory_hybrid`, 6 attention layers ✅
- **Build change**: `gcc -m64` used as linker driver instead of `ld` (auto-finds CRT objects), requires `libllama.dll.a` import lib generated via `gendef` + `dlltool`
- **Key insight**: LFM2 layers alternate attention/recurrent via `hparams.is_recr_impl[il] = (n_head_kv == 0)`. Only attention layers (6 of 16 for LFM2 1.2B) have KV cache tensors

### June 25 — KV SID Eviction VERIFIED + Incremental Pending + --script
- **Root cause found**: chat loop clears/refills full conversation every turn via `llama_memory_seq_rm(0,-1,-1)` — KV eviction had no visible effect
- **Incremental "pending" token-suffix pattern**: `pending_toks` buffer stores full prompt tokens, prefix-matched via `tok_prefix_len()`, only delta decoded on match
- **`--script FILE` option**: non-interactive deterministic chat input via `fopen` + `fgets`
- **Helper functions**: `tok_prefix_len()`, `tokbuf_reserve()`, `tokbuf_copy()`, `tokbuf_append()`
- **Build pipeline**: C11 compile with `gcc -std=c11` + link with `ld.exe` (bypasses `collect2` error 53)
- **MSYS2 PATH fix**: `cc1plus.exe` needs `C:\msys64\mingw64\bin` in PATH to load DLLs
- **KV Eviction CONFIRMED working** (Bob test):
  - No eviction: "What is my name?" → **"Your name is Bob."** ✅
  - After `/evict 24`: "What is my name?" → **"Sure, is there a job offer?"** ❌ (forgot)
  - All 24 layers swapped to backup via `tensor->data` pointer swap; originals poisoned 0xDE/0xAD
- Build command: `gcc -O2 -std=c11 -I. -I../collection ... -c -o llama_pogls_runner_sid_v2.o`
  Then: `ld.exe -m i386pep -Bdynamic --stack 16777216 ... --start-group -lstdc++ ... --end-group`
- **Alternative link (after June 26)**: `gcc -m64 -O2 -o runner.exe main.o kv_tensor_access.o -L. -llibllama -lggml -lggml-base -lggml-cpu -lggml-vulkan -lstdc++`

### June 22 — DGLS Integration + Diamond Shell FLAT Bugfix + SID Cache Compression
- Integrated DGLS components into FGLS_new: 6 new files + 3 updated files
- Full DGLS directory copy at `collection/dgls/` (mirror)
- Fixed `binary_shell_codec.h` missing include + `diamond_shell_codec.h` FLAT misclassification
- Benchmarks: DRam 2122 MB/s, Shell 4-5x ratio/lossless, Q4 Weights **1.88x/1400 MB/s decode**
- **SID Cache compression**: Added `sid_cache_put_compressed()` + transparent decompression in `sid_cache_get()`. Verified: Q4 weights → **1.88x compression**, lossless PASS. Random data → falls back to raw. Modified `sid_loader_load` to use compression. Added include paths to runner Makefile.
- All 35 DGLS pipeline tests pass

### June 20 — Cosplay Fix + Experiment + LFM2
- Fixed `CP_MAGIC` (0x504F434C), implemented `--cosplay-compare` + `--experiment`
- Profile-aware cosplay trainer + 5 unique `.cpl` files
- b9733 + LFM2.5-1.2B tested: 65.4 t/s on Vulkan
- Full pipeline validation: all components pass

### June 19 — SES2 + Cosplay Profile Training
- SES2 format with transition matrix + timeline
- Phase 1-6: weighted face selection, quality metrics, cmp/cluster/merge/featurize/batch
- Profile-aware cosplay training: stride modulation from face usage
- 400+ session profiles generated + clustered (k=3)
- cosplay_profile_train tool

### June 18 — ZoneCardSID Y-Triangle + SID E2E
- Retargeted ZoneCardSID to Y-triangle (GEO_FULL=20736)
- TensorMemStore with TMEM_DELTA compression
- SID E2E: 291/291 tensors, 170 weight swaps, decode at 65+ t/s
- Icosa bridge GPU context fix: CUDA error 400 resolved

### June 17 — Y-Triangle Migration
- Removed TETRA/OCTA compound addressing
- Unified to geo_jump Y-triangle (GEO_FULL=20736)
- Qwen3 TTS pipeline: bf16 fixes NaN, manual safetensors load

### June 15-16 — Milestone v1.0
- 12-Face Bridge Pipeline complete
- Capture pipeline + freeze wallet + gsten store
- SID coordinate system + time travel delta ring
- Bond prediction: cardioid express + Metatron topology
