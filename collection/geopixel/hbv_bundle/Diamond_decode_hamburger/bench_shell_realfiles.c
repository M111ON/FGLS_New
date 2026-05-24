/*
 * bench_shell_realfiles.c — Diamond Shell on real files
 * Build: gcc -O2 -I. -I..\Diamond_shell_encoder -I..\..\..\..\core\pogls_engine\twin_core -o bench_shell_realfiles bench_shell_realfiles.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "diamond_shell_codec.h"
#include "diamond_shell_v2.h"

/* ── seed index for dedup (content-verified) ─────────────── */
#define SIDX_CAP  (1<<16)
#define SIDX_MASK (SIDX_CAP-1)
typedef struct { uint64_t seed; uint8_t chunk[64]; uint8_t used; } SeedSlot;
typedef struct { SeedSlot *slots; uint64_t ne, nc; } SeedIndex;

static SeedIndex* sx(void) {
    SeedIndex *x = calloc(1, sizeof(SeedIndex));
    x->slots = calloc(SIDX_CAP, sizeof(SeedSlot)); return x;
}
static void sf(SeedIndex *x) { free(x->slots); free(x); }
static void sp(SeedIndex *x, uint64_t seed, const uint8_t *ch) {
    uint32_t s0 = (uint32_t)(seed & SIDX_MASK);
    for (int i = 0; i < SIDX_CAP; i++) {
        uint32_t s = (s0 + i) & SIDX_MASK;
        if (!x->slots[s].used) {
            x->slots[s].used = 1; x->slots[s].seed = seed;
            memcpy(x->slots[s].chunk, ch, 64); x->ne++; return;
        }
        if (x->slots[s].seed == seed) return;
        x->nc++;
    }
}
/* content-verified lookup: returns chunk ONLY if seed + content match */
static const uint8_t* sg_verify(const SeedIndex *x, uint64_t seed, const uint8_t *chunk) {
    uint32_t s0 = (uint32_t)(seed & SIDX_MASK);
    for (int i = 0; i < SIDX_CAP; i++) {
        uint32_t s = (s0 + i) & SIDX_MASK;
        if (!x->slots[s].used) return NULL;
        if (x->slots[s].seed == seed && memcmp(x->slots[s].chunk, chunk, 64) == 0)
            return x->slots[s].chunk;
    }
    return NULL;
}
/* seed-only lookup for decode (content already verified at encode time) */
static const uint8_t* sg(const SeedIndex *x, uint64_t seed) {
    uint32_t s0 = (uint32_t)(seed & SIDX_MASK);
    for (int i = 0; i < SIDX_CAP; i++) {
        uint32_t s = (s0 + i) & SIDX_MASK;
        if (!x->slots[s].used) return NULL;
        if (x->slots[s].seed == seed) return x->slots[s].chunk;
    }
    return NULL;
}

