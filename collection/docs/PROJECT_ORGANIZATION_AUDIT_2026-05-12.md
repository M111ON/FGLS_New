# Project Organization Audit
# Date: 2026-05-12

## Goal

Organize the active FGLS collection workspace into three buckets:

1. what is already canonical and should stay together
2. what should be flagged because it is missing, blocked, or still split
3. what should be recorded as unused, older, duplicate, or superseded

Rules used here:

- keep newer canonical files out of the duplicate list
- do not pull in parallel artifacts just because they are present
- record staging/snapshot material as reference-only unless it is the live source of truth

## Canonical Active Core

These areas are the main live path for this checkout:

- `core/core/`
- `core/geo_headers/`
- `core/pogls_engine/`
- `src/`
- `geopixel/`
- `active_updates/`

Why:

- `src/` is the live TGW/LCGW/GeoPixel integration surface
- `core/core/` carries the lower-level geometry and POGLS primitives
- `core/geo_headers/` carries the shared header set used by the active runtime
- `core/pogls_engine/` carries engine dependencies and bridge headers
- `geopixel/` and `active_updates/` are the newer active update lanes, so they should win when they clearly supersede a root copy

## Systems To Keep Together

### 1. TGW / dispatch / wire path

Keep these together:

- `src/tgw_dispatch.h`
- `src/tgw_dispatch_v2.h`
- `src/tgw_dispatch_v2_tri5.h`
- `src/tgw_stream_dispatch.h`
- `src/tgw_frustum_wire.h`
- `src/tgw_lc_bridge.h`
- `src/tgw_lc_bridge_p6.h`
- `src/tgw_ground_lcgw.h`
- `src/tgw_ground_lcgw_lazy.h`
- `src/tgw_tri5_wire.h`

Reason:

- these files form the active wire/dispatch seam
- they should be treated as one runtime family, not as isolated headers

### 2. LC / GCFS / deletion bridge

Keep these together:

- `src/lc_api.c`
- `src/lc_delete.h`
- `src/lc_fs.h`
- `src/lc_hdr.h`
- `src/lc_hdr_lazy.h`
- `src/lc_gcfs_wire.h`
- `src/lc_twin_gate.h`
- `src/lc_wire.h`
- `src/fgls_twin_store.h`
- `core/pogls_engine/lc_twin_gate.h`
- `core/pogls_engine/fgls_twin_store.h`

Reason:

- this is the runtime contract for storage, delete, and twin-store behavior
- duplicated header names across `src/` and `core/pogls_engine/` should be checked for canonical ownership

### 3. Geometry / temporal / Goldberg chain

Keep these together:

- `core/core/geo_temporal_ring.h`
- `core/core/geo_temporal_lut.h`
- `core/core/geo_tring_stream.h`
- `core/core/geo_tring_goldberg_wire.h`
- `core/core/geo_goldberg_tring_bridge.h`
- `core/core/pogls_goldberg_bridge.h`
- `core/core/pogls_stream.h`
- `core/core/pogls_payload_store.h`
- `core/core/pogls_route.h`

Reason:

- this is the main geometry and stream backbone
- it is the correct place to anchor any canonical merge work

### 4. GeoPixel / GPX / container line

Keep these together:

- `geopixel/geopixel_v21.c`
- `geopixel/geo_gpx_anim.h`
- `geopixel/gpx4_container.h`
- `geopixel/geo_o4_connector.h`
- `geopixel/geo_pixel.h`
- `geopixel/geo_tring_addr.h`
- `geopixel/geo_goldberg_tile.h`
- `geopixel/geo_metatron_route.h`
- `src/geopixel_v21.c`
- `src/geo_gpx_anim.h`
- `src/gpx4_container.h`
- `src/geo_o4_connector.h`
- `src/geo_pixel.h`
- `src/geo_tring_addr.h`
- `src/geo_goldberg_tile.h`
- `src/geo_metatron_route.h`

Reason:

- this is the active GeoPixel container/animation line that the runtime still depends on
- the `geopixel/` copies should be treated as the newer update lane when they differ from root wrappers
- these wrappers are part of the present integration surface, not dead code

## Flags

### F1. `src/` is not self-contained yet

Flag:

- `src/geo_gpx_anim.h`
- `src/gpx4_container.h`
- `src/geo_o4_connector.h`
- `src/geo_pixel.h`
- `src/geo_tring_addr.h`

still forward into `Hfolder/`.

Impact:

- `src/` is wired, but not fully owned
- the canonical source tree still depends on external donor headers
- some of those donor roles may now be superseded by `geopixel/` copies, so the next pass should compare those first before copying anything from `Hfolder/`

### F2. Handoff cluster still needs promotion or a clear decision

Flag these as live but not yet fully promoted:

- `active_updates/frustum_layout_v2.h`
- `active_updates/fabric_wire_drain.h`
- `active_updates/pogls_1440.h`
- `active_updates/pogls_rotation.h`
- `active_updates/heptagon_fence.h`
- `active_updates/pogls_atomic_reshape.h`

Impact:

- these belong to the same behavioral cluster
- the tree currently carries them in handoff form rather than a single owned canonical location
- treat them as active updates, not historical archive, until a newer root copy exists

### F3. Duplicate name collisions need ownership decisions

Flag these name overlaps for review:

