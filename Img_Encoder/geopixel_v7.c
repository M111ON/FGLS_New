/*
 * GeoPixel v7 — Multi-Predictor Lossless Codec
 *
 * Pipeline:
 *   RGB → YCgCo-R (reversible integer)
 *   → per-tile 16×16 predictor selection (MED / AVG / GRAD / LEFT)
 *     chosen by min sum|residual| on Y channel
 *   → residuals packed as int16
 *   → predictor map (2 bit/tile) packed as uint8 stream
 *   → zstd-19 on pred_map + res_Y + res_Cg + res_Co
 *
 * Compile: gcc -O2 -o geopixel_v7 geopixel_v7.c -lm -lzstd
 * Usage  : ./geopixel_v7 input.bmp
 *          ./geopixel_v7 input.bmp.gp7    (decode)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <zstd.h>

/* ── config constants ───────────────────────────────────── */
#define MAGIC    0x47503776u   /* "GP7v" */
#define TILE     16
#define ZST_LVL  19

/* predictor IDs */
#define PRED_MED  0   /* Median Edge Detection (LOCO-I) */
#define PRED_AVG  1   /* (Left + Top) / 2               */
#define PRED_GRAD 2   /* Left + Top - TopLeft           */
#define PRED_LEFT 3   /* Left only                      */
#define N_PRED    4

/* ── types ─────────────────────────────────────────────── */
typedef struct { int w, h; uint8_t *px; } Img;

/* ── BMP I/O ────────────────────────────────────────────── */
static int bmp_load(const char *path, Img *img) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return 0; }
    img->w = *(int32_t *)(hdr + 18);
    img->h = *(int32_t *)(hdr + 22);
    int rs = (img->w * 3 + 3) & ~3;
    img->px = malloc(img->w * img->h * 3);
    fseek(f, *(uint32_t *)(hdr + 10), SEEK_SET);
    uint8_t *row = malloc(rs);
    for (int y = img->h - 1; y >= 0; y--) {
        if (fread(row, 1, rs, f) != (size_t)rs) break;
        for (int x = 0; x < img->w; x++) {
            int d = (y * img->w + x) * 3;
            img->px[d+0] = row[x*3+2];
            img->px[d+1] = row[x*3+1];
            img->px[d+2] = row[x*3+0];
        }
    }
    free(row); fclose(f); return 1;
}

static void bmp_save(const char *path, const uint8_t *px, int w, int h) {
    int rs = (w * 3 + 3) & ~3;
    int file_sz = 54 + rs * h;
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    *(uint32_t*)(hdr+2)  = file_sz;
    *(uint32_t*)(hdr+10) = 54;
    *(uint32_t*)(hdr+14) = 40;
    *(int32_t *)(hdr+18) = w;
    *(int32_t *)(hdr+22) = h;
    *(uint16_t*)(hdr+26) = 1;
    *(uint16_t*)(hdr+28) = 24;
    *(uint32_t*)(hdr+34) = rs * h;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = calloc(rs, 1);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            int d = (y * w + x) * 3;
            row[x*3+0] = px[d+2];
            row[x*3+1] = px[d+1];
            row[x*3+2] = px[d+0];
        }
        fwrite(row, 1, rs, f);
    }
    free(row); fclose(f);
}

/* ── YCgCo-R (reversible) ───────────────────────────────── */
static inline void rgb_to_ycgco(int r, int g, int b,
                                 int *Y, int *Cg, int *Co) {
    *Co  = r - b;
    int tmp = b + (*Co >> 1);
    *Cg  = g - tmp;
    *Y   = tmp + (*Cg >> 1);
}

static inline void ycgco_to_rgb(int Y, int Cg, int Co,
                                 int *r, int *g, int *b) {
    int tmp = Y - (Cg >> 1);
    *g = Cg + tmp;
    *b = tmp - (Co >> 1);
    *r = *b + Co;
}

