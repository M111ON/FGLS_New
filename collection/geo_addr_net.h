/*
 * geo_addr_net.h — GeoNetAddr: Hilbert-spoke address routing layer
 * ═════════════════════════════════════════════════════════════════
 * SEAM 2 (P5-B): replaces enc%720 direct path in tgw_lc_bridge.h
 * with a proper geo_radial_hilbert-style spoke lane mapping.
 *
 * Problem SEAM 2 solves:
 *   Old path: enc%720 → spoke (integer modulo, no spatial coherence)
 *   New path: enc → GeoNetAddr → rh_map() → Hilbert lane (spoke-ordered,
 *             space-filling, cache-friendly traversal order)
 *
 * Architecture (3-layer address encoding — locked spec):
 *   addr_core   uint64    — raw physical address (DiamondBlock space)
 *   bridge      hex[16]   — 64-bit transit key (gateway identity)
 *   interface   base62[11]— human/LLM-facing compact address
 *
 * GeoNetAddr pipeline:
 *   uint64 enc
 *       │
 *       ▼  geo_net_encode()
 *   GeoNetAddr { addr_core, spoke, lane, tring_pos, hilbert_idx }
 *       │
 *       ▼  geo_net_to_lc_route()
 *   TGWLCRoute { spoke, polarity, tring_pos }   ← drop-in for old route
 *       │
 *       ▼  fed to tgw_dispatch() / tgw_lc_bridge.h (unchanged callers)
 *
 * Hilbert lane mapping (radial 2D Hilbert curve on spoke×position grid):
 *   Grid: 6 spokes × 120 positions = 720 cells
 *   Hilbert order → spatial locality: adjacent cells map to adjacent lanes
 *   → better prefetch pattern vs enc%720 (random scatter)
 *
 * Sacred constants (frozen — match POGLS spec):
 *   TRING_CYCLE = 720    GEO_SPOKES = 6    PENT_SPAN = 120
 *   GEO_FULL_N  = 3456   GEO_SLOTS  = 576  ANG_STEPS = 512
 *
 * No malloc. No float. No heap. Integer-only arithmetic.
 * ═════════════════════════════════════════════════════════════════
 */

#ifndef GEO_ADDR_NET_H
#define GEO_ADDR_NET_H

#include <stdint.h>
#include <string.h>

/* ── Sacred constants ──────────────────────────────────────── */
#define GAN_TRING_CYCLE   720u   /* 6 spokes × 120 positions     */
#define GAN_SPOKES          6u   /* geometry invariant            */
#define GAN_PENT_SPAN     120u   /* positions per spoke           */
#define GAN_MIRROR_HALF    60u   /* polarity boundary             */
#define GAN_ANG_STEPS     512u   /* 0-511 → 0-360°                */
#define GAN_GEO_FULL_N   3456u   /* L0 total nodes (54×64)        */
#define GAN_GEO_SLOTS     576u   /* geo slot space                */
#define GAN_LANE_COUNT    720u   /* Hilbert lane total            */

/* ── Hilbert curve dimension: 6×120 grid ──────────────────── */
/* We fold into a 32×24 grid (768 cells, use 720).             */
/* Hilbert order on 32×32 = 1024 cells, clip to 720.          */
#define GAN_H_BITS          5u   /* 2^5 = 32 side length          */
#define GAN_H_SIDE         32u
#define GAN_H_TOTAL      1024u   /* 32×32 Hilbert space           */

/* ── GeoNetAddr ────────────────────────────────────────────── */
typedef struct {
    uint64_t addr_core;    /* raw uint64 source enc          [0-7]  */
    uint16_t tring_pos;    /* 0-719: TRing cycle position    [8-9]  */
    uint16_t hilbert_idx;  /* 0-719: Hilbert-ordered lane    [10-11]*/
    uint16_t ang_step;     /* 0-511: angular position        [12-13]*/
    uint8_t  spoke;        /* 0-5: which of 6 spokes         [14]   */
    uint8_t  polarity;     /* 0=ROUTE, 1=GROUND              [15]   */
    /* face + pad folded into polarity byte's neighbour — use spoke%4 inline */
} GeoNetAddr;              /* 16 bytes exactly (8+2+2+2+1+1) ✅    */

