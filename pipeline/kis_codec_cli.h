/* ════════════════════════════════════════════════════════════════════════════
 * kis_codec_cli.c — Integration of kis_codec_v4 into FGLS CLI
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * This file adds the KIS geometric codec as a new route in the FGLS pipeline.
 * Route ID: FGLS_ROUTE_KIS (added to fgls_profile.h)
 *
 * The KIS codec is optimized for Q8_0 quantized weights (GGUF tensors).
 * It uses codebook + permutation delta encoding for lossless compression.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef KIS_CODEC_CLI_H
#define KIS_CODEC_CLI_H

#include <stdint.h>
#include "../core/kis_codec_v4.h"
#include "../collection/fgls_profile.h"

/* Extend FglsRoute enum - add KIS route after FRAMED */
#define FGLS_ROUTE_KIS 9

/* Update FGLS_ROUTE_COUNT in fgls_profile.h to 10 */

/* ═══════════════════════════════════════════════════════════════════════════
 * KIS Route Detection
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Check if data looks like Q8_0 weights (GGUF tensor)
 * Heuristics:
 *   - All values in int8 range (-128..127)
 *   - Multiple distinct values (not flat)
 *   - Structured distribution (not pure random)
 *   - Size is multiple of 32 (Q8_0 block size = 32 weights + 2B scale)
 */
static inline int fgls_is_kis_candidate(const uint8_t *data, uint32_t size) {
    if (size < 64 || size > (1u << 24)) return 0;
    if (size % 32 != 0) return 0;  /* Q8_0 blocks are 32 weights */

    /* Quick scan: check value range and distribution */
    uint32_t hist[256] = {0};
    uint32_t nz = 0, maxv = 0, minv = 255;
    for (uint32_t i = 0; i < size; i++) {
        uint8_t v = data[i];
        hist[v]++;
        if (v != 0) nz++;
        if (v > maxv) maxv = v;
        if (v < minv) minv = v;
    }

    /* Must use full int8 range (not just low values) */
    if (maxv < 128 && maxv > 0) return 0;

    /* Not too sparse, not too dense */
    uint32_t nz_pct = (nz * 100u) / size;
    if (nz_pct < 50 || nz_pct > 100) return 0;

    /* Multiple distinct values */
    uint32_t distinct = 0;
    for (int v = 0; v < 256; v++) if (hist[v]) distinct++;
    if (distinct < 10 || distinct > 200) return 0;

    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * KIS Encoder/Decoder (adapted for chunk-based pipeline)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Encode Q8_0 weight chunk using KIS codec v4
 * Input: 64 bytes of raw Q8_0 weights (no scale bytes)
 * Output: encoded payload (route byte handled by dispatcher)
 * Returns: payload size, or -1 on error */
static int enc_kis(uint8_t *out, uint32_t out_cap,
                   const uint8_t *data, uint32_t size)
{
    if (size == 0 || size > 65535) return -1;
    if (out_cap < size + 1024) return -1; /* need room for worst case */

    /* KIS codec expects int8_t weights */
    const int8_t *weights = (const int8_t *)data;

    /* Encode with KIS v4 */
    uint32_t encoded = kis_v4_encode(weights, size, out, out_cap);
    if (encoded == 0) return -1;

    return (int)encoded;
}

/* Decode KIS payload back to Q8_0 weights
 * Returns: bytes consumed from payload, or -1 on error */
static int dec_kis(uint8_t *out, uint32_t out_size,
                   const uint8_t *payload, uint32_t payload_cap)
{
    if (payload_cap < 8) return -1;
    if (out_size > 65535) return -1;

    int8_t *weights = (int8_t *)out;

    /* Decode with KIS v4 */
    int rc = kis_v4_decode(payload, payload_cap, weights, out_size);
    if (rc != 0) return -1;

    return (int)payload_cap; /* all payload consumed */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Route Name (for CLI output)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline const char *fgls_route_name_kis(FglsRoute r) {
    if (r == FGLS_ROUTE_KIS) return "KIS";
    return "UNKNOWN";
}

#endif /* KIS_CODEC_CLI_H */