static inline int clamp255(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* ── Predictors ─────────────────────────────────────────── */
/* fetch pixel from reconstructed plane, with border defaults */
static inline int get(const int *pl, int x, int y, int W,
                      int def_l, int def_t) {
    if (x < 0 && y < 0) return (def_l + def_t) >> 1;
    if (x < 0) return def_l;
    if (y < 0) return def_t;
    return pl[y * W + x];
}

static inline int pred_med(int L, int T, int TL) {
    /* LOCO-I MED */
    int mn = L < T ? L : T;
    int mx = L < T ? T : L;
    if (TL >= mx) return mn;
    if (TL <= mn) return mx;
    return L + T - TL;
}

static inline int predict(int pid, const int *pl,
                           int x, int y, int W,
                           int def_l, int def_t) {
    int L  = get(pl, x-1, y,   W, def_l, def_t);
    int T  = get(pl, x,   y-1, W, def_l, def_t);
    int TL = get(pl, x-1, y-1, W, def_l, def_t);
    switch (pid) {
        case PRED_MED:  return pred_med(L, T, TL);
        case PRED_AVG:  return (L + T + 1) >> 1;
        case PRED_GRAD: return L + T - TL;
        case PRED_LEFT: return L;
    }
    return L;
}

/* ── zstd helpers ───────────────────────────────────────── */
static void w32(uint8_t *b, int o, uint32_t v) {
    b[o]=v; b[o+1]=v>>8; b[o+2]=v>>16; b[o+3]=v>>24;
}
static uint32_t r32(const uint8_t *b, int o) {
    return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((uint32_t)b[o+3]<<24);
}
static uint8_t *zcomp(const uint8_t *s, int n, int *out) {
    size_t cap = ZSTD_compressBound(n);
    uint8_t *d = malloc(cap);
    *out = (int)ZSTD_compress(d, cap, s, n, ZST_LVL);
    return d;
}
static uint8_t *zdecomp(const uint8_t *s, int n, int orig) {
    uint8_t *d = malloc(orig);
    if ((int)ZSTD_decompress(d, orig, s, n) != orig) { free(d); return NULL; }
    return d;
}

/* ── PSNR ───────────────────────────────────────────────── */
static double calc_psnr(const uint8_t *a, const uint8_t *b, int n) {
    long long s = 0;
    for (int i = 0; i < n; i++) { int d = a[i]-b[i]; s += d*d; }
    double mse = (double)s / n;
    return mse == 0.0 ? 999.0 : 10.0 * log10(65025.0 / mse);
}

/* ══════════════════════════════════════════════════════════
 * ENCODE
 * ══════════════════════════════════════════════════════════ */
static int encode(const char *path) {
    Img img;
    if (!bmp_load(path, &img)) { fprintf(stderr, "load failed: %s\n", path); return -1; }
    const int W = img.w, H = img.h, N = W * H;

    /* ── 1. RGB → YCgCo ─────────────────────────────────── */
    int *iY  = malloc(N * sizeof(int));
    int *iCg = malloc(N * sizeof(int));
    int *iCo = malloc(N * sizeof(int));
    for (int i = 0; i < N; i++)
        rgb_to_ycgco(img.px[i*3], img.px[i*3+1], img.px[i*3+2],
                     &iY[i], &iCg[i], &iCo[i]);

    /* ── 2. Per-tile predictor selection ────────────────── */
    /* tile grid */
    const int TW = (W + TILE - 1) / TILE;
    const int TH = (H + TILE - 1) / TILE;
    const int NT = TW * TH;

    /* pred_map: 2 bits/tile packed into uint8 array */
    int pmap_bytes = (NT + 3) / 4;
    uint8_t *pred_map = calloc(pmap_bytes, 1);

    /* residual arrays (int16) */
    int16_t *resY  = malloc(N * 2);
    int16_t *resCg = malloc(N * 2);
    int16_t *resCo = malloc(N * 2);

    /* we build residuals scanline-by-scanline, but need
       reconstructed context → use separate rec arrays */
    int *recY  = malloc(N * sizeof(int));
    int *recCg = malloc(N * sizeof(int));
    int *recCo = malloc(N * sizeof(int));

    /* tile predictor count stats */
    int pcount[N_PRED] = {0};

    int tid = 0;
    for (int ty = 0; ty < TH; ty++) {
        for (int tx = 0; tx < TW; tx++, tid++) {
            int y0 = ty * TILE, y1 = y0 + TILE; if (y1 > H) y1 = H;
            int x0 = tx * TILE, x1 = x0 + TILE; if (x1 > W) x1 = W;

            /* ── trial: compute sum|res_Y| for each predictor ── */
            /* use already-reconstructed context for L/T/TL      */
            long best_cost = (long)1e18;
            int  best_pid  = PRED_MED;

            for (int pid = 0; pid < N_PRED; pid++) {
                long cost = 0;
                for (int y = y0; y < y1; y++) {
                    for (int x = x0; x < x1; x++) {
                        int p = predict(pid, recY, x, y, W, 128, 128);
                        cost += abs(iY[y*W+x] - p);
                    }
                }
                if (cost < best_cost) { best_cost = cost; best_pid = pid; }
            }

            /* store predictor id (2 bits) into map */
            int shift = (tid & 3) * 2;
            pred_map[tid >> 2] |= (uint8_t)(best_pid << shift);
            pcount[best_pid]++;

            /* ── encode tile pixels with best_pid ─────────── */
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    int i = y * W + x;
                    int pY  = predict(best_pid, recY,  x, y, W, 128, 128);
                    int pCg = predict(best_pid, recCg, x, y, W,   0,   0);
                    int pCo = predict(best_pid, recCo, x, y, W,   0,   0);
                    resY [i] = (int16_t)(iY [i] - pY);
                    resCg[i] = (int16_t)(iCg[i] - pCg);
                    resCo[i] = (int16_t)(iCo[i] - pCo);
                    /* reconstruct for next pixel context */
                    recY [i] = iY [i];
                    recCg[i] = iCg[i];
                    recCo[i] = iCo[i];
                }
            }
        }
    }

    /* ── 3. Compress streams ────────────────────────────── */
    int zpm_sz, zry_sz, zrg_sz, zro_sz;
    uint8_t *zpm = zcomp(pred_map,        pmap_bytes, &zpm_sz);
    uint8_t *zry = zcomp((uint8_t*)resY,  N*2,        &zry_sz);
    uint8_t *zrg = zcomp((uint8_t*)resCg, N*2,        &zrg_sz);
    uint8_t *zro = zcomp((uint8_t*)resCo, N*2,        &zro_sz);

    /* ── 4. Pack file ───────────────────────────────────── */
    /* header: 4 magic + 4 W + 4 H + 4 TW + 4 TH
     *       + 4×(4 orig + 4 zst) = 20 + 32 = 52 B
     */
    const int HDR = 4 + 4 + 4 + 4 + 4 + 4*8;  /* 52 B */
    int enc_total = HDR + zpm_sz + zry_sz + zrg_sz + zro_sz;

    uint8_t *C = malloc(enc_total);
    int off = 0;
    w32(C, off, MAGIC);         off += 4;
    w32(C, off, (uint32_t)W);   off += 4;
    w32(C, off, (uint32_t)H);   off += 4;
    w32(C, off, (uint32_t)TW);  off += 4;
    w32(C, off, (uint32_t)TH);  off += 4;
    /* stream size pairs: orig, zst */
    w32(C,off,(uint32_t)pmap_bytes); off+=4; w32(C,off,(uint32_t)zpm_sz); off+=4;
    w32(C,off,(uint32_t)(N*2));      off+=4; w32(C,off,(uint32_t)zry_sz); off+=4;
    w32(C,off,(uint32_t)(N*2));      off+=4; w32(C,off,(uint32_t)zrg_sz); off+=4;
    w32(C,off,(uint32_t)(N*2));      off+=4; w32(C,off,(uint32_t)zro_sz); off+=4;
    memcpy(C+off, zpm, zpm_sz); off += zpm_sz;
    memcpy(C+off, zry, zry_sz); off += zry_sz;
    memcpy(C+off, zrg, zrg_sz); off += zrg_sz;
    memcpy(C+off, zro, zro_sz);

    /* ── 5. Verify ──────────────────────────────────────── */
    /* decode inline to confirm lossless */
    uint8_t *d_pm = zdecomp(C + HDR,                          zpm_sz, pmap_bytes);
    uint8_t *d_ry = zdecomp(C + HDR + zpm_sz,                 zry_sz, N*2);
    uint8_t *d_rg = zdecomp(C + HDR + zpm_sz + zry_sz,        zrg_sz, N*2);
    uint8_t *d_ro = zdecomp(C + HDR + zpm_sz + zry_sz + zrg_sz, zro_sz, N*2);

    int16_t *dRY  = (int16_t *)d_ry;
    int16_t *dRCg = (int16_t *)d_rg;
    int16_t *dRCo = (int16_t *)d_ro;

    int *decY  = calloc(N, sizeof(int));
    int *decCg = calloc(N, sizeof(int));
    int *decCo = calloc(N, sizeof(int));

    tid = 0;
    for (int ty = 0; ty < TH; ty++) {
        for (int tx = 0; tx < TW; tx++, tid++) {
            int y0 = ty*TILE, y1 = y0+TILE; if (y1>H) y1=H;
            int x0 = tx*TILE, x1 = x0+TILE; if (x1>W) x1=W;
            int shift  = (tid & 3) * 2;
            int pid    = (d_pm[tid >> 2] >> shift) & 3;
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    int i = y * W + x;
                    decY [i] = dRY [i] + predict(pid, decY,  x, y, W, 128, 128);
                    decCg[i] = dRCg[i] + predict(pid, decCg, x, y, W,   0,   0);
                    decCo[i] = dRCo[i] + predict(pid, decCo, x, y, W,   0,   0);
                }
            }
        }
    }

    uint8_t *recon = malloc(N * 3);
    for (int i = 0; i < N; i++) {
        int r, g, b;
        ycgco_to_rgb(decY[i], decCg[i], decCo[i], &r, &g, &b);
        recon[i*3+0] = (uint8_t)clamp255(r);
        recon[i*3+1] = (uint8_t)clamp255(g);
        recon[i*3+2] = (uint8_t)clamp255(b);
    }

    int lossless = (memcmp(img.px, recon, (size_t)(N*3)) == 0);
    double psnr  = calc_psnr(img.px, recon, N*3);
    int raw = N * 3;

    /* ── 6. Save files ──────────────────────────────────── */
    char out[512], out_bmp[512];
    snprintf(out,     sizeof(out),     "%s.gp7",         path);
    snprintf(out_bmp, sizeof(out_bmp), "%s.decoded.bmp", path);
    FILE *fo = fopen(out, "wb");
    fwrite(C, 1, enc_total, fo);
    fclose(fo);
    bmp_save(out_bmp, recon, W, H);

    /* ── 7. Report ──────────────────────────────────────── */
    const char *pnames[N_PRED] = {"MED","AVG","GRAD","LEFT"};
    printf("Image  : %s (%dx%d)  tiles=%d×%d (%d total)\n\n",
           path, W, H, TW, TH, NT);
    printf("=== Predictor usage ===\n");
    for (int p = 0; p < N_PRED; p++)
        printf("  %-4s : %5d tiles  (%.1f%%)\n",
               pnames[p], pcount[p], 100.0*pcount[p]/NT);
    printf("\n=== Streams (raw → zstd-%d) ===\n", ZST_LVL);
    printf("  pred_map: %7d → %6d B\n", pmap_bytes, zpm_sz);
    printf("  res_Y   : %7d → %6d B  (%.2fx)\n", N*2, zry_sz, (double)(N*2)/zry_sz);
    printf("  res_Cg  : %7d → %6d B  (%.2fx)\n", N*2, zrg_sz, (double)(N*2)/zrg_sz);
    printf("  res_Co  : %7d → %6d B  (%.2fx)\n", N*2, zro_sz, (double)(N*2)/zro_sz);
    printf("  TOTAL   : %7d B  (%.2fx vs raw)\n\n", enc_total, (double)raw/enc_total);
    printf("=== Results ===\n");
    printf("  Raw      : %7d B\n",   raw);
    printf("  Encoded  : %7d B  (%.2fx)\n", enc_total, (double)raw/enc_total);
    printf("  Lossless : %s\n",      lossless ? "YES ✓" : "NO ✗ (bug)");
    printf("  PSNR     : %.2f dB\n", psnr);
    printf("  Saved    : %s\n",      out);

    /* cleanup */
    free(img.px);
    free(iY); free(iCg); free(iCo);
    free(recY); free(recCg); free(recCo);
    free(resY); free(resCg); free(resCo);
    free(pred_map);
    free(zpm); free(zry); free(zrg); free(zro);
    free(d_pm); free(d_ry); free(d_rg); free(d_ro);
    free(decY); free(decCg); free(decCo);
    free(recon); free(C);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * DECODE (standalone)
 * ══════════════════════════════════════════════════════════ */
