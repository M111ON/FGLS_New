/* ═══════════════════════════════════════════════════════════════════════════
 * kis_codec.h — Data Codec (codebook + RLE — global scope)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Q8_0 weights → histogram → codebook 256B + RLE counts.
 * No position stored — position comes from classify_bond geometry.
 */
#ifndef KIS_CODEC_H
#define KIS_CODEC_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Codec state container ──────────────────────────────────── */

typedef struct {
    uint64_t n_weights;
    uint32_t histo[256];       /* count per Q8 value 0..255 */
    uint8_t  active[32];       /* bitmap: active[byte] = which values present */
    uint32_t n_active;         /* distinct values */
    uint32_t rle_bytes;        /* varint RLE output bytes */
    uint32_t codebook_bytes;   /* 32 always */
} KisCodec;

/* ── Codebook build ──────────────────────────────────────────── */
static int kis_codec_build(KisCodec *c, const int8_t *data, uint64_t n)
{
    if (!c || !data || n == 0) return -1;

    memset(c, 0, sizeof(*c));
    c->n_weights = n;

    for (uint64_t i = 0; i < n; i++) {
        uint8_t v = (uint8_t)(data[i] + 128);
        c->histo[v]++;
    }

    for (int v = 0; v < 256; v++) {
        if (c->histo[v] > 0) {
            c->active[v >> 3] |= (uint8_t)(1u << (v & 7));
            c->n_active++;
        }
    }

    c->codebook_bytes = 32;
    return 0;
}

/* ── RLE encode ──────────────────────────────────────────────── */
static int kis_rle_encode(KisCodec *c, uint8_t *out, uint32_t cap)
{
    if (!c || !out || cap < 2048) return -1;

    uint32_t pos = 0;
    for (int v = 0; v < 256 && pos < cap; v++) {
        uint32_t cnt = c->histo[v];
        if (cnt == 0) continue;

        while (cnt >= 0x80) {
            if (pos >= cap) return -2;
            out[pos++] = (uint8_t)(cnt & 0x7F) | 0x80u;
            cnt >>= 7;
        }
        if (pos >= cap) return -2;
        out[pos++] = (uint8_t)cnt;
    }

    c->rle_bytes = pos;
    return 0;
}

/* ── RLE decode ──────────────────────────────────────────────── */
static int kis_rle_decode(const uint8_t *active, const uint8_t *rle,
                           uint32_t rle_len, int8_t *out, uint64_t out_n)
{
    if (!active || !rle || !out) return -1;

    uint64_t pos = 0;
    uint32_t r = 0;

    for (int v = 0; v < 256 && r < rle_len && pos < out_n; v++) {
        if (!(active[v >> 3] & (1u << (v & 7))))
            continue;

        uint32_t cnt = 0;
        uint32_t shift = 0;
        for (; r < rle_len; ) {
            uint8_t byte = rle[r++];
            cnt |= (uint32_t)(byte & 0x7F) << shift;
            shift += 7;
            if (!(byte & 0x80)) break;
        }

        int8_t val = (int8_t)(v - 128);
        for (uint32_t i = 0; i < cnt && pos < out_n; i++)
            out[pos++] = val;
    }

    return (pos == out_n) ? 0 : -2;
}

/* ── Verify histograms match ──────────────────────────────────── */
static int kis_codec_verify(KisCodec *c, const int8_t *original,
                              const int8_t *decoded)
{
    if (!c || !original || !decoded) return -1;

    uint32_t hist[256] = {0};
    for (uint64_t i = 0; i < c->n_weights; i++) {
        uint8_t v = (uint8_t)(decoded[i] + 128);
        hist[v]++;
    }

    for (int i = 0; i < 256; i++) {
        if (hist[i] != c->histo[i]) {
            fprintf(stderr, "  Mismatch val=%d: orig=%u dec=%u\n",
                    i, c->histo[i], hist[i]);
            return -1;
        }
    }
    return 0;
}

/* ── Print stats ───────────────────────────────────────────────── */
static void kis_codec_print(KisCodec *c)
{
    if (!c) return;
    printf("╔══ KIS CODEC ═══════════════════╗\n");
    printf("║ Weights:  %lu\n", (unsigned long)c->n_weights);
    printf("║ Distinct: %u / 256\n", c->n_active);
    uint32_t total = c->codebook_bytes + c->rle_bytes;
    printf("║ Codebook: %uB + RLE: %uB = %uB\n",
           c->codebook_bytes, c->rle_bytes, total);
    printf("║ Raw: %.2f MB  Codec: %.2f KB  Ratio: %.0fx\n",
           c->n_weights * 1.0 / 1048576.0,
           total / 1024.0,
           c->n_weights * 1.0 / (double)(total ? total : 1));
    printf("╚════════════════════════════════╝\n");
}

#endif /* KIS_CODEC_H */