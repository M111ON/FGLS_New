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
    /* build 2 cycles manually: cycle0 has tile_id 0..1, cycle1 has tile_id 0..1
     * but global index on decode: 0,1,2,3
     * If seed uses local tile_id: cycle1's tile 0 → seed(seed,0) = cycle0's tile 0
     * → decode will use seed(seed,2) for cycle1 tile0 → MISMATCH on HILBERT */

    Gpx5PipeEntry pipes[3]={
        {0,GPX5_CTYPE_RAW,GPX5_CODEC_HILBERT,33,3},
        {1,GPX5_CTYPE_RAW,GPX5_CODEC_HILBERT,33,3},
        {2,GPX5_CTYPE_RAW,GPX5_CODEC_HILBERT,34,3},
    };
    const uint32_t GSEED = 0xCAFEu;
    const uint32_t TILE_SZ = 16*16*6;

    /* make 4 distinct tiles */
    uint8_t tdata[4][16*16*6];
    for(int t=0;t<4;t++)
        for(uint32_t i=0;i<TILE_SZ;i++)
            tdata[t][i]=(uint8_t)((i*13+t*37)&0xFF);

    HbTileIn tiles_c0[2]={
        {.tile_id=0,.ttype=GPX5_TTYPE_EDGE,.data=tdata[0],.sz=TILE_SZ},
        {.tile_id=1,.ttype=GPX5_TTYPE_EDGE,.data=tdata[1],.sz=TILE_SZ},
    };
    HbTileIn tiles_c1[2]={
        {.tile_id=0,.ttype=GPX5_TTYPE_EDGE,.data=tdata[2],.sz=TILE_SZ},
        {.tile_id=1,.ttype=GPX5_TTYPE_EDGE,.data=tdata[3],.sz=TILE_SZ},
    };

    HbEncodeCtx ctx0,ctx1;
    memset(&ctx0,0,sizeof(ctx0)); hb_encode_init(&ctx0,GSEED,16,16,pipes,0);
    memset(&ctx1,0,sizeof(ctx1)); hb_encode_init(&ctx1,GSEED,16,16,pipes,0);

    const char *p0="/tmp/mc_c0.gpx5", *p1="/tmp/mc_c1.gpx5";
    hamburger_encode(p0,&ctx0,tiles_c0,2);
    hamburger_encode(p1,&ctx1,tiles_c1,2);
    hb_encode_free(&ctx0); hb_encode_free(&ctx1);

    /* decode each cycle separately */
    int pass=0,fail=0;
    const char *paths[2]={p0,p1};
    uint8_t *ref_tiles[2][2];
    for(int c=0;c<2;c++) for(int t=0;t<2;t++) ref_tiles[c][t]=calloc(1,HB_TILE_SZ_MAX);

    for(int c=0;c<2;c++){
        uint8_t *outs[2]={ref_tiles[c][0],ref_tiles[c][1]};
        uint32_t no=0,ns=0;
        hamburger_decode(paths[c],outs,&no,&ns);
    }

    for(int c=0;c<2;c++) for(int t=0;t<2;t++){
        int ti = c*2+t;
        if(memcmp(tdata[ti], ref_tiles[c][t], TILE_SZ)==0){
            printf("cycle%d tile%d PASS\n",c,t); pass++;
        } else {
            int f=-1;
            for(uint32_t i=0;i<TILE_SZ&&f<0;i++)
                if(tdata[ti][i]!=ref_tiles[c][t][i]) f=(int)i;
            printf("cycle%d tile%d FAIL first_diff=%d exp=%02X got=%02X\n",
                   c,t,f,tdata[ti][f],ref_tiles[c][t][f]); fail++;
        }
    }

    for(int c=0;c<2;c++) for(int t=0;t<2;t++) free(ref_tiles[c][t]);
    printf("\n%d/%d PASS\n",pass,pass+fail);
    return (fail==0)?0:1;
}
