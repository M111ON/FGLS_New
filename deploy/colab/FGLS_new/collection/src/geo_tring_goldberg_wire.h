#ifndef GEO_TRING_GOLDBERG_WIRE_H
#define GEO_TRING_GOLDBERG_WIRE_H

#include <stdint.h>
#include <string.h>
#include "../core/core/geo_temporal_ring.h"
#include "../core/core/geo_tring_stream.h"

#define GEO_BUNDLE_WORDS 8u
#define GEO_SLOTS 576u
#define GEO_BLOCK_BOUNDARY 288u
#define GEOMATRIX_PATHS 18

typedef struct {
    uint64_t gen2;
    uint64_t gen3;
} GeoSeed;

typedef struct {
    uint8_t phase;
    uint64_t sig;
    uint16_t hpos;
    uint16_t idx;
    uint8_t bit;
} GeoPacket;

typedef struct {
    uint32_t x;
} GeomatrixStatsV3;

typedef struct {
    uint32_t stamp_hash;
    uint32_t spatial_xor;
    uint64_t window_id;
    uint8_t  circuit_fired;
    uint16_t tring_start;
    uint16_t tring_end;
    uint16_t tring_span;
    uint16_t top_gaps[4];
    uint8_t  blueprint_ready;
} GBBlueprint;

typedef struct {
    uint8_t blueprint_ready;
    GBBlueprint bp;
} GoldbergPipelineResult;

typedef struct {
    GoldbergPipelineResult gpr;
    struct { uint8_t flags; } ev;
} TGWResult;

typedef struct {
    TRingCtx          tr;
    TStreamChunk      store[TSTREAM_MAX_PKTS];
    uint32_t          total_writes;
    uint32_t          blueprint_count;
    uint32_t          stream_pkts_rx;
    uint32_t          stream_gaps;
    uint64_t          _bundle[GEO_BUNDLE_WORDS];
} TGWCtx;

static inline uint64_t geo_compute_sig64(const uint64_t *bundle, uint8_t phase)
{
    uint64_t fold = (uint64_t)phase;
    for (uint32_t i = 0; i < GEO_BUNDLE_WORDS; i++) fold ^= bundle[i];
    return fold;
}

static inline int geomatrix_batch_verdict(GeoPacket *batch,
                                          const uint64_t *bundle,
                                          GeomatrixStatsV3 *stats)
{
    (void)batch;
    (void)bundle;
    (void)stats;
    return 1;
}

static inline void tgw_init(TGWCtx *ctx, GeoSeed seed, const uint64_t *bundle)
{
    (void)seed;
    memset(ctx, 0, sizeof(*ctx));
    tring_init(&ctx->tr);
    if (bundle) memcpy(ctx->_bundle, bundle, GEO_BUNDLE_WORDS * sizeof(uint64_t));
}

static inline TGWResult tgw_write(TGWCtx *ctx,
                                  uint64_t addr,
                                  uint64_t value,
                                  uint8_t slot_hot)
{
    (void)addr;
    (void)value;
    (void)slot_hot;
    TGWResult r;
    memset(&r, 0, sizeof(r));
    ctx->total_writes++;
    return r;
}

#endif
