/*
 * GeoPixel v13 — Adaptive Boundary Compression + Per-tile Mean Bias
 *
 * vs v12:
 *   1. FLAT     → zero boundary (inferred from defaults)
 *   2. GRADIENT → corner(1px) + linear vectors (dx,dy) = ~30B vs 384B
 *   3. EDGE     → full boundary but delta-encoded + zigzag (no raw int16)
 *   4. NOISE    → full boundary delta-encoded
 *   5. Per-tile mean bias: subtract mean Y/Cg/Co before predict → smaller residuals
 *
 * Boundary budget per tile:
 *   FLAT:     0B
 *   GRADIENT: 6B corner + 12B vectors = 18B (vs 384B)
 *   EDGE:     6B corner + delta(left+top) zigzag ≈ 60-100B (vs 384B)
 *   NOISE:    same as EDGE
 *
 * Compile: gcc -O3 -o geopixel_v13 geopixel_v13.c -lm -lzstd -lpthread
 * Usage  : ./geopixel_v13 input.bmp
 *          ./geopixel_v13 file.gp13
 *          ./geopixel_v13 file.gp13 tx ty
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
#define MAGIC_V13   0x47503D76u
#define TILE        32
#define ZST_LVL     9
#define MAX_FLAT_COLORS 4

/* tile types */
#define TTYPE_FLAT     0
#define TTYPE_GRADIENT 1
#define TTYPE_EDGE     2
#define TTYPE_NOISE    3

/* boundary modes (stored in blob, drives decode) */
#define BMODE_NONE    0   /* FLAT: use defaults */
#define BMODE_LINEAR  1   /* GRADIENT: corner + dx/dy vectors */
#define BMODE_DELTA   2   /* EDGE/NOISE: delta-coded full boundary */

/* predictors */
#define PRED_MED  0
#define PRED_GRAD 1
#define PRED_LEFT 2
#define N_PRED    3

/* header */
#define HDR_MAGIC  0
#define HDR_W      4
#define HDR_H      8
#define HDR_TW    12
#define HDR_TH    16
#define HDR_NT    20
#define HDR_FSIZE 24
#define HDR_FLAGS 28
#define HDR_BYTES 32

typedef struct { uint32_t offset; uint32_t size; } TileIdx;
typedef struct { int w, h; uint8_t *px; } Img;

/* ── BMP I/O ──────────────────────────────────────────────*/
static int bmp_load(const char *path, Img *img) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint8_t hdr[54];
    if (fread(hdr,1,54,f)!=54){fclose(f);return 0;}
    img->w=*(int32_t*)(hdr+18); img->h=*(int32_t*)(hdr+22);
    int rs=(img->w*3+3)&~3;
    img->px=malloc(img->w*img->h*3);
    fseek(f,*(uint32_t*)(hdr+10),SEEK_SET);
    uint8_t *row=malloc(rs);
    for(int y=img->h-1;y>=0;y--){
        if(fread(row,1,rs,f)!=(size_t)rs) break;
        for(int x=0;x<img->w;x++){
            int d=(y*img->w+x)*3;
            img->px[d+0]=row[x*3+2];
            img->px[d+1]=row[x*3+1];
            img->px[d+2]=row[x*3+0];
        }
    }
    free(row); fclose(f); return 1;
}

static void bmp_save(const char *path, const uint8_t *px, int w, int h){
    int rs=(w*3+3)&~3;
    FILE *f=fopen(path,"wb");
    uint8_t hdr[54]={0};
    hdr[0]='B';hdr[1]='M';
    *(uint32_t*)(hdr+2)=54+rs*h; *(uint32_t*)(hdr+10)=54;
    *(uint32_t*)(hdr+14)=40;
    *(int32_t*)(hdr+18)=w; *(int32_t*)(hdr+22)=h;
    *(uint16_t*)(hdr+26)=1; *(uint16_t*)(hdr+28)=24;
    *(uint32_t*)(hdr+34)=rs*h;
    fwrite(hdr,1,54,f);
    uint8_t *row=calloc(rs,1);
    for(int y=h-1;y>=0;y--){
        for(int x=0;x<w;x++){
            int d=(y*w+x)*3;
            row[x*3+0]=px[d+2];row[x*3+1]=px[d+1];row[x*3+2]=px[d+0];
        }
        fwrite(row,1,rs,f);
    }
    free(row); fclose(f);
}

/* ── YCgCo-R ──────────────────────────────────────────────*/
static inline void rgb_to_ycgco(int r,int g,int b,int*Y,int*Cg,int*Co){
    *Co=r-b; int t=b+(*Co>>1); *Cg=g-t; *Y=t+(*Cg>>1);
}
static inline void ycgco_to_rgb(int Y,int Cg,int Co,int*r,int*g,int*b){
    int t=Y-(Cg>>1); *g=Cg+t; *b=t-(Co>>1); *r=*b+Co;
}
static inline int clamp255(int v){return v<0?0:v>255?255:v;}

/* ── ZigZag ───────────────────────────────────────────────*/
static inline uint16_t zigzag(int16_t v){return (uint16_t)((v<<1)^(v>>15));}
static inline int16_t unzigzag(uint16_t v){return (int16_t)((v>>1)^-(v&1));}

