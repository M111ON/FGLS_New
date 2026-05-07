# Handoff: Fibo Clock Bridge Sync

## Current status

We have identified the shared architecture path:

- `core/core` is the canonical semantic layer
- `core/pogls_engine` is the runtime/app surface
- `Hfolder` is the GPX / animated GPX visual codec surface
- `fibo clock` is the timeline authority
- `bridge` is the translation layer between subsystems

I also added a more detailed map at:

- [FIBO_BRIDGE_SYNC.md](/I:/FGLS_new/FIBO_BRIDGE_SYNC.md)

## What was verified

- `core/core/geo_fibo_clock.h` is the shared clock authority candidate
- `core/core/pogls_sdk.h` owns the shared POGLS context semantics
- `core/core/geo_pipeline_wire.h` carries the shared pipeline wire shape
- `core/pogls_engine/twin_core/geo_fibo_clock.h` contains the active fibo clock implementation used by the engine side
- `Hfolder/gpx4_container.h` and `Hfolder/geo_gpx_anim.h` implement the GPX4 static and animated codec path

## Intended sync model

```text
fibo clock
  -> bridge contract
    -> core/core primitives
      -> core/pogls_engine runtime
      -> Hfolder GPX / animated GPX adapter
```

## Next step when resuming

1. Keep `core/core/geo_fibo_clock.h` as the clock authority.
2. Define the bridge boundary for POGLS events.
3. Define the bridge boundary for GPX / animated GPX emission.
4. Route both through the same tick / flush cadence.

## Note

I paused before making any code changes to the runtime or GPX layers. The repo is still in the analysis / mapping stage for this sync work.

