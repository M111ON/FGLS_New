/*
 * gen_pent_o18.c — O18: Gradient Flow Basin Classifier
 * ═════════════════════════════════════════════════════
 * O17 finding: threshold/boundary approach fundamentally broken
 *   - GP(2,0) seam zone covers ~50% sphere → no clean binary
 *   - blob scale varies 4× across image types
 *
 * O18 fix: treat blob_sad as scalar field, run gradient descent
 *   - pentagon faces = energy minima ("gravity wells")
 *   - each tile flows to its basin minimum
 *   - 12 basins emerge naturally, no threshold needed
 *
 * Analogy: raindrops on terrain — they find valleys, not "is this low enough?"
 *
 * Build: gcc -O3 -o gen_pent_o18 gen_pent_o18.c -lm
 * Input: raw_pano.bin, raw_render.bin (512×512×3)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define W     512
#define H     512
#define TILE  32
#define TX    (W/TILE)   /* 16 */
#define TY    (H/TILE)   /* 16 */
#define N_TILES (TX*TY)  /* 256 */
#define PI    3.14159265358979323846
#define TAU   (2.0*PI)
#define PHI   1.61803398874989484820

/* ── vector ───────────────────────────────────────────────── */
typedef struct { double x,y,z; } V3;
static inline V3     v3n(V3 v){ double r=sqrt(v.x*v.x+v.y*v.y+v.z*v.z); if(r<1e-12)return v; return (V3){v.x/r,v.y/r,v.z/r}; }
static inline double v3d(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline V3     v3a(V3 a,V3 b){ return (V3){a.x+b.x,a.y+b.y,a.z+b.z}; }
static inline V3     v3s(V3 v,double s){ return (V3){v.x*s,v.y*s,v.z*s}; }
static inline V3     v3mid(V3 a,V3 b){ return v3n(v3s(v3a(a,b),0.5)); }

/* ── icosahedron → GP(2,0) 42-face ───────────────────────── */
#define N_ICO_V 12
#define N_ICO_F 20
#define N_FACES 42

static V3  ico_v[N_ICO_V];
static int ico_f[N_ICO_F][3];
static V3  face_ctr[N_FACES];
static int face_pent[N_FACES];   /* 1=pentagon, 0=hexagon */
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
    /* pentagon indices: 0..11 */
}

/* ── pixel → sphere ──────────────────────────────────────── */
static V3 px2sph(int px, int py){
    double lon=((double)px/W)*TAU-PI;
    double lat=(1.0-(double)py/H)*PI-PI/2.0;
    double cl=cos(lat);
    return (V3){cl*cos(lon),cl*sin(lon),sin(lat)};
}

/* nearest pentagon ID (0-11) for a sphere point */
static int nearest_pent(V3 p){
    double best=-2.0; int bi=0;
    for(int i=0;i<12;i++){
        double d=v3d(p,face_ctr[i]);
        if(d>best){best=d;bi=i;}
    }
    return bi;
}

/* ── tile blob energy ────────────────────────────────────── */
static double tile_sad(const uint8_t *img, int tx, int ty){
    int x0=tx*TILE, y0=ty*TILE;
    double sad=0; int n=0;
    for(int py=y0;py<y0+TILE;py++)
        for(int px=x0;px<x0+TILE;px++){
            int d=(py*W+px)*3;
            if(px+1<x0+TILE){
                int d2=(py*W+px+1)*3;
                sad+=abs(img[d]-img[d2])+abs(img[d+1]-img[d2+1])+abs(img[d+2]-img[d2+2]); n++;
            }
            if(py+1<y0+TILE){
                int d2=((py+1)*W+px)*3;
                sad+=abs(img[d]-img[d2])+abs(img[d+1]-img[d2+1])+abs(img[d+2]-img[d2+2]); n++;
            }
        }
    return n>0 ? sad/n : 0;
}

