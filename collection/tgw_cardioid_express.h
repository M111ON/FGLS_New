/*
 * tgw_cardioid_express.h — Cardioid × TRing Express Lane
 * ════════════════════════════════════════════════════════
 *
 * Express lane เสียบเข้า tgw_dispatch_v2 ที่ 2 hook points:
 *
 *   HOOK A — GROUND early-exit  (line ~227 ใน dispatch)
 *     polarity=1 → cardioid_decide() ก่อน ground_fn
 *     ถ้า express pass → route ไป next_pos แทน ground
 *
 *   HOOK B — _v2_push  (line ~187 ใน dispatch)
 *     ROUTE path → ถ้า cardioid pass → override hilbert_idx
 *     ด้วย express next_pos (นั่นคือ counting sort bucket ใหม่)
 *
 * Behavior สรุป:
 *   cardioid_pass = true  → express lane: (pos*m)%720
 *   cardioid_pass = false → linear fallback: (pos+1)%720
 *
 * tri[5] FLOW wiring:
 *   arm:     tri[5] = pos (tring position)
 *   resolve: cardioid_pass(tri[5], extract_signal(enc,val))
 *            → trip → tri[2] (WARP dest)
 *
 * Sacred constants: FROZEN
 *   TRING_LEN = 720  (GAN_TRING_CYCLE)
 *   m values : gcd(m, 720) = 1 → full permutation
 *              m=7,11,13,17,19,23 recommended
 *   m=2,3,5  : partial (fan-out, ไม่ครบ 720) — ใช้ได้แต่ไม่ bijective
 *
 * No malloc. No float in hot path. COS_LUT init once at startup.
 * ════════════════════════════════════════════════════════
 */

#ifndef TGW_CARDIOID_EXPRESS_H
#define TGW_CARDIOID_EXPRESS_H

#include <stdint.h>
#include <math.h>    /* cos() — init only, not hot path */

/* ════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════ */

#define CARDIOID_TRING_LEN  720u          /* GAN_TRING_CYCLE — frozen */
#define CARDIOID_SCALE      256           /* Q8 fixed-point base */
#define CARDIOID_A          256           /* a = strength, tunable */

/*
 * Recommended m values (gcd(m,720)=1 → bijective full permutation):
 *   m=7   → cardioid-classic spread
 *   m=11  → tighter clusters
 *   m=13  → prime fan-out
 *   m=17  → high entropy
 * Partial (fan-out only, not bijective):
 *   m=2,3,5 → useful for routing shortcuts, not address permutation
 */
#define CARDIOID_M_DEFAULT  7u

/* ════════════════════════════════════════════════════════
   COS LUT  (Q8: range -255..255)
   init once before first dispatch call
   ════════════════════════════════════════════════════════ */

static int16_t _cardioid_cos_lut[CARDIOID_TRING_LEN];
static uint8_t _cardioid_init_done = 0;

/*
 * cardioid_lut_init()
 * Call once at startup (same place as geo_addr_net_init).
 * Uses float only here — hot path is integer-only after this.
 */
static inline void cardioid_lut_init(void)
{
    if (_cardioid_init_done) return;
    for (uint32_t i = 0; i < CARDIOID_TRING_LEN; i++) {
        double theta = (2.0 * 3.141592653589793 * i) / CARDIOID_TRING_LEN;
        _cardioid_cos_lut[i] = (int16_t)(cos(theta) * 255.0);
    }
    _cardioid_init_done = 1;
}

/* ════════════════════════════════════════════════════════
   CORE: Cardioid Gate  (integer-only, ~3 ops)
   r(θ) = a(1 + cosθ)   Q8 fixed-point
   ════════════════════════════════════════════════════════ */

/*
 * cardioid_pass(pos, r_data) → 1 (express) / 0 (linear)
 *
 * pos    : tring position [0..719] = enc % 720
 * r_data : signal strength [0..255]
 *           typical: extract_signal(enc, value) → value & 0xFF
 *
 * Express zone: r_data × 256 ≤ a × (256 + cos[pos]×255)
 *   θ near 0   → cos=+1 → r_limit = 2a = 512 → wide open
 *   θ near π   → cos=-1 → r_limit = 0        → blocked (cusp)
 *   θ near π/2 → cos=0  → r_limit = a = 256  → threshold
 */
static inline int cardioid_pass(uint16_t pos, uint16_t r_data)
{
    int32_t r_limit = (int32_t)CARDIOID_A *
                      ((int32_t)CARDIOID_SCALE + _cardioid_cos_lut[pos % CARDIOID_TRING_LEN]);
    return ((int32_t)r_data << 8) <= r_limit;
}

/* ════════════════════════════════════════════════════════
   SIGNAL EXTRACTOR
   pos extraction: enc % 720  (same as geo_net_encode)
   r extraction  : value & 0xFF  (byte-0 = traffic signal)
   ════════════════════════════════════════════════════════ */

static inline uint16_t cardioid_pos(uint64_t enc)
{
    return (uint16_t)(enc % CARDIOID_TRING_LEN);
}

static inline uint16_t cardioid_signal(uint32_t value)
{
    return (uint16_t)(value & 0xFFu);
}

/* ════════════════════════════════════════════════════════
   EXPRESS ROUTE  (modular circle jump)
   next = (pos × m) % 720
   gcd(m, 720) = 1 → bijective permutation (no address collision)
   ════════════════════════════════════════════════════════ */

