/*
 * gen_pent_o19.c — O19: Pentagon Address Encoder
 * ════════════════════════════════════════════════
 * O18 output: 12 basins from gradient flow
 * O19: encode each tile as (pentagon_id, hilbert_local) → 4-byte address
 *
 * Address layout (32-bit):
 *   [31..28]  pentagon_id   4 bits  → 0-11  (12 faces)
 *   [27..14]  hilbert_index 14 bits → 0-16383 (local order within basin)
 *   [13..0]   reserved / sub-pixel  (future: pixel offset inside tile)
 *
 * Hilbert local: tiles within each basin are ordered by 2D Hilbert curve
 *   - preserves spatial locality inside each pentagon cell
 *   - basin bbox → normalized (lx,ly) → hilbert_d
 *
 * Build: gcc -O3 -o gen_pent_o19 gen_pent_o19.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define W     512
#define H     512
#define TILE  32
#define TX    (W/TILE)
#define TY    (H/TILE)
#define N_TILES (TX*TY)
#define PI    3.14159265358979323846
#define TAU   (2.0*PI)
#define PHI   1.61803398874989484820

/* ── Hilbert curve (2D → 1D, order N=128 → 14-bit index) ── */
/* standard xy2d for NxN grid */
static uint32_t hilbert_xy2d(uint32_t n, uint32_t x, uint32_t y){
    uint32_t rx,ry,s,d=0;
    for(s=n/2;s>0;s/=2){
        rx=(x&s)>0; ry=(y&s)>0;
        d+=s*s*((3*rx)^ry);
        /* rotate */
        if(ry==0){
            if(rx==1){x=s-1-x;y=s-1-y;}
            uint32_t t=x;x=y;y=t;
        }
    }
    return d;
}

/* ── sphere geometry (same as O18) ──────────────────────── */
typedef struct { double x,y,z; } V3;
static inline V3     v3n(V3 v){ double r=sqrt(v.x*v.x+v.y*v.y+v.z*v.z); if(r<1e-12)return v; return (V3){v.x/r,v.y/r,v.z/r}; }
static inline double v3d(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline V3     v3a(V3 a,V3 b){ return (V3){a.x+b.x,a.y+b.y,a.z+b.z}; }
static inline V3     v3s(V3 v,double s){ return (V3){v.x*s,v.y*s,v.z*s}; }
static inline V3     v3mid(V3 a,V3 b){ return v3n(v3s(v3a(a,b),0.5)); }

#define N_ICO_V 12
#define N_ICO_F 20
#define N_FACES 42
static V3  ico_v[N_ICO_V];
static int ico_f[N_ICO_F][3];
static V3  face_ctr[N_FACES];
static int face_pent[N_FACES];
static int edge_done[N_ICO_V][N_ICO_V];

static void build_icosahedron(void){
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
}
static void build_gp20(void){
    int fc=0;
    for(int i=0;i<N_ICO_V;i++){ face_ctr[fc]=ico_v[i]; face_pent[fc]=1; fc++; }
    memset(edge_done,-1,sizeof(edge_done));
    for(int f=0;f<N_ICO_F;f++)
        for(int e=0;e<3;e++){
            int a=ico_f[f][e], b=ico_f[f][(e+1)%3];
            if(a>b){ int t=a;a=b;b=t; }
            if(edge_done[a][b]<0){
                edge_done[a][b]=fc;
                face_ctr[fc]=v3mid(ico_v[a],ico_v[b]);
                face_pent[fc]=0; fc++;
            }
        }
}
static V3 px2sph(int px, int py){
    double lon=((double)px/W)*TAU-PI;
    double lat=(1.0-(double)py/H)*PI-PI/2.0;
    double cl=cos(lat);
    return (V3){cl*cos(lon),cl*sin(lon),sin(lat)};
}
static int nearest_pent(V3 p){
    double best=-2.0; int bi=0;
    for(int i=0;i<12;i++){
        double d=v3d(p,face_ctr[i]);
        if(d>best){best=d;bi=i;}
    }
    return bi;
}

/* ── blob energy ─────────────────────────────────────────── */
static double tile_sad(const uint8_t *img, int tx, int ty){
    int x0=tx*TILE, y0=ty*TILE;
    double sad=0; int n=0;
    for(int py=y0;py<y0+TILE;py++)
        for(int px=x0;px<x0+TILE;px++){
            int d=(py*W+px)*3;
            if(px+1<x0+TILE){int d2=(py*W+px+1)*3; sad+=abs(img[d]-img[d2])+abs(img[d+1]-img[d2+1])+abs(img[d+2]-img[d2+2]);n++;}
            if(py+1<y0+TILE){int d2=((py+1)*W+px)*3;sad+=abs(img[d]-img[d2])+abs(img[d+1]-img[d2+1])+abs(img[d+2]-img[d2+2]);n++;}
        }
    return n>0?sad/n:0;
}

/* ── gradient flow (O18 core) ────────────────────────────── */
static int get_neighbors(int tx,int ty,int nb[8][2]){
    int cnt=0;
    for(int dy=-1;dy<=1;dy++)
        for(int dx=-1;dx<=1;dx++){
            if(!dx&&!dy) continue;
            int ny=ty+dy; if(ny<0||ny>=TY) continue;
            int nx=(tx+dx+TX)%TX;
            nb[cnt][0]=nx; nb[cnt][1]=ny; cnt++;
        }
    return cnt;
}

/* ── BMP ─────────────────────────────────────────────────── */
static void bmp_save(const char *path,const uint8_t *px,int w,int h){
    int rs=(w*3+3)&~3;
    FILE *f=fopen(path,"wb"); if(!f)return;
    uint8_t hdr[54]={0};
    hdr[0]='B';hdr[1]='M';
    *(uint32_t*)(hdr+2)=54+rs*h;*(uint32_t*)(hdr+10)=54;
    *(uint32_t*)(hdr+14)=40;*(int32_t*)(hdr+18)=w;*(int32_t*)(hdr+22)=h;
    *(uint16_t*)(hdr+26)=1;*(uint16_t*)(hdr+28)=24;*(uint32_t*)(hdr+34)=rs*h;
    fwrite(hdr,1,54,f);
    uint8_t *row=calloc(rs,1);
    for(int y=h-1;y>=0;y--){
        for(int x=0;x<w;x++){int d=(y*w+x)*3;row[x*3]=px[d+2];row[x*3+1]=px[d+1];row[x*3+2]=px[d];}
        fwrite(row,1,rs,f);
    }
    free(row);fclose(f);
}

/* ══════════════════════════════════════════════════════════
 * O19 ADDRESS STRUCT
 * ══════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  pent_id;      /* 0-11: which of the 12 pentagon cells */
    uint16_t hilbert_local;/* Hilbert index within pentagon cell    */
    uint8_t  flags;        /* bit0=on_ridge, bit1=pole, bit2=seam   */
    uint32_t packed;       /* [31..28]=pent_id [27..14]=hilbert [13..0]=reserved */
} TileAddr;

