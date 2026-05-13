# Canonical Merge Audit
# Date: 2026-05-12

## Scope

This audit merges the active system path across:

- `core/core`
- `src`
- `active_updates`
- `Hfolder`
- `geopixel`
- `frustum`

Goal:

- identify what can already be treated as one active path
- flag what is still missing or structurally blocked
- record what is outside the current merge path

Exclusions:

- duplicate mirrors
- older superseded versions
- archived zip bundles
- generated binaries and media artifacts

## Active Merge Path

### 1. Canonical geometry and stream core

Treat these as the current structural base:

- `core/core/geo_tring_stream.h`
- `core/core/geo_tring_goldberg_wire.h`
- `core/core/geo_temporal_ring.h`
- `core/core/geo_goldberg_tring_bridge.h`
- `core/core/pogls_goldberg_bridge.h`
- `core/core/pogls_stream.h`

Reason:

- these files form the geometry, temporal, stream, and Goldberg bridge line
- they are the least tied to donor bundles or experimental packaging

### 2. Runtime integration seam

Treat these as the current runtime seam:

- `src/tgw_stream_dispatch.h`
- `src/tgw_frustum_wire.h`
- `src/tgw_dispatch_v2.h`
- `src/tgw_dispatch_v2_tri5.h`
- `src/fabric_wire.h`
- `src/geopixel_v21.c`

Reason:

- this is the live integration surface where dispatch, TRing, route, and GeoPixel CLI come together

### 3. GeoPixel anim/container donor line

Treat these as the active donor source for GPX4/anim support:

- `Hfolder/geo_gpx_anim.h`
- `Hfolder/gpx4_container.h`
- `Hfolder/geo_o4_connector.h`
- `Hfolder/geo_pixel.h`
- `Hfolder/geo_tring_addr.h`
- `Hfolder/geo_goldberg_tile.h`

Current state:

- `src/*.h` wrapper headers already forward to this donor set
- `src/geopixel_v21.c` now resolves the expected include names through those wrappers

### 4. Handoff feature cluster that still matters

Treat these as the feature cluster that should be promoted, not ignored:

- `active_updates/frustum_layout_v2.h`
- `active_updates/fabric_wire_drain.h`
- `active_updates/pogls_1440.h`
- `active_updates/pogls_rotation.h`
- `active_updates/heptagon_fence.h`
- `active_updates/pogls_atomic_reshape.h`

Reason:

- this cluster is internally coherent
- tests and handoff notes describe it as the frozen/locked path for drain, 1440 overlay, rotation, fence, and atomic reshape
- `src/fabric_wire.h` already reflects part of this migration via `POGLS_TRING_CYCLE = 1440`

## Flags

### F1. `src` is not self-contained yet

Flag: `src` still depends on `Hfolder` for the GPX4/anim container stack.

Evidence:

- `src/geo_gpx_anim.h` includes `../Hfolder/geo_gpx_anim.h`
- `src/gpx4_container.h` includes `../Hfolder/gpx4_container.h`
- `src/geo_o4_connector.h` includes `../Hfolder/geo_o4_connector.h`
- `src/geo_pixel.h` includes `../Hfolder/geo_pixel.h`
- `src/geo_tring_addr.h` includes `../Hfolder/geo_tring_addr.h`

Impact:

- `src/geopixel_v21.c` is wired, but the canonical source still lives outside `src`
- the runtime path is not yet self-owned

### F2. handoff frustum/reshape cluster is not promoted into `src`

Flag: the frustum/drain/reshape chain is still absent from `src`.

Missing from `src`:

- `frustum_layout_v2.h`
- `fabric_wire_drain.h`
- `pogls_1440.h`
- `pogls_rotation.h`
- `heptagon_fence.h`
- `pogls_atomic_reshape.h`

Impact:

- `src` has the 1440 constant in `fabric_wire.h`, but does not own the full behavioral cluster
- atomic reshape and drain/fence semantics remain stranded in handoff and Metatron copies

