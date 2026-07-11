# pogls_dram — Flat-Arena Memory Store

Geometry-addressed tensor storage via a single `pogls_alloc_large` (VirtualAlloc/mmap) block. Sequential access, no hashing, predictable latency.

## Layout

```
┌──────────────────────────────────────────────────────────┐
│ 64B  StoreHeader  (magic, version, capacity, used, ...)  │
├──────────────────────────────────────────────────────────┤
│ DirEntry[512]    512 entries × 84B = 43,008B             │
│   (name[64] + dram_addr + nbytes + offset + session_tick │
│    + flags + pad)                                        │
├──────────────────────────────────────────────────────────┤
│ data arena   (remaining space, up to `capacity` bytes)   │
└──────────────────────────────────────────────────────────┘
```

All three regions sit in one contiguous allocation. After `pogls_dram_open`, `store->entries` and `store->arena` point into `store->base`.

## Key Features

- **Address lookup** — `find_by_addr()` linear scans up to 512 entries by `dram_addr` (masking `POGLS_DRAM_KV_FLAG`). Predictable ~25 µs worst-case on modern x64.
- **Name lookup** — `find_by_name()` linear scan by 64-char name.
- **KV ephemeral flag** — bit 31 (`POGLS_DRAM_KV_FLAG`) marks entries as ephemeral. `pogls_dram_save(..., is_kv=0)` skips KV entries; `is_kv=1` persists only KV entries. Runtime lookups respect the flag bit so KV and persistent entries can share the same low address bits.
- **Save/load cycle** — `pogls_dram_save` writes header + filtered directory + arena bytes to a binary file. There is no built-in `load` — call `pogls_dram_open` then `pogls_dram_put` for each entry.
- **512 entry maximum** — fixed `POGLS_DRAM_MAX_DIR = 512` slots. `find_free_slot()` returns first zero-addr slot.
- **No compaction on free** — `pogls_dram_free` zeroes the directory entry but does not reclaim arena space (`store->used` unchanged). Arena is append-only.

## API Reference

| Function | Description |
|---|---|
| `pogls_dram_open(store, path, capacity)` | Allocate and initialise a dram store. `path` (optional) saved to `store->filepath` for diagnostic use. Returns 0 on success, -1 on failure. |
| `pogls_dram_close(store)` | Free the underlying allocation and zero the struct. Safe to call on a zero-initialised store (`base == NULL` is a no-op). |
| `pogls_dram_save(store, path, is_kv)` | Serialise directory + arena to a binary file. When `is_kv=0`, entries with `POGLS_DRAM_KV_FLAG` are skipped; when `is_kv=1`, only KV-flagged entries are written. Returns 0 on success. |
| `pogls_dram_put(store, name, addr, data, sz)` | Store data at `addr`. If `addr` already exists: **overwrites** in-place if `sz <= existing->nbytes`, otherwise fails (-1). If new: allocates from arena, fills next free directory slot. Name copied (max 63 chars + null). Returns 0 on success. |
| `pogls_dram_get(store, addr, sz_out)` | Lookup by address. Returns pointer to arena data (or NULL). Sets `*sz_out` to entry size. |
| `pogls_dram_get_name(store, name, sz_out)` | Lookup by name (linear scan). Returns pointer to arena data (or NULL). Sets `*sz_out` to entry size. |
| `pogls_dram_free(store, addr)` | Zero the directory entry (addr=0, name=""). Does **not** reclaim arena space. Returns 0 on success, -1 if not found. |
| `pogls_dram_has(store, addr)` | Returns 1 if entry exists (non-zero `dram_addr`), 0 if not. |
| `pogls_dram_count(store)` | Returns `store->n_entries` (active entry count). |
| `pogls_dram_bytes(store)` | Returns `store->used` (arena bytes consumed, monotonically increasing until save/close). |

## Design Decisions

- **Flat arena vs hash map** — linear scan of 512 entries is simple, deterministic, and fast enough (~25 µs worst-case on a modern x64 CPU). No hashing overhead, no collision handling, no resize logic. The 20736-address space (`144²`, matching the Y-triangle geometry grid) is dense enough that 512 entries never cause cache-thrashing scans in practice.
- **KV flag bit** — bit 31 in `dram_addr` is reserved for ephemeral marking instead of a separate directory. This lets KV cache data share the same storage machinery while being explicitly excluded from serialisation without a separate store instance.
- **No arena compaction** — `free` is O(1) directory only. Arena is append-only to keep offsets stable across a session. The store is designed for session-local scratch use; compaction happens at save time (serialised entries are packed sequentially in the file, so reload via `open` + `put` naturally compacts).
- **Internal vs external DirEntry** — The public `PoglsDramEntry` (in `pogls_dram.h`) exposes `dram_addr`, `nbytes`, `name` for inspection. The internal `DirEntry` (in `pogls_dram.c`) adds `offset` into the arena and a `DirEntry*` cast is used internally; this layout is private and not serialised verbatim — save writes a packed format without the offset field.
- **PoglsDramStore fields** — `base`, `capacity`, `used`, `max_entries`, `n_entries`, `session_tick`, `entries`, `arena`, `filepath` are documented in the struct but marked internal. API consumers should treat `PoglsDramStore` as opaque and use the accessor functions.

## Usage

```c
PoglsDramStore store;
pogls_dram_open(&store, NULL, 64 * 1024 * 1024);

/* Store a tensor at geometry address 42 */
pogls_dram_put(&store, "test", 42, "hello", 6);

/* Retrieve by address */
size_t sz;
void *data = pogls_dram_get(&store, 42, &sz);

/* Retrieve by name */
void *also = pogls_dram_get_name(&store, "test", &sz);

/* Check existence */
int exists = pogls_dram_has(&store, 42);

/* Remove entry (directory only, no arena compaction) */
pogls_dram_free(&store, 42);

/* Persist to disk */
pogls_dram_save(&store, "snapshot.bin", 0);

pogls_dram_close(&store);
```