/* ── byte I/O helpers ─────────────────────────────────────*/
static void     w32(uint8_t*b,int o,uint32_t v){b[o]=v;b[o+1]=v>>8;b[o+2]=v>>16;b[o+3]=v>>24;}
static uint32_t r32(const uint8_t*b,int o){return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((uint32_t)b[o+3]<<24);}
static void     w16(uint8_t*b,int o,uint16_t v){b[o]=v&0xFF;b[o+1]=v>>8;}
static uint16_t r16(const uint8_t*b,int o){return(uint16_t)(b[o]|(b[o+1]<<8));}

/* ── Dynamic buffer ───────────────────────────────────────*/
typedef struct{uint8_t*buf;int sz,cap;}Buf;
static Buf newbuf(int c){Buf b={malloc(c),0,c};return b;}
static void bp(Buf*b,uint8_t v){
    if(b->sz==b->cap){b->cap*=2;b->buf=realloc(b->buf,b->cap);}
    b->buf[b->sz++]=v;
}
static void bpz(Buf*b,int16_t v){uint16_t z=zigzag(v);bp(b,z&0xFF);bp(b,z>>8);}

/* ════════════════════════════════════════════════════════
 * BOUNDARY CODEC
 * Three modes driven by tile type.
 * ════════════════════════════════════════════════════════ */

/*
 * Linear boundary: reconstruct left/top from corner + slope.
 * Encoder stores: corner_val(int16) + slope_left(int16) + slope_top(int16)
 * = 6B per channel × 3 channels = 18B total.
 * Decoder: left[y] = corner + y*slope_left/TILE
 *          top[x]  = corner + x*slope_top/TILE
 */
static void encode_boundary_linear(
        const int *col, const int *row, int corner,  /* col[th], row[tw] */
        int th, int tw, Buf *out)
{
    /* slope = (last - first) in Q8 fixed point, stored as int16 */
    int sl = th>1 ? ((col[th-1]-col[0])<<8)/(th-1) : 0;
    int st = tw>1 ? ((row[tw-1]-row[0])<<8)/(tw-1) : 0;
    bpz(out,(int16_t)corner);
    bpz(out,(int16_t)sl);
    bpz(out,(int16_t)st);
}

static void decode_boundary_linear(
        const uint8_t *blob, int *off,
        int *col_out, int *row_out, int *corner_out,
        int th, int tw, int def)
{
    int corner = (int)unzigzag(r16(blob,*off)); (*off)+=2;
    int sl     = (int)unzigzag(r16(blob,*off)); (*off)+=2;
    int st     = (int)unzigzag(r16(blob,*off)); (*off)+=2;
    *corner_out = corner;
    for(int y=0;y<th;y++) col_out[y] = corner + (y*sl>>8);
    for(int x=0;x<tw;x++) row_out[x] = corner + (x*st>>8);
    (void)def;
}

/*
 * Delta boundary: corner + delta-coded sequence.
 * left[0] = corner, left[y] = left[y-1] + delta[y-1]
 * top [0] = corner, top [x] = top [x-1] + delta[x-1]
 * Deltas stored as zigzag uint8 (clamped to -127..127).
 * corner = int16 zigzag (2B), then (th-1 + tw) delta bytes = ~63B for TILE=32.
 */
static void encode_boundary_delta(
        const int *col, const int *row, int corner,
        int th, int tw, Buf *out)
{
    bpz(out,(int16_t)corner);
    /* left col deltas: start from corner */
    int prev = corner;
    for(int y=0;y<th;y++){
        int d=col[y]-prev; prev=col[y];
        /* clamp and store as signed byte zigzag */
        if(d<-127)d=-127; if(d>127)d=127;
        bp(out,(uint8_t)zigzag((int16_t)d));
    }
    /* top row deltas: start from corner */
    prev = corner;
    for(int x=0;x<tw;x++){
        int d=row[x]-prev; prev=row[x];
        if(d<-127)d=-127; if(d>127)d=127;
        bp(out,(uint8_t)zigzag((int16_t)d));
    }
}

static void decode_boundary_delta(
        const uint8_t *blob, int *off,
        int *col_out, int *row_out, int *corner_out,
        int th, int tw)
{
    int corner=(int)unzigzag(r16(blob,*off)); (*off)+=2;
    *corner_out = corner;
    int prev=corner;
    for(int y=0;y<th;y++){
        int d=(int)unzigzag((uint16_t)blob[(*off)++]);
        prev+=d; col_out[y]=prev;
    }
    prev=corner;
    for(int x=0;x<tw;x++){
        int d=(int)unzigzag((uint16_t)blob[(*off)++]);
        prev+=d; row_out[x]=prev;
    }
}

