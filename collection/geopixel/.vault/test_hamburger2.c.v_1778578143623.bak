/*
 * test_hamburger.c — quick roundtrip test
 * gcc -O2 -o test_hamburger test_hamburger.c && ./test_hamburger
 *
 * No external deps. Tests:
 *   T1: pipe dispatch — 3 tiles go to 3 different carriers
 *   T2: 2-carrier band check — safe pair vs unsafe pair
 *   T3: codec roundtrip — SEED/DELTA/HILBERT encode→decode
 *   T4: invert recorder — zero tile = free, nonzero = active
 *   T5: full encode→file→decode roundtrip (4 dummy tiles)
 *   T6: LUT frozen — O(1) access after warm-up
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gpx5_container.h"
#include "hamburger_pipe.h"
#include "hamburger_encode.h"

/* ── test helpers ──────────────────────────────────── */
static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { printf("  PASS %s\n", label); g_pass++; } \
    else       { printf("  FAIL %s\n", label); g_fail++; } \
} while(0)

/* ══════════════════════════════════════════════════════
 * T1: pipe dispatch
 * hilbert_pos 0,1,2 → carrier 0,1,2 with default pipes
 * ══════════════════════════════════════════════════════ */
static void t1_dispatch(void) {
    printf("\nT1: pipe dispatch\n");
    Gpx5PipeEntry pipes[3];
    gpx5_default_pipes(pipes);

    HbDispatch d0 = hb_dispatch(0, pipes);
    HbDispatch d1 = hb_dispatch(1, pipes);
    HbDispatch d2 = hb_dispatch(2, pipes);
    HbDispatch d3 = hb_dispatch(3, pipes);  /* wraps: 3%3=0 → carrier 0 */

    CHECK("pos0 → carrier0", d0.carrier_id == 0);
    CHECK("pos1 → carrier1", d1.carrier_id == 1);
    CHECK("pos2 → carrier2", d2.carrier_id == 2);
    CHECK("pos3 → carrier0 (wrap)", d3.carrier_id == 0);
}

/* ══════════════════════════════════════════════════════
 * T2: 2-carrier band check
 * ══════════════════════════════════════════════════════ */
static void t2_band(void) {
    printf("\nT2: 2-carrier band check\n");
    /* safe: FLAT(low) + EDGE(high) */
    CHECK("FLAT+EDGE safe",  hb_2carrier_safe(GPX5_CTYPE_FLAT, GPX5_CTYPE_EDGE));
    /* unsafe: EDGE+NOISE both high */
    CHECK("EDGE+NOISE unsafe", !hb_2carrier_safe(GPX5_CTYPE_EDGE, GPX5_CTYPE_NOISE));
    /* safe: GRAD(low) + VECTOR(vec) */
    CHECK("GRAD+VECTOR safe", hb_2carrier_safe(GPX5_CTYPE_GRAD, GPX5_CTYPE_VECTOR));
    /* unsafe: FLAT+GRAD both low */
    CHECK("FLAT+GRAD unsafe", !hb_2carrier_safe(GPX5_CTYPE_FLAT, GPX5_CTYPE_GRAD));
}

/* ══════════════════════════════════════════════════════
 * T3: codec roundtrip
 * ══════════════════════════════════════════════════════ */
