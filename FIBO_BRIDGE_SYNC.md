# Fibo Clock Bridge Sync Map

This repo currently has the pieces needed for a shared timeline, but they are still split across three islands:

- `core/core` owns the shared core semantics
- `core/pogls_engine` owns runtime and app-facing POGLS surfaces
- `Hfolder` owns the GPX / animated GPX visual codec path

The intended integration model is:

1. `fibo clock` is the timeline authority.
2. `bridge` is the only supported translation layer between subsystems.
3. `adapter` modules expose each subsystem onto the shared contract.

## Canonical roles

### 1) Source of truth

Use `core/core` as the canonical base for shared primitives:

- `core/core/geo_fibo_clock.h`
- `core/core/pogls_sdk.h`
- `core/core/geo_pipeline_wire.h`
- `core/core/geo_route.h`
- `core/core/geo_tring_stream.h`
- `core/core/pogls_reconstruct.h`
- `core/core/pogls_engine_slice.h`

These headers already carry the shared clock, bridge, routing, cache, and pipeline semantics that other layers should consume instead of redefining.

### 2) Runtime layer

Use `core/pogls_engine` as the executable/runtime surface:

- `core/pogls_engine/pogls_engine.py`
- `core/pogls_engine/pogls_sdk.h`
- `core/pogls_engine/pogls_twin_bridge.h`
- `core/pogls_engine/pogls_bridge_s53.py`
- `core/pogls_engine/pogls_bridge_s54.py`

This layer should call into the shared core contract and should not become a second source of truth for clock or geometry semantics.

### 3) Visual codec layer

Use `Hfolder` as the GPX projection and animation layer:

- `Hfolder/geo_pixel.h`
- `Hfolder/geo_tring_addr.h`
- `Hfolder/geo_o4_connector.h`
- `Hfolder/gpx4_container.h`
- `Hfolder/geo_gpx_decode.h`
- `Hfolder/geo_gpx_anim.h`
- `Hfolder/geopixel_v20.c`

This layer should consume the same clock / bridge contract and only specialize in visual/container encoding.

## Timing contract

The shared timeline should treat the following as aligned boundaries:

- `17` for signal cadence
- `72` for drift cadence
- `144` for flush / ThirdEye boundary
- `720` for full snapshot cadence

Those values already appear in `core/core/geo_fibo_clock.h`, so that header should remain the clock authority.

## Integration shape

The clean integration shape is:

```text
fibo clock
  -> bridge contract
    -> core/core primitives
      -> core/pogls_engine runtime
      -> Hfolder GPX / animated GPX adapter
```

## What not to do

- Do not let `Hfolder` invent its own parallel clock semantics.
- Do not let `core/pogls_engine` duplicate routing or bridge rules that already belong in `core/core`.
- Do not wire GPX container logic directly into runtime code without going through the shared bridge contract.

## Recommended first refactor slice

1. Keep `core/core/geo_fibo_clock.h` as the only clock authority.
2. Expose a thin bridge adapter for POGLS runtime events.
3. Expose a thin adapter for GPX / animated GPX frame emission.
4. Route both adapters through the same timeline tick and flush boundaries.

## Practical success criterion

The system is synced when a single fibo tick can drive:

- POGLS write/read or replay events
- GPX frame emission or decode events
- flush / snapshot boundaries
- bridge translation without separate ad hoc timing rules

