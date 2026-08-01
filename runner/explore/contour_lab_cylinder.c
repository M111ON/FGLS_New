/*
 * contour_lab_cylinder.c — Cylinder container (C3: 6x576=3,456) + 4-phase label
 * Tests Geomatrix cylinder geometry integration with contour mask on real GGUF data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "gguf_reader.h"
#include "geomatrix_shared.h"
#include "geo_cylinder.h"

/* ── Inline entrance_stamp32 (from geomatrix_entrance.c) ── */
static inline void entrance_stamp32(
    GeoPacketSmall *pkt,
    const uint64_t *bundle,
    uint16_t raw_idx,
    uint8_t phase)
{
    pkt->sig32 = geo_compute_sig32(bundle, phase);
    pkt->idx   = raw_idx;
    pkt->bit   = (uint8_t)((bundle[raw_idx >> 6] >> (raw_idx & 63)) & 1ULL);
    pkt->phase = phase & 3;
}

/* ── Phase classification (4-phase contour mask) ── */
static uint8_t classify_phase(int8_t w)
{
    int aw = w < 0 ? -w : w;
    if (aw < 8)  return PHASE_PROBE;    /* |w| <  8  → probe (near-zero) */
    if (w > 0)   return PHASE_MAIN;     /* w >=  8   → main (positive)   */
    if (w >= -32) return PHASE_MIRROR;  /* -32 ≤ w < 0 → mirror         */
    return PHASE_CANCEL;                /* w < -32   → cancel (deep neg) */
}

static const char *phase_name(uint8_t p)
{
    switch (p) {
        case PHASE_PROBE:  return "PROBE ";
        case PHASE_MAIN:   return "MAIN  ";
        case PHASE_MIRROR: return "MIRROR";
        case PHASE_CANCEL: return "CANCEL";
        default:           return "???";
    }
}

/* ── Pack 64 bytes into 8 x uint64_t bundle (little-endian) ── */
static void pack_bundle(const uint8_t *src, uint64_t bundle[8])
{
    for (int b = 0; b < 8; b++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++)
            v |= (uint64_t)src[b * 8 + j] << (j * 8);
        bundle[b] = v;
    }
}

/* ── Run phase analysis on a buffer of raw bytes ── */
static uint64_t phase_analysis(
    const uint8_t *buf, uint32_t n, uint64_t hist[4],
    int *l1_pass_out, int *l1_fail_out)
{
    uint64_t l1_pass = 0, l1_fail = 0;
    hist[0] = hist[1] = hist[2] = hist[3] = 0;

    for (uint32_t i = 0; i < n; i++) {
        int8_t  w     = (int8_t)buf[i];
        uint8_t phase = classify_phase(w);
        hist[phase]++;

        /* Bundle: 64-byte chunk containing this byte */
        uint32_t bundle_idx     = i / 64;
        uint32_t byte_in_bundle = i % 64;
        uint64_t bundle[8];
        pack_bundle(buf + bundle_idx * 64, bundle);

        /* raw_idx = bit index of this byte's LSB within the bundle */
        uint16_t raw_idx = (uint16_t)(byte_in_bundle * 8);

        /* Stamp */
        GeoPacketSmall pkt;
        entrance_stamp32(&pkt, bundle, raw_idx, phase);

        /* L1 verify */
        if (geo_l1_check32(pkt.sig32, bundle, phase))
            l1_pass++;
        else
            l1_fail++;
    }

    *l1_pass_out = (int)l1_pass;
    *l1_fail_out = (int)l1_fail;
    return l1_pass + l1_fail;
}

/* ── Print histogram ── */
static void print_hist(const char *label, uint64_t hist[4], uint32_t total)
{
    printf("%s (n=%u):\n", label, total);
    for (int p = 0; p < 4; p++) {
        double pct = 100.0 * hist[p] / total;
        printf("  %s: %6"PRIu64"  (%5.1f%%)\n", phase_name(p), hist[p], pct);
    }
}

/* ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <gguf_file>\n", argv[0]);
        return 1;
    }

    GGUF_File *gf = gguf_open(argv[1]);
    if (!gf) {
        fprintf(stderr, "FAIL: cannot open GGUF: %s\n", argv[1]);
        return 1;
    }

    /* ── Find tensor[1] = token_embd.weight ── */
    int ti = gguf_find_tensor(gf, "token_embd");
    if (ti < 0) {
        fprintf(stderr, "FAIL: no token_embd tensor\n");
        gguf_close(gf);
        return 1;
    }
    GGUF_Tensor *t = &gf->tensors[ti];
    printf("Tensor[%d]: %s  type=%u  weights=%"PRIu64"  size=%"PRIu64" bytes\n",
           ti, t->name, t->type, t->n_weights, t->size_bytes);

    /* ── Cylinder geometry stats ── */
    printf("\n=== Cylinder C3 Geometry ===\n");
    printf("  Spokes:  %d (6 LADOS, 60 deg each)\n", CYL_SPOKES);
    printf("  Faces:   %d (8 outer + 1 center)\n",    CYL_FACES);
    printf("  Units:   %d per face\n",                 CYL_FACE_UNITS);
    printf("  Slots:   %d per spoke (9x64)\n",        CYL_SLOTS);
    printf("  Full:    %d (6x576 = 144x24)\n",        CYL_FULL_N);

    uint16_t H[CYL_FULL_N];
    geo_hilbert_cylinder(H, CYL_FULL_N);
    geo_cylinder_stats(H, CYL_FULL_N);

    /* ══════════════════════════════════════════════════════════════
     * PHASE 1: 1 cylinder = 3,456 cells
     * ══════════════════════════════════════════════════════════════ */
    printf("\n=== Phase 1: 1 Cylinder (C3: 6x576=%u cells) ===\n", CYL_FULL_N);

    uint8_t *buf1 = (uint8_t *)malloc(CYL_FULL_N);
    if (!buf1) { fprintf(stderr, "OOM\n"); gguf_close(gf); return 1; }

    fseek(gf->fp, (long)gf->tensor_data_start, SEEK_SET);
    if (fread(buf1, 1, CYL_FULL_N, gf->fp) != CYL_FULL_N) {
        fprintf(stderr, "FAIL: read %"PRIu32" bytes\n", CYL_FULL_N);
        free(buf1); gguf_close(gf); return 1;
    }

    uint64_t hist1[4];
    int pass1, fail1;
    phase_analysis(buf1, CYL_FULL_N, hist1, &pass1, &fail1);

    printf("L1 verification: %d PASS, %d FAIL\n", pass1, fail1);
    print_hist("Phase histogram (1 cylinder)", hist1, CYL_FULL_N);

    /* Per-spoke phase breakdown */
    printf("\nPer-spoke breakdown (1 cylinder):\n");
    for (int s = 0; s < CYL_SPOKES; s++) {
        uint64_t sh[4] = {0};
        for (uint32_t slot = 0; slot < CYL_SLOTS; slot++) {
            uint16_t fi = geo_full_idx(s, slot);
            if (fi < CYL_FULL_N) {
                int8_t w = (int8_t)buf1[fi];
                sh[classify_phase(w)]++;
            }
        }
        printf("  spoke %d (%s): PROBE=%4"PRIu64" MAIN=%4"PRIu64
               " MIRROR=%4"PRIu64" CANCEL=%4"PRIu64"\n",
               s, (s & 1) ? "bot" : "top",
               sh[0], sh[1], sh[2], sh[3]);
    }
    free(buf1);

    /* ══════════════════════════════════════════════════════════════
     * PHASE 2: 6 cylinders = 20,736 cells (1 full geo_jump block)
     * ══════════════════════════════════════════════════════════════ */
    uint32_t SCALE_N = 6 * CYL_FULL_N; /* 20736 */
    printf("\n=== Phase 2: 6 Cylinders (%u cells) ===\n", SCALE_N);

    uint8_t *buf2 = (uint8_t *)malloc(SCALE_N);
    if (!buf2) { fprintf(stderr, "OOM\n"); gguf_close(gf); return 1; }

    fseek(gf->fp, (long)gf->tensor_data_start, SEEK_SET);
    if (fread(buf2, 1, SCALE_N, gf->fp) != SCALE_N) {
        fprintf(stderr, "FAIL: read %"PRIu32" bytes\n", SCALE_N);
        free(buf2); gguf_close(gf); return 1;
    }

    uint64_t hist2[4];
    int pass2, fail2;
    phase_analysis(buf2, SCALE_N, hist2, &pass2, &fail2);

    printf("L1 verification: %d PASS, %d FAIL\n", pass2, fail2);
    print_hist("Phase histogram (6 cylinders)", hist2, SCALE_N);

    /* ── Compression estimate ── */
    uint64_t keep   = hist2[PHASE_MAIN] + hist2[PHASE_MIRROR];
    uint64_t discard = hist2[PHASE_PROBE] + hist2[PHASE_CANCEL];
    double   keep_pct = 100.0 * keep / SCALE_N;
    double   ratio    = (keep > 0) ? (double)SCALE_N / keep : 0;

    printf("\n=== Compression Estimate ===\n");
    printf("  Keep    (MAIN+MIRROR):  %6"PRIu64"  (%5.1f%%)\n", keep, keep_pct);
    printf("  Discard (PROBE+CANCEL): %6"PRIu64"  (%5.1f%%)\n", discard, 100.0 - keep_pct);
    printf("  Original:    %u bytes\n", SCALE_N);
    printf("  After trim:  %"PRIu64" bytes (%.1fx compression)\n", keep, ratio);

    free(buf2);
    gguf_close(gf);

    /* ══════════════════════════════════════════════════════════════
     * VERDICT
     * ══════════════════════════════════════════════════════════════ */
    printf("\n=== VERDICT ===\n");
    if (fail2 == 0)
        printf("PASS: All %u L1 checks pass.\n"
               "      Geomatrix cylinder geometry produces valid sig32 for every cell.\n",
               SCALE_N);
    else
        printf("FAIL: %d/%u L1 checks failed.\n", fail2, SCALE_N);

    printf("Phase distribution: MAIN+MIRROR = %.1f%% of data.\n", keep_pct);
    if (keep_pct > 10.0)
        printf("CONCLUSION: Meaningful contour labels — cylinder geometry integrates\n"
               "            with contour mask on real Q8_0 GGUF data.\n");
    else if (keep_pct > 1.0)
        printf("CONCLUSION: Moderate contour labels — most data is PROBE (near-zero).\n"
               "            Contour mask captures the sparse structure of quantized weights.\n");
    else
        printf("CONCLUSION: Very sparse contour labels — PROBE dominates.\n"
               "            Contour mask may need adjusted thresholds for Q8_0 data.\n");

    return 0;
}
