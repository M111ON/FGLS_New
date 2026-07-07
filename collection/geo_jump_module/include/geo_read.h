#pragma once
#include <stdint.h>
#include <string.h>

#include "theta_map.h"
#include "geo_route.h"
#include "geo_dodeca.h"
#include "geo_diamond_field.h"
#include "pogls_fold.h"

typedef enum {
    READ_HIT_EXACT  = 0,
    READ_HIT_MERKLE = 1,
    READ_HIT_CELL   = 2,
    READ_MISS       = 3,
} ReadStatus;

typedef struct {
    ReadStatus status;
    uint64_t   merkle_root;
    uint16_t   hop_count;
    uint8_t    segment;
    uint8_t    offset;
    uint32_t   ref_count;
    int        sha_ok;
} ReadResult;

static inline ReadResult read_result_miss(void) {
    ReadResult r;
    memset(&r, 0, sizeof(r));
    r.status = READ_MISS;
    r.sha_ok = 0;
    return r;
}

static inline ReadResult geo_read_by_addr(DodecaTable       *dodeca,
                                           uint64_t           route_addr,
                                           uint8_t            offset,
                                           const DiamondSHA  *sha)
{
    DodecaEntry *entry = NULL;
    DodecaResult dr = dodeca_lookup(dodeca, route_addr, offset, &entry);

    if (dr == DODECA_MISS || entry == NULL)
        return read_result_miss();

    ReadResult r;
    r.merkle_root = entry->merkle_root;
    r.hop_count   = entry->hop_count;
    r.segment     = entry->segment;
    r.offset      = entry->offset;
    r.ref_count   = entry->ref_count;
    r.sha_ok      = dodeca_verify(entry, sha);
    r.status      = (dr == DODECA_HIT_EXACT) ? READ_HIT_EXACT : READ_HIT_MERKLE;
    return r;
}

static inline ReadResult geo_read_by_raw(DodecaTable       *dodeca,
                                          const uint64_t    *cells_raw,
                                          uint32_t           n,
                                          uint64_t           baseline,
                                          const DiamondSHA  *sha)
{
    if (n == 0) return read_result_miss();

    DiamondFlowCtx ctx; diamond_flow_init(&ctx);

#   define _GEO_READ_TOPO_MASK UINT64_C(0x0000FFFFFFFFFFFF)
#   define _GEO_READ_PHI       UINT64_C(0x9E3779B97F4A7C15)

    for (uint32_t i = 0; i < n; i++) {
        uint64_t h  = theta_mix64(cells_raw[i]);
        uint32_t hi = (uint32_t)(h >> 32);
        uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);
        uint8_t  face = (uint8_t)(((uint64_t)hi * 12u) >> 32);
        uint8_t  edge = (uint8_t)(((uint64_t)lo *  5u) >> 32);

        uint64_t core_raw = ((uint64_t)face << 59)
                          | ((uint64_t)edge << 52)
                          | (cells_raw[i] & UINT64_C(0x000FFFFFFFFFFFFF));

        uint64_t r8  = (core_raw >> 8)  | (core_raw << 56);
        uint64_t r16 = (core_raw >> 16) | (core_raw << 48);
        uint64_t r24 = (core_raw >> 24) | (core_raw << 40);
        uint64_t isect = core_raw & r8 & r16 & r24;

        if ((core_raw & 7u) == 0u)
            ctx.drift_acc += (uint32_t)__builtin_popcountll(baseline & ~isect);

        uint64_t r_next = diamond_route_update(ctx.route_addr, isect);
        if ((r_next & _GEO_READ_TOPO_MASK) == 0)
            r_next ^= _GEO_READ_PHI ^ isect;

        int at_end = (isect == 0) || (ctx.drift_acc > 72u)
                  || (ctx.hop_count >= DIAMOND_HOP_MAX);

        if (at_end) {
            uint8_t offset = (uint8_t)(__builtin_popcountll(baseline & ~isect) & 0xFF);
            ReadResult r = geo_read_by_addr(dodeca, r_next, offset, sha);
            if (r.status != READ_MISS) return r;
            ctx.route_addr = 0;
            ctx.hop_count  = 0;
            ctx.drift_acc  = 0;
            continue;
        }

        ctx.route_addr = r_next;
        ctx.hop_count++;
    }

    if (ctx.route_addr != 0) {
        uint8_t offset = (uint8_t)(ctx.drift_acc & 0xFF);
        ReadResult r = geo_read_by_addr(dodeca, ctx.route_addr, offset, sha);
        if (r.status != READ_MISS) return r;
    }

#   undef _GEO_READ_TOPO_MASK
#   undef _GEO_READ_PHI

    return read_result_miss();
}

static inline ReadResult geo_read_by_cell(const DiamondBlock *b,
                                           DodecaTable        *dodeca,
                                           const DiamondSHA   *sha)
{
    HoneycombSlot s = honeycomb_read(b);

    if (s.merkle_root == 0 && s.dna_count == 0)
        return read_result_miss();

    ReadResult r;
    r.merkle_root = s.merkle_root;
    r.hop_count   = s.dna_count;
    r.offset      = s.reserved[0];
    r.segment     = 0;
    r.ref_count   = 0;
    r.sha_ok      = 0;
    r.status      = READ_HIT_CELL;

    if (dodeca) {
        DodecaEntry *entry = NULL;
        DodecaResult dr = dodeca_lookup(dodeca, s.merkle_root,
                                         s.reserved[0], &entry);
        if (dr != DODECA_MISS && entry) {
            r.segment   = entry->segment;
            r.ref_count = entry->ref_count;
            r.sha_ok    = dodeca_verify(entry, sha);
            r.status    = (dr == DODECA_HIT_EXACT)
                        ? READ_HIT_EXACT : READ_HIT_MERKLE;
        }
    }

    return r;
}

static inline uint32_t geo_read_scan(const DiamondBlock *cells,
                                      uint32_t            n,
                                      DodecaTable        *dodeca,
                                      const DiamondSHA   *sha,
                                      ReadResult         *results)
{
    uint32_t found = 0;
    for (uint32_t i = 0; i < n; i++) {
        results[i] = geo_read_by_cell(&cells[i], dodeca, sha);
        if (results[i].status != READ_MISS) found++;
    }
    return found;
}
