/* fgls_pipeline.h — Unified FGLS Geometric Weight Storage Pipeline
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Chains: contour_encode → geo_jump_route → frame_seek
 *         (DRamTile + GPU optional — requires full POGLS build)
 *
 * Usage:
 *   #define FGLS_PIPELINE_IMPLEMENTATION  // in exactly ONE .c file
 *   #include "fgls_pipeline.h"
 *
 * API:
 *   fgls_ctx *fgls_init(const fgls_config *cfg);
 *   void     fgls_free(fgls_ctx *ctx);
 *   int      fgls_encode(fgls_ctx *ctx, const fgls_cell *cells, int n);
 *   int      fgls_decode(fgls_ctx *ctx, fgls_cell *out, int max_out);
 *   double   fgls_benchmark(fgls_ctx *ctx, int iterations);
 *   void     fgls_stats(fgls_ctx *ctx, FILE *fp);
 *
 * Config:
 *   fgls_config cfg = {
 *       .strategy = FGLS_STRIDE37,
 *       .use_geojump = 1,
 *       .max_bytes = 64 * 1024 * 1024,
 *   };
 */

#ifndef FGLS_PIPELINE_H
#define FGLS_PIPELINE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════════
   Constants (from contour_codec_20736.h)
   ═══════════════════════════════════════════════════════════════════════════════ */

#define FGLS_FACES      6
#define FGLS_W          10
#define FGLS_H          10
#define FGLS_L          10
#define FGLS_CELLS      (FGLS_FACES * FGLS_W * FGLS_H * FGLS_L)   // 6000
#define FGLS_GEO_FULL   20736                                     // 144 × 144
#define FGLS_GEO_DIM    144
#define FGLS_FIBO_CLOCK 1440

/* ═══════════════════════════════════════════════════════════════════════════════
   Contour Cell (matches contour_codec_20736.h)
   ═══════════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int face, x, y, z;
    int8_t value;
    int global_idx;
} fgls_cell;

/* ═══════════════════════════════════════════════════════════════════════════════
   Strategy Enum
   ═══════════════════════════════════════════════════════════════════════════════ */

typedef enum {
    FGLS_SEQUENTIAL  = 0,  // face*1000 + z*100 + y*10 + x
    FGLS_STRIDE37    = 1,  // global_idx * 37 % 20736
    FGLS_FACE_REGION = 2,  // face*3456 + z*345 + y*34 + x
    FGLS_GRID        = 3,  // (face*24+z)*144 + (y*10+x)
    FGLS_STRATEGY_COUNT = 4
} fgls_strategy;

/* ═══════════════════════════════════════════════════════════════════════════════
   Configuration
   ═══════════════════════════════════════════════════════════════════════════════ */

typedef struct {
    fgls_strategy strategy;       // Mapping strategy
    int use_geojump;              // 1 = apply GeoJump routing
    size_t max_bytes;             // Capacity hint
    uint64_t seed_gen2;           // GeoSeed gen2
    uint64_t seed_gen3;           // GeoSeed gen3
    uint64_t bundle;              // Bundle key for twin_bridge
} fgls_config;

/* ═══════════════════════════════════════════════════════════════════════════════
   Context (opaque)
   ═══════════════════════════════════════════════════════════════════════════════ */

typedef struct fgls_ctx fgls_ctx;

/* ═══════════════════════════════════════════════════════════════════════════════
   Public API
   ═══════════════════════════════════════════════════════════════════════════════ */

/* Initialize pipeline. Returns NULL on failure. */
fgls_ctx *fgls_init(const fgls_config *cfg);

/* Free pipeline resources. */
void fgls_free(fgls_ctx *ctx);

/* Encode contour cells into pipeline.
 * cells: array of fgls_cell (must have valid face,x,y,z,value,global_idx)
 * n: number of cells (should be FGLS_CELLS = 6000)
 * Returns: 0 on success, -1 on error. */
int fgls_encode(fgls_ctx *ctx, const fgls_cell *cells, int n);

/* Decode from pipeline back to cells.
 * out: pre-allocated array of size max_out
 * max_out: maximum cells to decode
 * Returns: number of cells decoded, -1 on error. */
int fgls_decode(fgls_ctx *ctx, fgls_cell *out, int max_out);

/* Get single cell value (O(1) lookup). */
int8_t fgls_get(const fgls_ctx *ctx, int face, int x, int y, int z);

