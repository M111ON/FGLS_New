#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "gguf_reader.h"
int main(void){
    GGUF_File *gf = gguf_open("I:/model/SmolLM2-360M-Instruct.Q8_0.gguf");
    if(!gf){printf("open fail\n");return 1;}
    /* Compare both decodes' scale distribution on tensor 0 */
    GGUF_Tensor *t=&gf->tensors[0];
    fseek(gf->fp, gf->tensor_data_start + t->offset, SEEK_SET);
    uint64_t nblocks = (t->n_weights + 31)/32;
    uint64_t huge_ref=0, zero_ref=0, big_ref=0;
    uint64_t huge_f16=0, zero_f16=0, nan_f16=0;
    double ref_min=1e9, ref_max=-1e9;
    double f16_min=1e9, f16_max=-1e9;
    for(uint64_t b=0;b<nblocks && b<200000;b++){
        uint8_t blk[34];
        if(fread(blk,1,34,gf->fp)!=34) break;
        uint16_t u=(uint16_t)(blk[0]|(blk[1]<<8));
        /* ref: fixed-point 15-bit /1024 */
        float ref=(float)(u & 0x7FFF)/1024.0f;
        if(u & 0x8000) ref=-ref;
        float ra=fabsf(ref);
        if(ra<ref_min)ref_min=ra; if(ra>ref_max)ref_max=ra;
        if(ra>100.0f) huge_ref++;
        if(ra<1e-30f) zero_ref++;
        if(ra>5.0f) big_ref++;
        /* f16 decode */
        int s=(u>>15)&1, e=(u>>10)&0x1F, m=u&0x3FF;
        float d;
        if(e==0) d=(m!=0)?(float)m/1024.0f*0.000061f:0.0f;
        else if(e==31) d=(m!=0)?NAN:(s?-INFINITY:INFINITY);
        else d=(float)((m|0x400)*0.0009765625f)*ldexpf(1.0f,e-15);
        if(s) d=-d;
        if(isnan(d)||isinf(d)){ nan_f16++; continue; }
        float da=fabsf(d);
        if(da<f16_min)f16_min=da; if(da>f16_max)f16_max=da;
        if(da>100.0f) huge_f16++;
        if(da<1e-30f) zero_f16++;
    }
    printf("=== tensor0 first 200K blocks ===\n");
    printf("REF (fixed-point/1024): |d| range %.6g..%.6g |d|>100: %I64u |d|<1e-30: %I64u >5: %I64u\n",
           ref_min, ref_max, huge_ref, zero_ref, big_ref);
    printf("F16 decode:             |d| range %.6g..%.6g |d|>100: %I64u |d|<1e-30: %I64u NaN/Inf: %I64u\n",
           f16_min, f16_max, huge_f16, zero_f16, nan_f16);
    gguf_close(gf);
    return 0;
}