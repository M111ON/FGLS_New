/*
 * pogls_bermuda.h — Bermuda Geometry Router + Compression (Standalone)
 *
 * Stride-37 Hilbert routing over 20736-address icosahedral space.
 * Diamond Shell + RLE compression. Shadow bond key-value store.
 *
 * All integer arithmetic — no float ops on hotpath.
 * No external dependencies beyond <stdint.h>, <stddef.h>, <string.h>.
 */

#ifndef POGLS_BERMUDA_H
#define POGLS_BERMUDA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────────────────────────────────────── */

#define POGLS_BERMUDA_MAX_ADDR      20736u
#define POGLS_BERMUDA_STRIDE          37u
#define POGLS_BERMUDA_N_ZONES         12u
#define POGLS_BERMUDA_CHUNK_SZ        64u
#define POGLS_BERMUDA_ROT_STATES       6u

#define POGLS_BERMUDA_FLAG_FLAT        0u
#define POGLS_BERMUDA_FLAG_DENSE       2u

#define POGLS_BERMUDA_SHADOW_CAP      144u

/* ── BermudeFaceScorer ──────────────────────────────────── */

typedef struct {
    uint32_t base;
    uint32_t scores[12];
    uint32_t best_face;
    uint32_t best_score;
} BermudeFaceScorer;

/* ── PoglsDiamondRLE ────────────────────────────────────── */

typedef struct {
    uint8_t  flag;
    uint8_t  best_rot;
    uint8_t  data[POGLS_BERMUDA_CHUNK_SZ];
    uint32_t enc_size;
} PoglsDiamondRLE;

/* ── Stride-37 Routing ──────────────────────────────────── */

uint32_t pogls_bermuda_route(uint32_t from, uint32_t face, uint32_t stride);
void     pogls_bermuda_face_scores(uint32_t base, float scores[12]);

/* ── Diamond Shell Compression ──────────────────────────── */

uint32_t pogls_bermuda_diamond_compress(uint8_t *dst, size_t dst_cap,
                                        const uint8_t *src, size_t src_sz);
uint32_t pogls_bermuda_diamond_decompress(uint8_t *dst, size_t dst_cap,
                                          const uint8_t *src, size_t src_sz);

/* ── RLE Compression ────────────────────────────────────── */

uint32_t pogls_bermuda_rle_compress(uint8_t *dst, size_t dst_cap,
                                    const uint8_t *src, size_t src_sz);
uint32_t pogls_bermuda_rle_decompress(uint8_t *dst, size_t dst_cap,
                                      const uint8_t *src, size_t src_sz);

/* ── Shadow Bond ────────────────────────────────────────── */

int pogls_bermuda_shadow_write(const char *key, const uint8_t *data, size_t sz);
int pogls_bermuda_shadow_read(const char *key, uint8_t *data, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_BERMUDA_H */
