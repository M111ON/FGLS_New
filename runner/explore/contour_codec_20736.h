// contour_codec_20736.h — header-only pack/unpack codec for contour cube
// ══════════════════════════════════════════════════════════════════════════
// Packs 6000 cells (6 faces × 10×10×10) into 144×144 = 20736 addresses.
// Lossless roundtrip guaranteed by bijective mapping.
// Supports: sequential, stride37, face_region, grid strategies.
// All decode lookups are O(1) — direct index arithmetic, no search.
//
// Usage:
//   #define CONTOUR_CODEC_IMPLEMENTATION   // in exactly ONE .c file
//   #include "contour_codec_20736.h"
//
// API:
//   codec_ctx *codec_create(CODEC_STRATEGY strategy);
//   void       codec_free(codec_ctx *ctx);
//   int        codec_encode(codec_ctx *ctx, const contour_cell *cells, int n);
//   int        codec_decode(codec_ctx *ctx, contour_cell *out, int max_out);
//   int8_t     codec_get(const codec_ctx *ctx, int face, int x, int y, int z);
//   void       codec_set(codec_ctx *ctx, int face, int x, int y, int z, int8_t val);
//   int        codec_verify(codec_ctx *ctx, const contour_cell *cells, int n);
//   const char *codec_strategy_name(CODEC_STRATEGY s);
// ══════════════════════════════════════════════════════════════════════════

#ifndef CONTOUR_CODEC_20736_H
#define CONTOUR_CODEC_20736_H

#include <stdint.h>
#include <string.h>

// ── Constants ──────────────────────────────────────────────────────────────

#define CC_FACES       6
#define CC_W          10
#define CC_H          10
#define CC_L          10
#define CC_CELLS     (CC_FACES * CC_W * CC_H * CC_L)   // 6000
#define CC_GEO_FULL  20736                               // 144 × 144
#define CC_GEO_DIM   144                                 // sqrt(20736)

// ── Types ──────────────────────────────────────────────────────────────────

typedef struct {
    int face, x, y, z;
    int8_t value;
    int global_idx;       // 0..5999
} contour_cell;

typedef enum {
    CODEC_SEQUENTIAL  = 0,  // face*1000 + z*100 + y*10 + x
    CODEC_STRIDE37    = 1,  // global_idx * 37 % 20736
    CODEC_FACE_REGION = 2,  // face*3456 + z*345 + y*34 + x
    CODEC_GRID        = 3,  // row=face*24+z, col=y*10+x
    CODEC_COUNT       = 4
} CODEC_STRATEGY;

typedef struct {
    CODEC_STRATEGY strategy;
    int8_t geo[CC_GEO_FULL];       // flat 144×144 store
    int cell_count;                 // cells encoded
    int errors;                     // last roundtrip error count
    uint32_t collision_mask;        // bitfield: 1 = collision detected
} codec_ctx;

// ── Forward declarations ───────────────────────────────────────────────────

#ifdef __cplusplus
extern "C" {
#endif

codec_ctx *codec_create(CODEC_STRATEGY strategy);
void       codec_free(codec_ctx *ctx);
int        codec_encode(codec_ctx *ctx, const contour_cell *cells, int n);
int        codec_decode(codec_ctx *ctx, contour_cell *out, int max_out);
int8_t     codec_get(const codec_ctx *ctx, int face, int x, int y, int z);
void       codec_set(codec_ctx *ctx, int face, int x, int y, int z, int8_t val);
int        codec_verify(codec_ctx *ctx, const contour_cell *cells, int n);
const char *codec_strategy_name(CODEC_STRATEGY s);

#ifdef __cplusplus
}
#endif

// ══════════════════════════════════════════════════════════════════════════
// IMPLEMENTATION
// ══════════════════════════════════════════════════════════════════════════

#ifdef CONTOUR_CODEC_IMPLEMENTATION

#include <stdlib.h>
#include <stdio.h>

// ── Inline cell index helpers ──────────────────────────────────────────────

static inline int cell_global_idx(int face, int x, int y, int z) {
    return face * CC_W * CC_H * CC_L + z * CC_H * CC_W + y * CC_W + x;
}

static inline void cell_from_idx(int idx, int *face, int *x, int *y, int *z) {
    *face = idx / (CC_W * CC_H * CC_L);
    int rem = idx % (CC_W * CC_H * CC_L);
    *z = rem / (CC_H * CC_W);
    rem %= (CC_H * CC_W);
    *y = rem / CC_W;
    *x = rem % CC_W;
}

// ── Mapping functions: cell → geo address (O(1)) ──────────────────────────

static inline uint32_t map_sequential(const contour_cell *c) {
    return (uint32_t)(c->face * 1000 + c->z * 100 + c->y * 10 + c->x);
}

static inline uint32_t map_stride37(const contour_cell *c) {
    return (uint32_t)((uint32_t)c->global_idx * 37u) % CC_GEO_FULL;
}

