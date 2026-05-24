/*
 * bench_v3_integrated.c — Diamond Field v3: Shell + Tring + Rotation
 *                         + Classifier v2 (score-based) + Sparse encoding
 *
 * Integrates Diamond Shell rotation scan into v3 encode/decode:
 *   encode: rotate64(6) → pick best → classify(score) → store (sparse|full)
 *   decode: read from Tring → sparse_decode if needed → inverse_rotate
 *   delete: shell_clr() O(1)
 *   gc:     dfield_gc() sweep
 *   reshape: dfield_reshape() remap between levels
 *
 * Build:
 *   gcc -O2 -I. -I..\Diamond_shell_encoder -I..\Diamond_decode_hamburger `
 *       -I..\..\..\..\core\pogls_engine\twin_core `
 *       -o bench_v3_integrated.exe bench_v3_integrated.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "geo_diamond_field.h"
#include "diamond_shell_codec.h"   /* _shell_rotate64 + _shell_inverse_rotate64 */

/* ── Rotation metadata table (parallel to tring) ────────────────── */
typedef struct {
    uint8_t *rot;        /* rot[tick] for each tick */
    uint32_t capacity;
} RotTable;

static inline int rot_init(RotTable *rt, uint32_t cap) {
    rt->rot = (uint8_t *)calloc(cap, 1);
    if (!rt->rot) return -1;
    rt->capacity = cap;
    return 0;
}
static inline void rot_free(RotTable *rt) { free(rt->rot); rt->rot = NULL; rt->capacity = 0; }
static inline void rot_set(RotTable *rt, uint32_t tick, uint8_t r) {
    if (tick < rt->capacity) rt->rot[tick] = r;
}
static inline uint8_t rot_get(const RotTable *rt, uint32_t tick) {
    return (tick < rt->capacity) ? rt->rot[tick] : 0;
}

/* ── Enhanced dfield_encode with rotation + RotTable + sparse ─────── */
static inline uint32_t dfield_encode_rotated(DiamondField *df,
                                              RotTable      *rt,
                                              const uint8_t  chunk[64],
                                              uint8_t       *out_level)
{
    /* 1. rotation scan — pick best orientation */
    uint8_t rotbuf[64], best_buf[64];
    uint64_t best_isect = 0;
    uint8_t  best_rot   = 0;
    int      best_pc    = -1;

    for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
        _shell_rotate64(rotbuf, chunk, rot);
        DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, 0);
        if (!fold_xor_audit(&db)) {
            db.invert = ~db.core.raw;
            fold_build_quad_mirror(&db);
        }
        uint64_t isect = fold_fibo_intersect(&db);
        int pc = __builtin_popcountll(isect);
        if (pc > best_pc) { best_pc = pc; best_isect = isect; best_rot = rot; memcpy(best_buf, rotbuf, 64); }
    }

    /* 2. classify level via score-based classifier (uniq + variance on best_buf) */
    uint8_t n = shell_classify_level(best_buf);
    if (out_level) *out_level = n;

    /* 3-4. map + probe */
    uint16_t local_idx;
    uint32_t gidx;
    while (n <= SHELL_MAX_LEVEL) {
        uint16_t cap = df->shell[n].slot_count;
        uint16_t probe = chunk_to_slot_idx(best_buf, n);
        int found = 0;
        for (uint16_t i = 0; i < cap; i++) {
            uint32_t g_probe = slot_global(n, probe);
            if (!shell_get(&df->shell[n], probe) && sidx_get(&df->sidx, g_probe) == SLOT_NULL) {
                local_idx = probe; gidx = g_probe; found = 1; break;
            }
            probe = (uint16_t)((probe + 1u) % cap);
        }
        if (found) { if (out_level) *out_level = n; break; }
        n++;
    }
    if (n > SHELL_MAX_LEVEL) return SLOT_NULL;

    /* 5. push to tring (sparse if n <= SPARSE_MAX_LEVEL) */
    uint32_t tick;
    if (n <= SPARSE_MAX_LEVEL) {
        uint8_t sbuf[66];
        uint32_t ssz = sparse_encode(sbuf, best_buf);
        if (ssz > 0 && ssz < 64) {
            tick = tring_push(&df->tring, sbuf, ssz);
        } else {
            tick = tring_push(&df->tring, best_buf, 64);
        }
    } else {
        tick = tring_push(&df->tring, best_buf, 64);
    }
    if (tick == UINT32_MAX) return SLOT_NULL;
    rot_set(rt, tick, best_rot);

    /* 6. mark shell + index */
    shell_set(&df->shell[n], local_idx);
    sidx_set(&df->sidx, gidx, tick);

    return gidx;
}

