/*
 * test_h64_real.c — REAL pipeline
 *   pogls_scanner (real) → H64Encoder → H64GeopixelBridge → hamburger
 *   Input: high_detail.bmp (raw bytes, not BMP decode — scan as binary)
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define HB_HAS_ZSTD 1

#include "pogls_scanner.h"          /* real scanner */
#include "pogls_hilbert64_encoder.h"
#include "pogls_to_geopixel.h"
#include "hamburger_classify.h"
#include "hamburger_encode.h"

/* ── Callback state ─────────────────────────────────────────────────── */
typedef struct {
    H64Encoder     enc;
    uint32_t       path_counter;   /* cycles 0→1→2→0 per face group    */
    /* stats */
    uint64_t       enc_total;
    uint64_t       chunk_total;
    uint32_t       tc[4];
    uint32_t       win, lose;
    uint32_t       n_packets;
    uint32_t       n_skip;
} PipeState;

static void flush_packet(PipeState *st) {
    h64_finalize(&st->enc);
    H64GeopixelBridge bridge;
    uint32_t n_tiles = h64_to_geopixel(&st->enc, &bridge);
    st->n_packets++;

    for (uint32_t t = 0; t < n_tiles; t++) {
        H64TileIn *tile = &bridge.tiles[t];

        if (tile->flags & HB_TILE_FLAG_SKIP) {
            /* invert tile — seed only (4B) */
            st->enc_total  += 4u;
            st->chunk_total+= 64u;
            st->win++;
            st->n_skip++;
            continue;
        }

        /* CODEC_SEED: residual = pixel ^ hb_predict(seed32, i) */
        uint32_t seed32 = (uint32_t)(tile->seed & 0xFFFFFFFFu);
        uint8_t ycgco[64*6];
        memset(ycgco, 0, sizeof(ycgco));
        for (uint32_t p = 0; p < 64u; p++) {
            uint8_t pred   = hb_predict(seed32, p);
            int16_t resid  = (int16_t)((int)tile->pixels[p] - (int)pred);
            memcpy(ycgco + p*6u, &resid, 2);
        }

        int Y[64], Cg[64], Co[64];
        for (int p = 0; p < 64; p++) {
            int16_t r; memcpy(&r, ycgco+p*6, 2);
            Y[p]=(int)r; Cg[p]=0; Co[p]=0;
        }
        uint8_t ttype = hb_classify_tile(Y, Cg, Co, 0, 0, 8, 8, 8);
        if (ttype < 4) st->tc[ttype]++;

        uint8_t enc_buf[256];
        uint32_t enc_sz = hb_codec_apply(
            GPX5_CODEC_SEED, ttype,
            ycgco, (uint32_t)sizeof(ycgco),
            enc_buf, (uint32_t)sizeof(enc_buf),
            seed32);

        if (enc_sz == 0u || enc_sz > 64u) {
            uint32_t z = hb_zstd19_apply(tile->pixels, 64u, enc_buf, sizeof(enc_buf));
            enc_sz = (z > 0u && z < 64u) ? z : 64u;
        }

        st->enc_total  += enc_sz;
        st->chunk_total+= 64u;
        if (enc_sz < 64u) st->win++; else st->lose++;
    }

    h64_encoder_init(&st->enc);
    st->path_counter = 0;
}

/* ── Scanner callback ────────────────────────────────────────────────── */
static void on_chunk(const ScanEntry *e, void *user) {
    PipeState *st = (PipeState *)user;

    /* assign path_id cycling 0→1→2 per face */
    uint8_t path_id = (uint8_t)(st->path_counter % 3u);

    /* create ScanEntry copy with face from theta coord */
    ScanEntry local = *e;
    /* use real coord.face from theta_map */
    uint8_t face = local.coord.face;

    h64_feed(&st->enc, &local, path_id);
    st->path_counter++;

    /* flush every 36 entries (3 paths × 12 faces = full packet) */
    if (st->path_counter >= 36u)
        flush_packet(st);

    (void)face;
}

int main(void) {
    FILE *f = fopen("/mnt/user-data/uploads/high_detail.bmp", "rb");
    if (!f) { fprintf(stderr, "cannot open BMP\n"); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(buf); return 1; }
    fclose(f);

    printf("file    : %ld B  chunks: %ld\n\n", fsz, fsz/64);

    PipeState st;
    memset(&st, 0, sizeof(st));
    h64_encoder_init(&st.enc);

    /* scan entire file — real scanner, real theta_map, real ScanEntry */
    scan_buf(buf, (size_t)fsz, on_chunk, &st, NULL);

    /* flush any remaining partial packet */
    if (st.path_counter > 0)
        flush_packet(&st);

    free(buf);

    uint32_t n_enc = st.win + st.lose;
    printf("=== REAL PIPELINE RESULTS ===\n");
    printf("packets  : %u\n", st.n_packets);
    printf("tiles    : %u  skip(inv)=%u\n", n_enc, st.n_skip);
    printf("FLAT  : %5u (%.1f%%)\n", st.tc[0], n_enc?st.tc[0]*100.0/n_enc:0);
    printf("GRAD  : %5u (%.1f%%)\n", st.tc[1], n_enc?st.tc[1]*100.0/n_enc:0);
    printf("EDGE  : %5u (%.1f%%)\n", st.tc[2], n_enc?st.tc[2]*100.0/n_enc:0);
    printf("NOISE : %5u (%.1f%%)\n", st.tc[3], n_enc?st.tc[3]*100.0/n_enc:0);
    printf("win   : %u (%.1f%%)\n",  st.win, n_enc?st.win*100.0/n_enc:0);
    printf("\nchunk_in : %llu B\n", (unsigned long long)st.chunk_total);
    printf("enc_out  : %llu B\n",  (unsigned long long)st.enc_total);
    printf("ratio    : %.3fx\n",   (double)st.chunk_total / st.enc_total);
    return 0;
}
