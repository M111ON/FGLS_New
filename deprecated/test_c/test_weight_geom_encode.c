/*
 * test_weight_geom_encode.c — Weight tensor → hex_tile encode → lossless verify
 *
 * Pipeline:
 *   .qdat (Q8_0 raw) → split to 7B tiles → hex_tile_encode → store index
 *   → decode per tile → compare original → lossless ✓
 *
 * Compile:
 *   gcc -O2 -I. -D__USE_MINGW_ANSI_STDIO -o test_weight_geom tests/test_weight_geom_encode.c
 *   ./test_weight_geom ../build/qwen25_tensors_raw/blk.0.attn_q.weight.qdat
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "hex_tile.h"

#define TILE_SZ      7
#define INDEX_CAP    200000

typedef struct {
    uint32_t  tile_idx;
    uint32_t  encoded_offset;
    uint8_t   encoded_size;   /* 2 (FLAT) or 9 (non-FLAT) */
    uint8_t   type;           /* HENC_FLAT / TRIPLET / GRADIENT / EDGE */
    uint32_t  orig_offset;    /* byte offset in original data */
} TileIndex;

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while (0)

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.qdat>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *orig = (uint8_t *)malloc(fsize);
    if (!orig) { fprintf(stderr, "malloc(%ld) failed\n", fsize); return 1; }
    fread(orig, 1, fsize, f);
    fclose(f);

    printf("=== Weight Geometry Encode ===\n");
    printf("  File:   %s\n", path);
    printf("  Size:   %ld bytes (%.1f KB)\n", fsize, fsize / 1024.0);

    long n_values  = fsize;  /* bytes = values (Q8_0 raw) */
    int  n_tiles   = (n_values + TILE_SZ - 1) / TILE_SZ;

    /* ── Encode ── */
    double t0 = now_ms();

    uint8_t *enc_buf = (uint8_t *)malloc((size_t)n_tiles * 12);  /* max 12B/tile */
    TileIndex *index = (TileIndex *)malloc((size_t)n_tiles * sizeof(TileIndex));
    uint32_t enc_ofs = 0;

    int n_flat = 0, n_triplet = 0, n_grad = 0, n_edge = 0;

    for (int ti = 0; ti < n_tiles; ti++) {
        HexTile tile;
        long start = (long)ti * TILE_SZ;
        int remain = (int)(n_values - start);
        int copy_n = remain < TILE_SZ ? remain : TILE_SZ;

        memset(tile.c, 0, HEX_CELLS);
        memcpy(tile.c, orig + start, copy_n);

        uint8_t tmp[16];
        int enc_n = hex_tile_encode(&tile, tmp);

        memcpy(enc_buf + enc_ofs, tmp, enc_n);

        index[ti].tile_idx       = ti;
        index[ti].encoded_offset = enc_ofs;
        index[ti].encoded_size   = (uint8_t)enc_n;
        index[ti].type           = tmp[0];
        index[ti].orig_offset    = (uint32_t)start;

        if (tmp[0] == HENC_FLAT)         n_flat++;
        else if (tmp[0] == HENC_TRIPLET_FLAT) n_triplet++;
        else if (tmp[0] == HENC_GRADIENT)     n_grad++;
        else if (tmp[0] == HENC_EDGE)         n_edge++;

        enc_ofs += enc_n;
    }

    double t_enc = now_ms() - t0;

    printf("\n  Tiles:   %d\n", n_tiles);
    printf("  FLAT:    %d (%d%%)\n", n_flat, n_flat * 100 / n_tiles);
    printf("  TRIPLET: %d (%d%%)\n", n_triplet, n_triplet * 100 / n_tiles);
    printf("  GRADIENT:%d (%d%%)\n", n_grad, n_grad * 100 / n_tiles);
    printf("  EDGE:    %d (%d%%)\n", n_edge, n_edge * 100 / n_tiles);

    long total_enc = enc_ofs;
    printf("\n  Original: %ld bytes (%.1f KB)\n", fsize, fsize / 1024.0);
    printf("  Encoded:  %ld bytes (%.1f KB)\n", total_enc, total_enc / 1024.0);
    printf("  Ratio:    %.3f (enc/orig)\n", (double)total_enc / fsize);
    printf("  Index:    %ld bytes (%.1f KB)\n",
           (long)n_tiles * sizeof(TileIndex),
           (double)(n_tiles * (long)sizeof(TileIndex)) / 1024.0);
    printf("  Encode:   %.2f ms (%.1f µs/tile)\n", t_enc, t_enc * 1000.0 / n_tiles);

    /* ── Decode & verify ── */
    double t0_d = now_ms();

    uint8_t *dec = (uint8_t *)malloc(fsize);
    if (!dec) { fprintf(stderr, "malloc dec failed\n"); return 1; }
    memset(dec, 0, fsize);

    int verify_errs = 0;
    for (int ti = 0; ti < n_tiles; ti++) {
        HexTile out;
        memset(&out, 0xFF, sizeof(out));

        uint32_t ofs  = index[ti].encoded_offset;
        uint8_t  sz   = index[ti].encoded_size;
        int dn = hex_tile_decode(enc_buf + ofs, sz, &out);

        long start = (long)ti * TILE_SZ;
        int remain = (int)(n_values - start);
        int copy_n = remain < TILE_SZ ? remain : TILE_SZ;

        memcpy(dec + start, out.c, copy_n);

        /* verify this tile */
        if (memcmp(orig + start, dec + start, copy_n) != 0) {
            if (verify_errs < 5)
                printf("  TILE %d: VERIFY FAIL at byte %ld\n", ti, start);
            verify_errs++;
        }
    }

    double t_dec = now_ms() - t0_d;

    printf("\n  Decode:  %.2f ms (%.1f µs/tile)\n", t_dec, t_dec * 1000.0 / n_tiles);
    printf("  Verify:  %s (%d tiles OK, %d FAIL)\n",
           verify_errs == 0 ? "ALL PASS" : "SOME FAIL",
           n_tiles - verify_errs, verify_errs);

    /* full integrity */
    int full_match = memcmp(orig, dec, fsize) == 0;
    CHECK(full_match, "Full tensor lossless roundtrip");

    /* ── Random access benchmark ── */
    printf("\n  ── Random access ──\n");
    int n_rand = n_tiles < 1000 ? n_tiles : 1000;
    int *order = (int *)malloc(n_rand * sizeof(int));
    for (int i = 0; i < n_rand; i++) order[i] = rand() % n_tiles;

    double t0_r = now_ms();
    for (int i = 0; i < n_rand; i++) {
        int ti = order[i];
        HexTile out;
        memset(&out, 0xFF, sizeof(out));
        hex_tile_decode(enc_buf + index[ti].encoded_offset,
                        index[ti].encoded_size, &out);
    }
    double t_rand = now_ms() - t0_r;
    printf("  %d tiles in %.2f ms (%.1f µs/tile)\n",
           n_rand, t_rand, t_rand * 1000.0 / n_rand);

    free(order);
    free(orig);
    free(enc_buf);
    free(index);
    free(dec);

    printf("\n=== Results: %d pass, %d fail ===\n", _pass, _fail);
    return _fail;
}