static inline uint32_t map_face_region(const contour_cell *c) {
    return (uint32_t)(c->face * 3456 + c->z * 345 + c->y * 34 + c->x);
}

static inline uint32_t map_grid(const contour_cell *c) {
    uint32_t row = (uint32_t)(c->face * 24 + c->z);
    uint32_t col = (uint32_t)(c->y * 10 + c->x);
    return row * CC_GEO_DIM + col;
}

// ── Dispatcher ─────────────────────────────────────────────────────────────

typedef uint32_t (*cc_mapfn)(const contour_cell *);

static cc_mapfn map_func_for(CODEC_STRATEGY s) {
    switch (s) {
        case CODEC_SEQUENTIAL:  return map_sequential;
        case CODEC_STRIDE37:    return map_stride37;
        case CODEC_FACE_REGION: return map_face_region;
        case CODEC_GRID:        return map_grid;
        default:                return map_sequential;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

codec_ctx *codec_create(CODEC_STRATEGY strategy) {
    if (strategy < 0 || strategy >= CODEC_COUNT) return NULL;
    codec_ctx *c = (codec_ctx *)calloc(1, sizeof(codec_ctx));
    if (!c) return NULL;
    c->strategy = strategy;
    return c;
}

void codec_free(codec_ctx *ctx) {
    free(ctx);
}

const char *codec_strategy_name(CODEC_STRATEGY s) {
    static const char *names[] = {
        "sequential", "stride37", "face_region", "grid"
    };
    if (s >= 0 && s < CODEC_COUNT) return names[s];
    return "unknown";
}

int codec_encode(codec_ctx *ctx, const contour_cell *cells, int n) {
    if (!ctx || !cells || n <= 0) return -1;
    memset(ctx->geo, 0, CC_GEO_FULL);
    ctx->cell_count = n;
    ctx->collision_mask = 0;
    cc_mapfn map = map_func_for(ctx->strategy);

    uint8_t seen[CC_GEO_FULL];
    memset(seen, 0, CC_GEO_FULL);

    int collisions = 0;
    for (int i = 0; i < n; i++) {
        uint32_t addr = map(&cells[i]);
        if (addr >= CC_GEO_FULL) continue;
        if (seen[addr]) {
            collisions++;
            if (collisions <= 5) {
                fprintf(stderr, "[CODEC] collision %d at addr=%u (face=%d x=%d y=%d z=%d) val=%d overwriting %d\n",
                        collisions, addr,
                        cells[i].face, cells[i].x, cells[i].y, cells[i].z,
                        (int)cells[i].value, (int)ctx->geo[addr]);
            }
        }
        seen[addr] = 1;
        ctx->geo[addr] = cells[i].value;
    }
    ctx->collision_mask = (uint32_t)collisions;
    return collisions;
}

int codec_decode(codec_ctx *ctx, contour_cell *out, int max_out) {
    if (!ctx || !out || max_out <= 0) return -1;
    cc_mapfn map = map_func_for(ctx->strategy);
    int written = 0;
    ctx->errors = 0;

    // Decode each cell by recomputing its address
    for (int i = 0; i < ctx->cell_count && i < max_out; i++) {
        contour_cell tmp;
        cell_from_idx(i, &tmp.face, &tmp.x, &tmp.y, &tmp.z);
        tmp.global_idx = i;
        uint32_t addr = map(&tmp);
        out[written].face = tmp.face;
        out[written].x = tmp.x;
        out[written].y = tmp.y;
        out[written].z = tmp.z;
        out[written].global_idx = i;
        out[written].value = ctx->geo[addr];
        written++;
    }
    return written;
}

int8_t codec_get(const codec_ctx *ctx, int face, int x, int y, int z) {
    if (!ctx) return 0;
    contour_cell c;
    c.face = face; c.x = x; c.y = y; c.z = z;
    c.global_idx = cell_global_idx(face, x, y, z);
    cc_mapfn map = map_func_for(ctx->strategy);
    uint32_t addr = map(&c);
    return ctx->geo[addr];
}

void codec_set(codec_ctx *ctx, int face, int x, int y, int z, int8_t val) {
    if (!ctx) return;
    contour_cell c;
    c.face = face; c.x = x; c.y = y; c.z = z;
    c.global_idx = cell_global_idx(face, x, y, z);
    cc_mapfn map = map_func_for(ctx->strategy);
    uint32_t addr = map(&c);
    ctx->geo[addr] = val;
}

int codec_verify(codec_ctx *ctx, const contour_cell *cells, int n) {
    if (!ctx || !cells) return -1;
    cc_mapfn map = map_func_for(ctx->strategy);
    int errors = 0;
    for (int i = 0; i < n; i++) {
        uint32_t addr = map(&cells[i]);
        if (addr >= CC_GEO_FULL) { errors++; continue; }
        if (ctx->geo[addr] != cells[i].value) errors++;
    }
    ctx->errors = errors;
    return errors;
}

#endif // CONTOUR_CODEC_IMPLEMENTATION
#endif // CONTOUR_CODEC_20736_H
