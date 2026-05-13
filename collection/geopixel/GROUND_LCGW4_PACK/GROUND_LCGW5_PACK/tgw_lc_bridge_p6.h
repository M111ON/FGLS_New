/*
 * tgw_lc_bridge_p6.h — P6: TGWPendingSlot + Hilbert sort callbacks
 * ═══════════════════════════════════════════════════════════════════
 * P6 adds to P4 bridge:
 *   - TGWPendingSlot: addr+val+hilbert_lane (batch sort key)
 *   - TGWBatchFlushFn: callback for sorted batch flush
 *   - TGWGroundFn: callback for GROUND path
 *   - tgwlc_hilbert_lane(): enc → hilbert bucket (O(1))
 *
 * Designed to be included BEFORE tgw_dispatch_v2.h
 * Depends on: geo_addr_net.h (for hilbert_idx)
 *
 * Sacred constants (frozen):
 *   TRING_CYCLE=720  PENTAGON_SZ=60  SPOKES=6
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef TGW_LC_BRIDGE_P6_H
#define TGW_LC_BRIDGE_P6_H

#include <stdint.h>
#include "geo_addr_net.h"

/* ── Pending slot: one ROUTE entry waiting for Hilbert sort ─── */
typedef struct {
    uint64_t addr;
    uint64_t val;
    uint16_t hilbert_lane;
    uint8_t  _pad[6];      /* align to 24 bytes */
} TGWPendingSlot;

/* ── Callbacks ───────────────────────────────────────────────── */

/* Called with sorted (addrs[], vals[], n) batch */
typedef void (*TGWBatchFlushFn)(const uint64_t *addrs,
                                 const uint64_t *vals,
                                 uint32_t        n,
                                 void           *ctx);

/* Called for each GROUND packet (polarity=1) */
typedef void (*TGWGroundFn)(uint64_t addr, uint64_t val, void *ctx);

/* ── hilbert_lane: enc → Hilbert bucket (O(1) via LUT) ──────── */
static inline uint16_t tgwlc_hilbert_lane(uint64_t enc)
{
    return geo_net_encode(enc).hilbert_idx;
}

/* ── prefilter: check polarity without full encode ───────────── */
static inline uint8_t tgwlc_prefilter_p6(uint64_t enc)
{
    return geo_net_encode(enc).polarity;
}

#endif /* TGW_LC_BRIDGE_P6_H */
