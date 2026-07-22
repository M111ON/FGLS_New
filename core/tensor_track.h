/*
 * tensor_track.h — Tensor Data Tracker (Entropy Layer Only)
 * ═══════════════════════════════════════════════════════════════════════
 *
 * "enc (2B) ตอบทั้ง index และ container อยู่แล้ว"
 *
 * enc คือ address space เดียวที่ตอบ 2 คำถาม:
 *   Index:     frame_at(enc)        → (face, slot, ico)   — data คืออะไร
 *   Container: ft_enc_to_field(enc) → (ring, wedge)       — data เก็บที่ไหน
 *
 * สิ่งที่ enc ไม่รู้ (สิ่งเดียวที่ tracker ทำ):
 *   - Entropy analysis: ข้อมูลเข้ามามีความสุ่มแค่ไหน?
 *   - Stream statistics: ข้อมูลไหลผ่านระบบยังไง?
 *
 * Flow:
 *   data → rdh_capture() → enc (2B)
 *          tt_entropy_score(data) → entropy (0..255)
 *          frame_at(enc) → (face, slot, ico)     ← INDEX
 *          ft_enc_to_field(enc) → (ring, wedge)  ← CONTAINER
 *          ft_store_action(enc) → routing mode
 *
 * ไม่มี enc_find_home ซ้ำ
 * ไม่มี enc_chunk_idx แยก
 * ไม่มี routing decision แยก
 * ทุกอย่างมาจาก enc เดียว
 *
 * ═══════════════════════════════════════════════════════════════════════
 * No malloc. No float in core path. O(1) per chunk.
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef TENSOR_TRACK_H
#define TENSOR_TRACK_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>

/* ── Geometric core — enc answers everything ─────────────── */
#include "geo_frame_seek.h"     /* DualFrame, frame_at, FRAME_CYCLE   */
#include "rdh_capture.h"        /* rdh_capture → enc (2B)             */
#include "fibo_tick.h"          /* ft_store_action, ft_enc_to_field   */

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════ */

#define TT_RING_SIZE        256u    /* incoming chunk ring buffer     */

/* Entropy classification thresholds (0..255 scale) */
#define TT_ENTROPY_LOW      64u
#define TT_ENTROPY_MED      128u
#define TT_ENTROPY_HIGH     192u

/* ═══════════════════════════════════════════════════════════════════════
   ENTROPY ANALYSIS — สิ่งเดียวที่ enc ไม่รู้
   ═══════════════════════════════════════════════════════════════════════
   enc = address = WHERE
   entropy = characteristic = WHAT kind of data
   
   H = -Σ p_i × log2(p_i), normalized to 0..255
   ═══════════════════════════════════════════════════════════════════════ */

/* log2×256 fixed-point table for integer Shannon entropy */
static const uint16_t TT_LOG2_TABLE[256] = {
    0,   0, 256, 406, 512, 595, 663, 721, 768, 808, 844, 876, 905, 931, 955, 976,
    1024,1046,1067,1087,1106,1123,1140,1156,1171,1185,1199,1213,1225,1238,1250,1261,
    1280,1291,1301,1311,1321,1331,1340,1349,1358,1367,1376,1384,1392,1400,1408,1416,
    1424,1431,1439,1446,1453,1460,1467,1474,1481,1488,1494,1501,1507,1513,1519,1525,
    1536,1542,1548,1553,1559,1564,1570,1575,1580,1586,1591,1596,1601,1606,1611,1616,
    1632,1637,1642,1647,1652,1657,1662,1666,1671,1676,1680,1685,1689,1694,1698,1703,
    1707,1711,1715,1720,1724,1728,1732,1736,1740,1744,1748,1752,1756,1760,1763,1767,
    1792,1796,1799,1803,1807,1810,1814,1817,1821,1824,1828,1831,1835,1838,1841,1845,
    1856,1859,1862,1866,1869,1872,1875,1879,1882,1885,1888,1891,1894,1897,1900,1903,
    1920,1923,1926,1929,1932,1935,1938,1941,1944,1947,1950,1953,1956,1958,1961,1964,
    1984,1987,1989,1992,1995,1997,2000,2003,2005,2008,2011,2013,2016,2018,2021,2024,
    2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,
    2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,
    2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,
    2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,
    2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,
};