static const uint8_t PENT_COL[12][3]={
    {255,80,80},{80,255,80},{80,80,255},{255,220,80},
    {255,80,220},{80,220,255},{255,140,80},{140,80,255},
    {80,255,140},{220,80,255},{80,140,255},{140,255,80}
};

/* ── HILBERT ORDER N for tile grid ──────────────────────── */
/* We map each basin's tiles into a local NxN grid then hilbert-order them */
#define HILBERT_N  128   /* 7-bit per axis → 14-bit index */

static void run_o19(const char *label, const uint8_t *img)
{
    printf("\n══ O19: %s ══\n", label);

    /* ── 1. energy + flow (O18) ── */
    double energy[TY][TX];
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++)
            energy[ty][tx]=tile_sad(img,tx,ty);

    int flow[TY][TX];
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int nb[8][2]; int cnt=get_neighbors(tx,ty,nb);
            int bx=tx,by=ty; double be=energy[ty][tx];
            for(int n=0;n<cnt;n++){
                double e=energy[nb[n][1]][nb[n][0]];
                if(e<be){be=e;bx=nb[n][0];by=nb[n][1];}
            }
            flow[ty][tx]=by*TX+bx;
        }

    int basin_root[TY][TX];
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int cur=ty*TX+tx;
            for(int s=0;s<256;s++){int nxt=flow[cur/TX][cur%TX];if(nxt==cur)break;cur=nxt;}
            basin_root[ty][tx]=cur;
        }

    /* ── 2. map basin_root → pentagon_id (nearest-pent of minimum) ── */
    int root_to_pent[N_TILES];
    memset(root_to_pent,-1,sizeof(root_to_pent));
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int r=ty*TX+tx;
            if(flow[ty][tx]==r){  /* this IS a root */
                V3 p=px2sph(tx*TILE+TILE/2,ty*TILE+TILE/2);
                root_to_pent[r]=nearest_pent(p);
            }
        }

    /* ── 3. per-tile pentagon assignment ── */
    int tile_pent[TY][TX];
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++)
            tile_pent[ty][tx]=root_to_pent[basin_root[ty][tx]];

    /* ── 4. collect tiles per pentagon, compute hilbert local ── */
    /*    bbox of each pentagon in (tx,ty) space                  */
    int pent_tiles_tx[12][N_TILES], pent_tiles_ty[12][N_TILES];
    int pent_cnt[12]; memset(pent_cnt,0,sizeof(pent_cnt));

    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int p=tile_pent[ty][tx];
            if(p>=0&&p<12){
                int idx=pent_cnt[p]++;
                pent_tiles_tx[p][idx]=tx;
                pent_tiles_ty[p][idx]=ty;
            }
        }

    /* bbox per pentagon */
    int bbox_x0[12],bbox_y0[12],bbox_x1[12],bbox_y1[12];
    for(int p=0;p<12;p++){
        bbox_x0[p]=TX;bbox_y0[p]=TY;bbox_x1[p]=0;bbox_y1[p]=0;
        for(int i=0;i<pent_cnt[p];i++){
            int tx=pent_tiles_tx[p][i], ty=pent_tiles_ty[p][i];
            if(tx<bbox_x0[p]) bbox_x0[p]=tx;
            if(ty<bbox_y0[p]) bbox_y0[p]=ty;
            if(tx>bbox_x1[p]) bbox_x1[p]=tx;
            if(ty>bbox_y1[p]) bbox_y1[p]=ty;
        }
    }

    /* ── 5. build TileAddr for every tile ── */
    TileAddr addr[TY][TX];
    memset(addr,0,sizeof(addr));

    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int p=tile_pent[ty][tx];
            if(p<0||p>=12){addr[ty][tx].pent_id=0;continue;}

            /* normalize to bbox → [0, HILBERT_N-1] */
            int bw=bbox_x1[p]-bbox_x0[p]+1;
            int bh=bbox_y1[p]-bbox_y0[p]+1;
            uint32_t lx=(bw>1)?(uint32_t)((tx-bbox_x0[p])*(HILBERT_N-1)/(bw-1)):0;
            uint32_t ly=(bh>1)?(uint32_t)((ty-bbox_y0[p])*(HILBERT_N-1)/(bh-1)):0;
            uint32_t hd=hilbert_xy2d(HILBERT_N,lx,ly);

            /* ridge flag: any neighbor in different pentagon? */
            int on_ridge=0;
            int nb[8][2]; int cnt=get_neighbors(tx,ty,nb);
            for(int n=0;n<cnt;n++)
                if(tile_pent[nb[n][1]][nb[n][0]]!=p){on_ridge=1;break;}

            /* pole flag */
            int is_pole=(ty<=1||ty>=TY-2);

            addr[ty][tx].pent_id       = (uint8_t)p;
            addr[ty][tx].hilbert_local = (uint16_t)(hd & 0x3FFF);
            addr[ty][tx].flags         = (on_ridge?1:0)|(is_pole?2:0);
            addr[ty][tx].packed        = ((uint32_t)p<<28)|((hd&0x3FFF)<<14);
        }

    /* ── 6. print address table ── */
    printf("  Pentagon cell sizes:\n  ");
    for(int p=0;p<12;p++) printf("P%d:%d ", p, pent_cnt[p]);
    printf("\n");

    /* sample: print first tile of each pentagon */
    printf("\n  Sample addresses (first tile per pentagon):\n");
    printf("  %-4s %-6s %-8s %-8s %-8s %s\n","P","tile","pent_id","hilbert","flags","packed_hex");
    for(int p=0;p<12;p++){
        if(pent_cnt[p]==0) continue;
        int tx=pent_tiles_tx[p][0], ty=pent_tiles_ty[p][0];
        TileAddr *a=&addr[ty][tx];
        printf("  P%-3d [%2d,%2d] %-8d %-8d %-8d 0x%08X\n",
               p,tx,ty,a->pent_id,a->hilbert_local,a->flags,a->packed);
    }

    /* address uniqueness check */
    uint32_t seen[N_TILES]; int ns=0;
    int collisions=0;
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            uint32_t pk=addr[ty][tx].packed;
            for(int i=0;i<ns;i++) if(seen[i]==pk){collisions++;break;}
            if(collisions==0||ns==0) seen[ns++]=pk;
        }
    printf("\n  Address collisions: %d / %d tiles\n", collisions, N_TILES);
    printf("  Ridge tiles: ");
    int rc=0;
    for(int ty=0;ty<TY;ty++) for(int tx=0;tx<TX;tx++) if(addr[ty][tx].flags&1)rc++;
    printf("%d (%.0f%%)\n", rc, 100.0*rc/N_TILES);

    /* ── 7. render: heatmap of hilbert_local per pentagon ── */
    uint8_t *buf=malloc(W*H*3);
    memcpy(buf,img,W*H*3);

    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int p=addr[ty][tx].pent_id;
            uint16_t hl=addr[ty][tx].hilbert_local;
            int on_ridge=addr[ty][tx].flags&1;

            /* base: pentagon color tinted by hilbert brightness */
            const uint8_t *col=PENT_COL[p%12];
            float t=(float)hl/16383.0f;  /* 0=dark 1=bright */
            for(int py=ty*TILE;py<(ty+1)*TILE;py++)
                for(int px=tx*TILE;px<(tx+1)*TILE;px++){
                    int d=(py*W+px)*3;
                    buf[d  ]=(uint8_t)(buf[d  ]*0.5+col[0]*0.5*t+col[0]*0.1);
                    buf[d+1]=(uint8_t)(buf[d+1]*0.5+col[1]*0.5*t+col[1]*0.1);
                    buf[d+2]=(uint8_t)(buf[d+2]*0.5+col[2]*0.5*t+col[2]*0.1);
                }

            /* ridge = white border */
            if(on_ridge){
                for(int side=0;side<2;side++){
                    int py_=side?((ty+1)*TILE-1):ty*TILE;
                    for(int px_=tx*TILE;px_<(tx+1)*TILE;px_++){int d=(py_*W+px_)*3;buf[d]=buf[d+1]=buf[d+2]=255;}
                    int px_b=side?((tx+1)*TILE-1):tx*TILE;
                    for(int py_b=ty*TILE;py_b<(ty+1)*TILE;py_b++){int d=(py_b*W+px_b)*3;buf[d]=buf[d+1]=buf[d+2]=255;}
                }
            }
        }

    char fname[64]; snprintf(fname,64,"o19_%s.bmp",label);
    bmp_save(fname,buf,W,H);
    free(buf);
    printf("  Saved: %s\n",fname);
}

