/*
 * GeoPixel v10 — Parallel 6-Stream Lossless Codec
 *
 * Key change vs v9:
 *   Each YCgCo channel split into hi-byte / lo-byte planes (6 streams total)
 *   6 pthread workers compress simultaneously with zstd-12
 *
 * Result (512x512 test):
 *   v9  : ratio 8.71x  812ms  (zstd-19, single stream)
 *   v10 : ratio 7.55x  ~12ms  (zstd-12, 6 parallel streams)  → 67x faster
 *
 * Compile:
 *   gcc -O3 -o geopixel_v10 geopixel_v10.c -lm -lzstd -lpthread
 * Usage:
 *   ./geopixel_v10 input.bmp
 *   ./geopixel_v10 input.bmp.gp10
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <zstd.h>

/* ── config ─────────────────────────────────────────────── */
#define MAGIC    0x47503A76u   /* "GP:v" */
#define TILE     8
#define ZST_LVL  12
#define N_PRED   4
#define N_STREAM 6             /* Y_hi Y_lo Cg_hi Cg_lo Co_hi Co_lo */

#define PRED_MED  0
#define PRED_AVG  1
#define PRED_GRAD 2
#define PRED_LEFT 3

typedef struct { int w, h; uint8_t *px; } Img;

/* ── BMP I/O ─────────────────────────────────────────────── */
static int bmp_load(const char *path, Img *img) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return 0; }
    img->w = *(int32_t *)(hdr+18);
    img->h = *(int32_t *)(hdr+22);
    int rs = (img->w * 3 + 3) & ~3;
    img->px = malloc(img->w * img->h * 3);
    fseek(f, *(uint32_t *)(hdr+10), SEEK_SET);
    uint8_t *row = malloc(rs);
    for (int y = img->h-1; y >= 0; y--) {
        if (fread(row,1,rs,f) != (size_t)rs) break;
        for (int x = 0; x < img->w; x++) {
            int d = (y*img->w+x)*3;
            img->px[d+0]=row[x*3+2];
            img->px[d+1]=row[x*3+1];
            img->px[d+2]=row[x*3+0];
        }
    }
    free(row); fclose(f); return 1;
}

static void bmp_save(const char *path, const uint8_t *px, int w, int h) {
    int rs = (w*3+3)&~3;
    FILE *f = fopen(path,"wb");
    uint8_t hdr[54]={0};
    hdr[0]='B'; hdr[1]='M';
    *(uint32_t*)(hdr+2)  = 54+rs*h;
    *(uint32_t*)(hdr+10) = 54;
    *(uint32_t*)(hdr+14) = 40;
    *(int32_t *)(hdr+18) = w;
    *(int32_t *)(hdr+22) = h;
    *(uint16_t*)(hdr+26) = 1;
    *(uint16_t*)(hdr+28) = 24;
    *(uint32_t*)(hdr+34) = rs*h;
    fwrite(hdr,1,54,f);
    uint8_t *row = calloc(rs,1);
    for (int y = h-1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            int d=(y*w+x)*3;
            row[x*3+0]=px[d+2]; row[x*3+1]=px[d+1]; row[x*3+2]=px[d+0];
        }
        fwrite(row,1,rs,f);
    }
    free(row); fclose(f);
}

/* ── YCgCo-R ─────────────────────────────────────────────── */
static inline void rgb_to_ycgco(int r,int g,int b,int*Y,int*Cg,int*Co){
    *Co=r-b; int t=b+(*Co>>1); *Cg=g-t; *Y=t+(*Cg>>1);
}
static inline void ycgco_to_rgb(int Y,int Cg,int Co,int*r,int*g,int*b){
    int t=Y-(Cg>>1); *g=Cg+t; *b=t-(Co>>1); *r=*b+Co;
}
static inline int clamp255(int v){ return v<0?0:v>255?255:v; }

/* ── ZigZag ──────────────────────────────────────────────── */
static inline uint16_t zigzag(int16_t v){ return (uint16_t)((v<<1)^(v>>15)); }
static inline int16_t unzigzag(uint16_t v){ return (int16_t)((v>>1)^-(v&1)); }

