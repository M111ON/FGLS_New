/*
 * test_calibrate_bmp.c — calibrate codec on real BMP
 *
 * Flow: load BMP → RGB→YCgCo → encode → decode → PSNR + tile stats
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "gpx5_container.h"
#include "fibo_shell_walk.h"
#include "hamburger_classify.h"
#include "hamburger_pipe.h"
#include "hb_header_frame.h"
#include "gpx5_hbhf.h"
#include "hamburger_encode.h"

#define TILE_W  16
#define TILE_H  16
#define BMP_PATH "/mnt/user-data/uploads/high_detail.bmp"
#define OUT_PATH "/tmp/calib_real.gpx5"

/* ── BMP loader (24bpp, bottom-up) ── */
static uint8_t *bmp_load(const char *path, int *W, int *H) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return NULL; }
    *W = (int)(hdr[18]|(hdr[19]<<8)|(hdr[20]<<16)|(hdr[21]<<24));
    *H = (int)(hdr[22]|(hdr[23]<<8)|(hdr[24]<<16)|(hdr[25]<<24));
    uint32_t off = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|(hdr[13]<<24);
    int row_bytes = ((*W * 3) + 3) & ~3;
    uint8_t *rgb = (uint8_t *)malloc((size_t)(*W) * (size_t)(*H) * 3u);
    if (!rgb) { fclose(f); return NULL; }
    fseek(f, off, SEEK_SET);
    uint8_t *row = (uint8_t *)malloc((size_t)row_bytes);
    for (int y = *H - 1; y >= 0; y--) {
        fread(row, 1, (size_t)row_bytes, f);
        for (int x = 0; x < *W; x++) {
            rgb[(y * *W + x)*3+0] = row[x*3+2]; /* R */
            rgb[(y * *W + x)*3+1] = row[x*3+1]; /* G */
            rgb[(y * *W + x)*3+2] = row[x*3+0]; /* B */
        }
    }
    free(row); fclose(f);
    return rgb;
}

/* ── RGB → YCgCo (int) ── */
static void rgb_to_ycgco(const uint8_t *rgb, int *Y, int *Cg, int *Co, int n) {
    for (int i = 0; i < n; i++) {
        int r = rgb[i*3+0], g = rgb[i*3+1], b = rgb[i*3+2];
        Y[i]  =  (r + 2*g + b) >> 2;
        Cg[i] = -(r - 2*g + b) >> 2;
        Co[i] =  (r - b) >> 1;
    }
}