/* Set single cell value (O(1) update). */
void fgls_set(fgls_ctx *ctx, int face, int x, int y, int z, int8_t val);

/* Run benchmark. Returns average ns/op or -1 on error. */
double fgls_benchmark(fgls_ctx *ctx, int iterations);

/* Print pipeline stats. */
void fgls_stats(fgls_ctx *ctx, FILE *fp);

/* Default config helper. */
static inline fgls_config fgls_default_config(void) {
    fgls_config cfg = {
        .strategy = FGLS_STRIDE37,
        .use_geojump = 1,
        .max_bytes = 64 * 1024 * 1024,
        .seed_gen2 = 0x9E3779B97F4A7C15ULL,
        .seed_gen3 = 0,
        .bundle = 0,
    };
    return cfg;
}

/* Strategy name lookup. */
const char *fgls_strategy_name(fgls_strategy s);

#ifdef __cplusplus
}
#endif

#endif /* FGLS_PIPELINE_H */

/* ═══════════════════════════════════════════════════════════════════════════════
   IMPLEMENTATION
   ═══════════════════════════════════════════════════════════════════════════════ */

#ifdef FGLS_PIPELINE_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Include all required headers */
#define CONTOUR_CODEC_IMPLEMENTATION
#include "contour_codec_20736.h"
#include "../../collection/dgls/geo/include/geo_jump.h"
#include "../../collection/dgls/geo/include/geo_frame_seek.h"
#include "../../collection/dgls/geo/include/geo_thirdeye.h"
#include "../../collection/dgls/geo/include/geo_config.h"

/* ═══════════════════════════════════════════════════════════════════════════════
   Internal Context
   ═══════════════════════════════════════════════════════════════════════════════ */

struct fgls_ctx {
    fgls_config config;
    codec_ctx *codec;           // Contour codec (flat array path)
    int initialized;
};

/* ═══════════════════════════════════════════════════════════════════════════════
   Implementation
   ═══════════════════════════════════════════════════════════════════════════════ */

const char *fgls_strategy_name(fgls_strategy s) {
    static const char *names[] = { "sequential", "stride37", "face_region", "grid" };
    if (s >= 0 && s < FGLS_STRATEGY_COUNT) return names[s];
    return "unknown";
}

fgls_ctx *fgls_init(const fgls_config *cfg) {
    if (!cfg) return NULL;
    
    fgls_ctx *ctx = (fgls_ctx *)calloc(1, sizeof(fgls_ctx));
    if (!ctx) return NULL;
    
    ctx->config = *cfg;
    
    // Initialize contour codec
    CODEC_STRATEGY codec_strat = (CODEC_STRATEGY)cfg->strategy;
    ctx->codec = codec_create(codec_strat);
    if (!ctx->codec) {
        free(ctx);
        return NULL;
    }
    
    ctx->initialized = 1;
    return ctx;
}

void fgls_free(fgls_ctx *ctx) {
    if (!ctx) return;
    
    if (ctx->codec) codec_free(ctx->codec);
    free(ctx);
}

int fgls_encode(fgls_ctx *ctx, const fgls_cell *cells, int n) {
    if (!ctx || !ctx->initialized || !cells || n <= 0) return -1;
    
    // Convert fgls_cell to contour_cell
    contour_cell *cc = (contour_cell *)malloc(n * sizeof(contour_cell));
    if (!cc) return -1;
    
    for (int i = 0; i < n; i++) {
        cc[i].face = cells[i].face;
        cc[i].x = cells[i].x;
        cc[i].y = cells[i].y;
        cc[i].z = cells[i].z;
        cc[i].value = cells[i].value;
        cc[i].global_idx = cells[i].global_idx;
    }
    
    int collisions = codec_encode(ctx->codec, cc, n);
    free(cc);
    return collisions;
}

int fgls_decode(fgls_ctx *ctx, fgls_cell *out, int max_out) {
    if (!ctx || !ctx->initialized || !out || max_out <= 0) return -1;
    
    contour_cell *decoded = (contour_cell *)malloc(max_out * sizeof(contour_cell));
    if (!decoded) return -1;
    
    int n = codec_decode(ctx->codec, decoded, max_out);
    if (n < 0) {
        free(decoded);
        return -1;
    }
    
    for (int i = 0; i < n; i++) {
        out[i].face = decoded[i].face;
        out[i].x = decoded[i].x;
        out[i].y = decoded[i].y;
        out[i].z = decoded[i].z;
        out[i].value = decoded[i].value;
        out[i].global_idx = decoded[i].global_idx;
    }
    
    free(decoded);
    return n;
}

