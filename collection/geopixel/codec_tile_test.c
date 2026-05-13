#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "hamburger_encode.h"

static int load_tile(const char *path, int tx, int ty, int tw, int th, uint8_t **out, uint32_t *out_sz) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    uint8_t hdr[54]; if (fread(hdr,1,54,f)!=54) return -2;
    int w = *(int32_t*)(hdr+18), h = *(int32_t*)(hdr+22); if (h<0) h=-h;
    int off = *(int32_t*)(hdr+10); uint16_t bpp = *(uint16_t*)(hdr+28); if (bpp!=24) return -3;
    int row_bytes = ((w*3+3)/4)*4;
    uint8_t *rgb = malloc((size_t)row_bytes*h); if (!rgb) return -4;
    fseek(f, off, SEEK_SET); fread(rgb,1,(size_t)row_bytes*h,f); fclose(f);
    for(int y=0;y<h/2;y++){ uint8_t *a=rgb+(size_t)y*row_bytes, *b=rgb+(size_t)(h-1-y)*row_bytes; for(int i=0;i<row_bytes;i++){uint8_t t=a[i];a[i]=b[i];b[i]=t;}}
    uint8_t *buf = malloc((size_t)tw*th*6u); if (!buf) return -5;
    uint32_t offb=0;
    for (int y = ty*th; y < ty*th+th; y++) {
        for (int x = tx*tw; x < tx*tw+tw; x++) {
            int B = rgb[(size_t)y*row_bytes + x*3 + 0];
            int G = rgb[(size_t)y*row_bytes + x*3 + 1];
            int R = rgb[(size_t)y*row_bytes + x*3 + 2];
            int co  = R - B;
            int tmp = B + (co >> 1);
            int cg  = G - tmp;
            int Y   = tmp + (cg >> 1);
            int16_t yy = (int16_t)Y, ccg = (int16_t)(cg + 128), cco = (int16_t)(co + 128);
            buf[offb+0]=(uint8_t)(yy & 0xFF); buf[offb+1]=(uint8_t)(yy>>8);
            buf[offb+2]=(uint8_t)(ccg & 0xFF); buf[offb+3]=(uint8_t)(ccg>>8);
            buf[offb+4]=(uint8_t)(cco & 0xFF); buf[offb+5]=(uint8_t)(cco>>8);
            offb += 6u;
        }
    }
    free(rgb); *out = buf; *out_sz = offb; return 0;
}

static int roundtrip(uint8_t codec, uint8_t ttype, const uint8_t *in, uint32_t sz, uint32_t seed) {
    uint8_t enc[8192]; uint8_t dec[8192]; memset(enc,0,sizeof(enc)); memset(dec,0,sizeof(dec));
    uint32_t esz = hb_codec_apply(codec, ttype, in, sz, enc, sizeof(enc), seed);
    uint32_t dsz = hb_codec_invert(codec, ttype, enc, esz, dec, sizeof(dec), seed);
    int ok = (esz>0 && dsz==sz && memcmp(in, dec, sz)==0);
    printf("codec %u: esz=%u dsz=%u ok=%d\n", codec, esz, dsz, ok);
    return ok ? 0 : 1;
}

int main(void) {
    uint8_t *tile = NULL; uint32_t sz = 0;
    /* use a known gradient tile from the preview-calibrated image */
    if (load_tile("I:\\FGLS_new\\collection\\temp\\test02_768.bmp", 1, 0, 16, 16, &tile, &sz) != 0) return 1;
    uint32_t seed = 0xDEADBEEFu;
    int rc = 0;
    rc |= roundtrip(GPX5_CODEC_SEED, GPX5_TTYPE_FLAT, tile, sz, seed);
    rc |= roundtrip(GPX5_CODEC_FREQ, GPX5_TTYPE_GRADIENT, tile, sz, seed);
    rc |= roundtrip(GPX5_CODEC_RICE3, GPX5_TTYPE_EDGE, tile, sz, seed);
    rc |= roundtrip(GPX5_CODEC_ZSTD19, GPX5_TTYPE_NOISE, tile, sz, seed);
    free(tile);
    return rc;
}