/* ── TGWLCRoute (drop-in for tgw_lc_bridge.h callers) ─────── */
typedef struct {
    uint8_t  spoke;
    uint8_t  polarity;
    uint16_t tring_pos;
} GanLCRoute;

/* ═══════════════════════════════════════════════════════════
   HILBERT CURVE — xy→d and d→xy (integer, 2^n grid)
   Classic Hilbert curve bit-interleave algorithm.
   No float, no division, no malloc.
   ═══════════════════════════════════════════════════════════ */

/* Rotate/flip quadrant for Hilbert encoding */
static inline void _gan_hilbert_rot(uint32_t n, uint32_t *x, uint32_t *y,
                                     uint32_t rx, uint32_t ry) {
    if (ry == 0u) {
        if (rx == 1u) { *x = n - 1u - *x; *y = n - 1u - *y; }
        uint32_t t = *x; *x = *y; *y = t;
    }
}

/* (x,y) → Hilbert distance d in [0, n²) where n = 2^p */
static inline uint32_t gan_xy_to_hilbert(uint32_t n, uint32_t x, uint32_t y) {
    uint32_t d = 0u;
    for (uint32_t s = n >> 1u; s > 0u; s >>= 1u) {
        uint32_t rx = (x & s) ? 1u : 0u;
        uint32_t ry = (y & s) ? 1u : 0u;
        d += s * s * ((3u * rx) ^ ry);
        _gan_hilbert_rot(s, &x, &y, rx, ry);
    }
    return d;
}

/* Hilbert distance d → (x,y) in [0,n)×[0,n) */
static inline void gan_hilbert_to_xy(uint32_t n, uint32_t d,
                                      uint32_t *x, uint32_t *y) {
    *x = 0u; *y = 0u;
    for (uint32_t s = 1u; s < n; s <<= 1u) {
        uint32_t rx = (d >> 1u) & 1u;
        uint32_t ry = (d ^ rx) & 1u;
        _gan_hilbert_rot(s, x, y, rx, ry);
        *x += s * rx;
        *y += s * ry;
        d  >>= 2u;
    }
}

/* ═══════════════════════════════════════════════════════════
   SPOKE × POSITION → HILBERT LANE
   Maps (spoke[0-5], pos_in_spoke[0-119]) onto 32×32 Hilbert
   grid, returns lane index 0-719.

   Grid layout:
     x-axis = spoke (0-5) → map to [0,5] within [0,31]
     y-axis = pos_in_spoke (0-119) → map to [0,23] within [0,31]
             (120 positions / 5 = 24 bands of 5 pos each)

   Hilbert d clipped to [0,719] by LUT (built once at init).
   ═══════════════════════════════════════════════════════════ */

/* Pre-built LUT: (spoke, band) → hilbert_d → lane [0-719]    */
/* LUT size: 720 entries × 2 bytes = 1440 bytes (fits L1)     */
static uint16_t _gan_lut[GAN_TRING_CYCLE];   /* [tring_pos] → hilbert_lane */
static uint8_t  _gan_lut_ready = 0u;

/*
 * Build the spoke×position → Hilbert lane LUT.
 * Must be called once before geo_net_encode().
 * Thread-safe if called from single-thread init.
 */
