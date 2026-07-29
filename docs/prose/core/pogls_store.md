# pogls_store.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_core`  
**Path:** `pogls_core/pogls_store.h`  
**Status:** `active`  
**Note:** modified 17d ago; included by 5 file(s)  
**Generated:** 2026-07-29 21:54  

## Structures

- `typedef struct PoglsStore PoglsStore`

## API Functions

- `PoglsStore* pogls_store_open(const char *path, uint64_t capacity)`
- `void        pogls_store_close(PoglsStore *store)`
- `int         pogls_store_sync(PoglsStore *store, int async)`
- `int  pogls_store_put(PoglsStore *store, const char *name,`
- `void* pogls_store_get(PoglsStore *store, const char *name, size_t *sz_out)`
- `int  pogls_store_free(PoglsStore *store, const char *name)`
- `typedef int (*PoglsStoreVisitFn)(const char *name, size_t sz,`
- `int  pogls_store_foreach(PoglsStore *store, PoglsStoreVisitFn fn,`
- `uint32_t pogls_store_count(PoglsStore *store)`
- `uint64_t pogls_store_bytes(PoglsStore *store)`
- `int  pogls_store_has(PoglsStore *store, const char *name)`
- `size_t pogls_store_size(PoglsStore *store, const char *name)`

## Constants

- `#define POGLS_STORE_H`

