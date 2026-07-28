# pogls_dram.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_dram`  
**Path:** `pogls_dram/pogls_dram.c`  
**Status:** `active`  
**Note:** modified 16d ago  
**Generated:** 2026-07-28 10:24  

## Structures

- `typedef struct`
- `typedef struct`

## API Functions

- `static size_t dir_bytes(uint32_t max_entries)`
- `static size_t store_total_size(uint32_t max_entries, size_t capacity)`
- `static DirEntry* find_by_addr(PoglsDramStore *store, uint32_t addr)`
- `static DirEntry* find_by_name(PoglsDramStore *store, const char *name)`
- `static DirEntry* find_free_slot(PoglsDramStore *store)`
- `int pogls_dram_open(PoglsDramStore *store, const char *path, size_t capacity)`
- `void pogls_dram_close(PoglsDramStore *store)`
- `int pogls_dram_save(PoglsDramStore *store, const char *path, int is_kv)`
- `int pogls_dram_put(PoglsDramStore *store, const char *name, uint32_t addr,`
- `void* pogls_dram_get(PoglsDramStore *store, uint32_t addr, size_t *sz_out)`
- `void* pogls_dram_get_name(PoglsDramStore *store, const char *name, size_t *sz_out)`
- `int pogls_dram_free(PoglsDramStore *store, uint32_t addr)`
- `int pogls_dram_has(PoglsDramStore *store, uint32_t addr)`
- `return find_by_addr(store, addr) != NULL`
- `uint32_t pogls_dram_count(PoglsDramStore *store)`
- `uint64_t pogls_dram_bytes(PoglsDramStore *store)`

## Constants

- `#define POGLS_DRAM_MAGIC     0x50445241u`
- `#define POGLS_DRAM_VERSION   1u`
- `#define POGLS_DRAM_HDR_SZ    64u`
- `#define POGLS_DRAM_PATH_MAX  260u`
- `#define DIR_ENTRIES(store)  ((DirEntry*)(store)->entries)`
- `#define POGLS_DRAM_HDR_ENTRIES(base, max_entries) \`
- `#define POGLS_DRAM_HDR_ARENA(base, max_entries) \`

