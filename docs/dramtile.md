# DRamTile — Zero-Copy Geometry-Addressed Tensor Store

## Overview

DRamTile is a header-only C11 tensor storage library that maps model weights
into a large mmap'd region addressed by a geometric coordinate scheme.  It
provides O(1) deterministic lookup from tensor name → data pointer with no
hash collisions under normal conditions, and zero-copy pointer exchange at
swap time.

**Core principle**: "RAM is disk, disk is RAM." The backing store is a
file-backed mmap (`MAP_SHARED` on Linux, `CreateFileMapping` on Windows)
that persists across process restarts.  Modifying a pointer casts the same
page — no serialization, no WAL, no explicit save/load.

## Architecture

### Address Scheme

Tensor names are mapped to geometric coordinates via FNV-1a hashing:

```
name → FNV-1a hash → anchor / x / y / layer → dram_addr(anchor, x, y, layer)
```

The resulting `dram_addr` is a 16-bit value (0–20735 = `DRAM_FULL`)
encoding a 4D coordinate in the Y-triangle topology (see `geo_dram_tile.h`).
The address is deterministic — the same name always yields the same address.

### Hash Table

```
slot = dram_addr % DT_HASH_SLOTS  (512 slots)
```

Each slot stores a `DRamTileHashEntry`:

| Field       | Type        | Description                          |
|-------------|-------------|--------------------------------------|
| `dram_addr` | `uint32_t`  | Geometric address (0 = unused slot)  |
| `offset`    | `size_t`    | Byte offset in the mmap region       |
| `size`      | `size_t`    | Stored byte count                    |

Bit 31 of `dram_addr` (`DT_KV_FLAG = 0x80000000`) distinguishes weight
entries (clear) from KV cache entries (set).
Bit 30 (`DT_BOND_FLAG = 0x40000000`) marks entries spilled to the cold
region (both weights and KV).  The two flags can be combined (`KV+BOND`)
when a KV entry overflows past `kv_base` into cold storage.

### Dual-Region Layout

```
┌─────────────────────────────────────┐
│  Weight Region (file-backed)        │  ← store->base, persisted on disk
│  dt_put() / dt_putv()               │
├─────────────────────────────────────┤
│  KV Region (anonymous)              │  ← store->kv_base, ephemeral
│  dt_put_kv()                        │    lost on process exit
└─────────────────────────────────────┘
```

- **Weight region**: `CreateFileMapping` + `MapViewOfFile` / `mmap MAP_SHARED`
- **KV region**: `VirtualAlloc` / `mmap MAP_PRIVATE | MAP_ANONYMOUS`
- Selection at lookup: `dt_entry_ptr()` checks `DT_KV_FLAG` in `dram_addr`

### File Format (Twin)

```
┌───────────────────────────────────────┐
│  Tensor Data (64-byte aligned)        │
│  Sequential writes at store->used      │
├───────────────────────────────────────┤
│  Directory (TDI2 format)              │
│  Saved at end of file on destroy       │
├───────────────────────────────────────┤
│  dir_offset (uint32 LE, last 4 bytes) │
└───────────────────────────────────────┘
```

**TDI2 Directory Entry** (48 bytes + optional name padding):

| Offset | Size  | Field        |
|--------|-------|--------------|
| 0      | 4     | `dram_addr`  |
| 4      | 8     | offset       |
| 12     | 8     | size         |
| 20     | 4     | dtype        |
| 24     | 4     | ndim         |
| 28     | 4×4   | shape[4]     |
| 44     | 4     | namelen      |
| 48+    | var   | name + pad   |

Two formats exist for backward compatibility:
- **TDIR** (legacy): 20 bytes/entry, no metadata
- **TDI2** (current): 48 bytes/entry + optional name, self-describing

## API Reference

### Initialization

```c
int dt_store_init(DRamTileStore *store, size_t min_bytes);
```
Anonymous mode — `VirtualAlloc` / `mmap MAP_ANONYMOUS`.  Defaults to 4 GB.
Falls back to `malloc` if mmap fails.