static inline void geo_addr_net_init(void) {
    if (_gan_lut_ready) return;

    /*
     * Strategy:
     *   tring_pos 0-719 → spoke = pos/120, pos_in_spoke = pos%120
     *   Map onto 32×32 Hilbert grid:
     *     x = spoke * 5              (0,5,10,15,20,25 — 6 columns)
     *     y = pos_in_spoke * 24/120  (0-23 — 24 rows of 5 each)
     *   Compute hilbert d (0-1023), then rank-compress to [0-719]
     *
     * Rank compression: sort all 720 (tring_pos, d) by d,
     * assign lane 0-719 in Hilbert order.
     * Result: adjacent tring positions → adjacent lanes (locality).
     */

    /* Step 1: compute raw Hilbert d for each tring_pos */
    static uint32_t raw_d[GAN_TRING_CYCLE];
    for (uint32_t tp = 0u; tp < GAN_TRING_CYCLE; tp++) {
        uint32_t spoke        = tp / GAN_PENT_SPAN;        /* 0-5  */
        uint32_t pos_in_spoke = tp % GAN_PENT_SPAN;        /* 0-119 */
        /* map to 32×32 grid */
        uint32_t x = spoke * 5u;                           /* 0,5..25 */
        uint32_t y = pos_in_spoke * 24u / GAN_PENT_SPAN;  /* 0-23   */
        raw_d[tp] = gan_xy_to_hilbert(GAN_H_SIDE, x, y);
    }

    /* Step 2: rank-sort raw_d → assign lane 0-719 (insertion sort ok @720) */
    /* pair: (d, original_tp) */
    static uint32_t order[GAN_TRING_CYCLE]; /* sorted indices */
    for (uint32_t i = 0u; i < GAN_TRING_CYCLE; i++) order[i] = i;

    /* simple selection sort (720 items, runs once at init, no alloc) */
    for (uint32_t i = 0u; i < GAN_TRING_CYCLE - 1u; i++) {
        uint32_t min_j = i;
        for (uint32_t j = i + 1u; j < GAN_TRING_CYCLE; j++) {
            if (raw_d[order[j]] < raw_d[order[min_j]])
                min_j = j;
        }
        if (min_j != i) {
            uint32_t tmp = order[i];
            order[i]     = order[min_j];
            order[min_j] = tmp;
        }
    }

    /* Step 3: write LUT: _gan_lut[tring_pos] = lane (rank in Hilbert order) */
    for (uint32_t lane = 0u; lane < GAN_TRING_CYCLE; lane++)
        _gan_lut[order[lane]] = (uint16_t)lane;

    _gan_lut_ready = 1u;
}

/* ═══════════════════════════════════════════════════════════
   CORE: enc → GeoNetAddr
   ═══════════════════════════════════════════════════════════ */

/*
 * geo_net_encode() — main entry point (SEAM 2 replacement for enc%720)
 *
 * Input:  uint64_t enc  (raw address / TStreamPkt.enc)
 * Output: GeoNetAddr    (fully decoded, Hilbert lane filled)
 *
 * Old path: tring_pos = enc % 720, spoke = pos/120
 * New path: tring_pos = enc % 720, spoke = pos/120,
 *           hilbert_idx = _gan_lut[tring_pos]  (spatial lane)
 *
 * The enc%720 step is kept (backward compatible with sacred constants).
 * SEAM 2 adds the Hilbert layer on top — callers switch to hilbert_idx
 * for lane routing instead of raw tring_pos.
 */
static inline GeoNetAddr geo_net_encode(uint64_t enc) {
    GeoNetAddr a;
    a.addr_core  = enc;
    a.tring_pos  = (uint16_t)(enc % GAN_TRING_CYCLE);
    a.spoke      = (uint8_t)(a.tring_pos / GAN_PENT_SPAN);
    a.polarity   = (uint8_t)((a.tring_pos % GAN_PENT_SPAN) >= GAN_MIRROR_HALF);
    /* angular step: map tring_pos (0-719) → ang (0-511) */
    a.ang_step    = (uint16_t)(a.tring_pos * GAN_ANG_STEPS / GAN_TRING_CYCLE);
    /* Hilbert lane: LUT lookup (O(1) after init) */
    a.hilbert_idx = _gan_lut_ready
                    ? _gan_lut[a.tring_pos]
                    : a.tring_pos;   /* fallback: identity if not init */
    return a;
}

/* ── Drop-in for tgwlc_route() (SEAM 2 version) ─────────── */
static inline GanLCRoute geo_net_to_lc_route(uint64_t enc) {
    GeoNetAddr a = geo_net_encode(enc);
    GanLCRoute r;
    r.spoke     = a.spoke;
    r.polarity  = a.polarity;
    r.tring_pos = a.tring_pos;
    return r;
}

/* ── Hilbert-ordered lane for prefetch / dispatch routing ── */
static inline uint16_t geo_net_hilbert_lane(uint64_t enc) {
    GeoNetAddr a = geo_net_encode(enc);
    return a.hilbert_idx;
}

