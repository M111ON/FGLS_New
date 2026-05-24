/*
 * diamond_shell_v1.h — 3D Shell Diamond Field Compression
 * ════════════════════════════════════════════════════════
 *
 * Replaces: chunk_isect() + d4_canon_rid() + dcoord() from bench_v2
 *
 * Architecture:
 *   fixed 64B chunk = atomic unit (unchanged)
 *   shell layer     = batch grouping (8^n expansion)
 *   XOR diff        = sparse volume vs empty cube
 *   rotation 0..5   = geometric re-index (pick lowest entropy)
 *   z-axis          = fibo timeline (17/144/720 phase boundaries)
 *
 * Single chunk (L0):   fill 64B → XOR diff → best rot → classify
 * Batch    (L1+):      N chunks fill cube together → batch XOR diff
 *
 * Shell layer sizing:
 *   L0 =    64B = 1 chunk    (4^3 × 1)
 *   L1 =   512B = 8 chunks   (8^1 × 64)
 *   L2 =  4096B = 64 chunks  (8^2 × 64)
 *   L3 = 32768B = 512 chunks (8^3 × 64)
 *
 * Fibo timeline (z = chunk index in batch):
 *   z % 17  == 0 → SIG keyframe
 *   z % 144 == 0 → FLUSH boundary
 *   z % 720 == 0 → SNAP boundary
 *
 * Encode output per chunk (variable, replaces Enc struct):
 *   SHELL_FLAT   (flag=0): 2B  layer+rot only   (all-zero diff)
 *   SHELL_SPARSE (flag=1): 9B  layer+rot+seed   (< SPARSE_THRESH bits)
 *   SHELL_DENSE  (flag=2): 17B layer+rot+64b×2  (dense, store raw XOR)
 *   SHELL_BATCH  (flag=3): 5B  batch_id+chunk_z (belongs to larger batch)
 * ════════════════════════════════════════════════════════
 */

#ifndef DIAMOND_SHELL_V1_H
#define DIAMOND_SHELL_V1_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ── constants ─────────────────────────────────────────── */

#define SHELL_CHUNK_SZ      64u
#define SHELL_ROT_STATES     6u

/* batch sizes per layer (in chunks) */
#define SHELL_L0_CHUNKS      1u
#define SHELL_L1_CHUNKS      8u
#define SHELL_L2_CHUNKS     64u
#define SHELL_L3_CHUNKS    512u

/* fibo timeline periods (from geo_fibo_clock.h) */
#define SHELL_FIBO_SIG      17u
#define SHELL_FIBO_FLUSH   144u
#define SHELL_FIBO_SNAP    720u

/* classification thresholds (popcount of diff bits) */
#define SHELL_SPARSE_THRESH  8u   /* <= 8 set bits = sparse, encode as seed */
#define SHELL_FLAT_THRESH    0u   /* == 0 set bits = flat, skip entirely */

/* encode flags */
#define SHELL_FLAG_FLAT      0u
#define SHELL_FLAG_SPARSE    1u
#define SHELL_FLAG_DENSE     2u
#define SHELL_FLAG_BATCH     3u

/* ── types ─────────────────────────────────────────────── */

/* result of classifying 1 chunk through shell pipeline */
typedef struct {
    uint8_t  flag;        /* SHELL_FLAG_* */
    uint8_t  layer;       /* shell layer 0..3 */
    uint8_t  best_rot;    /* rotation 0..5 with lowest entropy */
    uint8_t  fibo_phase;  /* 0=normal 1=sig 2=flush 3=snap */
    uint64_t diff_a;      /* XOR diff word A (bytes 0..7 after rotation) */
    uint64_t diff_b;      /* XOR diff word B (bytes 8..15) — dense only */
    uint64_t seed;        /* FNV hash of diff_a — sparse only */
    uint32_t batch_id;    /* batch this chunk belongs to (L1+) */
    uint32_t chunk_z;     /* z-index within batch */
    uint8_t  popcount;    /* popcount of diff bits */
} ShellChunkResult;

/* batch context: accumulates chunks, fires when full */
typedef struct {
    uint8_t  layer;
    uint32_t capacity;    /* total chunks in this batch */
    uint32_t count;       /* chunks received so far */
    uint32_t batch_id;
    uint8_t  best_rot;    /* batch-level rotation (computed at flush) */
    uint64_t route_xor;   /* accumulated XOR across batch (fibo timeline) */
    uint8_t *cube;        /* filled cube buffer [capacity * 64] */
} ShellBatch;

/* metrics (replaces Metrics struct) */
typedef struct {
    uint64_t n_chunks;
    uint64_t n_flat;
    uint64_t n_sparse;
    uint64_t n_dense;
    uint64_t n_batch;
    uint64_t enc_bytes;
    uint64_t raw_bytes;
    uint64_t rot_wins[SHELL_ROT_STATES];  /* how often each rot won */
    double   ratio;
} ShellMetrics;

/* ── internal: rotation re-index 64B chunk ─────────────── */
/*
 * 64B chunk = 4×4×4 cube (base-2 geometry, 4^3 = 64)
 * 6 rotations = 6 face-forward orientations
 * re-index: which src byte maps to dst position
 */
