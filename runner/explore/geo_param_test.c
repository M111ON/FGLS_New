#include "core/geo_param_grid.h"
#include <stdio.h>
int main(void){
    /* 12 weights with repetition (4 distinct) */
    float w[] = {1.0f,2.0f,2.0f,3.0f,3.0f,3.0f,4.0f,4.0f,4.0f,4.0f,5.0f,5.0f};
    GeoCodec gc;
    if (geo_codec_init(&gc, GEO_COMPOUND_144, w, 12) != 0){printf("init fail\n");return 1;}
    geo_codec_stats(&gc);
    geo_codec_free(&gc);
    return 0;
}