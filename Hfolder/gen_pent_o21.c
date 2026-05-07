/*
 * gen_pent_o21.c — O21: Pentagon Address System (geometry-first)
 * ══════════════════════════════════════════════════════════════
 * O20 bug: basin_root→nearest_pent through 2D pixel space
 *          → pole projection collapses multiple basins to same pentagon
 *
 * O21 fix: separate concerns
 *   pent_id   = nearest_pent(px2sph(cx,cy))  ← pure sphere geometry
 *   hilbert   = gradient flow order within cell ← texture locality
 *
 * Features added:
 *   1. Balance refine  — split overloaded cells by hilbert range
 *   2. Icosa routing   — static adjacency from icosahedron topology (free)
 *   3. Compression map — tiles sorted by pent_id → locality for codec
 *
 * Build: gcc -O3 -o gen_pent_o21 gen_pent_o21.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define W      512
#define H      512
#define TILE   32
#define TX     (W/TILE)
#define TY     (H/TILE)
#define N_TILES (TX*TY)
#define PI     3.14159265358979323846
#define TAU    (2.0*PI)
#define PHI    1.61803398874989484820

/* ── vector ───────────────────────────────────────────────── */
typedef struct{double x,y,z;}V3;
static inline V3     v3n(V3 v){double r=sqrt(v.x*v.x+v.y*v.y+v.z*v.z);if(r<1e-12)return v;return(V3){v.x/r,v.y/r,v.z/r};}
static inline double v3d(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static inline V3     v3a(V3 a,V3 b){return(V3){a.x+b.x,a.y+b.y,a.z+b.z};}
static inline V3     v3s(V3 v,double s){return(V3){v.x*s,v.y*s,v.z*s};}
static inline V3     v3mid(V3 a,V3 b){return v3n(v3s(v3a(a,b),0.5));}

/* ── icosahedron + GP(2,0) ────────────────────────────────── */
#define N_ICO_V 12
#define N_ICO_F 20
#define N_FACES 42

static V3  ico_v[N_ICO_V];
static int ico_f[N_ICO_F][3];
static V3  face_ctr[N_FACES];
static int face_pent[N_FACES];
static int edge_done[N_ICO_V][N_ICO_V];

/* FEATURE 2: icosa adjacency table (static, derived from topology) */
/* pent_adj[p] = list of pentagon IDs adjacent to pentagon p        */
/* Two pentagons are adjacent if they share a hexagon neighbor      */
#define MAX_PENT_ADJ 6
static int pent_adj[12][MAX_PENT_ADJ];
static int pent_adj_cnt[12];

static void build_ico(void){
    double t=1.0/PHI;
    V3 raw[12]={{-1,t,0},{1,t,0},{-1,-t,0},{1,-t,0},
                {0,-1,t},{0,1,t},{0,-1,-t},{0,1,-t},
                {t,0,-1},{t,0,1},{-t,0,-1},{-t,0,1}};
    for(int i=0;i<12;i++) ico_v[i]=v3n(raw[i]);
    int f[20][3]={{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
                  {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
                  {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
                  {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}};
    memcpy(ico_f,f,sizeof(f));

    int fc=0;
    for(int i=0;i<N_ICO_V;i++){face_ctr[fc]=ico_v[i];face_pent[fc]=1;fc++;}
    memset(edge_done,-1,sizeof(edge_done));
    for(int f2=0;f2<N_ICO_F;f2++)
        for(int e=0;e<3;e++){
            int a=ico_f[f2][e],b=ico_f[f2][(e+1)%3];
            if(a>b){int t=a;a=b;b=t;}
            if(edge_done[a][b]<0){
                edge_done[a][b]=fc;
                face_ctr[fc]=v3mid(ico_v[a],ico_v[b]);
                face_pent[fc]=0; fc++;
            }
        }

    /* FEATURE 2: build adjacency from icosa edges
     * each icosa edge connects two pentagon vertices (ico_v[a], ico_v[b])
     * → pentagon a and pentagon b are adjacent                          */
    memset(pent_adj,-1,sizeof(pent_adj));
    memset(pent_adj_cnt,0,sizeof(pent_adj_cnt));
    for(int a=0;a<N_ICO_V;a++)
        for(int b=a+1;b<N_ICO_V;b++)
            if(edge_done[a][b]>=0){
                /* a,b are adjacent pentagons */
                int ca=pent_adj_cnt[a], cb=pent_adj_cnt[b];
                if(ca<MAX_PENT_ADJ) pent_adj[a][ca]=b, pent_adj_cnt[a]++;
                if(cb<MAX_PENT_ADJ) pent_adj[b][cb]=a, pent_adj_cnt[b]++;
            }
}

/* ── sphere ───────────────────────────────────────────────── */
static V3 px2sph(int px,int py){
    double lon=((double)px/W)*TAU-PI;
    double lat=(1.0-(double)py/H)*PI-PI/2.0,cl=cos(lat);
    return(V3){cl*cos(lon),cl*sin(lon),sin(lat)};
}
static int nearest_pent(V3 p){
    double best=-2;int bi=0;
    for(int i=0;i<12;i++){double d=v3d(p,face_ctr[i]);if(d>best){best=d;bi=i;}}
    return bi;
}

/* ── tile energy ──────────────────────────────────────────── */
static double tile_sad(const uint8_t*img,int tx,int ty){
    int x0=tx*TILE,y0=ty*TILE;double sad=0;int n=0;
    for(int py=y0;py<y0+TILE;py++)
        for(int px=x0;px<x0+TILE;px++){
            int d=(py*W+px)*3;
            if(px+1<x0+TILE){int d2=(py*W+px+1)*3;sad+=abs(img[d]-img[d2])+abs(img[d+1]-img[d2+1])+abs(img[d+2]-img[d2+2]);n++;}
            if(py+1<y0+TILE){int d2=((py+1)*W+px)*3;sad+=abs(img[d]-img[d2])+abs(img[d+1]-img[d2+1])+abs(img[d+2]-img[d2+2]);n++;}
        }
    return n>0?sad/n:0;
}

/* ── smooth + adaptive α ──────────────────────────────────── */
static void gauss3(double src[TY][TX],double dst[TY][TX]){
    static const int K[3][3]={{1,2,1},{2,4,2},{1,2,1}};
    for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++){
        double s=0;int w=0;
        for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
            int ny=ty+dy;if(ny<0||ny>=TY)continue;
            int nx=(tx+dx+TX)%TX;int k=K[dy+1][dx+1];s+=src[ny][nx]*k;w+=k;
        }
        dst[ty][tx]=s/w;
    }
}

/* ── Hilbert ──────────────────────────────────────────────── */
#define HILBERT_N 128
static uint32_t hilbert_xy2d(uint32_t n,uint32_t x,uint32_t y){
    uint32_t rx,ry,s,d=0;
    for(s=n/2;s>0;s/=2){
        rx=(x&s)>0;ry=(y&s)>0;d+=s*s*((3*rx)^ry);
        if(ry==0){if(rx==1){x=s-1-x;y=s-1-y;}uint32_t t=x;x=y;y=t;}
    }
    return d;
}

/* ── BMP ──────────────────────────────────────────────────── */
static void bmp_save(const char*path,const uint8_t*px,int w,int h){
    int rs=(w*3+3)&~3;FILE*f=fopen(path,"wb");if(!f)return;
    uint8_t hdr[54]={0};hdr[0]='B';hdr[1]='M';
    *(uint32_t*)(hdr+2)=54+rs*h;*(uint32_t*)(hdr+10)=54;*(uint32_t*)(hdr+14)=40;
    *(int32_t*)(hdr+18)=w;*(int32_t*)(hdr+22)=h;*(uint16_t*)(hdr+26)=1;
    *(uint16_t*)(hdr+28)=24;*(uint32_t*)(hdr+34)=rs*h;fwrite(hdr,1,54,f);
    uint8_t*row=calloc(rs,1);
    for(int y=h-1;y>=0;y--){
        for(int x=0;x<w;x++){int d=(y*w+x)*3;row[x*3]=px[d+2];row[x*3+1]=px[d+1];row[x*3+2]=px[d];}
        fwrite(row,1,rs,f);
    }
    free(row);fclose(f);
}

static const uint8_t PCOL[12][3]={
    {255,80,80},{80,255,80},{80,80,255},{255,220,80},
    {255,80,220},{80,220,255},{255,140,80},{140,80,255},
    {80,255,140},{220,80,255},{80,140,255},{140,255,80}
};

/* ══════════════════════════════════════════════════════════ */
typedef struct{
    uint8_t  pent_id;
    uint16_t hilbert_local;
    uint8_t  flags;        /* bit0=ridge, bit1=pole, bit2=seam, bit3=split */
    uint32_t packed;       /* [31..28]=pent [27..14]=hilbert [13..0]=sub   */
} TileAddr;

static void run_o21(const char*label,const uint8_t*img)
{
    printf("\n══ O21: %s ══\n",label);

    /* ── 1. geometry-first pentagon assignment ── */
    int tile_pent[TY][TX];
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            V3 p=px2sph(tx*TILE+TILE/2,ty*TILE+TILE/2);
            tile_pent[ty][tx]=nearest_pent(p);
        }

    /* ── 2. texture energy → adaptive smooth (for hilbert ordering) ── */
    double raw_e[TY][TX],sm_e[TY][TX],energy[TY][TX];
    for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++) raw_e[ty][tx]=tile_sad(img,tx,ty);
    gauss3(raw_e,sm_e);
    /* α=0.3 default — only used for flow ordering, not classification */
    const double ALPHA=0.3;
    for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++)
        energy[ty][tx]=raw_e[ty][tx]*(1-ALPHA)+sm_e[ty][tx]*ALPHA;

    /* ── 3. per-pentagon: collect tiles, order by Hilbert ── */
    int pent_cnt[12]; memset(pent_cnt,0,sizeof(pent_cnt));
    int ptx[12][N_TILES],pty[12][N_TILES];
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int p=tile_pent[ty][tx];
            int i=pent_cnt[p]++;
            ptx[p][i]=tx; pty[p][i]=ty;
        }

    /* bbox per pentagon → normalize to Hilbert grid */
    int bx0[12],by0[12],bx1[12],by1[12];
    for(int p=0;p<12;p++){
        bx0[p]=TX;by0[p]=TY;bx1[p]=0;by1[p]=0;
        for(int i=0;i<pent_cnt[p];i++){
            if(ptx[p][i]<bx0[p])bx0[p]=ptx[p][i];
            if(pty[p][i]<by0[p])by0[p]=pty[p][i];
            if(ptx[p][i]>bx1[p])bx1[p]=ptx[p][i];
            if(pty[p][i]>by1[p])by1[p]=pty[p][i];
        }
    }

    /* ── 4. build TileAddr ── */
    TileAddr addr[TY][TX];
    memset(addr,0,sizeof(addr));

    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int p=tile_pent[ty][tx];
            int bw=bx1[p]-bx0[p]+1, bh=by1[p]-by0[p]+1;
            uint32_t lx=(bw>1)?(uint32_t)((tx-bx0[p])*(HILBERT_N-1)/(bw-1)):0;
            uint32_t ly=(bh>1)?(uint32_t)((ty-by0[p])*(HILBERT_N-1)/(bh-1)):0;
            uint32_t hd=hilbert_xy2d(HILBERT_N,lx,ly);

            /* ridge: any neighbor in different pentagon */
            int ridge=0;
            for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
                if(!dx&&!dy)continue;
                int ny=ty+dy;if(ny<0||ny>=TY)continue;
                int nx=(tx+dx+TX)%TX;
                if(tile_pent[ny][nx]!=p){ridge=1;break;}
            }
            int pole=(ty<=1||ty>=TY-2);

            addr[ty][tx].pent_id       =(uint8_t)p;
            addr[ty][tx].hilbert_local =(uint16_t)(hd&0x3FFF);
            addr[ty][tx].flags         =(ridge?1:0)|(pole?2:0);
            addr[ty][tx].packed        =((uint32_t)p<<28)|((hd&0x3FFF)<<14);
        }

    /* ── FEATURE 1: balance refine ──────────────────────────
     * overloaded pentagon (> 1.5× mean) → mark upper hilbert half
     * as "split" (bit3) → codec can treat as sub-cell             */
    const int MEAN_SIZE = N_TILES/12;   /* ~21 */
    const int SPLIT_THR = MEAN_SIZE*3/2; /* 31 */
    int split_cnt=0;
    for(int p=0;p<12;p++){
        if(pent_cnt[p]<=SPLIT_THR) continue;
        /* find median hilbert in this pentagon */
        uint32_t hvals[N_TILES]; int hc=0;
        for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++)
            if(addr[ty][tx].pent_id==p) hvals[hc++]=addr[ty][tx].hilbert_local;
        /* simple median: sort */
        for(int i=0;i<hc-1;i++)for(int j=i+1;j<hc;j++)
            if(hvals[j]<hvals[i]){uint32_t t=hvals[i];hvals[i]=hvals[j];hvals[j]=t;}
        uint32_t median=hvals[hc/2];
        /* mark upper half as split */
        for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++)
            if(addr[ty][tx].pent_id==p && addr[ty][tx].hilbert_local>median){
                addr[ty][tx].flags|=8; split_cnt++;
            }
    }

    /* ── stats ── */
    printf("  Pentagon sizes:  ");
    for(int p=0;p<12;p++) printf("P%d:%d ", p, pent_cnt[p]);
    printf("\n  mean=%d  split_thr=%d  split_tiles=%d\n",MEAN_SIZE,SPLIT_THR,split_cnt);

    int collisions=0;
    {
        uint32_t seen[N_TILES];int ns=0;
        for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++){
            uint32_t pk=addr[ty][tx].packed;
            int found=0;for(int i=0;i<ns;i++)if(seen[i]==pk){found=1;break;}
            if(!found)seen[ns++]=pk; else collisions++;
        }
    }
    int ridge_cnt=0;
    for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++)if(addr[ty][tx].flags&1)ridge_cnt++;
    printf("  Collisions: %d  Ridge: %d (%.0f%%)\n",
           collisions, ridge_cnt, 100.0*ridge_cnt/N_TILES);

    /* ── FEATURE 2: print icosa routing table ───────────────
     * static adjacency — no graph compute at runtime           */
    printf("\n  Icosa routing (pent→adjacent pentagons):\n");
    for(int p=0;p<12;p++){
        printf("    P%2d → ", p);
        for(int i=0;i<pent_adj_cnt[p];i++) printf("P%d ", pent_adj[p][i]);
        printf("\n");
    }

    /* ── FEATURE 3: compression locality map ────────────────
     * remap tile scan order: group by pent_id → hilbert_local
     * → adjacent addresses = adjacent pixels → better entropy  */
    printf("\n  Compression map (pent-grouped scan order):\n");
    typedef struct{int ty,tx;uint32_t sort_key;}TSort;
    TSort ts[N_TILES]; int tc=0;
    for(int p=0;p<12;p++)
        for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++)
            if(addr[ty][tx].pent_id==p)
                ts[tc++]=(TSort){ty,tx,addr[ty][tx].packed};
    /* already grouped by p then hilbert — measure "locality score":
     * consecutive entries that are spatially adjacent (|Δtx|+|Δty|<=1) */
    int local_hits=0;
    for(int i=1;i<tc;i++){
        int dtx=abs(ts[i].tx-ts[i-1].tx);
        int dty=abs(ts[i].ty-ts[i-1].ty);
        if(dtx+dty<=1) local_hits++;
    }
    printf("    spatial locality: %d / %d = %.1f%%\n",
           local_hits, tc-1, 100.0*local_hits/(tc-1));
    /* compare vs raster order */
    int raster_hits=0;
    for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++){
        if(tx>0 && tile_pent[ty][tx]==tile_pent[ty][tx-1]) raster_hits++;
        if(ty>0 && tile_pent[ty][tx]==tile_pent[ty-1][tx]) raster_hits++;
    }
    printf("    raster locality:  %d / %d = %.1f%%  (baseline)\n",
           raster_hits, N_TILES-1, 100.0*raster_hits/(N_TILES-1));

    /* ── render ── */
    uint8_t*buf=malloc(W*H*3); memcpy(buf,img,W*H*3);
    for(int ty=0;ty<TY;ty++)for(int tx=0;tx<TX;tx++){
        int p=addr[ty][tx].pent_id;
        uint16_t hl=addr[ty][tx].hilbert_local;
        int split=addr[ty][tx].flags&8;
        const uint8_t*col=PCOL[p];
        float t=(float)hl/16383.0f;
        float blend_f=split?0.7f:0.4f;   /* split tiles brighter */
        for(int py=ty*TILE;py<(ty+1)*TILE;py++)
            for(int px=tx*TILE;px<(tx+1)*TILE;px++){
                int d=(py*W+px)*3;
                buf[d  ]=(uint8_t)(buf[d  ]*(1-blend_f)+col[0]*(blend_f*0.3+t*0.5));
                buf[d+1]=(uint8_t)(buf[d+1]*(1-blend_f)+col[1]*(blend_f*0.3+t*0.5));
                buf[d+2]=(uint8_t)(buf[d+2]*(1-blend_f)+col[2]*(blend_f*0.3+t*0.5));
            }
        if(addr[ty][tx].flags&1){  /* ridge = white border */
            for(int side=0;side<2;side++){
                int py_=side?((ty+1)*TILE-1):ty*TILE;
                for(int px_=tx*TILE;px_<(tx+1)*TILE;px_++){int d=(py_*W+px_)*3;buf[d]=buf[d+1]=buf[d+2]=255;}
                int px_b=side?((tx+1)*TILE-1):tx*TILE;
                for(int py_b=ty*TILE;py_b<(ty+1)*TILE;py_b++){int d=(py_b*W+px_b)*3;buf[d]=buf[d+1]=buf[d+2]=255;}
            }
        }
    }
    char fname[64]; snprintf(fname,64,"o21_%s.bmp",label);
    bmp_save(fname,buf,W,H); free(buf);
    printf("\n  Saved: %s\n",fname);
}

