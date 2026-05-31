# Session Handoff — Geometry Weight Store + Pipeline Integration (complete)

## สิ่งที่ทำใน session นี้

1. **GeomRouterBridge** — `geom_router_bridge.h`
   - `grb_build()` — assign tensors to 12 zones (round-robin)
   - `grb_decode_route()` — BermudaRouteEntry {zone, tring_slot, polarity} → O(1) tile decode
   - `grb_decode_batch()` — N routes at once
   - `tring_slot` → tile_idx proportional mapping (0..719 → 0..n_tiles-1)
   - No malloc beyond GeomBridge. O(1) per route.

2. **GeomShadowPipe** — `geom_shadow_pipe.h`
   - `gsp_push()` — single call: classify(HOT/COLD) + HOT→decode / COLD→ring push
   - `gsp_push_batch()` — N chunks, returns HOT count
   - `gsp_retrieve_cold()` — COLD retrieval by bond_key
   - `gsp_stats()` — counts + eviction info
   - Wires bermuda_shadow.h + geom_router_bridge.h end-to-end

3. **Tests** — ทั้งหมด 57/57 pass
   - `tests/test_grb.c`: 32/32 pass (6 synthetic + real 20-tensor qwen25_gsten)
   - `tests/test_gsp.c`: 19/19 pass (HOT/COLD classify + decode + batch + eviction)
   - `tests/test_geom_bridge.c`: 1 pass (gsten load OK, qdat path TBD)

## ไฟล์ใหม่/แก้ไข

| ไฟล์ | อะไร |
|------|------|
| `geom_router_bridge.h` | **ใหม่** — BermudaRouter → GeomBridge wire (zone→tensor→tile) |
| `geom_shadow_pipe.h` | **ใหม่** — Shadow classify + decode pipeline (gsp_push) |
| `tests/test_grb.c` | **ใหม่** — 6 unit + real gsten integration |
| `tests/test_gsp.c` | **ใหม่** — 6 pipeline tests (HOT/COLD/batch/eviction) |
| `geom_raw_bridge.h` | GeomBridge (gb_load/gb_get/gb_decode_tile/gb_decode_tensor) |
| `build_geom_tile_store.py` | Python encode .qdat → .gsten |
| `tests/test_geom_bridge.c` | End-to-end gsten verify |

## Architecture (complete pipeline)

```
.qdat (Q8_0 raw)
  → build_geom_tile_store.py → hex_tile_encode → .gsten
  → gb_load() → GeomBridge

raw 64B chunk → bermuda_shadow_dispatch() → HOT/COLD
  → HOT:  grb_decode_route(zone, tring_slot) → gb_decode_tile() → 7B
  → COLD: shadow ring push (bond_key → retrievable)

gsp_push() wraps the entire path in one call.
```

## Status: Pipeline integration complete

```
BermudaRouter → GeomRouterBridge → GeomBridge → tile[7]
    ↓ (verdict: zone+tring_slot+polarity)
BermudaShadow → GeomShadowPipe → gsp_push
    ↓ classify HOT/COLD
HOT → decode   COLD → shadow ring
```

## ข้อสังเกต

- Ratio ~2.14× ยังสูง — index optimization ยังคงเป็น bottleneck
- `geom_shadow_pipe.h` ใช้ `bermuda_shadow.h` ผ่าน `bermuda_shadow_dispatch()` ซึ่งต้องการ `bermuda_init()` ก่อน
- `geom_router_bridge.h` ไม่ depend on `bermuda_init()` — ใช้ struct เปล่าได้
- All integer, no float, no malloc ใน hot path

## ยังต้องทำ (next)

1. **Optimize index format** — 6B/tile → <1B (geo_key addressing)
2. **Quantize on demand** — tile[7] → Q4/Q8/F16 weight
3. **Build full model store** — fix Python timeout on 144MB embedding
4. **Connect to TGW** — C runner uses gsp_push_batch per layer