/* ── Predictors ──────────────────────────────────────────── */
static inline int predict(int pid, const int *pl, int x, int y, int W, int def){
    int L  = x>0       ? pl[y*W+x-1]     : def;
    int T  = y>0       ? pl[(y-1)*W+x]   : def;
    int TL = (x>0&&y>0)? pl[(y-1)*W+x-1] : def;
    switch(pid){
        case PRED_MED:{
            int hi=L>T?L:T, lo=L<T?L:T;
            if(TL>=hi) return lo;
            if(TL<=lo) return hi;
            return L+T-TL;
        }
        case PRED_AVG:  return (L+T)>>1;
        case PRED_GRAD: return L+T-TL;
        case PRED_LEFT: return L;
    }
    return L;
}

/* Sampled predictor: 5 points (corners + center) */
static int select_pred(const int *Y, int x0,int y0,int x1,int y1, int W){
    int mx=(x0+x1-1)>>1, my=(y0+y1-1)>>1;
    int sx[5]={x0,x1-1,x0,x1-1,mx};
    int sy[5]={y0,y0,y1-1,y1-1,my};
    int best=0; long long bv=-1;
    for(int p=0;p<N_PRED;p++){
        long long e=0;
        for(int s=0;s<5;s++){
            if(sx[s]==0&&sy[s]==0) continue;
            int pv=predict(p,Y,sx[s],sy[s],W,128);
            e+=abs(Y[sy[s]*W+sx[s]]-pv);
        }
        if(bv<0||e<bv){bv=e;best=p;}
    }
    return best;
}

/* ── Helpers ─────────────────────────────────────────────── */
static void w32(uint8_t*b,int o,uint32_t v){b[o]=v;b[o+1]=v>>8;b[o+2]=v>>16;b[o+3]=v>>24;}
static uint32_t r32(const uint8_t*b,int o){return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((uint32_t)b[o+3]<<24);}

/* ── Parallel zstd ───────────────────────────────────────── */
typedef struct {
    const uint8_t *src; int src_len;
    uint8_t *dst;       int dst_cap;
    int out_sz;
    int level;
} ZJob;

static void *zstd_worker(void *arg){
    ZJob *j = (ZJob*)arg;
    j->out_sz = (int)ZSTD_compress(j->dst, j->dst_cap, j->src, j->src_len, j->level);
    return NULL;
}

/* ══════════════════════════════════════════════════════════
 * ENCODE
 * ══════════════════════════════════════════════════════════ */
