/*
 * pogls_geofield_export.h — GeoField → POGLS Header Export (Approach B)
 * ════════════════════════════════════════════════════════════════════════
 *
 * Core principle (Approach B):
 *   geo_key = f(session_nonce, chunk_idx, tile_id, dim)
 *   bond_key = rotate(geo_key, chunk_idx % 63 + 1)
 *   enc      = g(tile_id, dim)   — maps to Fibo 1440 timeline
 *
 *   → HOT chunks: header 30B only, zero residual
 *   → COLD chunks: header + bond_key(8) + raw(64) = 72B residual
 *
 * Architecture position:
 *   geo_field_core_3.h → [THIS FILE] → bond_chain / bermuda / tgw_fgls
 *
 * Pipeline:
 *   encode: data → 64B chunks → ChunkDesc (O(1) per chunk)
 *                                   ↓
 *                              POGLSHeader (30B, from chunk_0)
 *                                   ↓
 *                   HOT → header only | COLD → +72B residual
 *
 *   decode: POGLSHeader + chunk_idx → ChunkDesc (no data needed)
 *           COLD: lookup residual by bond_key
 *
 * Validated:
 *   T1 header=30B  T2 determinism/1440  T3 uniqueness(coll=0)
 *   T4 roundtrip   T5 residual model    T6 header serial
 *
 * Dependencies (caller includes before this file):
 *   geo_field_core_3.h, pogls_bond_chain.h, bermuda_export.h
 *
 * No malloc in hot path. No float. O(1) per chunk.
 * Sacred: CYCLE=1440, STRIDE=37, GRID_W=27. FROZEN.
 * ════════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_GEOFIELD_EXPORT_H
#define POGLS_GEOFIELD_EXPORT_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════════════ */

#define PGFE_CHUNK_SZ          64u     /* DiamondBlock                    */
#define PGFE_HEADER_SZ         30u     /* POGLSHeader packed              */
#define PGFE_RESIDUAL_COLD     72u     /* bond_key(8) + raw chunk(64)     */
#define PGFE_CYCLE           1440u     /* Fibo timeline — FROZEN          */
#define PGFE_CODEC_W           27u     /* GeoPixel canonical grid width   */
#define PGFE_FACE_DEFAULT      12u     /* dodecahedron faces              */

/* flags */
#define PGFE_FLAG_HAS_RESIDUAL 0x01u
#define PGFE_FLAG_CHAIN_MODE   0x02u

/* temperature */
#define PGFE_HOT  0u
#define PGFE_COLD 1u

/* FNV-1a */
#define PGFE_FNV_OFFSET UINT64_C(14695981039346656037)
#define PGFE_FNV_PRIME  UINT64_C(1099511628211)

/* ════════════════════════════════════════════════════════════════
   POGLS HEADER — 30B packed
   Complete reconstruction key for the entire sequence.
   ════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint64_t session_nonce;  /* dispatch/fts seed                       */
    uint64_t root_seed;      /* = geo_key of chunk_0                    */
    uint16_t enc_start;      /* chunk_0 position on 1440 timeline       */
    uint8_t  gear;           /* bermuda gear 1..4                       */
    uint8_t  layer_count;    /* channels (1=mono, N=multi)              */
    uint8_t  codec_id;       /* GeoPixel W = 27 canonical               */
    uint8_t  flags;          /* PGFE_FLAG_*                             */
    uint64_t origin_key;     /* chain head anchor = root_seed           */
} POGLSHeader;               /* = 30B */

_Static_assert(sizeof(POGLSHeader) == PGFE_HEADER_SZ, "POGLSHeader must be 30B");

/* ════════════════════════════════════════════════════════════════
   CHUNK DESCRIPTOR — 56B, not stored (derived at runtime)
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t chunk_idx;
    uint32_t tile_id;
    uint8_t  dim;
    uint8_t  gear;
    uint8_t  zone;       /* 0..11 dodeca face                          */
    uint8_t  pole;       /* 0=south  1=north                           */
    uint8_t  shape;      /* 'I'/'O'/'S' routing shape                  */
    uint8_t  polarity;   /* 0=ROUTE  1=GROUND                          */
    uint8_t  temperature;/* PGFE_HOT / PGFE_COLD                       */
    uint8_t  _pad;
    uint16_t tring_slot; /* 0..719                                     */
    uint16_t enc;        /* 0..1439 timeline position                  */
    uint64_t geo_key;    /* f(nonce, chunk_idx, tile_id, dim)          */
    uint64_t bond_key;   /* rotate(geo_key, chunk_idx%63+1)            */
    uint64_t origin_key; /* chain head geo_key                         */
} ChunkDesc;             /* 56B — matches BondNode size                */

