#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tring.h"
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"

static uint8_t *load_bmp(const char *path, int *w, int *h, uint32_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint8_t hdr[54]; fread(hdr, 1, 54, f);
    if (hdr[0]!='B'||hdr[1]!='M') { fclose(f); return NULL; }
    *w = *(int*)(hdr+18); *h = *(int*)(hdr+22);
    int bpp = *(uint16_t*)(hdr+28); uint32_t off = *(uint32_t*)(hdr+10);
    int stride = ((*w * bpp / 8 + 3) / 4) * 4;
    uint32_t sz = (uint32_t)(*w * *h * 3);
    uint8_t *pix = malloc(sz); uint8_t *row = malloc(stride);
    fseek(f, off, SEEK_SET);
    for (int y = *h-1; y >= 0; y--) { fread(row, 1, stride, f);
        for (int x = 0; x < *w; x++) { uint8_t *d = pix + ((y * *w + x) * 3);
            d[0]=row[x*3+2]; d[1]=row[x*3+1]; d[2]=row[x*3+0]; } }
    free(row); fclose(f); *n = sz; return pix;
}

int main(void) {
    int w, h; uint32_t sz;
    uint8_t *pix = load_bmp("high_detail.bmp", &w, &h, &sz);
    if (!pix) return 1;
    uint32_t n = (sz + 62) / 63;
    uint8_t *chunks = calloc(n, 64);
    for (uint32_t i = 0; i < n; i++)
        for (int j = 0; j < 21 && i*63+j*3+2 < sz; j++)
            { chunks[i*64+j*3]=pix[i*63+j*3]; chunks[i*64+j*3+1]=pix[i*63+j*3+1]; chunks[i*64+j*3+2]=pix[i*63+j*3+2]; }

    DiamondField df; dfield_init(&df, n*2); dfield_set_x16(&df, 1);

    uint32_t ok=0, *gidxs = malloc(n * 4), ndebug=0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t g = dfield_encode(&df, chunks + i*64, NULL);
        if (g != SLOT_NULL) gidxs[ok++] = g;
    }

    for (uint32_t i = 0; i < ok && ndebug < 10; i++) {
        uint32_t g = gidxs[i];
        uint8_t lv = global_level(g); uint16_t idx = global_local(g);
        uint16_t sc = df.shell[lv].slot_count;
        int sh = (idx < sc) ? shell_get(&df.shell[lv], idx) : 999;
        int sv = sidx_get(&df.sidx, g);
        int is_sub = (idx >= sc);
        if (sh == 0 && idx < sc) {
            printf("FAIL chunk %u: lv=%u idx=%u sc=%u shell_get=%d sidx_val=0x%x sub=%d\n",
                   i, lv, idx, sc, sh, sv, is_sub);
            ndebug++;
        }
    }

    printf("ok=%u debug_shown=%u\n", ok, ndebug);
    dfield_free(&df); free(gidxs); free(chunks); free(pix);
    return 0;
}