- `src/lc_twin_gate.h` and `core/pogls_engine/lc_twin_gate.h`
- `src/fgls_twin_store.h` and `core/pogls_engine/fgls_twin_store.h`
- `src/geo_pixel.h` and `Hfolder/geo_pixel.h`
- `src/geo_tring_addr.h` and `Hfolder/geo_tring_addr.h`
- `src/geo_gpx_anim.h` and `Hfolder/geo_gpx_anim.h`
- `src/gpx4_container.h` and `Hfolder/gpx4_container.h`

Impact:

- these are not necessarily bugs, but they do mean the tree is split between canonical wrappers and donor copies
- ownership should be explicit so later cleanup does not delete the wrong side

### F4. Build reproducibility still has external dependencies

Flag:

- `png.h`
- `zstd.h`

Impact:

- the repository still depends on toolchain/include availability outside the tree
- build issues here are environmental as well as structural

## Record As Older / Unused / Superseded

These should be recorded, not merged into the active canonical path.

### O1. Older GeoPixel source variants

Treat these as older or alternate variants:

- `Hfolder/geopixel_v18.c`
- `Hfolder/geopixel_v19.c`
- `Hfolder/geopixel_v20.c`
- `Hfolder/geopixel_v20_o23.c`
- `Hfolder/geopixel_v20_o24.c`
- `Hfolder/geopixel_v20_o24_fix.c`
- `Hfolder/geopixel_v20_o24_fixed.c`
- `Hfolder/geopixel_v20_o24_fixed2.c`
- `Hfolder/geopixel_v20_o24_fixed3.c`
- `Hfolder/geopixel_v20_o24_fixed4.c`
- `Hfolder/geopixel_v21_delta.c`
- `Hfolder/geopixel_v21_clean.c`
- `Hfolder/geopixel_v21_o25.c`

Reason:

- these are versioned experiment branches, not a single canonical runtime source
- keep only the newest relevant line in active use

### O2. Archive and bundle material

Treat these as reference/archive only:

- `zip/*.zip`
- `lc_gcfs_pkg.zip`
- `active_updates/*.zip` if present in future refreshes
- built binaries such as `*.exe`
- generated image/output artifacts such as `*.bmp`, `*.webp`, `*.gpx`, `*.gpx2`, `*.gpx4`

Reason:

- they are outputs or transfer bundles
- they should not be merged into the runtime tree

### O3. Scratch / inspection artifacts

Treat these as non-canonical unless explicitly promoted:

- `context_active_*.md`
- `context_skeleton_*.md`
- `gcc.out`
- `gcc.err`
- ad hoc preview/entropy documents under `Hfolder/`

Reason:

- these are investigation artifacts or temporary outputs
- they are useful for history, not for the active source path

## What Still Needs A Decision

1. Whether the `Hfolder/` donor headers should be copied into `src/` or whether `src/` should keep thin wrappers permanently
2. Which side owns `lc_twin_gate.h` and `fgls_twin_store.h`
3. Whether the `active_updates` frustum/reshape cluster is to be promoted or archived as a frozen feature set
4. Whether any of the `geopixel_v20_*` variants should be kept as reference baselines or dropped from the working set

## Connection Seams

These are the current railyards where the system actually joins together:

### A. Dispatch seam

- `src/tgw_dispatch.h`
- `src/tgw_dispatch_v2.h`
- `src/tgw_dispatch_v2_tri5.h`
- `src/tgw_stream_dispatch.h`
- `src/tgw_frustum_wire.h`
- `src/tgw_lc_bridge.h`
- `src/tgw_lc_bridge_p6.h`

This seam is the main TGW entry path.

### B. LC / GCFS seam

- `src/lc_hdr.h`
- `src/lc_hdr_lazy.h`
- `src/lc_fs.h`
- `src/lc_delete.h`
- `src/lc_gcfs_wire.h`
- `src/lc_api.c`

This seam handles storage, rewind, and delete semantics.

### C. GeoPixel seam

- `src/geo_pixel.h`
- `src/geo_tring_addr.h`
- `src/geo_o4_connector.h`
- `src/gpx4_container.h`
- `src/geo_gpx_anim.h`

This seam is the active GPX/container chain.

### D. Frustum / reshape seam

- `src/frustum_gcfs.h`
- `src/frustum_slot64.h`
- `src/frustum_trit.h`
- `src/frustum_layout_v2.h`
- `src/fabric_wire.h`
- `src/fabric_wire_drain.h`
- `src/pogls_rotation.h`
- `src/pogls_1440.h`
- `src/heptagon_fence.h`
- `src/pogls_atomic_reshape.h`

This seam is the active advanced transform chain and should stay grouped.

### E. Geometry backbone

- `core/core/geo_temporal_ring.h`
- `core/core/geo_temporal_lut.h`
- `core/core/geo_tring_stream.h`
- `core/core/geo_tring_goldberg_wire.h`
- `core/core/geo_goldberg_tring_bridge.h`
- `core/core/pogls_goldberg_bridge.h`
- `core/core/pogls_stream.h`

This is the root backbone and should remain the baseline for root-owned primitives.

## Short Operating Rule

- keep `src/` + `core/core/` + `core/geo_headers/` + `core/pogls_engine/` as the active base
- fold in only the handoff pieces that are still live and not already superseded
- record older versions, archives, and generated outputs separately
- do not re-merge files that already have a newer canonical equivalent
