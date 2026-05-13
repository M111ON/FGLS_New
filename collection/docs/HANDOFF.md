# CPU Pipeline v1 + GROUND LC-GCFS Hook — COMPLETE
_Session: 2026-05-06_

## System Status

| Layer | Component | Status |
|---|---|---|
| L5 | SpaceFrame (TRing/Goldberg) | ✅ frozen |
| L4 | geo_addr_net.h (SEAM 2) | ✅ fixed (spoke formula) |
| L3 | geopixel_v21.c (SEAM 3) | ✅ done |
| L2 | tgw_dispatch_v2.h (P4+P6) | ✅ done |
| L1 | tgw_ground_lcgw.h | ✅ NEW — GROUND→LC-GCFS wired |

## Test Summary

```
test_tring_walk.c:     6/ 6  PASS
test_dispatch_v2.c:   24/24  PASS
test_cpu_pipeline.c:  15/15  PASS  (regression clean)
test_ground_lcgw.c:   16/16  PASS
─────────────────────────────────
Total:                61/61  PASS
```

## Bug Fixed: geo_addr_net.h spoke formula

```c
// WRONG: spoke = (pos / 60) % 6
//   → spoke 0,2,4 = ROUTE only
//   → spoke 1,3,5 = GROUND only

// CORRECT: spoke = (pos / 120) % 6
//   → every spoke: 60 ROUTE + 60 GROUND (exact 50/50)
```

## GROUND Path Architecture

```
tgw_dispatch_v2 GROUND early-exit
  → tgw_ground_fn(addr, val, &gs)       ← TGWGroundFn callback
    → lcgw_ground_write(&gs, addr, val)
      → spoke = geo_net_encode(addr).spoke  (0..5)
      → seed  = addr ^ val ^ mixing
      → lcgw_build_from_seed()           ← deterministic, no malloc
      → LCGWSpokeLane[spoke].chunk[]

Ghost delete:
  lcgw_ground_delete(&gs, spoke)
  → lcfs_delete_atomic() triple-cut
  → lane.ghosted = 1
  → lcgw_read_payload() → NULL           ← "present but unreachable"
```

## Key Properties (GROUND uniqueness)

- **Geometry-driven split:** TRing physics guarantees 50/50 ROUTE/GROUND
- **Per-spoke isolation:** 6 independent LC-GCFS lanes
- **Ghost delete:** content preserved, path severed (LC triple-cut)
- **Deterministic:** seed = f(addr, val) → no randomness
- **Zero malloc:** all on stack / static arrays

## Constants (frozen)

```c
GAN_TRING_CYCLE    = 720
GAN_SPOKES         = 6
spoke formula      = (pos / 120) % 6    ← NOT pos/60
polarity formula   = (pos % 120) >= 60
LCGW_GROUND_SPOKES = 6
LCGW_GROUND_SLOTS  = 16   /* chunks per spoke lane */
```

## Next Steps

1. bench_ground.c — measure GROUND write throughput vs ROUTE
2. counting sort — replace insertion sort in tgw_dispatch_v2 (bottleneck: 852 swaps/flush)
3. LC-GCFS rewind test — verify seed reconstruct after ghost delete
