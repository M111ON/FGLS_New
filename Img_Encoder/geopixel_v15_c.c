/*
 * GeoPixel v15 — Boundary-driven mode selection + 2-point gradient model
 *
 * vs v14: removes PARAM (EDGE→DELTA proved worse), redesigns boundary codec
 *
 * Key changes:
 *   1. Tile type no longer drives bmode directly — boundary variance does
 *      classify_boundary(): measure left-col/top-row variance + smoothness
 *      smooth  → BMODE_LINEAR2 (9B)   was 18B
 *      complex → BMODE_DELTA  (~63B)
 *   2. BMODE_LINEAR2: 2-point model per channel (Y0 + dx + dy = 3B × 3ch = 9B)
 *      vs v13/v14 LINEAR: corner + slope_left + slope_top = 6B × 3ch = 18B
 *   3. EDGE tiles use boundary classify (not forced PARAM/DELTA)
 *
 * Boundary budget per tile:
 *   FLAT:    0B
 *   smooth:  9B  (LINEAR2)
 *   complex: ~63B (DELTA)
 *
 * Expected: ratio 3.3–3.5x vs v14 3.26x
 *
 * Compile: gcc -O3 -o geopixel_v15 geopixel_v15.c -lm -lzstd -lpthread
 * Usage  : ./geopixel_v15 input.bmp | file.gp15 | file.gp15 tx ty
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
#define MAGIC_V13   0x47503D78u  /* v15 magic */
#define TILE        32
#define ZST_LVL     9
#define MAX_FLAT_COLORS 4

/* tile types */
#define TTYPE_FLAT     0
#define TTYPE_GRADIENT 1
#define TTYPE_EDGE     2
#define TTYPE_NOISE    3

/* boundary modes */
#define BMODE_NONE    0   /* FLAT: no boundary stored */
#define BMODE_LINEAR2 1   /* smooth boundary: Y0+dx+dy per ch = 9B */
#define BMODE_DELTA   2   /* complex boundary: delta-coded = ~63B */
#define BMODE_GRAD9   3   /* gradient tile: zigzag Y0(2B) + packed dx/dy/scale(2B) per ch = 12B */

/* RAW stream fallback: stored in tile raw_flags byte (1 bit per stream 0..5) */
#define FLAG_RAW_BASE 0x01

/* grad error thresholds — 2-stage + adaptive */
#define GRAD_ERR_TIGHT  32    /* perfect → GRAD9 normal */
#define GRAD_ERR_LOOSE  96    /* acceptable → GRAD9 + loose bias in pid high bit */
/* adaptive effective thresh = GRAD_ERR_TIGHT + (tile_var >> 1) */
/* force GRAD9 when tile_var below this — near-flat gradient */
#define GRAD_VAR_FORCE  96
/* loose bias stored in pid: bit 7 = loose flag (pid & 0x7F = actual pid) */
#define PID_LOOSE_FLAG  0x80
#define PID_MASK        0x7F

/* internal grad mode (not on wire) */
#define GRADMODE_NORMAL 0
#define GRADMODE_LOOSE  1
#define GRADMODE_NONE   2   /* fallback to boundary classify */

/*
 * BMODE_LINEAR2 per channel (3B × 3ch = 9B total):
 *   int8 Y0, int8 dx, int8 dy
 * BMODE_GRAD9 per channel (4B × 3ch = 12B total):
 *   int16 zigzag Y0 (2B) + uint16 packed (dx_q:6 | dy_q:6 | scale:4) (2B)
 *   reconstruct: val(x,y) = Y0 + dx_q*scale*(x-x0) + dy_q*scale*(y-y0)
 */

/* ── int6 pack/unpack ──────────────────────────────────────*/
static inline uint8_t pack_i6(int v){
    if(v < -32) v = -32;
    if(v >  31) v =  31;
    return (uint8_t)(v & 0x3Fu);
}
static inline int unpack_i6(uint8_t v){
    v &= 0x3Fu;
    return (v & 0x20u) ? (int)v - 64 : (int)v;
}

/* ── compute_grad_error ────────────────────────────────────
 * Sample 5 points (4 corners + centre), compare against linear gradient pred.
 * Y0/dx/dy in full-scale (not Q4): pred(x,y) = Y0 + dx*(x-x0) + dy*(y-y0)
 */
