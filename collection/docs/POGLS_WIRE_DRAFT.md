# POGLS Layer Stack — Draft Wire Map
# ====================================
# Status: DRAFT — สิ่งที่มีจริง vs สิ่งที่ต้องสร้าง
# Last updated: tgw_dispatch.h wired (D1-D5 pass)

## ══════════════════════════════════════════════════
## LAYER STACK
## ══════════════════════════════════════════════════

LAYER 1: World A/B + N-shell
─────────────────────────────────────────────────────
  EXISTS:   pogls_5world_core.h  → World A(2)/B(3) anchor/flow
            geo_diamond_field.h  → shell expansion
  API OUT:  shell_id, world_id   → passed down to Layer 2
  GAP:      ❌ no API that exposes "which shell am I in" to dispatch
  NOTE:     tgw_dispatch currently ignores shell/world entirely

LAYER 2: Twin Polyhedron (scheduler/dispatcher)
─────────────────────────────────────────────────────
  EXISTS:   twin_bridge_init/write/batch/flush  (TwinBridge)
            tgw_dispatch.h  → verdict → ROUTE/GROUND  ✅ WIRED
            lch_gate        → polarity check           ✅ WIRED
  API OUT:  TGWResult, TGWDispatch
  GAP:      ❌ GPU batch path (twin_bridge_batch) not in dispatch loop
            ❌ dispatch doesn't know CPU vs GPU split decision
  NOTE:     currently all dispatch is CPU-side only

LAYER 3: Geo_ Family (slice / label / stream)
─────────────────────────────────────────────────────
  EXISTS:   geo_pipeline_wire.h  → geo_pipeline_step/fill
            pogls_pipeline_wire.h → pipeline_wire_process/batch
            tgw_stream_file()   → slices file → tgw_write
  API OUT:  GeoPacketWire, PipelineResult, BatchVerdict
  GAP:      ❌ tgw_stream_file() calls tgw_write() directly
              does NOT go through tgw_dispatch
              → stream data bypasses ROUTE/GROUND split entirely
            ❌ geo_pixel_pentagon (GeoPixel v21) not wired into stream
  NOTE:     this is the biggest unwired seam right now

LAYER 4: Dimension Folder (Hilbert / LetterCube)
─────────────────────────────────────────────────────
  EXISTS:   geo_radial_hilbert.h → rh_map(GeoNetAddr) → RHLine
                                   rh_step → RHStep
            geo_letter_cube.h   → lc_force_match, lc_closure_verify_78
            geo_goldberg_tring_bridge.h → gbt_capture → GBBlueprint
  API OUT:  RHLine { spoke, group, layer }
            GBBlueprint { tring_start/end, stamp_hash, spatial_xor... }
  GAP:      ❌ rh_map takes GeoNetAddr — but dispatch receives raw uint64_t
              needs: addr → GeoNetAddr → rh_map → RHLine → spoke/lane
            ❌ LetterCube fold result not used in dispatch routing
  NOTE:     blueprint_to_geopkt uses tring_end%GEO_SLOTS directly
            (Hilbert position derived manually, not via rh_map)

LAYER 5: SpaceFrame (TRing + Goldberg)
─────────────────────────────────────────────────────
  EXISTS:   geo_temporal_ring.h  → TRingCtx, tring_init, tring_walk
            geo_goldberg_scan.h  → GBScanCtx, gb_scan, gb_face_write
            geo_goldberg_tracker.h → blueprint accumulation
            pogls_goldberg_pipeline.h → goldberg_pipeline_write ✅
  API OUT:  GBBlueprint (sacred constants baked in: 144, 3456, 6, 54)
  GAP:      ❌ no explicit "is this address valid in current TRing pos?"
              check exposed upward to Layer 4/dispatch
  NOTE:     sacred numbers flow correctly through blueprint
            3456/144/6 all verified in Phase 13 benchmarks

## ══════════════════════════════════════════════════
## TWIN BRIDGE — current role
## ══════════════════════════════════════════════════

  Twin Bridge IS wired as bus:
    Layer 5 → goldberg_pipeline_write → blueprint
    Layer 4 → gbt_capture → GBBlueprint fields
    Layer 3 → tgw_stream_file → tgw_write → goldberg+twin
    Layer 2 → tgw_dispatch → verdict → FtsTwin / PayloadStore ✅
    Layer 1 → NOT connected yet

  Missing bus connections:
    L3 stream → dispatch          ← HIGHEST PRIORITY GAP
    L4 rh_map → dispatch routing  ← MEDIUM (affects spoke/lane)
    L1 shell/world → dispatch     ← LOW (future N-shell expansion)

## ══════════════════════════════════════════════════
## KNOWN SEAMS — things built after, won't fit perfectly
## ══════════════════════════════════════════════════

SEAM 1: tgw_stream_file → dispatch
  Problem:  stream addr = pkts[i].enc (uint32_t TRing encoded)
            dispatch expects addr/value with bit63 = polarity
            → enc doesn't have polarity bit set
  Fix needed: encode polarity into enc before dispatch, OR
              add stream-specific dispatch path that skips lch_gate
              and routes based on TRing position instead

SEAM 2: rh_map(GeoNetAddr) → dispatch
  Problem:  dispatch takes raw uint64_t addr
            rh_map needs GeoNetAddr { group, spoke, layer... }
            → need addr → GeoNetAddr converter
  Fix needed: geo_addr_to_net(uint64_t) → GeoNetAddr wrapper

SEAM 3: GeoPixel v21 (geopixel_v21_o25.c)
  Problem:  completely separate codec, not in Layer 3 stream yet
            geo_pixel_pentagon maps to vector/img space
            → needs hook in Layer 3 (geo_pipeline_step or separate lane)
  Fix needed: define GeoPixel ↔ GeoPacketWire interface

SEAM 4: GPU batch (Phase 14)
  Problem:  twin_bridge_batch exists but not in tgw_dispatch
            persistent device buffer not yet allocated
            multi-stream async not implemented
  Fix needed: dispatch decides CPU vs GPU based on batch size threshold

## ══════════════════════════════════════════════════
## PRIORITY ORDER (draft)
## ══════════════════════════════════════════════════

P1 — Wire tgw_stream_file → tgw_dispatch          (SEAM 1)
     tgw_stream_file_dispatch() wrapper
     stream packets get ROUTE/GROUND treatment

P2 — addr → GeoNetAddr → rh_map spoke routing     (SEAM 2)
     so dispatch uses real Hilbert lane not raw addr%6

P3 — GeoPixel v21 Layer 3 hook                    (SEAM 3)
     geo_pixel_pentagon → GeoPacketWire lane

P4 — GPU batch path in dispatch                   (SEAM 4)
     Phase 14 persistent buffer + multi-stream

P5 — Layer 1 shell/world awareness                (future)
     when N-shell expansion needed

## ══════════════════════════════════════════════════
## FILES TO CREATE (per priority)
## ══════════════════════════════════════════════════

P1: tgw_stream_dispatch.h   — stream-aware dispatch wrapper
P2: geo_addr_net.h          — uint64_t → GeoNetAddr bridge
P3: geo_pixel_wire.h        — GeoPixel ↔ pipeline interface
P4: tgw_gpu_dispatch.h      — GPU batch dispatch (Phase 14)
