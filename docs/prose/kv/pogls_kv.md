# pogls_kv.h

**Module:** `pogls_kv`  
**Path:** `pogls_kv/pogls_kv.h`  
**Generated:** 2026-07-28 10:21  

## Structures

- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`

## API Functions

- `int  pogls_kv_skeleton_init(PoglsKvSkeleton *sk, const uint8_t *baseline, size_t n_bytes)`
- `void pogls_kv_skeleton_destroy(PoglsKvSkeleton *sk)`
- `int  pogls_kv_classify(const uint8_t *cur, const uint8_t *base, size_t n_bytes)`
- `int  pogls_kv_encode(PoglsKvDelta *delta, const uint8_t *cur,`
- `int  pogls_kv_decode(uint8_t *out, const PoglsKvDelta *delta,`
- `int  pogls_kv_rail_init(PoglsKvRail *rail, PoglsKvSkeleton *sk,`
- `int  pogls_kv_rail_step(PoglsKvRail *rail)`
- `void pogls_kv_rail_free(PoglsKvRail *rail)`
- `void pogls_kv_rail_freeze(PoglsKvRail *rail)`
- `void pogls_kv_rail_resume(PoglsKvRail *rail)`

## Constants

- `#define POGLS_KV_H`
- `#define POGLS_KV_REMAP_ENTROPY  0`
- `#define POGLS_KV_REMAP_GEO      1`
- `#define POGLS_KV_REMAP_REBUILD  2`
- `#define POGLS_KV_THRESH_LOW     15`
- `#define POGLS_KV_THRESH_HIGH    85`
- `#define POGLS_KV_MAX_GEO_RANGES 4096`
- `#define POGLS_KV_RLE_MAGIC      0x524C4531`

