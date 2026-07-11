# pogls_kv — Adaptive KV Cache Remap

Skeleton+delta compression for transformer KV cache. Auto-selects compression tier based on change percentage.

## Three Tiers

| Tier | Threshold | Method |
|------|-----------|--------|
| ENTROPY | 0-15% | XOR + RLE |
| GEO | 15-85% | byte-offset ranges |
| REBUILD | 85%+ | flush + new skeleton |

ENTROPY XORs current against baseline then RLE-compresses the sparse diff. GEO records byte-offset ranges of non-zero XOR regions and packs changed bytes consecutively — efficient for moderate drift. REBUILD signals the caller to discard delta and snapshot a fresh skeleton.

## API Reference

### Skeleton Lifecycle

```c
PoglsKvSkeleton sk;
pogls_kv_skeleton_init(&sk, baseline, n_bytes);
// ... use sk as baseline for encode/decode ...
pogls_kv_skeleton_destroy(&sk);
```

`pogls_kv_skeleton_init` copies `n_bytes` from `baseline` and stores an RLE-compressed copy for baseline reconstruction. `pogls_kv_skeleton_destroy` frees both.

### classify

```c
int pct = pogls_kv_classify(cur, base, n_bytes);
```

Compares `cur` against `base` in 4096-byte chunks via `memcmp`. Returns a percentage `0..100` of differing bytes. Returns `0` when identical.

### encode / decode

```c
PoglsKvDelta delta;
int ret = pogls_kv_encode(&delta, cur, base, n_bytes);
```

Returns `0` on success. On REBUILD (≥85% change) returns `-1` with `delta.type = POGLS_KV_REMAP_REBUILD` — caller must flush and init a new skeleton.

```c
pogls_kv_decode(out, &delta, base, n_bytes);
```

Always writes `out = base` then XORs the decoded delta on top. On REBUILD returns `-1` without touching `out`.

## Rail Scanner

The rail scanner performs a 3-lane round-robin background scan to detect drift between skeleton baseline and live KV data.

### Initialization

```c
uint8_t layer_hint(size_t off) { return (uint8_t)(off / bytes_per_layer); }
PoglsKvRail rail;
pogls_kv_rail_init(&rail, &sk, total_bytes, layer_hint);
```

`total_bytes` is divided into 3 equal lanes. The `get_layer` callback (currently unused, reserved for future layer-aware scanning) maps byte offset to layer index.

### Step

```c
int ret = pogls_kv_rail_step(&rail);
// ret: 1 = scan complete, 0 = in progress, -1 = error/disabled
```

Each step compares up to 4096 bytes per lane. When all lanes finish, it computes `change_pct` from accumulated diff/checked counts and resets the scanner.

### Freeze / Resume

```c
pogls_kv_rail_freeze(&rail);
// ... perform decode (which may modify KV data) ...
pogls_kv_rail_resume(&rail);
```

Freeze saves per-lane offsets and sets state to 2 (frozen). Resume restores offsets and sets state back to 1 (scanning). Decode operations between freeze/resume do not corrupt scan state.

## Usage

```c
PoglsKvSkeleton sk;
pogls_kv_skeleton_init(&sk, baseline, n_bytes);

int tier = pogls_kv_classify(current, baseline, n_bytes);

PoglsKvDelta delta;
if (pogls_kv_encode(&delta, current, baseline, n_bytes) == 0) {
    // ENTROPY or GEO — store delta, later restore:
    pogls_kv_decode(restored, &delta, baseline, n_bytes);
} else {
    // REBUILD — flush KV, init new skeleton
    pogls_kv_skeleton_destroy(&sk);
    pogls_kv_skeleton_init(&sk, current, n_bytes);
}
```

## Constants

| Define | Value | Description |
|--------|-------|-------------|
| `POGLS_KV_REMAP_ENTROPY` | 0 | Tier: XOR + RLE delta |
| `POGLS_KV_REMAP_GEO` | 1 | Tier: byte-offset ranges |
| `POGLS_KV_REMAP_REBUILD` | 2 | Tier: flush + new skeleton |
| `POGLS_KV_THRESH_LOW` | 15 | Boundary: ENTROPY ↔ GEO (%) |
| `POGLS_KV_THRESH_HIGH` | 85 | Boundary: GEO ↔ REBUILD (%) |
| `POGLS_KV_MAX_GEO_RANGES` | 4096 | Max GEO range entries |
| `POGLS_KV_RLE_MAGIC` | 0x524C4531 | RLE header magic ("RLE1") |
