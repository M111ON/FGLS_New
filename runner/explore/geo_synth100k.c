#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/geo_param_grid.h"
int main(void){
    int N = 100000;
    float *w = malloc(N*sizeof(float));
    for (int i=0;i<N;i++) w[i] = (float)(i % 1000) + (i%3)*0.25f;
    GeoCodec gc;
    if (geo_codec_init(&gc, GEO_AUTO, w, N) != 0){printf("init fail\n");return 1;}
    printf("init ok: distinct=%u idx_bits=%u\n", gc.n_uniq, gc.idx_bits);
    if (geo_codec_verify(&gc)==0) printf("ROUNDTRIP OK\n");
    else printf("ROUNDTRIP FAIL\n");
    geo_codec_free(&gc); free(w);
    return 0;
}