/* ═══════════════════════════════════════════════════════════
   BRIDGE HEX[16] — 3-layer address encoding
   addr_core → bridge hex string (16 hex chars, zero-padded)
   ═══════════════════════════════════════════════════════════ */

static const char _gan_hex_chars[16] = {
    '0','1','2','3','4','5','6','7',
    '8','9','a','b','c','d','e','f'
};

/* Write 16-char hex bridge key into buf[16] (no null term) */
static inline void geo_net_bridge_hex(uint64_t addr_core, char buf[16]) {
    for (int i = 15; i >= 0; i--) {
        buf[i] = _gan_hex_chars[addr_core & 0xFu];
        addr_core >>= 4u;
    }
}

/* ═══════════════════════════════════════════════════════════
   INTERFACE BASE62[11] — compact address for LLM/human layer
   62^11 ≈ 5.2×10^19 > 2^64 → full uint64 coverage
   Alphabet: 0-9 A-Z a-z (62 chars)
   ═══════════════════════════════════════════════════════════ */

static const char _gan_b62[62] = {
    '0','1','2','3','4','5','6','7','8','9',
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z'
};

/* Write 11-char base62 interface address into buf[11] (no null term) */
static inline void geo_net_interface_b62(uint64_t addr_core, char buf[11]) {
    for (int i = 10; i >= 0; i--) {
        buf[i] = _gan_b62[addr_core % 62u];
        addr_core /= 62u;
    }
}

/* ── Full 3-layer address dump to caller-owned buffers ───── */
typedef struct {
    uint64_t addr_core;
    char     bridge[17];      /* hex[16] + null */
    char     interface_b62[12]; /* base62[11] + null */
} GanAddress;

static inline GanAddress geo_net_address(uint64_t enc) {
    GanAddress out;
    out.addr_core = enc;
    geo_net_bridge_hex(enc, out.bridge);
    out.bridge[16] = '\0';
    geo_net_interface_b62(enc, out.interface_b62);
    out.interface_b62[11] = '\0';
    return out;
}

/* ═══════════════════════════════════════════════════════════
   BATCH ENCODE — N enc values → GeoNetAddr array
   caller-owned out[n], n ≤ 720
   ═══════════════════════════════════════════════════════════ */
static inline void geo_net_encode_batch(const uint64_t *encs,
                                         GeoNetAddr     *out,
                                         uint32_t        n) {
    for (uint32_t i = 0u; i < n; i++)
        out[i] = geo_net_encode(encs[i]);
}

/* ═══════════════════════════════════════════════════════════
   HILBERT COHERENCE VERIFY
   Check that Hilbert lane ordering has locality property:
   adjacent tring_pos differ by lane ≤ threshold.
   Returns max_delta across all adjacent pairs.
   ═══════════════════════════════════════════════════════════ */
static inline uint32_t geo_net_hilbert_max_delta(void) {
    uint32_t max_d = 0u;
    for (uint32_t tp = 0u; tp + 1u < GAN_TRING_CYCLE; tp++) {
        uint32_t a = _gan_lut[tp];
        uint32_t b = _gan_lut[tp + 1u];
        uint32_t d = (a > b) ? (a - b) : (b - a);
        if (d > max_d) max_d = d;
    }
    return max_d;
}

/* ═══════════════════════════════════════════════════════════
   SPOKE COVERAGE CHECK
   Given N GeoNetAddr, verify each spoke gets n/6 ±1 entries.
   Returns 1 if uniform, 0 if skewed.
   ═══════════════════════════════════════════════════════════ */
static inline int geo_net_spoke_uniform(const GeoNetAddr *addrs, uint32_t n,
                                         uint32_t hist[GAN_SPOKES]) {
    for (uint8_t s = 0u; s < GAN_SPOKES; s++) hist[s] = 0u;
    for (uint32_t i = 0u; i < n; i++) hist[addrs[i].spoke]++;
    uint32_t expected = n / GAN_SPOKES;
    for (uint8_t s = 0u; s < GAN_SPOKES; s++) {
        uint32_t diff = (hist[s] > expected)
                        ? hist[s] - expected
                        : expected - hist[s];
        if (diff > 1u) return 0;
    }
    return 1;
}

#endif /* GEO_ADDR_NET_H */
