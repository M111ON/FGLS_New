#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/geo_param_grid.h"
int main(void){
    /* synthetic: 5000 weights, 430 distinct w/ repeats */
    int N = 5000, D = 430;
    float *w = malloc(N*sizeof(float));
    for (int i=0;i<N;i++) w[i] = (float)(i % D);
    GeoCodec gc;
    if (geo_codec_init(&gc, GEO_DODEC_BASE, w, N) != 0){printf("init fail\n");return 1;}
    float *recon = malloc(N*sizeof(float));
    geo_codec_decode(&gc, recon, N);
    int mm=0; float first_a=0, first_b=0; int first_i=-1;
    for (int i=0;i<N;i++) if (w[i]!=recon[i]) { mm++; if(first_i<0){first_i=i;first_a=w[i];first_b=recon[i];} }
    printf("distinct=%u idx_bits=%u mismatches=%d first@%d: %f vs %f\n",
           gc.n_uniq, gc.idx_bits, mm, first_i, first_a, first_b);
    geo_codec_free(&gc); free(recon); free(w);
    return 0;
}
