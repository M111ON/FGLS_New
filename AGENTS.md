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

## สถานะระบบปัจจุบัน (June 27, 2026)

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

### 📁 Key Binaries (ใน `runner/`)
- `llama_pogls_runner_sid_v2.exe` — Main runner (b9528)
- `llama_b9733.dll` + `ggml*.dll` (b9733) — พร้อมใช้งาน
- `cosplay_train.exe`, `cosplay_profile_train.exe`
- `ses_cmp.exe`, `ses_cluster.exe`, `ses_featurize.exe`, `ses_merge.exe`
- `test_kv_remap.exe` — KV Remap test suite (7 tests, all pass)
- 403 `.ses` profiles ใน `runner/ses_profiles/`
- 9 `.cpl` cosplay profiles ใน `runner/cpl_profiles/`

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

## Next Scope: POGLS File Management

ผู้ใช้จะไปดูว่า POGLS จัดการกับไฟล์ต่างๆ อย่างไรในระบบ — การอ่าน/เขียน/store/versioning ผ่าน pipeline POGLS

### ⚠️ Diamond Shell FLAT Fix (June 22)
Fixed critical bug in `diamond_shell_codec.h`: codec classified non-zero chunks as FLAT (reconstructed as all-zero) when `fibo_intersect == 0`, causing data loss. Fix: only use FLAT when chunk is truly all-zero.
- Fixed in: `collection/geopixel/hbv_bundle/Diamond_decode_hamburger/diamond_shell_codec.h` 
- Also fixed in: `collection/dgls/diamond/include/diamond_shell_codec.h` (mirror)

### Related Files (เบื้องต้น)
- `collection/geo_vault*.h/c` — GeoVault I/O
- `collection/geopixel/` — Geopixel encoding pipeline
- `collection/core/pogls_engine/` — POGLS engine core
- `collection/python_src/` — Python bridge scripts
- `runner/capture_pipeline.h` — Capture → store pipeline
- `collection/src/tensor_memory.h` — TensorMemStore

---

## Session History (สรุปย่อ)

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
