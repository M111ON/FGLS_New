/*
 * Minimal integrity test — pure D4 → Hilbert → seed-index → decode
 * No DiamondBlock, no geometry — isolates the exact issue.
 * Build: gcc -O2 -I. -Icore/twin_core test_integrity_mini.c -lm
 */
 #include <stdint.h>
 #include <string.h>
 #include <stdio.h>
 #include <stdlib.h>

 static uint8_t G_H[64], G_HI[64];
 static void init_h(void){
     uint8_t hmap[64];
     for(int p=0;p<64;p++){
         uint32_t px=p%8,py=p/8,rx,ry,s,d=0,tx=px,ty=py;
         for(s=4;s>0;s>>=1){rx=(tx&s)?1:0;ry=(ty&s)?1:0;
             d+=s*s*((3*rx)^ry);
             if(!ry){if(rx){tx=s-1-tx;ty=s-1-ty;}uint32_t t=tx;tx=ty;ty=t;}}
         hmap[p]=(uint8_t)(d&0x3F);
     }
     for(int p=0;p<64;p++) G_H[hmap[p]]=(uint8_t)p;
     for(int p=0;p<64;p++) G_HI[G_H[p]]=p;
 }
 static inline void h_fwd(uint8_t o[64],const uint8_t s[64]){for(int i=0;i<64;i++)o[i]=s[G_H[i]];}
 static inline void h_inv(uint8_t o[64],const uint8_t s[64]){for(int i=0;i<64;i++)o[i]=s[G_HI[i]];}

 static uint8_t D4_FWD[8][64], D4_INV[8][64];
 static void init_d4(void){
     uint8_t s[64]; for(int i=0;i<64;i++) s[i]=(uint8_t)i;
     for(int i=0;i<64;i++) D4_FWD[0][i]=s[i];
     for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[1][x*8+(7-y)]=s[y*8+x];
     for(int i=0;i<64;i++) D4_FWD[2][63-i]=s[i];
     for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[3][(7-x)*8+y]=s[y*8+x];
     for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[4][y*8+(7-x)]=s[y*8+x];
     for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[5][(7-y)*8+x]=s[y*8+x];
     for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[6][x*8+y]=s[y*8+x];
     for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[7][(7-x)*8+(7-y)]=s[y*8+x];
     for(int st=0;st<8;st++) for(int i=0;i<64;i++) D4_INV[st][D4_FWD[st][i]]=(uint8_t)i;
 }
 static inline void d4_apply(uint8_t out[64],const uint8_t in[64],int st){for(int i=0;i<64;i++)out[i]=in[D4_FWD[st][i]];}
 static inline void d4_inv_apply(uint8_t out[64],const uint8_t in[64],int st){for(int i=0;i<64;i++)out[i]=in[D4_INV[st][i]];}

 static uint8_t d4_min_rid(const uint8_t s[64], uint8_t out[64]){
     uint64_t bh=UINT64_MAX; int bi=0;
     for(int i=0;i<8;i++){
         uint8_t tmp[64]; d4_apply(tmp,s,i);
         uint64_t h=1469598103934665603ULL;
         for(int j=0;j<64;j++){h^=tmp[j];h*=1099511628211ULL;}
         if(h<bh){bh=h;bi=i;memcpy(out,tmp,64);}
     }
     return (uint8_t)bi;
 }

int main(void){
    init_h(); init_d4();

    /* Create test data: 10 chunks with varied content */
    int N=1721;
    uint8_t *data=malloc(N*64);
    for(int i=0;i<N*64;i++) data[i]=(uint8_t)(i*17+i/64*13+42);

    /* Encode all chunks */
    typedef struct{uint64_t seed;uint8_t r[64];uint8_t flag;uint8_t rid;} Enc;
    typedef struct{uint64_t seed;uint8_t chunk[64];uint8_t rid;uint8_t used;} Slot;
    #define CAP (1<<16)
    Slot *idx=calloc(CAP,sizeof(Slot));
    Enc *enc=calloc(N,sizeof(Enc));
    #define MASK (CAP-1)

    for(int i=0;i<N;i++){
        uint8_t canon[64],hperm[64];
        uint8_t rid=d4_min_rid(data+i*64,canon);
        h_fwd(hperm,canon);
        uint64_t seed=1469598103934665603ULL;
        for(int j=0;j<64;j++){seed^=hperm[j];seed*=1099511628211ULL;}

        /* Check if seed already in index */
        int found=0;
        uint32_t slot=(uint32_t)(seed&MASK);
        for(int k=0;k<CAP;k++){
            uint32_t s=(slot+k)&MASK;
            if(!idx[s].used) break;
            if(idx[s].seed==seed){found=1;break;}
        }

        if(!found){
            uint32_t s=slot;
            for(int k=0;k<CAP;k++){
                s=(slot+k)&MASK;
                if(!idx[s].used){
                    idx[s].used=1; idx[s].seed=seed;
                    memcpy(idx[s].chunk,hperm,64);
                    idx[s].rid=rid;
                    break;
                }
            }
        }

        enc[i].seed=seed; enc[i].flag=0; enc[i].rid=rid;
    }

    /* Decode all chunks */
    uint8_t *dec=calloc(N,64);
    int ok=0;
    for(int i=0;i<N;i++){
        uint32_t slot=(uint32_t)(enc[i].seed&MASK);
        const Slot *sl=NULL;
        for(int k=0;k<CAP;k++){
            uint32_t s=(slot+k)&MASK;
            if(!idx[s].used) break;
            if(idx[s].seed==enc[i].seed){sl=&idx[s];break;}
        }
        if(!sl) continue;

        uint8_t hperm[64],canon[64],back[64];
        memcpy(hperm,sl->chunk,64);
        h_inv(canon,hperm);
        d4_inv_apply(back,canon,sl->rid);
        memcpy(dec+i*64,back,64);
        if(memcmp(data+i*64,dec+i*64,64)==0) ok++;
    }

    printf("N=%d  pass=%d/%d  ratio=%.1fx\n",N,ok,N,
           ok>0?(double)N*64/(double)(N*8):0.0);

    free(idx); free(enc); free(dec); free(data);
    return ok==N?0:1;
}