/* ── LOCAL predictor ──────────────────────────────────────*/
static inline int predict_local(int pid,
        const int *tl, int x, int y, int tw,
        const int *ctx_left, const int *ctx_top, int ctx_tl, int def)
{
    int L  = x>0 ? tl[y*tw+x-1]   : (ctx_left?ctx_left[y]:def);
    int T  = y>0 ? tl[(y-1)*tw+x] : (ctx_top ?ctx_top[x] :def);
    int TL;
    if     (x>0&&y>0) TL=tl[(y-1)*tw+x-1];
    else if(x==0&&y>0) TL=ctx_left?ctx_left[y-1]:def;
    else if(x>0&&y==0) TL=ctx_top ?ctx_top[x-1] :def;
    else               TL=ctx_tl;

    switch(pid){
        case PRED_MED:{
            int hi=L>T?L:T, lo=L<T?L:T;
            if(TL>=hi) return lo;
            if(TL<=lo) return hi;
            return L+T-TL;
        }
        case PRED_GRAD: return L+T-TL;
        case PRED_LEFT: return L;
    }
    return L;
}

/* ── Tile classification ──────────────────────────────────*/
static int classify_tile(
        const int *iY,const int *iCg,const int *iCo,
        int x0,int y0,int x1,int y1,int W,int *out_pred)
{
    int tw=x1-x0, th=y1-y0, tn=tw*th;

    /* variance per predictor */
    long long var[N_PRED]={0};
    for(int pid=0;pid<N_PRED;pid++){
        int *loc=malloc(tn*4);
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int li=(y-y0)*tw+(x-x0);
            int L=x>x0?iY[y*W+x-1]:(x>0?iY[y*W+x-1]:128);
            int T=y>y0?iY[(y-1)*W+x]:(y>0?iY[(y-1)*W+x]:128);
            int TL=(x>x0&&y>y0)?iY[(y-1)*W+x-1]:(x>0&&y>0?iY[(y-1)*W+x-1]:128);
            int p;
            switch(pid){
                case PRED_MED:{int hi=L>T?L:T,lo=L<T?L:T;p=TL>=hi?lo:TL<=lo?hi:L+T-TL;break;}
                case PRED_GRAD: p=L+T-TL; break;
                default:        p=L;
            }
            loc[li]=iY[y*W+x]-p;
            var[pid]+=(long long)loc[li]*loc[li];
        }
        free(loc);
    }
    int best=0; for(int p=1;p<N_PRED;p++) if(var[p]<var[best]) best=p;
    *out_pred=best;
    double avg_var=(double)var[best]/tn;

    /* flat check: unique YCgCo combos */
    int palY[MAX_FLAT_COLORS],palCg[MAX_FLAT_COLORS],palCo[MAX_FLAT_COLORS];
    int n_pal=0; int is_flat=1;
    for(int y=y0;y<y1&&is_flat;y++) for(int x=x0;x<x1&&is_flat;x++){
        int gi=y*W+x;
        int fy=iY[gi],fcg=iCg[gi],fco=iCo[gi];
        int found=0;
        for(int k=0;k<n_pal;k++)
            if(palY[k]==fy&&palCg[k]==fcg&&palCo[k]==fco){found=1;break;}
        if(!found){
            if(n_pal>=MAX_FLAT_COLORS){is_flat=0;break;}
            palY[n_pal]=fy;palCg[n_pal]=fcg;palCo[n_pal]=fco;n_pal++;
        }
    }
    if(is_flat)           return TTYPE_FLAT;
    if(avg_var<16.0)      return TTYPE_GRADIENT;
    if(avg_var<200.0)     return TTYPE_EDGE;
    return TTYPE_NOISE;
}

/* ════════════════════════════════════════════════════════
 * ENCODE TILE
 * ════════════════════════════════════════════════════════ */