static void t3_codec(void) {
    printf("\nT3: codec roundtrip\n");
    uint32_t seed = 0xDEADBEEFu;

    /* test data: 18 bytes as repeated 6-byte exact flat sample */
    uint8_t orig[18] = {
        0x10,0x20,0x30,0x40,0x50,0x60,
        0x10,0x20,0x30,0x40,0x50,0x60,
        0x10,0x20,0x30,0x40,0x50,0x60
    };
    uint8_t enc[64]  = {0};
    uint8_t dec[64]  = {0};

    /* DELTA roundtrip */
    uint32_t enc_sz = hb_codec_apply(GPX5_CODEC_DELTA, GPX5_TTYPE_EDGE,
                                     orig, 18, enc, 64, seed);
    uint32_t dec_sz = hb_codec_invert(GPX5_CODEC_DELTA, GPX5_TTYPE_EDGE,
                                      enc, enc_sz, dec, 64, seed);
    CHECK("DELTA enc_sz>0",    enc_sz > 0);
    CHECK("DELTA dec_sz==18",  dec_sz == 18);
    CHECK("DELTA roundtrip",   memcmp(orig, dec, 18) == 0);

    /* SEED roundtrip for FLAT tile */
    memset(enc, 0, 64); memset(dec, 0, 64);
    enc_sz = hb_codec_apply(GPX5_CODEC_SEED, GPX5_TTYPE_FLAT,
                             orig, 18, enc, 64, seed);
    CHECK("SEED flat enc_sz==8", enc_sz == 8);  /* size + one exact signed sample */
    dec_sz = hb_codec_invert(GPX5_CODEC_SEED, GPX5_TTYPE_FLAT,
                             enc, enc_sz, dec, 18, seed);
    CHECK("SEED dec_sz==18", dec_sz == 18);
    CHECK("SEED roundtrip",   memcmp(orig, dec, 18) == 0);

    /* HILBERT lossless roundtrip */
    memset(enc, 0, 64); memset(dec, 0, 64);
    enc_sz = hb_codec_apply(GPX5_CODEC_HILBERT, GPX5_TTYPE_EDGE,
                             orig, 18, enc, 64, seed);
    /* enc_sz = 2(orig_sz) + 2(bitmask 16bits) + n_diff — must be > 4 */
    CHECK("HILBERT enc_sz>=4",  enc_sz >= 4);
    dec_sz = hb_codec_invert(GPX5_CODEC_HILBERT, GPX5_TTYPE_EDGE,
                              enc, enc_sz, dec, 64, seed);
    CHECK("HILBERT dec_sz==18", dec_sz == 18);
    CHECK("HILBERT roundtrip",  memcmp(orig, dec, 18) == 0);

    /* FREQ roundtrip */
    memset(enc, 0, 64); memset(dec, 0, 64);
    enc_sz = hb_codec_apply(GPX5_CODEC_FREQ, GPX5_TTYPE_GRADIENT,
                             orig, 18, enc, 64, seed);
    dec_sz = hb_codec_invert(GPX5_CODEC_FREQ, GPX5_TTYPE_GRADIENT,
                              enc, enc_sz, dec, 64, seed);
    CHECK("FREQ dec_sz==18",  dec_sz == 18);
    CHECK("FREQ roundtrip",   memcmp(orig, dec, 18) == 0);

    /* ZSTD roundtrip */
    memset(enc, 0, 64); memset(dec, 0, 64);
    enc_sz = hb_codec_apply(GPX5_CODEC_ZSTD19, GPX5_TTYPE_NOISE,
                             orig, 18, enc, 64, seed);
    dec_sz = hb_codec_invert(GPX5_CODEC_ZSTD19, GPX5_TTYPE_NOISE,
                              enc, enc_sz, dec, 64, seed);
    CHECK("ZSTD enc_sz>0",    enc_sz > 0);
    CHECK("ZSTD dec_sz==18",  dec_sz == 18);
    CHECK("ZSTD roundtrip",   memcmp(orig, dec, 18) == 0);

    /* RAW roundtrip */
    memset(enc, 0, 64); memset(dec, 0, 64);
    enc_sz = hb_codec_apply(GPX5_CODEC_RAW, GPX5_TTYPE_NOISE,
                             orig, 18, enc, 64, seed);
    dec_sz = hb_codec_invert(GPX5_CODEC_RAW, GPX5_TTYPE_NOISE,
                              enc, enc_sz, dec, 64, seed);
    CHECK("RAW roundtrip",      memcmp(orig, dec, 18) == 0);

    /* long-tile roundtrip: catches 1-byte length truncation bugs */
    uint8_t big[768];
    uint8_t big_enc[1024] = {0};
    uint8_t big_dec[1024] = {0};
    for (uint32_t i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i * 7u + 3u);
    enc_sz = hb_codec_apply(GPX5_CODEC_DELTA, GPX5_TTYPE_EDGE,
                             big, sizeof(big), big_enc, sizeof(big_enc), seed);
    dec_sz = hb_codec_invert(GPX5_CODEC_DELTA, GPX5_TTYPE_EDGE,
                              big_enc, enc_sz, big_dec, sizeof(big_dec), seed);
    CHECK("DELTA 768B enc_sz>0",   enc_sz > 0);
    CHECK("DELTA 768B dec_sz==768", dec_sz == sizeof(big));
    CHECK("DELTA 768B roundtrip",   memcmp(big, big_dec, sizeof(big)) == 0);
}

/* ══════════════════════════════════════════════════════
 * T4: invert recorder
 * ══════════════════════════════════════════════════════ */
