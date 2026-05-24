#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "core/pogls_fold.h"
#include "geo_flow_chunker.h"

int main() {
    uint8_t rnd[10240];
    for (int i = 0; i < 10240; i++) rnd[i] = (uint8_t)(rand() & 0xFF);
    FlowSegment *segs = NULL;
    int n = flow_chunk(rnd, 10240, &segs, 0, 0, 0);
    printf("random 10KB: %d segments avg %.1fB\n", n, 10240.0/n);
    flow_segments_free(segs);
    FILE *f = fopen("core/pogls_fold.h","rb");
    if(f){fseek(f,0,SEEK_END);size_t sz=ftell(f);rewind(f);
        uint8_t *b=malloc(sz);fread(b,1,sz,f);fclose(f);
        n=flow_chunk(b,sz,&segs,0,0,0);
        printf("pogls_fold.h: %d segments avg %.1fB of %zuB\n",n,(double)sz/n,sz);
        flow_segments_free(segs);free(b);}
    return 0;
}
