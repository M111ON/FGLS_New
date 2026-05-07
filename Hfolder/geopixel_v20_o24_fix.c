/*
 * GeoPixel v18 — Goldberg Full Integration: Point 2 (dedup) + Point 3 (pure circuit)
 *
 * Point 3: gerr REMOVED — pure circuit_fired replaces all threshold logic
 *   n_circuits 0-2 → GRAD9_NORMAL   (simple, avg_var ~215)
 *   n_circuits 3-5 → GRAD9_LOOSE    (moderate, avg_var ~311-437)
 *   n_circuits 6   → DELTA          (complex, avg_var ~575)
 *   Mapping calibrated from probe data on test01.bmp
 *
 * Point 2: Blob dedup via stamp hash (XOR fold of blob bytes)
 *   Duplicate tiles → ref-pointer in index (bit31 set)
 *   Decoder resolves ref-pointer transparently
 *   Dedup table: 4096 slots, 16-probe linear hash
 *
 * Compile: gcc -O3 -o geopixel_v18 geopixel_v18_c.c -lm -lzstd -lpthread
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <zstd.h>
#include <png.h>
#define GEO_GPX_ANIM_IMPL
#include "geo_goldberg_tile.h"   /* Point 2+3: Goldberg tile integration */
#include "geo_tring_addr.h"      /* O5: TRing address encoder            */
#include "geo_o4_connector.h"    /* O5: O4 grid bridge                   */
#include "gpx4_container_o22.h"  /* GPX4 + O22 GEO layer                 */



#include "geo_gpx_anim_o23.h"    /* GPX4 animated pattern support        */

/* grad mode counters (thread-safe) — reset before encode, read after */
static atomic_int g_count_grad_normal = 0;
static atomic_int g_count_grad_loose  = 0;
static atomic_int g_count_grad_none   = 0;

static atomic_int g_count_gb_override_loose = 0;
static atomic_int g_count_gb_override_delta = 0;

/* noise avg_var histogram: buckets 0-9 = [200,300) [300,400) ... [1100,∞) */
#define NOISE_HIST_N 10
static atomic_int g_noise_hist[NOISE_HIST_N];

/* Point 2: global window accumulator + blueprint log (max 64 windows) */
#define GGT_MAX_BLUEPRINTS  64
static GGWindow      g_gb_window;
static GGBlueprint   g_gb_blueprints[GGT_MAX_BLUEPRINTS];
static int           g_gb_bp_count = 0;

/* ── config ─────────────────────────────────────────────── */
#define MAGIC_V13   0x47503D78u  /* v15 magic */
#define TILE        32
#define ZST_LVL     9

/* ════════════════════════════════════════════════════════
 * O23: Pentagon Address System (O21 geometry, embedded)
 * Used by encoder to build GEOA layer (GPX4_LAYER_GEO)
 * ════════════════════════════════════════════════════════ */
#ifndef O23_PHI
#define O23_PHI 1.61803398874989484820
#define O23_HILBERT_N 16  /* must match tile grid (512/32=16) */

typedef struct { double x,y,z; } O23V3;
static O23V3 o23_ico_v[12];
static int   o23_ico_built = 0;

static O23V3 o23v3n(O23V3 v){
    double r=sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
    if(r<1e-12)return v; return(O23V3){v.x/r,v.y/r,v.z/r};
}
static double o23v3d(O23V3 a,O23V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}

static void o23_build_ico(void){
    if(o23_ico_built) return;
    double t=1.0/O23_PHI;
    O23V3 raw[12]={{-1,t,0},{1,t,0},{-1,-t,0},{1,-t,0},
                   {0,-1,t},{0,1,t},{0,-1,-t},{0,1,-t},
                   {t,0,-1},{t,0,1},{-t,0,-1},{-t,0,1}};
    for(int i=0;i<12;i++) o23_ico_v[i]=o23v3n(raw[i]);
    o23_ico_built=1;
}

static int o23_nearest_pent(O23V3 s){
    int best=0; double bd=-2;
    for(int i=0;i<12;i++){double d=o23v3d(s,o23_ico_v[i]);if(d>bd){bd=d;best=i;}}
    return best;
}

static uint32_t o23_hilbert(uint32_t n,uint32_t x,uint32_t y){
    uint32_t d=0;
    for(uint32_t s=n/2;s>0;s>>=1){
        uint32_t rx=(x&s)?1:0,ry=(y&s)?1:0;
        d+=(uint32_t)s*s*((3u*rx)^ry);
        if(ry==0){if(rx==1){x=(uint32_t)(s-1)-x;y=(uint32_t)(s-1)-y;}
                  uint32_t tmp=x;x=y;y=tmp;}
    }
    return d;
}

/*
 * o23_build_geo_addrs — O21 geometry + balance-refine
 *   W,H    : image pixel dims
 *   TW,TH  : tile grid dims (W/TILE, H/TILE)
 *   out    : caller-allocated Gpx4GeoAddr[TW*TH]
 *
 *   Balance-refine: pentagons with >1.5x mean tile count
 *   get upper-hilbert-half tiles flagged in bit 13 of sub field.
 */
static void o23_build_geo_addrs(int W,int H,int TW,int TH,Gpx4GeoAddr *out){
    o23_build_ico();
    int NT=TW*TH;

    /* pass 1: assign pent + hilbert */
    for(int ty=0;ty<TH;ty++){
        for(int tx=0;tx<TW;tx++){
            double cx=((tx+0.5)*(double)TILE - W*0.5)/(W*0.5);
            double cy=((ty+0.5)*(double)TILE - H*0.5)/(H*0.5);
            double r2=cx*cx+cy*cy;
            O23V3 s;
            if(r2>=1.0){ s=(O23V3){0,0,1}; }
            else { double z=sqrt(1.0-r2); s=o23v3n((O23V3){cx,cy,z}); }
            int p=o23_nearest_pent(s);
            uint32_t hd=o23_hilbert(O23_HILBERT_N,(uint32_t)tx,(uint32_t)ty);
            out[ty*TW+tx].packed=((uint32_t)p<<28)|((hd&0x3FFFu)<<14);
        }
    }

    /* pass 2: balance-refine — mark overloaded pent upper-hilbert-half */
    int pcnt[12]={0};
    for(int i=0;i<NT;i++) pcnt[GPX4_GEO_PENT(out[i].packed)]++;
    int mean = NT/12;
    int threshold = mean + mean/2;  /* 1.5x mean */

    for(int p=0;p<12;p++){
        if(pcnt[p]<=threshold) continue;
        /* collect hilbert values for this pent */
        uint32_t *hv=(uint32_t*)malloc(pcnt[p]*sizeof(uint32_t));
        int hc=0;
        for(int i=0;i<NT;i++)
            if(GPX4_GEO_PENT(out[i].packed)==(uint32_t)p)
                hv[hc++]=GPX4_GEO_HILBERT(out[i].packed);
        /* find median */
        uint32_t median=0;
        if(hc>0){
            /* simple selection sort for small N */
            for(int a=0;a<hc-1;a++)
                for(int b=a+1;b<hc;b++)
                    if(hv[b]<hv[a]){uint32_t t=hv[a];hv[a]=hv[b];hv[b]=t;}
            median=hv[hc/2];
        }
        free(hv);
        /* set bit 13 of sub field for upper half */
        for(int i=0;i<NT;i++){
            if(GPX4_GEO_PENT(out[i].packed)==(uint32_t)p &&
               GPX4_GEO_HILBERT(out[i].packed)>median){
                out[i].packed |= (1u<<13);  /* balance-refine flag in sub[13] */
            }
        }
    }
}
#endif /* O23_PHI */
#define MAX_FLAT_COLORS 4

/* tile types */
#define TTYPE_FLAT     0
#define TTYPE_GRADIENT 1
#define TTYPE_EDGE     2
#define TTYPE_NOISE    3
#define TTYPE_DELTA    4   /* high-variance predictable: 500 ≤ avg_var < 1000 */

/* boundary modes */
#define BMODE_NONE    0   /* FLAT: no boundary stored */
#define BMODE_LINEAR2 1   /* smooth boundary: Y0+dx+dy per ch = 9B */
#define BMODE_DELTA   2   /* complex boundary: delta-coded = ~63B */
#define BMODE_GRAD9   3   /* gradient tile: zigzag Y0(2B) + packed dx/dy/scale(2B) per ch = 12B */

/* RAW stream fallback: stored in tile raw_flags byte (1 bit per stream 0..5) */
#define FLAG_RAW_BASE 0x01

