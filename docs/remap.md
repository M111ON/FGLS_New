# KV Remap — Adaptive Skeleton + Delta KV Cache Management

## Overview

KV Remap monitors KV cache state between decode steps and classifies change intensity to choose the most efficient storage strategy. A background rail system performs idle-time verification with layer-based segment chunking.

## Quick Start

```bash
llama_pogls_runner_sid_v2.exe model.gguf --remap --ngl 1 -c 2048
```

## How It Works

### 3-Tier Adaptive Classification

After each generation response, the system classifies how much the KV cache has changed compared to the saved skeleton:

| Change % | Tier | Strategy | When |
|---|---|---|---|
| 0-15% | ENTROPY | XOR diff → RLE compressed delta | Minimal change (same topic) |
| 15-85% | GEO | Byte-offset ranges | Moderate change (new context) |
| 85%+ | REBUILD | Flush + new skeleton baseline | Major change (topic switch) |

### Layer-Based Segment Chunking (Rail)

The rail system scans KV cache in background during idle time. Instead of scanning flat byte ranges, it scans **per-layer**:

- 3 lanes, each assigned `RAIL_LAYERS_PER_LANE=2` layers
- Scan walks `layers[l].k_data` → `layers[l].v_data` directly
- Patch writes skeleton back per-layer (decompress → copy per K/V region)

### Lifecycle

```
┌─────────────────────────────────────────┐
│  Init: register KV tensors from model   │
│  (skeleton deferred — no memory spike)  │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│  /rscan: set skeleton baseline (first   │
│  use) then rail scan to completion      │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│  Decode loop:                           │
│    1. Rail FREEZE (save scan state)     │
│    2. sid_swap_apply()                  │
│    3. llama_decode()                    │
│    4. sid_swap_restore()                │
│    5. Rail RESUME (continue scan)       │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│  After generation response:             │
│    1. Rail idle step (if skeleton valid)│
│    2. kv_remap_cycle() (classify→store) │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│  Cleanup: destroy rail + remap          │
└─────────────────────────────────────────┘
```

## Command-Line Options

| Flag | Description |
|---|---|
| `--remap` | Enable KV Remap with adaptive skeleton+delta + rail background scan |

## Chat Commands

| Command | Description |
|---|---|
| `/rstatus` | Print remap skeleton/delta stats + rail scan state |
| `/rscan` | Trigger a full rail scan to completion (blocking) |

## Architecture

### Files

| File | Purpose |
|---|---|
| `runner/kv_remap.h` | Core adaptive skeleton+delta (RLE compressed, self-contained) |
| `runner/kv_remap_rail.h` | Rail layer-based segment chunking (per-layer scan/patch) |
| `runner/test_kv_remap.c` | Full test suite (7 tests) |

### Key Structures

```
KVRemapCtx          — Main context: skeleton, delta, layer info
  ├─ layers[]       — Per-layer K/V data pointers and sizes
  ├─ skeleton_data  — Compressed skeleton snapshot
  ├─ ref_skeleton   — Uncompressed reference for classify()
  └─ delta          — Current delta (ENTROPY or GEO)

KVRemapRail         — Background scan/patch system
  ├─ lanes[3]       — Each lane covers RAIL_LAYERS_PER_LANE layers
  │   ├─ layer_start/end  — Layer range for this lane
  │   ├─ cur_layer/phase  — Scan progress (0=K, 1=V)
  │   └─ diff_count       — Bytes changed in this lane
  └─ state          — PARK / SCAN / PATCH / REBUILD / FREEZE
```

### Data Flow

```
Live KV State ──→ classify() ──→ change_pct
                                    │
                    ┌───────────────┼───────────────┐
                    ▼               ▼               ▼
              0-15%: ENTROPY   15-85%: GEO     85%+: REBUILD
              XOR+RLE compress  Byte ranges     Flush+re-snapshot
```

### Integration Points in Runner

- **Init** (after model load): `kv_remap_register()` — stores KV pointers, no copy
- **`/rscan`** (first use): `kv_remap_set_skeleton()` — copies + compresses KV as baseline
- **Before decode**: `kv_remap_rail_freeze()` — pause rail scan
- **After decode**: `kv_remap_rail_resume()` — resume rail scan
- **After generation**: `kv_remap_rail_step()` + `kv_remap_cycle()` — idle work (only if skeleton valid)
- **Exit**: `kv_remap_rail_destroy()` + `kv_remap_destroy()`

### Windows Compatibility

On Windows, the runner defines a custom `clock_gettime(PoglsTime*)` using `QueryPerformanceCounter`. The rail header uses `POGLS_RAIL_USE_POGTIME` macro to switch between `PoglsTime` (runner) and `struct timespec` (standalone tests).

## Testing

```bash
cd runner
gcc -O2 -std=c11 -I. -o test_kv_remap.exe test_kv_remap.c -lm
./test_kv_remap.exe
```

### Test Suite (7 tests)

| Test | Description |
|---|---|
| test_classify | Classify at 0/5/10/15/20/40/50/70/85/90/100% change |
| test_full_cycle | Store delta → restore → verify data integrity |
| test_geo_delta | GEO range generation at 50% change |
| test_rebuild | Rebuild path at 90% change |
| test_rail | Full rail scan to completion |
| test_rail_freeze | Rail scan with mid-scan freeze/resume |
| test_streaming | 10-turn incremental perturbation, auto-rebuild at 85%+ |

## Metrics

Rail scan prints per-lane stats:
```
[rail] scan started: 6 layers across 3 lanes
  lane0: layers[0..2) 524288 bytes
  lane1: layers[2..4) 524288 bytes
  lane2: layers[4..6) 524288 bytes
[rail] scan complete: 282437/1572864 = 17% (4.8 ms)
[rail] decision: PATCH (17% > 15%)
```

Remap prints skeleton/delta stats:
```
[kv-remap] skeleton: orig=1572864 comp=686275 ratio=2.29x
[kv-remap] store: DELTA_ENTROPY (9%), raw_xor=1572864, compressed=686275
```
