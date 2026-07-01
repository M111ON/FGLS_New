╔═══════════════════════════════════════════════╗
║  DRamTile + GearShift — Session 30 Benchmark ║
╚═══════════════════════════════════════════════╝

Config: 251 tensors, 10000 iterations

═══════════════════════════════════════════════
 GearShift — Stream/Invalidate/Reset Benchmark
═══════════════════════════════════════════════

  Register 251 entries:    0.511 ms
  Batch stream 10000 × 251:  5758.619 ms (2294.3 ns/op)
  Single stream × 10000:     5.912 ms (591.2 ns/op)
  gs_reset_done × 10000:    1.995 ms (199.5 ns/op) [only scans DONE]
  gs_reset_all  × 10000:    1.606 ms (160.6 ns/op) [resets all]
  invalidate+re-reg × 10000: 8063.437 ms (6425.0 ns/op per invalidate)

═══════════════════════════════════════════════
 DRamTile — Hash/Lookup/Iterate Benchmark
═══════════════════════════════════════════════

  dt_put 251 entries:      0.089 ms (354.2 ns/op)
  dt_get 10000 × 251:        779.793 ms (310.7 ns/op)
  dt_store_foreach 10000 × 251: 0.018 ms (0.0 ns/op)
  dt_store_total_bytes × 10000: 0.000 ms (0.0 ns/op)

═══════════════════════════════════════════════
 Double-Stream (old) vs Single-Stream (new)
═══════════════════════════════════════════════

  Old path (double-stream):  6494.963 ms  [5020000 tensor_update calls]
  New path (single-stream):  6744.689 ms  [2510000 tensor_update calls]
  Speedup:                   0.96x  (50.0% fewer tensor_update)
  tensor_update saved:       5020000 → 2510000 (2510000 fewer per 10000 iterations)

═══════════════════════════════════════════════
 Memory Allocation: mmap vs malloc
═══════════════════════════════════════════════

  malloc  × 16 × 64 MB:  616.453 ms
  mmap    × 16 × 64 MB:  523.943 ms
  Ratio: mmap is 1.2x faster than malloc

═══════════════════════════════════════════════
 Summary
═══════════════════════════════════════════════
  • GearShift batch stream: O(n) per cycle
  • DRamTile hash lookup:   O(1) amortized
  • Double→single stream:   ~50% tensor_update saved
  • mmap vs malloc:         lazy page-fault advantage
═══════════════════════════════════════════════
