# Test Run Log (2026-05-08)

## Order (newest-first)
1. test_frustum_wire
2. test_metatron_route
3. test_rewind_lcgw
4. test_ground_lcgw
5. test_dispatch_v2

## Results
- PASS: `test_frustum_wire` (exit 0)
- PASS: `test_metatron_route` (exit 0)
- PASS: `test_rewind_lcgw` (exit 0)
- PASS: `test_ground_lcgw` (exit 0)
- FAIL: `test_dispatch_v2` (exit 1)

## Failure Notes (do not force in this round)
### test_dispatch_v2
- V04 failed: "flushed batch sorted ASC by hilbert_lane (linear)"
- V06 failed:
  - expected `ground=360 (50% polarity)`
  - expected `route=360 (50% polarity)`
- V07 failed:
  - expected pending unchanged after GROUND pkt (linear)
  - expected ground_fn called for GROUND pkt (linear)

## Interpretation
- Core dispatch behavior appears shifted from assumptions in this test profile.
- Newer core path (cardioid/express + route/ground accounting) likely changed expected linear-mode invariants.
- Keep this test as regression signal, but update expectation model before treating as blocker.