/* ── address API (header-style summary) ─────────────────── */
static void print_api(void){
    printf("\n══ O19 ADDRESS API ══\n");
    printf("struct TileAddr {\n");
    printf("  uint8_t  pent_id;       // 0-11  which pentagon cell\n");
    printf("  uint16_t hilbert_local; // 0-16383  Hilbert order within cell\n");
    printf("  uint8_t  flags;         // bit0=ridge, bit1=pole, bit2=seam\n");
    printf("  uint32_t packed;        // [31..28]=pent [27..14]=hilbert [13..0]=sub\n");
    printf("};\n\n");
    printf("Decode:\n");
    printf("  pent_id       = (packed >> 28) & 0xF\n");
    printf("  hilbert_local = (packed >> 14) & 0x3FFF\n");
    printf("  sub_pixel     = (packed >>  0) & 0x3FFF  // future use\n\n");
    printf("Address space: 12 × 16384 = %d slots\n", 12*16384);
    printf("Actual tiles:  %d (%.2f%% fill)\n", N_TILES, 100.0*N_TILES/(12*16384));
    printf("\n→ O20: bind TileAddr → POGLS Hilbert wire coordinate\n");
    printf("       pent_id = Rubik face shard, hilbert_local = wire position\n");
}

int main(void){
    printf("gen_pent_o19 — Pentagon Address Encoder\n");
    build_icosahedron(); build_gp20();

    uint8_t *pano=malloc(W*H*3),*render=malloc(W*H*3);
    FILE *fp;
    fp=fopen("raw_pano.bin","rb");   if(!fp){fprintf(stderr,"no raw_pano.bin\n");return 1;}
    (void)fread(pano,  1,W*H*3,fp); fclose(fp);
    fp=fopen("raw_render.bin","rb"); if(!fp){fprintf(stderr,"no raw_render.bin\n");return 1;}
    (void)fread(render,1,W*H*3,fp); fclose(fp);

    run_o19("panorama", pano);
    run_o19("render",   render);
    print_api();

    free(pano); free(render);
    return 0;
}
