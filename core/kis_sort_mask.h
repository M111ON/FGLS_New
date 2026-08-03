/* ═══════════════════════════════════════════════════════════════════════════
 * kis_sort_mask.h — Sort + RLE Compression (Lossless on Repetitive Data)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * หลักการ MAP NOT COMPRESS จาก user:
 *   นำ weight มาที่ sort → ค่าที่ซ้ำกันจะติดกันเป็น run → RLE เก็บ (count, val)
 *   color bucket (0-9) ทำ position → mask แบบ active/not-active ที่บีบง่าย
 *
 * Encode (lossless บน data ที่มี repetition หลัง sort):
 *   1. copy weights → sorted[] = {val, original idx}
 *   2. qsort by val
 *   3. กลุ่มค่าที่เท่ากัน → codebook (distinct values + counts)
 *   4. เก็บ permutation เป็น delta-varint (เพื่อ unsort กลับ)
 *
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef KIS_SORT_MASK_H
#define KIS_SORT_MASK_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define KSM_DIRECTIONS      6u
#define KSM_ACTIVE_PER_DIR  1000u
#define KSM_TOTAL_ACTIVE    (KSM_ACTIVE_PER_DIR * KSM_DIRECTIONS)

typedef struct {
    float val;
    uint32_t idx;
} KSM_Item;

static int ksm_cmp_item(const void *a, const void *b)
{
    const KSM_Item *ia = (const KSM_Item*)a;
    const KSM_Item *ib = (const KSM_Item*)b;
    if (ia->val < ib->val) return -1;
    if (ia->val > ib->val) return 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   VARINT — zigzag delta encoding
   ═══════════════════════════════════════════════════════════════ */

static inline uint32_t varint_encode(uint32_t val, uint8_t *out)
{
    uint32_t len = 0;
    do { out[len++] = (val & 0x7F) | (val >> 7 ? 0x80 : 0); val >>= 7; } while (val);
    return len;
}

static inline uint32_t varint_decode(const uint8_t *in, uint32_t *val)
{
    uint32_t v = 0, s = 0, l = 0;
    do { v |= ((uint32_t)(in[l] & 0x7F)) << s; s += 7; } while (in[l++] & 0x80);
    *val = v; return l;
}

/* ═══════════════════════════════════════════════════════════════
   CODEBOOK — distinct values + counts (the sorted-RLE)
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t n_distinct;
    float   *uniq;
    uint32_t *counts;
} KSM_Codebook;

/* Build codebook from a SORTED array of values. */
static inline int ksm_codebook_build(KSM_Codebook *cb, const float *sv, uint32_t n)
{
    if (!cb || !sv || n == 0) return -1;
    uint32_t nd = 1;
    for (uint32_t i = 1; i < n; i++)
        if (sv[i] != sv[i-1]) nd++;

    cb->uniq = (float*)malloc(nd * sizeof(float));
    cb->counts = (uint32_t*)malloc(nd * sizeof(uint32_t));
    if (!cb->uniq || !cb->counts) return -1;
    cb->n_distinct = nd;

    uint32_t u = 0;
    cb->uniq[u] = sv[0];
    cb->counts[u] = 1;
    for (uint32_t i = 1; i < n; i++) {
        if (sv[i] == sv[i-1]) cb->counts[u]++;
        else { u++; cb->uniq[u] = sv[i]; cb->counts[u] = 1; }
    }
    return 0;
}

static inline void ksm_codebook_free(KSM_Codebook *cb)
{
    free(cb->uniq); cb->uniq = NULL;
    free(cb->counts); cb->counts = NULL;
    cb->n_distinct = 0;
}

/* ═══════════════════════════════════════════════════════════════
   CODEC
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    const float *weights;
    uint32_t n_weights;

    KSM_Item *sorted;     /* {val, orig idx} sorted by val */
    KSM_Codebook cb;      /* codebook of sorted values */

    uint32_t perm_bytes;
    uint32_t codebook_bytes;
    uint32_t total_bytes;
    float compression_ratio;
} KSM_Codec;

static inline int ksm_init(KSM_Codec *c, const float *weights, uint32_t n_weights)
{
    if (!c || !weights || n_weights == 0) return -1;
    memset(c, 0, sizeof(*c));
    c->weights = weights;
    c->n_weights = n_weights;
    c->sorted = (KSM_Item*)malloc(n_weights * sizeof(KSM_Item));
    if (!c->sorted) return -1;
    for (uint32_t i = 0; i < n_weights; i++) {
        c->sorted[i].val = weights[i];
        c->sorted[i].idx = i;
    }
    return 0;
}

