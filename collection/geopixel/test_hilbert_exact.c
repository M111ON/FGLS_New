#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gpx5_container.h"
#include "fibo_shell_walk.h"
#include "hamburger_classify.h"
#include "hamburger_pipe.h"
#include "hamburger_encode.h"

int main(void) {
    int pass=0, fail=0;
    const uint32_t seed_local = gpx5_seed_local(0xABCD1234u, 0u);
    uint8_t in[256], enc[1024], dec[512];
    for (int i=0;i<256;i++) in[i]=(uint8_t)(i*3+7);
    memset(enc,0,sizeof(enc));

    /* T1: codec_apply → codec_invert exact roundtrip */
    uint32_t enc_sz = hb_codec_apply(GPX5_CODEC_HILBERT, GPX5_TTYPE_EDGE,
                                      in, 256, enc, 1024, seed_local);
    printf("enc_sz=%u\n", enc_sz);
    uint32_t dec_sz = hb_codec_invert(GPX5_CODEC_HILBERT, GPX5_TTYPE_EDGE,
                                       enc, enc_sz, dec, 512, seed_local);
    if (dec_sz==256 && memcmp(in,dec,256)==0) { printf("T1 PASS\n"); pass++; }
    else { int f=-1; for(int i=0;i<256&&f<0;i++) if(in[i]!=dec[i]) f=i;
           printf("T1 FAIL dec_sz=%u first_diff=%d\n",dec_sz,f); fail++; }

    /* T2: all-prediction → minimal enc_sz */
    uint8_t in2[64];
    for(int i=0;i<64;i++) in2[i]=hb_predict(seed_local,(uint32_t)i);
    memset(enc,0,sizeof(enc));
    enc_sz = hb_codec_apply(GPX5_CODEC_HILBERT,GPX5_TTYPE_FLAT,in2,64,enc,1024,seed_local);
    uint32_t exp_min = 2u + ((64u+7u)/8u);
    if (enc_sz==exp_min) { printf("T2 PASS all-pred enc_sz=%u\n",enc_sz); pass++; }
    else { printf("T2 FAIL enc_sz=%u expected=%u\n",enc_sz,exp_min); fail++; }

    /* T3: full encode+decode via hamburger_encode / hamburger_decode */
    #define W 16
    #define H 16
    uint8_t tdata[W*H*6];
    for(int i=0;i<W*H;i++){
        hb_store_i16le(tdata+i*6+0,(int16_t)((i*17+3)&0xFF));
        hb_store_i16le(tdata+i*6+2,(int16_t)128);
        hb_store_i16le(tdata+i*6+4,(int16_t)128);
    }
    HbTileIn tile={.tile_id=0,.ttype=GPX5_TTYPE_EDGE,.data=tdata,.sz=W*H*6};

    Gpx5PipeEntry pipes[3]={
        {0,GPX5_CTYPE_RAW,GPX5_CODEC_HILBERT,33,3},
        {1,GPX5_CTYPE_RAW,GPX5_CODEC_HILBERT,33,3},
        {2,GPX5_CTYPE_RAW,GPX5_CODEC_HILBERT,34,3},
    };
    HbEncodeCtx ctx; memset(&ctx,0,sizeof(ctx));
    hb_encode_init(&ctx,0xDEADu,W,H,pipes,0);

    const char *p="/tmp/hb_hilbert3.gpx5";
    int rc = hamburger_encode(p, &ctx, &tile, 1);
    if (rc!=HB_OK) { printf("T3 FAIL encode rc=%d\n",rc); fail++; }
    else {
        uint8_t *out_t=calloc(1,HB_TILE_SZ_MAX);
        uint8_t *outs[1]={out_t};
        uint32_t no=0,ns=0;
        hamburger_decode(p,outs,&no,&ns);
        if (memcmp(tdata,out_t,tile.sz)==0) { printf("T3 PASS encode→decode exact\n"); pass++; }
        else {
            int f=-1; for(uint32_t i=0;i<tile.sz&&f<0;i++) if(tdata[i]!=out_t[i]) f=(int)i;
            printf("T3 FAIL first_diff=%d exp=%02X got=%02X\n",f,tdata[f],out_t[f]); fail++;
        }
        free(out_t);
    }
    hb_encode_free(&ctx);

    printf("\n%d/%d PASS\n",pass,pass+fail);
    return (fail==0)?0:1;
}
