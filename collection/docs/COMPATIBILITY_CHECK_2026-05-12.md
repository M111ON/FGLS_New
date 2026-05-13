# Compatibility Check — 2026-05-12

## Scope
- Workspace: `I:/FGLS_new/collection`
- Canonical policy:
  - `core/` = root core
  - `geopixel/` = active update lane
  - `active_updates/` = active update lane
  - `src/` = bridge/integration surface

## Changes Applied
- Updated include policy in `Makefile.integration`:
  - from: `-Isrc -Icore/pogls_engine -Icore/core`
  - to: `-Isrc -Icore/core -Icore/geo_headers -Icore/pogls_engine -Igeopixel -Iactive_updates`

## Structural Compatibility
- `src` no longer includes `../Hfolder/` in active bridge headers.
- All `__has_include(...)` targets in `src/*.h` resolve to existing files.
- GeoPixel bridge headers point to `geopixel/geopixel/*`.
- Frustum/reshape bridge headers point to `active_updates/*`.

## Build and Test Results

### Environment Notes
- `make` is not available in current PowerShell environment.
- Used direct `gcc` compile for integration checks.

### PASS
- `tests/integration/test_metatron_route.c`
  - compile: PASS
  - run: PASS (`37 PASS, 0 FAIL`)
- `tests/integration/test_frustum_wire.c`
  - compile: PASS
  - run: PASS (`20 PASS, 0 FAIL`)

### RESOLVED (Harness Drift)
- `tests/integration/test_dispatch_v2.c`
- `tests/integration/test_ground_lcgw.c`
- `tests/integration/test_rewind_lcgw.c`

Fix applied:
- Added `TEST_STUB_MODE` gating in each file.
- Default build path is `REAL_MODE` (`TEST_STUB_MODE=0`) using canonical headers.
- Legacy local stubs remain available only when explicitly enabled with `-DTEST_STUB_MODE=1`.

Post-fix results:
- `test_dispatch_v2`: compile PASS, run PASS (`25 PASS, 0 FAIL`)
- `test_ground_lcgw`: compile PASS, run PASS (`21 PASS, 0 FAIL`)
- `test_rewind_lcgw`: compile PASS, run PASS (`23 PASS, 0 FAIL`)

## Current Compatibility Verdict
- Core bridge integration is structurally consistent.
- Integration tests now pass in real-header mode across dispatch, ground hook, rewind, metatron, and frustum paths.
- Remaining compiler warnings are non-blocking:
  - signed/unsigned compare in `core/core/geo_temporal_ring.h`
  - implicit `tgw_batch` declaration path in `src/tgw_dispatch_v2.h` under current compile setup