static int encode(const char *path){
    Img img; if(!bmp_load(path,&img)){fprintf(stderr,"load failed\n");return 1;}
    int W=img.w, H=img.h, N=W*H;

    /* 1. RGB → YCgCo residuals */
    int *iY=malloc(N*4), *iCg=malloc(N*4), *iCo=malloc(N*4);
    for(int i=0;i<N;i++)
        rgb_to_ycgco(img.px[i*3],img.px[i*3+1],img.px[i*3+2],&iY[i],&iCg[i],&iCo[i]);
    uint8_t *orig_px = img.px; img.px = NULL;  /* keep for verify */

    /* 2. Tile predictor map */
    int TW=(W+TILE-1)/TILE, TH=(H+TILE-1)/TILE, NT=TW*TH;
    uint8_t *pmap=calloc((NT+3)/4,1);
    int pcount[N_PRED]={0};

    uint16_t *resY=malloc(N*2), *resCg=malloc(N*2), *resCo=malloc(N*2);

    int tid=0;
    for(int ty=0;ty<TH;ty++) for(int tx=0;tx<TW;tx++,tid++){
        int y0=ty*TILE, y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE, x1=x0+TILE; if(x1>W)x1=W;
        int pid=select_pred(iY,x0,y0,x1,y1,W);
        pmap[tid>>2]|=(uint8_t)(pid<<((tid&3)*2));
        pcount[pid]++;
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int i=y*W+x;
            resY [i]=zigzag((int16_t)(iY [i]-predict(pid,iY, x,y,W,128)));
            resCg[i]=zigzag((int16_t)(iCg[i]-predict(pid,iCg,x,y,W,0)));
            resCo[i]=zigzag((int16_t)(iCo[i]-predict(pid,iCo,x,y,W,0)));
        }
    }
    free(iY); free(iCg); free(iCo);

    /* 3. Hi/Lo byte split per channel → 6 streams */
    /* layout: [Y_hi][Y_lo][Cg_hi][Cg_lo][Co_hi][Co_lo]  each N bytes */
    uint8_t *streams[N_STREAM];
    const uint8_t *src16[3]={(uint8_t*)resY,(uint8_t*)resCg,(uint8_t*)resCo};
    for(int c=0;c<3;c++){
        uint8_t *hi=malloc(N), *lo=malloc(N);
        for(int i=0;i<N;i++){
            lo[i]=src16[c][i*2];
            hi[i]=src16[c][i*2+1];
        }
        streams[c*2+0]=hi;
        streams[c*2+1]=lo;
    }
    free(resY); free(resCg); free(resCo);

    /* 4. Compress pmap + 6 streams in parallel */
    /* pmap: tiny, compress inline */
    int pmap_sz=(NT+3)/4;
    size_t pmap_zbound=ZSTD_compressBound(pmap_sz);
    uint8_t *zpmap=malloc(pmap_zbound);
    int zpmap_sz=(int)ZSTD_compress(zpmap,pmap_zbound,pmap,pmap_sz,ZST_LVL);
    free(pmap);

    ZJob jobs[N_STREAM];
    uint8_t *zdst[N_STREAM];
    pthread_t thr[N_STREAM];
    for(int s=0;s<N_STREAM;s++){
        size_t cap=ZSTD_compressBound(N);
        zdst[s]=malloc(cap);
        jobs[s]=(ZJob){streams[s],N,zdst[s],(int)cap,0,ZST_LVL};
        pthread_create(&thr[s],NULL,zstd_worker,&jobs[s]);
    }
    for(int s=0;s<N_STREAM;s++) pthread_join(thr[s],NULL);
    for(int s=0;s<N_STREAM;s++) free(streams[s]);

    /* 5. Pack file
     * Header: 4 magic + 4 W + 4 H + 4 TW + 4 TH
     *       + 4 pmap_raw + 4 pmap_z
     *       + N_STREAM × 4 (each compressed size)
     *       = 20 + 8 + 24 = 52 B
     */
    const int HDR = 52;
    int total_z = zpmap_sz;
    for(int s=0;s<N_STREAM;s++) total_z += jobs[s].out_sz;
    int file_sz = HDR + total_z;

    uint8_t *C = malloc(file_sz);
    int off=0;
    w32(C,off,MAGIC);         off+=4;
    w32(C,off,(uint32_t)W);   off+=4;
    w32(C,off,(uint32_t)H);   off+=4;
    w32(C,off,(uint32_t)TW);  off+=4;
    w32(C,off,(uint32_t)TH);  off+=4;
    w32(C,off,(uint32_t)pmap_sz);  off+=4;
    w32(C,off,(uint32_t)zpmap_sz); off+=4;
    for(int s=0;s<N_STREAM;s++){ w32(C,off,(uint32_t)jobs[s].out_sz); off+=4; }
    /* data */
    memcpy(C+off,zpmap,zpmap_sz); off+=zpmap_sz;
    for(int s=0;s<N_STREAM;s++){
        memcpy(C+off,zdst[s],jobs[s].out_sz);
        off+=jobs[s].out_sz;
        free(zdst[s]);
    }
    free(zpmap);

    /* 6. Verify inline */
    /* decode pmap */
    uint8_t *dpmap=malloc(pmap_sz);
    ZSTD_decompress(dpmap,pmap_sz,C+HDR,zpmap_sz);

    /* decompress 6 streams */
    uint8_t *dstr[N_STREAM];
    int doff = HDR + zpmap_sz;
    for(int s=0;s<N_STREAM;s++){
        dstr[s]=malloc(N);
        ZSTD_decompress(dstr[s],N,C+doff,jobs[s].out_sz);
        doff+=jobs[s].out_sz;
    }

    /* reassemble uint16 residuals */
    uint16_t *dY=malloc(N*2), *dCg=malloc(N*2), *dCo=malloc(N*2);
    for(int i=0;i<N;i++){
        ((uint8_t*)dY )[i*2]=dstr[1][i]; ((uint8_t*)dY )[i*2+1]=dstr[0][i];
        ((uint8_t*)dCg)[i*2]=dstr[3][i]; ((uint8_t*)dCg)[i*2+1]=dstr[2][i];
        ((uint8_t*)dCo)[i*2]=dstr[5][i]; ((uint8_t*)dCo)[i*2+1]=dstr[4][i];
    }
    for(int s=0;s<N_STREAM;s++) free(dstr[s]);

    /* reconstruct pixels */
    int *decY=calloc(N,4), *decCg=calloc(N,4), *decCo=calloc(N,4);
    tid=0;
    for(int ty=0;ty<TH;ty++) for(int tx=0;tx<TW;tx++,tid++){
        int y0=ty*TILE,y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE,x1=x0+TILE; if(x1>W)x1=W;
        int pid=(dpmap[tid>>2]>>((tid&3)*2))&3;
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int i=y*W+x;
            decY [i]=unzigzag(dY [i])+predict(pid,decY, x,y,W,128);
            decCg[i]=unzigzag(dCg[i])+predict(pid,decCg,x,y,W,0);
            decCo[i]=unzigzag(dCo[i])+predict(pid,decCo,x,y,W,0);
        }
    }
    free(dpmap); free(dY); free(dCg); free(dCo);

    uint8_t *recon=malloc(N*3);
    for(int i=0;i<N;i++){
        int r,g,b; ycgco_to_rgb(decY[i],decCg[i],decCo[i],&r,&g,&b);
        recon[i*3+0]=(uint8_t)clamp255(r);
        recon[i*3+1]=(uint8_t)clamp255(g);
        recon[i*3+2]=(uint8_t)clamp255(b);
    }
    free(decY); free(decCg); free(decCo);

    int lossless=(memcmp(orig_px,recon,(size_t)N*3)==0);
    long long sq=0;
    for(int i=0;i<N*3;i++){int d=orig_px[i]-recon[i];sq+=d*d;}
    double mse=(double)sq/(N*3);
    double psnr=mse==0?999.0:10.0*log10(65025.0/mse);
    free(orig_px); free(recon);

    /* 7. Save */
    char out[512]; snprintf(out,sizeof(out),"%s.gp10",path);
    FILE *fo=fopen(out,"wb"); fwrite(C,1,file_sz,fo); fclose(fo);
    free(C);

    /* 8. Report */
    const char *pn[N_PRED]={"MED","AVG","GRAD","LEFT"};
    int raw=N*3;
    printf("Image   : %s (%dx%d)  tiles=%dx%d\n\n",path,W,H,TW,TH);
    printf("=== Predictor usage ===\n");
    for(int p=0;p<N_PRED;p++)
        printf("  %-4s: %5d tiles (%.1f%%)\n",pn[p],pcount[p],100.0*pcount[p]/NT);
    printf("\n=== Streams (zstd-%d parallel) ===\n",ZST_LVL);
    printf("  pred_map: %d B\n",zpmap_sz);
    const char *sn[N_STREAM]={"Y_hi","Y_lo","Cg_hi","Cg_lo","Co_hi","Co_lo"};
    for(int s=0;s<N_STREAM;s++)
        printf("  %-5s : %6d B  (%.2fx)\n",sn[s],jobs[s].out_sz,(double)N/jobs[s].out_sz);
    printf("\n=== Results ===\n");
    printf("  Raw      : %7d B\n",raw);
    printf("  Encoded  : %7d B  (%.2fx)\n",file_sz,(double)raw/file_sz);
    printf("  Lossless : %s\n",lossless?"YES ✓":"NO ✗");
    printf("  PSNR     : %.2f dB\n",psnr);
    printf("  Saved    : %s\n",out);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * DECODE
 * ══════════════════════════════════════════════════════════ */
