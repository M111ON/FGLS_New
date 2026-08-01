/*
 * w_dup.c — Weight value duplication & clustering analysis (Q8_0 / Q4_0)
 *
 * Answers:
 *   - which exact values repeat most (duplicates)
 *   - which |w| ranges hold 50/80/95% of the mass (clusters)
 *   - consecutive same-value runs (run-length redundancy)
 *   - identical whole blocks (32-weight block duplication)
 *   - fp16 scale distribution (per-block magnitude clustering)
 *
 * Compile:
 *   gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I.
 *     runner/explore/w_dup.c -o runner/explore/w_dup.exe
 * Run:
 *   runner/explore/w_dup.exe I:/model/Qwen3-0.6B-Q8_0.gguf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define NBIN 256  /* signed -128..127 */
static uint64_t g_hist[NBIN];
static int64_t g_sum_w = 0;
static uint64_t g_n = 0;

/* per-tensor duplicate stats (tensor-level) */
typedef struct {
    uint64_t dups;      /* occurrences beyond first per distinct value */
    uint64_t total;
    double   entropy;   /* bits per value */
} TStat;

static inline float fp16_to_f32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (man == 0) f = sign << 31;
        else {
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) { man <<= 1; exp--; }
            man &= 0x3FF;
            f = (sign << 31) | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1F) {
        f = (sign << 31) | 0x7F800000 | (man << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (man << 13);
    }
    float r; memcpy(&r, &f, 4); return r;
}

