/*
 * bermuda_export.h — Bermuda Geometry Router C ABI
 * ═══════════════════════════════════════════════════════════════
 * C export of core bermuda ops: gear snap, Hilbert stride-37,
 * geometry traverse, zone classification.
 *
 * All integer arithmetic — no float, no malloc, O(1) per op.
 * Mirrors bermuda_reshape_v3.py exactly.
 *
 * Integration:
 *   #include "bermuda_export.h"
 *   uint16_t idx = bermuda_traverse(42, 2, 0);  // ORBITAL
 *   uint8_t  z   = bermuda_zone(42, 2);
 *
 * Compile as .dll/.so:
 *   gcc -O2 -shared -fPIC -o pogls_bermuda.so bermuda_export.c
 */

#ifndef BERMUDA_EXPORT_H
#define BERMUDA_EXPORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants (mirrors bermuda_reshape_v3.py) ─────────────── */
#define BERMUDA_STRIDE       37u
#define BERMUDA_N_ZONES      12u
#define BERMUDA_TRING_SLOTS  720u

/* Gear table: 128*n slots, 2^k aligned */
#define BERMUDA_GEAR1_SLOTS  512u
#define BERMUDA_GEAR2_SLOTS  1024u
#define BERMUDA_GEAR3_SLOTS  2048u
#define BERMUDA_GEAR4_SLOTS  4096u

/* ── Runtime Tables (filled by bermuda_init) ─────────────────
 *
 * The per-gear walk_len (smallest multiple of 12 coprime with 37)
 * and modular inverses of 37 are computed dynamically at init.
 * The static tables below are reference only; bermuda_init()
 * overwrites the runtime BermudaCtx with correct values. */

/* Slot count per gear */
static const uint16_t BERMUDA_SLOTS[] = {
    0, 512, 1024, 2048, 4096
};

/* ── CROSS LUT (self-inverse zone pair mapping) ────────────── */
static const uint8_t BERMUDA_CROSS[] = {9,10,11,6,7,8,3,4,5,0,1,2};

/* ── Runtime state (filled by bermuda_init) ────────────────── */

typedef struct {
    uint16_t slots[5];       /* 0-indexed, [0]=0, [1]=512, ... */
    uint16_t inv37[5];       /* modular inverse of 37 mod slots */
    uint16_t walk_len[5];    /* per-gear walk_len */
    uint16_t face_sz[5];     /* walk_len / 12 */
    uint16_t inv37_wl[5];    /* modular inverse of 37 mod walk_len */
    uint8_t  initialized;
} BermudaCtx;

static BermudaCtx _bermuda_ctx = {0};

/* ── Modular inverse (Extended Euclidean) ──────────────────── */
static uint16_t _bermuda_modinv(uint16_t a, uint16_t m) {
    int32_t g = (int32_t)m, x = 0, a0 = (int32_t)a, x0 = 1;
    while (a0 != 0) {
        int32_t q = g / a0;
        int32_t t = a0; a0 = g - q * a0; g = t;
        t = x0; x0 = x - q * x0; x = t;
    }
    int32_t r = x % (int32_t)m;
    return (uint16_t)(r < 0 ? r + (int32_t)m : r);
}

/* ── Compute walk_len (smallest multiple of 12 >= slots, coprime with 37) ─ */
static uint16_t _bermuda_walk_len(uint16_t slots) {
    uint16_t wl = slots;
    while (1) {
        if (wl % 12 == 0) {
            /* gcd(37, wl) — 37 is prime, so check wl % 37 != 0 */
            if (wl % 37 != 0) return wl;
        }
        wl++;
    }
}