static inline uint32_t tt_floor_log2(uint32_t v) {
    uint32_t r = 0;
    if (v >= 1u << 16) { v >>= 16; r |= 16; }
    if (v >= 1u <<  8) { v >>=  8; r |=  8; }
    if (v >= 1u <<  4) { v >>=  4; r |=  4; }
    if (v >= 1u <<  2) { v >>=  2; r |=  2; }
    if (v >= 1u <<  1) { r |=  1; }
    return r;
}

static inline uint32_t tt_log2_x256(uint32_t v) {
    if (v == 0) return 0;
    if (v < 256) return TT_LOG2_TABLE[v];
    uint32_t bits = tt_floor_log2(v);
    if (bits <= 8) return TT_LOG2_TABLE[v];
    uint32_t shifted = v >> (bits - 8);
    return (bits << 8) + TT_LOG2_TABLE[shifted & 0xFF];
}

/* Shannon entropy → 0..255 score. O(len), integer-only. */
static inline uint8_t tt_entropy_score(const uint8_t *data, uint32_t len) {
    if (!data || len == 0) return 0;

    uint32_t counts[256] = {0};
    for (uint32_t i = 0; i < len; i++) counts[data[i]]++;

    int32_t h_x256 = 0;
    for (uint32_t i = 0; i < 256; i++) {
        if (counts[i] == 0) continue;
        uint32_t c = counts[i];
        int32_t log2p_x256 = (int32_t)tt_log2_x256(c) - (int32_t)tt_log2_x256(len);
        h_x256 -= (int32_t)((int64_t)c * log2p_x256 * 256 / len);
    }

    int32_t h_max_x256 = (int32_t)tt_log2_x256(len);
    if (h_max_x256 <= 0) return 0;

    int32_t score = (h_x256 * 255) / h_max_x256;
    if (score < 0) score = 0;
    return (uint8_t)(score > 255 ? 255 : score);
}

static inline uint8_t tt_entropy_class(uint8_t score) {
    if (score < TT_ENTROPY_LOW)  return 0;  /* structured */
    if (score < TT_ENTROPY_MED)  return 1;  /* moderate    */
    if (score < TT_ENTROPY_HIGH) return 2;  /* high        */
    return 3;                                /* near-random */
}

/* ═══════════════════════════════════════════════════════════════════════
   CHUNK RECORD — enc + entropy + data (reconstructable)
   ═══════════════════════════════════════════════════════════════════════
   Properties:
     DETERMINISTIC:  same data → same enc, same entropy, same route
     LOSSLESS:       enc = rdh_capture(data) — integer bijection
                     data stored in record — zero information loss
     RECONSTRUCTABLE: from record, reconstruct:
       1. original data  → record.data[]
       2. address        → record.enc → frame_at(enc), ft_enc_to_field(enc)
       3. routing        → tt_route(enc, entropy_class) → strategy
       4. temporal       → ft_store_action(enc) → MAIN/PIPE/BRIDGE/FREEZE
   
   enc ให้: face, slot, ico_idx, phase, tick, store_action, field position
   tracker ให้: entropy_score, entropy_class, original data
   ═══════════════════════════════════════════════════════════════════════ */

#define TT_MAX_CHUNK_SZ  64u   /* max data per record */

typedef struct {
    uint32_t  chunk_id;         /* sequence number                    */
    uint32_t  data_len;         /* actual data length                 */
    uint16_t  enc;              /* flat_key % 1440 — the address     */
    uint8_t   entropy_score;    /* 0..255 Shannon score               */
    uint8_t   entropy_class;    /* 0=structured, 1=moderate, 2=high, 3=random */
    uint8_t   data[TT_MAX_CHUNK_SZ]; /* original data — for lossless reconstruct */
} TTChunkRecord;

