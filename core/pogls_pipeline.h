/*
 * pogls_pipeline.h — Full POGLS Encode/Decode Pipeline
 * ════════════════════════════════════════════════════════════════════════
 *
 * Complete stack in one header:
 *
 *   ENCODE:
 *     data → 64B chunks → ChunkDesc (Approach B: positional keys)
 *       HOT  → tgw_fgls_store_raw() → FGLS geometric storage
 *       COLD → ColdStore ring (144 slots) → overflow → flat array
 *     → POGLSHeader (30B)
 *
 *   DECODE:
 *     POGLSHeader + chunk_idx → pgfe_reconstruct() → ChunkDesc
 *       HOT  → FGLS lookup by geo_key
 *       COLD → cold_find(bond_key) → raw 64B
 *
 *   RESIDUAL STORE (ColdStore):
 *     Primary:  144-slot ring (BermudaShadowRing, sacred)
 *   Overflow: hash table (open addressing, linear probe), caller-supplied
 *     Lookup:   O(1) hint → O(144) worst case ring, O(1) average overflow
 *
 *   Integration points (caller must supply real implementations):
 *     tgw_fgls_connector.h  → TgwFglsCtx, tgw_fgls_store_raw()
 *     bermuda_shadow.h      → BermudaShadowRing (144 slots, frozen)
 *     pogls_geofield_export.h → POGLSHeader, ChunkDesc, pgfe_*
 *
 * No malloc in hot path. No float. O(1) per chunk.
 * Sacred: RING=144, CYCLE=1440, STRIDE=37, GRID_W=27. FROZEN.
 * ════════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_PIPELINE_H
#define POGLS_PIPELINE_H

#include <stdint.h>
#include <string.h>
#include "pogls_geofield_export.h"

/* ════════════════════════════════════════════════════════════════
   COLD STORE — two-tier residual storage
   Tier 1: 144-slot ring (O(1) hint + O(144) scan)
   Tier 2: hash table (open addressing, linear probe, O(1) avg)
   ════════════════════════════════════════════════════════════════ */

#define POGLS_COLD_RING_CAP  144u   /* 2^4x3^2, sacred — FROZEN        */
#define POGLS_COLD_ENTRY_SZ   72u   /* bond_key(8) + chunk(64)         */

typedef struct {
    uint64_t bond_key;
    uint8_t  data[PGFE_CHUNK_SZ];
} ColdEntry;

_Static_assert(sizeof(ColdEntry) == PGFE_RESIDUAL_COLD, "ColdEntry must be 72B");

typedef struct {
    ColdEntry entries[POGLS_COLD_RING_CAP];
    uint8_t   head;
    uint8_t   count;
    uint32_t  evictions;
    uint32_t  total_stored;
    /* overflow hash table — caller allocates both */
    ColdEntry *overflow_buf;       /* [overflow_cap] entries         */
    uint8_t   *overflow_occ;       /* [overflow_cap] bytes, 1=occupied */
    uint32_t   overflow_cap;       /* MUST be power of 2             */
    uint32_t   overflow_count;
} ColdStore;

static inline void cold_store_init(ColdStore *s,
                                    ColdEntry *overflow_buf,
                                    uint32_t   overflow_cap)
{
    memset(s, 0, sizeof(*s));
    s->overflow_buf = overflow_buf;
    s->overflow_cap = overflow_cap;
    /* overflow_occ sits immediately after overflow_buf in memory */
    if (overflow_buf && overflow_cap > 0u) {
        s->overflow_occ = (uint8_t *)(overflow_buf + overflow_cap);
        memset(s->overflow_occ, 0, overflow_cap);
    }
}

/* max linear probe distance — at 75% load, avg cluster ≈ 4 */
#define POGLS_COLD_PROBE_MAX  32u

/* bond_key is already well-distributed */
static inline uint32_t _cold_hash(uint32_t mask, uint64_t bond_key)
{
    return (uint32_t)(bond_key ^ (bond_key >> 32u)) & mask;
}