/* ── Shell encode WITH dedup seed index ──────────────────── */
/* Returns bytes written. seed_idx populated during encode.   */
static uint64_t shell_encode_dedup(const uint8_t *data, uint64_t n_chunks,
                                    uint8_t *out, SeedIndex *idx)
{
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        const uint8_t *chunk = data + i * SHELL_CHUNK_SZ;
        uint8_t rotbuf[64], best_buf[64];
        uint64_t best_isect = 0;
        uint8_t  best_rot   = 0;
        int      best_pc    = -1;

        /* rotation scan — uses fibo_intersect for alignment only */
        for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
            _shell_rotate64(rotbuf, chunk, rot);
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, (uint32_t)i);
            if (!fold_xor_audit(&db)) {
                db.invert = ~db.core.raw;
                fold_build_quad_mirror(&db);
            }
            uint64_t isect = fold_fibo_intersect(&db);
            int pc = __builtin_popcountll(isect);
            if (pc > best_pc) { best_pc = pc; best_isect = isect; best_rot = rot; memcpy(best_buf, rotbuf, 64); }
        }

        /* classify based on ACTUAL data content, not fibo_intersect */
        uint32_t zero_cnt = 0;
        for (int j = 0; j < 64; j++) if (best_buf[j] == 0) zero_cnt++;
        int is_flat   = (zero_cnt == 64);
        int is_sparse = (zero_cnt >= 56);  /* at most 8 non-zero bytes */

        /* try dedup (content-verified) */
        uint64_t seed = _shell_fnv64(best_buf, 64);
        const uint8_t *found = (!is_flat) ? sg_verify(idx, seed, best_buf) : NULL;

        if (is_flat) {
            out[pos++] = SHELL_FLAG_FLAT;
            out[pos++] = best_rot;
            /* 2B total */
        } else if (found) {
            /* dedup hit: store seed only = 10B total (flag=SPARSE=1 means dedup) */
            out[pos++] = SHELL_FLAG_SPARSE;
            out[pos++] = best_rot;
            memcpy(out + pos, &seed, 8); pos += 8;
        } else {
            /* dedup miss: store full 64B rotated = 66B total (flag=DENSE=2 means full) */
            sp(idx, seed, best_buf);
            out[pos++] = SHELL_FLAG_DENSE;
            out[pos++] = best_rot;
            memcpy(out + pos, best_buf, 64); pos += 64;
        }
    }
    return pos;
}

/* ── Shell decode WITH dedup seed index ──────────────────── */
static uint64_t shell_decode_dedup(const uint8_t *in, uint64_t n_chunks,
                                    uint8_t *out, const SeedIndex *idx)
{
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        uint8_t flag = in[pos++];
        uint8_t rot  = in[pos++];
        uint8_t *chunk_out = out + i * SHELL_CHUNK_SZ;

        if (flag == SHELL_FLAG_FLAT) {
            memset(chunk_out, 0, SHELL_CHUNK_SZ);
        } else if (flag == SHELL_FLAG_SPARSE) {
            /* seed-dedup: 8B seed, look up in index */
            uint64_t seed;
            memcpy(&seed, in + pos, 8); pos += 8;
            const uint8_t *found = sg(idx, seed);
            if (found) {
                _shell_inverse_rotate64(chunk_out, found, rot);
            } else {
                memset(chunk_out, 0, SHELL_CHUNK_SZ);
            }
        } else {
            /* DENSE: full 64B rotated stored inline */
            uint8_t rotbuf[64];
            memcpy(rotbuf, in + pos, 64); pos += 64;
            _shell_inverse_rotate64(chunk_out, rotbuf, rot);
        }
    }
    return pos;
}

/* ── test configs ────────────────────────────────────────── */
typedef struct { const char *label; int pattern; } DataSpec;
static void gen_data(uint8_t *buf, size_t n, int pat) {
    for (size_t i = 0; i < n; i++) {
        switch (pat) {
            case 0: buf[i] = (uint8_t)(32 + (i % 90)); break;
            case 1: buf[i] = (uint8_t)((i % 256) ^ (i >> 8)); break;
            case 2: buf[i] = (uint8_t)(i * 2654435761ULL ^ (i >> 3)); break;
            case 3: buf[i] = (uint8_t)(i % 17); break;
            case 4: buf[i] = ((i % 64) < 4) ? (uint8_t)(i & 0xFF) : 0; break;
            case 5: buf[i] = ((i / 64) % 2 == 0) ? (uint8_t)(i & 0xFF) : (uint8_t)(i % 3); break;
        }
    }
}

static uint8_t *load_file(const char *path, uint32_t *len_out) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f); *len_out = (uint32_t)sz; return buf;
}

