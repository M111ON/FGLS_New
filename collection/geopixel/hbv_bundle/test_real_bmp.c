/*
 * test_real_bmp.c — scan high_detail.bmp → fibo dispatch → hb encode
 * Shows real ttype mix from actual file data
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define HB_HAS_ZSTD 1
#include "fibo_hb_wire.h"   /* pulls fibo_tile_dispatch.h + hamburger */

/* ── Minimal standalone seed + ThetaCoord (inlined from scanner) ── */
static inline uint64_t chunk_seed(const uint8_t *c) {
    uint64_t w[8]; memcpy(w, c, 64);
    uint64_t s = w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]^w[7];
    s ^= s >> 33; s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33; s *= 0xc4ceb9fe1a85ec53ULL;
    s ^= s >> 33;
    return s;
}
typedef struct { uint8_t face, edge, z; } TC;
static inline TC theta(uint64_t h) {
    uint32_t hi = (uint32_t)(h >> 32), lo = (uint32_t)h;
    TC c;
    c.face = (uint8_t)(((uint64_t)hi * 12u) >> 32);
    c.edge = (uint8_t)(((uint64_t)lo *  5u) >> 32);
    c.z    = (uint8_t)((h >> 16) & 0xFF);
    return c;
}

int main(void) {
    /* load BMP raw bytes */
    FILE *f = fopen("/mnt/user-data/uploads/high_detail.bmp", "rb");
    if (!f) { fprintf(stderr, "cannot open BMP\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    fread(buf, 1, (size_t)fsz, f); fclose(f);

    printf("file: %ld B  chunks: %ld\n", fsz, fsz / 64);

    FiboDispatchCtx ctx;
    /* seed_origin = seed of first chunk */
    uint8_t  first[64]; memcpy(first, buf, 64);
    uint64_t seed0 = chunk_seed(first);
    fibo_dispatch_init(&ctx, seed0, 8);  /* (ctx, seed, tick) */

    /* stats */
    uint32_t tc[4]  = {0};   /* FLAT GRAD EDGE NOISE */
    uint32_t enc_total = 0, raw_total = 0;
    uint32_t n = 0, verified = 0;
    /* channel variance accumulators (sample first 100 tiles) */
    uint64_t var_R = 0, var_G = 0, var_B = 0;
    uint32_t sample_n = 0;

    GpxTileInput tile;
    FiboHbResult res;

    long n_chunks = fsz / 64;
    for (long ci = 0; ci < n_chunks; ci++) {
        const uint8_t *chunk = buf + ci * 64;
        uint64_t seed = chunk_seed(chunk);
        TC       coord = theta(seed);

        fibo_dispatch_entry(&ctx, chunk,
                            coord.face, coord.edge, coord.z,
                            (uint32_t)ci, 0, &tile);

        fibo_hb_encode_tile(&tile, &res);

        /* sample R/G/B variance for first 200 tiles */
        if (sample_n < 200) {
            uint32_t sumR=0, sumG=0, sumB=0;
            for (int p=0; p<64; p++) {
                sumR += tile.tile_rgb[p*3+0];
                sumG += tile.tile_rgb[p*3+1];
                sumB += tile.tile_rgb[p*3+2];
            }
            uint32_t mR=sumR/64, mG=sumG/64, mB=sumB/64;
            for (int p=0; p<64; p++) {
                int dr=tile.tile_rgb[p*3+0]-mR;
                int dg=tile.tile_rgb[p*3+1]-mG;
                int db=tile.tile_rgb[p*3+2]-mB;
                var_R += (uint64_t)(dr*dr);
                var_G += (uint64_t)(dg*dg);
                var_B += (uint64_t)(db*db);
            }
            sample_n++;
        }

        if (res.ttype < 4) tc[res.ttype]++;
        enc_total += res.enc_sz;
        raw_total += FTD_CHUNK_SZ;   /* compare vs 64B chunk input */
        if (res.verified) verified++;
        n++;
    }
    free(buf);

    printf("\n=== RESULTS (%u tiles) ===\n", n);
    printf("FLAT  : %5u  (%.1f%%)\n", tc[0], tc[0]*100.0/n);
    printf("GRAD  : %5u  (%.1f%%)\n", tc[1], tc[1]*100.0/n);
    printf("EDGE  : %5u  (%.1f%%)\n", tc[2], tc[2]*100.0/n);
    printf("NOISE : %5u  (%.1f%%)\n", tc[3], tc[3]*100.0/n);
    printf("verified: %u/%u\n", verified, n);
    printf("\nChannel variance (avg over %u tiles):\n", sample_n);
    printf("  R(content) : %llu\n", (unsigned long long)(var_R/(sample_n*64)));
    printf("  G(flow)    : %llu\n", (unsigned long long)(var_G/(sample_n*64)));
    printf("  B(binding) : %llu\n", (unsigned long long)(var_B/(sample_n*64)));
    printf("\nchunk_in : %u B (64B × %u tiles)\n", raw_total, n);
    printf("enc_out  : %u B\n", enc_total);
    printf("ratio    : %.2fx  (enc vs original chunk data)\n", (double)raw_total/enc_total);

    return 0;
}
/* already compiled — patch inline */