```c
int dt_store_init_twin(DRamTileStore *store, const char *filepath, size_t max_bytes);
```
File-backed twin mode.  Opens or creates `filepath`.  If the file already
contains a valid TDI2/TDIR directory, the hash table is rebuilt from it
(reopen path).  Default capacity: 4 GB.

```c
int dt_store_init_twin_dual(DRamTileStore *store, const char *filepath,
                            size_t max_bytes, float weight_ratio);
```
Dual-region twin: splits `max_bytes` into weight (file-backed) and KV
(anonymous) regions by `weight_ratio` (0.0–1.0).  KV entries survive only
within the session.

### Store / Retrieve

```c
uint8_t *dt_put(DRamTileStore *store, const char *name,
                const uint8_t *data, size_t sz);
```
Store a weight tensor.  Returns zero-copy pointer (into mmap), NULL on
out-of-space.  Overwrite by same name is allowed if size matches.

```c
uint8_t *dt_put_kv(DRamTileStore *store, const char *name,
                   const uint8_t *data, size_t sz);
```
Store a KV cache tensor in the anonymous ephemeral region.  Returns NULL
if `kv_base` is not initialized.  If `kv_base` is full and a cold region
exists, spills transparently to cold (KV+BOND) — the same `dt_get()` /
`dt_get_size()` APIs continue to work without caller changes.

```c
uint8_t *dt_putv(DRamTileStore *store, const char *name,
                 uint32_t dtype, int ndim, const uint32_t *shape,
                 const uint8_t *data, size_t sz);
```
Store with self-describing metadata.  Metadata is saved in the TDI2
directory on `dt_store_destroy_twinv()`.

```c
uint8_t *dt_get(DRamTileStore *store, const char *name);
```
O(1) lookup by name.  Returns direct pointer (works for both weight and KV
entries).  NULL if not found.

```c
size_t dt_get_size(DRamTileStore *store, const char *name);
```
Bytes of a stored tensor, or 0 if not found.

```c
DtTensorView dt_getv(DRamTileStore *store, const char *name);
```
Returns full view (data, offset, nbytes, dram_addr, name).  If the store
was saved with TDI2, also returns dtype/ndim/shape.

```c
DtTensorView dt_view(DRamTileStore *store, const char *name,
                     uint32_t dtype, int ndim, const uint32_t *shape);
```
Explicitly typed view — caller provides dtype/shape from GGUF metadata.

```c
DtTensorView dt_resolve(DRamTileStore *store, uint32_t dram_addr);
```
Lookup by geometric coordinate instead of name.  Returns data at that
address regardless of whether it was stored as weight or KV.

### Persistence

```c
int dt_store_sync(DRamTileStore *store, int async);
```
Explicit `FlushViewOfFile` / `msync`.  Normally OS handles this via
`MAP_SHARED`; call at checkpoint boundaries for extra safety.
`async=0` = synchronous (block until flushed).

```c
void dt_store_destroy_twin(DRamTileStore *store);
```
Save directory (basic TDI2), sync, unmap file, release KV region.

```c
void dt_store_destroy_twinv(DRamTileStore *store);
```
Collect views from in-memory hash, save TDI2 with full metadata (dtype,
ndim, shape, name), sync, unmap, release KV region.

```c
void dt_store_destroy(DRamTileStore *store);
```
Anonymous mode cleanup.  If twin, unmaps without saving (caller decides).

### Directory Operations

```c
int dt_store_save_dir(DRamTileStore *store);
int dt_store_save_dir_v2(DRamTileStore *store, DtTensorView *views, uint32_t n);
```
Write directory to the end of the backing file.  KV entries are skipped.

```c
int dt_store_load_dir(DRamTileStore *store);
```
Read directory from the end of file, rebuild hash table.  Detects TDI2
vs TDIR automatically.

```c
int dt_store_load_views(DRamTileStore *store, DtTensorView **out_views);
```
Load TDI2 directory and return full metadata views (allocated, caller
must `free`).  Also rebuilds hash table.  Returns count of views, or -1.

### Utilities

```c
int dt_store_foreach(DRamTileStore *store, DtTensorCallback callback, void *user);
```
Iterate all stored tensors.  KV entries are skipped.  Returns count.
Callback returning non-zero stops iteration.

