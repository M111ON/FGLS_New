/*
 * tgw_tri5_wire.h — tri[5] FLOW Drop-in Hook for tgw_dispatch_v2
 * ═══════════════════════════════════════════════════════════════
 *
 * Drop-in: #include AFTER tgw_cardioid_express.h, BEFORE tgw_dispatch_v2.h
 *
 * Adds tri[5] FLOW condition to both dispatch hook points without
 * touching tgw_dispatch_v2.h or any sacred number.
 *
 * Wire chain (O(1), integer-only, no malloc):
 *   addr → addr%720 → GEO_WALK[pos] → tri5_from_enc() → route decision
 *
 * HOOK A (GROUND early-exit, polarity=1):
 *   tri[5] cond = face_id (comp)
 *   if CHIRAL or CROSS → bypass ground_fn → re-route to Metatron dest
 *   if ORBITAL or HUB  → fall through to cardioid / ground_fn (unchanged)
 *
 * HOOK B (ROUTE push, polarity=0):
 *   if CHIRAL or CROSS → override hilbert_lane with tri5_trip() position
 *   else               → fall through to cardioid bucket (unchanged)
 *
 * Usage in tgw_dispatch_v2() — replace existing HOOK A block:
 *
 *   // HOOK A:
 *   Tri5Decision td = tgw_tri5_decide(addr);
 *   if (td.override) {
 *       _v2_push(d, addr, val, td.lane, flush_fn, fn_ctx);
 *       d->tri5_count++; d->route_count++; return;
 *   }
 *   // ... existing cardioid / ground_fn code unchanged ...
 *
 *   // HOOK B (before _v2_push):
 *   Tri5Decision td = tgw_tri5_decide(addr);
 *   uint16_t bucket = td.override ? td.lane : (cd.use_express ? cd.next_pos/6u : geo.hilbert_idx);
 *
 * Sacred: 720, 12, 6 — all frozen. Additive only.
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef TGW_TRI5_WIRE_H
#define TGW_TRI5_WIRE_H

#include <stdint.h>
#include "geo_temporal_lut.h"     /* GEO_WALK[], TRING_COMP, TRING_CPAIR  */
#include "geo_goldberg_lut.h"     /* GB_PEN_TO_PAIR, GB_PEN_POLE          */
#include "geo_metatron_route.h"   /* Tri5Wire, METATRON_CROSS, tri5_*     */

/* ══════════════════════════════════════════════════════════════
   TRI5 DECISION — result of one tgw_tri5_decide() call
   ══════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t       face_id;   /* TRING_COMP(geo_enc)  0..11             */
    uint8_t       pair_id;   /* GB bipolar pair       0..5             */
    uint8_t       pole;      /* 0=positive 1=negative                  */
    MetatronRoute route;     /* ORBITAL/CHIRAL/CROSS_R/HUB             */
    uint16_t      lane;      /* hilbert_lane override (valid if override=1) */
    uint8_t       override;  /* 1 = use lane, skip cardioid/ground     */
    uint8_t       _pad;
} Tri5Decision;

/* ══════════════════════════════════════════════════════════════
   ADDR → GEO_WALK ENC
   addr (raw uint64) → walk position → GEO_WALK enc
   This is the single extra LUT lookup needed before tri5.
   ══════════════════════════════════════════════════════════════ */
static inline uint32_t tri5_addr_to_enc(uint64_t addr)
{
    return GEO_WALK[addr % TEMPORAL_WALK_LEN];
}

/* ══════════════════════════════════════════════════════════════
   ROUTE SELECTOR
   Given Tri5Wire, decide MetatronRoute based on pole:
     pole=0 (positive) → CHIRAL  (diameter cross to pole B)
     pole=1 (negative) → CROSS_R (inter-ring to nearest ring partner)
   ORBITAL reserved for same-face accumulation (not dispatched here).
   HUB used when face_id is in cusp zone (comp 3,5,6,8 — mixed pairs).
   ══════════════════════════════════════════════════════════════ */
static inline MetatronRoute tri5_select_route(const Tri5Wire *w)
{
    /* comp 4 and 7: TRING_CPAIR lands in same GB pair → use CROSS instead */
    if (w->face_id == 4u || w->face_id == 7u)
        return METATRON_CROSS_R;

    /* positive pole → CHIRAL (pole A → pole B, diameter line) */
    if (w->pole == 0u)
        return METATRON_CHIRAL;

    /* negative pole → CROSS_R (inter-ring, non-chiral bypass) */
    return METATRON_CROSS_R;
}