int8_t fgls_get(const fgls_ctx *ctx, int face, int x, int y, int z) {
    if (!ctx || !ctx->initialized) return 0;
    return codec_get(ctx->codec, face, x, y, z);
}

void fgls_set(fgls_ctx *ctx, int face, int x, int y, int z, int8_t val) {
    if (!ctx || !ctx->initialized) return;
    codec_set(ctx->codec, face, x, y, z, val);
}

double fgls_benchmark(fgls_ctx *ctx, int iterations) {
    if (!ctx || !ctx->initialized || iterations <= 0) return -1;
    
    // Generate test data
    fgls_cell cells[FGLS_CELLS];
    for (int i = 0; i < FGLS_CELLS; i++) {
        int face, x, y, z;
        int face_cells = FGLS_W * FGLS_H * FGLS_L;
        face = i / face_cells;
        int rem = i % face_cells;
        z = rem / (FGLS_W * FGLS_H);
        rem %= (FGLS_W * FGLS_H);
        y = rem / FGLS_W;
        x = rem % FGLS_W;
        cells[i].face = face;
        cells[i].x = x;
        cells[i].y = y;
        cells[i].z = z;
        cells[i].value = (int8_t)((i * 37 + 13) % 256 - 128);
        if (cells[i].value == 0) cells[i].value = 1;
        cells[i].global_idx = i;
    }
    
    // Encode once
    fgls_encode(ctx, cells, FGLS_CELLS);
    
    // Benchmark decode
    uint64_t start = 0, end = 0;
#ifdef _WIN32
    // Windows high-res timer without including windows.h
    typedef struct { long long QuadPart; } LARGE_INTEGER;
    __declspec(dllimport) int __stdcall QueryPerformanceFrequency(LARGE_INTEGER *);
    __declspec(dllimport) int __stdcall QueryPerformanceCounter(LARGE_INTEGER *);
    LARGE_INTEGER freq, s, e;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&s);
#else
    struct timespec s, e;
    clock_gettime(CLOCK_MONOTONIC, &s);
#endif
    
    fgls_cell out[FGLS_CELLS];
    for (int i = 0; i < iterations; i++) {
        fgls_decode(ctx, out, FGLS_CELLS);
    }
    
#ifdef _WIN32
    QueryPerformanceCounter(&e);
    double ns = (double)(e.QuadPart - s.QuadPart) * 1e9 / freq.QuadPart;
#else
    clock_gettime(CLOCK_MONOTONIC, &e);
    double ns = (e.tv_sec - s.tv_sec) * 1e9 + (e.tv_nsec - s.tv_nsec);
#endif
    
    return ns / (iterations * FGLS_CELLS);
}

void fgls_stats(fgls_ctx *ctx, FILE *fp) {
    if (!ctx || !fp) return;
    
    fprintf(fp, "╔═══════════════════════════════════════════════════════════════════╗\n");
    fprintf(fp, "║  FGLS Pipeline Stats                                           ║\n");
    fprintf(fp, "╠═══════════════════════════════════════════════════════════════════╣\n");
    fprintf(fp, "║  Strategy: %s (%d)                                             ║\n", 
            fgls_strategy_name(ctx->config.strategy), ctx->config.strategy);
    fprintf(fp, "║  GeoJump:  %s                                                    ║\n",
            ctx->config.use_geojump ? "enabled" : "disabled");
    fprintf(fp, "║  Cells:    %d (%d faces × %d×%d×%d)                            ║\n",
            FGLS_CELLS, FGLS_FACES, FGLS_W, FGLS_H, FGLS_L);
    fprintf(fp, "║  Geo Space: %d (%d×%d)                                         ║\n",
            FGLS_GEO_FULL, FGLS_GEO_DIM, FGLS_GEO_DIM);
    fprintf(fp, "║  Utilization: %.1f%%                                           ║\n",
            100.0 * FGLS_CELLS / FGLS_GEO_FULL);
    fprintf(fp, "╚═══════════════════════════════════════════════════════════════════╝\n");
}

#endif /* FGLS_PIPELINE_IMPLEMENTATION */