static void bench_one(const char *label, const uint8_t *data, uint64_t len) {
    uint64_t N = len / 64; if (N == 0) return;
    uint8_t *encbuf = malloc(N * 70);  /* worst case */
    uint8_t *decbuf = malloc(N * SHELL_CHUNK_SZ);

    SeedIndex *idx = sx();

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t enc_sz = shell_encode_dedup(data, N, encbuf, idx);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double enc_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t dec_sz = shell_decode_dedup(encbuf, N, decbuf, idx);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dec_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6;

    int exact = (memcmp(data, decbuf, N * SHELL_CHUNK_SZ) == 0);
    double ratio = (double)(N * SHELL_CHUNK_SZ) / (double)enc_sz;

    printf("%-24s N=%5llu  ratio=%6.3fx  dedup=%5llu/%llu  exact=%s  enc=%5.0fMB/s  dec=%5.0fMB/s\n",
           label, (unsigned long long)N, ratio,
           (unsigned long long)idx->ne, (unsigned long long)N,
           exact ? "YES" : "NO",
           (double)len / 1e6 / (enc_ms / 1e3),
           (double)len / 1e6 / (dec_ms / 1e3));

    /* shuffle test */
    uint8_t *shuf = malloc(N * SHELL_CHUNK_SZ);
    memcpy(shuf, data, N * SHELL_CHUNK_SZ);
    for (uint64_t i = 0; i < N; i++) {
        uint64_t j = i + (uint64_t)rand() % (N - i);
        uint8_t tmp[64]; memcpy(tmp, shuf + i*64, 64);
        memcpy(shuf + i*64, shuf + j*64, 64);
        memcpy(shuf + j*64, tmp, 64);
    }
    SeedIndex *idx2 = sx();
    uint64_t enc_sz2 = shell_encode_dedup(shuf, N, encbuf, idx2);
    double ratio2 = (double)(N * SHELL_CHUNK_SZ) / (double)enc_sz2;
    double drop = (ratio > 0) ? (1.0 - ratio2 / ratio) * 100.0 : 0;
    printf("  └─ shuffled            ratio=%6.3fx  drop=%+.1f%%\n", ratio2, drop);
    free(shuf); sf(idx2);

    sf(idx); free(encbuf); free(decbuf);
}

int main(void) {
    srand(42);

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Diamond Shell Codec — Real File Benchmark               ║\n");
    printf("║  Mode: FLAT(2B) / SEED-DEDUP(10B) / FULL-64B(66B)       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ── 1. Synthetic patterns ── */
    printf("── Synthetic patterns (4096 chunks = 256KB each) ──\n");
    {
        const uint64_t N = 4096;
        uint8_t *buf = malloc(N * SHELL_CHUNK_SZ);
        DataSpec specs[] = {
            {"Text-like", 0}, {"Structured", 1}, {"Pseudo-rnd", 2},
            {"Repetitive", 3}, {"Sparse", 4}, {"Mixed", 5}
        };
        for (int i = 0; i < 6; i++) {
            gen_data(buf, N * SHELL_CHUNK_SZ, specs[i].pattern);
            bench_one(specs[i].label, buf, N * SHELL_CHUNK_SZ);
        }
        free(buf);
    }

    /* ── 2. Real files ── */
    printf("\n── Real files ──\n");
    struct { const char *label, *path; } files[] = {
        {"C source",  "..\\test_diamond_flow_grouping.c"},
        {"C header",  "..\\fibo_layer_header.h"},
        {"Markdown (pipeline)","..\\Pogls_full pipeline.md"},
        {"Markdown (handoff)","..\\HANDOFF.md"},
        {"Markdown (hv5)","..\\HANDOFF_hbv_session5.md"},
        {"Binary EXE","..\\debug_check.exe"},
        {"Compressed zip","..\\fgls_handoff_fibolayer.zip"},
        {"Fold header","..\\core\\twin_core\\pogls_fold.h"},
        {"Diamond fd","..\\core\\twin_core\\geo_diamond_field.h"},
        {NULL, NULL}
    };
    for (int fi = 0; files[fi].label; fi++) {
        uint32_t len; uint8_t *buf = load_file(files[fi].path, &len);
        if (buf) { bench_one(files[fi].label, buf, len); free(buf); }
        else printf("%-12s  (cannot open: %s)\n", files[fi].label, files[fi].path);
    }

    printf("\nDone.\n");
    return 0;
}
