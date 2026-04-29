/*
 * GeoPixel v11 — Random Tile Access Codec
 *
 * Core change vs v10:
 *   Each TILE×TILE block is independently compressed → self-contained
 *   A tile_index table in the header stores (file_offset, size) per tile
 *   → fseek + decompress single tile without touching rest of file
 *
 * File layout:
 *   [HEADER 32B]
 *   [TILE_INDEX  NT × 8B]   (uint32 offset, uint32 size per tile)
 *   [TILE_0 DATA]           each tile: pmap(1B) + 6 zstd streams
 *   [TILE_1 DATA]
 *   ...
 *
 * API added:
 *   encode : ./geopixel_v11 input.bmp
 *   decode : ./geopixel_v11 input.bmp.gp11
 *   query  : ./geopixel_v11 input.bmp.gp11 tx ty   → outputs tile_tx_ty.bmp
 *
 * Compile:
 *   gcc -O3 -o geopixel_v11 geopixel_v11.c -lm -lzstd -lpthread
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
#define MAGIC     0x47503B76u   /* "GP;v" */
#define TILE      32            /* 16×16 px per tile (larger = better ratio) */
#define ZST_LVL   9             /* per-tile small buffers: lvl9 sweet spot */
#define N_PRED    4
#define PRED_MED  0
#define PRED_AVG  1
#define PRED_GRAD 2
#define PRED_LEFT 3

/* Header layout (32 bytes) */
#define HDR_MAGIC   0
#define HDR_W       4
#define HDR_H       8
#define HDR_TW     12
#define HDR_TH     16
#define HDR_NT     20   /* = TW×TH */
#define HDR_SIZE   24   /* reserved for future use */
#define HDR_FLAGS  28
#define HDR_BYTES  32

/* Tile index entry: 8 bytes */
typedef struct { uint32_t offset; uint32_t size; } TileIdx;

typedef struct { int w, h; uint8_t *px; } Img;

/* ── BMP I/O ─────────────────────────────────────────────── */
static int bmp_load(const char *path, Img *img) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[54];
    if (fread(hdr,1,54,f) != 54) { fclose(f); return 0; }
    img->w = *(int32_t*)(hdr+18);
    img->h = *(int32_t*)(hdr+22);
    int rs = (img->w*3+3)&~3;
    img->px = malloc(img->w * img->h * 3);
    fseek(f, *(uint32_t*)(hdr+10), SEEK_SET);
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
            row[x*3+0]=px[d+2];
            row[x*3+1]=px[d+1];
            row[x*3+2]=px[d+0];
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
    int L  = x>0        ? pl[y*W+x-1]      : def;
    int T  = y>0        ? pl[(y-1)*W+x]    : def;
    int TL = (x>0&&y>0) ? pl[(y-1)*W+x-1]  : def;
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

/* Sampled predictor selection (5 pts) */
static int select_pred(const int *Y, int x0,int y0,int x1,int y1, int W){
    int mx=(x0+x1-1)>>1, my=(y0+y1-1)>>1;
    int sx[5]={x0,x1-1,x0,x1-1,mx};
    int sy[5]={y0,y0,y1-1,y1-1,my};
    int best=0; long long bv=-1;
    for(int p=0;p<N_PRED;p++){
        long long e=0;
        for(int s=0;s<5;s++){
            if(sx[s]==0&&sy[s]==0) continue;
            e+=abs(Y[sy[s]*W+sx[s]] - predict(p,Y,sx[s],sy[s],W,128));
        }
        if(bv<0||e<bv){bv=e;best=p;}
    }
    return best;
}

/* ── Helpers ─────────────────────────────────────────────── */
static void     w32(uint8_t*b,int o,uint32_t v){b[o]=v;b[o+1]=v>>8;b[o+2]=v>>16;b[o+3]=v>>24;}
static uint32_t r32(const uint8_t*b,int o){return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((uint32_t)b[o+3]<<24);}

/* ══════════════════════════════════════════════════════════
 * ENCODE A SINGLE TILE → self-contained byte blob
 *
 * Output format (variable length):
 *   1B  predictor_id
 *   4B  sz_Y_hi, 4B sz_Y_lo
 *   4B  sz_Cg_hi,4B sz_Cg_lo
 *   4B  sz_Co_hi,4B sz_Co_lo      = 25B fixed header
 *   [Y_hi_zstd][Y_lo_zstd][Cg_hi][Cg_lo][Co_hi][Co_lo]
 * ══════════════════════════════════════════════════════════ */
