#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

#define BASE_UNIT 16u
#define ROT_STATES 6u

static int calc_shell_layer(size_t n) {
    uint64_t cap = 4096; int l = 0;
    while (cap < n && l < 8) { cap *= 8; l++; }
    return l;
}
static uint32_t shell_side(int l) {
    uint32_t s = BASE_UNIT;
    for (int i=0;i<l;i++) s*=2;
    return s;
}
static void apply_rotation(uint8_t *out, const uint8_t *c, uint32_t s, uint8_t rot) {
    for (uint32_t z=0;z<s;z++) for (uint32_t y=0;y<s;y++) for (uint32_t x=0;x<s;x++) {
        uint32_t sx,sy,sz;
        switch(rot%6){
            case 0:sx=x;sy=y;sz=z;break; case 1:sx=y;sy=z;sz=x;break;
            case 2:sx=z;sy=x;sz=y;break; case 3:sx=x;sy=z;sz=y;break;
            case 4:sx=z;sy=y;sz=x;break; default:sx=y;sy=x;sz=z;break;
        }
        sx%=s;sy%=s;sz%=s;
        out[z*s*s+y*s+x]=c[sz*s*s+sy*s+sx];
    }
}
static size_t compress_buf(const uint8_t *src, size_t n, uint8_t *dst, size_t dm) {
    uLongf dl=(uLongf)dm;
    return (compress2(dst,&dl,src,(uLong)n,Z_BEST_COMPRESSION)==Z_OK)?(size_t)dl:0;
}

int main(void) {
    struct { const char *name; int pattern; size_t size; } tests[] = {
        {"Text-like (low entropy)",  0, 65536},
        {"Binary structured",        1, 65536},
        {"High entropy (random)",    2, 65536},
        {"Repetitive pattern",       3, 65536},
    };
    printf("\n=== ROTATION IMPACT ON REAL DATA PATTERNS (64KB each) ===\n");
    for (int t=0;t<4;t++) {
        size_t nb = tests[t].size;
        uint8_t *data = malloc(nb);
        // generate pattern
        for (size_t i=0;i<nb;i++) {
            switch(tests[t].pattern) {
                case 0: data[i]=(uint8_t)(32+(i%90));break;          // text
                case 1: data[i]=(uint8_t)((i%256)^(i>>8));break;     // structured binary
                case 2: data[i]=(uint8_t)((i*2654435761ULL^(i>>3)));break; // random
                case 3: data[i]=(uint8_t)(i%17);break;               // repetitive
            }
        }
        int layer=calc_shell_layer(nb);
        uint32_t side=shell_side(layer);
        uint64_t cb=(uint64_t)side*side*side;
        uint8_t *cube=calloc(cb,1);
        uint8_t *rot=malloc(cb);
        uint8_t *cz=malloc(cb*2);
        for (size_t i=0;i<nb;i++) cube[i]=data[i];

        printf("\n[%s]\n",tests[t].name);
        size_t best_cz=cb*2; uint8_t best_r=0;
        for (uint8_t r=0;r<ROT_STATES;r++) {
            apply_rotation(rot,cube,side,r);
            size_t z=compress_buf(rot,cb,cz,cb*2);
            float ratio=z?(float)nb/z:0;
            printf("  rot=%d  %6zu bytes  %.2fx", r, z, ratio);
            if (z < best_cz) { best_cz=z; best_r=r; printf(" ★"); }
            printf("\n");
        }
        printf("  → best rot=%d  net %.2fx vs raw\n", best_r, (double)nb/best_cz);
        free(data);free(cube);free(rot);free(cz);
    }
    return 0;
}