```c
size_t dt_store_total_bytes(DRamTileStore *store);
```
Sum of bytes for all weight entries (excludes KV).

```c
int dt_store_check_twin(const char *filepath);
```
Quick sanity check — returns 0 if file exists and ≥ 64 bytes.

```c
int dt_is_kv(DRamTileStore *store, uint32_t slot);
```
Check if a hash slot contains a KV entry.

```c
uint32_t dt_name_to_addr(const char *name);
```
FNV-1a hash → geometric address.

### Cold Spill

```c
int dt_store_init_cold(DRamTileStore *store, size_t max_bytes);
int dt_store_init_cold_twin(DRamTileStore *store, const char *filepath, size_t max_bytes);
```
Initialize anonymous or file-backed cold region.  Default minimum: 256 MB
(`DT_COLD_DEFAULT`).  On reopen, `dt_cold_rebuild_used()` scans bond entries
to recompute `cold_used`.

```c
uint8_t *dt_cold_alloc(DRamTileStore *store, size_t sz);
```
Allocate from cold region (64-byte aligned).  Returns NULL if full.

```c
void dt_cold_rebuild_used(DRamTileStore *store);
```
Rebuild `cold_used` from hash bond entries — called after reopen or eviction.

### Migrate + Eviction

```c
int dt_migrate_promote_one(DRamTileStore *store, int slot, size_t dir_reserve);
int dt_migrate_step(DRamTileStore *store, int max_entries, size_t dir_reserve, int promote_newest);
```
Promote bond entries from cold back to primary.  `promote_newest=1` promotes
newest-first (higher `session_tick`).  Returns count promoted.

```c
int dt_evict_step(DRamTileStore *store, int max_entries);
```
LRU evict oldest bond entries from cold (lowest `session_tick`).  Returns
count evicted.

```c
int dt_cold_make_room(DRamTileStore *store, size_t sz, int max_evict);
```
Evict until `sz` bytes fit in cold.  Returns evicted count, or -1.

### KV Compose

```c
uint8_t *kv_compose(DRamTileStore *store, uint32_t slot);
```
Resolve composed pointer for KV+BOND entry.  Returns cold or kv_base
pointer depending on flag state.

### Type-Safe Container (`dramtile_container.h`)

```c
DtContainer dtc_wrap(DtTensorView *view, uint32_t dtype);
```
Wrap a view into a stride-computed container for typed element access.

```c
float  dtc_f32_1d(DtContainer *c, size_t i0);
float  dtc_f32_2d(DtContainer *c, size_t i0, size_t i1);
float  dtc_f32_3d(DtContainer *c, size_t i0, size_t i1, size_t i2);
uint16_t dtc_f16_1d(DtContainer *c, size_t i0);
uint16_t dtc_f16_2d(DtContainer *c, size_t i0, size_t i1);
int32_t  dtc_i32_1d(DtContainer *c, size_t i0);
int8_t   dtc_i8_1d(DtContainer *c, size_t i0);
```
Typed element accessors (1D–3D).  Unchecked — caller must verify bounds.

```c
DtContainer dtc_slice(DtContainer *c, int dim, size_t start, size_t end);
```
Sub-tensor slice.  `dim` = dimension to slice, `start` inclusive,
`end` exclusive.  Returns new container sharing the same backing mmap.

```c
DtContainer dtc_flatten(DtContainer *c);
```
Flatten to 1D view (all elements contiguous).  Requires contiguous layout.

```c
void *dtc_ptr(DtContainer *c, const size_t *indices);
```
Element pointer for bulk access.

```c
void dtc_print(DtContainer *c, const char *label);
```
Debug dump: dtype, ndim, shape, strides, pointer.

## Key Design Properties

### Deterministic Placement

`dt_name_to_addr()` is a pure function of the tensor name.  The same name
always maps to the same geometric address → same hash slot → same mmap
offset.  This means:

- No write-ahead log needed — replay produces identical placements.
- Hash collisions are deterministic and predictable.
- Slot reuse across model reloads is automatic.