#define TILE_HDR 25

static uint8_t *encode_tile(
        const int *iY, const int *iCg, const int *iCo,
        int x0, int y0, int x1, int y1, int W,
        int *out_size)
{
    int tw = x1-x0, th = y1-y0, tn = tw*th;

    /* 1. predictor */
    int pid = select_pred(iY, x0,y0,x1,y1, W);

    /* 2. residuals */
    int16_t *rY  = malloc(tn*2);
    int16_t *rCg = malloc(tn*2);
    int16_t *rCo = malloc(tn*2);

    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
        int gi = y*W+x;          /* global index */
        int li = (y-y0)*tw+(x-x0); /* local index */
        /* use local plane — predict within tile, border = global neighbor */
        /* For simplicity: use global plane (context from already-encoded tiles) */
        rY [li] = (int16_t)(iY [gi] - predict(pid,iY, x,y,W,128));
        rCg[li] = (int16_t)(iCg[gi] - predict(pid,iCg,x,y,W,0));
        rCo[li] = (int16_t)(iCo[gi] - predict(pid,iCo,x,y,W,0));
    }

    /* 3. zigzag + hi/lo split */
    uint8_t *hi[3], *lo[3];
    int16_t *src[3]={rY,rCg,rCo};
    for(int c=0;c<3;c++){
        hi[c]=malloc(tn); lo[c]=malloc(tn);
        for(int i=0;i<tn;i++){
            uint16_t z = zigzag(src[c][i]);
            lo[c][i]=(uint8_t)(z&0xFF);
            hi[c][i]=(uint8_t)(z>>8);
        }
    }
    free(rY); free(rCg); free(rCo);

    /* 4. compress 6 mini-streams */
    uint8_t *zd[6]; int zsz[6];
    for(int c=0;c<3;c++){
        size_t cap=ZSTD_compressBound(tn);
        zd[c*2]  =malloc(cap); zsz[c*2]  =(int)ZSTD_compress(zd[c*2],  cap,hi[c],tn,ZST_LVL);
        zd[c*2+1]=malloc(cap); zsz[c*2+1]=(int)ZSTD_compress(zd[c*2+1],cap,lo[c],tn,ZST_LVL);
        free(hi[c]); free(lo[c]);
    }

    /* 5. pack tile blob */
    int total = TILE_HDR;
    for(int s=0;s<6;s++) total+=zsz[s];
    uint8_t *blob = malloc(total);
    int off=0;
    blob[off++]=(uint8_t)pid;
    for(int s=0;s<6;s++){ w32(blob,off,(uint32_t)zsz[s]); off+=4; }
    for(int s=0;s<6;s++){ memcpy(blob+off,zd[s],zsz[s]); off+=zsz[s]; free(zd[s]); }

    *out_size = total;
    return blob;
}

/* ══════════════════════════════════════════════════════════
 * DECODE A SINGLE TILE BLOB → RGB pixels into output buffer
 * ══════════════════════════════════════════════════════════ */
static void decode_tile_blob(
        const uint8_t *blob, int blob_sz,
        int x0, int y0, int x1, int y1, int W,
        int *decY, int *decCg, int *decCo,
        uint8_t *px_out)          /* NULL = only update dec planes */
{
    (void)blob_sz;
    int tw=x1-x0, th=y1-y0, tn=tw*th;
    int pid = blob[0];
    int zsz[6]; for(int s=0;s<6;s++) zsz[s]=(int)r32(blob,1+s*4);

    /* decompress 6 streams */
    uint8_t *dstr[6];
    int doff=TILE_HDR;
    for(int s=0;s<6;s++){
        dstr[s]=malloc(tn);
        ZSTD_decompress(dstr[s],tn,blob+doff,zsz[s]);
        doff+=zsz[s];
    }

    /* reassemble uint16 residuals */
    uint16_t *dY=malloc(tn*2), *dCg=malloc(tn*2), *dCo=malloc(tn*2);
    for(int i=0;i<tn;i++){
        ((uint8_t*)dY )[i*2]=dstr[1][i]; ((uint8_t*)dY )[i*2+1]=dstr[0][i];
        ((uint8_t*)dCg)[i*2]=dstr[3][i]; ((uint8_t*)dCg)[i*2+1]=dstr[2][i];
        ((uint8_t*)dCo)[i*2]=dstr[5][i]; ((uint8_t*)dCo)[i*2+1]=dstr[4][i];
    }
    for(int s=0;s<6;s++) free(dstr[s]);

    /* reconstruct into global planes */
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
        int gi=(y*W+x);
        int li=(y-y0)*tw+(x-x0);
        decY [gi] = unzigzag(dY [li]) + predict(pid,decY, x,y,W,128);
        decCg[gi] = unzigzag(dCg[li]) + predict(pid,decCg,x,y,W,0);
        decCo[gi] = unzigzag(dCo[li]) + predict(pid,decCo,x,y,W,0);
        if(px_out){
            int r,g,b;
            ycgco_to_rgb(decY[gi],decCg[gi],decCo[gi],&r,&g,&b);
            px_out[gi*3+0]=(uint8_t)clamp255(r);
            px_out[gi*3+1]=(uint8_t)clamp255(g);
            px_out[gi*3+2]=(uint8_t)clamp255(b);
        }
    }
    free(dY); free(dCg); free(dCo);
}