/* grad error thresholds — 2-stage + adaptive */
#define GRAD_ERR_TIGHT  32    /* perfect → GRAD9 normal */
/* LOOSE threshold is now adaptive: 160 + tile_var (replaces hardcode 96) */
/* adaptive effective thresh_tight = 48 + (tile_var >> 1)               */
/* force GRAD9 when tile_var below this — near-flat gradient */
#define GRAD_VAR_FORCE  0    /* disabled: let adaptive thresh handle everything */
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

static FILE *gv_open_memory_file(const uint8_t *data, size_t size) {
    /* Portable replacement for GNU fmemopen(): spill the PNG blob into a temp file. */
    FILE *f = tmpfile();
    if (!f) return NULL;
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    return f;
}

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
    if(avg_var<500.0)     return TTYPE_EDGE;
    if(avg_var<1000.0)    return TTYPE_DELTA;
    /* noise histogram probe */
    { int b=(int)((avg_var-200.0)/100.0); if(b<0)b=0; if(b>=NOISE_HIST_N)b=NOISE_HIST_N-1;
      atomic_fetch_add(&g_noise_hist[b],1); }
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

        /* ── Point 3: Pure Goldberg circuit decision (replaces gerr) ────
         * n_circuits 0-2 → GRAD9_NORMAL  (simple, low variance)
         * n_circuits 3-5 → GRAD9_LOOSE   (moderate complexity)
         * n_circuits 6   → DELTA         (high complexity, ~575 avg_var)
         *
         * Calibrated from probe: n_circuits correlates with tile variance
         * ─────────────────────────────────────────────────────────────── */
        GGTileResult gbr = ggt_tile_scan(iY, x0, y0, x1, y1, W);
        int grad_mode;
        if     (gbr.n_circuits <= 2) grad_mode = GRADMODE_NORMAL;
        else if(gbr.n_circuits <= 5) grad_mode = GRADMODE_LOOSE;
        else                         grad_mode = GRADMODE_NONE;

        if(grad_mode == GRADMODE_NORMAL){
            bmode = BMODE_GRAD9;
            atomic_fetch_add(&g_count_grad_normal, 1);
        } else if(grad_mode == GRADMODE_LOOSE){
            bmode = BMODE_GRAD9;
            pid  |= PID_LOOSE_FLAG;
            atomic_fetch_add(&g_count_grad_loose, 1);
        } else {
            atomic_fetch_add(&g_count_grad_none, 1);
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
    int grad_mode;  /* GRADMODE_NORMAL/LOOSE/NONE — set by encode_tile */
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
    /* reset grad mode counters */
    atomic_store(&g_count_grad_normal, 0);
    atomic_store(&g_count_grad_loose,  0);
    atomic_store(&g_count_grad_none,   0);
    atomic_store(&g_count_gb_override_loose, 0);
    atomic_store(&g_count_gb_override_delta, 0);
    for(int _h=0;_h<NOISE_HIST_N;_h++) atomic_store(&g_noise_hist[_h],0);
    /* reset point 2 window */
    ggt_window_init(&g_gb_window);
    g_gb_bp_count = 0;
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
    /* ── Precompute perceptual fingerprint keys for NOISE tiles (before iY freed) ── */
    uint32_t *noise_fkey = (uint32_t*)malloc(NT * sizeof(uint32_t));
    for(int i=0; i<NT; i++){
        noise_fkey[i] = 0xFFFFFFFFu;
        if(jobs[i].ttype == TTYPE_NOISE){
            int tw_i = jobs[i].x1 - jobs[i].x0;
            int th_i = jobs[i].y1 - jobs[i].y0;
            int tn_i = tw_i * th_i; if(tn_i < 1) tn_i = 1;
            long sum_y = 0, sum_y2 = 0;
            for(int y=jobs[i].y0; y<jobs[i].y1; y++)
                for(int x=jobs[i].x0; x<jobs[i].x1; x++){
                    int v = jobs[i].iY[y*jobs[i].W+x];
                    sum_y += v; sum_y2 += v*v;
                }
            int mean_y = (int)(sum_y / tn_i);
            int var_y  = (int)(sum_y2 / tn_i - (long)mean_y * mean_y);
            int vb = var_y < 500 ? 0 : var_y < 2000 ? 1 : var_y < 6000 ? 2 : 3;
            noise_fkey[i] = ((uint32_t)(mean_y & 0xFF) | ((uint32_t)vb << 8)) & (1024-1);
        }
    }

    free(iY);free(iCg);free(iCo);

    /* ── Point 2: feed all tile stamps into blueprint window ── */
    for(int i = 0; i < NT; i++){
        /* derive mode from blob[0] = bmode byte
         * BMODE_FLAT=0, BMODE_GRAD9=1, BMODE_LINEAR2=2, BMODE_DELTA=3 */
        uint8_t bmode_byte = (jobs[i].blob && jobs[i].blob_sz > 0) ? jobs[i].blob[0] : 0;
        GGTileResult stub;
        memset(&stub, 0, sizeof(stub));
        stub.stamp         = (uint32_t)i ^ (uint32_t)bmode_byte;
        stub.circuit_fired = bmode_byte;
        stub.n_circuits    = (bmode_byte == 1) ? 1 : (bmode_byte >= 2) ? 3 : 0;
        ggt_window_feed(&g_gb_window, &stub);
        if(g_gb_window.ready && g_gb_bp_count < GGT_MAX_BLUEPRINTS){
            g_gb_blueprints[g_gb_bp_count++] = g_gb_window.blueprint;
        }
    }
    /* flush partial last window */
    if(g_gb_window.event_count % GGT_FLUSH_PERIOD != 0
       && g_gb_bp_count < GGT_MAX_BLUEPRINTS){
        g_gb_blueprints[g_gb_bp_count++] = ggt_window_flush(&g_gb_window);
    }

    int index_sz=NT*8;
    uint32_t cur_off=HDR_BYTES+index_sz;
    TileIdx *idx=malloc(NT*sizeof(TileIdx));
    int total_tile=0;
    int dedup_hits=0, dedup_saved=0;
    int fb_dedup_hits=0, fb_dedup_saved=0;

    /* ── Point 2: Blob dedup — FLAT tiles only ──────────────────────────
     * Only FLAT blobs (bmode=0) are coordinate-independent → safe to dedup.
     * GRAD9/LINEAR2/DELTA blobs embed boundary data relative to tile position
     * → same blob decoded at different (x0,y0) produces different pixels.
     * ─────────────────────────────────────────────────────────────────── */
#define DEDUP_TABLE_SIZE  4096
    typedef struct { uint32_t stamp; uint32_t offset; uint16_t size; uint8_t used; } DedupEntry;
    DedupEntry *dedup = calloc(DEDUP_TABLE_SIZE, sizeof(DedupEntry));

    for(int i = 0; i < NT; i++){
        uint8_t bmode_b = (jobs[i].blob && jobs[i].blob_sz > 0) ? jobs[i].blob[0] : 0xFF;
        int is_flat = (bmode_b == 0);   /* BMODE_FLAT = 0 */

        if(is_flat){
            uint32_t stamp = 0;
            for(int b = 0; b < jobs[i].blob_sz; b++) stamp ^= jobs[i].blob[b];
            uint32_t slot = (stamp ^ (uint32_t)jobs[i].blob_sz) & (DEDUP_TABLE_SIZE - 1);
            int found = 0;
            for(int probe = 0; probe < 16; probe++){
                uint32_t s = (slot + (uint32_t)probe) & (DEDUP_TABLE_SIZE - 1);
                DedupEntry *e = &dedup[s];
                if(!e->used){
                    e->stamp=stamp; e->offset=cur_off;
                    e->size=(uint16_t)jobs[i].blob_sz; e->used=1;
                    idx[i].offset=cur_off; idx[i].size=(uint32_t)jobs[i].blob_sz;
                    cur_off+=jobs[i].blob_sz; total_tile+=jobs[i].blob_sz;
                    found=1; break;
                } else if(e->stamp==stamp && e->size==(uint16_t)jobs[i].blob_sz){
                    idx[i].offset=0x80000000u|e->offset;
                    idx[i].size=(uint32_t)jobs[i].blob_sz;
                    dedup_hits++; dedup_saved+=jobs[i].blob_sz;
                    found=1; break;
                }
            }
            if(!found){
                idx[i].offset=cur_off; idx[i].size=(uint32_t)jobs[i].blob_sz;
                cur_off+=jobs[i].blob_sz; total_tile+=jobs[i].blob_sz;
            }
        } else {
            idx[i].offset=cur_off; idx[i].size=(uint32_t)jobs[i].blob_sz;
            cur_off+=jobs[i].blob_sz; total_tile+=jobs[i].blob_sz;
        }
    }
    free(dedup);

    int file_sz = HDR_BYTES + index_sz + total_tile;
    uint8_t *C=malloc(file_sz);
    w32(C,HDR_MAGIC,(uint32_t)MAGIC_V13);
    w32(C,HDR_W,(uint32_t)W); w32(C,HDR_H,(uint32_t)H);
    w32(C,HDR_TW,(uint32_t)TW); w32(C,HDR_TH,(uint32_t)TH);
    w32(C,HDR_NT,(uint32_t)NT);
    w32(C,HDR_FSIZE,(uint32_t)file_sz);
    w32(C,HDR_FLAGS,0u);
    for(int i=0;i<NT;i++){w32(C,HDR_BYTES+i*8,idx[i].offset);w32(C,HDR_BYTES+i*8+4,idx[i].size);}
    int off2=HDR_BYTES+index_sz;
    for(int i=0;i<NT;i++){
        if(!(idx[i].offset & 0x80000000u)){   /* not a ref-pointer → write blob */
            memcpy(C+off2,jobs[i].blob,jobs[i].blob_sz);
            off2+=jobs[i].blob_sz;
        }
        free(jobs[i].blob);
    }
    free(idx);

    /* verify — resolve ref-pointer (bit31) before decode */
    uint8_t *recon=malloc(N*3);
    for(int i=0;i<NT;i++){
        int tx=i%TW,ty=i/TW;
        int y0=ty*TILE,y1=y0+TILE; if(y1>H)y1=H;
        int x0=tx*TILE,x1=x0+TILE; if(x1>W)x1=W;
        uint32_t off_raw = r32(C,HDR_BYTES+i*8);
        uint32_t off_real = off_raw & 0x7FFFFFFFu;  /* strip bit31 */
        uint32_t bsz = r32(C,HDR_BYTES+i*8+4);
        decode_tile_blob(C+off_real, bsz, x0,y0,x1,y1,W,recon);
    }
    int lossless=(memcmp(img.px,recon,(size_t)N*3)==0);
    long long sq=0; for(int i=0;i<N*3;i++){int d=img.px[i]-recon[i];sq+=d*d;}
    double psnr=sq==0?999.0:10.0*log10(65025.0*N*3/sq);
    free(img.px);free(recon);

    int tc[5]={0};
    for(int i=0;i<NT;i++){
        int t=jobs[i].ttype; if(t<0||t>4) t=3;
        tc[t]++;
    }
    /* jobs freed after GPX3 block (needs jobs[i].ttype) */

    char out[512]; snprintf(out,sizeof(out),"%s.gp15",path);
    FILE *fo=fopen(out,"wb"); fwrite(C,1,file_sz,fo); fclose(fo);

    /* ── O5/O10: GPX3 output — entropy-adaptive O4 routing ───────────────
     * TTYPE_NOISE tiles bypass O4 (geometry adds no compression value).
     * tile_rows[i]=0  → fallback: raw blob stored after PNG section.
     * tile_rows[i]>0  → O4 path (GPX2 compatible for non-noise tiles).
     * Format: magic'GPX3' + 16B hdr + NT×6B table + PNG + fallback_section
     * ──────────────────────────────────────────────────────────────────── */
    {
        /* Pass 1: count O4 rows — skip TTYPE_NOISE tiles only */
        uint32_t total_rows = 0;
        for(int i=0; i<NT; i++){
            if(jobs[i].ttype == TTYPE_NOISE) continue;
            uint32_t bsz = r32(C, HDR_BYTES + i*8 + 4);
            uint32_t n_chunks = (bsz + O4_CHUNK_BYTES - 1) / O4_CHUNK_BYTES;
            uint32_t gh = (n_chunks + O4_GRID_W - 1) / O4_GRID_W;
            if(gh < 1) gh = 1;
            total_rows += gh;
        }

        uint32_t gpx_rgb_sz = O4_GRID_W * (total_rows > 0 ? total_rows : 1) * 3u;
        uint8_t *gpx_rgb = (uint8_t*)calloc(gpx_rgb_sz, 1);
        uint32_t row_cursor = 0;
        int o4_failed = 0;
        int n_fallback = 0;

        uint16_t *tile_rows = (uint16_t*)malloc(NT * sizeof(uint16_t));
        uint32_t *tile_bsz  = (uint32_t*)malloc(NT * sizeof(uint32_t));

        /* Pre-allocate fallback section for noise blobs */
        uint32_t fb_total = 0;
        for(int i=0;i<NT;i++) if(jobs[i].ttype==TTYPE_NOISE)
            fb_total += r32(C, HDR_BYTES + i*8 + 4);
        uint8_t *fb_buf = fb_total > 0 ? (uint8_t*)malloc(fb_total) : NULL;
        uint32_t fb_cursor = 0;

        /* ── Approx dedup table for NOISE/fallback tiles ───────────────────
         * Fingerprint = mean_Y (8b) XOR var_bucket (2b) → 10-bit key → 1024 buckets
         * Each bucket holds up to FB_BUCKET_DEPTH candidates (tile index).
         * On match: set GPX4_TILE_REF, store ref idx in blob_sz, skip fb_buf write.
         * ─────────────────────────────────────────────────────────────── */
#define FB_BUCKET_N     1024
#define FB_BUCKET_DEPTH    8
        typedef struct { int tile_idx; uint32_t bsz; uint32_t fb_off; } FBCandidate;
        FBCandidate fb_buckets[FB_BUCKET_N][FB_BUCKET_DEPTH];
        int         fb_bucket_cnt[FB_BUCKET_N];
        memset(fb_buckets,    0, sizeof(fb_buckets));
        memset(fb_bucket_cnt, 0, sizeof(fb_bucket_cnt));
        /* fb_tile_off[i]: actual offset of tile i's blob in fb_buf (-1 = ref) */
        int *fb_tile_off = (int*)malloc(NT * sizeof(int));
        for(int ii=0;ii<NT;ii++) fb_tile_off[ii]=-1;
        /* fb_dedup_hits/saved declared at function scope */

        /* Pass 2: route tiles */
        for(int i=0; i<NT && gpx_rgb; i++){
            uint32_t off_raw = r32(C, HDR_BYTES + i*8);
            uint32_t bsz     = r32(C, HDR_BYTES + i*8 + 4);
            const uint8_t *blob = C + (off_raw & 0x7FFFFFFFu);
            if(tile_bsz) tile_bsz[i] = bsz;

            /* ── Noise: approx dedup then fallback section ── */
            if(jobs[i].ttype == TTYPE_NOISE){
                if(tile_rows) tile_rows[i] = 0;

                /* use precomputed fingerprint key */
                uint32_t fkey = noise_fkey[i] & (FB_BUCKET_N-1);

                /* search bucket for exact blob match */
                int matched = 0;
                if(fb_buf){
                    int cnt = fb_bucket_cnt[fkey];
                    for(int p=0; p<cnt && p<FB_BUCKET_DEPTH; p++){
                        FBCandidate *c = &fb_buckets[fkey][p];
                        if(c->bsz == bsz &&
                           memcmp(blob, fb_buf + c->fb_off, bsz) == 0){
                            /* REF: tile_bsz[i] repurposed as ref tile index */
                            tile_bsz[i] = (uint32_t)c->tile_idx;
                            fb_tile_off[i] = -1; /* mark as ref */
                            fb_dedup_hits++; fb_dedup_saved += (int)bsz;
                            matched = 1;
                            break;
                        }
                    }
                }

                if(!matched){
                    int cnt = fb_bucket_cnt[fkey];
                    fb_buckets[fkey][cnt % FB_BUCKET_DEPTH].tile_idx = i;
                    fb_buckets[fkey][cnt % FB_BUCKET_DEPTH].bsz      = bsz;
                    fb_buckets[fkey][cnt % FB_BUCKET_DEPTH].fb_off   = fb_cursor;
                    fb_bucket_cnt[fkey]++;
                    fb_tile_off[i] = (int)fb_cursor;
                    if(fb_buf){ memcpy(fb_buf + fb_cursor, blob, bsz); fb_cursor += bsz; }
                }
                n_fallback++;
                continue;
            }

            /* ── O4 path: FLAT / GRADIENT / EDGE ── */
            O4GridCtx ctx;
            uint8_t tx = (uint8_t)(i % TW);
            uint8_t ty = (uint8_t)(i / TW);
            o4_encode(blob, bsz, tx, ty, &ctx);
            if(tile_rows) tile_rows[i] = (uint16_t)ctx.grid_h;

            for(uint32_t r=0; r<ctx.grid_h; r++){
                for(uint32_t c=0; c<O4_GRID_W; c++){
                    GeoPixel px = ctx.grid[r][c];
                    uint32_t base = (row_cursor + r) * O4_GRID_W * 3 + c * 3;
                    gpx_rgb[base+0] = px.r;
                    gpx_rgb[base+1] = px.g;
                    gpx_rgb[base+2] = px.b;
                }
            }
            row_cursor += ctx.grid_h;

            /* verify roundtrip for first tile */
            if(i==0){
                uint8_t *recovered = malloc(bsz);
                if(recovered){
                    uint32_t n_chunks = (bsz + O4_CHUNK_BYTES - 1) / O4_CHUNK_BYTES;
                    o4_decode(&ctx, recovered, n_chunks, bsz);
                    if(memcmp(blob, recovered, bsz)!=0) o4_failed++;
                    free(recovered);
                }
            }
        }

        /* ── Write GPX4 container: O4 layer (PNG) + FALLBACK layer + META ── */
        if(gpx_rgb){
            char gpx_path[512]; snprintf(gpx_path,sizeof(gpx_path),"%s.gpx4",path);

            /* encode O4 grid → PNG blob in memory via fmemopen */
            uint8_t *png_blob = NULL;
            uint32_t png_blob_sz = 0;
            {
                /* write PNG to temp file then read back */
                char tmp_png[512]; snprintf(tmp_png,sizeof(tmp_png),"%s.tmp_o4.png",path);
                png_structp pp = png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
                png_infop   pi = png_create_info_struct(pp);
                if(!setjmp(png_jmpbuf(pp))){
                    FILE *pf = fopen(tmp_png,"wb");
                    png_init_io(pp, pf);
                    png_set_IHDR(pp,pi, O4_GRID_W, total_rows>0?total_rows:1, 8,
                                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
                    png_write_info(pp, pi);
                    for(uint32_t r=0; r<total_rows; r++)
                        png_write_row(pp, gpx_rgb + r * O4_GRID_W * 3);
                    png_write_end(pp, pi);
                    fclose(pf);
                    /* read back */
                    FILE *rf = fopen(tmp_png,"rb");
                    if(rf){
                        fseek(rf,0,SEEK_END); long psz=ftell(rf); rewind(rf);
                        png_blob = (uint8_t*)malloc((size_t)psz);
                        png_blob_sz = (uint32_t)psz;
                        if((long)fread(png_blob,1,(size_t)psz,rf)!=psz){
                            free(png_blob); png_blob=NULL; png_blob_sz=0;
                        }
                        fclose(rf);
                    }
                    remove(tmp_png);
                }
                png_destroy_write_struct(&pp, &pi);
            }

            /* build tile tables for GPX4 */
            Gpx4TileEntry *o4_tiles = (Gpx4TileEntry*)calloc(NT, sizeof(Gpx4TileEntry));
            Gpx4TileEntry *fb_tiles = (Gpx4TileEntry*)calloc(NT, sizeof(Gpx4TileEntry));
            for(int i=0; i<NT; i++){
                if(tile_rows[i] > 0){
                    /* O4 tile */
                    o4_tiles[i].rows    = tile_rows[i];
                    o4_tiles[i].tflags  = GPX4_TILE_PRESENT;
                    o4_tiles[i].blob_sz = tile_bsz[i];
                } else {
                    /* noise → fallback */
                    fb_tiles[i].rows   = 0;
                    if(fb_tile_off[i] == -1){
                        /* dedup ref: blob_sz holds ref tile index */
                        fb_tiles[i].tflags  = GPX4_TILE_PRESENT | GPX4_TILE_REF;
                        fb_tiles[i].blob_sz = tile_bsz[i]; /* ref tile index */
                    } else {
                        fb_tiles[i].tflags  = GPX4_TILE_PRESENT;
                        fb_tiles[i].blob_sz = tile_bsz[i];
                    }
                }
            }
            free(fb_tile_off);

            /* META: encode stats as compact string */
            char meta_buf[256];
            int meta_len = snprintf(meta_buf, sizeof(meta_buf),
                "o4_rows=%u,noise=%d,o4_rt=%s,grid_w=%u,img_w=%d,img_h=%d",
                total_rows, n_fallback,
                o4_failed?"FAIL":"OK", O4_GRID_W, W, H);

            /* O23: build GEOA layer */
            Gpx4GeoAddr *geo_addrs=(Gpx4GeoAddr*)malloc(NT*sizeof(Gpx4GeoAddr));
            o23_build_geo_addrs(W,H,TW,TH,geo_addrs);
            uint8_t *geo_buf=(uint8_t*)malloc((size_t)NT*GPX4_GEO_ADDR_SZ);
            gpx4_geo_write_layer_data(geo_addrs,NT,geo_buf);
            free(geo_addrs);

            /* assemble layers */
            Gpx4LayerDef layers[4];
            layers[0].type = GPX4_LAYER_O4;
            layers[0].lflags = 0;
            memcpy(layers[0].name, "O4  ", 4);
            layers[0].data = png_blob;
            layers[0].size = png_blob_sz;
            layers[0].tiles = o4_tiles;
            layers[1].type = GPX4_LAYER_FALLBACK;
            layers[1].lflags = 0;
            memcpy(layers[1].name, "NOIS", 4);
            layers[1].data = fb_buf;
            layers[1].size = fb_cursor;
            layers[1].tiles = fb_tiles;
            layers[2].type = GPX4_LAYER_META;
            layers[2].lflags = 0;
            memcpy(layers[2].name, "META", 4);
            layers[2].data = (uint8_t*)meta_buf;
            layers[2].size = (uint32_t)meta_len;
            layers[2].tiles = NULL;
            layers[3].type = GPX4_LAYER_GEO;
            layers[3].lflags = 0;
            memcpy(layers[3].name, GPX4_GEO_NAME, 4);
            layers[3].data = geo_buf;
            layers[3].size = (uint32_t)NT*GPX4_GEO_ADDR_SZ;
            layers[3].tiles = NULL;
            /* GEO layer always present (layer[3]); FALLBACK only when fb_cursor>0 */
            /* Swap: put GEO at [2] (always), META at [3], FALLBACK conditional at [4] */
            /* Simpler: always write 4 layers, NOIS size=0 is harmless */
            int n_layers = 4;

            gpx4_write(gpx_path, (uint16_t)TW, (uint16_t)TH, layers, n_layers);
            free(o4_tiles);
            free(fb_tiles);
            free(png_blob);
            free(geo_buf);

            /* stats */
            long gpx_sz = 0;
            FILE *sz_f = fopen(gpx_path,"rb");
            if(sz_f){fseek(sz_f,0,SEEK_END);gpx_sz=ftell(sz_f);fclose(sz_f);}
            printf("\n=== GPX4 Container (.gpx4) ===\n");
            printf("  Layers    : O4 + %s+ META\n", fb_cursor>0?"FALLBACK ":"");
            printf("  Tiles     : %d  O4_grid=%dx%u\n", NT, O4_GRID_W, total_rows);
            printf("  Noise FB  : %d tiles  %u B\n", n_fallback, fb_cursor);
            printf("  O4 PNG    : %u B\n", png_blob_sz);
            printf("  Roundtrip : %s\n", o4_failed?"FAIL ✗":"OK ✓");
            printf("  GPX4 size : %ld B  (%.2fx vs gp15)\n", gpx_sz,
                   file_sz>0?(double)gpx_sz/file_sz:0.0);
            printf("  Saved     : %s\n", gpx_path);

            free(gpx_rgb);
            free(tile_rows);
            free(tile_bsz);
            free(fb_buf);
        }
    }
    free(noise_fkey);
    free(jobs);
    free(C);

    int raw=N*3;
    long long bnd_flat=0, bnd_grad=0, bnd_other=0;
    int cn = atomic_load(&g_count_grad_normal);
    int cl = atomic_load(&g_count_grad_loose);
    int cd = atomic_load(&g_count_grad_none);
    int cg = tc[1];
    printf("Image  : %s (%dx%d)  tiles=%dx%d (%d)\n\n",path,W,H,TW,TH,NT);
    printf("=== Tile types ===\n");
    printf("  flat=%d  gradient=%d  edge=%d  delta=%d  noise=%d\n",tc[0],tc[1],tc[2],tc[4],tc[3]);
    printf("\n=== Noise avg_var histogram (residual after best predictor) ===\n");
    { int total_noise=tc[3]; if(total_noise<1)total_noise=1;
      for(int _h=0;_h<NOISE_HIST_N;_h++){
          int lo=200+_h*100, hi=lo+100;
          int cnt=atomic_load(&g_noise_hist[_h]);
          printf("  [%4d-%4s) : %3d tiles (%4.1f%%)\n",
                 lo, _h==NOISE_HIST_N-1?"inf":"",(int)(lo+100),
                 cnt, (double)cnt/total_noise*100.0);
      }
    }
    printf("\n=== Point 3: Circuit-based grad mode (of %d gradient tiles) ===\n", cg);
    printf("  GRAD9_N : %4d  (%5.1f%%)  n_circuits 0-2\n", cn, cg>0?(double)cn/cg*100.0:0.0);
    printf("  GRAD9_L : %4d  (%5.1f%%)  n_circuits 3-5\n", cl, cg>0?(double)cl/cg*100.0:0.0);
    printf("  DELTA   : %4d  (%5.1f%%)  n_circuits 6\n",   cd, cg>0?(double)cd/cg*100.0:0.0);
    printf("\n=== Point 2: Blueprint dedup ===\n");
    printf("  Windows    : %d  (every %d tiles)\n", g_gb_bp_count, GGT_FLUSH_PERIOD);
    printf("  Dedup hits : %d tiles  saved %d B\n", dedup_hits, dedup_saved);
    printf("  FB dedup   : %d tiles  saved %d B\n", fb_dedup_hits, fb_dedup_saved);
    printf("  Blueprints :\n");
    for(int i = 0; i < g_gb_bp_count && i < 6; i++)
        printf("    [w%02d] stamp=%08X  circuits=%02X\n",
               (int)g_gb_blueprints[i].window_id,
               g_gb_blueprints[i].stamp_hash,
               g_gb_blueprints[i].circuit_map);
    if(g_gb_bp_count > 6) printf("    ... (%d more)\n", g_gb_bp_count - 6);
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
 * GPX4 DECODE PATH (internal)
 * Uses gpx4_open() layer API — called when magic == 'GPX4'
 * ════════════════════════════════════════════════════════ */
static int decode_gpx4(const char *gpx_path, const char *out_bmp){
    Gpx4File gf;
    if(gpx4_open(gpx_path, &gf) != 0){
        fprintf(stderr,"gpx4: cannot open %s\n", gpx_path); return 1;
    }

    int TW = gf.tw, TH = gf.th, NT = (int)gf.n_tiles;
    int W = TW * TILE, H = TH * TILE;

    /* ── Read actual W,H from META layer (avoids tile-padding artifacts) ── */
    uint32_t meta_sz = 0;
    const uint8_t *meta_data = gpx4_layer_data(&gf, GPX4_LAYER_META, &meta_sz);
    if(meta_data && meta_sz > 0){
        char meta_str[256]; int msz = (int)meta_sz < 255 ? (int)meta_sz : 255;
        memcpy(meta_str, meta_data, msz); meta_str[msz] = 0;
        int mw=0, mh=0;
        if(sscanf(strstr(meta_str,"img_w=") ? strstr(meta_str,"img_w=") : "", "img_w=%d", &mw)==1 &&
           sscanf(strstr(meta_str,"img_h=") ? strstr(meta_str,"img_h=") : "", "img_h=%d", &mh)==1 &&
           mw>0 && mh>0){ W=mw; H=mh; }
    }
    int N = W * H;

    /* ── Locate O4 layer ── */
    uint32_t o4_sz = 0;
    const uint8_t *o4_data = gpx4_layer_data(&gf, GPX4_LAYER_O4, &o4_sz);
    Gpx4TileEntry *o4_tiles = gpx4_tile_table(&gf, GPX4_LAYER_O4);
    if(!o4_data || !o4_tiles){
        fprintf(stderr,"gpx4: missing O4 layer\n"); gpx4_close(&gf); return 1;
    }

    /* ── Locate FALLBACK layer (optional) ── */
    uint32_t fb_sz = 0;
    const uint8_t *fb_data = gpx4_layer_data(&gf, GPX4_LAYER_FALLBACK, &fb_sz);
    Gpx4TileEntry *fb_tiles = gpx4_tile_table(&gf, GPX4_LAYER_FALLBACK);

    /* compute total_rows from O4 tile table */
    uint32_t total_rows = 0;
    for(int i=0; i<NT; i++) total_rows += o4_tiles[i].rows;

    /* ── Decode PNG blob → O4 RGB grid ── */
    uint8_t *gpx_rgb = (uint8_t*)calloc(O4_GRID_W * (total_rows > 0 ? total_rows : 1) * 3u, 1);
    {
        FILE *mf = gv_open_memory_file(o4_data, (size_t)o4_sz);
        if(!mf){ free(gpx_rgb); gpx4_close(&gf); return 1; }
        png_structp pp = png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
        png_infop   pi = png_create_info_struct(pp);
        if(setjmp(png_jmpbuf(pp))){
            png_destroy_read_struct(&pp,&pi,NULL);
            fclose(mf); free(gpx_rgb); gpx4_close(&gf); return 1;
        }
        png_init_io(pp, mf);
        png_read_info(pp, pi);
        for(uint32_t r=0; r<total_rows; r++)
            png_read_row(pp, gpx_rgb + r * O4_GRID_W * 3, NULL);
        png_read_end(pp, pi);
        png_destroy_read_struct(&pp,&pi,NULL);
        fclose(mf);
    }

    /* ── build per-noise-tile byte offsets into fb_data ── */
    uint32_t *fb_offsets = (uint32_t*)calloc(NT, sizeof(uint32_t));
    if(fb_data && fb_tiles){
        uint32_t fb_cur = 0;
        for(int i=0; i<NT; i++){
            fb_offsets[i] = fb_cur;
            if(o4_tiles[i].rows == 0 && !(fb_tiles[i].tflags & GPX4_TILE_REF))
                fb_cur += fb_tiles[i].blob_sz;
        }
    }

    /* ── O24: load GEOA layer → geo-sorted decode order ── */
    Gpx4GeoAddr *geo_addrs = gpx4_geo_open(&gf);  /* NULL if absent */

    /* pre-compute row_start[i]: O4 grid row offset for each tile (linear order) */
    uint32_t *row_start = (uint32_t*)calloc(NT, sizeof(uint32_t));
    {
        uint32_t rc = 0;
        for(int i=0; i<NT; i++){
            row_start[i] = rc;
            rc += o4_tiles[i].rows;
        }
    }

    /* build decode order[] — indirection layer, never touches tile data */
    int *order = (int*)malloc(NT * sizeof(int));
    if(geo_addrs){
        /* geo-sort: primary=pent_id, secondary=hilbert_local */
        typedef struct { uint32_t key; int idx; } GSort;
        GSort *gs = (GSort*)malloc(NT * sizeof(GSort));
        for(int i=0; i<NT; i++){
            uint32_t pent    = GPX4_GEO_PENT(geo_addrs[i].packed);
            uint32_t hilbert = GPX4_GEO_HILBERT(geo_addrs[i].packed);
            gs[i].key = (pent << 16) | (hilbert & 0xFFFFu);
            gs[i].idx = i;
        }
        /* insertion sort — NT typically ≤ 1024, fast enough */
        for(int a=1; a<NT; a++){
            GSort tmp=gs[a]; int b=a-1;
            while(b>=0 && gs[b].key > tmp.key){ gs[b+1]=gs[b]; b--; }
            gs[b+1]=tmp;
        }
        for(int k=0; k<NT; k++) order[k]=gs[k].idx;
        free(gs);
    } else {
        /* no GEOA layer — fall back to linear order */
        for(int k=0; k<NT; k++) order[k]=k;
    }

    /* ── Reconstruct pixels in geo-sorted order ── */
    uint8_t *img_out = (uint8_t*)calloc((size_t)N * 3, 1);

    for(int k=0; k<NT; k++){
        int i  = order[k];
        uint32_t gh = o4_tiles[i].rows;
        int tx = i % TW, ty = i / TW;
        int x0 = tx*TILE, x1 = x0+TILE; if(x1>W) x1=W;
        int y0 = ty*TILE, y1 = y0+TILE; if(y1>H) y1=H;

        /* noise tile → fallback layer */
        if(gh == 0){
            if(fb_data && fb_tiles){
                if(fb_tiles[i].tflags & GPX4_TILE_REF){
                    /* dedup ref: blob_sz holds original tile index */
                    int ref_idx = (int)fb_tiles[i].blob_sz;
                    if(ref_idx >= 0 && ref_idx < NT && fb_tiles[ref_idx].blob_sz > 0 &&
                       !(fb_tiles[ref_idx].tflags & GPX4_TILE_REF)){
                        uint32_t bsz = fb_tiles[ref_idx].blob_sz;
                        if(fb_offsets[ref_idx] + bsz <= fb_sz)
                            decode_tile_blob(fb_data + fb_offsets[ref_idx], bsz, x0,y0,x1,y1, W, img_out);
                    }
                } else {
                    uint32_t bsz = fb_tiles[i].blob_sz;
                    if(fb_offsets[i] + bsz <= fb_sz)
                        decode_tile_blob(fb_data + fb_offsets[i], bsz, x0,y0,x1,y1, W, img_out);
                }
            }
            continue;
        }

        /* O4 path — uses row_start[i] not a running cursor */
        O4GridCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.grid_h = gh;
        ctx.tile_x = (uint8_t)tx;
        ctx.tile_y = (uint8_t)ty;
        uint32_t rs = row_start[i];
        for(uint32_t r=0; r<gh && r<O4_MAX_GRID_H; r++){
            const uint8_t *row_rgb = gpx_rgb + (rs + r) * O4_GRID_W * 3;
            for(uint32_t c=0; c<O4_GRID_W; c++){
                ctx.grid[r][c].r = row_rgb[c*3+0];
                ctx.grid[r][c].g = row_rgb[c*3+1];
                ctx.grid[r][c].b = row_rgb[c*3+2];
            }
        }

        uint32_t blob_sz = o4_tiles[i].blob_sz;
        uint32_t n_slots = (blob_sz + O4_CHUNK_BYTES - 1) / O4_CHUNK_BYTES;  /* FIX: was gh*27 */
        ctx.n_slots = n_slots;
        uint8_t *blob = (uint8_t*)malloc(blob_sz);
        o4_decode(&ctx, blob, n_slots, blob_sz);
        decode_tile_blob(blob, blob_sz, x0,y0,x1,y1, W, img_out);
        free(blob);
    }

    free(order);
    free(row_start);
    if(geo_addrs) free(geo_addrs);
    free(fb_offsets);
    free(gpx_rgb);
    gpx4_close(&gf);

    /* ── Write BMP ── */
    int row_bytes = W * 3, pad = (4 - row_bytes%4)%4, bmp_row = row_bytes+pad;
    int img_sz = bmp_row * H;
    FILE *fo = fopen(out_bmp, "wb");
    if(!fo){ free(img_out); return 1; }
    uint8_t bh[54]={0};
    bh[0]='B'; bh[1]='M';
    uint32_t fsize=54+img_sz;
    memcpy(bh+2,&fsize,4); bh[10]=54; bh[14]=40;
    memcpy(bh+18,&W,4); memcpy(bh+22,&H,4);
    bh[26]=1; bh[28]=24; memcpy(bh+34,&img_sz,4);
    fwrite(bh,1,54,fo);
    uint8_t *padrow = (uint8_t*)calloc(bmp_row,1);
    for(int y=H-1; y>=0; y--){
        for(int x=0; x<W; x++){
            padrow[x*3+0] = img_out[(y*W+x)*3+2];
            padrow[x*3+1] = img_out[(y*W+x)*3+1];
            padrow[x*3+2] = img_out[(y*W+x)*3+0];
        }
        fwrite(padrow, 1, bmp_row, fo);
    }
    free(padrow); fclose(fo); free(img_out);
    printf("GPX4 decode: %s → %s  (tiles=%dx%d)\n", gpx_path, out_bmp, TW, TH);
    return 0;
}

/* ════════════════════════════════════════════════════════
 * O6: DECODE GPX  (dispatcher: GPX4 or legacy GPX2/GPX3)
 * ════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════
 * O24.5: BENCHMARK — linear vs geo-sorted decode
 *   Usage: ./geopixel_v20_o24 bench file.gpx4
 *   Runs each mode N_BENCH times, reports:
 *     - wall time (ms)
 *     - "cache miss proxy": sum of |tile_idx[k] - tile_idx[k-1]| in access order
 *       (measures spatial locality — lower = better)
 * ════════════════════════════════════════════════════════ */
#define O24_BENCH_RUNS 20

static int bench_gpx4(const char *gpx_path){
    Gpx4File gf;
    if(gpx4_open(gpx_path, &gf)!=0){
        fprintf(stderr,"bench: cannot open %s\n",gpx_path); return 1;
    }
    int TW=gf.tw, TH=gf.th, NT=(int)gf.n_tiles;
    int W=TW*TILE, H=TH*TILE, N=W*H;

    uint32_t o4_sz=0;
    const uint8_t *o4_data=gpx4_layer_data(&gf,GPX4_LAYER_O4,&o4_sz);
    Gpx4TileEntry *o4_tiles=gpx4_tile_table(&gf,GPX4_LAYER_O4);
    uint32_t fb_sz=0;
    const uint8_t *fb_data=gpx4_layer_data(&gf,GPX4_LAYER_FALLBACK,&fb_sz);
    Gpx4TileEntry *fb_tiles=gpx4_tile_table(&gf,GPX4_LAYER_FALLBACK);
    if(!o4_data||!o4_tiles){fprintf(stderr,"bench: no O4\n");gpx4_close(&gf);return 1;}

    /* pre-compute row_start[] */
    uint32_t *row_start=(uint32_t*)calloc(NT,sizeof(uint32_t));
    { uint32_t rc=0; for(int i=0;i<NT;i++){row_start[i]=rc;rc+=o4_tiles[i].rows;} }

    /* pre-compute fb_offsets[] */
    uint32_t *fb_offsets=(uint32_t*)calloc(NT,sizeof(uint32_t));
    if(fb_data&&fb_tiles){
        uint32_t fc=0;
        for(int i=0;i<NT;i++){fb_offsets[i]=fc;if(o4_tiles[i].rows==0)fc+=fb_tiles[i].blob_sz;}
    }

    /* decode PNG once (same for both modes) */
    uint32_t total_rows=0;
    for(int i=0;i<NT;i++) total_rows+=o4_tiles[i].rows;
    uint8_t *gpx_rgb=(uint8_t*)calloc(O4_GRID_W*(total_rows?total_rows:1)*3u,1);
    {
        FILE *mf=gv_open_memory_file(o4_data,(size_t)o4_sz);
        if(mf){
            png_structp pp=png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
            png_infop   pi=png_create_info_struct(pp);
            if(!setjmp(png_jmpbuf(pp))){
                png_init_io(pp,mf);
                png_read_info(pp,pi);
                for(uint32_t r=0;r<total_rows;r++)
                    png_read_row(pp,gpx_rgb+r*O4_GRID_W*3,NULL);
                png_read_end(pp,pi);
            }
            png_destroy_read_struct(&pp,&pi,NULL);
            fclose(mf);
        }
    }

    /* build geo order */
    Gpx4GeoAddr *geo_addrs=gpx4_geo_open(&gf);
    int *geo_order=(int*)malloc(NT*sizeof(int));
    int *lin_order=(int*)malloc(NT*sizeof(int));
    for(int k=0;k<NT;k++) lin_order[k]=k;
    if(geo_addrs){
        typedef struct{uint32_t key;int idx;}GS;
        GS *gs=(GS*)malloc(NT*sizeof(GS));
        for(int i=0;i<NT;i++){
            gs[i].key=((GPX4_GEO_PENT(geo_addrs[i].packed))<<16)|
                       (GPX4_GEO_HILBERT(geo_addrs[i].packed)&0xFFFFu);
            gs[i].idx=i;
        }
        for(int a=1;a<NT;a++){GS t=gs[a];int b=a-1;
            while(b>=0&&gs[b].key>t.key){gs[b+1]=gs[b];b--;}gs[b+1]=t;}
        for(int k=0;k<NT;k++) geo_order[k]=gs[k].idx;
        free(gs);
    } else {
        for(int k=0;k<NT;k++) geo_order[k]=k;
        fprintf(stderr,"bench: no GEOA layer — geo order = linear\n");
    }

    /* locality proxy: sum |order[k]-order[k-1]| */
    long long lin_locality=0, geo_locality=0;
    for(int k=1;k<NT;k++){
        int d=lin_order[k]-lin_order[k-1]; if(d<0)d=-d; lin_locality+=d;
        d=geo_order[k]-geo_order[k-1];     if(d<0)d=-d; geo_locality+=d;
    }

    /* inner decode loop (reusable for both modes) */
    #define O24_DECODE_LOOP(ORD) do { \
        for(int k=0;k<NT;k++){ \
            int i=(ORD)[k]; \
            uint32_t gh=o4_tiles[i].rows; \
            int tx=i%TW,ty=i/TW; \
            int x0=tx*TILE,x1=x0+TILE;if(x1>W)x1=W; \
            int y0=ty*TILE,y1=y0+TILE;if(y1>H)y1=H; \
            if(gh==0){ \
                if(fb_data&&fb_tiles){ \
                    uint32_t bsz=fb_tiles[i].blob_sz; \
                    if(fb_offsets[i]+bsz<=fb_sz) \
                        decode_tile_blob(fb_data+fb_offsets[i],bsz,x0,y0,x1,y1,W,img_out); \
                } \
                continue; \
            } \
            O4GridCtx ctx; memset(&ctx,0,sizeof(ctx)); \
            ctx.grid_h=gh; ctx.tile_x=(uint8_t)tx; ctx.tile_y=(uint8_t)ty; \
            uint32_t rs=row_start[i]; \
            for(uint32_t r=0;r<gh&&r<O4_MAX_GRID_H;r++){ \
                const uint8_t *rr=gpx_rgb+(rs+r)*O4_GRID_W*3; \
                for(uint32_t c=0;c<O4_GRID_W;c++){ \
                    ctx.grid[r][c].r=rr[c*3]; \
                    ctx.grid[r][c].g=rr[c*3+1]; \
                    ctx.grid[r][c].b=rr[c*3+2]; \
                } \
            } \
            uint32_t bsz=o4_tiles[i].blob_sz; \
            uint32_t ns=(bsz+O4_CHUNK_BYTES-1)/O4_CHUNK_BYTES; ctx.n_slots=ns; \
            uint8_t *blob=(uint8_t*)malloc(bsz); \
            o4_decode(&ctx,blob,ns,bsz); \
            decode_tile_blob(blob,bsz,x0,y0,x1,y1,W,img_out); \
            free(blob); \
        } \
    } while(0)

    /* warm-up run */
    uint8_t *img_out=(uint8_t*)calloc((size_t)N*3,1);
    O24_DECODE_LOOP(lin_order);
    memset(img_out,0,(size_t)N*3);

    /* benchmark linear */
    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    for(int r=0;r<O24_BENCH_RUNS;r++){
        memset(img_out,0,(size_t)N*3);
        O24_DECODE_LOOP(lin_order);
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double lin_ms=((t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6)/O24_BENCH_RUNS;

    /* benchmark geo */
    clock_gettime(CLOCK_MONOTONIC,&t0);
    for(int r=0;r<O24_BENCH_RUNS;r++){
        memset(img_out,0,(size_t)N*3);
        O24_DECODE_LOOP(geo_order);
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double geo_ms=((t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6)/O24_BENCH_RUNS;

    #undef O24_DECODE_LOOP

    printf("O24.5 Benchmark  (%s)  tiles=%dx%d (%d)  runs=%d\n",
           gpx_path,TW,TH,NT,O24_BENCH_RUNS);
    printf("─────────────────────────────────────────────\n");
    printf("Mode        Time(ms)   Locality-proxy   Speedup\n");
    printf("Linear      %7.3f    %12lld        1.00x\n", lin_ms, lin_locality);
    printf("Geo-sorted  %7.3f    %12lld        %.2fx\n", geo_ms, geo_locality,
           lin_ms/geo_ms);
    printf("─────────────────────────────────────────────\n");
    printf("Locality gain: %.1fx fewer index jumps\n",
           geo_locality>0?(double)lin_locality/geo_locality:0.0);

    free(img_out); free(geo_order); free(lin_order);
    if(geo_addrs) free(geo_addrs);
    free(row_start); free(fb_offsets); free(gpx_rgb);
    gpx4_close(&gf);
    return 0;
}

static int decode_gpx(const char *gpx_path, const char *out_bmp){
    /* ── peek magic ── */
    FILE *fm = fopen(gpx_path, "rb");
    if(!fm){ fprintf(stderr,"gpx: cannot open %s\n", gpx_path); return 1; }
    uint8_t mag[4]={0}; fread(mag,1,4,fm); fclose(fm);

    /* GPX4 multi-layer container */
    if(mag[0]=='G'&&mag[1]=='P'&&mag[2]=='X'&&mag[3]=='4')
        return decode_gpx4(gpx_path, out_bmp);

    /* ── legacy GPX2/GPX3 path ── */
    FILE *f = fopen(gpx_path, "rb");
    if(!f) return 1;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    uint8_t *raw = (uint8_t*)malloc((size_t)fsz);
    if((long)fread(raw,1,(size_t)fsz,f)!=fsz){ fclose(f); free(raw); return 1; }
    fclose(f);

    /* ── 2. Parse 16B header ── */
    int is_gpx3 = (raw[0]=='G'&&raw[1]=='P'&&raw[2]=='X'&&raw[3]=='3');
    if(fsz < 16 || (!is_gpx3 && !(raw[0]=='G'&&raw[1]=='P'&&raw[2]=='X'&&raw[3]=='2'))){
        fprintf(stderr,"gpx: bad magic\n"); free(raw); return 1;
    }
    int TW = (raw[4]<<8)|raw[5];
    int TH = (raw[6]<<8)|raw[7];
    int NT = (raw[8]<<24)|(raw[9]<<16)|(raw[10]<<8)|raw[11];
    uint32_t total_rows = (raw[12]<<24)|(raw[13]<<16)|(raw[14]<<8)|raw[15];
    int W = TW * TILE, H = TH * TILE;
    int N = W * H;

    /* ── 3. Read per-tile table (6B each: 2B rows + 4B blob_sz) ── */
    uint16_t *tile_rows = (uint16_t*)malloc(NT * sizeof(uint16_t));
    uint32_t *tile_bsz  = (uint32_t*)malloc(NT * sizeof(uint32_t));
    for(int i=0; i<NT; i++){
        int base = 16 + i*6;
        tile_rows[i] = (uint16_t)((raw[base]<<8)|raw[base+1]);
        tile_bsz[i]  = ((uint32_t)raw[base+2]<<24)|((uint32_t)raw[base+3]<<16)
                       |((uint32_t)raw[base+4]<<8)|(uint32_t)raw[base+5];
    }
    long png_offset = 16 + NT * 6;

    /* ── GPX3: locate fallback section (after PNG) ── */
    /* We'll find png end after decoding; pre-scan fallback blobs by tile order */
    /* Fallback offsets computed lazily from tile_rows==0 + tile_bsz */
    /* PNG end = we must decode PNG first, then fb section follows */
    /* Strategy: track fb_section_start after PNG decode via portable temp-file EOF pos */

    /* ── 4. PNG decode from offset into RGB buffer ── */
    uint8_t *gpx_rgb = (uint8_t*)malloc(O4_GRID_W * (total_rows > 0 ? total_rows : 1) * 3u);
    long png_end_pos = 0;
    {
        /* wrap raw+png_offset in FILE* via temp file so libpng can read it portably */
        FILE *mf = gv_open_memory_file(raw + png_offset, (size_t)(fsz - png_offset));
        png_structp pp = png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
        png_infop   pi = png_create_info_struct(pp);
        if(setjmp(png_jmpbuf(pp))){
            png_destroy_read_struct(&pp,&pi,NULL);
            fclose(mf); free(gpx_rgb); free(tile_rows); free(raw); return 1;
        }
        png_init_io(pp, mf);
        png_read_info(pp, pi);
        for(uint32_t r=0; r<total_rows; r++)
            png_read_row(pp, gpx_rgb + r * O4_GRID_W * 3, NULL);
        png_read_end(pp, pi);
        png_destroy_read_struct(&pp,&pi,NULL);
        png_end_pos = png_offset + ftell(mf);
        fclose(mf);
    }

    /* ── GPX3: load fallback section into memory ── */
    uint8_t *fb_data = NULL;
    uint32_t fb_data_sz = 0;
    if(is_gpx3 && png_end_pos + 4 <= fsz){
        const uint8_t *p = raw + png_end_pos;
        fb_data_sz = ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
        if(fb_data_sz > 0 && (long)(png_end_pos + 4 + (long)fb_data_sz) <= fsz){
            fb_data = (uint8_t*)malloc(fb_data_sz);
            if(fb_data) memcpy(fb_data, raw + png_end_pos + 4, fb_data_sz);
        }
    }
    /* build per-noise-tile offsets into fb_data */
    uint32_t *fb_offsets = (uint32_t*)calloc(NT, sizeof(uint32_t));
    {
        uint32_t fb_cur = 0;
        for(int i=0; i<NT; i++){
            fb_offsets[i] = fb_cur;
            if(tile_rows[i] == 0) fb_cur += tile_bsz[i];
        }
    }

    free(raw);

    /* ── 5. Reconstruct image pixel buffer ── */
    uint8_t *img_out = (uint8_t*)calloc((size_t)N * 3, 1);
    uint32_t row_cursor = 0;
    int rt_fails = 0;

    for(int i=0; i<NT; i++){
        uint32_t gh = tile_rows[i];
        int tx = i % TW, ty = i / TW;
        int x0 = tx*TILE, x1 = x0+TILE; if(x1>W) x1=W;
        int y0 = ty*TILE, y1 = y0+TILE; if(y1>H) y1=H;
        int tw = x1-x0, th = y1-y0;
        (void)tw; (void)th;

        /* GPX3 fallback: tile_rows==0 → noise blob from fallback section */
        if(gh == 0){
            uint32_t blob_sz = tile_bsz[i];
            if(fb_data && fb_offsets[i] + blob_sz <= fb_data_sz){
                decode_tile_blob(fb_data + fb_offsets[i], blob_sz, x0,y0,x1,y1, W, img_out);
            }
            continue;
        }

        /* rebuild O4GridCtx from RGB rows */
        O4GridCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.grid_h = gh;
        ctx.tile_x = (uint8_t)tx;
        ctx.tile_y = (uint8_t)ty;
        for(uint32_t r=0; r<gh && r<O4_MAX_GRID_H; r++){
            const uint8_t *row_rgb = gpx_rgb + (row_cursor + r) * O4_GRID_W * 3;
            for(uint32_t c=0; c<O4_GRID_W; c++){
                ctx.grid[r][c].r = row_rgb[c*3+0];
                ctx.grid[r][c].g = row_rgb[c*3+1];
                ctx.grid[r][c].b = row_rgb[c*3+2];
            }
        }
        row_cursor += gh;

        /* o4_decode → blob bytes using actual blob_sz from table */
        uint32_t n_slots = gh * O4_GRID_W;
        ctx.n_slots = n_slots;
        uint32_t blob_sz = tile_bsz[i];
        uint8_t *blob = (uint8_t*)malloc(blob_sz);
        o4_decode(&ctx, blob, n_slots, blob_sz);

        /* decode_tile_blob → pixels into img_out */
        decode_tile_blob(blob, blob_sz, x0,y0,x1,y1, W, img_out);
        free(blob);
    }

    free(fb_offsets);
    free(fb_data);
    free(tile_rows);
    free(tile_bsz);
    free(gpx_rgb);

    /* ── 6. Write output BMP ── */
    int row_bytes = W * 3;
    int pad = (4 - row_bytes % 4) % 4;
    int bmp_row = row_bytes + pad;
    int img_sz = bmp_row * H;
    FILE *fo = fopen(out_bmp, "wb");
    if(!fo){ free(img_out); return 1; }
    /* BMP header */
    uint8_t bh[54]={0};
    bh[0]='B'; bh[1]='M';
    uint32_t fsize=54+img_sz;
    memcpy(bh+2,&fsize,4); bh[10]=54;
    bh[14]=40; memcpy(bh+18,&W,4); memcpy(bh+22,&H,4);
    bh[26]=1; bh[28]=24; memcpy(bh+34,&img_sz,4);
    fwrite(bh,1,54,fo);
    uint8_t *padrow = (uint8_t*)calloc(bmp_row,1);
    for(int y=H-1; y>=0; y--){
        for(int x=0; x<W; x++){
            /* img_out is YCbCr-ordered — store as BGR for BMP */
            padrow[x*3+0] = img_out[(y*W+x)*3+2];
            padrow[x*3+1] = img_out[(y*W+x)*3+1];
            padrow[x*3+2] = img_out[(y*W+x)*3+0];
        }
        fwrite(padrow, 1, bmp_row, fo);
    }
    free(padrow);
    fclose(fo);
    free(img_out);

    printf("GPX decode: %s → %s  (tiles=%dx%d)\n", gpx_path, out_bmp, TW, TH);
    printf("  rt_fails: %d\n", rt_fails);
    return 0;
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

typedef struct {
    const char *prefix;
    int width;
    int height;
} AnimDumpCtx;

static int dump_anim_frame(int frame_idx, uint8_t *rgb, void *ud)
{
    AnimDumpCtx *ctx = (AnimDumpCtx*)ud;
    char out[512];
    snprintf(out, sizeof(out), "%s_%04d.bmp", ctx->prefix, frame_idx);
    bmp_save(out, rgb, ctx->width, ctx->height);
    printf("  frame %d -> %s\n", frame_idx, out);
    return 0;
}

/* ════════════════════════════════════════════════════════
 * ANIMATED GPX4 EXPORT
 * ════════════════════════════════════════════════════════ */
static int encode_anim(int argc, char **argv)
{
    if(argc < 5){
        fprintf(stderr,
            "Usage: %s anim out.gpx4 frame0.bmp frame1.bmp [...]\n",
            argv[0]);
        return 1;
    }

    const char *out_path = argv[2];
    int n_frames = argc - 3;
    uint8_t **frames = (uint8_t**)calloc((size_t)n_frames, sizeof(uint8_t*));
    int W = 0, H = 0;
    int ret = 1;

    for(int i = 0; i < n_frames; i++){
        Img img = {0};
        if(!bmp_load(argv[i + 3], &img)){
            fprintf(stderr, "anim: cannot load %s\n", argv[i + 3]);
            goto done;
        }
        if(i == 0){
            W = img.w;
            H = img.h;
        } else if(img.w != W || img.h != H){
            fprintf(stderr, "anim: size mismatch in %s\n", argv[i + 3]);
            free(img.px);
            goto done;
        }
        frames[i] = img.px;
    }

    GpxAnimEncCfg cfg = {24, 1, 16, 9, TILE};
    ret = gpx_anim_encode(frames, n_frames, W, H, &cfg, out_path);
    if(ret == 0){
        printf("Animated GPX4 saved: %s  (frames=%d)\n", out_path, n_frames);
    }

done:
    for(int i = 0; i < n_frames; i++) free(frames[i]);
    free(frames);
    return ret;
}

static int decode_anim(const char *path, const char *prefix)
{
    Gpx4AnimHdr hdr;
    if(gpx_anim_info(path, &hdr) != 0){
        fprintf(stderr, "anim: cannot read header %s\n", path);
        return 1;
    }

    printf("Animated GPX4\n");
    printf("  frames : %u\n", hdr.n_frames);
    printf("  fps    : %u/%u\n", hdr.fps_num, hdr.fps_den);
    printf("  kfi    : %u\n", hdr.keyframe_interval);
    printf("  size   : %ux%u\n", hdr.width_px, hdr.height_px);

    AnimDumpCtx ctx = { prefix, hdr.width_px, hdr.height_px };
    return gpx_anim_decode(path, dump_anim_frame, &ctx);
}

/* ── main ─────────────────────────────────────────────────*/
int main(int argc,char**argv){
    if(argc<2){
        fprintf(stderr,
            "GeoPixel v20\n"
            "  encode : %s input.bmp          -> .gp15 + .gpx4\n"
            "  anim   : %s anim out.gpx4 frames...\n"
            "  anim   : %s animdecode in.gpx4 out_prefix\n"
            "  anim   : %s animinfo in.gpx4\n"
            "  decode : %s file.gpx4 out.bmp  -> GPX4 or legacy GPX2/3\n"
            "  decode : %s file.gp15\n"
            "  query  : %s file.gp15 tx ty\n",
            argv[0],argv[0],argv[0],argv[0],argv[0],argv[0],argv[0]);
        return 1;
    }
    clock_t t0=clock();
    int ret;
    if(strcmp(argv[1], "anim") == 0){
        ret = encode_anim(argc, argv);
    } else if(strcmp(argv[1], "animdecode") == 0 && argc >= 4){
        ret = decode_anim(argv[2], argv[3]);
    } else if(strcmp(argv[1], "animinfo") == 0 && argc >= 3){
        Gpx4AnimHdr hdr;
        ret = gpx_anim_info(argv[2], &hdr);
        if(ret == 0){
            printf("frames=%u fps=%u/%u kfi=%u size=%ux%u\n",
                hdr.n_frames, hdr.fps_num, hdr.fps_den,
                hdr.keyframe_interval, hdr.width_px, hdr.height_px);
        }
    } else if(strcmp(argv[1],"bench")==0 && argc>=3){
        ret=bench_gpx4(argv[2]);
    } else if(strstr(argv[1],".gpx") && argc>=3){
        ret=decode_gpx(argv[1], argv[2]);
    } else if(strstr(argv[1],".gp15")){
        ret=(argc==4)?query_tile(argv[1],atoi(argv[2]),atoi(argv[3])):decode_full(argv[1]);
    } else {
        ret=encode(argv[1]);
    }
    printf("  Time   : %.2f ms\n",(double)(clock()-t0)/CLOCKS_PER_SEC*1000.0);
    return ret;
}
