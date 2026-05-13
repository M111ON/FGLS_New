/*
 * encode_test02.c — load a 24bpp BMP, run hamburger_encode_image(),
 * then decode back and save a BMP for inspection.
 *
 * Usage:
 *   gcc -O2 -Wall -Wextra -I. -o .\temp\encode_test02.exe .\temp\encode_test02.c
 *   .\temp\encode_test02.exe .\temp\test02_768.bmp
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "hamburger_encode.h"

typedef struct {
    int w, h;
    int *Y, *Cg, *Co;
} BmpYCgCo;

static int load_bmp_ycgco(const char *path, BmpYCgCo *img) {
    memset(img, 0, sizeof(*img));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return -1; }

    int w = *(int32_t *)(hdr + 18);
    int h = *(int32_t *)(hdr + 22);
    if (h < 0) h = -h;
    uint16_t bpp = *(uint16_t *)(hdr + 28);
    int off = *(int32_t *)(hdr + 10);
    if (bpp != 24) { fclose(f); return -2; }

    int row_bytes = ((w * 3 + 3) / 4) * 4;
    uint8_t *rgb = (uint8_t *)malloc((size_t)row_bytes * (size_t)h);
    if (!rgb) { fclose(f); return -3; }
    fseek(f, off, SEEK_SET);
    if (fread(rgb, 1, (size_t)row_bytes * (size_t)h, f) != (size_t)row_bytes * (size_t)h) {
        free(rgb); fclose(f); return -4;
    }
    fclose(f);

    for (int y = 0; y < h / 2; y++) {
        uint8_t *a = rgb + (size_t)y * row_bytes;
        uint8_t *b = rgb + (size_t)(h - 1 - y) * row_bytes;
        for (int i = 0; i < row_bytes; i++) {
            uint8_t t = a[i];
            a[i] = b[i];
            b[i] = t;
        }
    }

    img->w = w;
    img->h = h;
    img->Y  = (int *)calloc((size_t)w * (size_t)h, sizeof(int));
    img->Cg = (int *)calloc((size_t)w * (size_t)h, sizeof(int));
    img->Co = (int *)calloc((size_t)w * (size_t)h, sizeof(int));
    if (!img->Y || !img->Cg || !img->Co) {
        free(rgb);
        free(img->Y); free(img->Cg); free(img->Co);
        memset(img, 0, sizeof(*img));
        return -5;
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            int B = rgb[(size_t)y * row_bytes + x * 3 + 0];
            int G = rgb[(size_t)y * row_bytes + x * 3 + 1];
            int R = rgb[(size_t)y * row_bytes + x * 3 + 2];
            int co  = R - B;
            int tmp = B + (co >> 1);
            int cg  = G - tmp;
            img->Y[idx]  = tmp + (cg >> 1);
            img->Cg[idx] = cg + 128;
            img->Co[idx] = co + 128;
        }
    }

    free(rgb);
    return 0;
}

static int save_bmp_rgb(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int bmp_row = row_bytes + pad;
    int img_sz = bmp_row * h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)(hdr + 2)  = 54u + (uint32_t)img_sz;
    *(uint32_t *)(hdr + 10) = 54u;
    *(uint32_t *)(hdr + 14) = 40u;
    *(int32_t *)(hdr + 18)  = w;
    *(int32_t *)(hdr + 22)  = h;
    *(uint16_t *)(hdr + 26) = 1u;
    *(uint16_t *)(hdr + 28) = 24u;
    *(uint32_t *)(hdr + 34) = (uint32_t)img_sz;
    if (fwrite(hdr, 1, 54, f) != 54) { fclose(f); return -2; }

    uint8_t *padrow = (uint8_t *)calloc((size_t)bmp_row, 1u);
    if (!padrow) { fclose(f); return -3; }
    for (int y = h - 1; y >= 0; y--) {
        memcpy(padrow, rgb + (size_t)y * row_bytes, (size_t)row_bytes);
        if (fwrite(padrow, 1, (size_t)bmp_row, f) != (size_t)bmp_row) {
            free(padrow); fclose(f); return -4;
        }
    }
    free(padrow);
    fclose(f);
    return 0;
}

static void free_bmp_ycgco(BmpYCgCo *img) {
    free(img->Y);
    free(img->Cg);
    free(img->Co);
    memset(img, 0, sizeof(*img));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <input.bmp>\n", argv[0]);
        return 1;
    }

    const char *in_path = argv[1];
    const char *gpx_path = ".\\temp\\test02.gpx5";
    const char *out_path = ".\\temp\\test02_decoded.bmp";

    BmpYCgCo src;
    if (load_bmp_ycgco(in_path, &src) != 0) {
        fprintf(stderr, "failed to load bmp: %s\n", in_path);
        return 2;
    }

    HbImageIn img = { src.Y, src.Cg, src.Co, src.w, src.h, 16, 16 };
    int r = hamburger_encode_image(gpx_path, &img, 0xDEADBEEFu, 0u);
    if (r != HB_OK) {
        fprintf(stderr, "encode failed: %d\n", r);
        free_bmp_ycgco(&src);
        return 3;
    }

    HbImageOut out;
    memset(&out, 0, sizeof(out));
    r = hamburger_decode_image(gpx_path, &out, src.w, src.h, 16, 16);
    if (r != HB_OK) {
        fprintf(stderr, "decode failed: %d\n", r);
        free_bmp_ycgco(&src);
        return 4;
    }

    size_t npx = (size_t)src.w * (size_t)src.h;
    uint8_t *rgb = (uint8_t *)calloc(npx * 3u, 1u);
    if (!rgb) {
        fprintf(stderr, "rgb alloc failed\n");
        hb_image_out_free(&out);
        free_bmp_ycgco(&src);
        return 5;
    }

    for (int y = 0; y < src.h; y++) {
        for (int x = 0; x < src.w; x++) {
            size_t idx = (size_t)y * (size_t)src.w + (size_t)x;
            int Y  = out.Y[idx];
            int Cg = out.Cg[idx] - 128;
            int Co = out.Co[idx] - 128;
            int tmp = Y - (Cg >> 1);
            int G = Cg + tmp;
            int B = tmp - (Co >> 1);
            int R = Co + B;
            if (R < 0) R = 0;
            if (R > 255) R = 255;
            if (G < 0) G = 0;
            if (G > 255) G = 255;
            if (B < 0) B = 0;
            if (B > 255) B = 255;
            rgb[idx * 3u + 0u] = (uint8_t)B;
            rgb[idx * 3u + 1u] = (uint8_t)G;
            rgb[idx * 3u + 2u] = (uint8_t)R;
        }
    }

    if (save_bmp_rgb(out_path, rgb, src.w, src.h) != 0) {
        fprintf(stderr, "failed to save bmp: %s\n", out_path);
        free(rgb);
        hb_image_out_free(&out);
        free_bmp_ycgco(&src);
        return 6;
    }

    FILE *g = fopen(gpx_path, "rb");
    fseek(g, 0, SEEK_END);
    long gsz = ftell(g);
    fclose(g);

    printf("encode ok: %s\n", gpx_path);
    printf("decoded bmp: %s\n", out_path);
    printf("gpx5 bytes: %ld\n", gsz);

    free(rgb);
    hb_image_out_free(&out);
    free_bmp_ycgco(&src);
    return 0;
}