static inline uint16_t cardioid_route(uint16_t pos, uint16_t m)
{
    return (uint16_t)(((uint32_t)pos * m) % CARDIOID_TRING_LEN);
}

/* ════════════════════════════════════════════════════════
   DECISION STRUCT
   ════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t pos;          /* tring position of input */
    uint16_t next_pos;     /* express or linear next   */
    uint16_t r;            /* signal value used        */
    int      use_express;  /* 1 = express, 0 = linear  */
} CardioidDecision;

/*
 * tgw_cardioid_decide()
 *
 * Slot into tgw_dispatch_v2 BEFORE routing decision.
 * enc   = addr (raw uint64_t from dispatch)
 * value = val  (raw uint32_t)
 * m     = modular multiplier (use CARDIOID_M_DEFAULT=7 or tune)
 *
 * Returns CardioidDecision — caller uses .next_pos as hilbert_idx
 * override or GROUND bypass target.
 */
static inline CardioidDecision
tgw_cardioid_decide(uint64_t enc, uint32_t value, uint16_t m)
{
    CardioidDecision d;
    d.pos         = cardioid_pos(enc);
    d.r           = cardioid_signal(value);
    d.use_express = cardioid_pass(d.pos, d.r);
    d.next_pos    = d.use_express
                    ? cardioid_route(d.pos, m)
                    : (uint16_t)((d.pos + 1u) % CARDIOID_TRING_LEN);
    return d;
}

/* ════════════════════════════════════════════════════════
   HOOK A — GROUND early-exit bypass
   ════════════════════════════════════════════════════════

   Insert BEFORE ground_fn call in tgw_dispatch_v2():

   if (geo.polarity) {
       CardioidDecision cd = tgw_cardioid_decide(addr, val,
                                                  CARDIOID_M_DEFAULT);
       if (cd.use_express) {
           // EXPRESS: redirect to next_pos instead of GROUND sink
           // Override hilbert_idx → let this slot route instead
           _v2_push(d, addr, val, cd.next_pos, flush_fn, fn_ctx);
           d->route_count++;
           return;                          // skip ground_fn
       }
       // else: fall through to normal GROUND path
       ground_fn ? ground_fn(addr, val, ground_ctx) : (void)0;
       d->ground_count++;
       return;
   }

   ════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════
   HOOK B — _v2_push hilbert_idx override
   ════════════════════════════════════════════════════════

   Insert AFTER GeoNetAddr computed, BEFORE _v2_push in ROUTE path:

   GeoNetAddr geo = geo_net_encode(addr);
   // ... verdict check ...
   CardioidDecision cd = tgw_cardioid_decide(addr, val,
                                              CARDIOID_M_DEFAULT);
   uint16_t bucket = cd.use_express ? cd.next_pos : geo.hilbert_idx;
   _v2_push(d, addr, val, bucket, flush_fn, fn_ctx);

   Effect: express packets jump to cardioid bucket in counting sort
   → no insertion sort needed for those packets
   → reduces swap count (currently 852 swaps/flush average)

   ════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════
   TRI[5] FLOW WIRING  (EHCP×Topo circuit switch)
   ════════════════════════════════════════════════════════

   cell_delete → fold → tri[5] = cardioid_pos(enc)

   circuit_resolve(folded_cell, key):
     uint16_t pos = tri[5];
     uint16_t r   = cardioid_signal(key);          // key = enc of requester
     if (cardioid_pass(pos, r))
         return tri[2];   // WARP destination
     return UINT32_MAX;   // open circuit

   Why this works:
   - tri[5] stores spatial position of deleted cell (geometry-derived)
   - requester's signal (r) must be "inside" cardioid at that angle
   - complement pair (WARP) opens only when r fits the curve
   - no separate key store — geometry IS the condition

   ════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════
   STATS HELPERS
   ════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t express_count;
    uint32_t linear_count;
    uint32_t ground_bypass;   /* GROUND → express rerouted */
} CardioidStats;

static inline void cardioid_stats_update(CardioidStats *s,
                                          const CardioidDecision *d,
                                          int was_ground)
{
    if (d->use_express) {
        s->express_count++;
        if (was_ground) s->ground_bypass++;
    } else {
        s->linear_count++;
    }
}

static inline double cardioid_express_ratio(const CardioidStats *s)
{
    uint32_t total = s->express_count + s->linear_count;
    if (total == 0u) return 0.0;
    return (double)s->express_count / (double)total;
}

/* ════════════════════════════════════════════════════════
   CUSP ZONE DETECTOR
   θ near π → cos → -1 → r_limit → 0 → anomaly zone
   Use for monitoring, not routing
   ════════════════════════════════════════════════════════ */

/*
 * cardioid_in_cusp(pos)
 * Returns 1 if position is in cusp zone (cos < -0.75 = approx pos 315..405)
 * These slots are blocked regardless of r — use as anomaly signal
 */
static inline int cardioid_in_cusp(uint16_t pos)
{
    /* cos_lut[pos] < -191 (= -0.75 × 255) */
    return _cardioid_cos_lut[pos % CARDIOID_TRING_LEN] < -191;
}

#endif /* TGW_CARDIOID_EXPRESS_H */
