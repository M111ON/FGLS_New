/*
 * test_h64_diamond.c — REAL pipeline with Diamond routing
 *
 * File → scanner → fold_block_init → diamond_gate → diamond_route
 *      → geo_route_init → H64Encoder → hamburger
 *
 * ตอนนี้ chunk ถูก "หั่น" ที่ diamond gate จริงๆ
 * ก่อนเข้า H64 ทุก chunk ต้องผ่าน:
 *   1. fold_block_init  → DiamondBlock (pair invariant check ready)
 *   2. diamond_gate     → 0=drop, 1=pass, 2=pass+drift
 *   3. diamond_route    → MAIN(1) or TEMPORAL(2) lane
 *   4. geo_route_init   → TorusNode → path_id (0..2 cycling per lane)
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define HB_HAS_ZSTD 1

#include "pogls_fold.h"
#include "geo_diamond_field.h"
#include "geo_route.h"
#include "pogls_scanner.h"
#include "pogls_hilbert64_encoder.h"
#include "pogls_to_geopixel.h"
#include "hamburger_classify.h"
#include "hamburger_encode.h"

typedef struct {
    H64Encoder enc;
    uint32_t   path_main;   /* path counter for MAIN lane  0..2 */
    uint32_t   path_temp;   /* path counter for TEMP lane  0..2 */
    uint32_t   drift_acc;   /* diamond drift accumulator        */
    uint64_t   baseline;    /* fold_fibo_intersect of first block */
    int        baseline_set;
    const uint8_t *buf_base; /* pointer to file buffer           */

    /* stats */
    uint64_t   enc_total, chunk_total;
    uint32_t   tc[4];
    uint32_t   win, lose, n_packets, n_skip;
    uint32_t   n_drop, n_drift, n_main, n_temp;
} PipeState;

/* ── flush one H64 packet → hamburger ──────────────────────────── */
static void flush_packet(PipeState *st) {
    h64_finalize(&st->enc);
    H64GeopixelBridge bridge;
    uint32_t n_tiles = h64_to_geopixel(&st->enc, &bridge);
    st->n_packets++;

    for (uint32_t t = 0; t < n_tiles; t++) {
        H64TileIn *tile = &bridge.tiles[t];

        if (tile->flags & HB_TILE_FLAG_SKIP) {
            st->enc_total  += 4u;
            st->chunk_total+= 64u;
            st->win++; st->n_skip++;
            continue;
        }

        uint32_t seed32 = (uint32_t)(tile->seed & 0xFFFFFFFFu);

        /* ── Step 1: classify from RAW pixels (signal, not residual) ── */
        int Y[64], Cg[64], Co[64];
        for (int p = 0; p < 64; p++) {
            Y[p]  = (int)tile->pixels[p];
            Cg[p] = 0;
            Co[p] = 0;
        }
        uint8_t ttype = hb_classify_tile(Y, Cg, Co, 0, 0, 8, 8, 8);

        /* ── Step 2: predict + build ycgco residuals (ttype already known) ── */
        uint8_t ycgco[64*6];
        memset(ycgco, 0, sizeof(ycgco));
        for (uint32_t p = 0; p < 64u; p++) {
            uint8_t pred  = hb_predict(seed32, p);
            int16_t resid = (int16_t)((int)tile->pixels[p] - (int)pred);
            memcpy(ycgco + p*6u, &resid, 2);
        }
        if (ttype < 4) st->tc[ttype]++;

        uint8_t enc_buf[256];
        uint32_t enc_sz = hb_codec_apply(
            GPX5_CODEC_SEED, ttype,
            ycgco, (uint32_t)sizeof(ycgco),
            enc_buf, (uint32_t)sizeof(enc_buf), seed32);

        if (enc_sz == 0u || enc_sz > 64u) {
            uint32_t z = hb_zstd19_apply(tile->pixels, 64u,
                                          enc_buf, sizeof(enc_buf));
            enc_sz = (z > 0u && z < 64u) ? z : 64u;
        }
        st->enc_total  += enc_sz;
        st->chunk_total+= 64u;
        if (enc_sz < 64u) st->win++; else st->lose++;
    }
    h64_encoder_init(&st->enc);
    st->path_main = st->path_temp = 0;
}