/* ── Enhanced dfield_decode with inverse rotation + sparse support ── */
static inline int dfield_decode_rotated(const DiamondField *df,
                                         const RotTable    *rt,
                                         uint32_t           gidx,
                                         uint8_t            out[64])
{
    uint8_t  n   = global_level(gidx);
    uint16_t idx = global_local(gidx);
    if (n > SHELL_MAX_LEVEL) return -1;
    if (!shell_get(&df->shell[n], idx)) return -1;
    uint32_t tick = sidx_get(&df->sidx, gidx);
    if (tick == SLOT_NULL) return -1;

    /* read from tring — may be sparse */
    uint32_t sz;
    const uint8_t *data = tring_read(&df->tring, tick, &sz);
    if (!data) return -1;

    uint8_t tmp[64];
    if (sz < 64) sparse_decode(tmp, data, sz);
    else         memcpy(tmp, data, 64);

    _shell_inverse_rotate64(out, tmp, rot_get(rt, tick));
    return 0;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  BENCHMARKS                                                       */
/* ══════════════════════════════════════════════════════════════════ */

static int fails = 0;
#define CHECK(cond, msg) do { if(!(cond)) { printf("[FAIL] %s\n", msg); fails++; } else { printf("[PASS] %s\n", msg); } } while(0)

static uint8_t *load_file(const char *path, uint32_t *len_out) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f); *len_out = (uint32_t)sz; return buf;
}