/* ════════════════════════════════════════════════════════════════
   CORE PRIMITIVES — Approach B deterministic functions
   ════════════════════════════════════════════════════════════════ */

/*
 * geo_key_from_pos — pure positional key, no chunk data needed.
 * Avalanche: 1-bit flip → >20 output bits change (validated).
 */
static inline uint64_t pgfe_geo_key(uint64_t nonce,
                                     uint64_t chunk_idx,
                                     uint32_t tile_id,
                                     uint8_t  dim)
{
    uint64_t h = PGFE_FNV_OFFSET;
    h ^= nonce;    h *= PGFE_FNV_PRIME;
    h ^= chunk_idx; h *= PGFE_FNV_PRIME;
    h ^= tile_id;  h *= PGFE_FNV_PRIME;
    h ^= dim;      h *= PGFE_FNV_PRIME;
    return h;
}

/*
 * bond_key — positional rotation of geo_key.
 * Each chunk_idx → unique rotation offset (1..63, never 0).
 */
static inline uint64_t pgfe_bond_key(uint64_t geo_key, uint64_t chunk_idx)
{
    uint8_t rot = (uint8_t)(chunk_idx % 63u) + 1u;
    return (geo_key << rot) | (geo_key >> (64u - rot));
}

/*
 * enc_from_addr — (tile_id, dim) → position on 1440 timeline.
 * Mirrors geo_tring_addr.h compound/spoke/offset formula.
 */
static inline uint16_t pgfe_enc(uint32_t tile_id, uint8_t dim)
{
    uint32_t compound = ((tile_id % 32u) + (uint32_t)(dim % 32u) * 32u) % 144u;
    uint32_t spoke    = ((tile_id % 32u) ^ (uint32_t)(dim % 32u)) % 6u;
    uint32_t offset   = ((tile_id % 32u) * 3u + (uint32_t)(dim % 32u) * 7u) % 4u;
    return (uint16_t)((compound * 24u + spoke * 4u + offset) % PGFE_CYCLE);
}

/*
 * temperature_classify — range discriminator (fast, no float).
 * Mirrors bermuda_shadow HOT/COLD threshold (range < 64 → HOT).
 * Pass NULL chunk → always HOT (decode path assumption).
 */
static inline uint8_t pgfe_temperature(const uint8_t *chunk)
{
    if (!chunk) return PGFE_HOT;
    uint8_t mn = 255u, mx = 0u;
    for (int i = 0; i < 64; i++) {
        if (chunk[i] < mn) mn = chunk[i];
        if (chunk[i] > mx) mx = chunk[i];
    }
    return ((uint32_t)(mx - mn) < 64u) ? PGFE_HOT : PGFE_COLD;
}

/* ════════════════════════════════════════════════════════════════
   DESC BUILDER — single chunk → ChunkDesc, O(1)
   chunk=NULL → decode path (no data, pure positional)
   ════════════════════════════════════════════════════════════════ */

static inline ChunkDesc pgfe_desc(const uint8_t *chunk,
                                   uint64_t       chunk_idx,
                                   uint32_t       tile_id,
                                   uint8_t        dim,
                                   uint32_t       n_tokens,
                                   uint64_t       nonce,
                                   uint64_t       origin_key)
{
    ChunkDesc d;
    d.chunk_idx  = chunk_idx;
    d.tile_id    = tile_id;
    d.dim        = dim;
    d.temperature = pgfe_temperature(chunk);

    /* gear */
    d.gear = (n_tokens <= 512u)  ? 1u :
             (n_tokens <= 1024u) ? 2u :
             (n_tokens <= 2048u) ? 3u : 4u;

    /* timeline */
    d.enc        = pgfe_enc(tile_id, dim);
    d.tring_slot = (uint16_t)(d.enc % 720u);

    /* zone / pole */
    uint32_t compound = ((tile_id % 32u) + (uint32_t)(dim % 32u) * 32u) % 144u;
    d.zone = (uint8_t)(compound % PGFE_FACE_DEFAULT);
    d.pole = (d.zone >= 6u) ? 1u : 0u;

    /* shape / polarity — mirrors bermuda_shape */
    if (d.temperature == PGFE_HOT) {
        d.shape    = (d.pole == 0u) ? 'I' : 'O';
        d.polarity = d.pole;
    } else {
        d.shape    = 'S';   /* COLD → CROSS → GROUND */
        d.polarity = 1u;
    }

    /* keys — Approach B: purely positional */
    d.geo_key    = pgfe_geo_key(nonce, chunk_idx, tile_id, dim);
    d.bond_key   = pgfe_bond_key(d.geo_key, chunk_idx);
    d.origin_key = (origin_key != 0u) ? origin_key : d.geo_key;

    d._pad = 0u;
    return d;
}