static uint8_t *encode_tile(
        const int *iY,const int *iCg,const int *iCo,
        int x0,int y0,int x1,int y1,int W,int H,
        int *out_size)
{
    int tw=x1-x0, th=y1-y0, tn=tw*th;
    int pid,ttype;
    ttype=classify_tile(iY,iCg,iCo,x0,y0,x1,y1,W,&pid);

    /* ── FLAT ── */
    if(ttype==TTYPE_FLAT){
        int palY[MAX_FLAT_COLORS],palCg[MAX_FLAT_COLORS],palCo[MAX_FLAT_COLORS];
        int n_pal=0;
        uint8_t *idx_map=malloc(tn);
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int gi=y*W+x, li=(y-y0)*tw+(x-x0);
            int fy=iY[gi],fcg=iCg[gi],fco=iCo[gi];
            int found=-1;
            for(int k=0;k<n_pal;k++)
                if(palY[k]==fy&&palCg[k]==fcg&&palCo[k]==fco){found=k;break;}
            if(found<0){palY[n_pal]=fy;palCg[n_pal]=fcg;palCo[n_pal]=fco;found=n_pal++;}
            idx_map[li]=(uint8_t)found;
        }
        uint8_t *rle=malloc(tn*2+4); int rle_sz=0,ri=0;
        while(ri<tn){
            uint8_t cur=idx_map[ri]; int run=1;
            while(ri+run<tn&&idx_map[ri+run]==cur&&run<255) run++;
            rle[rle_sz++]=(uint8_t)run; rle[rle_sz++]=cur; ri+=run;
        }
        free(idx_map);
        int total=2+n_pal*6+rle_sz;
        uint8_t *blob=malloc(total); int off=0;
        blob[off++]=(uint8_t)TTYPE_FLAT;
        blob[off++]=(uint8_t)n_pal;
        for(int k=0;k<n_pal;k++){
            w16(blob,off,zigzag((int16_t)palY[k]));  off+=2;
            w16(blob,off,zigzag((int16_t)palCg[k])); off+=2;
            w16(blob,off,zigzag((int16_t)palCo[k])); off+=2;
        }
        memcpy(blob+off,rle,rle_sz);
        free(rle);
        *out_size=total; return blob;
    }

    /* ── Predictive (GRADIENT / EDGE / NOISE) ── */
    int def_Y=128, def_C=0;

    /* boundary mode by type */
    int bmode = (ttype==TTYPE_GRADIENT) ? BMODE_LINEAR : BMODE_DELTA;

    /* collect raw boundary values */
    int *col_Y =malloc(th*4),*col_Cg=malloc(th*4),*col_Co=malloc(th*4);
    int *row_Y =malloc(tw*4),*row_Cg=malloc(tw*4),*row_Co=malloc(tw*4);
    int tl_Y,tl_Cg,tl_Co;

    tl_Y  =(x0>0&&y0>0)?iY [(y0-1)*W+x0-1]:def_Y;
    tl_Cg =(x0>0&&y0>0)?iCg[(y0-1)*W+x0-1]:def_C;
    tl_Co =(x0>0&&y0>0)?iCo[(y0-1)*W+x0-1]:def_C;

    for(int y=0;y<th;y++){
        col_Y [y]=x0>0?iY [(y0+y)*W+x0-1]:def_Y;
        col_Cg[y]=x0>0?iCg[(y0+y)*W+x0-1]:def_C;
        col_Co[y]=x0>0?iCo[(y0+y)*W+x0-1]:def_C;
    }
    for(int x=0;x<tw;x++){
        row_Y [x]=y0>0?iY [(y0-1)*W+x0+x]:def_Y;
        row_Cg[x]=y0>0?iCg[(y0-1)*W+x0+x]:def_C;
        row_Co[x]=y0>0?iCo[(y0-1)*W+x0+x]:def_C;
    }

    /* ── per-tile mean bias ── */
    long sumY=0,sumCg=0,sumCo=0;
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
        int gi=y*W+x; sumY+=iY[gi]; sumCg+=iCg[gi]; sumCo+=iCo[gi];
    }
    int meanY =(int)(sumY /tn);
    int meanCg=(int)(sumCg/tn);
    int meanCo=(int)(sumCo/tn);

    /* adjust boundary by mean (decoder will add mean back) */
    for(int y=0;y<th;y++){col_Y[y]-=meanY; col_Cg[y]-=meanCg; col_Co[y]-=meanCo;}
    for(int x=0;x<tw;x++){row_Y[x]-=meanY; row_Cg[x]-=meanCg; row_Co[x]-=meanCo;}
    int adj_tl_Y=tl_Y-meanY, adj_tl_Cg=tl_Cg-meanCg, adj_tl_Co=tl_Co-meanCo;

    /* encode boundary into temp buf */
    Buf bctx=newbuf(256);
    if(bmode==BMODE_LINEAR){
        encode_boundary_linear(col_Y, row_Y, adj_tl_Y, th,tw,&bctx);
        encode_boundary_linear(col_Cg,row_Cg,adj_tl_Cg,th,tw,&bctx);
        encode_boundary_linear(col_Co,row_Co,adj_tl_Co,th,tw,&bctx);
    } else {
        encode_boundary_delta(col_Y, row_Y, adj_tl_Y, th,tw,&bctx);
        encode_boundary_delta(col_Cg,row_Cg,adj_tl_Cg,th,tw,&bctx);
        encode_boundary_delta(col_Co,row_Co,adj_tl_Co,th,tw,&bctx);
    }
    free(col_Y);free(col_Cg);free(col_Co);
    free(row_Y);free(row_Cg);free(row_Co);

    /* rebuild int* context for predictor (mean-biased) */
    int *ctxLY=malloc(th*4),*ctxLCg=malloc(th*4),*ctxLCo=malloc(th*4);
    int *ctxTY=malloc(tw*4),*ctxTCg=malloc(tw*4),*ctxTCo=malloc(tw*4);
    int dummy_tl_Y,dummy_tl_Cg,dummy_tl_Co;

    /* decode our own boundary to get exact reconstruction context */
    int tmp_off=0;
    if(bmode==BMODE_LINEAR){
        decode_boundary_linear(bctx.buf,&tmp_off,ctxLY, ctxTY, &dummy_tl_Y, th,tw,def_Y-meanY);
        decode_boundary_linear(bctx.buf,&tmp_off,ctxLCg,ctxTCg,&dummy_tl_Cg,th,tw,def_C-meanCg);
        decode_boundary_linear(bctx.buf,&tmp_off,ctxLCo,ctxTCo,&dummy_tl_Co,th,tw,def_C-meanCo);
    } else {
        decode_boundary_delta(bctx.buf,&tmp_off,ctxLY, ctxTY, &dummy_tl_Y, th,tw);
        decode_boundary_delta(bctx.buf,&tmp_off,ctxLCg,ctxTCg,&dummy_tl_Cg,th,tw);
        decode_boundary_delta(bctx.buf,&tmp_off,ctxLCo,ctxTCo,&dummy_tl_Co,th,tw);
    }
    /* tlY used by predictor is the decoded corner */
    int ptl_Y=dummy_tl_Y, ptl_Cg=dummy_tl_Cg, ptl_Co=dummy_tl_Co;

    /* residuals using mean-biased local planes */
    int *locY =malloc(tn*4),*locCg=malloc(tn*4),*locCo=malloc(tn*4);
    int16_t *rY=malloc(tn*2),*rCg=malloc(tn*2),*rCo=malloc(tn*2);

    for(int y=0;y<th;y++) for(int x=0;x<tw;x++){
        int li=y*tw+x, gi=(y0+y)*W+(x0+x);
        /* mean-bias the actual pixel values */
        int bY =iY [gi]-meanY;
        int bCg=iCg[gi]-meanCg;
        int bCo=iCo[gi]-meanCo;
        int pY =predict_local(pid,locY, x,y,tw,ctxLY, ctxTY, ptl_Y, def_Y-meanY);
        int pCg=predict_local(pid,locCg,x,y,tw,ctxLCg,ctxTCg,ptl_Cg,def_C-meanCg);
        int pCo=predict_local(pid,locCo,x,y,tw,ctxLCo,ctxTCo,ptl_Co,def_C-meanCo);
        locY [li]=bY;  locCg[li]=bCg;  locCo[li]=bCo;
        rY [li]=(int16_t)(bY -pY);
        rCg[li]=(int16_t)(bCg-pCg);
        rCo[li]=(int16_t)(bCo-pCo);
    }
    free(locY);free(locCg);free(locCo);
    free(ctxLY);free(ctxLCg);free(ctxLCo);
    free(ctxTY);free(ctxTCg);free(ctxTCo);

    /* zigzag hi/lo split */
    uint8_t *hi[3],*lo[3]; int16_t *src3[3]={rY,rCg,rCo};
    for(int c=0;c<3;c++){
        hi[c]=malloc(tn); lo[c]=malloc(tn);
        for(int i=0;i<tn;i++){
            uint16_t z=zigzag(src3[c][i]);
            lo[c][i]=(uint8_t)(z&0xFF); hi[c][i]=(uint8_t)(z>>8);
        }
    }
    free(rY);free(rCg);free(rCo);

    /* compress 6 streams */
    uint8_t *zd[6]; int zsz[6];
    for(int c=0;c<3;c++){
        size_t cap=ZSTD_compressBound(tn);
        zd[c*2]  =malloc(cap); zsz[c*2]  =(int)ZSTD_compress(zd[c*2],  cap,hi[c],tn,ZST_LVL);
        zd[c*2+1]=malloc(cap); zsz[c*2+1]=(int)ZSTD_compress(zd[c*2+1],cap,lo[c],tn,ZST_LVL);
        free(hi[c]);free(lo[c]);
    }

    /*
     * Blob layout:
     *   1B ttype | 1B bmode | 1B pid
     *   6B mean  [Y,Cg,Co as int16 zigzag]
     *   ctx bytes (bctx)
     *   6×4B stream sizes
     *   6 zstd streams
     */
    int total = 3 + 6 + bctx.sz + 6*4;
    for(int s=0;s<6;s++) total+=zsz[s];
    uint8_t *blob=malloc(total); int off=0;

    blob[off++]=(uint8_t)ttype;
    blob[off++]=(uint8_t)bmode;
    blob[off++]=(uint8_t)pid;
    w16(blob,off,zigzag((int16_t)meanY));  off+=2;
    w16(blob,off,zigzag((int16_t)meanCg)); off+=2;
    w16(blob,off,zigzag((int16_t)meanCo)); off+=2;
    memcpy(blob+off,bctx.buf,bctx.sz); off+=bctx.sz; free(bctx.buf);
    for(int s=0;s<6;s++){w32(blob,off,(uint32_t)zsz[s]);off+=4;}
    for(int s=0;s<6;s++){memcpy(blob+off,zd[s],zsz[s]);off+=zsz[s];free(zd[s]);}

    *out_size=total; return blob;
}