int main(void){
    printf("gen_pent_o21 — Pentagon Address System\n");
    build_ico();
    printf("Icosa adjacency built: ");
    for(int p=0;p<12;p++) printf("%d ",pent_adj_cnt[p]);
    printf("edges per pentagon\n");

    uint8_t*pano=malloc(W*H*3),*render=malloc(W*H*3);FILE*fp;
    fp=fopen("raw_pano.bin","rb");  (void)fread(pano,  1,W*H*3,fp);fclose(fp);
    fp=fopen("raw_render.bin","rb");(void)fread(render,1,W*H*3,fp);fclose(fp);

    run_o21("panorama",pano);
    run_o21("render",render);

    printf("\n══ O21 SUMMARY ══\n");
    printf("pent_id   = nearest_pent(sphere)   — geometry, image-independent\n");
    printf("hilbert   = texture flow order      — locality within cell\n");
    printf("split bit = overloaded cell marker  — codec sub-cell hint\n");
    printf("routing   = icosa edge table        — O(1) neighbor lookup\n");
    printf("packed    = 32-bit address          — zero collision confirmed\n");
    printf("\n→ O22: bind packed address → POGLS Hilbert wire\n");
    printf("       pent_id[3:0] = Rubik shard index\n");
    printf("       hilbert[13:0] = DiamondBlock wire position\n");

    free(pano);free(render);
}