static int compute_grad_error(
        const int *plane, int x0,int y0,int x1,int y1,int W,
        int Y0, int dx, int dy)
{
    int mx=(x0+x1)>>1, my=(y0+y1)>>1;
    int sx[5]={x0,x1-1,x0,  x1-1,mx};
    int sy[5]={y0,y0,  y1-1,y1-1,my};
    int err=0;
    for(int i=0;i<5;i++){
        int pred=Y0+dx*(sx[i]-x0)+dy*(sy[i]-y0);
        err+=abs(plane[sy[i]*W+sx[i]]-pred);
    }
    return err;
}

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
    int top_down = (img->h < 0);
    if(top_down) img->h = -img->h;
    int rs=(img->w*3+3)&~3;
    img->px=malloc(img->w*img->h*3);
    fseek(f,*(uint32_t*)(hdr+10),SEEK_SET);
    uint8_t *row=malloc(rs);
    for(int y=img->h-1;y>=0;y--){
        int dest_y = top_down ? (img->h-1-y) : y;
        if(fread(row,1,rs,f)!=(size_t)rs) break;
        for(int x=0;x<img->w;x++){
            int d=(dest_y*img->w+x)*3;
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
 * ── BOUNDARY CODEC (v15) ───────────────────────────────────
 *
 * Two modes only:
 *   BMODE_LINEAR2 (1): 2-point model  — Y0(int8) + dx(int8) + dy(int8) = 3B/ch = 9B
 *   BMODE_DELTA   (2): delta-sequence — corner(2B) + Σdeltas(1B each) ≈ 63B
 *
 * classify_boundary() chooses mode by measuring boundary variance + smoothness.
 * This decouples bmode selection from tile-type classification.
 */

/* ── classify_boundary ────────────────────────────────────
 * Inputs: mean-biased left col + top row for Y channel only (fast proxy).
 * Returns BMODE_LINEAR2 if boundary is smooth enough, else BMODE_DELTA.
 *
 * Criteria:
 *   var    = variance of col[]+row[] values
 *   smooth = Σ|second_diff| / (th+tw)   (curvature proxy)
 *   → LINEAR2 when var < VAR_THRESH AND smooth < SMOOTH_THRESH
 */
#define BVAR_THRESH   200   /* boundary variance threshold */
#define BSMOOTH_THRESH 4    /* curvature threshold per sample */

static int classify_boundary(
        const int *col_Y, const int *row_Y,
        int th, int tw)
{
    int N = th + tw;
    long sum = 0, sum2 = 0;
    for(int y=0;y<th;y++){ sum+=col_Y[y]; sum2+=(long)col_Y[y]*col_Y[y]; }
    for(int x=0;x<tw;x++){ sum+=row_Y[x]; sum2+=(long)row_Y[x]*row_Y[x]; }
    double mean = (double)sum / N;
    double var  = (double)sum2 / N - mean*mean;
    if(var >= BVAR_THRESH) return BMODE_DELTA;

    /* second-difference smoothness (cheaper than full curvature) */
    long smooth = 0;
    for(int y=1;y<th-1;y++) smooth += abs(col_Y[y+1]-2*col_Y[y]+col_Y[y-1]);
    for(int x=1;x<tw-1;x++) smooth += abs(row_Y[x+1]-2*row_Y[x]+row_Y[x-1]);
    double smooth_per = (double)smooth / N;
    return smooth_per < BSMOOTH_THRESH ? BMODE_LINEAR2 : BMODE_DELTA;
}

/* ── BMODE_LINEAR2: 2-point model ─────────────────────────
 * 3 bytes per channel: Y0(int8) dx(int8) dy(int8)
 *   Y0 clamped to int8 (mean-biased boundary fits ±127)
 *   dx = (row[tw-1] - row[0]) * 16 / (tw-1)  Q4, clamped int8
 *   dy = (col[th-1] - col[0]) * 16 / (th-1)  Q4, clamped int8
 * Reconstruction: val(x,y) ≈ Y0 + (dx*x + dy*y) / 16
 *   col[y] uses dy path, row[x] uses dx path.
 */
static inline int8_t clamp8(int v){ return (int8_t)(v<-127?-127:v>127?127:v); }

static void encode_boundary_linear2(
        const int *col, const int *row, int corner,
        int th, int tw, Buf *out)
{
    int dx = tw>1 ? (int)((row[tw-1]-row[0])*16/(tw-1)) : 0;
    int dy = th>1 ? (int)((col[th-1]-col[0])*16/(th-1)) : 0;
    bp(out,(uint8_t)clamp8(corner));
    bp(out,(uint8_t)clamp8(dx));
    bp(out,(uint8_t)clamp8(dy));
}

static void decode_boundary_linear2(
        const uint8_t *blob, int *off,
        int *col_out, int *row_out, int *corner_out,
        int th, int tw)
{
    int Y0 = (int)(int8_t)blob[(*off)++];
    int dx = (int)(int8_t)blob[(*off)++];
    int dy = (int)(int8_t)blob[(*off)++];
    *corner_out = Y0;
    for(int y=0;y<th;y++) col_out[y] = Y0 + dy*y/16;
    for(int x=0;x<tw;x++) row_out[x] = Y0 + dx*x/16;
}

/* ── BMODE_DELTA: delta-coded sequence ────────────────────
 * corner(2B zigzag int16) + th delta bytes (col) + tw delta bytes (row)
 * Total per channel: 2 + th + tw ≈ 66B  (lossless for any boundary)
 */
static void encode_boundary_delta(
        const int *col, const int *row, int corner,
        int th, int tw, Buf *out)
{
    bpz(out,(int16_t)corner);
    int prev=corner;
    for(int y=0;y<th;y++){
        int d=col[y]-prev; prev=col[y];
        if(d<-127)d=-127; if(d>127)d=127;
        bp(out,(uint8_t)zigzag((int16_t)d));
    }
    prev=corner;
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
    *corner_out=corner;
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

/* ── BMODE_GRAD9: compact gradient model ─────────────────
 * 3B per channel: int8 Y0(1B) + uint16 packed(dx_q:6|dy_q:6|scale:4)(2B) = 9B total
 * reconstruct: val(x,y) = Y0 + dx_q*scale*(x-x0) + dy_q*scale*(y-y0)
 * Y0 is mean-biased so range fits int8 (±127)
 */
static void encode_boundary_grad9(int Y0, int dx_raw, int dy_raw, Buf *out)
{
    bp(out,(uint8_t)clamp8(Y0));   /* 1B — mean-biased Y0 fits int8 */
    int maxv=abs(dx_raw)>abs(dy_raw)?abs(dx_raw):abs(dy_raw);
    int scale=maxv>31?(maxv+30)/31:1;
    int dx_q=dx_raw/scale, dy_q=dy_raw/scale;
    int sc4=scale>15?15:scale;
    uint16_t packed=(uint16_t)(((uint16_t)(pack_i6(dx_q)&0x3Fu)<<10)|
                               ((uint16_t)(pack_i6(dy_q)&0x3Fu)<<4)|
                               ((uint16_t)(sc4&0xFu)));
    bp(out,(uint8_t)(packed&0xFFu));
    bp(out,(uint8_t)(packed>>8));
}

static void decode_boundary_grad9(
        const uint8_t *blob, int *off,
        int *col_out, int *row_out, int *corner_out,
        int th, int tw)
{
    int Y0=(int)(int8_t)blob[(*off)++];            /* 1B */
    uint16_t packed=(uint16_t)(blob[*off]|(blob[(*off)+1]<<8)); (*off)+=2;
    int dx_q=unpack_i6((uint8_t)((packed>>10)&0x3Fu));
    int dy_q=unpack_i6((uint8_t)((packed>>4) &0x3Fu));
    int scale=(int)(packed&0xFu); if(scale==0) scale=1;
    int dx=dx_q*scale, dy=dy_q*scale;
    *corner_out=Y0;
    for(int y=0;y<th;y++) col_out[y]=Y0+dy*y;
    for(int x=0;x<tw;x++) row_out[x]=Y0+dx*x;
}

/* ── boundary encode/decode dispatch ─────────────────────
 * Both encode/decode one channel per call.
 * bmode must match between encode and decode.
 * GRAD9: caller passes Y0/dx/dy directly via col[0]/row[0]/corner convention:
 *   col[0]=Y0, row[0]=dx_raw(full-scale), corner=dy_raw(full-scale)
 */
static void encode_boundary_ch(int bmode,
        const int *col, const int *row, int corner,
        int th, int tw, Buf *out)
{
    if(bmode==BMODE_GRAD9)
        encode_boundary_grad9(col[0], row[0], corner, out);
    else if(bmode==BMODE_LINEAR2)
        encode_boundary_linear2(col,row,corner,th,tw,out);
    else
        encode_boundary_delta(col,row,corner,th,tw,out);
}

static void decode_boundary_ch(int bmode,
        const uint8_t *blob, int *off,
        int *col_out, int *row_out, int *corner_out,
        int th, int tw, int def)
{
    if(bmode==BMODE_GRAD9)
        decode_boundary_grad9(blob,off,col_out,row_out,corner_out,th,tw);
    else if(bmode==BMODE_LINEAR2)
        decode_boundary_linear2(blob,off,col_out,row_out,corner_out,th,tw);
    else
        decode_boundary_delta(blob,off,col_out,row_out,corner_out,th,tw);
    (void)def;
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

/* ── compute_tile_var ─────────────────────────────────────
 * Returns integer variance of Y channel for adaptive threshold.
 */
static int compute_tile_var(
        const int *iY, int x0, int y0, int x1, int y1, int W)
{
    int tw = x1-x0, th = y1-y0, tn = tw*th;
    long sum = 0, sum2 = 0;
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
        int v = iY[y*W+x];
        sum  += v;
        sum2 += (long)v*v;
    }
    long mean = sum / tn;
    long var  = sum2 / tn - mean*mean;
    return (int)(var < 0 ? 0 : var);
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

    /* boundary mode selection — v15 logic:
     *   GRADIENT tile → try GRAD9 first (5-point error check); fallback DELTA
     *   other tiles   → classify_boundary() → LINEAR2 or DELTA
     */
    int bmode;
    int grad_Y0=0, grad_dxY=0, grad_dyY=0;   /* GRAD9 params for Y */
    int grad_Y0Cg=0, grad_dxCg=0, grad_dyCg=0;
    int grad_Y0Co=0, grad_dxCo=0, grad_dyCol=0;

    if(ttype==TTYPE_GRADIENT){
        /* compute full-scale gradient params from corner pixels */
        grad_Y0  = iY [y0*W+x0];
        grad_dxY = (x1>x0+1)?(iY [y0*W+(x1-1)]-grad_Y0)/(tw-1):0;
        grad_dyY = (y1>y0+1)?(iY [(y1-1)*W+x0]-grad_Y0)/(th-1):0;
        int gerr    = compute_grad_error(iY,x0,y0,x1,y1,W,grad_Y0,grad_dxY,grad_dyY);
        int tile_var= compute_tile_var(iY,x0,y0,x1,y1,W);

        /* 2-stage + adaptive threshold + force GRAD9 for near-flat */
        int thresh_tight = 48 + (tile_var >> 1);  /* adaptive: wider window → more GRAD9 */
        int grad_mode;
        if(tile_var < GRAD_VAR_FORCE){
            grad_mode = GRADMODE_NORMAL;   /* force GRAD9: near-flat, ~70-80% coverage */
        } else if(gerr < thresh_tight){
            grad_mode = GRADMODE_NORMAL;   /* perfect fit */
        } else if(gerr < GRAD_ERR_LOOSE){
            grad_mode = GRADMODE_LOOSE;    /* acceptable: GRAD9 + loose bias in pid bit7 */
        } else {
            grad_mode = GRADMODE_NONE;     /* too noisy: fallback to boundary classify */
        }

        if(grad_mode == GRADMODE_NORMAL){
            bmode = BMODE_GRAD9;
            /* pid bit7 clear = normal */
        } else if(grad_mode == GRADMODE_LOOSE){
            bmode = BMODE_GRAD9;
            pid  |= PID_LOOSE_FLAG;        /* signal loose bias to residual encoder */
        } else {
            /* fallback: use classify_boundary (may give LINEAR2 or DELTA) */
            int *tmp_col=malloc(th*4), *tmp_row=malloc(tw*4);
            for(int y=0;y<th;y++) tmp_col[y]=x0>0?iY[(y0+y)*W+x0-1]:128;
            for(int x=0;x<tw;x++) tmp_row[x]=y0>0?iY[(y0-1)*W+x0+x]:128;
            long sy=0;
            for(int y=0;y<th;y++) sy+=tmp_col[y];
            for(int x=0;x<tw;x++) sy+=tmp_row[x];
            int bm=(int)(sy/(th+tw));
            for(int y=0;y<th;y++) tmp_col[y]-=bm;
            for(int x=0;x<tw;x++) tmp_row[x]-=bm;
            bmode=classify_boundary(tmp_col,tmp_row,th,tw);
            free(tmp_col); free(tmp_row);
        }

        grad_Y0Cg  = iCg[y0*W+x0];
        grad_dxCg  = (x1>x0+1)?(iCg[y0*W+(x1-1)]-grad_Y0Cg)/(tw-1):0;
        grad_dyCg  = (y1>y0+1)?(iCg[(y1-1)*W+x0]-grad_Y0Cg)/(th-1):0;
        grad_Y0Co  = iCo[y0*W+x0];
        grad_dxCo  = (x1>x0+1)?(iCo[y0*W+(x1-1)]-grad_Y0Co)/(tw-1):0;
        grad_dyCol = (y1>y0+1)?(iCo[(y1-1)*W+x0]-grad_Y0Co)/(th-1):0;
    } else {
        /* collect boundary Y for classify_boundary */
        int *tmp_col=malloc(th*4), *tmp_row=malloc(tw*4);
        for(int y=0;y<th;y++) tmp_col[y]=x0>0?iY[(y0+y)*W+x0-1]:128;
        for(int x=0;x<tw;x++) tmp_row[x]=y0>0?iY[(y0-1)*W+x0+x]:128;
        long sy=0;
        for(int y=0;y<th;y++) sy+=tmp_col[y];
        for(int x=0;x<tw;x++) sy+=tmp_row[x];
        int bm=(int)(sy/(th+tw));
        for(int y=0;y<th;y++) tmp_col[y]-=bm;
        for(int x=0;x<tw;x++) tmp_row[x]-=bm;
        bmode=classify_boundary(tmp_col,tmp_row,th,tw);
        free(tmp_col); free(tmp_row);
    }

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
    if(bmode==BMODE_GRAD9){
        /* GRAD9: pass mean-adjusted grad params via col[0]/row[0]/corner convention */
        int gY0 =grad_Y0 -meanY,  gdxY =grad_dxY,  gdyY =grad_dyY;
        int gCg0=grad_Y0Cg-meanCg,gdxCg=grad_dxCg, gdyCg=grad_dyCg;
        int gCo0=grad_Y0Co-meanCo,gdxCo=grad_dxCo, gdyCo=grad_dyCol;
        int tmpY[1]={gY0},  tdxY[1]={gdxY};
        int tmpCg[1]={gCg0},tdxCg[1]={gdxCg};
        int tmpCo[1]={gCo0},tdxCo[1]={gdxCo};
        encode_boundary_ch(BMODE_GRAD9,tmpY, tdxY, gdyY, th,tw,&bctx);
        encode_boundary_ch(BMODE_GRAD9,tmpCg,tdxCg,gdyCg,th,tw,&bctx);
        encode_boundary_ch(BMODE_GRAD9,tmpCo,tdxCo,gdyCo,th,tw,&bctx);
    } else {
        encode_boundary_ch(bmode,col_Y, row_Y, adj_tl_Y, th,tw,&bctx);
        encode_boundary_ch(bmode,col_Cg,row_Cg,adj_tl_Cg,th,tw,&bctx);
        encode_boundary_ch(bmode,col_Co,row_Co,adj_tl_Co,th,tw,&bctx);
    }
    free(col_Y);free(col_Cg);free(col_Co);
    free(row_Y);free(row_Cg);free(row_Co);

    /* rebuild context for predictor by decoding our own boundary */
    int *ctxLY=malloc(th*4),*ctxLCg=malloc(th*4),*ctxLCo=malloc(th*4);
    int *ctxTY=malloc(tw*4),*ctxTCg=malloc(tw*4),*ctxTCo=malloc(tw*4);
    int dummy_tl_Y,dummy_tl_Cg,dummy_tl_Co;
    int tmp_off=0;
    int clean_pid = pid & 0x7F;         /* strip loose flag for predictor calls */
    int is_loose  = (pid & PID_LOOSE_FLAG) != 0;
    decode_boundary_ch(bmode,bctx.buf,&tmp_off,ctxLY, ctxTY, &dummy_tl_Y, th,tw,def_Y-meanY);
    decode_boundary_ch(bmode,bctx.buf,&tmp_off,ctxLCg,ctxTCg,&dummy_tl_Cg,th,tw,def_C-meanCg);
    decode_boundary_ch(bmode,bctx.buf,&tmp_off,ctxLCo,ctxTCo,&dummy_tl_Co,th,tw,def_C-meanCo);
    int ptl_Y=dummy_tl_Y, ptl_Cg=dummy_tl_Cg, ptl_Co=dummy_tl_Co;

    /* residuals */
    int *locY =malloc(tn*4),*locCg=malloc(tn*4),*locCo=malloc(tn*4);
    int16_t *rY=malloc(tn*2),*rCg=malloc(tn*2),*rCo=malloc(tn*2);

    /* loose bias: 1B — biasY only, shared across all channels */
    int loose_bias_Y  = 0, loose_bias_Cg = 0, loose_bias_Co = 0;
    if(is_loose){
        int gerrY  = compute_grad_error(iY, x0,y0,x1,y1,W,grad_Y0, grad_dxY, grad_dyY);
        loose_bias_Y = loose_bias_Cg = loose_bias_Co = gerrY >> 3;
    }

    for(int y=0;y<th;y++) for(int x=0;x<tw;x++){
        int li=y*tw+x, gi=(y0+y)*W+(x0+x);
        int bY =iY [gi]-meanY;
        int bCg=iCg[gi]-meanCg;
        int bCo=iCo[gi]-meanCo;
        int pY =predict_local(clean_pid,locY, x,y,tw,ctxLY, ctxTY, ptl_Y, def_Y-meanY);
        int pCg=predict_local(clean_pid,locCg,x,y,tw,ctxLCg,ctxTCg,ptl_Cg,def_C-meanCg);
        int pCo=predict_local(clean_pid,locCo,x,y,tw,ctxLCo,ctxTCo,ptl_Co,def_C-meanCo);
        /* loose bias: shift pred toward signal center → smaller residuals → zstd wins */
        if(is_loose){ pY += loose_bias_Y; pCg += loose_bias_Cg; pCo += loose_bias_Co; }
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

    /* compress 6 streams — RAW fallback if zstd expands (noise tiles) */
    uint8_t *zd[6]; int zsz[6];
    uint8_t raw_flags=0;   /* bitmask: bit s = stream s stored raw */
    uint8_t *srcs[6]={hi[0],lo[0],hi[1],lo[1],hi[2],lo[2]};
    for(int s=0;s<6;s++){
        size_t cap=ZSTD_compressBound(tn);
        uint8_t *tmp=malloc(cap);
        int csz=(int)ZSTD_compress(tmp,cap,srcs[s],tn,ZST_LVL);
        if(csz<=0||csz>=tn){
            /* RAW fallback */
            free(tmp);
            zd[s]=malloc(tn); memcpy(zd[s],srcs[s],tn);
            zsz[s]=tn;
            raw_flags|=(uint8_t)(FLAG_RAW_BASE<<s);
        } else {
            zd[s]=tmp; zsz[s]=csz;
        }
    }
    for(int c=0;c<3;c++){free(hi[c]);free(lo[c]);}

    /*
     * Blob layout v15c:
     *   1B ttype | 1B bmode | 1B pid (bit7=loose) | 1B raw_flags
     *   [1B biasY — only if pid bit7 set; biasCg/biasCo derived = biasY]
     *   6B mean  [Y,Cg,Co as int16 zigzag]
     *   bctx.sz boundary ctx bytes
     *   6×4B stream sizes
     *   6 streams (zstd or raw per raw_flags bit)
     */
    int total = 4 + (is_loose?1:0) + 6 + bctx.sz + 6*4;
    for(int s=0;s<6;s++) total+=zsz[s];
    uint8_t *blob=malloc(total); int off=0;

    blob[off++]=(uint8_t)ttype;
    blob[off++]=(uint8_t)bmode;
    blob[off++]=(uint8_t)pid;   /* pid already has PID_LOOSE_FLAG set if is_loose */
    blob[off++]=raw_flags;
    if(is_loose){
        /* store biasY only (1B); decoder uses same value for all 3 channels */
        blob[off++]=(uint8_t)(loose_bias_Y & 0xFF);
    }
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
    int bmode    =blob[off++];
    int pid_raw  =blob[off++];
    int pid      = pid_raw & PID_MASK;      /* strip loose flag */
    int is_loose = (pid_raw & PID_LOOSE_FLAG) != 0;
    uint8_t raw_flags=blob[off++];   /* bit s = stream s is raw (not zstd) */
    /* read 1B biasY — same value applied to all 3 channels */
    int loose_bias_Y=0, loose_bias_Cg=0, loose_bias_Co=0;
    if(is_loose){
        loose_bias_Y = loose_bias_Cg = loose_bias_Co = (int)blob[off++];
    }
    int def_Y=128,def_C=0;

    int meanY =(int)unzigzag(r16(blob,off)); off+=2;
    int meanCg=(int)unzigzag(r16(blob,off)); off+=2;
    int meanCo=(int)unzigzag(r16(blob,off)); off+=2;

    int *ctxLY =malloc(th*4),*ctxLCg=malloc(th*4),*ctxLCo=malloc(th*4);
    int *ctxTY =malloc(tw*4),*ctxTCg=malloc(tw*4),*ctxTCo=malloc(tw*4);
    int tl_Y,tl_Cg,tl_Co;

    decode_boundary_ch(bmode,blob,&off,ctxLY, ctxTY, &tl_Y, th,tw,def_Y-meanY);
    decode_boundary_ch(bmode,blob,&off,ctxLCg,ctxTCg,&tl_Cg,th,tw,def_C-meanCg);
    decode_boundary_ch(bmode,blob,&off,ctxLCo,ctxTCo,&tl_Co,th,tw,def_C-meanCo);

    int zsz[6]; for(int s=0;s<6;s++){zsz[s]=(int)r32(blob,off);off+=4;}
    uint8_t *dstr[6];
    for(int s=0;s<6;s++){
        dstr[s]=malloc(tn);
        if(raw_flags&(uint8_t)(FLAG_RAW_BASE<<s)){
            memcpy(dstr[s],blob+off,zsz[s]);
        } else {
            ZSTD_decompress(dstr[s],tn,blob+off,zsz[s]);
        }
        off+=zsz[s];
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
        if(is_loose){ pY += loose_bias_Y; pCg += loose_bias_Cg; pCo += loose_bias_Co; }
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

    char out[512]; snprintf(out,sizeof(out),"%s.gp15",path);
    FILE *fo=fopen(out,"wb"); fwrite(C,1,file_sz,fo); fclose(fo); free(C);

    int raw=N*3;
    long long bnd_flat=0, bnd_grad=0, bnd_other=0;
    printf("Image  : %s (%dx%d)  tiles=%dx%d (%d)\n\n",path,W,H,TW,TH,NT);
    printf("=== Tile types ===\n");
    printf("  flat=%d  gradient=%d  edge=%d  noise=%d\n",tc[0],tc[1],tc[2],tc[3]);
    printf("  boundary: FLAT=0B  GRADIENT≈18B  EDGE≈32B(PARAM)  NOISE≈%dB\n", 3*3*(2+TH+TW));
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
    const char *bmnames[]={"NONE","LINEAR2","DELTA","?"};

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
            "GeoPixel v15\n"
            "  encode : %s input.bmp\n"
            "  decode : %s file.gp15\n"
            "  query  : %s file.gp15 tx ty\n",
            argv[0],argv[0],argv[0]);
        return 1;
    }
    clock_t t0=clock();
    int ret;
    if(strstr(argv[1],".gp15")){
        ret=(argc==4)?query_tile(argv[1],atoi(argv[2]),atoi(argv[3])):decode_full(argv[1]);
    } else {
        ret=encode(argv[1]);
    }
    printf("  Time   : %.2f ms\n",(double)(clock()-t0)/CLOCKS_PER_SEC*1000.0);
    return ret;
}