/* ════════════════════════════════════════════════════════
 * DECODE TILE BLOB
 * ════════════════════════════════════════════════════════ */
static void decode_tile_blob(
        const uint8_t *blob,int blob_sz,
        int x0,int y0,int x1,int y1,int W,
        uint8_t *px_out)
{
    (void)blob_sz;
    int tw=x1-x0,th=y1-y0,tn=tw*th;
    int off=0;
    int ttype=blob[off++];

    /* ── FLAT ── */
    if(ttype==TTYPE_FLAT){
        int n_pal=blob[off++];
        int palY[MAX_FLAT_COLORS],palCg[MAX_FLAT_COLORS],palCo[MAX_FLAT_COLORS];
        for(int k=0;k<n_pal;k++){
            palY [k]=(int)unzigzag(r16(blob,off)); off+=2;
            palCg[k]=(int)unzigzag(r16(blob,off)); off+=2;
            palCo[k]=(int)unzigzag(r16(blob,off)); off+=2;
        }
        int li=0;
        while(li<tn&&off<blob_sz-1){
            int run=blob[off++]; int idx=blob[off++];
            if(idx>=n_pal) idx=0;
            for(int k=0;k<run&&li<tn;k++,li++){
                int lx=li%tw,ly=li/tw,gi=(y0+ly)*W+(x0+lx);
                int r,g,b;
                ycgco_to_rgb(palY[idx],palCg[idx],palCo[idx],&r,&g,&b);
                px_out[gi*3+0]=(uint8_t)clamp255(r);
                px_out[gi*3+1]=(uint8_t)clamp255(g);
                px_out[gi*3+2]=(uint8_t)clamp255(b);
            }
        }
        return;
    }

    /* ── Predictive ── */
    int bmode=blob[off++];
    int pid  =blob[off++];
    int def_Y=128,def_C=0;

    int meanY =(int)unzigzag(r16(blob,off)); off+=2;
    int meanCg=(int)unzigzag(r16(blob,off)); off+=2;
    int meanCo=(int)unzigzag(r16(blob,off)); off+=2;

    int *ctxLY =malloc(th*4),*ctxLCg=malloc(th*4),*ctxLCo=malloc(th*4);
    int *ctxTY =malloc(tw*4),*ctxTCg=malloc(tw*4),*ctxTCo=malloc(tw*4);
    int tl_Y,tl_Cg,tl_Co;

    if(bmode==BMODE_LINEAR){
        decode_boundary_linear(blob,&off,ctxLY, ctxTY, &tl_Y, th,tw,def_Y-meanY);
        decode_boundary_linear(blob,&off,ctxLCg,ctxTCg,&tl_Cg,th,tw,def_C-meanCg);
        decode_boundary_linear(blob,&off,ctxLCo,ctxTCo,&tl_Co,th,tw,def_C-meanCo);
    } else {
        decode_boundary_delta(blob,&off,ctxLY, ctxTY, &tl_Y, th,tw);
        decode_boundary_delta(blob,&off,ctxLCg,ctxTCg,&tl_Cg,th,tw);
        decode_boundary_delta(blob,&off,ctxLCo,ctxTCo,&tl_Co,th,tw);
    }

    int zsz[6]; for(int s=0;s<6;s++){zsz[s]=(int)r32(blob,off);off+=4;}
    uint8_t *dstr[6];
    for(int s=0;s<6;s++){
        dstr[s]=malloc(tn);
        ZSTD_decompress(dstr[s],tn,blob+off,zsz[s]); off+=zsz[s];
    }

    int *locY =malloc(tn*4),*locCg=malloc(tn*4),*locCo=malloc(tn*4);
    for(int y=0;y<th;y++) for(int x=0;x<tw;x++){
        int li=y*tw+x, gi=(y0+y)*W+(x0+x);
        uint16_t zy =(uint16_t)(dstr[1][li]|(dstr[0][li]<<8));
        uint16_t zcg=(uint16_t)(dstr[3][li]|(dstr[2][li]<<8));
        uint16_t zco=(uint16_t)(dstr[5][li]|(dstr[4][li]<<8));
        int pY =predict_local(pid,locY, x,y,tw,ctxLY, ctxTY, tl_Y, def_Y-meanY);
        int pCg=predict_local(pid,locCg,x,y,tw,ctxLCg,ctxTCg,tl_Cg,def_C-meanCg);
        int pCo=predict_local(pid,locCo,x,y,tw,ctxLCo,ctxTCo,tl_Co,def_C-meanCo);
        locY [li]=(int)unzigzag(zy) +pY;
        locCg[li]=(int)unzigzag(zcg)+pCg;
        locCo[li]=(int)unzigzag(zco)+pCo;
        /* add mean back */
        int r,g,b;
        ycgco_to_rgb(locY[li]+meanY,locCg[li]+meanCg,locCo[li]+meanCo,&r,&g,&b);
        px_out[gi*3+0]=(uint8_t)clamp255(r);
        px_out[gi*3+1]=(uint8_t)clamp255(g);
        px_out[gi*3+2]=(uint8_t)clamp255(b);
    }
    for(int s=0;s<6;s++) free(dstr[s]);
    free(locY);free(locCg);free(locCo);
    free(ctxLY);free(ctxLCg);free(ctxLCo);
    free(ctxTY);free(ctxTCg);free(ctxTCo);
}

