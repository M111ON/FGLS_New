# pogls_kv.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_kv`  
**Path:** `pogls_kv/pogls_kv.c`  
**Status:** `active`  
**Note:** modified 16d ago  
**Generated:** 2026-07-28 10:45  

## Structures

- `typedef struct`
- `typedef struct`

## API Functions

- `static int rle_compress(const uint8_t *src, size_t size,`
- `static uint8_t *rle_decompress(const void *compressed, size_t comp_size,`
- `static uint32_t build_geo_ranges(const uint8_t *diff, size_t diff_size,`
- `int pogls_kv_skeleton_init(PoglsKvSkeleton *sk, const uint8_t *baseline, size_t n_bytes)`
- `void pogls_kv_skeleton_destroy(PoglsKvSkeleton *sk)`
- `int pogls_kv_classify(const uint8_t *cur, const uint8_t *base, size_t n_bytes)`
- `int pogls_kv_encode(PoglsKvDelta *delta, const uint8_t *cur,`
- `int pogls_kv_decode(uint8_t *out, const PoglsKvDelta *delta,`
- `int pogls_kv_rail_init(PoglsKvRail *rail, PoglsKvSkeleton *sk,`
- `int pogls_kv_rail_step(PoglsKvRail *rail)`
- `void pogls_kv_rail_freeze(PoglsKvRail *rail)`
- `void pogls_kv_rail_resume(PoglsKvRail *rail)`
- `void pogls_kv_rail_free(PoglsKvRail *rail)`