static inline void _shell_rotate64(uint8_t out[64],
                                    const uint8_t in[64],
                                    uint8_t rot)
{
    /* 4×4×4 grid: idx = z*16 + y*4 + x */
    for (uint8_t z = 0; z < 4; z++) {
        for (uint8_t y = 0; y < 4; y++) {
            for (uint8_t x = 0; x < 4; x++) {
                uint8_t sx, sy, sz;
                switch (rot % SHELL_ROT_STATES) {
                    case 0: sx=x;   sy=y;   sz=z;   break; /* identity    */
                    case 1: sx=y;   sy=z;   sz=x;   break; /* +X forward  */
                    case 2: sx=z;   sy=x;   sz=y;   break; /* +Y forward  */
                    case 3: sx=x;   sy=z;   sz=3-y; break; /* -Y (z/y)    */
                    case 4: sx=z;   sy=y;   sz=3-x; break; /* -X (z/x)    */
                    case 5: sx=3-y; sy=x;   sz=z;   break; /* -Z (xy swap)*/
                    default: sx=x; sy=y; sz=z; break;
                }
                out[z*16 + y*4 + x] = in[sz*16 + sy*4 + sx];
            }
        }
    }
}

/* ── internal: entropy proxy (sum of set bits in each byte position) ── */
/* lower = more sparse = better for codec */
static inline uint32_t _shell_entropy(const uint8_t buf[64])
{
    uint32_t e = 0;
    for (int i = 0; i < 64; i++) e += __builtin_popcount(buf[i]);
    return e;
}

/* ── internal: FNV-64 hash ─────────────────────────────── */
static inline uint64_t _shell_fnv64(const uint8_t *d, int n)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

/* ── internal: fibo phase of z-index ───────────────────── */
static inline uint8_t _shell_fibo_phase(uint32_t z)
{
    if (z % SHELL_FIBO_SNAP  == 0 && z > 0) return 3;
    if (z % SHELL_FIBO_FLUSH == 0 && z > 0) return 2;
    if (z % SHELL_FIBO_SIG   == 0 && z > 0) return 1;
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * shell_classify_chunk()
 *
 * Core pipeline for single 64B chunk (L0 / single mode).
 *
 * Steps:
 *   1. XOR diff: chunk XOR empty (= chunk itself, pad zeros)
 *   2. rotation scan: try all 6, pick lowest entropy
 *   3. classify: FLAT / SPARSE / DENSE
 *   4. assign fibo phase from z-index
 * ══════════════════════════════════════════════════════════ */
static inline ShellChunkResult shell_classify_chunk(
    const uint8_t *chunk,     /* 64B input */
    uint32_t       chunk_z,   /* z-index in sequence (fibo timeline) */
    uint32_t       batch_id)
{
    ShellChunkResult r;
    memset(&r, 0, sizeof(r));
    r.layer     = 0;
    r.batch_id  = batch_id;
    r.chunk_z   = chunk_z;
    r.fibo_phase = _shell_fibo_phase(chunk_z);

    /* XOR diff vs empty cube = chunk itself (empty = all zeros) */
    /* Rotation scan on the diff */
    uint8_t  rotbuf[64];
    uint32_t best_entropy = UINT32_MAX;
    uint8_t  best_rot = 0;
    uint8_t  best_buf[64];

    for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
        _shell_rotate64(rotbuf, chunk, rot);
        uint32_t e = _shell_entropy(rotbuf);
        if (e < best_entropy) {
            best_entropy = e;
            best_rot = rot;
            memcpy(best_buf, rotbuf, 64);
        }
    }

    r.best_rot = best_rot;
    r.popcount = (uint8_t)(best_entropy); /* total set bits after best rotation */

    /* pack diff into two 64-bit words */
    memcpy(&r.diff_a, best_buf,      8);
    memcpy(&r.diff_b, best_buf + 8, 8);

    /* classify */
    if (best_entropy == 0) {
        r.flag = SHELL_FLAG_FLAT;
    } else if (best_entropy <= SHELL_SPARSE_THRESH) {
        r.flag = SHELL_FLAG_SPARSE;
        r.seed = _shell_fnv64(best_buf, 64);
    } else {
        r.flag = SHELL_FLAG_DENSE;
        r.seed = _shell_fnv64(best_buf, 64);
    }

    return r;
}

/* ══════════════════════════════════════════════════════════
 * shell_encode_size()
 * Returns byte cost for encoding a classified chunk.
 * ══════════════════════════════════════════════════════════ */
static inline uint32_t shell_encode_size(const ShellChunkResult *r)
{
    switch (r->flag) {
        case SHELL_FLAG_FLAT:   return 2u;   /* layer(1) + rot(1) */
        case SHELL_FLAG_SPARSE: return 9u;   /* +layer+rot+seed(8) */
        case SHELL_FLAG_DENSE:  return 17u;  /* +layer+rot+diff_a(8)+diff_b(8) */
        case SHELL_FLAG_BATCH:  return 5u;   /* batch_id(4)+z(1) */
        default:                return 17u;
    }
}

