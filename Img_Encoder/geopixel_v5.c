/*
 * GeoPixel v5 — Best-of-Three Hybrid Codec
 *
 * Pipeline:
 *   RGB → YCgCo-R (lossless integer, v2)
 *   → MED spatial prediction residuals (v2)
 *   → per-tile 16×16 histogram on axis 0..767 (v4)
 *   → zstd-19 on all streams (v4)
 *
 * Compile: gcc -O2 -o geopixel_v5 geopixel_v5.c -lm -lzstd
 * Usage  : ./geopixel_v5 input.bmp [merge_thresh]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <zstd.h>

/* ── constants ─────────────────────────────────────────── */
#define MAGIC    0x47503576u   /* "GP5v" */
#define TILE     16
#define AXIS     768           /* R:0-255  CG:256-511  CO:512-767 */
#define ZST_LVL  19
#define MAX_W    8192
#define MAX_H    8192

/* ── types ─────────────────────────────────────────────── */
typedef struct { int w, h; uint8_t *px; } Img;

typedef struct {
    uint8_t *buf;
    int      sz, cap;
} Buf;

/* ── BMP loader ────────────────────────────────────────── */
static int bmp_load(const char *path, Img *img) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[54];
    fread(hdr, 1, 54, f);
    img->w = *(int32_t *)(hdr + 18);
    img->h = *(int32_t *)(hdr + 22);
    int rs = (img->w * 3 + 3) & ~3;
    img->px = malloc(img->w * img->h * 3);
    fseek(f, *(uint32_t *)(hdr + 10), SEEK_SET);
    uint8_t *row = malloc(rs);
    for (int y = img->h - 1; y >= 0; y--) {
        fread(row, 1, rs, f);
        for (int x = 0; x < img->w; x++) {
            int d = (y * img->w + x) * 3;
            img->px[d + 0] = row[x * 3 + 2]; /* R */
            img->px[d + 1] = row[x * 3 + 1]; /* G */
            img->px[d + 2] = row[x * 3 + 0]; /* B */
        }
    }
    free(row); fclose(f); return 1;
}

/* ── BMP writer ────────────────────────────────────────── */
static void bmp_save(const char *path, const uint8_t *px, int w, int h) {
    int rs = (w * 3 + 3) & ~3;
    int data_sz = rs * h;
    int file_sz = 54 + data_sz;
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54] = {0};
    /* BMP header */
    hdr[0]='B'; hdr[1]='M';
    *(uint32_t*)(hdr+2)  = file_sz;
    *(uint32_t*)(hdr+10) = 54;
    /* DIB header */
    *(uint32_t*)(hdr+14) = 40;
    *(int32_t *)(hdr+18) = w;
    *(int32_t *)(hdr+22) = h;
    *(uint16_t*)(hdr+26) = 1;
    *(uint16_t*)(hdr+28) = 24;
    *(uint32_t*)(hdr+34) = data_sz;
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

/* ── YCgCo-R (reversible integer, v2) ─────────────────── */
static inline void rgb_to_ycgco(int r, int g, int b,
                                 int *Y, int *Cg, int *Co) {
    *Co  = r - b;
    int tmp = b + (*Co) / 2;
    *Cg  = g - tmp;
    *Y   = tmp + (*Cg) / 2;
}

static inline void ycgco_to_rgb(int Y, int Cg, int Co,
                                 int *r, int *g, int *b) {
    int tmp = Y - Cg / 2;
    *g = Cg + tmp;
    *b = tmp - Co / 2;
    *r = *b + Co;
}

