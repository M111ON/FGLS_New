#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "gguf_reader.h"
int main(void){
    GGUF_File *gf = gguf_open("I:/model/SmolLM2-360M-Instruct.Q8_0.gguf");
    if(!gf){printf("open fail\n");return 1;}
    printf("tensors=%llu data_start=%I64u\n",
           (unsigned long long)gf->tensor_count, gf->tensor_data_start);
    GGUF_Tensor *t = &gf->tensors[0];
    printf("t0: %s type=%u n_w=%I64u offset=%I64u\n", t->name, t->type, t->n_weights, t->offset);

    fseek(gf->fp, gf->tensor_data_start + t->offset, SEEK_SET);
    for(int b=0;b<5;b++){
        uint8_t blk[34];
        if(fread(blk,1,34,gf->fp)!=34) break;
        uint16_t u = (uint16_t)(blk[0]|(blk[1]<<8));
        float ref = (float)(u & 0x7FFF)/1024.0f;
        if (u & 0x8000) ref = -ref;
        int s=(u>>15)&1, e=(u>>10)&0x1F, m=u&0x3FF;
        float d;
        if(e==0) d=(m!=0)? (float)m/1024.0f*0.000061f:0.0f;
        else if(e==31) d=(m!=0)?NAN:(s?-INFINITY:INFINITY);
        else d=(float)((m|0x400)*0.0009765625f)*ldexpf(1.0f,e-15);
        if(s) d=-d;
        printf("blk%d u=0x%04X s=%d e=%d m=%d ref=%.6f f16=%.6f q=%d,%d,%d,%d\n",
               b, u, s, e, m, ref, d, (int8_t)blk[2],(int8_t)blk[3],(int8_t)blk[4],(int8_t)blk[5]);
    }
    gguf_close(gf);
    return 0;
}