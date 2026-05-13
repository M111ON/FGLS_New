# Dispatch V2 Profile Baseline (2026-05-08)

## Purpose
Separate strict legacy assumptions from roadmap behavior during core transition.

## Profiles
- ROADMAP_1440 (default):
  - compile: `gcc -O2 -Wall -Wextra -Isrc -Icore/pogls_engine -Icore/core tests/integration/test_dispatch_v2.c -o build/integration/test_dispatch_v2_roadmap.exe`
  - run: `build/integration/test_dispatch_v2_roadmap.exe`
- LEGACY_720 (strict):
  - compile: `gcc -O2 -Wall -Wextra -DTEST_PROFILE_LEGACY_720=1 -Isrc -Icore/pogls_engine -Icore/core tests/integration/test_dispatch_v2.c -o build/integration/test_dispatch_v2_legacy.exe`
  - run: `build/integration/test_dispatch_v2_legacy.exe`

## Observed Results
- ROADMAP_1440: `25 PASS / 0 FAIL` (exit 0)
- LEGACY_720: `19 PASS / 5 FAIL` (exit 1)

## Legacy Fail Set (expected during transition)
- V04: strict sorted-by-hilbert expectation
- V06: strict 50/50 ground-route split (360/360)
- V07: strict ground linear fallback semantics

## Interpretation
- Core dispatch behavior is now consistent with cardioid/express-first routing.
- Legacy checks remain useful as drift signal but should not block roadmap integration.
