/*
 * diamond_shell_v3.h — Optimized Diamond Shell v3 (declarations only)
 * ════════════════════════════════════════════════════════════════════
 *
 * Compile: gcc -O3 -mavx2 -std=c11 -c dgls/diamond/src/diamond_shell_v3.c
 * Link:    test_diamond_shell_v3.o + dgls/diamond/src/diamond_shell_v3.o
 */

#ifndef DIAMOND_SHELL_V3_H
#define DIAMOND_SHELL_V3_H

#include <stdint.h>

/* ── Constants (compatible with v2) ────────────────────────── */

#define SHELL_CHUNK_SZ       64u
#define SHELL_ROT_STATES      6u
#define SHELL_SPARSE_THRESH   4u

#define SHELL_FLAG_FLAT       0u
#define SHELL_FLAG_SPARSE     1u
#define SHELL_FLAG_DENSE      2u
#define SHELL_FLAG_BATCH      3u

/* ── Types ─────────────────────────────────────────────────── */

typedef struct {
    uint8_t  flag;
    uint8_t  layer;
    uint8_t  best_rot;
    uint8_t  fibo_phase;
    uint64_t fibo_isect;
    uint8_t  isect_pc;
    uint64_t diff_a;
    uint64_t diff_b;
    uint64_t seed;
    uint32_t batch_id;
    uint32_t chunk_z;
} ShellChunkResult;

typedef struct {
    uint64_t n_chunks;
    uint64_t n_flat;
    uint64_t n_sparse;
    uint64_t n_dense;
    uint64_t n_batch;
    uint64_t enc_bytes;
    uint64_t raw_bytes;
    uint64_t rot_wins[SHELL_ROT_STATES];
    uint64_t isect_total;
    double   ratio;
} ShellMetrics;

/* ── Public API ────────────────────────────────────────────── */

/* Classify a single chunk — returns full result with best rotation */
ShellChunkResult v3_classify_chunk(const uint8_t chunk[64],
                                    uint32_t chunk_z,
                                    uint32_t batch_id);

/* Process 4 chunks at once for better ILP */
void v3_classify_4chunks(const uint8_t chunks[4][64],
                          uint32_t chunk_z_base,
                          uint32_t batch_id,
                          ShellChunkResult results[4]);

/* Encode size for a classified chunk */
uint32_t v3_encode_size(const ShellChunkResult *r);

/* Full pipeline encode — returns metrics */
ShellMetrics v3_shell_encode(const uint8_t *data, uint64_t n_chunks,
                              uint8_t layer, int use_batch);

#endif /* DIAMOND_SHELL_V3_H */