static inline void cold_push(ColdStore       *s,
                              uint64_t         bond_key,
                              const uint8_t   *chunk64)
{
    if (s->count == POGLS_COLD_RING_CAP) {
        /* evict oldest → hash table (only if under 75% load) */
        uint32_t cap75 = s->overflow_cap - (s->overflow_cap >> 2u);
        if (s->overflow_buf && s->overflow_occ &&
            s->overflow_count < cap75)
        {
            uint32_t mask = s->overflow_cap - 1u;
            uint32_t slot = _cold_hash(mask, s->entries[s->head].bond_key);
            while (s->overflow_occ[slot])
                slot = (slot + 1u) & mask;
            s->overflow_buf[slot] = s->entries[s->head];
            s->overflow_occ[slot] = 1u;
            s->overflow_count++;
        }
        s->evictions++;
    } else {
        s->count++;
    }
    s->entries[s->head].bond_key = bond_key;
    memcpy(s->entries[s->head].data, chunk64, PGFE_CHUNK_SZ);
    s->head = (uint8_t)((s->head + 1u) % POGLS_COLD_RING_CAP);
    s->total_stored++;
}

static inline const uint8_t *cold_find(const ColdStore *s, uint64_t bond_key)
{
    if (s->count == 0u) return NULL;
    /* Ring: O(1) hint */
    uint8_t hint = (uint8_t)(bond_key % POGLS_COLD_RING_CAP);
    if (s->entries[hint].bond_key == bond_key)
        return s->entries[hint].data;
    /* Ring: O(144) scan */
    for (uint8_t i = 0u; i < POGLS_COLD_RING_CAP; i++)
        if (s->entries[i].bond_key == bond_key)
            return s->entries[i].data;
    /* Overflow hash table: O(1) avg, POGLS_COLD_PROBE_MAX bound */
    if (s->overflow_buf && s->overflow_occ && s->overflow_count > 0u) {
        uint32_t mask = s->overflow_cap - 1u;
        uint32_t slot = _cold_hash(mask, bond_key);
        for (uint32_t p = 0u; p < POGLS_COLD_PROBE_MAX; p++) {
            if (!s->overflow_occ[slot]) break;
            if (s->overflow_buf[slot].bond_key == bond_key)
                return s->overflow_buf[slot].data;
            slot = (slot + 1u) & mask;
        }
    }
    return NULL;
}

/* ════════════════════════════════════════════════════════════════
   HOT STORE — thin wrapper over tgw_fgls_store_raw()
   ════════════════════════════════════════════════════════════════ */

typedef int (*HotStoreFn)(void *ctx, uint64_t addr,
                           uint64_t value, uint8_t shape);

typedef struct {
    void       *ctx;
    HotStoreFn  store_fn;
    uint32_t    stored;
    uint32_t    errors;
} HotStore;

static inline void hot_store_init(HotStore *h, void *ctx, HotStoreFn fn)
{
    memset(h, 0, sizeof(*h));
    h->ctx      = ctx;
    h->store_fn = fn;
}

static inline int hot_push(HotStore *h, const ChunkDesc *d)
{
    if (!h->store_fn) return -1;
    int rc = h->store_fn(h->ctx, d->geo_key, d->bond_key, d->shape);
    if (rc == 0) h->stored++;
    else         h->errors++;
    return rc;
}

/* ════════════════════════════════════════════════════════════════
   PIPELINE CONTEXT — owns everything
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    POGLSHeader  header;
    HotStore     hot;
    ColdStore    cold;
    uint64_t     nonce;
    uint32_t     face_max;
    uint32_t     n_tokens;
    uint8_t      layer_count;
    uint64_t     chunks_encoded;
    uint64_t     chunks_decoded;
    uint64_t     hot_count;
    uint64_t     cold_count;
    uint8_t      header_ready;
} PoglsPipeline;

static inline void pogls_pipeline_init(PoglsPipeline *p,
                                        uint64_t       nonce,
                                        uint32_t       face_max,
                                        uint32_t       n_tokens,
                                        uint8_t        layer_count,
                                        void          *hot_ctx,
                                        HotStoreFn     hot_fn,
                                        ColdEntry     *cold_overflow,
                                        uint32_t       cold_overflow_cap)
{
    memset(p, 0, sizeof(*p));
    p->nonce       = nonce;
    p->face_max    = (face_max > 0u) ? face_max : PGFE_FACE_DEFAULT;
    p->n_tokens    = (n_tokens > 0u) ? n_tokens : 64u;
    p->layer_count = (layer_count > 0u) ? layer_count : 1u;
    hot_store_init(&p->hot, hot_ctx, hot_fn);
    cold_store_init(&p->cold, cold_overflow, cold_overflow_cap);
}

/* ════════════════════════════════════════════════════════════════
   ENCODE — one chunk at a time (streaming-friendly)
   Returns: PGFE_HOT(0) or PGFE_COLD(1), <0 on error
   ════════════════════════════════════════════════════════════════ */

