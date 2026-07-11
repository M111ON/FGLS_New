/*
 * pogls_geopixel.h — Standalone Geopixel Spatial Coherence Compression
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Compresses 64-byte blocks by exploiting spatial coherence:
 *   FLAT     (0x00): All 64 bytes identical           → 2 bytes
 *   SMOOTH   (0x01): Near-constant (|byte-mean| ≤ 16) → 10 bytes
 *   GRADIENT (0x02): Piecewise-linear trend            → 10 bytes
 *   EDGE     (0x03): Random/no pattern (raw)           → 65 bytes
 *
 * Full tensor encode splits data into 64-byte blocks and emits a tagged
 * stream. Decoder dispatches on tag byte per block.
 *
 * Hilbert 2D curve maps 8×8 block coordinates to a 1D index (order=3).
 *
 * Session feed tracks incremental frames + block accumulation.
 *
 * No libpng, no file I/O, no heap in hot path.
 * Dependencies: <stdint.h> <stddef.h> <string.h> <stdio.h>
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_GEOPIXEL_H
#define POGLS_GEOPIXEL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════ */

#define POGLS_GEOPIXEL_BLOCK_SIZE  64u

/* Block type tags */
#define POGLS_GEOPIXEL_FLAT      0x00u   /* 2 bytes:  [tag][value]            */
#define POGLS_GEOPIXEL_SMOOTH    0x01u   /* 10 bytes: [tag][mean][res0..res7] */
#define POGLS_GEOPIXEL_GRADIENT  0x02u   /* 10 bytes: [tag][slope][icpt][r0..r6] */
#define POGLS_GEOPIXEL_EDGE      0x03u   /* 65 bytes: [tag][raw 64 bytes]     */

/* Encoded sizes per mode (tag byte included) */
#define POGLS_GEOPIXEL_FLAT_SZ      2u
#define POGLS_GEOPIXEL_SMOOTH_SZ   10u
#define POGLS_GEOPIXEL_GRADIENT_SZ 10u
#define POGLS_GEOPIXEL_EDGE_SZ     65u

/* SMOOTH threshold: max |byte - mean| for a block to be "smooth" */
#define POGLS_GEOPIXEL_SMOOTH_MAX_DIFF  16

/* ═══════════════════════════════════════════════════════════════════════
   SESSION
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t frame_seq;
    uint32_t block_count;
    uint64_t total_bytes;
    uint64_t compressed_bytes;
} PoglsGeopixelSession;

/* ═══════════════════════════════════════════════════════════════════════
   BLOCK ENCODE / DECODE
   ═══════════════════════════════════════════════════════════════════════
 * Encode one 64-byte block into tagged representation.
 * Returns bytes written to dst, or 0 on error/capacity exceeded.
 * dst must be at least POGLS_GEOPIXEL_EDGE_SZ (65) bytes. */
uint32_t pogls_geopixel_encode_block(uint8_t *dst, size_t dst_cap,
                                     const uint8_t *src, size_t block_sz);

/* Decode one tagged block back to 64 bytes.
 * Returns bytes written to dst (always 64 on success), or 0 on error. */
uint32_t pogls_geopixel_decode_block(uint8_t *dst, size_t dst_cap,
                                     const uint8_t *src, size_t src_sz);

/* ═══════════════════════════════════════════════════════════════════════
   FULL TENSOR ENCODE / DECODE
   ═══════════════════════════════════════════════════════════════════════
 * Encode entire tensor by splitting into 64-byte blocks.
 * Returns total bytes written, or 0 on error.
 * Partial final block is zero-padded. */
uint32_t pogls_geopixel_encode(uint8_t *dst, size_t dst_cap,
                               const uint8_t *src, size_t src_sz);

/* Decode entire tagged stream back to original size.
 * Returns total bytes written, or 0 on error.
 * Result is always rounded up to multiple of 64. */
uint32_t pogls_geopixel_decode(uint8_t *dst, size_t dst_cap,
                               const uint8_t *src, size_t src_sz);

/* ═══════════════════════════════════════════════════════════════════════
   HILBERT 2D CURVE (order=3 for 8x8 block)
   ═══════════════════════════════════════════════════════════════════════ */

/* Convert (x,y) to 1D Hilbert index d for given order (bits). */
uint32_t pogls_geopixel_hilbert_xy_to_d(uint32_t x, uint32_t y, uint32_t order);

/* Convert 1D Hilbert index d back to (x,y). */
void     pogls_geopixel_hilbert_d_to_xy(uint32_t d, uint32_t order,
                                        uint32_t *x, uint32_t *y);

/* ═══════════════════════════════════════════════════════════════════════
   SESSION FEED
   ═══════════════════════════════════════════════════════════════════════ */

/* Initialize session with expected block count. */
int      pogls_geopixel_session_init(PoglsGeopixelSession *s, uint32_t blocks);

/* Feed one tensor into session: encodes and returns bytes written.
 * Accumulates frame_seq and total_bytes. */
uint32_t pogls_geopixel_session_feed(PoglsGeopixelSession *s,
                                     uint8_t *dst, size_t dst_cap,
                                     const uint8_t *src, size_t src_sz);

/* Print session statistics to stdout. */
void     pogls_geopixel_session_stats(const PoglsGeopixelSession *s);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_GEOPIXEL_H */