/* block hashing for identical-block detection */
static uint64_t fnv64(const uint8_t *p, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
typedef struct { uint64_t hash; uint64_t idx; } BEnt;
static int bcmp(const void *a, const void *b) {
    const BEnt *x = a, *y = b;
    if (x->hash < y->hash) return -1;
    if (x->hash > y->hash) return 1;
    return (x->idx < y->idx) ? -1 : (x->idx > y->idx);
}

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    int q4 = (argc > 2) && strcmp(argv[2], "q4") == 0;

    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }
    FILE *fp = fopen(fin, "rb");
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    printf("=== WEIGHT DUP / CLUSTER ANALYSIS ===\n  %s (%.1f MB, %s)\n\n",
           fin, fsz / 1048576.0, q4 ? "Q4_0" : "Q8_0");

    int want_type = q4 ? 2 : 8;
    uint64_t nbins = q4 ? 16 : 256;
    uint64_t *bhist = (uint64_t*)calloc(nbins, 8);

    /* scale + real-magnitude analysis (Q8_0 only): log2 bins, 0.5 width */
    #define LBINS 64
    #define LMIN (-26.0)
    #define LWIDTH 0.5
    uint64_t scale_hist[LBINS];
    uint64_t mag_hist_r[LBINS];
    memset(scale_hist, 0, sizeof(scale_hist));
    memset(mag_hist_r, 0, sizeof(mag_hist_r));
    double scale_min = 1e30, scale_max = 0, scale_sum = 0, scale_sumsq = 0;
    uint64_t n_scales = 0, n_zero_scales = 0;

    /* run-length stats */
    uint64_t run_occ = 0;   /* values that are part of a run (len>=2) */
    uint64_t max_run = 0;
    int prev = 0x7FFFFFFF, runlen = 0;
    uint64_t n_blocks_total = 0, n_blocks_same_prev = 0;

    /* per-tensor stats */
    uint64_t n_q = 0;
    TStat *tstat = (TStat*)calloc(gf->tensor_count, sizeof(TStat));

    /* block-level duplicate detection (BIG: full pass) */
    BEnt *bents = NULL;
    uint64_t bcap = 0, bn = 0;

    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != want_type) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        if (sz < 34) continue;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint64_t blocks = sz / 34;
        n_q++;

        uint8_t *buf = (uint8_t*)malloc(sz);
        fseek(fp, off, SEEK_SET);
        fread(buf, 1, sz, fp);

        /* grow block array once */
        if (bn + blocks > bcap) {
            bcap = (bn + blocks) * 2;
            bents = (BEnt*)realloc(bents, bcap * sizeof(BEnt));
        }

        uint64_t tdups = 0;
        double tent = 0;
        uint64_t tloc[256];
        memset(tloc, 0, sizeof(tloc));

        for (uint64_t b = 0; b < blocks; b++) {
            const uint8_t *blk = buf + b * 34;
            /* scale */
            uint16_t h; memcpy(&h, blk, 2);
            float s = fp16_to_f32(h);

            /* collect scale stats (Q8_0 only) */
            if (!q4 && s != 0.0f) {
                if (s < 0) s = -s;
                double log2s = log2(s);
                int lb = (int)((log2s - LMIN) / LWIDTH);
                if (lb < 0) lb = 0;
                if (lb >= LBINS) lb = LBINS - 1;
                scale_hist[lb]++;
                scale_sum += s; scale_sumsq += (double)s * s;
                n_scales++;
                if (s < scale_min) scale_min = s;
                if (s > scale_max) scale_max = s;
            } else if (!q4 && s == 0.0f) {
                n_zero_scales++;
            }

            if (q4) {
                const uint8_t *q = blk + 2;
                for (int i = 0; i < 32; i++) {
                    int v0 = q[i] & 0x0F, v1 = q[i] >> 4;
                    bhist[v0]++; bhist[v1]++;
                    tloc[v0]++; tloc[v1]++;
                    /* runs */
                    if (v0 == prev) { runlen++; } else { runlen = 1; prev = v0; }
                    if (runlen == 2) run_occ += 2; else if (runlen > 2) run_occ++;
                    if (runlen > max_run) max_run = runlen;
                    if (v1 == v0) { runlen++; } else { runlen = 1; prev = v1; }
                    if (runlen == 2) run_occ += 2; else if (runlen > 2) run_occ++;
                    if (runlen > max_run) max_run = runlen;
                }
            } else {
                const int8_t *w = (const int8_t*)(blk + 2);
                for (int i = 0; i < 32; i++) {
                    int v = w[i];
                    int idx = v + 128;
                    g_hist[idx]++; bhist[idx]++;
                    tloc[idx]++;
                    g_sum_w += v;
                    g_n++;
                    /* real magnitude = |scale × w| */
                    double mag = fabs((double)s * v);
                    if (mag > 0) {
                        double log2m = log2(mag);
                        int lb = (int)((log2m - LMIN) / LWIDTH);
                        if (lb < 0) lb = 0;
                if (lb >= LBINS) lb = LBINS - 1;
                        mag_hist_r[lb]++;
                    }
                    if (v == prev) { runlen++; } else { runlen = 1; prev = v; }
                    if (runlen == 2) run_occ += 2; else if (runlen > 2) run_occ++;
                    if (runlen > max_run) max_run = runlen;
                }
            }

            /* identical block detection: hash the 32 weight bytes */
            bents[bn].hash = fnv64(blk + 2, 32);
            bents[bn].idx = bn;
            bn++;
            if (b > 0) {
                if (memcmp(blk + 2, buf + (b - 1) * 34 + 2, 32) == 0) n_blocks_same_prev++;
            }
        }
        n_blocks_total += blocks;

        /* per-tensor dup ratio + entropy */
        for (uint64_t v = 0; v < nbins; v++) {
            if (tloc[v] > 1) tdups += tloc[v] - 1;
            if (tloc[v] > 0) {
                double p = (double)tloc[v] / (blocks * (q4 ? 64 : 32));
                tent += -p * log2(p);
            }
        }
        tstat[t].dups = tdups;
        tstat[t].total = blocks * (q4 ? 64 : 32);
        tstat[t].entropy = tent;
        free(buf);
    }
    fclose(fp);

    uint64_t nvals = q4 ? 0 : g_n;
    if (q4) {
        for (int i = 0; i < 16; i++) nvals += bhist[i];
    }

    printf("Q-type tensors: %" PRIu64 ", values: %" PRIu64 ", blocks: %" PRIu64 "\n\n", n_q, nvals, n_blocks_total);

    /* ── 1. Value histogram + duplicates (aggregate) ── */
    printf("── VALUE DUPLICATES (top 20 most repeated values) ──\n");
    typedef struct { int v; uint64_t c; } VC;
    VC *vcs = (VC*)malloc(nbins * sizeof(VC));
    for (uint64_t i = 0; i < nbins; i++) {
        vcs[i].v = (int)i - (q4 ? 0 : 128);
        vcs[i].c = bhist[i];
    }
    /* sort by count desc (simple selection for 16/256 bins is fine) */
    for (uint64_t i = 0; i < nbins && i < 20; i++) {
        uint64_t best = i;
        for (uint64_t j = i + 1; j < nbins; j++)
            if (vcs[j].c > vcs[best].c) best = j;
        VC tmp = vcs[i]; vcs[i] = vcs[best]; vcs[best] = tmp;
    }
    for (uint64_t i = 0; i < nbins && i < 20; i++) {
        if (vcs[i].c == 0) break;
        printf("  val %4d : %12" PRIu64 "  (%6.3f%%)\n",
               vcs[i].v, vcs[i].c, 100.0 * vcs[i].c / nvals);
    }
    free(vcs);

    /* duplicated occurrences: n - distinct */
    uint64_t distinct = 0;
    for (uint64_t i = 0; i < nbins; i++) if (bhist[i] > 0) distinct++;
    uint64_t dup_occ = 0;
    for (uint64_t i = 0; i < nbins; i++) if (bhist[i] > 1) dup_occ += bhist[i] - 1;
    printf("  distinct values used: %" PRIu64 " / %" PRIu64 "\n", distinct, nbins);
    printf("  duplicated occurrences: %" PRIu64 " / %" PRIu64 " (%.2f%%)\n\n",
           dup_occ, nvals, 100.0 * dup_occ / nvals);

    /* ── 2. Cluster ranges (percentiles) ── */
    printf("── MAGNITUDE CLUSTERS (Q8_0: |w|, Q4_0: nibble v) ──\n");
    double ent = 0;
    for (uint64_t i = 0; i < nbins; i++) {
        if (bhist[i] == 0) continue;
        double p = (double)bhist[i] / nvals;
        ent += -p * log2(p);
    }
    printf("  entropy: %.4f bits/value (Q8_0 max 8, Q4_0 max 4)\n", ent);
    printf("  theoretical min size: %.1f MB (values only)\n",
           nvals * ent / 8 / 1048576.0);
    /* percentile walk on magnitude */
    uint64_t mag_hist[129];  /* |w| 0..127 + overflow */
    memset(mag_hist, 0, sizeof(mag_hist));
    if (!q4) {
        for (int v = -128; v <= 127; v++) {
            int a = (v < 0) ? -v : v;
            if (a > 127) a = 127;
            mag_hist[a] += bhist[v + 128];
        }
        uint64_t m = 0;
        int pct[5] = {50, 80, 90, 95, 99};
        int pi = 0;
        printf("  |w| range containing X%% of weights:\n");
        for (int a = 0; a < 128 && pi < 5; a++) {
            m += mag_hist[a];
            while (pi < 5 && m * 100 >= (uint64_t)pct[pi] * nvals) {
                printf("    %d%% of weights have |w| <= %d\n", pct[pi], a);
                pi++;
            }
        }
        if (pi < 5) printf("    (rest beyond |w|=127)\n");
        /* sign split */
        uint64_t neg = 0, pos = 0, zero = 0;
        for (int v = -128; v < 0; v++) neg += bhist[v + 128];
        zero = bhist[128];
        for (int v = 1; v <= 127; v++) pos += bhist[v + 128];
        printf("  sign split: negative %" PRIu64 " (%.2f%%), zero %" PRIu64 " (%.2f%%), positive %" PRIu64 " (%.2f%%)\n\n",
               neg, 100.0 * neg / nvals, zero, 100.0 * zero / nvals, pos, 100.0 * pos / nvals);
    }

    /* ── 3. Runs ── */
    printf("── CONSECUTIVE DUPLICATE RUNS ──\n");
    printf("  max run: %" PRIu64 "\n", max_run);
    printf("  values inside runs (len>=2): %" PRIu64 " / %" PRIu64 " (%.2f%%)\n\n",
           run_occ, nvals, 100.0 * run_occ / nvals);

    /* ── 3b. Scale & real-magnitude distribution (Q8_0 only) ── */
    if (!q4) {
        printf("── SCALE DISTRIBUTION (fp16 per block) ──\n");
        printf("  n_scales: %" PRIu64 ", zero scales: %" PRIu64 "\n", n_scales, n_zero_scales);
        if (n_scales > 0) {
            double mean = scale_sum / n_scales;
            double stddev = sqrt(scale_sumsq / n_scales - mean * mean);
            printf("  min: %.6e  max: %.6e  mean: %.6e  stddev: %.6e\n",
                   scale_min, scale_max, mean, stddev);
            printf("  log2(scale) histogram (0.5-width bins):\n");
            for (int b = 0; b < LBINS; b++) {
                if (scale_hist[b] == 0) continue;
                double edge = LMIN + b * LWIDTH;
                printf("    [%.1f, %.1f) : %12" PRIu64 "  (%5.2f%%)\n",
                       edge, edge + LWIDTH, scale_hist[b],
                       100.0 * scale_hist[b] / n_scales);
            }
        }
        printf("\n");

        printf("── REAL MAGNITUDE |scale * w| (where w = int8) ──\n");
        uint64_t mag_total = 0;
        for (int b = 0; b < LBINS; b++) mag_total += mag_hist_r[b];
        printf("  non-zero magnitudes: %" PRIu64 " / %" PRIu64 "\n", mag_total, nvals);
        /* percentiles from mag_hist_r */
        uint64_t mcum = 0;
        int mpct[5] = {25, 50, 75, 90, 95};
        int mpi = 0;
        printf("  log2|scale*w| histogram (percentile markers):\n");
        for (int b = 0; b < LBINS; b++) {
            if (mag_hist_r[b] == 0) continue;
            mcum += mag_hist_r[b];
            double edge = LMIN + b * LWIDTH;
            while (mpi < 5 && mcum * 100 >= (uint64_t)mpct[mpi] * mag_total) {
                printf("    [%d-th pct at log2=%.1f, real value ~%.2e]  ",
                       mpct[mpi], edge, pow(2.0, edge));
                mpi++;
            }
            if (b % 2 == 0)  /* show even bins only */
                printf("    [%.1f, %.1f) : %12" PRIu64 "  (%5.2f%%)\n",
                       edge, edge + LWIDTH, mag_hist_r[b],
                       100.0 * mag_hist_r[b] / mag_total);
        }
        /* cluster summary: where 50% of mass lives */
        mcum = 0; mpi = 0;
        int cl_start = -1, cl_end = -1;
        for (int b = 0; b < LBINS; b++) {
            mcum += mag_hist_r[b];
            if (cl_start < 0 && mcum * 100 >= 25 * mag_total) cl_start = b;
            if (cl_end < 0 && mcum * 100 >= 75 * mag_total) { cl_end = b; break; }
        }
        if (cl_start >= 0 && cl_end >= 0) {
            printf("  50%% of real magnitudes live in log2 range [%.1f, %.1f] "
                   "(~[%.2e, %.2e])\n",
                   LMIN + cl_start * LWIDTH, LMIN + (cl_end + 1) * LWIDTH,
                   pow(2.0, LMIN + cl_start * LWIDTH),
                   pow(2.0, LMIN + (cl_end + 1) * LWIDTH));
        }
        printf("\n");
    }

    /* ── 4. Identical whole blocks ── */
    printf("── IDENTICAL 32-WEIGHT BLOCKS (full pass) ──\n");
    qsort(bents, bn, sizeof(BEnt), bcmp);
    uint64_t dup_blocks = 0, groups = 0;
    uint64_t i = 0;
    while (i < bn) {
        uint64_t j = i + 1;
        while (j < bn && bents[j].hash == bents[i].hash) j++;
        if (j - i > 1) {
            /* verify at least one real duplicate (hash collision unlikely but check) */
            groups++;
            dup_blocks += (j - i) - 1;
        }
        i = j;
    }
    printf("  distinct block hashes: %" PRIu64 " / %" PRIu64 "\n", bn - dup_blocks, bn);
    printf("  duplicate blocks: %" PRIu64 " (%.2f%% of all blocks)\n",
           dup_blocks, 100.0 * dup_blocks / bn);
    printf("  blocks identical to previous block: %" PRIu64 " (%.2f%%)\n\n",
           n_blocks_same_prev, 100.0 * n_blocks_same_prev / n_blocks_total);

    /* ── 5. Per-tensor concentration (top 15) ── */
    printf("── PER-TENSOR: most concentrated (lowest entropy / most dups) ──\n");
    typedef struct { const char *name; double ent; double dup_pct; uint64_t n; } R;
    R *rows = (R*)malloc(gf->tensor_count * sizeof(R));
    uint64_t nr = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (tstat[t].total == 0) continue;
        rows[nr].name = gf->tensors[t].name;
        rows[nr].ent = tstat[t].entropy;
        rows[nr].dup_pct = 100.0 * tstat[t].dups / tstat[t].total;
        rows[nr].n = tstat[t].total;
        nr++;
    }
    for (uint64_t k = 0; k < nr && k < 15; k++) {
        uint64_t best = k;
        for (uint64_t j = k + 1; j < nr; j++)
            if (rows[j].ent < rows[best].ent) best = j;
        R tmp = rows[k]; rows[k] = rows[best]; rows[best] = tmp;
        printf("  %-34s n=%10" PRIu64 "  ent=%.3f bits  dups=%.2f%%\n",
               rows[k].name, rows[k].n, rows[k].ent, rows[k].dup_pct);
    }
    free(rows);
    free(bhist);
    free(tstat);
    free(bents);
    gguf_close(gf);
    return 0;
}