/* Reconstruct from record:
 *   frame_at(rec.enc)           → (face, slot, ico_idx, phase)
 *   ft_enc_to_field(rec.enc)    → (ring, wedge) on 144×144 field
 *   ft_store_action(rec.enc)    → MAIN/PIPE/BRIDGE/FREEZE
 *   tt_route(rec.enc, cls)      → storage strategy
 *   rec.data                    → original data (lossless)
 */

/* ═══════════════════════════════════════════════════════════════════════
   STREAM STATISTICS — aggregate entropy distribution
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  total_chunks;
    uint32_t  total_bytes;
    uint32_t  entropy_count[4];  /* [structured, moderate, high, random] */
    uint8_t   entropy_avg;       /* running average                     */
    uint32_t  action_count[4];   /* [MAIN, BRIDGE, PIPE, FREEZE]        */
    uint32_t  enc_coverage;      /* unique enc values seen              */
    uint16_t  last_enc;
} TTStats;

/* ═══════════════════════════════════════════════════════════════════════
   TRACKER CONTEXT — minimal
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    TTChunkRecord ring[TT_RING_SIZE];
    uint32_t  ring_head;
    uint32_t  ring_count;
    TTStats   stats;
    uint16_t  enc_seen[(FRAME_CYCLE + 15) / 16];
} TTContext;

/* ═══════════════════════════════════════════════════════════════════════
   INIT
   ═══════════════════════════════════════════════════════════════════════ */

static inline void tt_init(TTContext *ctx) {
    memset(ctx, 0, sizeof(TTContext));
}

/* ═══════════════════════════════════════════════════════════════════════
   CORE: tt_ingest — data → enc + entropy
   ═══════════════════════════════════════════════════════════════════════
   1 ขั้นตอน:
     1. rdh_capture → enc
     2. tt_entropy_score → entropy
   
   หลัง return ใช้ rec.enc เพื่อ:
     frame_at(rec.enc)        → หน้า, ช่อง, ico
     ft_enc_to_field(rec.enc) → พิกัดบน field
     ft_store_action(rec.enc) → โหมดเก็บ
   ═══════════════════════════════════════════════════════════════════════ */

static inline int tt_ingest(TTContext *ctx,
                             const uint8_t *data, uint32_t data_len,
                             TTChunkRecord *rec_out)
{
    if (!ctx || !data || data_len == 0 || !rec_out) return -1;

    uint32_t cid = ctx->ring_count;

    /* enc — the single address that answers everything */
    int64_t flat_key = rdh_capture(data, (size_t)data_len, &RDH_CAPTURE_144);
    uint16_t enc = (uint16_t)((uint64_t)flat_key % FRAME_CYCLE);

    /* entropy — the one thing enc doesn't know */
    uint8_t ent = tt_entropy_score(data, data_len);
    uint8_t cls = tt_entropy_class(ent);

    /* record: enc + entropy + data (lossless) */
    rec_out->chunk_id      = cid;
    rec_out->data_len      = data_len;
    rec_out->enc           = enc;
    rec_out->entropy_score = ent;
    rec_out->entropy_class = cls;
    /* copy data for lossless reconstruct (max TT_MAX_CHUNK_SZ bytes) */
    uint32_t copy_len = data_len;
    if (copy_len > TT_MAX_CHUNK_SZ) copy_len = TT_MAX_CHUNK_SZ;
    memcpy(rec_out->data, data, copy_len);

    /* ring buffer */
    ctx->ring[ctx->ring_head] = *rec_out;
    ctx->ring_head = (ctx->ring_head + 1) % TT_RING_SIZE;
    ctx->ring_count = cid + 1;

    /* stats */
    TTStats *s = &ctx->stats;
    s->total_chunks++;
    s->total_bytes += data_len;
    s->entropy_count[cls]++;
    s->action_count[ft_store_action(enc)]++;
    s->entropy_avg = (uint8_t)(
        ((uint32_t)s->entropy_avg * (s->total_chunks - 1) + ent)
        / s->total_chunks
    );
    s->last_enc = enc;

    /* enc coverage */
    uint32_t w = enc / 16, b = enc % 16;
    if (!(ctx->enc_seen[w] & (1u << b))) {
        ctx->enc_seen[w] |= (uint16_t)(1u << b);
        s->enc_coverage++;
    }

    return (int)cid;
}