/* ════════════════════════════════════════════════════════
 * PARALLEL ENCODE WORKER
 * ════════════════════════════════════════════════════════ */
typedef struct{
    const int *iY,*iCg,*iCo;
    int x0,y0,x1,y1,W,H;
    uint8_t *blob; int blob_sz,ttype;
} TileJob;

static void *tile_worker(void *arg){
    TileJob *j=(TileJob*)arg;
    int pid;
    j->ttype=classify_tile(j->iY,j->iCg,j->iCo,j->x0,j->y0,j->x1,j->y1,j->W,&pid);
    j->blob=encode_tile(j->iY,j->iCg,j->iCo,j->x0,j->y0,j->x1,j->y1,j->W,j->H,&j->blob_sz);
    return NULL;
}

/* ════════════════════════════════════════════════════════
 * ENCODE
 * ════════════════════════════════════════════════════════ */
static int encode(const char *path){
    Img img; if(!bmp_load(path,&img)){fprintf(stderr,"load failed\n");return 1;}
    int W=img.w,H=img.h,N=W*H;
    int *iY=malloc(N*4),*iCg=malloc(N*4),*iCo=malloc(N*4);
    for(int i=0;i<N;i++)
        rgb_to_ycgco(img.px[i*3],img.px[i*3+1],img.px[i*3+2],&iY[i],&iCg[i],&iCo[i]);

    int TW=(W+TILE-1)/TILE,TH=(H+TILE-1)/TILE,NT=TW*TH;
    TileJob *jobs=calloc(NT,sizeof(TileJob));
    int tid=0;
    for(int ty=0;ty<TH;ty++) for(int tx=0;tx<TW;tx++,tid++){
        int y0=ty*TILE,y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE,x1=x0+TILE; if(x1>W)x1=W;
        jobs[tid]=(TileJob){iY,iCg,iCo,x0,y0,x1,y1,W,H,NULL,0,0};
    }
    const int BATCH=8; pthread_t thr[8];
    for(int i=0;i<NT;i+=BATCH){
        int cnt=NT-i<BATCH?NT-i:BATCH;
        for(int k=0;k<cnt;k++) pthread_create(&thr[k],NULL,tile_worker,&jobs[i+k]);
        for(int k=0;k<cnt;k++) pthread_join(thr[k],NULL);
    }
    free(iY);free(iCg);free(iCo);

    int index_sz=NT*8;
    uint32_t cur_off=HDR_BYTES+index_sz;
    TileIdx *idx=malloc(NT*sizeof(TileIdx));
    int total_tile=0;
    for(int i=0;i<NT;i++){
        idx[i].offset=cur_off; idx[i].size=(uint32_t)jobs[i].blob_sz;
        cur_off+=jobs[i].blob_sz; total_tile+=jobs[i].blob_sz;
    }
    int file_sz=HDR_BYTES+index_sz+total_tile;
    uint8_t *C=malloc(file_sz);
    w32(C,HDR_MAGIC,(uint32_t)MAGIC_V13);
    w32(C,HDR_W,(uint32_t)W); w32(C,HDR_H,(uint32_t)H);
    w32(C,HDR_TW,(uint32_t)TW); w32(C,HDR_TH,(uint32_t)TH);
    w32(C,HDR_NT,(uint32_t)NT);
    w32(C,HDR_FSIZE,(uint32_t)file_sz);
    w32(C,HDR_FLAGS,0u);
    for(int i=0;i<NT;i++){w32(C,HDR_BYTES+i*8,idx[i].offset);w32(C,HDR_BYTES+i*8+4,idx[i].size);}
    free(idx);
    int off2=HDR_BYTES+index_sz;
    for(int i=0;i<NT;i++){memcpy(C+off2,jobs[i].blob,jobs[i].blob_sz);off2+=jobs[i].blob_sz;free(jobs[i].blob);}

    /* verify */
    uint8_t *recon=malloc(N*3);
    for(int i=0;i<NT;i++){
        int tx=i%TW,ty=i/TW;
        int y0=ty*TILE,y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE,x1=x0+TILE; if(x1>W)x1=W;
        decode_tile_blob(C+r32(C,HDR_BYTES+i*8),r32(C,HDR_BYTES+i*8+4),x0,y0,x1,y1,W,recon);
    }
    int lossless=(memcmp(img.px,recon,(size_t)N*3)==0);
    long long sq=0; for(int i=0;i<N*3;i++){int d=img.px[i]-recon[i];sq+=d*d;}
    double psnr=sq==0?999.0:10.0*log10(65025.0*N*3/sq);
    free(img.px);free(recon);

    int tc[4]={0};
    for(int i=0;i<NT;i++) tc[jobs[i].ttype]++;
    free(jobs);

    char out[512]; snprintf(out,sizeof(out),"%s.gp13",path);
    FILE *fo=fopen(out,"wb"); fwrite(C,1,file_sz,fo); fclose(fo); free(C);

    int raw=N*3;
    long long bnd_flat=0, bnd_grad=0, bnd_other=0;
    printf("Image  : %s (%dx%d)  tiles=%dx%d (%d)\n\n",path,W,H,TW,TH,NT);
    printf("=== Tile types ===\n");
    printf("  flat=%d  gradient=%d  edge=%d  noise=%d\n",tc[0],tc[1],tc[2],tc[3]);
    printf("  boundary: FLAT=0B  GRADIENT≈18B  EDGE/NOISE≈%dB\n", 3*3*(2+TH+TW));
    printf("\n=== Results ===\n");
    printf("  Raw      : %7d B\n",raw);
    printf("  Encoded  : %7d B  (%.2fx)\n",file_sz,(double)raw/file_sz);
    printf("  Index    : %7d B  (%.2f%%)\n",index_sz,(double)index_sz/file_sz*100.0);
    printf("  Lossless : %s\n",lossless?"YES ✓":"NO ✗");
    printf("  PSNR     : %.2f dB\n",psnr);
    printf("  Saved    : %s\n",out);
    (void)bnd_flat;(void)bnd_grad;(void)bnd_other;
    return 0;

    // clang warning suppression
    int th=TILE; int unused_th = th; (void)unused_th;
}