static inline int clamp255(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* ── MED predictor (v2) ────────────────────────────────── */
static inline int med3(int a, int b, int c) {
    if (c >= a && c >= b) return a + b - c;
    if (c <= a && c <= b) return a + b - c;
    return c;
}

static inline int pred(const int *plane, int x, int y, int W,
                        int def_left, int def_top) {
    int L  = (x > 0)           ? plane[y * W + x - 1]       : def_left;
    int T  = (y > 0)           ? plane[(y-1) * W + x]        : def_top;
    int TL = (x > 0 && y > 0) ? plane[(y-1) * W + x - 1]   : def_left;
    return med3(L, T, TL);
}

/* ── dynamic buffer ────────────────────────────────────── */
static Buf newbuf(int cap) { Buf b = {malloc(cap), 0, cap}; return b; }
static void bp(Buf *b, uint8_t v) {
    if (b->sz == b->cap) { b->cap *= 2; b->buf = realloc(b->buf, b->cap); }
    b->buf[b->sz++] = v;
}
static void bp16(Buf *b, uint16_t v) { bp(b, v & 0xFF); bp(b, v >> 8); }

/* varint delta-pos: 1→0x01..255→0xFF, else 0x00+u16 */
static void push_dp(Buf *b, int d) {
    if (d > 0 && d < 256) bp(b, (uint8_t)d);
    else { bp(b, 0); bp16(b, (uint16_t)d); }
}
/* varint count: <128→1B, ≥128→0x80|hi + lo */
static void push_cnt(Buf *b, int c) {
    if (c < 128) bp(b, (uint8_t)c);
    else { bp(b, (uint8_t)(0x80 | (c >> 8))); bp(b, (uint8_t)(c & 0xFF)); }
}

/* ── zstd helpers ──────────────────────────────────────── */
static void w32(uint8_t *buf, int o, uint32_t v) {
    buf[o]=v&0xFF; buf[o+1]=(v>>8)&0xFF;
    buf[o+2]=(v>>16)&0xFF; buf[o+3]=v>>24;
}
static uint32_t r32(const uint8_t *buf, int o) {
    return buf[o] | (buf[o+1]<<8) | (buf[o+2]<<16) | ((uint32_t)buf[o+3]<<24);
}
static uint8_t* zcomp(const uint8_t *s, int n, int *out) {
    size_t cap = ZSTD_compressBound(n);
    uint8_t *d = malloc(cap);
    *out = (int)ZSTD_compress(d, cap, s, n, ZST_LVL);
    return d;
}
static uint8_t* zdecomp(const uint8_t *s, int n, int orig) {
    uint8_t *d = malloc(orig);
    if ((int)ZSTD_decompress(d, orig, s, n) != orig) { free(d); return NULL; }
    return d;
}

/* ── PSNR ──────────────────────────────────────────────── */
static double calc_psnr(const uint8_t *a, const uint8_t *b, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) { int d = a[i]-b[i]; s += d*d; }
    double mse = (double)s / n;
    return mse == 0 ? 999.0 : 10.0 * log10(65025.0 / mse);
}

/* ══════════════════════════════════════════════════════════
 * ENCODE
 * ══════════════════════════════════════════════════════════ */