/* ══════════════════════════════════════════════════════════════
   MAIN ENTRY — tgw_tri5_decide()
   Call once per packet at each hook point.
   addr: raw dispatch addr (uint64_t), same value passed to tgw_dispatch_v2.
   polarity: geo.polarity from geo_net_encode(addr) — already computed.

   Returns Tri5Decision:
     .override=1 → use .lane as hilbert_lane (bypass cardioid/ground)
     .override=0 → fall through to existing cardioid path (no change)
   ══════════════════════════════════════════════════════════════ */
static inline Tri5Decision tgw_tri5_decide(uint64_t addr, uint8_t polarity)
{
    Tri5Decision td;
    td._pad = 0u;

    /* step 1: addr → GEO_WALK enc (1 LUT lookup) */
    uint32_t enc = tri5_addr_to_enc(addr);

    /* step 2: enc → Tri5Wire (4 integer ops) */
    Tri5Wire w   = tri5_from_enc(enc);

    /* step 3: select route from geometry */
    MetatronRoute route = tri5_select_route(&w);
    w.route = route;

    td.face_id  = w.face_id;
    td.pair_id  = w.pair_id;
    td.pole     = w.pole;
    td.route    = route;

    /* step 4: compute destination walk position */
    uint16_t dest_pos = tri5_trip(enc, route);

    /* step 5: override decision
     * HOOK A (GROUND, polarity=1):
     *   CHIRAL  → positive pole hitting GROUND → re-route to ROUTE lane
     *   CROSS_R → inter-ring bypass → push to route, skip ground_fn
     * HOOK B (ROUTE, polarity=0):
     *   CHIRAL / CROSS_R → geometry overrides hilbert bucket
     * ORBITAL → no override (same face, let cardioid handle)
     * dest_pos must be valid (< 720)                              */
    if (dest_pos < TEMPORAL_WALK_LEN &&
        (route == METATRON_CHIRAL || route == METATRON_CROSS_R)) {
        td.lane     = dest_pos / 6u;   /* hilbert_lane = pos/6, range 0..119 */
        td.override = 1u;
    } else {
        td.lane     = 0u;
        td.override = 0u;
    }

    (void)polarity;   /* available for future polarity-conditional logic */
    return td;
}

/* ══════════════════════════════════════════════════════════════
   STATS COUNTER — add to TGWDispatchV2 struct
   (field name: tri5_count — increment when override fires)

   Paste into TGWDispatchV2 struct in tgw_dispatch_v2.h:
     uint32_t tri5_count;       // tri5 override fires (CHIRAL+CROSS)

   Paste into tgw_dispatch_v2_init():
     d->tri5_count = 0u;
   ══════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════
   HOOK MACROS — paste into tgw_dispatch_v2() at each hook point

   HOOK A — replace existing HOOK A comment block:

   TRI5_HOOK_A(d, addr, val, geo, flush_fn, fn_ctx, ground_fn)

   HOOK B — insert before _v2_push():

   TRI5_HOOK_B(bucket, addr, geo, cd)
   ══════════════════════════════════════════════════════════════ */

/*
 * TRI5_HOOK_A — GROUND early-exit tri5 intercept
 * If override fires: push to route lane, skip ground_fn, return.
 * Falls through if override=0 (cardioid + ground_fn path unchanged).
 */
#define TRI5_HOOK_A(d, addr, val, geo, flush_fn, fn_ctx)               \
    do {                                                                 \
        Tri5Decision _td = tgw_tri5_decide((addr), (geo).polarity);     \
        if (_td.override) {                                              \
            _v2_push((d), (addr), (uint32_t)(val), _td.lane,            \
                     (flush_fn), (fn_ctx));                              \
            (d)->bucket_hist[_td.lane]++;                               \
            (d)->tri5_count++;                                          \
            (d)->route_count++;                                         \
            return;                                                      \
        }                                                                \
    } while(0)

/*
 * TRI5_HOOK_B — ROUTE bucket override
 * Replaces cardioid/hilbert bucket with tri5 geometry lane when override fires.
 * bucket_var must be declared uint16_t before this macro.
 */
#define TRI5_HOOK_B(bucket_var, addr, geo, cd)                          \
    do {                                                                 \
        Tri5Decision _td = tgw_tri5_decide((addr), (geo).polarity);     \
        if (_td.override) {                                              \
            (bucket_var) = _td.lane;                                    \
        } else if ((cd).use_express) {                                  \
            (bucket_var) = (cd).next_pos / 6u;                          \
        } else {                                                         \
            (bucket_var) = (geo).hilbert_idx;                           \
        }                                                                \
    } while(0)

#endif /* TGW_TRI5_WIRE_H */