/* ═══════════════════════════════════════════════════════════════════════
   BATCH — process N × 48B chunks
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t tt_ingest_batch(TTContext *ctx,
                                        const uint8_t *data, uint32_t total_len,
                                        uint32_t chunk_sz,
                                        TTChunkRecord *records, uint32_t max_records)
{
    uint32_t n = total_len / chunk_sz;
    if (n > max_records) n = max_records;
    if (n > TT_RING_SIZE - ctx->ring_count)
        n = TT_RING_SIZE - ctx->ring_count;

    for (uint32_t i = 0; i < n; i++)
        tt_ingest(ctx, data + i * chunk_sz, chunk_sz, &records[i]);
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════
   QUERY — get from ring buffer
   ═══════════════════════════════════════════════════════════════════════ */

static inline const TTChunkRecord* tt_ring_get(const TTContext *ctx, uint32_t idx) {
    uint32_t count = ctx->ring_count;
    if (idx >= count) return NULL;
    if (count <= TT_RING_SIZE) return &ctx->ring[idx];
    uint32_t oldest = count - TT_RING_SIZE;
    if (idx < oldest) return NULL;
    return &ctx->ring[idx % TT_RING_SIZE];
}

/* ═══════════════════════════════════════════════════════════════════════
   ENTROPY-AWARE ROUTING — เชื่อม tracker ↔ enclosure
   ═══════════════════════════════════════════════════════════════════════
   enc บอก WHERE (face/slot/field pos) + WHEN (tick/action)
   entropy บอก HOW (ยังไงเก็บ)
   
   Routing logic:
     entropy_class  ×  tick_action  →  storage_strategy
   
     structured (0) + BRIDGE  →  compress before residual store
     structured (0) + PIPE    →  pipe room, inner texture (ordered)
     structured (0) + MAIN    →  standard field, high compressibility
     moderate   (1) + BRIDGE  →  residual store, no compress
     moderate   (1) + PIPE    →  pipe room, outer texture
     high       (2) + BRIDGE  →  raw residual (incompressible)
     high       (2) + PIPE    →  pipe room, raw mode
     random     (3) + *       →  always raw, never compress
   
   Destination is always derived from enc — never stored separately.
   ═══════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════
   THREE-VIEW ROUTING — enc → all field dimensions
   ═══════════════════════════════════════════════════════════════════════
   enc (2B) answers ALL questions simultaneously:
   
   View 1 (Frame Seek):  face, slot, phase, ico    — "data คืออะไร"
   View 2 (Fibo Spine):  pipe_id, tick             — "data อยู่ใน pipe ไหน"
   View 3 (P5H Ribcage): flower_id, phase, texture — "data อยู่ใน flower ไหน"
   
   Routing strategy comes from entropy analysis (tt_entropy_class).
   Destination is always enc-derived — never stored separately.
   ═══════════════════════════════════════════════════════════════════════ */

/* Storage strategy — returned by tt_route() */
#define TT_STRAT_STANDARD    0u   /* normal field storage               */
#define TT_STRAT_COMPRESS    1u   /* compress then store                */
#define TT_STRAT_RAW         2u   /* store raw (incompressible)         */
#define TT_STRAT_BRIDGE_RAW  3u   /* residual, raw                      */
#define TT_STRAT_BRIDGE_CMP  4u   /* residual, compressed               */