### F3. `tgw_stream_dispatch` is structurally important but still blocked by compile/test debt

Flag: the dispatch seam exists, but the checkpoint path still records unresolved compile/test issues.

Evidence:

- `geopixel/checkpoint/HANDOFF.md` notes duplicate typedef conflicts
- `geopixel/checkpoint/P0_CHECKPOINT.md` notes duplicate typedef issues
- `geopixel/checkpoint/P1_CHECKPOINT.md` notes full compilation conflicts
- `geopixel/checkpoint/EXISTING_TESTS_MAP.md` says `test_tgw_stream_dispatch.c` had not been run

Impact:

- the stream-to-dispatch seam is designed, but not yet closed as a verified canonical path

### F4. GeoPixel build path still depends on external image/compression headers

Flag: the source tree is not build-self-sufficient for GeoPixel.

External requirements observed in the code:

- `png.h`
- `zstd.h`

Impact:

- source structure is mostly wired
- build reproducibility still depends on toolchain/include configuration outside the repo

## Record: Not In Current Merge Path

These are not being marked as duplicates or old versions. They are being recorded as outside the current merge target.

### R1. Paperwallet / coord-wallet line

Files:

- `core/pogls_engine/pogls_coord_wallet.h`
- `core/pogls_engine/pogls_paperwallet.html`
- `core/pogls_engine/pogls_paperwallet_v2.html`
- `geopixel/wallet/*`

Reason:

- useful as optional structural envelope
- not required for the current `shape sequence / frame timeline / layer group` merge path

### R2. Hamburger / GPX5 line

Files:

- `geopixel/gpx5_container.h`
- `geopixel/hamburger_classify.h`
- `geopixel/hamburger_pipe.h`
- `geopixel/hamburger_encode.h`
- `geopixel/test_hamburger.c`
- `geopixel/test_hamburger2.c`

Reason:

- this path is a working prototype/runtime surface
- it is not the same thing as the GPX4 animated shape-sequence merge path
- keep it recorded as a parallel consumer path, not part of the current canonical merge

### R3. Historical frustum phase stacks

Folders:

- `frustum/phase1`
- `frustum/phase2`
- `frustum/phase3`
- `frustum/phase4`
- `frustum/phase5`
- `frustum/phase6`
- `frustum/phase8`
- `frustum/phase9`
- `frustum/phase10`

Reason:

- important historical lineage
- not the active merge target while `core/core + src + handoff feature cluster + Hfolder donor` is still unresolved

## Explicitly Ignored In This Audit

The following were intentionally not pulled into decision-making:

- `active_updates/handoff_h_c/*` mirror copies
- versioned GeoPixel alternates such as `geopixel_v18.c`, `geopixel_v19.c`, `geopixel_v20*.c`
- archive bundles such as `*.zip`
- built binaries such as `*.exe`
- generated images and sample artifacts such as `*.bmp`, `*.gpx4`, `*.gpx5`

## Recommended Merge Order

1. Promote the handoff frustum/reshape cluster into `src`
2. Replace `src` wrapper includes with canonical owned copies of the GPX4/anim donor headers
3. Resolve `tgw_stream_dispatch` typedef/test debt
4. Only then wire full `stream -> dispatch -> GeoPixel anim/container`

## Minimal Next Actions

1. add canonical `src` copies for:
   - `frustum_layout_v2.h`
   - `fabric_wire_drain.h`
   - `pogls_1440.h`
   - `pogls_rotation.h`
   - `heptagon_fence.h`
   - `pogls_atomic_reshape.h`
2. choose one canonical home for:
   - `geo_gpx_anim.h`
   - `gpx4_container.h`
   - `geo_o4_connector.h`
   - `geo_pixel.h`
   - `geo_tring_addr.h`
3. verify `test_tgw_stream_dispatch.c`
4. verify `anim -> gpx4 -> animdecode`