/* ════════════════════════════════════════════════════════
 * DECODE FULL
 * ════════════════════════════════════════════════════════ */
static int decode_full(const char *path){
    FILE *f=fopen(path,"rb"); if(!f) return 1;
    fseek(f,0,SEEK_END); int fsz=ftell(f); rewind(f);
    uint8_t *C=malloc(fsz);
    if((int)fread(C,1,fsz,f)!=fsz){fclose(f);free(C);return 1;} fclose(f);
    if(r32(C,HDR_MAGIC)!=MAGIC_V13){fprintf(stderr,"bad magic\n");free(C);return 1;}
    int W=r32(C,HDR_W),H=r32(C,HDR_H);
    int TW=r32(C,HDR_TW),TH=r32(C,HDR_TH),NT=r32(C,HDR_NT);
    uint8_t *px=malloc(W*H*3);
    for(int i=0;i<NT;i++){
        int tx=i%TW,ty=i/TW;
        int y0=ty*TILE,y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE,x1=x0+TILE; if(x1>W)x1=W;
        decode_tile_blob(C+r32(C,HDR_BYTES+i*8),r32(C,HDR_BYTES+i*8+4),x0,y0,x1,y1,W,px);
    }
    free(C);
    char out[512]; snprintf(out,sizeof(out),"%s.bmp",path);
    bmp_save(out,px,W,H); free(px);
    printf("Decoded: %s → %s\n",path,out); return 0;
}