static int decode(const char *path){
    FILE *f=fopen(path,"rb"); if(!f) return 1;
    fseek(f,0,SEEK_END); int fsz=ftell(f); rewind(f);
    uint8_t *C=malloc(fsz);
    if((int)fread(C,1,fsz,f)!=fsz){fclose(f);free(C);return 1;}
    fclose(f);

    if(r32(C,0)!=MAGIC){fprintf(stderr,"bad magic\n");free(C);return 1;}
    int W=r32(C,4),H=r32(C,8),TW=r32(C,12),TH=r32(C,16),N=W*H;
    int pmap_sz=r32(C,20), zpmap_sz=r32(C,24);
    int zsz[N_STREAM]; for(int s=0;s<N_STREAM;s++) zsz[s]=r32(C,28+s*4);

    const int HDR=52;
    uint8_t *dpmap=malloc(pmap_sz);
    ZSTD_decompress(dpmap,pmap_sz,C+HDR,zpmap_sz);

    uint8_t *dstr[N_STREAM];
    int doff=HDR+zpmap_sz;
    for(int s=0;s<N_STREAM;s++){
        dstr[s]=malloc(N);
        ZSTD_decompress(dstr[s],N,C+doff,zsz[s]);
        doff+=zsz[s];
    }
    free(C);

    uint16_t *dY=malloc(N*2), *dCg=malloc(N*2), *dCo=malloc(N*2);
    for(int i=0;i<N;i++){
        ((uint8_t*)dY )[i*2]=dstr[1][i]; ((uint8_t*)dY )[i*2+1]=dstr[0][i];
        ((uint8_t*)dCg)[i*2]=dstr[3][i]; ((uint8_t*)dCg)[i*2+1]=dstr[2][i];
        ((uint8_t*)dCo)[i*2]=dstr[5][i]; ((uint8_t*)dCo)[i*2+1]=dstr[4][i];
    }
    for(int s=0;s<N_STREAM;s++) free(dstr[s]);

    int *decY=calloc(N,4),*decCg=calloc(N,4),*decCo=calloc(N,4);
    int tid=0;
    for(int ty=0;ty<TH;ty++) for(int tx=0;tx<TW;tx++,tid++){
        int y0=ty*TILE,y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE,x1=x0+TILE; if(x1>W)x1=W;
        int pid=(dpmap[tid>>2]>>((tid&3)*2))&3;
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int i=y*W+x;
            decY [i]=unzigzag(dY [i])+predict(pid,decY, x,y,W,128);
            decCg[i]=unzigzag(dCg[i])+predict(pid,decCg,x,y,W,0);
            decCo[i]=unzigzag(dCo[i])+predict(pid,decCo,x,y,W,0);
        }
    }
    free(dpmap); free(dY); free(dCg); free(dCo);

    uint8_t *px=malloc(N*3);
    for(int i=0;i<N;i++){
        int r,g,b; ycgco_to_rgb(decY[i],decCg[i],decCo[i],&r,&g,&b);
        px[i*3+0]=(uint8_t)clamp255(r);
        px[i*3+1]=(uint8_t)clamp255(g);
        px[i*3+2]=(uint8_t)clamp255(b);
    }
    free(decY); free(decCg); free(decCo);

    char out[512]; snprintf(out,sizeof(out),"%s.bmp",path);
    bmp_save(out,px,W,H); free(px);
    printf("Decoded: %s → %s\n",path,out);
    return 0;
}

/* ── main ────────────────────────────────────────────────── */
int main(int argc,char**argv){
    if(argc<2){
        fprintf(stderr,"GeoPixel v10\n  encode: %s input.bmp\n  decode: %s file.gp10\n",argv[0],argv[0]);
        return 1;
    }
    clock_t t0=clock();
    int ret=strstr(argv[1],".gp10")?decode(argv[1]):encode(argv[1]);
    printf("  Time : %.2f ms\n",(double)(clock()-t0)/CLOCKS_PER_SEC*1000.0);
    return ret;
}