/* ── Test F1: real file roundtrip ────────────────────────────────── */
static void test_real_file_roundtrip(void) {
    struct { const char *label, *path; } files[] = {
        {"C source",  "..\\test_diamond_flow_grouping.c"},
        {"C header",  "..\\fibo_layer_header.h"},
        {"Markdown pipeline","..\\Pogls_full pipeline.md"},
        {"Compressed zip","..\\fgls_handoff_fibolayer.zip"},
        {"Fold header","..\\core\\twin_core\\pogls_fold.h"},
        {NULL, NULL}
    };

    for (int fi = 0; files[fi].label; fi++) {
        uint32_t len;
        uint8_t *buf = load_file(files[fi].path, &len);
        if (!buf) { printf("[SKIP] %s (cannot open)\n", files[fi].label); continue; }

        uint64_t N = len / 64;
        if (N == 0) { free(buf); continue; }

        DiamondField df;
        RotTable rt;
        dfield_init(&df, (uint32_t)(N * 2));
        rot_init(&rt, (uint32_t)(N * 2));

        uint32_t *gidxs = malloc(N * sizeof(uint32_t));
        uint8_t  *levels = malloc(N);
        uint8_t  *rots   = malloc(N);
        int ok = 1;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        for (uint64_t i = 0; i < N; i++) {
            gidxs[i] = dfield_encode_rotated(&df, &rt, buf + i * 64, &levels[i]);
            if (gidxs[i] == SLOT_NULL) { ok = 0; break; }
            uint32_t tick = sidx_get(&df.sidx, gidxs[i]);
            rots[i] = (tick != SLOT_NULL) ? rot_get(&rt, tick) : 0;
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double enc_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6;

        /* decode all and verify */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int all_match = 1;
        for (uint64_t i = 0; i < N && ok; i++) {
            uint8_t dec[64];
            if (dfield_decode_rotated(&df, &rt, gidxs[i], dec) != 0 ||
                memcmp(dec, buf + i * 64, 64) != 0) {
                printf("  MISMATCH at chunk %llu (gidx=%u level=%u rot=%u)\n",
                       (unsigned long long)i, gidxs[i], levels[i], rots[i]);
                all_match = 0; break;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dec_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6;

        /* level distribution */
        uint64_t lvl_cnt[9] = {0};
        for (uint64_t i = 0; i < N && ok; i++)
            if (levels[i] <= 8) lvl_cnt[levels[i]]++;

        /* rot distribution */
        uint64_t rot_cnt[6] = {0};
        for (uint64_t i = 0; i < N && ok; i++)
            if (rots[i] < 6) rot_cnt[rots[i]]++;

        printf("%-24s N=%5llu  enc=%5.0fMB/s  dec=%5.0fMB/s  %s\n",
               files[fi].label, (unsigned long long)N,
               (double)len/1e6/(enc_ms/1e3),
               (double)len/1e6/(dec_ms/1e3),
               all_match ? "ALL MATCH" : "MISMATCH!");

        if (ok) {
            printf("  levels: ");
            for (int l = 0; l <= 8; l++)
                if (lvl_cnt[l]) printf("n%d=%llu ", l, (unsigned long long)lvl_cnt[l]);
            printf("\n  rots  : ");
            for (int r = 0; r < 6; r++)
                if (rot_cnt[r]) printf("r%d=%llu ", r, (unsigned long long)rot_cnt[r]);
            printf("\n");
        }

        if (all_match && ok) CHECK(1, files[fi].label);
        else fails++;

        dfield_free(&df); rot_free(&rt);
        free(gidxs); free(levels); free(rots); free(buf);
    }
}

/* ── Test F2: delete + GC ────────────────────────────────────────── */
static void test_delete_gc(void) {
    DiamondField df;
    RotTable rt;
    dfield_init(&df, 1024);
    rot_init(&rt, 1024);

    uint8_t chunk[64];
    uint32_t gidxs[50];
    for (int i = 0; i < 50; i++) {
        memset(chunk, (uint8_t)(i * 17), 64);
        gidxs[i] = dfield_encode_rotated(&df, &rt, chunk, NULL);
    }
    CHECK(df.tring.live_count == 50, "50 chunks encoded");

    /* delete half */
    for (int i = 0; i < 25; i++) dfield_delete(&df, gidxs[i]);
    CHECK(df.tring.live_count == 50, "data still in tring after delete (flag cleared)");

    /* GC */
    uint32_t freed = dfield_gc(&df);
    CHECK(freed == 25, "GC freed 25 unreferenced ticks");
    CHECK(df.tring.live_count == 25, "25 live after GC");

    /* verify remaining chunks are still correct */
    int all_ok = 1;
    for (int i = 25; i < 50; i++) {
        uint8_t dec[64];
        memset(chunk, (uint8_t)(i * 17), 64);
        if (dfield_decode_rotated(&df, &rt, gidxs[i], dec) != 0 ||
            memcmp(dec, chunk, 64) != 0) { all_ok = 0; break; }
    }
    CHECK(all_ok, "remaining chunks correct after GC");

    dfield_free(&df); rot_free(&rt);
}

/* ── Test F3: reshape ────────────────────────────────────────────── */
static void test_reshape(void) {
    DiamondField df;
    RotTable rt;
    dfield_init(&df, 4096);
    rot_init(&rt, 4096);

    uint8_t chunk[64];
    memset(chunk, 0, 64);  /* all-zero → fits n=0 */
    uint8_t level;
    uint32_t gidx = dfield_encode_rotated(&df, &rt, chunk, &level);
    CHECK(gidx != SLOT_NULL, "encode for reshape");
    CHECK(level == 0, "zero chunk at level 0");

    /* reshape from n=0 to n=1 */
    uint32_t moved = dfield_reshape(&df, 0, 1);
    CHECK(moved == 1, "reshape moved 1 slot");
    CHECK(!shell_get(&df.shell[0], 0), "old slot (n=0) cleared");
    CHECK(shell_any(&df.shell[1]), "new shell (n=1) occupied");

    uint8_t dec[64];
    int r = dfield_decode_rotated(&df, &rt, gidx, dec);
    CHECK(r == -1, "old gidx returns -1 after reshape");

    /* find new gidx via sidx scan */
    uint32_t new_gidx = SLOT_NULL;
    for (uint32_t g = 0; g < INDEX_SIZE; g++) {
        if (sidx_get(&df.sidx, g) != SLOT_NULL) { new_gidx = g; break; }
    }
    CHECK(new_gidx != SLOT_NULL, "new gidx found after reshape");

    dfield_decode_rotated(&df, &rt, new_gidx, dec);
    CHECK(memcmp(dec, chunk, 64) == 0, "data survives reshape");

    dfield_free(&df); rot_free(&rt);
}

/* ── Test F5a: windowed batch (fingerprint grouping) ────────────── */
static void test_windowed_batch(void) {
    /* A. synthetic repetitive data: proves batch works when data is similar */
    DiamondField df_syn;
    dfield_init(&df_syn, 65536);

    uint8_t syn[64][64]; /* 64 chunks, each with same base but 3 varying bytes */
    uint8_t base_syn[64]; memset(base_syn, 0xAB, 64);
    for (int i = 0; i < 64; i++) {
        memcpy(syn[i], base_syn, 64);
        syn[i][0] = (uint8_t)i;
        syn[i][1] = (uint8_t)(i * 3);
        syn[i][2] = (uint8_t)(i * 7);
    }

    uint32_t syn_gidxs[64];
    uint32_t syn_e = dfield_encode_windowed(&df_syn, (const uint8_t *)syn, 64, 8, syn_gidxs);
    CHECK(syn_e == 64, "synthetic windowed: all 64 encoded");

    uint32_t syn_batch = 0, syn_indiv = 0;
    for (uint32_t i = 0; i < syn_e; i++) {
        uint32_t sv = sidx_get(&df_syn.sidx, syn_gidxs[i]);
        if (sv == SLOT_NULL) continue;
        if (sidx_is_batch(sv)) syn_batch++; else syn_indiv++;
    }
    printf("  Synthetic (3/64 bytes vary): batched=%u individual=%u (%.0f%% hit)\n",
           syn_batch, syn_indiv,
           syn_e > 0 ? 100.0 * syn_batch / syn_e : 0);
    CHECK(syn_batch > 0, "synthetic repetitive data: batch hits > 0");

    /* decode synthetic roundtrip */
    int syn_ok = 1;
    for (uint32_t i = 0; i < syn_e; i++) {
        uint8_t dec[64];
        if (dfield_decode(&df_syn, syn_gidxs[i], dec) != 0 ||
            memcmp(dec, syn[i], 64) != 0) { syn_ok = 0; break; }
    }
    CHECK(syn_ok, "synthetic windowed batch decode lossless");
    dfield_free(&df_syn);

    /* B. real C source file: tests with actual diverse data */
    uint32_t len;
    uint8_t *buf = load_file("..\\test_diamond_flow_grouping.c", &len);
    if (!buf) { printf("[SKIP] real file windowed batch\n"); return; }

    DiamondField df;
    dfield_init(&df, 65536);
    uint64_t N = len / 64;
    uint32_t real_batch = 0, real_indiv = 0;

    for (uint64_t off = 0; off + 64 <= N; off += 64) {
        uint32_t gidxs[64];
        uint32_t e = dfield_encode_windowed(&df, buf + off * 64, 64, 8, gidxs);
        for (uint32_t i = 0; i < e && i < 64; i++) {
            uint32_t sv = sidx_get(&df.sidx, gidxs[i]);
            if (sv == SLOT_NULL) continue;
            if (sidx_is_batch(sv)) real_batch++;
            else real_indiv++;
        }
    }
    for (uint64_t i = N - (N % 64); i < N; i++)
        dfield_encode(&df, buf + i * 64, NULL), real_indiv++;

    uint64_t real_total = real_batch + real_indiv;
    printf("  Real file: batched=%u individual=%u (%.0f%% hit)\n",
           real_batch, real_indiv,
           real_total > 0 ? 100.0 * real_batch / real_total : 0);

    /* real file roundtrip — spot check first window */
    int real_ok = 1;
    uint32_t gb[64];
    DiamondField df2;
    dfield_init(&df2, 1024);
    uint32_t e2 = dfield_encode_windowed(&df2, buf, 64, 8, gb);
    for (uint32_t i = 0; i < e2 && i < 64; i++) {
        uint8_t dec[64];
        if (dfield_decode(&df2, gb[i], dec) != 0 ||
            memcmp(dec, buf + i * 64, 64) != 0) { real_ok = 0; break; }
    }
    CHECK(real_ok, "real file windowed batch decode lossless");
    dfield_free(&df2);

    dfield_free(&df);
    free(buf);
}

/* ── Test F5: batch roundtrip + compression ratio ────────────────── */
static void test_batch_mode(void) {
    /* F5a: synthetic batch roundtrip */
    DiamondField df;
    dfield_init(&df, 65536);

    /* create 8 similar chunks (L1 batch): most bytes identical, few vary */
    uint8_t base_8[64]; memset(base_8, 0x42, 64);
    uint8_t chunks[8][64];
    for (int i = 0; i < 8; i++) {
        memcpy(chunks[i], base_8, 64);
        chunks[i][0] = (uint8_t)(0x10 + i);  /* 1st byte varies */
        chunks[i][1] = (uint8_t)(0x20 + i);  /* 2nd byte varies */
        /* bytes 2-63 all 0x42 = identical across all chunks */
    }

    uint32_t gidxs[8];
    uint32_t encoded = dfield_encode_batch(&df, (const uint8_t *)chunks, 8, 0, gidxs);
    CHECK(encoded == 8, "batch L1 encoded 8 chunks");

    /* decode each and verify */
    int all_ok = 1;
    for (uint32_t i = 0; i < encoded; i++) {
        uint8_t dec[64];
        if (dfield_decode(&df, gidxs[i], dec) != 0 ||
            memcmp(dec, chunks[i], 64) != 0) { all_ok = 0; break; }
    }
    CHECK(all_ok, "batch decode all 8 chunks lossless");

    /* F5b: compression ratio measurement */
    /* create 64 similar chunks — 1-2 varying bytes (realistic) */
    uint8_t base_64[64]; memset(base_64, 0xBB, 64);
    uint8_t chunks64[64][64];
    for (int i = 0; i < 64; i++) {
        memcpy(chunks64[i], base_64, 64);
        chunks64[i][0] = (uint8_t)(0x30 + i);   /* 1 varying byte */
        chunks64[i][1] = (uint8_t)(i);           /* 2nd varying byte */
    }
    uint32_t gidxs64[64];
    DiamondField df2;
    dfield_init(&df2, 65536);
    uint32_t enc64 = dfield_encode_batch(&df2, (const uint8_t *)chunks64, 64, 1, gidxs64);
    CHECK(enc64 == 64, "batch L2 encoded 64 chunks");

    /* measure batch tring node size */
    uint32_t batch_tick = sidx_is_batch(sidx_get(&df2.sidx, gidxs64[0]))
        ? sidx_batch_tick(sidx_get(&df2.sidx, gidxs64[0])) : 0;
    uint32_t bsz;
    tring_read(&df2.tring, batch_tick, &bsz);
    printf("  L2 batch: %u bytes for 64 chunks = %.2f B/chunk\n", bsz, bsz / 64.0);
    CHECK(bsz < 64 * 64, "batch smaller than individual full storage");

    /* adaptive: each diff = 8B mask + popcnt(mask)B. With 2 varying bytes:
       diff = 8 + 2 = 10 bytes. Total = 4 + 64 + 64*10 = 708. */
    CHECK(bsz >= 4 + 64 + 64 * 9, "batch min size (1 diff per chunk)");
    CHECK(bsz <= 4 + 64 + 64 * (8 + 12), "batch max size (12 diffs per chunk)");

    /* F5c: real file batch efficiency */
    uint32_t len;
    uint8_t *buf = load_file("..\\test_diamond_flow_grouping.c", &len);
    if (buf) {
        uint64_t N = len / 64;
        /* group first N%8 chunks into L1 batches */
        DiamondField df3;
        dfield_init(&df3, 65536);
        uint32_t batch_count = 0;
        uint32_t total_batch_size = 0;
        for (uint64_t i = 0; i + 8 <= N; i += 8) {
            uint32_t gb[8];
            uint32_t e = dfield_encode_batch(&df3, buf + i * 64, 8, 0, gb);
            if (e == 8) {
                batch_count++;
                uint32_t bt = sidx_batch_tick(sidx_get(&df3.sidx, gb[0]));
                uint32_t bz; tring_read(&df3.tring, bt, &bz);
                total_batch_size += bz;
            }
        }
        printf("  Real file: %u batches (L1), avg %.2f B/chunk (vs 64)\n",
               batch_count,
               batch_count > 0 ? (double)total_batch_size / (batch_count * 8) : 0);
        dfield_free(&df3);
        free(buf);
    }

    /* decode roundtrip check on df2 (L2 batch) */
    int all64 = 1;
    for (uint32_t i = 0; i < 64; i++) {
        uint8_t dec[64];
        if (dfield_decode(&df2, gidxs64[i], dec) != 0 ||
            memcmp(dec, chunks64[i], 64) != 0) { all64 = 0; break; }
    }
    CHECK(all64, "L2 batch decode all 64 chunks lossless");

    /* delete + GC on batch */
    for (uint32_t i = 0; i < 32; i++) dfield_delete(&df2, gidxs64[i]);
    uint32_t freed = dfield_gc(&df2);
    CHECK(freed == 0, "batch GC: no chunks freed (all point to same tick, shell still has 32 refs)");

    for (uint32_t i = 32; i < 64; i++) dfield_delete(&df2, gidxs64[i]);
    freed = dfield_gc(&df2);
    CHECK(freed == 1, "batch GC: batch tick freed after all 64 deleted");

    dfield_free(&df2);
    dfield_free(&df);
}

/* ── Test F4: multi-chunk level distribution ─────────────────────── */
static void test_level_distribution(void) {
    DiamondField df;
    RotTable rt;
    dfield_init(&df, 65536);
    rot_init(&rt, 65536);

    uint32_t len;
    uint8_t *buf = load_file("..\\test_diamond_flow_grouping.c", &len);
    if (!buf) { printf("[SKIP] level distribution (no file)\n"); return; }

    uint64_t N = len / 64;
    uint8_t levels[200];
    uint64_t n_max = N < 200 ? N : 200;

    for (uint64_t i = 0; i < n_max; i++)
        dfield_encode_rotated(&df, &rt, buf + i * 64, &levels[i]);

    uint64_t cnt[9] = {0};
    for (uint64_t i = 0; i < n_max; i++)
        if (levels[i] <= 8) cnt[levels[i]]++;

    printf("Level distribution (C source, first %llu chunks):\n",
           (unsigned long long)n_max);
    for (int l = 0; l <= 8; l++)
        if (cnt[l]) printf("  n=%d: %llu (%.0f%%)\n", l,
               (unsigned long long)cnt[l], 100.0 * cnt[l] / n_max);

    dfield_free(&df); rot_free(&rt);
    free(buf);
}

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Diamond Field v3 — Classifier + Sparse + Batch + Rot  ║\n");
    printf("║  Shell + Tring + 6-DOF + Score-based + Sparse + Batch  ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_real_file_roundtrip();
    printf("\n");
    test_delete_gc();
    printf("\n");
    test_reshape();
    printf("\n");
    test_level_distribution();
    printf("\n");
    test_batch_mode();
    printf("\n");
    test_windowed_batch();
    printf("\n");

    printf("%s — %d failure(s)\n", fails == 0 ? "ALL PASS" : "FAILURES FOUND", fails);
    return fails ? 1 : 0;
}