static void t4_invert(void) {
    printf("\nT4: invert recorder (v6 analog negative model)\n");
    HbInvertRecorder rec;
    hb_invert_init(&rec);

    /* tile 0: original == encoded → all-zero residual → clear tile (free) */
    uint8_t orig0[8] = {0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA};
    uint8_t enc0 [8] = {0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA};
    hb_invert_record_enc(&rec, 0, orig0, enc0, 8u, 8u, 0u);

    /* tile 1: original != encoded → residual bytes stored (active) */
    uint8_t orig1[8] = {0x12,0x34,0x56,0x78,0x00,0x00,0x00,0x00};
    uint8_t enc1 [8] = {0x87,0x65,0x43,0x21,0x00,0x00,0x00,0x00};
    hb_invert_record_enc(&rec, 1, orig1, enc1, 8u, 8u, 0u);

    /* tile 2: all-nonzero difference → active */
    uint8_t orig2[4] = {0x00,0x00,0x00,0x00};
    uint8_t enc2 [4] = {0xFF,0xFF,0xFF,0xFF};
    hb_invert_record_enc(&rec, 2, orig2, enc2, 4u, 4u, 0u);

    CHECK("tile0 is clear (perfect prediction)", hb_invert_is_clear(&rec, 0));
    CHECK("tile1 is active",                    !hb_invert_is_clear(&rec, 1));
    CHECK("tile2 is active",                    !hb_invert_is_clear(&rec, 2));
    CHECK("tile1 res_sz==8",                     rec.res_sz[1] == 8u);
    CHECK("tile2 res_sz==4",                     rec.res_sz[2] == 4u);

    uint32_t active = hb_invert_freeze(&rec);
    CHECK("freeze: 2 active entries", active == 2u);

    /* stream: tile1 = HDR(5)+8 = 13B, tile2 = HDR(5)+4 = 9B → total 22B */
    uint8_t stream[64] = {0};
    uint32_t sz = hb_invert_write_stream(&rec, stream);
    CHECK("stream_sz == 22B (HDR+data per active tile)", sz == 22u);

    /* verify enc_buf bytes stored verbatim in stream (enc1 at offset 5) */
    CHECK("stream tile1 enc_buf[0]==0x87", stream[5] == 0x87u);

    hb_invert_free(&rec);
}

/* ══════════════════════════════════════════════════════
 * T5: full encode → file → decode roundtrip (4 tiles)
 * ══════════════════════════════════════════════════════ */
static void t5_roundtrip(void) {
    printf("\nT5: full encode→file→decode roundtrip\n");

    /* 4 tiles: FLAT, EDGE, GRAD, NOISE — 64 bytes each */
    const uint32_t TSIZ = 64u;
    const uint32_t NT   = 4u;

    uint8_t tile_data[4][64];
    for (uint32_t b = 0; b < TSIZ; b++) {
        uint32_t r = b % 6u;
        tile_data[0][b] = (r == 0u) ? 0x80u :
                          (r == 1u) ? 0x40u :
                          (r == 2u) ? 0x20u :
                          (r == 3u) ? 0x10u :
                          (r == 4u) ? 0x08u : 0x04u;
    }
    for (uint32_t b = 0; b < TSIZ; b++)
        tile_data[1][b] = (uint8_t)((64u + b) & 0xFFu);
    for (uint32_t b = 0; b < TSIZ; b++)
        tile_data[2][b] = (uint8_t)((b * 3u) & 0xFFu);
    for (uint32_t b = 0; b < TSIZ; b++)
        tile_data[3][b] = (uint8_t)(0xC0u ^ (uint8_t)b);

    HbTileIn tiles[4];
    uint8_t ttypes[4] = {GPX5_TTYPE_FLAT, GPX5_TTYPE_EDGE,
                         GPX5_TTYPE_GRADIENT, GPX5_TTYPE_NOISE};
    for (uint32_t t = 0; t < NT; t++) {
        tiles[t].data    = tile_data[t];
        tiles[t].sz      = TSIZ;
        tiles[t].tile_id = (uint16_t)t;
        tiles[t].ttype   = ttypes[t];
    }

    /* setup context */
    Gpx5PipeEntry pipes[3];
    gpx5_default_pipes(pipes);
    pipes[0].ctype = GPX5_CTYPE_FLAT;  pipes[0].codec = GPX5_CODEC_SEED;
    pipes[1].ctype = GPX5_CTYPE_EDGE;  pipes[1].codec = GPX5_CODEC_DELTA;
    pipes[2].ctype = GPX5_CTYPE_NOISE; pipes[2].codec = GPX5_CODEC_RAW;

    HbEncodeCtx ctx;
    hb_encode_init(&ctx, 0xCAFEBABEu, 2, 2, pipes, 0);

    const char *tmpfile = ".\\geopixel\\test_hamburger.gpx5";
    int r = hamburger_encode(tmpfile, &ctx, tiles, NT);
    CHECK("encode returns OK",     r == HB_OK);
    CHECK("ctx frozen after enc",  ctx.frozen == 1);
    CHECK("LUT allocated",         ctx.lut != NULL);
    CHECK("n_active > 0",          ctx.n_active > 0);

    hb_encode_free(&ctx);

    /* decode */
    uint8_t *out[4];
    for (int i = 0; i < 4; i++) out[i] = (uint8_t*)calloc(1, HB_TILE_SZ_MAX);
    uint32_t out_n = 0, out_sz = 0;
    r = hamburger_decode(tmpfile, out, &out_n, &out_sz);
    CHECK("decode returns OK",  r == HB_OK);
    CHECK("decode n_tiles==4",  out_n == NT);
    for (int i = 0; i < 4; i++) {
        CHECK("tile roundtrip bytes", memcmp(tile_data[i], out[i], TSIZ) == 0);
    }
    for (int i = 0; i < 4; i++) free(out[i]);
}

