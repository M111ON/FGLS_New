/* contour_codec_scaled.h — Parameterized contour codec for any cube size
 * ═══════════════════════════════════════════════════════════════════════════
 * 
 * Unlike contour_codec_20736.h (fixed 6×10×10×10), this codec accepts
 * dimensions at runtime: faces, W, H, L.
 * 
 * Geo space auto-sizes: finds next square ≥ cells (or uses 20736 max)
 * Mapping functions compute O(1) from config — no big tables.
 * 
 * Usage:
 *   #define CONTOUR_CODEC_SCALED_IMPLEMENTATION
 *   #include "contour_codec_scaled.h"
 * 
 *   codec_scaled_config cfg = { .faces=6, .W=10, .H=10, .L=10, .strategy=1 };
 *   codec_scaled_ctx *ctx = codec_scaled_create(&cfg);
 *   codec_scaled_encode(ctx, cells, n);
 *   codec_scaled_decode(ctx, out, max_out);
 */

#ifndef CONTOUR_CODEC_SCALED_H
#define CONTOUR_CODEC_SCALED_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
   Configuration
   ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    CCS_SEQUENTIAL  = 0,  // face*(W*H*L) + z*(H*W) + y*W + x
    CCS_STRIDE37    = 1,  // global_idx * stride % geo_full (stride coprime to geo_full)
    CCS_FACE_REGION = 2,  // face*region_size + z*region_z + y*region_y + x
    CCS_GRID        = 3,  // row = face*z_planes + z, col = y*W + x
    CCS_COUNT       = 4
} ccs_strategy;

typedef struct {
    int faces;              // Number of faces (6 or 12 typically)
    int W, H, L;            // Dimensions per face
    ccs_strategy strategy;  // Mapping strategy
    int geo_full;           // Geo space size (0 = auto)
    int geo_dim;            // sqrt(geo_full) (0 = auto)
    int stride;             // For stride37 (0 = auto coprime)
} ccs_config;

typedef struct {
    int face, x, y, z;
    int8_t value;
    int global_idx;         // 0..cells-1
} ccs_cell;

typedef struct ccs_ctx ccs_ctx;

/* ═══════════════════════════════════════════════════════════════════════════
   Public API
   ═══════════════════════════════════════════════════════════════════════════ */

/* Create context with config. Returns NULL on invalid config. */
ccs_ctx *ccs_create(const ccs_config *cfg);

/* Free context and its geo array */
void ccs_free(ccs_ctx *ctx);

/* Encode cells into geo space. Returns collision count (0 = perfect). */
int ccs_encode(ccs_ctx *ctx, const ccs_cell *cells, int n);

/* Decode cells from geo space. Returns cells decoded. */
int ccs_decode(ccs_ctx *ctx, ccs_cell *out, int max_out);

/* O(1) single cell get/set */
int8_t ccs_get(const ccs_ctx *ctx, int face, int x, int y, int z);
void   ccs_set(ccs_ctx *ctx, int face, int x, int y, int z, int8_t val);

/* Verify roundtrip. Returns mismatch count. */
int ccs_verify(ccs_ctx *ctx, const ccs_cell *cells, int n);

/* Strategy name */
const char *ccs_strategy_name(ccs_strategy s);

/* Default config helper */
static inline ccs_config ccs_default_config(void) {
    ccs_config cfg = {
        .faces = 6,
        .W = 10, .H = 10, .L = 10,
        .strategy = CCS_STRIDE37,
        .geo_full = 0,  // auto
        .geo_dim = 0,   // auto
        .stride = 0,    // auto
    };
    return cfg;
}

/* Get config from context */
const ccs_config *ccs_get_config(const ccs_ctx *ctx);

/* Total cells in this config */
static inline int ccs_total_cells(const ccs_config *cfg) {
    return cfg->faces * cfg->W * cfg->H * cfg->L;
}

#ifdef __cplusplus
}
#endif

#endif /* CONTOUR_CODEC_SCALED_H */


/* ═══════════════════════════════════════════════════════════════════════════
   IMPLEMENTATION
   ═══════════════════════════════════════════════════════════════════════════ */

#ifdef CONTOUR_CODEC_SCALED_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════════════
   Internal Context
   ═══════════════════════════════════════════════════════════════════════════ */