/* ── Init ───────────────────────────────────────────────────── */
static inline void bermuda_init(void) {
    if (_bermuda_ctx.initialized) return;
    for (int g = 1; g <= 4; g++) {
        uint16_t slots = BERMUDA_SLOTS[g];
        _bermuda_ctx.slots[g]    = slots;
        _bermuda_ctx.inv37[g]    = _bermuda_modinv(BERMUDA_STRIDE, slots);
        _bermuda_ctx.walk_len[g] = _bermuda_walk_len(slots);
        _bermuda_ctx.face_sz[g]  = _bermuda_ctx.walk_len[g] / 12;
        _bermuda_ctx.inv37_wl[g] = _bermuda_modinv(BERMUDA_STRIDE,
                                     _bermuda_ctx.walk_len[g]);
    }
    _bermuda_ctx.initialized = 1;
}

/* ── Gear snap ─────────────────────────────────────────────── */
static inline uint8_t bermuda_snap_gear(uint16_t n_tokens) {
    if (n_tokens <= 512)  return 1;
    if (n_tokens <= 1024) return 2;
    if (n_tokens <= 2048) return 3;
    return 4;
}

/* ── Hilbert encode/decode ─────────────────────────────────── */
static inline uint16_t bermuda_hilbert_encode(uint16_t position, uint8_t gear) {
    return (uint16_t)(((uint32_t)position * BERMUDA_STRIDE)
                      % _bermuda_ctx.slots[gear]);
}

static inline uint16_t bermuda_hilbert_decode(uint16_t index, uint8_t gear) {
    return (uint16_t)(((uint32_t)index * _bermuda_ctx.inv37[gear])
                      % _bermuda_ctx.slots[gear]);
}

/* ── Zone from index (0-11) ────────────────────────────────── */
static inline uint8_t bermuda_zone(uint16_t idx, uint8_t gear) {
    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE)
                              % _bermuda_ctx.walk_len[gear]);
    return (uint8_t)(enc / _bermuda_ctx.face_sz[gear]);
}

/* ── Pole from zone (0=south 1=north) ──────────────────────── */
static inline uint8_t bermuda_pole(uint8_t zone) {
    return zone >= 6 ? 1 : 0;
}

/* ── TRing slot from index (0-719) ─────────────────────────── */
static inline uint16_t bermuda_tring_slot(uint16_t idx) {
    return idx % BERMUDA_TRING_SLOTS;
}

/* ── Shape from mode + zone ────────────────────────────────── */
/* Returns shape byte matching pogls_bond.h (ASCII: I=73, O=79, S=83, L=76) */
static inline uint8_t bermuda_shape(uint8_t mode, uint8_t zone) {
    switch (mode) {
        case 0: return zone < 6 ? 73 : 79;   /* ORBITAL: I or O */
        case 1: return 79;                    /* CHIRAL: O */
        case 2: return 83;                    /* CROSS: S */
        case 3: return 76;                    /* HUB: L */
        default: return 73;                   /* fallback: I */
    }
}

/* ── Polarity from mode + zone (0=ROUTE 1=GROUND) ──────────── */
static inline uint8_t bermuda_polarity(uint8_t mode, uint8_t zone) {
    switch (mode) {
        case 0: return bermuda_pole(zone);   /* ORBITAL: pole-dependent */
        case 1: return 1;                    /* CHIRAL: always GROUND */
        case 2: return 0;                    /* CROSS: always ROUTE */
        case 3: return 1;                    /* HUB: always GROUND */
        default: return 0;
    }
}

/* ── Traverse modes ────────────────────────────────────────── */
static inline uint16_t bermuda_traverse_orbit(uint16_t idx, uint8_t gear) {
    uint16_t N = _bermuda_ctx.slots[gear];
    return (uint16_t)((idx + 1) % N);
}

static inline uint16_t bermuda_traverse_chiral(uint16_t idx, uint8_t gear) {
    uint16_t N = _bermuda_ctx.slots[gear];
    return (uint16_t)((idx + N / 2) % N);
}