/* ══════════════════════════════════════════════════════
 * T6: LUT O(1) access
 * ══════════════════════════════════════════════════════ */
static void t6_lut(void) {
    printf("\nT6: LUT O(1) access\n");
    Gpx5LutEntry lut[4];
    HbInvertRecorder rec;
    hb_invert_init(&rec);

    /* active: different bytes */
    uint8_t a0[4]={0xAA,0xAA,0xAA,0xAA}, b0[4]={0xBB,0xBB,0xBB,0xBB};
    hb_invert_record_enc(&rec, 0, a0, b0, 4u, 4u, 0u);  /* active */
    /* free: identical original==encoded */
    uint8_t a1[4]={0x12,0x34,0x12,0x34}, b1[4]={0x12,0x34,0x12,0x34};
    hb_invert_record_enc(&rec, 1, a1, b1, 4u, 4u, 0u);  /* free   */
    uint8_t a2[4]={0x00,0x00,0x00,0x00}, b2[4]={0xFF,0xFF,0xFF,0xFF};
    hb_invert_record_enc(&rec, 2, a2, b2, 4u, 4u, 0u);  /* active */
    uint8_t a3[4]={0xDE,0xAD,0xDE,0xAD}, b3[4]={0xDE,0xAD,0xDE,0xAD};
    hb_invert_record_enc(&rec, 3, a3, b3, 4u, 4u, 0u);  /* free   */
    hb_invert_freeze(&rec);

    uint32_t active = hb_build_lut(&rec, lut, 0u, 1000u);
    CHECK("2 active LUT entries",       active == 2);
    CHECK("tile0 has offset",           lut[0].invert_offset != 0xFFFFFFFFu);
    CHECK("tile1 is free (0xFFFFFFFF)", lut[1].invert_offset == 0xFFFFFFFFu);
    CHECK("tile2 has offset > tile0",   lut[2].invert_offset > lut[0].invert_offset);
    CHECK("tile3 is free (0xFFFFFFFF)", lut[3].invert_offset == 0xFFFFFFFFu);
}