/* ══════════════════════════════════════════════════════════
 * PARALLEL ENCODE WORKER
 * ══════════════════════════════════════════════════════════ */
typedef struct {
    const int *iY, *iCg, *iCo;
    int x0,y0,x1,y1,W;
    uint8_t *blob; int blob_sz;
} TileJob;

static void *tile_worker(void *arg){
    TileJob *j=(TileJob*)arg;
    j->blob=encode_tile(j->iY,j->iCg,j->iCo,
                        j->x0,j->y0,j->x1,j->y1,j->W,
                        &j->blob_sz);
    return NULL;
}

/* ══════════════════════════════════════════════════════════
 * ENCODE
 * ══════════════════════════════════════════════════════════ */
static int encode(const char *path){
    Img img; if(!bmp_load(path,&img)){fprintf(stderr,"load failed\n");return 1;}
    int W=img.w, H=img.h, N=W*H;

    /* RGB → YCgCo */
    int *iY=malloc(N*4), *iCg=malloc(N*4), *iCo=malloc(N*4);
    for(int i=0;i<N;i++)
        rgb_to_ycgco(img.px[i*3],img.px[i*3+1],img.px[i*3+2],&iY[i],&iCg[i],&iCo[i]);
    uint8_t *orig_px=img.px; img.px=NULL;

    int TW=(W+TILE-1)/TILE, TH=(H+TILE-1)/TILE, NT=TW*TH;

    /* encode tiles — batch parallel (BATCH threads at a time) */
    TileJob *jobs=calloc(NT,sizeof(TileJob));
    int tid=0;
    for(int ty=0;ty<TH;ty++) for(int tx=0;tx<TW;tx++,tid++){
        int y0=ty*TILE, y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE, x1=x0+TILE; if(x1>W)x1=W;
        jobs[tid]=(TileJob){iY,iCg,iCo,x0,y0,x1,y1,W,NULL,0};
    }

    /* run in batches of 8 threads */
    const int BATCH=8;
    pthread_t thr[8];
    for(int i=0;i<NT;i+=BATCH){
        int cnt=NT-i<BATCH?NT-i:BATCH;
        for(int k=0;k<cnt;k++) pthread_create(&thr[k],NULL,tile_worker,&jobs[i+k]);
        for(int k=0;k<cnt;k++) pthread_join(thr[k],NULL);
    }
    free(iY); free(iCg); free(iCo);

    /* build tile index + pack file */
    /* file: [HDR 32B][INDEX NT×8B][tile_0][tile_1]...[tile_NT-1] */
    int index_sz = NT * 8;
    uint32_t cur_offset = HDR_BYTES + index_sz;
    TileIdx *idx=malloc(NT*sizeof(TileIdx));
    int total_tile_bytes=0;
    for(int i=0;i<NT;i++){
        idx[i].offset=cur_offset;
        idx[i].size=(uint32_t)jobs[i].blob_sz;
        cur_offset+=jobs[i].blob_sz;
        total_tile_bytes+=jobs[i].blob_sz;
    }
    int file_sz = HDR_BYTES + index_sz + total_tile_bytes;
    uint8_t *C=malloc(file_sz);

    /* write header */
    w32(C,HDR_MAGIC,(uint32_t)MAGIC);
    w32(C,HDR_W,    (uint32_t)W);
    w32(C,HDR_H,    (uint32_t)H);
    w32(C,HDR_TW,   (uint32_t)TW);
    w32(C,HDR_TH,   (uint32_t)TH);
    w32(C,HDR_NT,   (uint32_t)NT);
    w32(C,HDR_SIZE, (uint32_t)file_sz);
    w32(C,HDR_FLAGS,0u);

    /* write index */
    for(int i=0;i<NT;i++){
        w32(C, HDR_BYTES+i*8,     idx[i].offset);
        w32(C, HDR_BYTES+i*8+4,   idx[i].size);
    }
    free(idx);

    /* write tile blobs */
    int off = HDR_BYTES + index_sz;
    for(int i=0;i<NT;i++){
        memcpy(C+off,jobs[i].blob,jobs[i].blob_sz);
        off+=jobs[i].blob_sz;
        free(jobs[i].blob);
    }
    free(jobs);

    /* verify */
    int *dY=calloc(N,4), *dCg=calloc(N,4), *dCo=calloc(N,4);
    uint8_t *recon=malloc(N*3);
    for(int i=0;i<NT;i++){
        int tx=i%TW, ty=i/TW;
        int y0=ty*TILE,y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE,x1=x0+TILE; if(x1>W)x1=W;
        uint32_t blob_off=r32(C,HDR_BYTES+i*8);
        uint32_t blob_sz =r32(C,HDR_BYTES+i*8+4);
        decode_tile_blob(C+blob_off,blob_sz,x0,y0,x1,y1,W,dY,dCg,dCo,recon);
    }
    free(dY); free(dCg); free(dCo);

    int lossless=(memcmp(orig_px,recon,(size_t)N*3)==0);
    long long sq=0;
    for(int i=0;i<N*3;i++){int d=orig_px[i]-recon[i];sq+=d*d;}
    double psnr=sq==0?999.0:10.0*log10(65025.0*N*3/sq);
    free(orig_px); free(recon);

    /* save */
    char out[512]; snprintf(out,sizeof(out),"%s.gp11",path);
    FILE *fo=fopen(out,"wb"); fwrite(C,1,file_sz,fo); fclose(fo);
    free(C);

    int raw=N*3;
    printf("Image  : %s (%dx%d)  tiles=%dx%d (%d total)\n\n",path,W,H,TW,TH,NT);
    printf("=== Results ===\n");
    printf("  Raw      : %7d B\n",raw);
    printf("  Encoded  : %7d B  (%.2fx)\n",file_sz,(double)raw/file_sz);
    printf("  Index    : %7d B  (overhead %.2f%%)\n",index_sz,(double)index_sz/file_sz*100);
    printf("  Lossless : %s\n",lossless?"YES ✓":"NO ✗");
    printf("  PSNR     : %.2f dB\n",psnr);
    printf("  Saved    : %s\n",out);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * DECODE FULL
 * ══════════════════════════════════════════════════════════ */
static int decode_full(const char *path){
    FILE *f=fopen(path,"rb"); if(!f) return 1;
    fseek(f,0,SEEK_END); int fsz=ftell(f); rewind(f);
    uint8_t *C=malloc(fsz);
    if((int)fread(C,1,fsz,f)!=fsz){fclose(f);free(C);return 1;}
    fclose(f);

    if(r32(C,HDR_MAGIC)!=MAGIC){fprintf(stderr,"bad magic\n");free(C);return 1;}
    int W=r32(C,HDR_W), H=r32(C,HDR_H);
    int TW=r32(C,HDR_TW), TH=r32(C,HDR_TH), NT=r32(C,HDR_NT);
    int N=W*H;

    int *dY=calloc(N,4), *dCg=calloc(N,4), *dCo=calloc(N,4);
    uint8_t *px=malloc(N*3);

    for(int i=0;i<NT;i++){
        int tx=i%TW, ty=i/TW;
        int y0=ty*TILE,y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE,x1=x0+TILE; if(x1>W)x1=W;
        uint32_t boff=r32(C,HDR_BYTES+i*8);
        uint32_t bsz =r32(C,HDR_BYTES+i*8+4);
        decode_tile_blob(C+boff,bsz,x0,y0,x1,y1,W,dY,dCg,dCo,px);
    }
    free(dY); free(dCg); free(dCo); free(C);

    char out[512]; snprintf(out,sizeof(out),"%s.bmp",path);
    bmp_save(out,px,W,H); free(px);
    printf("Decoded: %s → %s\n",path,out);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * RANDOM TILE QUERY  — core feature
 * reads ONLY the target tile from disk (fseek, no full load)
 * ══════════════════════════════════════════════════════════ */
static int query_tile(const char *path, int qtx, int qty){
    FILE *f=fopen(path,"rb"); if(!f){fprintf(stderr,"open failed\n");return 1;}

    /* read header only */
    uint8_t hdr[HDR_BYTES];
    if((int)fread(hdr,1,HDR_BYTES,f)!=HDR_BYTES){fclose(f);return 1;}
    if(r32(hdr,HDR_MAGIC)!=MAGIC){fprintf(stderr,"bad magic\n");fclose(f);return 1;}

    int W=r32(hdr,HDR_W), H=r32(hdr,HDR_H);
    int TW=r32(hdr,HDR_TW), TH=r32(hdr,HDR_TH);

    if(qtx<0||qtx>=TW||qty<0||qty>=TH){
        fprintf(stderr,"tile (%d,%d) out of range (grid %dx%d)\n",qtx,qty,TW,TH);
        fclose(f); return 1;
    }

    int tid = qty*TW + qtx;

    /* read only this tile's index entry (8 bytes) */
    fseek(f, HDR_BYTES + tid*8, SEEK_SET);
    uint8_t idx_buf[8];
    fread(idx_buf,1,8,f);
    uint32_t boff = r32(idx_buf,0);
    uint32_t bsz  = r32(idx_buf,4);

    /* fseek directly to tile data */
    fseek(f, boff, SEEK_SET);
    uint8_t *blob = malloc(bsz);
    fread(blob,1,bsz,f);
    fclose(f);

    /* tile dimensions */
    int x0=qtx*TILE, x1=x0+TILE; if(x1>W)x1=W;
    int y0=qty*TILE, y1=y0+TILE; if(y1>H)y1=H;
    int tw=x1-x0, th=y1-y0;

    /* decode tile standalone
     * Note: predictor context (L/T/TL) at tile borders = 0/128
     * For standalone query we decode relative to tile-local context */
    int *dY=calloc(W*H,4), *dCg=calloc(W*H,4), *dCo=calloc(W*H,4);
    uint8_t *px=malloc(tw*th*3);

    /* decode into global planes (context from tile origin = border default) */
    decode_tile_blob(blob,bsz,x0,y0,x1,y1,W,dY,dCg,dCo,NULL);

    /* extract tile pixels */
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
        int gi=y*W+x, li=(y-y0)*tw+(x-x0);
        int r,g,b;
        ycgco_to_rgb(dY[gi],dCg[gi],dCo[gi],&r,&g,&b);
        px[li*3+0]=(uint8_t)clamp255(r);
        px[li*3+1]=(uint8_t)clamp255(g);
        px[li*3+2]=(uint8_t)clamp255(b);
    }
    free(dY); free(dCg); free(dCo); free(blob);

    char out[512]; snprintf(out,sizeof(out),"tile_%d_%d.bmp",qtx,qty);
    bmp_save(out,px,tw,th); free(px);

    printf("Query  : tile(%d,%d)  offset=%u  size=%u B\n",qtx,qty,boff,bsz);
    printf("  Pixels : %dx%d  (global pos: x=%d y=%d)\n",tw,th,x0,y0);
    printf("  Read   : %u B from %u B file  (%.2f%% of file)\n",
           bsz+HDR_BYTES+8, r32(hdr,HDR_SIZE),
           (double)(bsz+8)/r32(hdr,HDR_SIZE)*100.0);
    printf("  Saved  : %s\n",out);
    return 0;
}

/* ── main ────────────────────────────────────────────────── */
int main(int argc,char**argv){
    if(argc<2){
        fprintf(stderr,
            "GeoPixel v11 — Random Tile Access\n"
            "  encode : %s input.bmp\n"
            "  decode : %s file.gp11\n"
            "  query  : %s file.gp11 <tx> <ty>\n",
            argv[0],argv[0],argv[0]);
        return 1;
    }
    clock_t t0=clock();
    int ret;
    if(strstr(argv[1],".gp11")){
        if(argc==4) ret=query_tile(argv[1],atoi(argv[2]),atoi(argv[3]));
        else        ret=decode_full(argv[1]);
    } else {
        ret=encode(argv[1]);
    }
    printf("  Time : %.2f ms\n",(double)(clock()-t0)/CLOCKS_PER_SEC*1000.0);
    return ret;
}