/* ══════════════════════════════════════════════════════════
 * ShellBatch API
 * ══════════════════════════════════════════════════════════ */

static inline ShellBatch *shell_batch_new(uint8_t layer, uint32_t batch_id)
{
    static const uint32_t caps[] = {
        SHELL_L0_CHUNKS, SHELL_L1_CHUNKS, SHELL_L2_CHUNKS, SHELL_L3_CHUNKS
    };
    uint8_t l = layer < 4 ? layer : 3;
    ShellBatch *b = (ShellBatch *)calloc(1, sizeof(ShellBatch));
    b->layer    = l;
    b->capacity = caps[l];
    b->batch_id = batch_id;
    b->cube     = (uint8_t *)calloc(b->capacity * SHELL_CHUNK_SZ, 1);
    return b;
}

static inline void shell_batch_free(ShellBatch *b)
{
    if (b) { free(b->cube); free(b); }
}

/* push one chunk into batch; returns 1 when batch is full */
static inline int shell_batch_push(ShellBatch *b, const uint8_t *chunk)
{
    if (b->count >= b->capacity) return 1;
    memcpy(b->cube + b->count * SHELL_CHUNK_SZ, chunk, SHELL_CHUNK_SZ);
    b->count++;
    return (b->count >= b->capacity) ? 1 : 0;
}

/*
 * shell_batch_flush()
 * Called when batch is full (or end of stream).
 * Runs rotation scan over entire cube, sets best_rot.
 * Returns best rotation for this batch.
 */
static inline uint8_t shell_batch_flush(ShellBatch *b)
{
    size_t cube_sz = (size_t)b->count * SHELL_CHUNK_SZ;
    uint8_t *rotbuf = (uint8_t *)malloc(cube_sz);
    uint32_t best_e = UINT32_MAX;
    uint8_t  best_r = 0;

    for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
        /* rotate each chunk in batch with same rotation */
        uint32_t total_e = 0;
        for (uint32_t i = 0; i < b->count; i++) {
            _shell_rotate64(rotbuf + i * SHELL_CHUNK_SZ,
                            b->cube + i * SHELL_CHUNK_SZ, rot);
            total_e += _shell_entropy(rotbuf + i * SHELL_CHUNK_SZ);
        }
        if (total_e < best_e) { best_e = total_e; best_r = rot; }
    }

    b->best_rot = best_r;
    free(rotbuf);
    return best_r;
}

/* ══════════════════════════════════════════════════════════
 * shell_run_pipeline()
 *
 * Full pipeline replacing run_pipe() from bench_v2.
 * Processes N×64B chunks, returns ShellMetrics.
 *
 * Layer selection:
 *   pass layer=0 for single-chunk mode
 *   pass layer=1..3 for batch mode (auto-groups)
 * ══════════════════════════════════════════════════════════ */
static inline ShellMetrics shell_run_pipeline(
    const uint8_t *data,
    uint64_t       n_chunks,
    uint8_t        layer)
{
    ShellMetrics m;
    memset(&m, 0, sizeof(m));
    m.n_chunks  = n_chunks;
    m.raw_bytes = n_chunks * SHELL_CHUNK_SZ;

    if (layer == 0) {
        /* ── single chunk mode (L0) ── */
        for (uint64_t i = 0; i < n_chunks; i++) {
            ShellChunkResult r = shell_classify_chunk(
                data + i * SHELL_CHUNK_SZ, (uint32_t)(i), 0);
            uint32_t esz = shell_encode_size(&r);
            m.enc_bytes += esz;
            m.rot_wins[r.best_rot]++;
            switch (r.flag) {
                case SHELL_FLAG_FLAT:   m.n_flat++;   break;
                case SHELL_FLAG_SPARSE: m.n_sparse++; break;
                case SHELL_FLAG_DENSE:  m.n_dense++;  break;
                default: break;
            }
        }
    } else {
        /* ── batch mode (L1+) ── */
        uint32_t batch_id = 0;
        ShellBatch *batch = shell_batch_new(layer, batch_id);

        for (uint64_t i = 0; i < n_chunks; i++) {
            int full = shell_batch_push(batch, data + i * SHELL_CHUNK_SZ);

            if (full) {
                uint8_t br = shell_batch_flush(batch);
                m.rot_wins[br]++;

                /* encode: batch header = layer(1)+rot(1)+count(2) = 4B */
                /* each chunk in batch = BATCH flag = 5B */
                m.enc_bytes += 4u + batch->count * 5u;
                m.n_batch   += batch->count;

                shell_batch_free(batch);
                batch = shell_batch_new(layer, ++batch_id);
            }
        }

        /* flush partial batch */
        if (batch->count > 0) {
            uint8_t br = shell_batch_flush(batch);
            m.rot_wins[br]++;
            m.enc_bytes += 4u + batch->count * 5u;
            m.n_batch   += batch->count;
        }
        shell_batch_free(batch);
    }

    m.ratio = m.enc_bytes > 0
        ? (double)m.raw_bytes / (double)m.enc_bytes
        : 0.0;
    return m;
}

#endif /* DIAMOND_SHELL_V1_H */