### Zero-Copy Swap

On SID swap (weight exchange), only the `tensor->data` pointer changes:

```c
tensor->data = dt_get(&g_dramtile, name);
```

No `memcpy` of weight data.  The OS lazily page-faults the backing file
pages on first access.  KV cache insertion is equally pointer-based.

### Cold Spill Region

When the primary (weight) region is full, `dt_put()` transparently spills
to the cold region:

```
Primary full?  ──yes──→  alloc in cold  ──→  set DT_BOND_FLAG
    │
    no
    ↓
  store in primary (local)
```

The cold region is initialized via:

- `dt_store_init_cold()` — anonymous (`VirtualAlloc` / `mmap`), default 256 MB
- `dt_store_init_cold_twin()` — file-backed (persistent across restarts)

On spill, the hash entry stores `cold_offset` (position in cold region) and
`session_tick` (monotonic timestamp).  Lookup via `dt_get()` routes
transparently:

```c
if (entry & DT_BOND_FLAG)
    return store->cold_base + store->hash[slot].cold_offset;
```

### KV Compose (`kv_compose`)

KV cache entries that cannot fit in `kv_base` spill to cold as **KV+BOND**
entries (both `DT_KV_FLAG` and `DT_BOND_FLAG` set).  `kv_compose()` resolves
the composed pointer:

```c
static inline uint8_t *kv_compose(DRamTileStore *store, uint32_t slot) {
    if (store->hash[slot].dram_addr & DT_BOND_FLAG)
        return store->cold_base + store->hash[slot].cold_offset;
    return store->kv_base + store->hash[slot].offset;
}
```

**Current strategy**: cold stores the latest full copy → return cold pointer
directly.  Future: delta compose floor0 (kv_base) + floor1 (cold).

`dt_get()`, `dt_get_size()`, and `dt_put_kv()` (update path) all handle
KV+BOND transparently — no caller changes needed.

### Migrate Path (`dt_migrate_step`)

Periodically promotes bond entries from cold back to primary when space
frees up, preventing cold from accumulating:

```c
int dt_migrate_step(DRamTileStore *store, int max_entries,
                    size_t dir_reserve, int promote_newest);
```

- Scans hash for `DT_BOND_FLAG` entries (skips KV+BOND)
- Sorts by `session_tick` (oldest-first or newest-first per `promote_newest`)
- Copies data from `cold_base` → `base`, clears `DT_BOND_FLAG`
- `dir_reserve`: bytes to leave at end of primary for directory (avoids
  save_dir failure)
- Returns number promoted, stops when primary is full

Helper:
```c
int dt_migrate_promote_one(DRamTileStore *store, int slot, size_t dir_reserve);
```

### Eviction Policy (`dt_evict_step`)

LRU eviction from cold when migration can't keep up:

```c
int dt_evict_step(DRamTileStore *store, int max_entries);
```

- Finds bond entries with lowest `session_tick` → removes oldest first
- Clears hash slot, decrements `n_stored`
- Calls `dt_cold_rebuild_used()` to recompute `cold_used` from remaining
  bond entries
- Returns number evicted

To make room before allocation:
```c
int dt_cold_make_room(DRamTileStore *store, size_t sz, int max_evict);
```
Evicts oldest entries until `sz` bytes fit in cold region.  Returns evicted
count, or -1 if impossible.

### Session Tick

`store->session_tick` is a monotonic counter incremented on every
`dt_put()` / `dt_put_kv()` (cold spill path) and stored in the hash entry
at spill time:

| Field          | Set when               | Used by                |
|----------------|------------------------|------------------------|
| `session_tick` | On cold spill          | Eviction (oldest tick) |
|                |                        | Migration (sort order) |

Lower tick = older = less recently accessed = first to evict.  Tick is
session-local and not persisted across restarts.

### Three-Tier Storage Hierarchy

```
┌─────────────────────┐
│  Primary (weight)   │  ← file-backed, persisted, zero-copy
│  fast, large        │
├─────────────────────┤
│  KV (anonymous)     │  ← session-only, ephemeral
│  fast, moderate     │
├─────────────────────┤
│  Cold (anonymous     │  ← overflow spill, LRU evicted
│  or file-backed)    │
│  slower, large      │
└─────────────────────┘
```