/* ── palette: 12 colors for 12 basins ───────────────────── */
static const uint8_t BASIN_COL[12][3]={
    {255,80,80},  {80,255,80},  {80,80,255},  {255,255,80},
    {255,80,255}, {80,255,255}, {255,160,80}, {160,80,255},
    {80,255,160}, {255,80,160}, {80,160,255}, {160,255,80}
};

/* ── BMP write ───────────────────────────────────────────── */
static void bmp_save(const char *path, const uint8_t *px, int w, int h){
    int rs=(w*3+3)&~3;
    FILE *f=fopen(path,"wb"); if(!f) return;
    uint8_t hdr[54]={0};
    hdr[0]='B'; hdr[1]='M';
    *(uint32_t*)(hdr+2)=54+rs*h; *(uint32_t*)(hdr+10)=54;
    *(uint32_t*)(hdr+14)=40; *(int32_t*)(hdr+18)=w; *(int32_t*)(hdr+22)=h;
    *(uint16_t*)(hdr+26)=1; *(uint16_t*)(hdr+28)=24; *(uint32_t*)(hdr+34)=rs*h;
    fwrite(hdr,1,54,f);
    uint8_t *row=calloc(rs,1);
    for(int y=h-1;y>=0;y--){
        for(int x=0;x<w;x++){
            int d=(y*w+x)*3;
            row[x*3]=px[d+2]; row[x*3+1]=px[d+1]; row[x*3+2]=px[d];
        }
        fwrite(row,1,rs,f);
    }
    free(row); fclose(f);
}

/* ══════════════════════════════════════════════════════════
 * CORE O18: gradient flow basin finder
 * ══════════════════════════════════════════════════════════
 *
 * energy[ty][tx] = blob_sad (high = texture-rich = hex ridge)
 *                            (low  = smooth      = pentagon well)
 *
 * gradient descent: each tile follows steepest downhill neighbor
 * until it reaches a local minimum → that minimum = basin ID
 *
 * wrap: longitude wraps (tx-1+TX)%TX, latitude clamps
 */

