# v4 System Overview — File Type Behavior

## Architecture

```
ไฟล์ → [64B chunking] → [Entropy classifier] ─┬── low → [shell encode (×16 optional)] ──► tring
                                                │                       ↓
                                                ├── med → [shell encode (skip Hilbert)]
                                                │                       ↓
                                                └── high → [raw store]
                                                                       ↓
                                                              [adaptive per chunk]
                                                  sparse | bitpack | LZ | LZ+Hilbert | raw
```

## File Type → Behavior

| File type | Entropy class | Mode | Encode rate | Ratio | Speed |
|---|---|---|---|---|---|
| **Geometric** (DiamondBlock, structured) | **low (0)** | shell ×16 | 100% | 2.5–3× | ~7 MB/s |
| **Source code** (C, Python) | **med (1)** | shell ×16 | ~100% | 1.3–2× | ~7 MB/s |
| **Image (photo)** (BMP, PNG pixels) | **mixed** (90% low + 10% high) | shell ×16 → **77%** / flat → **100%** | 77% (shell) / 100% (flat) | 1.3× (×16) / 1.26× (flat) | ~7 MB/s (×16) / ~12 MB/s (flat) |
| **Random / encrypted** | **high (2)** | raw → tring | 100% | 1.0× (no compression) | ~20 MB/s |
| **Large file** (>12 MB, 200K+ chunks) | mixed | **flat mode** (default) | 100% | 1.26–1.5× | ~12 MB/s |

## Decision Flow

```
ไฟล์เข้า →
  │
  ├─ ≤12MB ──→ shell (×16 optional)
  │              ├─ geometric → shell ปกติ (ratio 2.9×)
  │              └─ mixed     → ×16 on (77% capacity)
  │
  └─ >12MB ──→ flat mode (shell ไม่พอ)
                 ├─ compressible → adaptive trials
                 └─ high entropy → raw store
```

## Per-chunk adaptive encoder

```
chunk → [sparse try] → [bitpack try] → [LZ try] → [LZ+Hilbert×4 try]
         ↓                ↓               ↓           ↓
         └──────────────────────────────────────────────┴→ pick smallest
```

With entropy early-exit:
- entropy=2 → raw only (skip all trials)
- entropy=1 → skip Hilbert (save 4× LZ)
- entropy=0 + LZ-failed → skip Hilbert

## Performance (G4400 worst-case)

| Mode | Throughput | Latency (1MB) |
|---|---|---|
| Shell normal | 2.4 MB/s | ~430 ms |
| Shell ×16 | 6.8 MB/s | ~150 ms |
| Flat | 12 MB/s | ~85 ms |
| Raw (high entropy) | ~20 MB/s | ~50 ms |

## Colab reference

119 GB/s potential (from earlier benchmarks) — current bottleneck is single-threaded probe + hash.

## Key flags / API

```c
dfield_set_x16(&df, 1);                 // enable ×16 sub-slot precision
dfield_encode(&df, chunk, &level);      // shell encode (returns gidx)
dfield_encode_flat(&df, chunk);         // flat encode (returns tick)
dfield_encode_windowed(&df, chunks, 32, 16, gidxs);  // batch-grouped
dfield_encode_flow(&df, chunk, NULL);   // flow-aware routing
dfield_decode(&df, gidx, out);          // shell decode
dfield_decode_flat(&df, tick, out);     // flat decode
```