/* ════════════════════════════════════════════════════════
 * RANDOM TILE QUERY
 * ════════════════════════════════════════════════════════ */
static int query_tile(const char *path,int qtx,int qty){
    FILE *f=fopen(path,"rb"); if(!f) return 1;
    uint8_t hdr[HDR_BYTES];
    if((int)fread(hdr,1,HDR_BYTES,f)!=HDR_BYTES){fclose(f);return 1;}
    if(r32(hdr,HDR_MAGIC)!=MAGIC_V13){fprintf(stderr,"bad magic\n");fclose(f);return 1;}
    int W=r32(hdr,HDR_W),H=r32(hdr,HDR_H);
    int TW=r32(hdr,HDR_TW),TH=r32(hdr,HDR_TH),fsz=r32(hdr,HDR_FSIZE);
    if(qtx<0||qtx>=TW||qty<0||qty>=TH){fprintf(stderr,"tile out of range\n");fclose(f);return 1;}
    int tid=qty*TW+qtx;
    fseek(f,HDR_BYTES+tid*8,SEEK_SET);
    uint8_t idx_buf[8]; fread(idx_buf,1,8,f);
    uint32_t boff=r32(idx_buf,0),bsz=r32(idx_buf,4);
    fseek(f,boff,SEEK_SET);
    uint8_t *blob=malloc(bsz); fread(blob,1,bsz,f); fclose(f);
    int x0=qtx*TILE,x1=x0+TILE; if(x1>W)x1=W;
    int y0=qty*TILE,y1=y0+TILE; if(y1>H)y1=H;
    int tw=x1-x0,th=y1-y0;

    /* tile type name */
    const char *tnames[]={"FLAT","GRADIENT","EDGE","NOISE"};
    int tt=blob[0]<4?blob[0]:3;
    int bmode=tt==TTYPE_FLAT?BMODE_NONE:(int)blob[1];
    const char *bmnames[]={"NONE","LINEAR","DELTA"};

    uint8_t *px=calloc(W*H*3,1);
    decode_tile_blob(blob,bsz,x0,y0,x1,y1,W,px); free(blob);

    uint8_t *tile_px=malloc(tw*th*3);
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
        int gi=y*W+x,li=(y-y0)*tw+(x-x0);
        tile_px[li*3+0]=px[gi*3+0]; tile_px[li*3+1]=px[gi*3+1]; tile_px[li*3+2]=px[gi*3+2];
    }
    free(px);
    char out[512]; snprintf(out,sizeof(out),"tile_%d_%d.bmp",qtx,qty);
    bmp_save(out,tile_px,tw,th); free(tile_px);
    int bread=(int)(bsz+HDR_BYTES+8);
    printf("Query  : tile(%d,%d)  type=%s  boundary=%s\n",qtx,qty,tnames[tt],bmnames[bmode]);
    printf("  offset=%u  size=%u B\n",boff,bsz);
    printf("  Read   : %d B from %d B file  (%.3f%%)\n",bread,fsz,(double)bread/fsz*100.0);
    printf("  Saved  : %s\n",out);
    return 0;
}

/* ── main ─────────────────────────────────────────────────*/
int main(int argc,char**argv){
    if(argc<2){
        fprintf(stderr,
            "GeoPixel v13\n"
            "  encode : %s input.bmp\n"
            "  decode : %s file.gp13\n"
            "  query  : %s file.gp13 tx ty\n",
            argv[0],argv[0],argv[0]);
        return 1;
    }
    clock_t t0=clock();
    int ret;
    if(strstr(argv[1],".gp13")){
        ret=(argc==4)?query_tile(argv[1],atoi(argv[2]),atoi(argv[3])):decode_full(argv[1]);
    } else {
        ret=encode(argv[1]);
    }
    printf("  Time   : %.2f ms\n",(double)(clock()-t0)/CLOCKS_PER_SEC*1000.0);
    return ret;
}