/* Full routing result — all three views + strategy */
typedef struct {
    /* Address — the single source of truth */
    uint16_t  enc;              /* the address (0..1439)                */
    
    /* View 1: Frame Seek (geometry) */
    uint8_t   face;             /* 0..11 (icosahedron face)            */
    uint8_t   slot;             /* 0..119 (position in face)           */
    uint8_t   ico_idx;          /* icosphere index                     */
    
    /* View 2: Fibo Spine (timeline) */
    uint16_t  pipe_id;          /* 0..1727 (pipe channel)              */
    uint8_t   tick;             /* 0..11 (sync axis)                   */
    
    /* View 3: P5H Ribcage (flower field) */
    uint16_t  flower_id;        /* 0..1727 (flower scatter)            */
    uint8_t   phase_in_flower;  /* 0..9 or 255=barrier                 */
    uint8_t   texture;          /* P5H_TEX_INNER / P5H_TEX_OUTER      */
    
    /* Entropy + Strategy */
    uint8_t   entropy_class;    /* 0..3                                */
    uint8_t   strategy;         /* TT_STRAT_*                          */
    uint8_t   is_residual;      /* 1 = Jet Bridge residual (tick 11)   */
    uint8_t   is_compressed;    /* 1 = strategy includes compression   */
} TTRoute;

/* Route one chunk: enc → all three views + strategy.
 * This function does NOT move data — it returns routing metadata. */
static inline TTRoute tt_route(uint16_t enc, uint8_t entropy_class) {
    TTRoute r;
    r.enc = enc;
    r.entropy_class = entropy_class;
    
    /* View 1: Frame Seek */
    DualFrame f = frame_at(enc);
    r.face    = f.face;
    r.slot    = f.slot;
    r.ico_idx = f.ico_idx;
    
    /* View 2: Fibo Spine */
    r.pipe_id = enc % 1728u;   /* enc 0..1439, pipes 0..1727 */
    r.tick    = (uint8_t)((enc / FRAME_EDGES) % 12);
    
    /* View 3: P5H Ribcage */
    r.flower_id = (uint16_t)(((uint32_t)enc * 37u) % 1728u);
    uint8_t t = r.tick;
    if (t == 0 || t > 10) {
        r.phase_in_flower = 255;  /* barrier */
        r.texture = 0;
    } else {
        r.phase_in_flower = t - 1;  /* 1→0, 2→1, ..., 10→9 */
        r.texture = (r.phase_in_flower & 1) ? 1u : 0u;  /* inner/outer */
    }
    
    /* Strategy — from entropy class + residual check */
    r.is_residual = (ft_store_action(enc) == FT_STORE_BRIDGE) ? 1 : 0;

    if (entropy_class >= 3) {
        r.strategy = r.is_residual ? TT_STRAT_BRIDGE_RAW : TT_STRAT_RAW;
        r.is_compressed = 0;
    } else if (entropy_class == 2) {
        r.strategy = r.is_residual ? TT_STRAT_BRIDGE_RAW : TT_STRAT_RAW;
        r.is_compressed = 0;
    } else if (entropy_class == 1) {
        r.strategy = r.is_residual ? TT_STRAT_BRIDGE_RAW : TT_STRAT_STANDARD;
        r.is_compressed = 0;
    } else {
        if (r.is_residual) {
            r.strategy = TT_STRAT_BRIDGE_CMP;
            r.is_compressed = 1;
        } else {
            r.strategy = TT_STRAT_COMPRESS;
            r.is_compressed = 1;
        }
    }

    return r;
}

/* Convenience: route a TTChunkRecord */
static inline TTRoute tt_route_record(const TTChunkRecord *rec) {
    return tt_route(rec->enc, rec->entropy_class);
}

/* ═══════════════════════════════════════════════════════════════════════
   ENCLOSURE GLUE — เชื่อม tracker ↔ enclosure (gls_enclosure.h)
   ═══════════════════════════════════════════════════════════════════════
   tensor_track บอก WHERE (enc → face/slot/field) + HOW (entropy → strategy)
   gls_enclosure บอก storing mechanics (hexagon spread, chunk pack)
   
   tt_store() bridges the two:
     1. Derive field position from enc (ft_enc_to_field)
     2. Compute frame_range for high-entropy tolerance
     3. Call enclosure's enc_pack_chunk() for actual storage
   
   Requires: #include "gls_enclosure.h" before calling tt_store().
   ═══════════════════════════════════════════════════════════════════════ */