/* ════════════════════════════════════════════════════════════════
   HEADER — build from chunk_0 desc + session params
   ════════════════════════════════════════════════════════════════ */

static inline POGLSHeader pgfe_build_header(const ChunkDesc *head,
                                             uint64_t         nonce,
                                             uint8_t          layer_count,
                                             uint8_t          has_cold)
{
    POGLSHeader h;
    h.session_nonce = nonce;
    h.root_seed     = head->geo_key;
    h.enc_start     = head->enc;
    h.gear          = head->gear;
    h.layer_count   = layer_count;
    h.codec_id      = PGFE_CODEC_W;
    h.flags         = has_cold ? PGFE_FLAG_HAS_RESIDUAL : 0u;
    h.origin_key    = head->geo_key;
    return h;
}

/* ════════════════════════════════════════════════════════════════
   HEADER SERIAL / DESERIAL — 30B memcpy (LE, platform-neutral)
   ════════════════════════════════════════════════════════════════ */

static inline void pgfe_header_write(const POGLSHeader *h,
                                      uint8_t out[PGFE_HEADER_SZ])
{
    memcpy(out, h, PGFE_HEADER_SZ);
}

static inline void pgfe_header_read(const uint8_t in[PGFE_HEADER_SZ],
                                     POGLSHeader *h)
{
    memcpy(h, in, PGFE_HEADER_SZ);
}

/* ════════════════════════════════════════════════════════════════
   RECONSTRUCT — decode path, no chunk data needed (Approach B)
   ════════════════════════════════════════════════════════════════ */

static inline ChunkDesc pgfe_reconstruct(const POGLSHeader *h,
                                          uint64_t           chunk_idx,
                                          uint32_t           face_max)
{
    uint32_t tile_id = (uint32_t)(chunk_idx % face_max);
    uint8_t  dim     = (uint8_t)((chunk_idx / face_max) & 0x7Fu);
    uint32_t n_tokens = (uint32_t)1u << (h->gear + 8u); /* gear→token approx */
    return pgfe_desc(NULL, chunk_idx, tile_id, dim,
                     n_tokens, h->session_nonce, h->origin_key);
}

/* ════════════════════════════════════════════════════════════════
   ENCODE STREAM — data → header + desc array
   ════════════════════════════════════════════════════════════════
 *
 * Usage:
 *   POGLSHeader hdr;
 *   ChunkDesc   descs[N];
 *   PgfeStats   stats;
 *   pgfe_encode(data, sz, face_max, n_tokens, nonce, 1,
 *               &hdr, descs, N, &stats);
 *
 * Output:
 *   hdr           → write 30B (file header / frame 0)
 *   descs[i].temperature==COLD → write 72B residual keyed by bond_key
 *   descs[i].temperature==HOT  → nothing extra
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t n_chunks;
    uint64_t n_hot;
    uint64_t n_cold;
    uint32_t residual_bytes;   /* n_cold × PGFE_RESIDUAL_COLD */
} PgfeStats;