static inline uint16_t bermuda_traverse_cross(uint16_t idx, uint8_t gear) {
    uint16_t WL = _bermuda_ctx.walk_len[gear];
    uint16_t FS = _bermuda_ctx.face_sz[gear];
    uint16_t IW = _bermuda_ctx.inv37_wl[gear];
    uint16_t N  = _bermuda_ctx.slots[gear];

    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % WL);
    uint8_t  z   = (uint8_t)(enc / FS);
    uint8_t  pz  = BERMUDA_CROSS[z % 12];
    uint16_t ne  = (uint16_t)(pz * FS + enc % FS);
    return (uint16_t)(((uint32_t)ne * IW) % WL % N);
}

static inline uint16_t bermuda_traverse_hub(uint16_t idx, uint8_t gear) {
    uint16_t WL    = _bermuda_ctx.walk_len[gear];
    uint16_t FS    = _bermuda_ctx.face_sz[gear];
    uint16_t N     = _bermuda_ctx.slots[gear];
    uint16_t enc   = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % WL);
    uint8_t  z     = (uint8_t)(enc / FS);
    return (uint16_t)((uint32_t)z * (N / BERMUDA_N_ZONES) % N);
}

/* Unified traverse: mode=0..3 */
static inline uint16_t bermuda_traverse(uint16_t idx, uint8_t gear, uint8_t mode) {
    switch (mode) {
        case 0:  return bermuda_traverse_orbit(idx, gear);
        case 1:  return bermuda_traverse_chiral(idx, gear);
        case 2:  return bermuda_traverse_cross(idx, gear);
        case 3:  return bermuda_traverse_hub(idx, gear);
        default: return idx;
    }
}

/* ── Batch route (convenience) ─────────────────────────────── */
typedef struct {
    uint16_t idx_in;       /* original codebook index */
    uint16_t idx_out;      /* after traverse */
    uint8_t  zone;         /* 0-11 */
    uint8_t  pole;         /* 0=south 1=north */
    uint8_t  shape;        /* shape byte */
    uint8_t  polarity;     /* 0=ROUTE 1=GROUND */
    uint16_t tring_slot;   /* 0-719 */
} BermudaRouteEntry;

static inline void bermuda_route_token(
    uint16_t          idx_in,
    uint8_t           gear,
    uint8_t           mode,
    BermudaRouteEntry *out
) {
    uint16_t idx_out = bermuda_traverse(idx_in, gear, mode);
    uint8_t  z       = bermuda_zone(idx_in, gear);
    out->idx_in      = idx_in;
    out->idx_out     = idx_out;
    out->zone        = z;
    out->pole        = bermuda_pole(z);
    out->shape       = bermuda_shape(mode, z);
    out->polarity    = bermuda_polarity(mode, z);
    out->tring_slot  = bermuda_tring_slot(idx_in);
}

/* ── Exported C ABI functions (for .dll/.so) ───────────────── */
#ifdef BERMUDA_EXPORT_DLL
    #ifdef _WIN32
        #define BERMUDA_API __declspec(dllexport)
    #else
        #define BERMUDA_API __attribute__((visibility("default")))
    #endif

    BERMUDA_API void     bermuda_init_dll(void);
    BERMUDA_API uint8_t  bermuda_snap_gear_dll(uint16_t n);
    BERMUDA_API uint16_t bermuda_hilbert_encode_dll(uint16_t pos, uint8_t g);
    BERMUDA_API uint16_t bermuda_hilbert_decode_dll(uint16_t idx, uint8_t g);
    BERMUDA_API uint16_t bermuda_traverse_dll(uint16_t idx, uint8_t g, uint8_t m);
    BERMUDA_API uint8_t  bermuda_zone_dll(uint16_t idx, uint8_t g);
    BERMUDA_API void     bermuda_route_batch(
                           const uint16_t *idxs, uint8_t gear, uint8_t mode,
                           BermudaRouteEntry *out, uint32_t n);

#endif /* BERMUDA_EXPORT_DLL */

#ifdef __cplusplus
}
#endif

#endif /* BERMUDA_EXPORT_H */
