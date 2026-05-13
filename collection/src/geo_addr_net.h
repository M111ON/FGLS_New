/*
 * geo_addr_net.h — SEAM 2: TRing address → GeoNetAddr (O(1) LUT)
 * ═══════════════════════════════════════════════════════════════
 * Maps enc (uint64_t) → GeoNetAddr { polarity, spoke, hilbert_idx }
 *
 * Polarity: pos = enc % 720 → pentagon = pos % 60 → polarity = (pentagon % 60) >= 30
 *   0 = ROUTE, 1 = GROUND (50/50 split over full 720 cycle)
 *
 * hilbert_idx: spoke-local position (0..119) for Hilbert lane sorting
 *   = pos / 6  (maps 720 positions → 120 Hilbert buckets)
 *
 * Sacred constants (frozen):
 *   GAN_TRING_CYCLE = 720   GAN_PENTAGON_SZ = 60   GAN_SPOKES = 6
 *
 * geo_addr_net_init() must be called before geo_net_encode().
 * No malloc. No heap.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_ADDR_NET_H
#define GEO_ADDR_NET_H

#include <stdint.h>
#include <string.h>

#define GAN_TRING_CYCLE   720u
#define GAN_PENTAGON_SZ    60u
#define GAN_SPOKES          6u
#define GAN_HILBERT_BUCKETS 120u   /* 720 / 6 */

typedef struct {
    uint8_t  polarity;     /* 0=ROUTE, 1=GROUND */
    uint8_t  spoke;        /* 0..5 */
    uint16_t hilbert_idx;  /* 0..119 — Hilbert lane for batch sort */
} GeoNetAddr;

/* ── LUT: 720 entries, built once by geo_addr_net_init() ─────── */
static GeoNetAddr _gan_lut[GAN_TRING_CYCLE];
static int        _gan_ready = 0;

static inline void geo_addr_net_init(void)
{
    if (_gan_ready) return;
    for (uint32_t pos = 0u; pos < GAN_TRING_CYCLE; pos++) {
        uint8_t  polarity = (uint8_t)((pos % 120u) >= 60u);
        uint8_t  spoke    = (uint8_t)((pos / 120u) % GAN_SPOKES);
        uint16_t hilbert  = (uint16_t)(pos / GAN_SPOKES);  /* 0..119 */
        _gan_lut[pos].polarity    = polarity;
        _gan_lut[pos].spoke       = spoke;
        _gan_lut[pos].hilbert_idx = hilbert;
    }
    _gan_ready = 1;
}

static inline GeoNetAddr geo_net_encode(uint64_t enc)
{
    return _gan_lut[enc % GAN_TRING_CYCLE];
}

#endif /* GEO_ADDR_NET_H */