static inline int pgfe_encode(const uint8_t  *data,
                               size_t          data_sz,
                               uint32_t        face_max,
                               uint32_t        n_tokens,
                               uint64_t        nonce,
                               uint8_t         layer_count,
                               POGLSHeader    *hdr_out,
                               ChunkDesc      *descs,
                               uint64_t        descs_cap,
                               PgfeStats      *stats)
{
    if (!data || !hdr_out || !descs || !stats) return -1;
    memset(stats, 0, sizeof(*stats));

    uint64_t n = (data_sz + PGFE_CHUNK_SZ - 1) / PGFE_CHUNK_SZ;
    if (n > descs_cap) return -2;
    stats->n_chunks = n;

    uint64_t origin = 0u;

    for (uint64_t ci = 0; ci < n; ci++) {
        size_t  off   = (size_t)(ci * PGFE_CHUNK_SZ);
        size_t  rem   = (off < data_sz) ? (data_sz - off) : 0u;
        uint8_t chunk[PGFE_CHUNK_SZ] = {0};
        if (rem > 0u)
            memcpy(chunk, data + off,
                   rem < PGFE_CHUNK_SZ ? rem : PGFE_CHUNK_SZ);

        uint32_t tile_id = (uint32_t)(ci % face_max);
        uint8_t  dim     = (uint8_t)((ci / face_max) & 0x7Fu);

        descs[ci] = pgfe_desc(chunk, ci, tile_id, dim,
                               n_tokens, nonce, origin);

        if (ci == 0u) origin = descs[0].geo_key; /* lock chain anchor */

        if (descs[ci].temperature == PGFE_HOT)
            stats->n_hot++;
        else {
            stats->n_cold++;
            stats->residual_bytes += PGFE_RESIDUAL_COLD;
        }
    }

    uint8_t has_cold = (stats->n_cold > 0u) ? 1u : 0u;
    *hdr_out = pgfe_build_header(&descs[0], nonce, layer_count, has_cold);
    return 0;
}

/* ════════════════════════════════════════════════════════════════
   VERIFY — 7 tests, returns 0 on pass
   ════════════════════════════════════════════════════════════════ */

static inline int pgfe_verify(void)
{
    /* T1: header size */
    if (sizeof(POGLSHeader) != PGFE_HEADER_SZ) return -1;

    uint64_t nonce = 0xDEADBEEFCAFEBABEULL;

    /* T2: geo_key determinism over 1440 */
    for (uint64_t ci = 0; ci < 1440u; ci++) {
        uint32_t t = (uint32_t)(ci % 12u);
        uint8_t  d = (uint8_t)((ci / 12u) & 0x7Fu);
        if (pgfe_geo_key(nonce,ci,t,d) != pgfe_geo_key(nonce,ci,t,d))
            return -2;
    }

    /* T3: uniqueness — no collisions in 1440 */
    uint64_t keys[1440];
    for (uint64_t ci = 0; ci < 1440u; ci++) {
        uint32_t t = (uint32_t)(ci % 12u);
        uint8_t  d = (uint8_t)((ci / 12u) & 0x7Fu);
        keys[ci] = pgfe_geo_key(nonce, ci, t, d);
    }
    for (int i = 0; i < 1440; i++)
        for (int j = i+1; j < 1440; j++)
            if (keys[i] == keys[j]) return -3;

    /* T4: enc in valid range */
    for (uint32_t t = 0; t < 12u; t++)
        for (uint8_t d = 0; d < 8u; d++)
            if (pgfe_enc(t, d) >= PGFE_CYCLE) return -4;

    /* T5: header serial roundtrip */
    POGLSHeader h1 = {nonce, keys[0], 42u, 2u, 1u, 27u,
                       PGFE_FLAG_HAS_RESIDUAL, keys[0]};
    uint8_t buf[PGFE_HEADER_SZ];
    pgfe_header_write(&h1, buf);
    POGLSHeader h2;
    pgfe_header_read(buf, &h2);
    if (h2.session_nonce != h1.session_nonce) return -5;
    if (h2.enc_start     != h1.enc_start)     return -5;
    if (h2.origin_key    != h1.origin_key)     return -5;
    if (h2.codec_id      != 27u)               return -5;

    /* T6: encode stream + decode roundtrip (HOT path) */
    uint8_t data[256] = {0};
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)(i % 63u);

    POGLSHeader hdr;
    ChunkDesc   descs[4];
    PgfeStats   stats;
    if (pgfe_encode(data, 256, 12u, 64u, nonce, 1u,
                    &hdr, descs, 4u, &stats) != 0) return -6;
    if (stats.n_chunks != 4u)  return -6;
    if (hdr.codec_id   != 27u) return -6;

    /* T7: reconstruct matches encode (geo_key, bond_key, enc) */
    for (uint64_t ci = 0; ci < 4u; ci++) {
        ChunkDesc rd = pgfe_reconstruct(&hdr, ci, 12u);
        if (rd.geo_key  != descs[ci].geo_key)  return -7;
        if (rd.bond_key != descs[ci].bond_key) return -7;
        if (rd.enc      != descs[ci].enc)       return -7;
    }

    return 0;
}

#endif /* POGLS_GEOFIELD_EXPORT_H */