static int encode(const char *path, int merge) {
    Img img;
    if (!bmp_load(path, &img)) { fprintf(stderr, "load failed: %s\n", path); return -1; }
    const int W = img.w, H = img.h, N = W * H;

    /* 1. RGB → YCgCo (full-res int arrays for MED prediction) */
    int *iY  = malloc(N * sizeof(int));
    int *iCg = malloc(N * sizeof(int));
    int *iCo = malloc(N * sizeof(int));

    for (int i = 0; i < N; i++) {
        int r = img.px[i*3], g = img.px[i*3+1], b = img.px[i*3+2];
        rgb_to_ycgco(r, g, b, &iY[i], &iCg[i], &iCo[i]);
    }

    /* 2. MED prediction → residuals, then clamp to [0,255] for axis */
    /* YCgCo range: Y≈0-255, Cg≈-256..255, Co≈-256..255 → offset by 256 */
    #define CG_OFF 256
    #define CO_OFF 256

    /* axis encoding: R=Y (0-255 direct), CG=Cg+CG_OFF (mapped to 0-255 quantised),
       CO=Co+CO_OFF similarly — we store raw signed residuals per-pixel
       and feed the tile histogram the original transform values */

    /* Build YCgCo as uint8 for axis histogram:
       Y direct [0,255], Cg offset+clamp, Co offset+clamp */
    uint8_t *ycc = malloc(N * 3);
    for (int i = 0; i < N; i++) {
        ycc[i*3+0] = (uint8_t)clamp255(iY[i]);
        ycc[i*3+1] = (uint8_t)clamp255(iCg[i] + 128); /* shift to 0-255 */
        ycc[i*3+2] = (uint8_t)clamp255(iCo[i] + 128);
    }

    /* 3. MED residuals for Y/Cg/Co (used for lossless reconstruction) */
    int *resY  = malloc(N * sizeof(int));
    int *resCg = malloc(N * sizeof(int));
    int *resCo = malloc(N * sizeof(int));

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            resY [i] = iY [i] - pred(iY,  x, y, W, 128, 128);
            resCg[i] = iCg[i] - pred(iCg, x, y, W,   0,   0);
            resCo[i] = iCo[i] - pred(iCo, x, y, W,   0,   0);
        }
    }

    /* 4. Tile histogram streams (v4 style, on ycc uint8) */
    int tw = (W + TILE - 1) / TILE;
    int th = (H + TILE - 1) / TILE;
    int nt = tw * th;
    int bmap_bytes = (nt + 7) / 8;

    Buf bmap = newbuf(bmap_bytes + 1);
    bmap.sz  = bmap_bytes;
    memset(bmap.buf, 0, bmap_bytes);

    Buf s_ne  = newbuf(nt * 2 + 8);
    Buf s_dp  = newbuf(1 << 21);
    Buf s_cnt = newbuf(1 << 20);

    int tid = 0;
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++, tid++) {
            int freq[AXIS] = {0};
            int tn = 0;
            for (int dy = 0; dy < TILE; dy++) {
                int py = ty * TILE + dy; if (py >= H) continue;
                for (int dx = 0; dx < TILE; dx++) {
                    int px = tx * TILE + dx; if (px >= W) continue;
                    int i = py * W + px;
                    freq[       ycc[i*3+0]]++;  /* Y  → axis 0-255   */
                    freq[256 +  ycc[i*3+1]]++;  /* Cg → axis 256-511 */
                    freq[512 +  ycc[i*3+2]]++;  /* Co → axis 512-767 */
                    tn++;
                }
            }
            if (!tn) continue;
            bmap.buf[tid >> 3] |= (1 << (tid & 7));

            /* optional merge (lossy) */
            if (merge > 0) {
                for (int p = 0; p < AXIS - 1; p++)
                    if (freq[p] && freq[p] <= merge) { freq[p+1] += freq[p]; freq[p] = 0; }
            }

            int ne = 0;
            for (int p = 0; p < AXIS; p++) if (freq[p]) ne++;
            bp16(&s_ne, (uint16_t)ne);

            int prev = 0;
            for (int p = 0; p < AXIS; p++) {
                if (!freq[p]) continue;
                push_dp(&s_dp, p - prev);
                push_cnt(&s_cnt, freq[p]);
                prev = p;
            }
        }
    }

    /* 5. Compress all streams with zstd-19 */
    int zbm_sz, zne_sz, zdp_sz, zct_sz, zry_sz, zrg_sz, zro_sz;

    /* pack residuals as int16 */
    int16_t *pResY  = malloc(N * 2);
    int16_t *pResCg = malloc(N * 2);
    int16_t *pResCo = malloc(N * 2);
    for (int i = 0; i < N; i++) {
        pResY [i] = (int16_t)resY [i];
        pResCg[i] = (int16_t)resCg[i];
        pResCo[i] = (int16_t)resCo[i];
    }

    uint8_t *zbm  = zcomp(bmap.buf,        bmap.sz,    &zbm_sz);
    uint8_t *zne  = zcomp(s_ne.buf,         s_ne.sz,   &zne_sz);
    uint8_t *zdp  = zcomp(s_dp.buf,         s_dp.sz,   &zdp_sz);
    uint8_t *zct  = zcomp(s_cnt.buf,        s_cnt.sz,  &zct_sz);
    uint8_t *zry  = zcomp((uint8_t*)pResY,  N*2,       &zry_sz);
    uint8_t *zrg  = zcomp((uint8_t*)pResCg, N*2,       &zrg_sz);
    uint8_t *zro  = zcomp((uint8_t*)pResCo, N*2,       &zro_sz);

    /* 6. Pack file */
    /* header: 4 magic + 4 W + 4 H + 7×(4 orig + 4 zst) = 68 B */
    int hdr_sz = 4 + 4 + 4 + 7 * 8;
    int enc_total = hdr_sz
        + zbm_sz + zne_sz + zdp_sz + zct_sz
        + zry_sz + zrg_sz + zro_sz;

    uint8_t *C = malloc(enc_total);
    int off = 0;
    w32(C, off, MAGIC);       off += 4;
    w32(C, off, (uint32_t)W); off += 4;
    w32(C, off, (uint32_t)H); off += 4;

    #define WRITE_STREAM(orig_sz, zst_sz, zst_buf) \
        w32(C,off,(uint32_t)(orig_sz)); off+=4; \
        w32(C,off,(uint32_t)(zst_sz));  off+=4; \
        memcpy(C+off,(zst_buf),(zst_sz)); /* written later */

    /* store sizes first, then blobs */
    w32(C,off,(uint32_t)bmap.sz); off+=4; w32(C,off,(uint32_t)zbm_sz); off+=4;
    w32(C,off,(uint32_t)s_ne.sz); off+=4; w32(C,off,(uint32_t)zne_sz); off+=4;
    w32(C,off,(uint32_t)s_dp.sz); off+=4; w32(C,off,(uint32_t)zdp_sz); off+=4;
    w32(C,off,(uint32_t)s_cnt.sz);off+=4; w32(C,off,(uint32_t)zct_sz); off+=4;
    w32(C,off,(uint32_t)(N*2));   off+=4; w32(C,off,(uint32_t)zry_sz); off+=4;
    w32(C,off,(uint32_t)(N*2));   off+=4; w32(C,off,(uint32_t)zrg_sz); off+=4;
    w32(C,off,(uint32_t)(N*2));   off+=4; w32(C,off,(uint32_t)zro_sz); off+=4;

    memcpy(C+off, zbm,  zbm_sz);  off += zbm_sz;
    memcpy(C+off, zne,  zne_sz);  off += zne_sz;
    memcpy(C+off, zdp,  zdp_sz);  off += zdp_sz;
    memcpy(C+off, zct,  zct_sz);  off += zct_sz;
    memcpy(C+off, zry,  zry_sz);  off += zry_sz;
    memcpy(C+off, zrg,  zrg_sz);  off += zrg_sz;
    memcpy(C+off, zro,  zro_sz);

    /* 7. Decode & verify */
    /* --- tile histogram decode (approximate, not lossless alone) --- */
    /* --- lossless path: MED residual reconstruct --- */
    off = 4 + 4 + 4 + 7*8;         /* skip to blobs */
    off += zbm_sz + zne_sz + zdp_sz + zct_sz;

    uint8_t *d_ry = zdecomp(C + off, zry_sz, N*2); off += zry_sz;
    uint8_t *d_rg = zdecomp(C + off, zrg_sz, N*2); off += zrg_sz;
    uint8_t *d_ro = zdecomp(C + off, zro_sz, N*2);

    int16_t *dResY  = (int16_t *)d_ry;
    int16_t *dResCg = (int16_t *)d_rg;
    int16_t *dResCo = (int16_t *)d_ro;

    int *decY  = malloc(N * sizeof(int));
    int *decCg = malloc(N * sizeof(int));
    int *decCo = malloc(N * sizeof(int));

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            decY [i] = dResY [i] + pred(decY,  x, y, W, 128, 128);
            decCg[i] = dResCg[i] + pred(decCg, x, y, W,   0,   0);
            decCo[i] = dResCo[i] + pred(decCo, x, y, W,   0,   0);
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

    int lossless = (merge == 0) && (memcmp(img.px, recon, N*3) == 0);
    double psnr  = calc_psnr(img.px, recon, N*3);
    int raw = N * 3;

    /* save .gp5 */
    char out[512];
    snprintf(out, sizeof(out), "%s.gp5", path);
    FILE *fo = fopen(out, "wb");
    fwrite(C, 1, enc_total, fo);
    fclose(fo);

    /* save decoded BMP */
    char out_bmp[512];
    snprintf(out_bmp, sizeof(out_bmp), "%s.decoded.bmp", path);
    bmp_save(out_bmp, recon, W, H);

    /* report */
    printf("Image : %s (%dx%d)  merge=%d\n\n", path, W, H, merge);
    printf("=== Streams (raw → zstd-%d) ===\n", ZST_LVL);
    printf("  bitmap   : %7d → %6d B\n", bmap.sz, zbm_sz);
    printf("  n_entries: %7d → %6d B\n", s_ne.sz,  zne_sz);
    printf("  delta-pos: %7d → %6d B\n", s_dp.sz,  zdp_sz);
    printf("  count    : %7d → %6d B\n", s_cnt.sz, zct_sz);
    printf("  res_Y    : %7d → %6d B\n", N*2, zry_sz);
    printf("  res_Cg   : %7d → %6d B\n", N*2, zrg_sz);
    printf("  res_Co   : %7d → %6d B\n", N*2, zro_sz);
    printf("  TOTAL    : %7d B  (%.2fx vs raw)\n\n",
           enc_total, (double)raw / enc_total);
    printf("=== Results ===\n");
    printf("  Raw      : %7d B\n", raw);
    printf("  Encoded  : %7d B  (%.2fx)\n", enc_total, (double)raw / enc_total);
    printf("  Lossless : %s\n", merge == 0 ? (lossless ? "YES ✓" : "NO ✗ (bug)") : "N/A (lossy)");
    printf("  PSNR     : %.2f dB\n", psnr);
    printf("  Saved    : %s\n", out);
    printf("  Decoded  : %s\n", out_bmp);

    /* cleanup */
    free(img.px); free(iY); free(iCg); free(iCo);
    free(ycc); free(resY); free(resCg); free(resCo);
    free(pResY); free(pResCg); free(pResCo);
    free(bmap.buf); free(s_ne.buf); free(s_dp.buf); free(s_cnt.buf);
    free(zbm); free(zne); free(zdp); free(zct);
    free(zry); free(zrg); free(zro);
    free(d_ry); free(d_rg); free(d_ro);
    free(decY); free(decCg); free(decCo); free(recon); free(C);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    const char *path  = argc > 1 ? argv[1] : "test02.bmp";
    int         merge = argc > 2 ? atoi(argv[2]) : 0;

    clock_t t0 = clock();
    int ret = encode(path, merge);
    clock_t t1 = clock();

    printf("  Time     : %.2f ms\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0);
    return ret;
}