static int decode(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open failed: %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); int fsz = ftell(f); rewind(f);
    uint8_t *C = malloc(fsz);
    if (fread(C, 1, fsz, f) != (size_t)fsz) { fclose(f); free(C); return -1; }
    fclose(f);

    if (r32(C, 0) != MAGIC) { fprintf(stderr, "bad magic\n"); free(C); return -1; }
    int W  = (int)r32(C, 4);
    int H  = (int)r32(C, 8);
    int TW = (int)r32(C,12);
    int TH = (int)r32(C,16);
    int N  = W * H;
    const int HDR = 52;

    int pmap_bytes = (int)r32(C,20); int zpm_sz = (int)r32(C,24);
    /* orig_Y  */                    int zry_sz = (int)r32(C,32);
    /* orig_Cg */                    int zrg_sz = (int)r32(C,40);
    /* orig_Co */                    int zro_sz = (int)r32(C,48);

    uint8_t *d_pm = zdecomp(C + HDR,                          zpm_sz, pmap_bytes);
    uint8_t *d_ry = zdecomp(C + HDR + zpm_sz,                 zry_sz, N*2);
    uint8_t *d_rg = zdecomp(C + HDR + zpm_sz + zry_sz,        zrg_sz, N*2);
    uint8_t *d_ro = zdecomp(C + HDR + zpm_sz + zry_sz + zrg_sz, zro_sz, N*2);

    int16_t *dRY  = (int16_t *)d_ry;
    int16_t *dRCg = (int16_t *)d_rg;
    int16_t *dRCo = (int16_t *)d_ro;

    int *decY  = calloc(N, sizeof(int));
    int *decCg = calloc(N, sizeof(int));
    int *decCo = calloc(N, sizeof(int));

    int tid = 0;
    for (int ty = 0; ty < TH; ty++) {
        for (int tx = 0; tx < TW; tx++, tid++) {
            int y0 = ty*TILE, y1 = y0+TILE; if (y1>H) y1=H;
            int x0 = tx*TILE, x1 = x0+TILE; if (x1>W) x1=W;
            int shift = (tid & 3) * 2;
            int pid   = (d_pm[tid >> 2] >> shift) & 3;
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    int i = y * W + x;
                    decY [i] = dRY [i] + predict(pid, decY,  x, y, W, 128, 128);
                    decCg[i] = dRCg[i] + predict(pid, decCg, x, y, W,   0,   0);
                    decCo[i] = dRCo[i] + predict(pid, decCo, x, y, W,   0,   0);
                }
            }
        }
    }

    uint8_t *px = malloc(N * 3);
    for (int i = 0; i < N; i++) {
        int r, g, b;
        ycgco_to_rgb(decY[i], decCg[i], decCo[i], &r, &g, &b);
        px[i*3+0] = (uint8_t)clamp255(r);
        px[i*3+1] = (uint8_t)clamp255(g);
        px[i*3+2] = (uint8_t)clamp255(b);
    }

    char out[512];
    snprintf(out, sizeof(out), "%s.bmp", path);
    bmp_save(out, px, W, H);
    printf("Decoded: %s (%dx%d) → %s\n", path, W, H, out);

    free(C); free(d_pm); free(d_ry); free(d_rg); free(d_ro);
    free(decY); free(decCg); free(decCo); free(px);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "GeoPixel v7 — Multi-Predictor Lossless\n"
            "  Encode: %s input.bmp\n"
            "  Decode: %s input.bmp.gp7\n", argv[0], argv[0]);
        return 1;
    }
    const char *path  = argv[1];
    int         is_gp7 = strstr(path, ".gp7") != NULL;

    clock_t t0 = clock();
    int ret = is_gp7 ? decode(path) : encode(path);
    clock_t t1 = clock();
    printf("  Time : %.2f ms\n", (double)(t1-t0)/CLOCKS_PER_SEC*1000.0);
    return ret;
}