/* ── YCgCo → RGB ── */
static void ycgco_to_rgb(const int *Y, const int *Cg, const int *Co,
                          uint8_t *rgb, int n) {
    for (int i = 0; i < n; i++) {
        int tmp = Y[i] - Cg[i];
        int r = tmp + Co[i];
        int g = Y[i] + Cg[i];
        int b = tmp - Co[i];
        rgb[i*3+0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
        rgb[i*3+1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
        rgb[i*3+2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    }
}

int main(void) {
    int W, H;
    uint8_t *rgb_orig = bmp_load(BMP_PATH, &W, &H);
    if (!rgb_orig) { fprintf(stderr, "FAIL: load BMP\n"); return 1; }
    printf("BMP loaded: %dx%d\n", W, H);

    int n = W * H;
    int tiles_x = W / TILE_W, tiles_y = H / TILE_H;
    uint32_t n_tiles = (uint32_t)(tiles_x * tiles_y);

    int *Y  = (int *)malloc((size_t)n * sizeof(int));
    int *Cg = (int *)malloc((size_t)n * sizeof(int));
    int *Co = (int *)malloc((size_t)n * sizeof(int));
    rgb_to_ycgco(rgb_orig, Y, Cg, Co, n);

    /* ── encode ── */
    HbImageIn img = { Y, Cg, Co, W, H, TILE_W, TILE_H };
    printf("encoding %u tiles...\n", n_tiles);
    if (hamburger_encode_image(OUT_PATH, &img, 0xBEEFCAFEu, 0) != HB_OK) {
        fprintf(stderr, "FAIL: encode\n"); return 1;
    }

    /* file size */
    FILE *fs = fopen(OUT_PATH, "rb");
    fseek(fs, 0, SEEK_END);
    long enc_sz = ftell(fs); fclose(fs);
    long orig_sz = (long)n * 3;
    printf("encode OK  orig=%ld B  enc=%ld B  ratio=%.2fx\n",
           orig_sz, enc_sz, (double)orig_sz / (double)enc_sz);

    /* ── decode ── */
    uint8_t **tiles_out = (uint8_t **)calloc(n_tiles, sizeof(uint8_t *));
    for (uint32_t i = 0; i < n_tiles; i++)
        tiles_out[i] = (uint8_t *)calloc(1, HB_TILE_SZ_MAX);

    uint32_t nt = 0, tsz = 0;
    if (hamburger_decode(OUT_PATH, tiles_out, &nt, &tsz) != HB_OK) {
        fprintf(stderr, "FAIL: decode\n"); return 1;
    }
    printf("decode OK  n=%u  tile_sz=%u\n", nt, tsz);

    /* ── reconstruct image from tiles ── */
    int *rY  = (int *)calloc((size_t)n, sizeof(int));
    int *rCg = (int *)calloc((size_t)n, sizeof(int));
    int *rCo = (int *)calloc((size_t)n, sizeof(int));

    for (uint32_t ti = 0; ti < nt; ti++) {
        int tx = (int)(ti % (uint32_t)tiles_x);
        int ty = (int)(ti / (uint32_t)tiles_x);
        const uint8_t *td = tiles_out[ti];
        uint32_t off = 0;
        for (int y = ty*TILE_H; y < ty*TILE_H+TILE_H; y++) {
            for (int x = tx*TILE_W; x < tx*TILE_W+TILE_W; x++) {
                int idx = y*W+x;
                rY[idx]  = (int16_t)(td[off]   | (td[off+1]<<8));
                rCg[idx] = (int16_t)(td[off+2] | (td[off+3]<<8));
                rCo[idx] = (int16_t)(td[off+4] | (td[off+5]<<8));
                off += 6;
            }
        }
    }

    /* ── PSNR on Y channel ── */
    double mse = 0.0;
    for (int i = 0; i < n; i++) {
        double d = (double)(Y[i] - rY[i]);
        mse += d * d;
    }
    mse /= (double)n;
    double psnr = (mse < 1e-10) ? 999.0 : 10.0 * log10(255.0*255.0 / mse);

    /* ── tile type stats ── */
    uint8_t *ttypes = (uint8_t *)calloc(n_tiles, 1u);
    hb_classify_batch(Y, Cg, Co, W, TILE_W, TILE_H, tiles_x, tiles_y, ttypes);
    uint32_t tc[5] = {0};
    for (uint32_t i = 0; i < n_tiles; i++)
        tc[ttypes[i] < 4 ? ttypes[i] : 4]++;

    printf("\n=== CALIBRATION RESULTS ===\n");
    printf("Image      : %dx%d  tiles=%u (%dx%d)\n", W, H, n_tiles, tiles_x, tiles_y);
    printf("Orig size  : %ld B  (%.1f KB)\n", orig_sz, orig_sz/1024.0);
    printf("Enc size   : %ld B  (%.1f KB)\n", enc_sz,  enc_sz/1024.0);
    printf("Ratio      : %.2fx\n", (double)orig_sz/(double)enc_sz);
    printf("PSNR(Y)    : %.2f dB%s\n", psnr, psnr>80?"  [lossless]":" [lossy?]");
    printf("MSE        : %.4f\n", mse);
    printf("Tile types : FLAT=%u GRAD=%u EDGE=%u NOISE=%u OTHER=%u\n",
           tc[0], tc[1], tc[2], tc[3], tc[4]);

    /* ── HBHF verify ── */
    HbHeaderFrame hf;
    if (gpx5_read_hbhf(OUT_PATH, &hf) == 0) {
        printf("HBHF       : OK  n_cycles=%u total_tiles=%u\n",
               hf.n_cycles, hf.total_tiles);
    } else {
        printf("HBHF       : MISSING\n");
    }

    free(ttypes); free(rY); free(rCg); free(rCo);
    free(Y); free(Cg); free(Co); free(rgb_orig);
    for (uint32_t i = 0; i < n_tiles; i++) free(tiles_out[i]);
    free(tiles_out);
    return 0;
}
/* appended codec stat snippet — compile separately */
