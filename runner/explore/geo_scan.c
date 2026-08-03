#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "gguf_reader.h"
int main(void){
    GGUF_File *gf = gguf_open("I:/model/SmolLM2-360M-Instruct.Q8_0.gguf");
    if(!gf){printf("open fail\n");return 1;}
    /* scan ALL tensors for abnormal f16 scales */
    uint64_t nan_blk=0, inf_blk=0, huge_blk=0, zero_blk=0, sub_blk=0, total=0;
    double scale_min=1e9, scale_max=-1e9;
    for(uint64_t ti=0; ti<gf->tensor_count; ti++){
        GGUF_Tensor *t=&gf->tensors[ti];
        if(t->type != GGML_TYPE_Q8_0) continue;
        fseek(gf->fp, gf->tensor_data_start + t->offset, SEEK_SET);
        uint64_t nblocks = (t->n_weights + 31)/32;
        for(uint64_t b=0;b<nblocks;b++){
            uint8_t blk[34];
            if(fread(blk,1,34,gf->fp)!=34) break;
            uint16_t u=(uint16_t)(blk[0]|(blk[1]<<8));
            int s=(u>>15)&1, e=(u>>10)&0x1F, m=u&0x3FF;
            float d;
            if(e==0){ d=(m!=0)?(float)m/1024.0f*0.000061f:0.0f; if(m) sub_blk++; else zero_blk++; }
            else if(e==31){ d=(m!=0)?NAN:(s?-INFINITY:INFINITY); if(m) nan_blk++; else inf_blk++; }
            else { d=(float)((m|0x400)*0.0009765625f)*ldexpf(1.0f,e-15); if(s)d=-d; }
            if(!isnan(d)&&!isinf(d)){
                float a=fabsf(d);
                if(a<scale_min)scale_min=a;
                if(a>scale_max)scale_max=a;
                if(a>100.0f) huge_blk++;
            }
            total++;
        }
    }
    printf("total blocks=%I64u\n", total);
    printf("NaN scales:  %I64u\n", nan_blk);
    printf("Inf scales:  %I64u\n", inf_blk);
    printf("zero scales: %I64u\n", zero_blk);
    printf("subnormal:   %I64u\n", sub_blk);
    printf("|d|>100:     %I64u\n", huge_blk);
    printf("|d| range:   %.6g .. %.6g\n", scale_min, scale_max);
    gguf_close(gf);
    return 0;
}