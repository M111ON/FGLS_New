/* ═══════════════════════════════════════════════════════════════
 * geofield_pipeline.h — Public API: GeoField Compression Library
 * ═══════════════════════════════════════════════════════════════
 *
 * Integrates:
 *   GeoField topology    → Goldberg sphere scatter (FrustumBlock × 54)
 *   Diamond Shell codec  → rotation / sub-block entropy coding
 *   Huffman entropy      → global byte-level post-compression
 *   Temporal Ring        → stride-37 geometric addressing
 *
 * Pipeline:
 *   input bytes → 64B chunks → GpSphere scatter → FrustumBlocks
 *   → Diamond Shell (geometric-neighbor delta) → Huffman → GFUF
 *
 * Build:
 *   gcc -O2 -Icollection/geopixel/geofield -I. \
 *       pipeline/geofield_pipeline.c -o geofield_pipeline.exe
 *
 * ═══════════════════════════════════════════════════════════════ */

#ifndef GEOFIELD_PIPELINE_H
#define GEOFIELD_PIPELINE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compressed format ────────────────────────────────────────
 * GFUF v3:
 *   [Header:64B] [BlockEncs:n_blocks×2B] [BlockOffs:(n_blocks+1)×4B]
 *   [BlockData:...]
 *
 *   BlockData layout (per FrustumBlock = 54 chunks):
 *     [seed_ds_sz:1] [seed_ds:seed_ds_sz]
 *     [res_0_sz:1][res_0_ds] ... [res_52_sz:1][res_52_ds]
 *
 *   Each ds entry is a Diamond Shell encoded 64B block.
 *   res_N = chunk[N] XOR chunk[N-1]  (geometric-walk adjacent delta)
 *   If huff_compressed flag set, BlockData is globally Huffman-coded.
 *
 *   File boundary: header + block_encs + block_offs + compressed_data
 * ─────────────────────────────────────────────────────────── */

#define GEOFIELD_MAGIC    0x46554647u   /* "GFUF" */
#define GEOFIELD_VER      3
#define GEOFIELD_FLAG_HUFF  0x0001u     /* block data Huffman-compressed */
#define GEOFIELD_HDR_SZ   64

/* ── Pipeline config ────────────────────────────────────────── */
typedef struct {
    uint8_t  gp_level;        /* 1..8 Goldberg subdivision depth   */
    uint16_t flags;           /* compression flags                  */
} GeoFieldPipeConfig;

#define GEOFIELD_PIPE_DEFAULT  { .gp_level = 2, .flags = GEOFIELD_FLAG_HUFF }

/* ── Info about compressed data ─────────────────────────────── */
typedef struct {
    uint32_t orig_size;       /* original data size                 */
    uint32_t n_chunks;        /* number of 64B chunks               */
    uint32_t n_blocks;        /* number of FrustumBlocks (×54)      */
    uint32_t comp_size;       /* compressed file size               */
    uint8_t  gp_level;        /* Goldberg subdivision depth used    */
    int      huff_used;       /* was Huffman post-compression used? */
    double   ratio;           /* compression ratio                  */
} GeoFieldInfo;

/* ── Core API ────────────────────────────────────────────────── */

/* Compress: input bytes → GFUF-format compressed buffer.
 * Returns 0 on success, -1 on error.
 * *output is allocated by the function; caller frees with geofield_free(). */
int geofield_compress(const uint8_t *input, uint32_t input_sz,
                      uint8_t **output, uint32_t *output_sz,
                      const GeoFieldPipeConfig *cfg);

/* Decompress: GFUF-format buffer → original bytes.
 * Returns 0 on success, -1 on error.
 * *output is allocated by the function; caller frees with geofield_free(). */
int geofield_decompress(const uint8_t *input, uint32_t input_sz,
                        uint8_t **output, uint32_t *output_sz);

/* Get info about compressed GFUF data without decompressing.
 * Returns 0 on success, -1 on invalid format. */
int geofield_info(const uint8_t *input, uint32_t input_sz,
                  GeoFieldInfo *info);

/* Free buffer allocated by compress/decompress. */
void geofield_free(uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* GEOFIELD_PIPELINE_H */