/* ── scanner callback — diamond gate + route ─────────────────────── */
static void on_chunk(const ScanEntry *e, void *user) {
    PipeState *st = (PipeState *)user;

    /* 1. fold_block_init — chunk becomes DiamondBlock
     * XOR-fold chunk 64B → 8B seed → mix into core.raw
     * ทำให้ diamond_route เห็น content จริงไม่ใช่แค่ metadata  */
    const uint8_t *chunk = st->buf_base + e->offset;
    uint64_t chunk_fold = e->seed;  /* scanner คำนวณ XOR-fold ไว้แล้ว */

    DiamondBlock db = fold_block_init(
        e->coord.face,
        0u,
        e->chunk_idx,
        (uint8_t)(e->coord.z & 0x0Fu),
        0u);

    /* bind chunk content: XOR fold into core, rebuild invert */
    db.core.raw ^= chunk_fold;
    db.invert    = ~db.core.raw;   /* maintain pair invariant */
    fold_build_quad_mirror(&db);   /* rebuild mirror from new core */
    (void)chunk;

    /* set baseline from first block */
    if (!st->baseline_set) {
        st->baseline     = fold_fibo_intersect(&db);
        st->baseline_set = 1;
    }

    /* 2. diamond_gate — drop bad blocks */
    int gate = diamond_gate(&db, st->baseline, &st->drift_acc);
    if (gate == 0) { st->n_drop++; return; }
    if (gate == 2)   st->n_drift++;

    /* 3. diamond_route — assign lane */
    int lane = diamond_route(&db);  /* 1=MAIN, 2=TEMP */

    /* 4. path_id assignment per lane (0→1→2 cycling) */
    uint8_t path_id;
    if (lane == 1) {
        path_id = (uint8_t)(st->path_main % 3u);
        st->path_main++;
        st->n_main++;
    } else {
        path_id = (uint8_t)(st->path_temp % 3u);
        st->path_temp++;
        st->n_temp++;
    }

    /* 5. feed into H64 with real face from coord */
    ScanEntry local = *e;
    h64_feed(&st->enc, &local, path_id);

    /* flush every 36 feeds */
    uint32_t total_fed = st->path_main + st->path_temp;
    if (total_fed >= 36u)
        flush_packet(st);
}

int main(void) {
    FILE *f = fopen("/mnt/user-data/uploads/high_detail.bmp", "rb");
    if (!f) { fprintf(stderr, "cannot open\n"); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(buf); return 1; }
    fclose(f);

    printf("file    : %ld B  chunks: %ld\n\n", fsz, fsz/64);

    PipeState st;
    memset(&st, 0, sizeof(st));
    h64_encoder_init(&st.enc);

    st.buf_base = buf;
    scan_buf(buf, (size_t)fsz, on_chunk, &st, NULL);
    if (st.path_main + st.path_temp > 0) flush_packet(&st);
    free(buf);

    uint32_t n_enc = st.win + st.lose;
    printf("=== DIAMOND PIPELINE RESULTS ===\n");
    printf("drop(gate=0) : %u  drift : %u\n", st.n_drop, st.n_drift);
    printf("MAIN lane    : %u  TEMP  : %u\n", st.n_main, st.n_temp);
    printf("packets      : %u\n", st.n_packets);
    printf("tiles        : %u  skip(inv)=%u\n", n_enc, st.n_skip);
    printf("FLAT : %5u (%.1f%%)\n", st.tc[0], n_enc?st.tc[0]*100.0/n_enc:0);
    printf("GRAD : %5u (%.1f%%)\n", st.tc[1], n_enc?st.tc[1]*100.0/n_enc:0);
    printf("EDGE : %5u (%.1f%%)\n", st.tc[2], n_enc?st.tc[2]*100.0/n_enc:0);
    printf("NOISE: %5u (%.1f%%)\n", st.tc[3], n_enc?st.tc[3]*100.0/n_enc:0);
    printf("win  : %u (%.1f%%)\n",  st.win, n_enc?st.win*100.0/n_enc:0);
    printf("\nchunk_in : %llu B\n", (unsigned long long)st.chunk_total);
    printf("enc_out  : %llu B\n",  (unsigned long long)st.enc_total);
    printf("ratio    : %.3fx\n",   (double)st.chunk_total / st.enc_total);
    return 0;
}