/* ══════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════ */
static void t7_classify(void) {
    printf("\nT7: classify + auto_pipes\n");
    /* 16x16 tiles - same size as v21 calibration */
    int Y_flat[256], Cg_flat[256], Co_flat[256];
    for(int i=0;i<256;i++){Y_flat[i]=128;Cg_flat[i]=0;Co_flat[i]=0;}
    uint8_t t = hb_classify_tile(Y_flat,Cg_flat,Co_flat, 0,0,16,16, 16);
    CHECK("flat tile->TTYPE_FLAT", t == GPX5_TTYPE_FLAT);

    int Y_grad[256], Cg_grad[256], Co_grad[256];
    for(int y=0;y<16;y++) for(int x=0;x<16;x++)
        {Y_grad[y*16+x]=x;Cg_grad[y*16+x]=0;Co_grad[y*16+x]=0;}
    t = hb_classify_tile(Y_grad,Cg_grad,Co_grad, 0,0,16,16, 16);
    CHECK("smooth grad->TTYPE_GRADIENT", t == GPX5_TTYPE_GRADIENT);

    int Y_edge[256], Cg_edge[256], Co_edge[256];
    for(int y=0;y<16;y++) for(int x=0;x<16;x++)
        {Y_edge[y*16+x]=x<8?0:255;Cg_edge[y*16+x]=0;Co_edge[y*16+x]=0;}
    t = hb_classify_tile(Y_edge,Cg_edge,Co_edge, 0,0,16,16, 16);
    CHECK("half-step edge->EDGE or NOISE", t==GPX5_TTYPE_EDGE||t==GPX5_TTYPE_NOISE);

    CHECK("FLAT->CTYPE_FLAT",  hb_ttype_to_ctype(GPX5_TTYPE_FLAT)==GPX5_CTYPE_FLAT);
    CHECK("GRAD->CTYPE_GRAD",  hb_ttype_to_ctype(GPX5_TTYPE_GRADIENT)==GPX5_CTYPE_GRAD);
    CHECK("EDGE->CTYPE_EDGE",  hb_ttype_to_ctype(GPX5_TTYPE_EDGE)==GPX5_CTYPE_EDGE);
    CHECK("NOISE->CTYPE_NOISE",hb_ttype_to_ctype(GPX5_TTYPE_NOISE)==GPX5_CTYPE_NOISE);

    CHECK("FLAT->CODEC_SEED",  hb_ttype_to_codec(GPX5_TTYPE_FLAT)==GPX5_CODEC_SEED);
    CHECK("GRAD->CODEC_FREQ",  hb_ttype_to_codec(GPX5_TTYPE_GRADIENT)==GPX5_CODEC_FREQ);
    CHECK("EDGE->CODEC_RICE3", hb_ttype_to_codec(GPX5_TTYPE_EDGE)==GPX5_CODEC_RICE3);
    CHECK("NOISE->CODEC_ZSTD19",hb_ttype_to_codec(GPX5_TTYPE_NOISE)==GPX5_CODEC_ZSTD19);

    uint8_t ttypes[10]={0,0,0,0,0,0,1,1,1,2};
    Gpx5PipeEntry pipes[3];
    hb_auto_pipes(ttypes,10,pipes);
    CHECK("auto_pipes[0] ctype=FLAT", pipes[0].ctype==GPX5_CTYPE_FLAT);
    CHECK("auto_pipes[1] ctype=GRAD", pipes[1].ctype==GPX5_CTYPE_GRAD);
    CHECK("auto_pipes[2] ctype=EDGE", pipes[2].ctype==GPX5_CTYPE_EDGE);
    CHECK("auto_pipes[0] codec=SEED", pipes[0].codec==GPX5_CODEC_SEED);
    CHECK("auto_pipes[1] codec=FREQ", pipes[1].codec==GPX5_CODEC_FREQ);
    CHECK("auto_pipes[2] codec=RICE3", pipes[2].codec==GPX5_CODEC_RICE3);
    CHECK("load_pct sum<=100",
        (int)pipes[0].load_pct+pipes[1].load_pct+pipes[2].load_pct<=100);
}

static void t8_encode_image(void) {
    printf("\nT8: hamburger_encode_image (auto classify+pipe)\n");

    /* 32x32 image, 16x16 tiles → 4 tiles */
    int Y[1024], Cg[1024], Co[1024];
    /* tile0: flat */
    for(int y=0;y<16;y++) for(int x=0;x<16;x++){int i=y*32+x;Y[i]=128;Cg[i]=0;Co[i]=0;}
    /* tile1: gradient */
    for(int y=0;y<16;y++) for(int x=16;x<32;x++){int i=y*32+x;Y[i]=x-16;Cg[i]=0;Co[i]=0;}
    /* tile2: edge */
    for(int y=16;y<32;y++) for(int x=0;x<16;x++){int i=y*32+x;Y[i]=x<8?0:255;Cg[i]=0;Co[i]=0;}
    /* tile3: flat */
    for(int y=16;y<32;y++) for(int x=16;x<32;x++){int i=y*32+x;Y[i]=200;Cg[i]=0;Co[i]=0;}

    HbImageIn img = {Y,Cg,Co, 32,32, 16,16};
    int r = hamburger_encode_image(".\\geopixel\\test_img.gpx5", &img, 0xDEADBEEFu, 0);
    CHECK("encode_image returns OK", r == HB_OK);

    /* verify file exists and has correct magic */
    FILE *f = fopen(".\\geopixel\\test_img.gpx5","rb");
    CHECK("output file created", f != NULL);
    if(f){
        uint8_t hbuf[4]; fread(hbuf,1,4,f); fclose(f);
        uint32_t magic = ((uint32_t)hbuf[0]<<24)|((uint32_t)hbuf[1]<<16)|
                         ((uint32_t)hbuf[2]<<8)|(uint32_t)hbuf[3];
        CHECK("file magic == GPX5", magic == GPX5_MAGIC);
    }

    /* decode and verify tile count */
    uint8_t *out[4]; uint8_t bufs[4][HB_TILE_SZ_MAX];
    for(int i=0;i<4;i++) out[i]=bufs[i];
    uint32_t nt=0, tsz=0;
    r = hamburger_decode(".\\geopixel\\test_img.gpx5",(uint8_t**)out,&nt,&tsz);
    CHECK("decode_image returns OK", r == HB_OK);
    CHECK("decoded n_tiles==4", nt == 4);
}

