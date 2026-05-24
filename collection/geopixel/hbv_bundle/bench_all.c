/*
 * bench_all.c — ×16 + flat + shell comparison for BMP
 * gcc -O2 -I. -Icore -Inew_diamond_tring -o bench_all.exe bench_all.c -lm
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "tring.h"
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint8_t *load_bmp(const char *path, int *w, int *h, uint32_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint8_t hdr[54]; fread(hdr, 1, 54, f);
    if (hdr[0]!='B'||hdr[1]!='M') { fclose(f); return NULL; }
    *w = *(int*)(hdr+18); *h = *(int*)(hdr+22);
    int bpp = *(uint16_t*)(hdr+28);
    uint32_t off = *(uint32_t*)(hdr+10);
    int stride = ((*w * bpp / 8 + 3) / 4) * 4;
    uint32_t sz = (uint32_t)(*w * *h * 3);
    uint8_t *pix = malloc(sz);
    uint8_t *row = malloc(stride);
    fseek(f, off, SEEK_SET);
    for (int y = *h-1; y >= 0; y--) {
        fread(row, 1, stride, f);
        for (int x = 0; x < *w; x++) {
            uint8_t *d = pix + ((y * *w + x) * 3);
            d[0]=row[x*3+2]; d[1]=row[x*3+1]; d[2]=row[x*3+0];
        }
    }
    free(row); fclose(f);
    *n = sz; return pix;
}

static void pack(uint8_t *out, const uint8_t *pix, uint32_t off, uint32_t total) {
    for (int i = 0; i < 21 && off + i*3 + 2 < total; i++) {
        out[i*3]=pix[off+i*3]; out[i*3+1]=pix[off+i*3+1]; out[i*3+2]=pix[off+i*3+2];
    }
}

int main(int argc, char *argv[]) {
    const char *path = argc>1 ? argv[1] : "high_detail.bmp";
    int w, h; uint32_t sz;
    uint8_t *pix = load_bmp(path, &w, &h, &sz);
    if (!pix) { printf("Cannot load %s\n", path); return 1; }

    uint32_t n_chunks = (sz + 62) / 63;
    uint8_t *chunks = calloc(n_chunks, 64);
    for (uint32_t i = 0; i < n_chunks; i++)
        pack(chunks + i*64, pix, i*63, sz);

    printf("BMP: %dx%d = %u chunks\n\n", w, h, n_chunks);

    /* Mode 1: shell (normal) */
    {
        DiamondField df; dfield_init(&df, n_chunks*2);
        double t0 = now_ms(); uint32_t ok = 0; uint64_t out = 0;
        const uint32_t W=32;
        for (uint32_t off=0; off<n_chunks; off+=W) {
            uint32_t ws = (off+W>n_chunks)?n_chunks-off:W;
            uint32_t gb[32], e = dfield_encode_windowed(&df, chunks+off*64, ws, 16, gb);
            for (uint32_t j=0;j<e;j++) { ok++; uint32_t s; tring_read(&df.tring, sidx_get(&df.sidx,gb[j]),&s); out+=s; }
        }
        double t1 = now_ms();
        printf("  Shell (normal): %u/%u (%.0f%%)  %.0f ms  %.0f ch/s  %.1f MB/s  %.3fx\n",
               ok, n_chunks, 100.0*ok/n_chunks, t1-t0, ok/((t1-t0)/1000), ok*64.0/1e6/((t1-t0)/1000),
               (double)(n_chunks*64)/out);
        dfield_free(&df);
    }

    /* Mode 2: shell (x16) */
    {
        DiamondField df; dfield_init(&df, n_chunks*2); dfield_set_x16(&df, 1);
        double t0 = now_ms(); uint32_t ok = 0; uint64_t out = 0;
        const uint32_t W=32;
        for (uint32_t off=0; off<n_chunks; off+=W) {
            uint32_t ws = (off+W>n_chunks)?n_chunks-off:W;
            uint32_t gb[32], e = dfield_encode_windowed(&df, chunks+off*64, ws, 16, gb);
            for (uint32_t j=0;j<e;j++) { ok++; uint32_t s; tring_read(&df.tring, sidx_get(&df.sidx,gb[j]),&s); out+=s; }
        }
        double t1 = now_ms();
        printf("  Shell (x16):    %u/%u (%.0f%%)  %.0f ms  %.0f ch/s  %.1f MB/s  %.3fx\n",
               ok, n_chunks, 100.0*ok/n_chunks, t1-t0, ok/((t1-t0)/1000), ok*64.0/1e6/((t1-t0)/1000),
               (double)(n_chunks*64)/out);
        dfield_free(&df);
    }

    /* Mode 3: flat */
    {
        DiamondField df; dfield_init(&df, n_chunks+1);
        double t0 = now_ms(); uint32_t ok = 0; uint64_t out = 0;
        for (uint32_t i = 0; i < n_chunks; i++) {
            uint32_t t = dfield_encode_flat(&df, chunks+i*64);
            if (t != UINT32_MAX) { ok++; uint32_t s; tring_read(&df.tring, t, &s); out += s; }
        }
        double t1 = now_ms();
        printf("  Flat:           %u/%u (%.0f%%)  %.0f ms  %.0f ch/s  %.1f MB/s  %.3fx\n",
               ok, n_chunks, 100.0*ok/n_chunks, t1-t0, ok/((t1-t0)/1000), ok*64.0/1e6/((t1-t0)/1000),
               (double)(n_chunks*64)/out);
        dfield_free(&df);
    }

    /* Roundtrip check */
    {
        DiamondField df; dfield_init(&df, n_chunks*2); dfield_set_x16(&df, 1);
        uint32_t gb[32], e = dfield_encode_windowed(&df, chunks, 32, 16, gb);
        int ok = 1;
        for (uint32_t j=0;j<e&&j<20;j++) {
            uint8_t out[64];
            if (dfield_decode(&df, gb[j], out)!=0||memcmp(out,chunks+j*64,64)!=0) { ok=0; break; }
        }
        printf("\nRoundtrip (x16): %s\n", ok?"PASS":"FAIL");
        dfield_free(&df);
    }

    free(chunks); free(pix);
    return 0;
}
