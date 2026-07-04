# RDH Wedge-Ring Addressing — Integration Report

## Overview

Replaced FNV-1a hash-based tensor address resolution with RDH (Wedge-Ring) generative integer grid: a 5-parameter `(ring, wedge, mirror, u, v)` formula producing deterministic, collision-free addresses.

**Source:** `collection/rdh/rdh_addr.h` — standalone module, depends on `stdint.h` only.

## Architecture

```
collection/rdh/
  rdh_addr.h                   Core: 3 functions (key, decompose, capacity)
  kv_page_rdh.h                KV page store consumer (prototype)
  wedge_ring_address.h         Original 24-gon x 6-ring reference
  test_rdh_addr.c              6/6 PASS
  test_kv_page_rdh.c           3/3 PASS

runner/
  addr_space.h                 addr_from_rdn_name() + addr_rdh_capo()
  dramtile_store.h             dt_name_to_rdh()
  gguf_to_pogls.c              --rdh flag
  llama_pogls_runner_sid_v2.c  --rdh flag + tensor_addr() helper
```

## Formula

Mixed-radix address formula:

```
key = ((ring * n_wedges + wedge) * n_mirror + mirror) * max_u + u
    = ring * 256 + wedge           (for Tier0: n_wedges=256, mirror=1, max_u=1)
    = (ring << 8) | wedge          (pure bitfield, 2^n radix)
```

Address space: 128 x 256 = 32,768 slots, 256 used for LFM2 8B, 20736 anchored by 162 geometry.

## Performance

### Address computation (ns/call)

| Scenario | Hash (FNV-1a) | RDH Formula | Speedup |
|---|---|---|---|
| 1.2B (148 names) | 41.1 ns | 19.1 ns | 2.15x |
| 8B (256 names) | 39.6 ns | 25.7 ns | 1.54x |
| Stress (1M random) | 31.2 ns | 16.4 ns | 1.90x |

### Conversion time (includes I/O)

| Model | Hash | RDH |
|---|---|---|
| 1.2B (697 MB) | ~4.9s | ~4.8s |
| Qwen 4B (2.5 GB) | ~19s | ~18s |

99% of conversion time is disk I/O, not address computation. RDH does not measurably improve end-to-end speed.

## File Size

POGLS file size is **identical** with or without `--rdh`:

| Model | Hash POGLS | RDH POGLS | Same? |
|---|---|---|---|
| LFM2.5-1.2B (148 tensors) | 731,242,514 | 731,242,514 | Yes |
| Qwen3-4B (398 tensors) | 2,497,653,590 | 2,497,653,590 | Yes |
| LFM2.5-8B (256 tensors) | 5,155,923,339 | 5,155,923,339 | Yes |

Address change is 4 bytes per meta entry. Everything else (tensor data, model meta, header) is identical.

## Collision Analysis

| Model | Hash | RDH |
|---|---|---|
| 1.2B (148 names) | 0 | 0 |
| Qwen 4B (398 names) | 0 | 0 |
| 8B (256 names) | 0 | 0 |

Hash already produces zero collisions at current scale (0.8% load factor). RDH guarantees zero regardless of scale.

## Bugs Fixed

- **32-bit fseek truncation** (`gguf_to_pogls.c:182`): `fseek(f, (long)abs_off)` truncated tensor offsets > 2 GB. Models > 2 GB wrote corrupt output. Fixed with `fseeko(f, (off_t)abs_off)`.

## User-Facing Changes

- **--rdh flag**: enables RDH addressing for all tensor address resolution
- **gguf_to_pogls --rdh**: produces RDH-addressed POGLS files
- **runner --rdh**: uses RDH formula (hash bypassed entirely)
- **No regressions**: 418/418 tests pass

## When RDH Matters

| Scenario | Hash | RDH |
|---|---|---|
| Tier0 (today, 256 tensors) | Works fine, 0 coll | Guarantee + 2x compute speed |
| Tier1 (430M addresses) | Collision + rebuild cost | Zero-cost blueprint |
| SID face duality | addr_capo() modular rot | mirror flag = bit flip |
| Zero-copy indirect | Cache miss on hash table | Formula, no memory touch |
| Ring-aware scheduler | Random scatter | Same-ring cluster |
