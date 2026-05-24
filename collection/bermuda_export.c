/*
 * bermuda_export.c — DLL exports for bermuda_export.h
 * ═══════════════════════════════════════════════════════════════
 * Compile:
 *   gcc -O2 -shared -fPIC -DBERMUDA_EXPORT_DLL
 *       -o pogls_bermuda.dll bermuda_export.c
 *       -Wl,--out-implib,libpogls_bermuda.a
 *
 * IMPORTANT: Every DLL entry point guards with bermuda_init()
 * to prevent inline calls before context tables are populated.
 */

#define BERMUDA_EXPORT_DLL
#include "bermuda_export.h"

/* ── Guard macro: call bermuda_init() once before every DLL op ── */
#define BERMUDA_GUARD() do {                \
    if (!_bermuda_ctx.initialized) {        \
        bermuda_init();                     \
    }                                       \
} while(0)

void bermuda_init_dll(void) {
    bermuda_init();
}

uint8_t bermuda_snap_gear_dll(uint16_t n) {
    BERMUDA_GUARD();
    return bermuda_snap_gear(n);
}

uint16_t bermuda_hilbert_encode_dll(uint16_t pos, uint8_t g) {
    BERMUDA_GUARD();
    return bermuda_hilbert_encode(pos, g);
}

uint16_t bermuda_hilbert_decode_dll(uint16_t idx, uint8_t g) {
    BERMUDA_GUARD();
    return bermuda_hilbert_decode(idx, g);
}

uint16_t bermuda_traverse_dll(uint16_t idx, uint8_t g, uint8_t m) {
    BERMUDA_GUARD();
    return bermuda_traverse(idx, g, m);
}

uint8_t bermuda_zone_dll(uint16_t idx, uint8_t g) {
    BERMUDA_GUARD();
    return bermuda_zone(idx, g);
}

void bermuda_route_batch(
    const uint16_t *idxs, uint8_t gear, uint8_t mode,
    BermudaRouteEntry *out, uint32_t n
) {
    BERMUDA_GUARD();
    for (uint32_t i = 0; i < n; i++) {
        bermuda_route_token(idxs[i], gear, mode, &out[i]);
    }
}
