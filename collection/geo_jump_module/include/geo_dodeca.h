#pragma once
#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t route_addr;
    uint16_t hop_count;
    uint16_t _pad;
    uint32_t drift_acc;
    uint32_t drift_threshold;
} DiamondFlowCtxV2;

static inline void diamond_flow_init_v2(DiamondFlowCtxV2 *ctx,
                                         uint64_t baseline)
{
    ctx->route_addr      = 0;
    ctx->hop_count       = 0;
    ctx->_pad            = 0;
    ctx->drift_acc       = 0;
    uint32_t pc = (uint32_t)__builtin_popcountll(baseline);
    ctx->drift_threshold = (pc >> 1) < 8u ? 8u : (pc >> 1);
}

static inline void diamond_ctx_branch(const DiamondFlowCtxV2 *src,
                                       DiamondFlowCtxV2       *dst)
{
    *dst = *src;
}

typedef struct {
    uint64_t merkle_root;
    uint64_t sha256_hi;
    uint64_t sha256_lo;
    uint8_t  offset;
    uint16_t hop_count;
    uint8_t  segment;
    uint32_t ref_count;
} DodecaEntry;

#ifndef DODECA_TABLE_SIZE
#  define DODECA_TABLE_SIZE 144u
#endif

typedef struct {
    DodecaEntry slots[DODECA_TABLE_SIZE];
    uint32_t    count;
    uint32_t    hit_count;
    uint32_t    miss_count;
} DodecaTable;

static inline void dodeca_init(DodecaTable *t) {
    memset(t, 0, sizeof(*t));
}

static inline uint32_t dodeca_slot(uint64_t merkle_root) {
    return (uint32_t)((merkle_root * 0x9e3779b97f4a7c15ULL) >> 32)
           % DODECA_TABLE_SIZE;
}

typedef enum {
    DODECA_HIT_EXACT  = 0,
    DODECA_HIT_MERKLE = 1,
    DODECA_MISS       = 2,
} DodecaResult;
#define DODECA_HIT DODECA_HIT_EXACT

static inline DodecaResult dodeca_lookup(DodecaTable        *t,
                                          uint64_t            merkle_root,
                                          uint8_t             offset,
                                          DodecaEntry       **out_entry)
{
    uint32_t idx = dodeca_slot(merkle_root);
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t s = (idx + i) % DODECA_TABLE_SIZE;
        DodecaEntry *e = &t->slots[s];
        if (e->ref_count == 0) break;
        if (e->merkle_root == merkle_root) {
            e->ref_count++;
            *out_entry = e;
            t->hit_count++;
            if (e->offset == offset) return DODECA_HIT_EXACT;
            return DODECA_HIT_MERKLE;
        }
    }
    t->miss_count++;
    *out_entry = NULL;
    return DODECA_MISS;
}

static inline DodecaEntry *dodeca_insert(DodecaTable *t,
                                          uint64_t     merkle_root,
                                          uint64_t     sha256_hi,
                                          uint64_t     sha256_lo,
                                          uint8_t      offset,
                                          uint16_t     hop_count,
                                          uint8_t      segment)
{
    uint32_t idx = dodeca_slot(merkle_root);
    DodecaEntry *target = NULL;
    uint32_t min_ref = UINT32_MAX;

    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t s = (idx + i) % DODECA_TABLE_SIZE;
        DodecaEntry *e = &t->slots[s];
        if (e->ref_count == 0) { target = e; break; }
        if (e->ref_count < min_ref) { min_ref = e->ref_count; target = e; }
    }
    if (!target) return NULL;

    target->merkle_root = merkle_root;
    target->sha256_hi   = sha256_hi;
    target->sha256_lo   = sha256_lo;
    target->offset      = offset;
    target->hop_count   = hop_count;
    target->segment     = segment;
    target->ref_count   = 1;

    if (t->count < DODECA_TABLE_SIZE) t->count++;
    return target;
}

typedef struct {
    uint64_t hi;
    uint64_t lo;
} DiamondSHA;

static inline DiamondSHA diamond_sha_from_raw(const uint8_t sha256_raw[32])
{
    DiamondSHA s;
    s.hi = ((uint64_t)sha256_raw[0]  << 56) | ((uint64_t)sha256_raw[1]  << 48)
         | ((uint64_t)sha256_raw[2]  << 40) | ((uint64_t)sha256_raw[3]  << 32)
         | ((uint64_t)sha256_raw[4]  << 24) | ((uint64_t)sha256_raw[5]  << 16)
         | ((uint64_t)sha256_raw[6]  <<  8) |  (uint64_t)sha256_raw[7];
    s.lo = ((uint64_t)sha256_raw[8]  << 56) | ((uint64_t)sha256_raw[9]  << 48)
         | ((uint64_t)sha256_raw[10] << 40) | ((uint64_t)sha256_raw[11] << 32)
         | ((uint64_t)sha256_raw[12] << 24) | ((uint64_t)sha256_raw[13] << 16)
         | ((uint64_t)sha256_raw[14] <<  8) |  (uint64_t)sha256_raw[15];
    return s;
}

static inline int diamond_sha_is_zero(const DiamondSHA *s)
{
    return (s->hi == 0) && (s->lo == 0);
}

static inline int dodeca_verify(const DodecaEntry  *e,
                                 const DiamondSHA   *sha)
{
    if (!sha) return 0;
    if (e->sha256_hi == 0 && e->sha256_lo == 0) return 0;
    if (e->sha256_hi == sha->hi && e->sha256_lo == sha->lo) return  1;
    return -1;
}

static inline DodecaEntry *dodeca_insert_sha(DodecaTable      *t,
                                               uint64_t          merkle_root,
                                               const DiamondSHA *sha,
                                               uint8_t           offset,
                                               uint16_t          hop_count,
                                               uint8_t           segment)
{
    uint64_t hi = sha ? sha->hi : 0;
    uint64_t lo = sha ? sha->lo : 0;
    return dodeca_insert(t, merkle_root, hi, lo, offset, hop_count, segment);
}

static inline void dodeca_fill_sha(DodecaEntry    *e,
                                    const DiamondSHA *sha)
{
    if (!e || !sha) return;
    e->sha256_hi = sha->hi;
    e->sha256_lo = sha->lo;
}