/* ══════════════════════════════════════════════════════
 * T9: hamburger_decode_image — pixel-level lossless roundtrip
 *   encode_image(32x32, 16x16 tiles) → decode_image → compare YCgCo
 *   All 1024 pixels must match exactly.
 * ══════════════════════════════════════════════════════ */
static void t9_decode_image(void) {
    printf("\nT9: decode_image pixel-level lossless roundtrip\n");

    const int W = 32, H = 32, TW = 16, TH = 16;
    const int NPX = W * H;
    int Y[1024], Cg[1024], Co[1024];

    /* tile0: flat */
    for(int y=0;y<16;y++) for(int x=0; x<16;x++){int i=y*W+x;   Y[i]=128;Cg[i]=0;Co[i]=0;}
    /* tile1: gradient */
    for(int y=0;y<16;y++) for(int x=16;x<32;x++){int i=y*W+x;   Y[i]=x-16;Cg[i]=4;Co[i]=-4;}
    /* tile2: edge */
    for(int y=16;y<32;y++) for(int x=0; x<16;x++){int i=y*W+x;  Y[i]=x<8?0:255;Cg[i]=-8;Co[i]=8;}
    /* tile3: varied gradient with non-zero chroma */
    for(int y=16;y<32;y++) for(int x=16;x<32;x++){int i=y*W+x;  Y[i]=(y-16)*8+(x-16);Cg[i]=y-16;Co[i]=x-16;}

    HbImageIn img = {Y, Cg, Co, W, H, TW, TH};
    int r = hamburger_encode_image(".\\geopixel\\t9_img.gpx5", &img, 0xCAFEBABEu, 0);
    CHECK("T9 encode_image OK", r == HB_OK);
    if (r != HB_OK) return;

    HbImageOut dec;
    memset(&dec, 0, sizeof(dec));
    r = hamburger_decode_image(".\\geopixel\\t9_img.gpx5", &dec, W, H, TW, TH);
    CHECK("T9 decode_image OK", r == HB_OK);
    if (r != HB_OK) return;

    CHECK("T9 decoded width",  dec.w  == W);
    CHECK("T9 decoded height", dec.h  == H);
    CHECK("T9 decoded tw",     dec.tw == TW);
    CHECK("T9 decoded th",     dec.th == TH);

    int mismatch = 0, max_delta = 0;
    for (int i = 0; i < NPX; i++) {
        int dY  = abs(dec.Y [i] - Y [i]);
        int dCg = abs(dec.Cg[i] - Cg[i]);
        int dCo = abs(dec.Co[i] - Co[i]);
        int d = dY > dCg ? dY : dCg;
        d = d > dCo ? d : dCo;
        if (d > 0) { mismatch++; if (d > max_delta) max_delta = d; }
    }
    printf("  pixel mismatch=%d  max_delta=%d\n", mismatch, max_delta);
    CHECK("T9 lossless: mismatch==0", mismatch == 0);
    CHECK("T9 lossless: max_delta==0", max_delta == 0);

    hb_image_out_free(&dec);
}

int main(void) {
    printf("=== hamburger test suite ===\n");
    t1_dispatch();
    t2_band();
    t3_codec();
    t4_invert();
    t5_roundtrip();
    t6_lut();
    t7_classify();
    t8_encode_image();
    t9_decode_image();
    printf("\n=== %d PASS / %d FAIL ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
