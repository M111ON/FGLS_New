/*
 * geo_flow_chunker.h — Content-driven chunk boundaries using geometric flow
 *
 * Based on test_integrity_v2: DiamondBlock flow uses `fold_fibo_intersect`
 * popcnt as a measure of geometric structure. When popcnt drops to 0,
 * the "vein" has run out — it's a natural dead zone → chunk boundary.
 *
 * This produces variable-length chunks that align with the data's own
 * geometric structure, unlike fixed 64B partitions that may cut through
 * patterns.
 *
 * Algorithm:
 *   Fit a DiamondBlock to every byte position via fold_block_init(face,edge,z).
 *   fold_fibo_intersect ≈ 0 near unstructured inter-seam zones.
 *   Use min_chunk and max_chunk constraints to avoid degenerate splits.
 *
 * Parameters:
 *   window_sz      — DiamondBlock stride (default 64, same as chunk)
 *   min_chunk      — minimum chunk size in bytes (default 32)
 *   max_chunk      — maximum chunk size (default 4096)
 *   isect_thresh   — popcnt threshold for dead zone (default 0)
 *
 * Output: array of {offset, length} segment descriptors.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pogls_fold.h"

#define FLOW_MIN_CHUNK    32u
#define FLOW_MAX_CHUNK    4096u
#define FLOW_WINDOW       64u
#define FLOW_ISECT_THRESH 0     /* dead zone = popcnt <= this */

/* One content-driven segment — v8: 64-bit for large file support */
typedef struct {
    uint64_t   offset;   /* byte offset in original data */
    uint64_t   length;   /* byte length of this segment (1..max_chunk) */
} FlowSegment;

/* ── Derive seed from sliding window ───────────────────────────── */

/* XOR-fold 64 bytes into uint64, same as _sm_derive_seed */
static inline uint64_t flow_derive_seed(const uint8_t *data) {
    const uint64_t *w = (const uint64_t*)data;
    uint64_t s = w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]^w[7];
    s ^= s >> 33; s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33; s *= 0xc4ceb9fe1a85ec53ULL; s ^= s >> 33;
    return s;
}

/* Derive face/edge/z from seed */
static inline void flow_derive_coord(uint64_t seed, uint8_t *face, uint8_t *edge, uint8_t *z) {
    uint64_t h = seed;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
    *face = (uint8_t)(((uint64_t)(uint32_t)(h >> 32) * 12u) >> 32);
    *edge = (uint8_t)(((uint64_t)(uint32_t)(h & 0xFFFFFFFFu) * 5u) >> 32);
    *z    = (uint8_t)((h >> 16) & 0xFFu);
}

/* Compute isect popcnt at a given byte offset (needs 64B window) */
static inline int flow_isect_at(const uint8_t *data, size_t data_sz, size_t offset) {
    if (offset + FLOW_WINDOW > data_sz) return -1;
    uint64_t seed = flow_derive_seed(data + offset);
    uint8_t face, edge, z;
    flow_derive_coord(seed, &face, &edge, &z);
    DiamondBlock db = fold_block_init(face, edge, (uint32_t)z << 16, 1, 0);
    memcpy(&db.core.raw, data + offset, 8);
    db.invert = ~db.core.raw;
    fold_build_quad_mirror(&db);
    uint64_t isect = fold_fibo_intersect(&db);
    return __builtin_popcountll(isect);
}

/* ── Chunk the data ────────────────────────────────────────────── */

/* Returns number of segments (≤ max_segments). Caller must free *segments. */
static inline int flow_chunk(const uint8_t *data, size_t data_sz,
                              FlowSegment **out_segments,
                              uint32_t max_segments,
                              uint32_t min_chunk, uint32_t max_chunk) {
    if (!data || !out_segments || data_sz == 0) return 0;
    if (max_segments == 0) max_segments = 65536;
    if (min_chunk == 0) min_chunk = FLOW_MIN_CHUNK;
    if (max_chunk == 0) max_chunk = FLOW_MAX_CHUNK;

    FlowSegment *segs = malloc((size_t)max_segments * sizeof(FlowSegment));
    if (!segs) return -1;

    uint64_t seg_count = 0;
    uint64_t pos = 0;

    while (pos < data_sz) {
        uint64_t remaining = (uint64_t)(data_sz - pos);
        uint64_t chunk_end = pos + (remaining < max_chunk ? remaining : max_chunk);

        /* Find a dead zone boundary within [pos + min_chunk, chunk_end] */
        uint64_t boundary = data_sz; /* default: end of file */
        uint64_t scan_start = pos + min_chunk;
        if (scan_start > data_sz) scan_start = (uint64_t)data_sz;

        for (uint64_t bp = scan_start; bp <= chunk_end && bp + FLOW_WINDOW <= data_sz; bp++) {
            int isect = flow_isect_at(data, data_sz, (size_t)bp);
            if (isect <= FLOW_ISECT_THRESH) {
                boundary = bp;
                break;
            }
        }

        /* If no dead zone found within max_chunk, force boundary */
        if (boundary > chunk_end) boundary = chunk_end;

        uint64_t seg_len = boundary - pos;
        if (seg_len < 1) seg_len = 1; /* safety: at least 1 byte */

        if (seg_count >= max_segments) break;
        segs[seg_count].offset = pos;
        segs[seg_count].length = seg_len;
        seg_count++;

        pos = boundary;
    }

    *out_segments = segs;
    return (int)seg_count;
}

static inline void flow_segments_free(FlowSegment *segs) {
    free(segs);
}