/* 8-connected neighbors (lon wraps, lat clamps) */
static int get_neighbors(int tx, int ty, int nb[8][2]){
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

static void run_o18(const char *label, const uint8_t *img)
{
    printf("\n══ O18: %s ══\n", label);

    /* 1. compute energy field (blob_sad) */
    double energy[TY][TX];
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++)
            energy[ty][tx] = tile_sad(img,tx,ty);

    /* debug: energy range */
    double emin=1e9,emax=0;
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            if(energy[ty][tx]<emin) emin=energy[ty][tx];
            if(energy[ty][tx]>emax) emax=energy[ty][tx];
        }
    printf("  energy range: %.2f – %.2f\n", emin, emax);

    /* 2. gradient descent: flow each tile to its basin minimum
     *    basin[ty][tx] = (min_ty*TX + min_tx) flat index of local min */
    int basin[TY][TX];
    memset(basin,-1,sizeof(basin));

    /* iterative label propagation (fast, no recursion) */
    /* first pass: each tile points to steepest-descent neighbor */
    int flow[TY][TX];  /* flat index of next tile */
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int nb[8][2]; int cnt=get_neighbors(tx,ty,nb);
            int best_nx=tx, best_ny=ty;
            double best_e=energy[ty][tx];
            for(int n=0;n<cnt;n++){
                double e=energy[nb[n][1]][nb[n][0]];
                if(e<best_e){ best_e=e; best_nx=nb[n][0]; best_ny=nb[n][1]; }
            }
            flow[ty][tx] = best_ny*TX + best_nx;
        }

    /* follow chain to root (local minimum = self-loop) */
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            /* path compression */
            int cur = ty*TX+tx;
            /* walk up to 256 steps max */
            for(int step=0;step<256;step++){
                int nxt = flow[cur/TX][cur%TX];
                if(nxt==cur) break;
                cur=nxt;
            }
            basin[ty][tx] = cur;  /* flat index of basin minimum */
        }

    /* 3. count distinct basins */
    int basin_ids[N_TILES]; int n_basins=0;
    memset(basin_ids,-1,sizeof(basin_ids));
    int basin_label[TY][TX];   /* 0-based label */
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int b=basin[ty][tx];
            int found=-1;
            for(int i=0;i<n_basins;i++) if(basin_ids[i]==b){found=i;break;}
            if(found<0){ basin_ids[n_basins]=b; found=n_basins++; }
            basin_label[ty][tx]=found;
        }
    printf("  distinct basins: %d\n", n_basins);

    /* 4. compare basin label → nearest-pentagon GT */
    /*    tile center sphere point → nearest pentagon (0-11) */
    int gt_pent[TY][TX];
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            V3 p=px2sph(tx*TILE+TILE/2, ty*TILE+TILE/2);
            gt_pent[ty][tx]=nearest_pent(p);
        }

    /* find majority GT pentagon per basin */
    int basin_gt[N_TILES];
    memset(basin_gt,-1,sizeof(basin_gt));
    {
        int votes[N_TILES][12]; memset(votes,0,sizeof(votes));
        for(int ty=0;ty<TY;ty++)
            for(int tx=0;tx<TX;tx++){
                int bl=basin_label[ty][tx];
                int gp=gt_pent[ty][tx];
                if(bl<N_TILES && gp<12) votes[bl][gp]++;
            }
        for(int b=0;b<n_basins;b++){
            int best=0,bv=-1;
            for(int p=0;p<12;p++) if(votes[b][p]>bv){bv=votes[b][p];best=p;}
            basin_gt[b]=best;
        }
    }

    /* purity: fraction of tiles in basin matching majority GT */
    double total_match=0;
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int bl=basin_label[ty][tx];
            if(gt_pent[ty][tx]==basin_gt[bl]) total_match++;
        }
    printf("  basin purity: %.1f%%  (%d tiles)\n",
           100.0*total_match/N_TILES, N_TILES);

    /* 5. basin size distribution */
    int bsize[N_TILES]; memset(bsize,0,sizeof(bsize));
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++)
            bsize[basin_label[ty][tx]]++;
    printf("  basin sizes (top 12):\n  ");
    /* sort by size descending for display */
    for(int i=0;i<n_basins&&i<12;i++){
        int mx=0,mxi=0;
        for(int b=i;b<n_basins;b++) if(bsize[b]>mx){mx=bsize[b];mxi=b;}
        int tmp=bsize[i]; bsize[i]=bsize[mxi]; bsize[mxi]=tmp;
        int tmpi=basin_gt[i]; basin_gt[i]=basin_gt[mxi]; basin_gt[mxi]=tmpi;
        printf("B%d:%d ", i, bsize[i]);
    }
    printf("\n");

    /* 6. topology: basin adjacency (ridge lines = where basins meet) */
    printf("  adjacency pairs (ridge lines):\n  ");
    int adj_printed=0;
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int bl=basin_label[ty][tx];
            int nb[8][2]; int cnt=get_neighbors(tx,ty,nb);
            for(int n=0;n<cnt;n++){
                int bl2=basin_label[nb[n][1]][nb[n][0]];
                if(bl2>bl){ printf("%d↔%d ",bl,bl2); adj_printed++; if(adj_printed>=10){printf("..."); goto adj_done;} }
            }
        }
    adj_done:
    printf("\n");

    /* 7. render: color tiles by basin + ridge overlay */
    uint8_t *buf=malloc(W*H*3);
    memcpy(buf,img,W*H*3);

    /* basin fill: blend 40% basin color over image */
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int bl=basin_label[ty][tx] % 12;
            const uint8_t *col=BASIN_COL[bl];
            for(int py=ty*TILE;py<(ty+1)*TILE;py++)
                for(int px=tx*TILE;px<(tx+1)*TILE;px++){
                    int d=(py*W+px)*3;
                    buf[d  ]=(uint8_t)(buf[d  ]*0.6+col[0]*0.4);
                    buf[d+1]=(uint8_t)(buf[d+1]*0.6+col[1]*0.4);
                    buf[d+2]=(uint8_t)(buf[d+2]*0.6+col[2]*0.4);
                }
        }

    /* ridge lines: white border where adjacent tiles differ */
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int bl=basin_label[ty][tx];
            /* right edge */
            int tx2=(tx+1)%TX;
            if(basin_label[ty][tx2]!=bl){
                int px=tx*TILE+TILE-1;
                for(int py=ty*TILE;py<(ty+1)*TILE;py++){
                    int d=(py*W+px)*3;
                    buf[d]=buf[d+1]=buf[d+2]=255;
                }
            }
            /* bottom edge */
            if(ty+1<TY && basin_label[ty+1][tx]!=bl){
                int py=(ty+1)*TILE;
                for(int px_=tx*TILE;px_<(tx+1)*TILE;px_++){
                    int d=(py*W+px_)*3;
                    buf[d]=buf[d+1]=buf[d+2]=255;
                }
            }
        }

    /* mark basin minima with bright dot */
    for(int b=0;b<n_basins;b++){
        int bi=basin_ids[b];
        int my=bi/TX, mx=bi%TX;
        /* 5×5 white cross */
        for(int dy=-2;dy<=2;dy++)
            for(int dx=-2;dx<=2;dx++){
                int py=(my*TILE+TILE/2)+dy;
                int px=(mx*TILE+TILE/2)+dx;
                if(px>=0&&px<W&&py>=0&&py<H){
                    int d=(py*W+px)*3;
                    buf[d]=buf[d+1]=buf[d+2]=(abs(dx)+abs(dy)<=2)?255:0;
                }
            }
    }

    char fname[64]; snprintf(fname,64,"o18_%s.bmp",label);
    bmp_save(fname,buf,W,H);
    free(buf);
    printf("  Saved: %s\n",fname);

    /* 8. O18 summary stats */
    printf("\n  ── O18 field summary ──\n");
    printf("  Method: gradient descent on blob_sad field\n");
    printf("  Basins found: %d  (target: 12 pentagon wells)\n", n_basins);
    printf("  No threshold needed — topology emerges from field\n");
    printf("  Ridge lines = Voronoi edges on sphere texture\n");
}

