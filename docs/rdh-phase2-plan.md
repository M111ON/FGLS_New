# RDH Phase 2 — Ring-Aware System Upgrade

## Goal
Unlock geometric locality benefits of RDH addressing across DRamTile, GearShift, GearLock, SID, and KV page store.

## Tasks

### 1. SID Mirror Flip (10 min)
- Replace `addr_capo()` modular add with `addr ^ RDH_MIRROR_BIT`
- Miror flag = SID face duality by construction
- File: `addr_space.h`, `llama_pogls_runner_sid_v2.c`

### 2. GearShift Ring Batching (~1 hour)
- Sort pending entries by ring before streaming
- Same-ring tensors move together (locality)
- File: `gear_shift.h`

### 3. DRamTile Ring Eviction (~0.5 day)
- Change eviction sort from `session_tick` (LRU) to:
  - Primary: ring distance from active (farthest first)
  - Secondary: LRU within same ring
- Promition during migrate: count ring usage per session
- File: `dramtile_store.h`

### 4. GearLock Ring Priority (~1 hour)
- Add ring-based base score:
  - Ring 0-2: +3 (core layers)
  - Ring 3-5: +2 (mid layers)
  - Ring 6+: +1 (deep layers)
  - Configurable via `gear_lock_ring_score[]`
- GearLock still overrides with per-tensor adjustment
- File: `gear_lock.h`

### 5. KV Page RDH Merge (~0.5 day)
- Replace flat `page_id` 0..255 with `(ring, wedge, mirror, u)` address
- Per-layer selective restore (don't zero full page)
- File: `kv_page_rdh.h` → wire into runner

## Summary

| Step | Effort | Code Change | Benefit |
|---|---|---|---|
| 1. SID mirror flip | 10 min | ~5 lines | Simpler design |
| 2. GearShift batching | 1 hr | ring sort in pending queue | Tensor locality during stream |
| 3. DRamTile eviction | 0.5 day | evict sort key change | Keep active layers in RAM |
| 4. GearLock priority | 1 hr | ring score table | Deep layers evict first |
| 5. KV page RDH | 0.5 day | flat page_id → RDH tuple | Per-layer selective restore |
| **Total** | **~2 days** | 5 files | Ring-aware data movement |