struct ccs_ctx {
    ccs_config config;
    int8_t *geo;              // Dynamic geo array [geo_full]
    uint8_t *seen;            // For collision detection [geo_full]
    int initialized;
};

/* ═══════════════════════════════════════════════════════════════════════════
   Helpers
   ═══════════════════════════════════════════════════════════════════════════ */

static int gcd_int(int a, int b) {
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

static int next_coprime(int n, int start) {
    for (int i = start; i < n; i++) {
        if (gcd_int(i, n) == 1) return i;
    }
    return 37;  // fallback
}

static int next_square_ge(int n) {
    int r = (int)ceil(sqrt((double)n));
    return r * r;
}

/* Compute derived values from config */
static void ccs_compute_derived(ccs_config *cfg) {
    int cells = cfg->faces * cfg->W * cfg->H * cfg->L;
    
    if (cfg->geo_full <= 0) {
        cfg->geo_full = next_square_ge(cells);
        // Cap at 20736 (144×144) for compatibility
        if (cfg->geo_full > 20736) cfg->geo_full = 20736;
    }
    if (cfg->geo_dim <= 0) {
        cfg->geo_dim = (int)round(sqrt((double)cfg->geo_full));
    }
    if (cfg->stride <= 0) {
        cfg->stride = next_coprime(cfg->geo_full, 37);
    }
}

/* Cell helpers using config dimensions */
static inline int ccs_global_idx(const ccs_config *cfg, int face, int x, int y, int z) {
    return face * cfg->W * cfg->H * cfg->L + z * cfg->H * cfg->W + y * cfg->W + x;
}

static inline void ccs_from_idx(const ccs_config *cfg, int idx, int *face, int *x, int *y, int *z) {
    int face_cells = cfg->W * cfg->H * cfg->L;
    *face = idx / face_cells;
    int rem = idx % face_cells;
    *z = rem / (cfg->H * cfg->W);
    rem %= (cfg->H * cfg->W);
    *y = rem / cfg->W;
    *x = rem % cfg->W;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Mapping Functions (all O(1), no tables)
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t map_sequential(const ccs_config *cfg, const ccs_cell *c) {
    return (uint32_t)(c->face * cfg->W * cfg->H * cfg->L + c->z * cfg->H * cfg->W + c->y * cfg->W + c->x);
}

static inline uint32_t map_stride37(const ccs_config *cfg, const ccs_cell *c) {
    int global = ccs_global_idx(cfg, c->face, c->x, c->y, c->z);
    return (uint32_t)((uint32_t)global * (uint32_t)cfg->stride) % (uint32_t)cfg->geo_full;
}

static inline uint32_t map_face_region(const ccs_config *cfg, const ccs_cell *c) {
    int region_size = cfg->geo_full / cfg->faces;
    int region_z = cfg->H;
    int region_y = cfg->W;
    return (uint32_t)(c->face * region_size + c->z * region_z * region_y + c->y * region_y + c->x);
}

static inline uint32_t map_grid(const ccs_config *cfg, const ccs_cell *c) {
    int z_planes = (cfg->geo_dim + cfg->faces - 1) / cfg->faces;  // ceil
    uint32_t row = (uint32_t)(c->face * z_planes + c->z);
    uint32_t col = (uint32_t)(c->y * cfg->W + c->x);
    return row * (uint32_t)cfg->geo_dim + col;
}

typedef uint32_t (*ccs_mapfn)(const ccs_config *, const ccs_cell *);

static ccs_mapfn map_func_for(ccs_strategy s) {
    switch (s) {
        case CCS_SEQUENTIAL:  return map_sequential;
        case CCS_STRIDE37:    return map_stride37;
        case CCS_FACE_REGION: return map_face_region;
        case CCS_GRID:        return map_grid;
        default:              return map_sequential;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Public API Implementation
   ═══════════════════════════════════════════════════════════════════════════ */

ccs_ctx *ccs_create(const ccs_config *cfg) {
    if (!cfg) return NULL;
    
    ccs_config computed = *cfg;
    ccs_compute_derived(&computed);
    
    int cells = computed.faces * computed.W * computed.H * computed.L;
    if (cells > computed.geo_full) {
        // Cannot fit: more cells than geo addresses
        return NULL;
    }
    
    ccs_ctx *ctx = (ccs_ctx*)calloc(1, sizeof(ccs_ctx));
    if (!ctx) return NULL;
    
    ctx->config = computed;
    ctx->geo = (int8_t*)calloc(computed.geo_full, sizeof(int8_t));
    ctx->seen = (uint8_t*)calloc(computed.geo_full, sizeof(uint8_t));
    
    if (!ctx->geo || !ctx->seen) {
        ccs_free(ctx);
        return NULL;
    }
    
    ctx->initialized = 1;
    return ctx;
}

void ccs_free(ccs_ctx *ctx) {
    if (!ctx) return;
    free(ctx->geo);
    free(ctx->seen);
    free(ctx);
}

int ccs_encode(ccs_ctx *ctx, const ccs_cell *cells, int n) {
    if (!ctx || !ctx->initialized || !cells || n <= 0) return -1;
    
    memset(ctx->geo, 0, ctx->config.geo_full);
    memset(ctx->seen, 0, ctx->config.geo_full);
    
    ccs_mapfn map = map_func_for(ctx->config.strategy);
    int collisions = 0;
    
    for (int i = 0; i < n; i++) {
        uint32_t addr = map(&ctx->config, &cells[i]);
        if (addr >= (uint32_t)ctx->config.geo_full) continue;
        if (ctx->seen[addr]) {
            collisions++;
        }
        ctx->seen[addr] = 1;
        ctx->geo[addr] = cells[i].value;
    }
    return collisions;
}

int ccs_decode(ccs_ctx *ctx, ccs_cell *out, int max_out) {
    if (!ctx || !ctx->initialized || !out || max_out <= 0) return -1;
    
    ccs_mapfn map = map_func_for(ctx->config.strategy);
    int written = 0;
    int total_cells = ccs_total_cells(&ctx->config);
    
    for (int i = 0; i < total_cells && written < max_out; i++) {
        ccs_cell tmp;
        ccs_from_idx(&ctx->config, i, &tmp.face, &tmp.x, &tmp.y, &tmp.z);
        tmp.global_idx = i;
        uint32_t addr = map(&ctx->config, &tmp);
        if (addr >= (uint32_t)ctx->config.geo_full) continue;
        
        out[written] = tmp;
        out[written].value = ctx->geo[addr];
        written++;
    }
    return written;
}

int8_t ccs_get(const ccs_ctx *ctx, int face, int x, int y, int z) {
    if (!ctx || !ctx->initialized) return 0;
    
    ccs_cell tmp = { .face = face, .x = x, .y = y, .z = z };
    tmp.global_idx = ccs_global_idx(&ctx->config, face, x, y, z);
    
    ccs_mapfn map = map_func_for(ctx->config.strategy);
    uint32_t addr = map(&ctx->config, &tmp);
    if (addr >= (uint32_t)ctx->config.geo_full) return 0;
    return ctx->geo[addr];
}

void ccs_set(ccs_ctx *ctx, int face, int x, int y, int z, int8_t val) {
    if (!ctx || !ctx->initialized) return;
    
    ccs_cell tmp = { .face = face, .x = x, .y = y, .z = z };
    tmp.global_idx = ccs_global_idx(&ctx->config, face, x, y, z);
    
    ccs_mapfn map = map_func_for(ctx->config.strategy);
    uint32_t addr = map(&ctx->config, &tmp);
    if (addr >= (uint32_t)ctx->config.geo_full) return;
    ctx->geo[addr] = val;
}

int ccs_verify(ccs_ctx *ctx, const ccs_cell *cells, int n) {
    if (!ctx || !cells || n <= 0) return -1;
    
    ccs_cell *decoded = (ccs_cell*)malloc(n * sizeof(ccs_cell));
    if (!decoded) return -1;
    
    int decoded_n = ccs_decode(ctx, decoded, n);
    int mismatches = 0;
    
    for (int i = 0; i < n && i < decoded_n; i++) {
        if (cells[i].value != decoded[i].value) mismatches++;
    }
    
    free(decoded);
    return mismatches;
}

const char *ccs_strategy_name(ccs_strategy s) {
    static const char *names[] = { "sequential", "stride37", "face_region", "grid" };
    if (s >= 0 && s < CCS_COUNT) return names[s];
    return "unknown";
}

const ccs_config *ccs_get_config(const ccs_ctx *ctx) {
    return ctx ? &ctx->config : NULL;
}

#endif /* CONTOUR_CODEC_SCALED_IMPLEMENTATION */