int main(void){
    printf("gen_pent_o18 — Gradient Flow Basin Classifier\n");
    build_icosahedron(); build_gp20();
    printf("GP(2,0): %d faces (%d pent + %d hex)\n", N_FACES, 12, N_FACES-12);

    uint8_t *pano=malloc(W*H*3), *render=malloc(W*H*3);
    FILE *fp;
    fp=fopen("raw_pano.bin","rb");   if(!fp){fprintf(stderr,"no raw_pano.bin\n");return 1;}
    (void)fread(pano,  1,W*H*3,fp); fclose(fp);
    fp=fopen("raw_render.bin","rb"); if(!fp){fprintf(stderr,"no raw_render.bin\n");return 1;}
    (void)fread(render,1,W*H*3,fp); fclose(fp);

    run_o18("panorama", pano);
    run_o18("render",   render);

    printf("\n══ O18 FINDINGS ══\n");
    printf("Field dynamics replaces threshold:\n");
    printf("  blob_sad = energy landscape\n");
    printf("  pentagon = local minimum (smooth well)\n");
    printf("  hex      = ridge / high energy\n");
    printf("  tile     = raindrop → flows to basin\n");
    printf("\nFree outputs:\n");
    printf("  - basin adjacency graph (topology)\n");
    printf("  - ridge pixel map (seam lines)\n");
    printf("  - basin purity vs GT pentagon\n");
    printf("\n→ O19: encode basin_id → POGLS geometric address\n");
    printf("       pentagon_id (0-11) = 12 Voronoi cells on sphere\n");

    free(pano); free(render);
    return 0;
}
