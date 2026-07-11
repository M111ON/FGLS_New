/*
 * geofield_shutter_wire.h — Wire GoldbergShutter into Encode Path
 * ════════════════════════════════════════════════════════════════
 *
 * CONCEPT:
 *   GoldbergShutter (goldberg_shutter.h) = confidence ring system
 *   Ring states: 5 → 1 → 0  (confidence gradient)
 *     5 = fresh / unverified
 *     1 = soft-confirmed
 *     0 = locked / committed
 *
 *   Currently: dead layer — goldberg_shutter.h ไม่ถูกเรียกใน encode path เลย
 *   Goal: wire ring confidence เข้า geo_field_core.h encode pipeline
 *
 * WHERE TO WIRE (geo_field_core.h):
 *   encode step ปัจจุบัน:
 *     chunk → GpAddr → FrustumBlock → gp_blk_write() → stats
 *
 *   หลัง wire:
 *     chunk → GpAddr → FrustumBlock → gp_blk_write()
 *                                         ↓
 *                                    gs_ring_update()   ← NEW
 *                                         ↓
 *                                    confidence check   ← NEW
 *                                         ↓
 *                                    stats + reshape trigger
 *
 * RING BEHAVIOR per chunk:
 *   write ใหม่ → ring = 5 (fresh)
 *   verify pass (roundtrip OK) → ring → 1
 *   reshape fired → ring → 0 (locked, ไม่ rewrite)
 *
 *   ring = 0 → block ถือว่า committed
 *            → encode path ข้ามได้ถ้า already locked
 *            → นี่คือ "virtual" property: ถ้า ring=0 → reconstruct จาก seed
 *
 * INTEGRATION POINT:
 *   geo_field_encode_chunk() ใน geo_field_core.h บรรทัดหลัง gp_blk_write()
 *   เพิ่ม: geofield_shutter_update(blk, slot, ring_state)
 *
 * ════════════════════════════════════════════════════════════════
 */
#pragma once
#include <stdint.h>
#include "goldberg_shutter.h"          /* GsRing, gs_ring_layer(), etc.  */
#include "format/geofield_header.h"    /* GEOF_SHAPE_* constants         */

/* ── Ring state constants ───────────────────────────────────── */
#define GS_WIRE_FRESH       5u   /* just written — unverified            */
#define GS_WIRE_SOFT        1u   /* roundtrip verified                   */
#define GS_WIRE_LOCKED      0u   /* reshape committed → virtual/seed     */

/* ── Per-block shutter state ────────────────────────────────── */
/*
 * GsWireState — lightweight companion to FrustumBlock
 * One per active FrustumBlock in encode pipeline
 * Does NOT go into the file — encode-time only
 */
typedef struct {
    uint8_t  ring[54];      /* confidence per diamond slot (0..53)       */
    uint8_t  locked_count;  /* how many slots at ring=0                  */
    uint8_t  fresh_count;   /* how many slots at ring=5                  */
} GsWireState;

/* ── Init ────────────────────────────────────────────────────── */
static inline void gs_wire_init(GsWireState *ws) {
    for (int i = 0; i < 54; i++) ws->ring[i] = GS_WIRE_FRESH;
    ws->locked_count = 0;
    ws->fresh_count  = 54;
}

/* ── Update after write ─────────────────────────────────────── */
/*
 * gs_wire_on_write — call after gp_blk_write()
 * slot = diamond_slot (0..53)
 * Sets ring[slot] = GS_WIRE_FRESH
 */
static inline void gs_wire_on_write(GsWireState *ws, uint8_t slot) {
    if (slot >= 54) return;
    if (ws->ring[slot] == GS_WIRE_LOCKED) return;  /* committed, skip  */
    ws->ring[slot] = GS_WIRE_FRESH;
    ws->fresh_count++;
}

/* ── Update after verify (roundtrip OK) ─────────────────────── */
/*
 * gs_wire_on_verify — call after successful decode roundtrip
 * Transitions: FRESH(5) → SOFT(1)
 */
static inline void gs_wire_on_verify(GsWireState *ws, uint8_t slot) {
    if (slot >= 54) return;
    if (ws->ring[slot] == GS_WIRE_FRESH) {
        ws->ring[slot] = GS_WIRE_SOFT;
        if (ws->fresh_count > 0) ws->fresh_count--;
    }
}

/* ── Lock after reshape ─────────────────────────────────────── */
/*
 * gs_wire_on_reshape — call after pogls_atomic_reshape()
 * Transitions: SOFT(1) → LOCKED(0) for all soft slots
 * LOCKED slots = reconstructible from seed → virtual
 */
static inline void gs_wire_on_reshape(GsWireState *ws) {
    for (int i = 0; i < 54; i++) {
        if (ws->ring[i] == GS_WIRE_SOFT) {
            ws->ring[i] = GS_WIRE_LOCKED;
            ws->locked_count++;
        }
    }
}

/* ── Query ───────────────────────────────────────────────────── */

/* Returns 1 if slot is committed (can reconstruct from seed) */
static inline int gs_wire_is_locked(const GsWireState *ws, uint8_t slot) {
    return (slot < 54 && ws->ring[slot] == GS_WIRE_LOCKED) ? 1 : 0;
}

/* Returns 1 if all 54 slots locked → entire block is virtual */
static inline int gs_wire_block_virtual(const GsWireState *ws) {
    return (ws->locked_count >= 54) ? 1 : 0;
}

/*
 * gs_wire_confidence — overall block confidence (0..100)
 * 0   = all fresh  (นอ reliable)
 * 50  = mix
 * 100 = all locked (fully virtual)
 *
 * TODO: wire into GeoField stats for monitoring
 */
static inline uint8_t gs_wire_confidence(const GsWireState *ws) {
    return (uint8_t)((ws->locked_count * 100u) / 54u);
}
