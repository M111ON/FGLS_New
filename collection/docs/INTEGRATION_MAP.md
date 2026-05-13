# Integration Map

## Canonical Source Layout
- `core/core/` is the root canonical core for geometry, temporal, Goldberg, and POGLS primitives.
- `core/geo_headers/` and `core/pogls_engine/` provide shared engine and GCFS dependencies.
- `src/` is the canonical integration surface for TGW/LCGW wire headers and additive routing hooks.
- `geopixel/` and `active_updates/` are the active update areas and may carry newer surface changes than the root core.

## Include Policy
- Use include roots: `-Icore/core -Icore/geo_headers -Icore/pogls_engine -Isrc -Igeopixel -Iactive_updates`.
- Do not use fragile relative includes like `../pogls_engine/...` from `src`.
- Keep `active_updates/` as the active update/snapshot lane, not the root canonical core.

## Merge Rule
- Prefer the newest active implementation from `geopixel/` and `active_updates/` when it clearly supersedes a root copy.
- Keep root `core/` files as the baseline unless a newer active file exists in the update folders.
- Do not re-import older versioned alternates or archive bundles just because they are nearby.

## Added From Handoff Into Canonical `src/`
- `frustum_gcfs.h`
- `frustum_slot64.h`
- `frustum_trit.h`
- `geo_goldberg_lut.h`
- `geo_metatron_route.h`
- `geo_temporal_lut.h`
- `lcgw_adaptive.h`
- `lc_hdr_lazy.h`
- `tgw_dispatch_v2_tri5.h`
- `tgw_frustum_wire.h`
- `tgw_ground_lcgw_lazy.h`

## Compatibility Shims Applied
- `src/lc_gcfs_wire.h`
: added `LCGW_RAW_BYTES` alias to `GCFS_TOTAL_BYTES`.
: added `lcgw_reset()` as back-compat helper that calls `lcgw_init()`.
- `src/tgw_ground_lcgw.h`
: rewired `lcgw_ground_rewind()` to match current `lcgw_rewind()` API surface by rebuilding chunks from saved lane seeds.