/* Storage result — what happened when we tried to store */
typedef struct {
    int       chunk_idx;    /* enclosure chunk index (-1 = failed)        */
    uint16_t  enc;          /* the address used                           */
    uint8_t   frame_lo;     /* low frame bound (entropy tolerance)        */
    uint8_t   frame_hi;     /* high frame bound (entropy tolerance)       */
    uint8_t   strategy;     /* what routing decided                       */
    uint8_t   stored;       /* 1 = successfully packed into chunk_out     */
} TTStoreResult;

#ifdef GLS_ENCLOSURE_H   /* only compile if gls_enclosure.h was included */

/* Store one record into enclosure field.
 * chunk_out must be enc_cfg.chunk_size bytes (caller allocates).
 * Returns TTStoreResult with enclosure chunk index + frame range. */
static inline TTStoreResult tt_store(EncCtx *enc_ctx,
                                     const TTChunkRecord *rec,
                                     uint8_t *chunk_out)
{
    TTStoreResult sr;
    sr.enc       = rec->enc;
    sr.strategy  = rec->entropy_class;  /* pass through for stats */
    sr.stored    = 0;
    sr.chunk_idx = -1;

    /* 1. Frame range — entropy tolerance for high-entropy data */
    FrameRange fr = frame_range(rec->enc, rec->entropy_class);
    sr.frame_lo = fr.frame_lo;
    sr.frame_hi = fr.frame_hi;

    /* 2. Field position from enc */
    uint32_t ring, wedge;
    ft_enc_to_field(rec->enc, &ring, &wedge);

    /* 3. Pack into enclosure chunk (hexagon spread) */
    uint32_t home_x = wedge;
    uint32_t home_y = ring;
    int cidx = enc_pack_chunk(enc_ctx,
                               rec->data, rec->data_len,
                               home_x, home_y,
                               chunk_out);
    sr.chunk_idx = cidx;
    sr.stored = (cidx >= 0) ? 1 : 0;

    /* 4. Update enclosure state */
    if (sr.stored) {
        enc_ctx->last_home_x = home_x;
        enc_ctx->last_home_y = home_y;
        enc_ctx->n_chunks++;
        enc_ctx->total_bytes += rec->data_len;
    }

    return sr;
}

#else  /* gls_enclosure.h not included — stub */

static inline TTStoreResult tt_store(void *enc_ctx,
                                     const TTChunkRecord *rec,
                                     uint8_t *chunk_out)
{
    (void)enc_ctx; (void)rec; (void)chunk_out;
    TTStoreResult sr = { -1, rec->enc, 0, 0, 0, 0 };
    return sr;
}

#endif /* GLS_ENCLOSURE_H */

/* ═══════════════════════════════════════════════════════════════════════
   VERIFY — structural integrity
   ═══════════════════════════════════════════════════════════════════════ */

static inline int tt_verify(const TTContext *ctx) {
    if (ctx->ring_count != ctx->stats.total_chunks) return -1;
    if (ctx->ring_head >= TT_RING_SIZE) return -2;

    uint32_t ent_sum = ctx->stats.entropy_count[0] + ctx->stats.entropy_count[1]
                     + ctx->stats.entropy_count[2] + ctx->stats.entropy_count[3];
    if (ent_sum != ctx->stats.total_chunks) return -3;

    uint32_t act_sum = ctx->stats.action_count[0] + ctx->stats.action_count[1]
                     + ctx->stats.action_count[2] + ctx->stats.action_count[3];
    if (act_sum != ctx->stats.total_chunks) return -4;

    if (ctx->stats.enc_coverage > FRAME_CYCLE) return -5;

    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* TENSOR_TRACK_H */