static inline void ksm_free(KSM_Codec *c)
{
    free(c->sorted);
    ksm_codebook_free(&c->cb);
}

/* Encode: sort, codebook, cost estimates (in-memory demo) */
static inline int ksm_encode(KSM_Codec *c)
{
    if (!c || !c->sorted) return -2;

    qsort(c->sorted, c->n_weights, sizeof(KSM_Item), ksm_cmp_item);

    /* Build codebook by scanning sorted values (bounded rle). */
    /* First compute distinct */
    static float *sv_scratch = NULL;        /* unused, keep small */
    (void)sv_scratch;
    {
        /* copy into temp float array for codebook build */
        float *tmp = (float*)malloc(c->n_weights * sizeof(float));
        if (!tmp) return -1;
        for (uint32_t i = 0; i < c->n_weights; i++) tmp[i] = c->sorted[i].val;
        if (ksm_codebook_build(&c->cb, tmp, c->n_weights) != 0) { free(tmp); return -1; }
        free(tmp);
    }

    /* Permutation delta cost (sorted[i].idx → original order) */
    uint8_t bytes[8];
    uint32_t pcost = 0;
    int32_t prev = -1;
    for (uint32_t i = 0; i < c->n_weights; i++) {
        int32_t cur = (int32_t)c->sorted[i].idx;
        int32_t delta = (prev < 0) ? cur : (cur - prev);
        uint32_t zz = (uint32_t)((delta << 1) ^ (delta >> 31));
        pcost += varint_encode(zz, bytes);
        prev = cur;
    }
    c->perm_bytes = pcost;

    /* Codebook cost: float values (4B each) + counts (varint each) */
    uint32_t cbcost = 0;
    uint8_t vbuf[8];
    for (uint32_t i = 0; i < c->cb.n_distinct; i++) {
        cbcost += 4;
        cbcost += varint_encode(c->cb.counts[i], vbuf);
    }
    /* Sorting order is implicit in codebook; but for reconstruction we
       ALSO need to know positions.  In this prototype the perm deltas
       ARE the reconstruction from codebook: to rebuild the full sorted
       list we emit each `count` copies of `uniq[i]`. */
    c->codebook_bytes = cbcost;

    c->total_bytes = pcost + cbcost;
    uint32_t ob = c->n_weights ? c->n_weights * 4u : 1u;
    c->compression_ratio = (float)ob / (float)(c->total_bytes ? c->total_bytes : 1);
    return 0;
}

/* Decode: reconstruct from sorted array (in-memory) */
static inline int ksm_decode(KSM_Codec *c, float *output, uint32_t output_n)
{
    if (!c || !output || output_n < c->n_weights) return -1;
    memset(output, 0, output_n * sizeof(float));
    for (uint32_t i = 0; i < c->n_weights; i++) {
        uint32_t idx = c->sorted[i].idx;
        if (idx < output_n) output[idx] = c->sorted[i].val;
    }
    return 0;
}

static inline int ksm_verify(KSM_Codec *c)
{
    float *recon = (float*)malloc(c->n_weights * sizeof(float));
    if (!recon) return -1;
    memset(recon, 0, c->n_weights * sizeof(float));
    if (ksm_decode(c, recon, c->n_weights) != 0) { free(recon); return -1; }
    uint32_t mm = 0;
    for (uint32_t i = 0; i < c->n_weights; i++) {
        float a = c->weights[i], b = recon[i];
        if (a != b && !(a != a && b != b)) mm++;
    }
    free(recon);
    return mm ? -1 : 0;
}

static inline void ksm_print_stats(const KSM_Codec *c)
{
    printf("===============================================================\n");
    printf("  KIS Sort-Mask Codec\n");
    printf("===============================================================\n");
    printf("  Input weights:    %u (%.2f MB raw)\n",
           c->n_weights, c->n_weights * 4.0 / 1048576.0);
    printf("  Distinct values:  %u\n", c->cb.n_distinct);
    printf("  Codebook bytes:   %u\n", c->codebook_bytes);
    printf("  Perm bytes:       %u\n", c->perm_bytes);
    printf("  Total compressed: %u\n", c->total_bytes);
    printf("  Compression ratio: %.2fx\n", c->compression_ratio);
    printf("===============================================================\n");
}

#endif /* KIS_SORT_MASK_H */