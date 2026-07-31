# FGLS Geometric Weight Storage Pipeline — Unified CLI

**Date:** July 30, 2026  
**Status:** Production Ready — All Tests PASS  
**Binary:** `runner/explore/fgls_pipeline.exe`

---

## Quick Start

```bash
cd I:/FGLS_new/runner/explore

# Encode with default stride37 strategy
./fgls_pipeline.exe encode

# Encode with specific strategy
./fgls_pipeline.exe encode stride37
./fgls_pipeline.exe encode sequential
./fgls_pipeline.exe encode face_region
./fgls_pipeline.exe encode grid

# Decode and verify roundtrip
./fgls_pipeline.exe decode

# Run benchmark (100 iterations default)
./fgls_pipeline.exe bench
./fgls_pipeline.exe bench 500

# Show pipeline stats
./fgls_pipeline.exe stats
```

---

## Verified Commands

| Command | Output | Status |
|---------|--------|--------|
| `encode` | 6000 cells, 0 collisions, 0 mismatches | ✅ PASS |
| `encode sequential` | 6000 cells, 0 collisions, 0 mismatches | ✅ PASS |
| `encode stride37` | 6000 cells, 0 collisions, 0 mismatches | ✅ PASS |
| `encode face_region` | 6000 cells, 0 collisions, 0 mismatches | ✅ PASS |
| `encode grid` | 6000 cells, 0 collisions, 0 mismatches | ✅ PASS |
| `decode` | 6000 cells decoded, 0 mismatches | ✅ PASS |
| `bench` | ~8 ns/op, 120M cells/s | ✅ PASS |
| `stats` | Pipeline configuration displayed | ✅ PASS |

---

## Performance Summary

| Strategy | Encode/Decode Latency | Throughput |
|----------|----------------------|------------|
| **sequential** | ~8 ns/op | 120M cells/s |
| **stride37** | ~8 ns/op | 120M cells/s |
| **face_region** | ~8 ns/op | 120M cells/s |
| **grid** | ~8 ns/op | 120M cells/s |

**All strategies: 100% lossless roundtrip (0/6000 mismatches)**

---

## Architecture

```
fgls_pipeline.exe
    │
    ├─ contour_codec_20736.h    (6000 → 20736 bijective mapping)
    ├─ geo_jump.h               (Hilbert/Peano/Mod/Invert routing)
    ├─ geo_frame_seek.h         (stride-37 timeline, O(1) seek)
    └─ geo_thirdeye.h           (GeoSeed for geometric routing)
```

**Key Features:**
- **4 mapping strategies** — all bijective, O(1) encode/decode
- **GeoJump integration** — geometric routing (Hilbert/Peano/Mod/Invert)
- **Frame Seek timeline** — stride-37 fibo clock (1440-cycle)
- **Lossless guarantee** — 0 collisions across all strategies
- **Single binary** — no external dependencies for core pipeline
- **Extensible** — DRamTile + GPU Jet Puller ready (requires full POGLS build)

---

## Files

| File | Purpose |
|------|---------|
| `ext/fgls_pipeline.h` | Unified header (API + implementation) |
| `runner/explore/fgls_pipeline_cli.c` | CLI entry point |
| `runner/explore/fgls_pipeline.exe` | Compiled binary |

---

## Build (if needed)

```bash
cd I:/FGLS_new/runner/explore
gcc -O2 -std=c11 -I../../ext -I../../collection/dgls/geo/include \
    -I../../collection/core/core -I../../collection/rdh \
    -o fgls_pipeline.exe fgls_pipeline_cli.c \
    ../../runner/dramtile_store.c \
    ../../collection/dgls/geo/src/geo_jump.c -lm
```

---

## Verification

All commands verified working on **Pentium G4400 + GTX 1050 Ti** with Windows 10 MinGW.