### KV Ephemeral Rule

KV entries carry `DT_KV_FLAG` in their `dram_addr`:

- `dt_entry_ptr()` selects `kv_base` vs `base` based on the flag.
- `dt_store_save_dir()` skips KV entries → never persisted.
- `dt_store_foreach()` / `dt_store_total_bytes()` exclude KV entries.
- On reopen, only weight entries are restored.

### Hash Slot Collision

If two tensor names hash to the same slot, the second `dt_put()` replaces
the first.  In practice this does not happen because:

- 512 slots for ~300 tensors (8B model) → load factor ~0.6.
- Tensor names are distinct and well-distributed by the GGUF naming
  convention (`blk.{layer}.{type}.weight`).

If a collision occurs, the library prefers the last writer (no probing).

## Platform Portability

| Feature            | Windows                            | Linux/macOS                        |
|--------------------|------------------------------------|------------------------------------|
| Anonymous alloc    | `VirtualAlloc(MEM_COMMIT)`         | `mmap(MAP_ANONYMOUS)`              |
| File-backed        | `CreateFileMapping` + `MapViewOfFile` | `mmap(MAP_SHARED)`             |
| KV region          | `VirtualAlloc`                     | `mmap(MAP_PRIVATE | MAP_ANONYMOUS)`|
| Flush              | `FlushViewOfFile`                  | `msync()`                          |
| Heap fallback      | `malloc`                           | `malloc`                           |

The library is pure C11 with no architecture-specific code.  Compiles with
GCC, Clang, and MSVC (`/std:c11`).

## Test Coverage

All tests in `test_dramtile_twin.c` (97 tests, all passing):

| Phase | Description | Tests |
|-------|-------------|-------|
| 1     | Fresh init, put/get, view, resolve, foreach, total_bytes | 17 |
| 2     | Persistence across reopen | 5 |
| 3     | Twin principle — modify, sync, reopen, verify | 3 |
| 4     | Self-describing TDI2 format, destroy_twinv | 7 |
| 5     | dt_store_load_views — full metadata reconstruction | 3 |
| 6     | Dual-region — KV ephemeral, not persisted | 11 |
| 7     | Type-safe container — wrap, element access, slice, flatten | 7 |
| 8     | Cold overflow spill (anonymous bond) | 7 |
| 9     | Persistent cold twin (file-backed bond) | 12 |
| 10    | Migrate bond → primary promote | 6 |
| 11    | Evict oldest bond entries from cold | 6 |
| 12    | Auto-evict on cold full (dt_cold_make_room) | 6 |
| 13    | KV compose (KV spill to cold) | 8 |

## Usage Example

```c
DRamTileStore store;
dt_store_init_twin_dual(&store, "weights.bin", 8UL << 30, 0.9f);
// 7.2 GB weight (file-backed) + 800 MB KV (anonymous)

// Store weights from SID cache
for (int i = 0; i < n_tensors; i++)
    dt_put(&store, names[i>, data[i], sizes[i]);

// Swap: zero-copy pointer exchange
model->layers[l].attn.weight.data = dt_get(&store, name);

// Store KV cache entries (ephemeral)
dt_put_kv(&store, "kv.layer0.k", k_data, k_size);

// Retrieve via geometric coordinate
DtTensorView v = dt_resolve(&store, dram_addr);

// Type-safe access
DtContainer c = dtc_wrap(&v, DT_F32);
float w = dtc_f32_2d(&c, row, col);

// On exit: save and cleanup
dt_store_destroy_twinv(&store);
```

## Files

| File | Description |
|------|-------------|
| `runner/dramtile_store.h` | Core store — init, put/get, twin, dual-region, save/load |
| `runner/dramtile_container.h` | Type-safe container — wrap, element access, slice, flatten |
| `runner/test_dramtile_twin.c` | 97 tests covering all 13 phases |
| `collection/geo_dram_tile.h` | Hilbert-curve addressing scheme |
| `docs/dramtile.md` | This document |