static inline int pogls_encode_chunk(PoglsPipeline *p,
                                      const uint8_t *chunk64,
                                      uint64_t       chunk_idx)
{
    uint32_t tile_id = (uint32_t)(chunk_idx % p->face_max);
    uint8_t  dim     = (uint8_t)((chunk_idx / p->face_max) & 0x7Fu);

    uint64_t origin = p->header_ready ? p->header.origin_key : 0u;
    ChunkDesc d = pgfe_desc(chunk64, chunk_idx, tile_id, dim,
                             p->n_tokens, p->nonce, origin);

    if (!p->header_ready) {
        p->header = pgfe_build_header(&d, p->nonce, p->layer_count, 0u);
        p->header_ready = 1u;
    }

    p->chunks_encoded++;

    if (d.temperature == PGFE_HOT) {
        p->hot_count++;
        hot_push(&p->hot, &d);
        return PGFE_HOT;
    } else {
        p->cold_count++;
        cold_push(&p->cold, d.bond_key, chunk64);
        p->header.flags |= PGFE_FLAG_HAS_RESIDUAL;
        return PGFE_COLD;
    }
}

static inline int pogls_encode(PoglsPipeline  *p,
                                const uint8_t  *data,
                                size_t          data_sz)
{
    if (!data || data_sz == 0u) return -1;
    uint64_t n = (data_sz + PGFE_CHUNK_SZ - 1) / PGFE_CHUNK_SZ;
    for (uint64_t ci = 0; ci < n; ci++) {
        size_t  off = (size_t)(ci * PGFE_CHUNK_SZ);
        size_t  rem = (off < data_sz) ? data_sz - off : 0u;
        uint8_t chunk[PGFE_CHUNK_SZ] = {0};
        if (rem > 0u)
            memcpy(chunk, data + off,
                   rem < PGFE_CHUNK_SZ ? rem : PGFE_CHUNK_SZ);
        if (pogls_encode_chunk(p, chunk, ci) < 0) return -1;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════
   DECODE — one chunk at a time
   Returns: 1=HOT(use geo_key)  0=COLD(chunk written to out64)  -1=miss
   ════════════════════════════════════════════════════════════════ */

static inline int pogls_decode_chunk(PoglsPipeline *p,
                                      uint64_t       chunk_idx,
                                      uint8_t       *out64,
                                      uint64_t      *geo_key_out)
{
    ChunkDesc d = pgfe_reconstruct(&p->header, chunk_idx, p->face_max);
    if (geo_key_out) *geo_key_out = d.geo_key;

    /* Try cold store first (if header flagged or on any chunk) */
    const uint8_t *raw = cold_find(&p->cold, d.bond_key);
    if (raw) {
        if (out64) memcpy(out64, raw, PGFE_CHUNK_SZ);
        p->chunks_decoded++;
        return 0;   /* COLD */
    }

    /* Not in cold store → caller fetches from FGLS by geo_key */
    p->chunks_decoded++;
    return 1;       /* HOT */
}

/* ════════════════════════════════════════════════════════════════
   HEADER EXPORT / IMPORT
   ════════════════════════════════════════════════════════════════ */

static inline void pogls_write_header(const PoglsPipeline *p,
                                       uint8_t out[PGFE_HEADER_SZ])
{
    pgfe_header_write(&p->header, out);
}

static inline void pogls_load_header(PoglsPipeline       *p,
                                      const uint8_t        in[PGFE_HEADER_SZ])
{
    pgfe_header_read(in, &p->header);
    p->header_ready = 1u;
    p->nonce       = p->header.session_nonce;
    p->layer_count = p->header.layer_count;
    p->n_tokens    = (uint32_t)1u << (p->header.gear + 8u);
}

/* ════════════════════════════════════════════════════════════════
   STATS
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t encoded;
    uint64_t decoded;
    uint64_t hot;
    uint64_t cold;
    uint32_t hot_stored;
    uint32_t hot_errors;
    uint32_t cold_ring_count;
    uint32_t cold_overflow_count;
    uint32_t cold_evictions;
    uint8_t  header_ready;
} PipelineStats;

static inline PipelineStats pogls_stats(const PoglsPipeline *p)
{
    PipelineStats s;
    s.encoded             = p->chunks_encoded;
    s.decoded             = p->chunks_decoded;
    s.hot                 = p->hot_count;
    s.cold                = p->cold_count;
    s.hot_stored          = p->hot.stored;
    s.hot_errors          = p->hot.errors;
    s.cold_ring_count     = p->cold.count;
    s.cold_overflow_count = p->cold.overflow_count;
    s.cold_evictions      = p->cold.evictions;
    s.header_ready        = p->header_ready;
    return s;
}

/* ════════════════════════════════════════════════════════════════
   VERIFY — 8 tests
   ════════════════════════════════════════════════════════════════ */

static inline int pogls_pipeline_verify(void)
{
    /* T1: ColdEntry size */
    if (sizeof(ColdEntry) != PGFE_RESIDUAL_COLD) return -1;

    /* T2: ring push/find roundtrip */
    ColdStore cs;
    cold_store_init(&cs, NULL, 0u);
    uint8_t chunk[64]; memset(chunk, 0xAB, 64);
    cold_push(&cs, 0xDEADBEEFULL, chunk);
    const uint8_t *found = cold_find(&cs, 0xDEADBEEFULL);
    if (!found)              return -2;
    if (found[0] != 0xABu)  return -2;

    /* T3: ring miss */
    if (cold_find(&cs, 0xCAFEBABEULL)) return -3;

    /* T4: ring capacity */
    ColdStore cs2;
    cold_store_init(&cs2, NULL, 0u);
    for (uint32_t i = 0; i < POGLS_COLD_RING_CAP + 5u; i++) {
        uint8_t c[64]; memset(c, (uint8_t)i, 64);
        cold_push(&cs2, (uint64_t)i + 1u, c);
    }
    if (cs2.evictions != 5u)                return -4;
    if (cs2.count     != POGLS_COLD_RING_CAP) return -4;

    /* T5: overflow tier (75% load cap = 12 of 16) */
    uint8_t overflow_mem[sizeof(ColdEntry) * 16u + 16u];
    ColdStore cs3;
    cold_store_init(&cs3, (ColdEntry *)overflow_mem, 16u);
    for (uint32_t i = 0; i < POGLS_COLD_RING_CAP + 20u; i++) {
        uint8_t c[64]; memset(c, (uint8_t)(i+1u), 64);
        cold_push(&cs3, (uint64_t)i + 100u, c);
    }
    uint32_t cap75 = cs3.overflow_cap - (cs3.overflow_cap >> 2u);
    if (cs3.overflow_count != cap75) return -5;
    if (cs3.evictions  != 20u)       return -5;

    /* T6: hot_store null stub */
    HotStore hs;
    hot_store_init(&hs, NULL, NULL);
    ChunkDesc dummy; memset(&dummy, 0, sizeof(dummy));
    dummy.shape = 'I';
    if (hot_push(&hs, &dummy) != -1) return -6;

    /* T7: pipeline encode + decode roundtrip (HOT path) */
    PoglsPipeline pipe;
    pogls_pipeline_init(&pipe, 0xBEEF42ULL, 12u, 64u, 1u,
                         NULL, NULL, NULL, 0u);
    uint8_t hot_data[PGFE_CHUNK_SZ] = {0};
    int temp = pogls_encode_chunk(&pipe, hot_data, 0u);
    if (temp != PGFE_HOT) return -7;
    if (!pipe.header_ready) return -7;
    uint64_t gk = 0;
    int dr = pogls_decode_chunk(&pipe, 0u, NULL, &gk);
    if (dr != 1) return -7;                 /* HOT = not in cold store */
    /* geo_key must match encode */
    ChunkDesc ed = pgfe_desc(hot_data, 0u, 0u, 0u, 64u, 0xBEEF42ULL, 0u);
    if (gk != ed.geo_key) return -7;

    /* T8: pipeline encode + decode roundtrip (COLD path) */
    uint8_t cold_data[PGFE_CHUNK_SZ];
    for (int i = 0; i < 64; i++) cold_data[i] = (uint8_t)(i * 37u % 255u);
    int tc = pogls_encode_chunk(&pipe, cold_data, 1u);
    if (tc != PGFE_COLD) return -8;
    uint8_t recovered[PGFE_CHUNK_SZ] = {0};
    int dc = pogls_decode_chunk(&pipe, 1u, recovered, NULL);
    if (dc != 0) return -8;
    if (memcmp(recovered, cold_data, PGFE_CHUNK_SZ) != 0) return -8;

    return 0;
}

#endif /* POGLS_PIPELINE_H */
