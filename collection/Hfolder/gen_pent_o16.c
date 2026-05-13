/*
 * gen_pent_o16.c — O16: Real Image Test
 * Input: raw_pano.bin, raw_render.bin (512×512×3 raw RGB)
 * Build: gcc -O3 -o gen_pent_o16 gen_pent_o16.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define W       512
#define H       512
#define TILE    32
#define TX      (W/TILE)   /* 16 */
#define TY      (H/TILE)   /* 16 */
#define PI      3.14159265358979323846
#define TAU     (2.0*PI)
#define PHI     1.61803398874989484820

/* ── Sphere geometry (same as O15) ─────────────────────── */
typedef struct { double x, y, z; } V3;
static inline V3 v3_norm(V3 v) {
    double r = sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
    if(r<1e-12) return v;
    return (V3){v.x/r,v.y/r,v.z/r};
}
static inline double v3_dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static inline V3 v3_add(V3 a,V3 b){return (V3){a.x+b.x,a.y+b.y,a.z+b.z};}
static inline V3 v3_scale(V3 v,double s){return (V3){v.x*s,v.y*s,v.z*s};}
static inline V3 v3_mid(V3 a,V3 b){return v3_norm(v3_scale(v3_add(a,b),0.5));}

#define N_ICO_V 12
#define N_ICO_F 20
#define N_FACES 42
static V3  ico_v[N_ICO_V];
static int ico_f[N_ICO_F][3];
static V3  face_centers[N_FACES];
static int face_is_pent[N_FACES];
static int edge_done[N_ICO_V][N_ICO_V];

static void build_icosahedron(void){
    double t=1.0/PHI;
    V3 raw[12]={{-1,t,0},{1,t,0},{-1,-t,0},{1,-t,0},
                {0,-1,t},{0,1,t},{0,-1,-t},{0,1,-t},
                {t,0,-1},{t,0,1},{-t,0,-1},{-t,0,1}};
    for(int i=0;i<12;i++) ico_v[i]=v3_norm(raw[i]);
    int f[20][3]={{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
                  {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
                  {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
                  {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}};
    memcpy(ico_f,f,sizeof(f));
}
static void build_gp20(void){
    int fc=0;
    for(int i=0;i<N_ICO_V;i++){face_centers[fc]=ico_v[i];face_is_pent[fc]=1;fc++;}
    memset(edge_done,-1,sizeof(edge_done));
    for(int f=0;f<N_ICO_F;f++)
        for(int e=0;e<3;e++){
            int a=ico_f[f][e],b=ico_f[f][(e+1)%3];
            if(a>b){int t=a;a=b;b=t;}
            if(edge_done[a][b]<0){
                edge_done[a][b]=fc;
                face_centers[fc]=v3_mid(ico_v[a],ico_v[b]);
                face_is_pent[fc]=0; fc++;
            }
        }
}
static int classify_sphere(V3 p){
    double best=-2.0; int bi=0;
    for(int i=0;i<N_FACES;i++){double d=v3_dot(p,face_centers[i]);if(d>best){best=d;bi=i;}}
    return bi;
}
static int is_pent_boundary(V3 p, double thr){
    double d1=-2,d2=-2; int id1=0;
    for(int i=0;i<N_FACES;i++){
        double d=v3_dot(p,face_centers[i]);
        if(d>d1){d2=d1;d1=d;id1=i;}
        else if(d>d2) d2=d;
    }
    if(face_is_pent[id1]) return 1;
    return (d1-d2)<thr;
}
static V3 pixel_to_sphere(int px,int py){
    double lon=((double)px/W)*TAU-PI;
    double lat=(1.0-(double)py/H)*PI-PI/2.0;
    double cl=cos(lat);
    return (V3){cl*cos(lon),cl*sin(lon),sin(lat)};
}

/* ── blob_sz proxy from real image ──────────────────────────
 * Encoder blob_sz ≈ compressed size of tile
 * Proxy: sum of absolute differences between adjacent pixels (SAD)
 * SAD high → high spatial frequency → encoder compresses poorly → big blob
 * Normalize to [0,255] range for comparison with synthetic results
 */
static double tile_blob_proxy(const uint8_t *img, int tx, int ty){
    int x0=tx*TILE, y0=ty*TILE;
    double sad=0;
    int count=0;
    for(int py=y0;py<y0+TILE;py++){
        for(int px=x0;px<x0+TILE;px++){
            int d=(py*W+px)*3;
            /* horizontal diff */
            if(px+1<x0+TILE){
                int d2=(py*W+px+1)*3;
                sad+=abs(img[d]-img[d2])+abs(img[d+1]-img[d2+1])+abs(img[d+2]-img[d2+2]);
                count++;
            }
            /* vertical diff */
            if(py+1<y0+TILE){
                int d2=((py+1)*W+px)*3;
                sad+=abs(img[d]-img[d2])+abs(img[d+1]-img[d2+1])+abs(img[d+2]-img[d2+2]);
                count++;
            }
        }
    }
    return count>0 ? sad/count : 0;
}

/* max_delta: max channel range within tile */
static double tile_max_delta(const uint8_t *img, int tx, int ty){
    int x0=tx*TILE, y0=ty*TILE;
    int mn[3]={255,255,255}, mx[3]={0,0,0};
    for(int py=y0;py<y0+TILE;py++)
        for(int px=x0;px<x0+TILE;px++){
            int d=(py*W+px)*3;
            for(int c=0;c<3;c++){
                if(img[d+c]<mn[c]) mn[c]=img[d+c];
                if(img[d+c]>mx[c]) mx[c]=img[d+c];
            }
        }
    int delta=0;
    for(int c=0;c<3;c++) if(mx[c]-mn[c]>delta) delta=mx[c]-mn[c];
    return delta;
}

/* ── BMP write ──────────────────────────────────────────── */
static void bmp_save(const char *path, const uint8_t *px, int w, int h){
    int rs=(w*3+3)&~3;
    FILE *f=fopen(path,"wb"); if(!f) return;
    uint8_t hdr[54]={0};
    hdr[0]='B';hdr[1]='M';
    *(uint32_t*)(hdr+2)=54+rs*h; *(uint32_t*)(hdr+10)=54;
    *(uint32_t*)(hdr+14)=40; *(int32_t*)(hdr+18)=w; *(int32_t*)(hdr+22)=h;
    *(uint16_t*)(hdr+26)=1; *(uint16_t*)(hdr+28)=24; *(uint32_t*)(hdr+34)=rs*h;
    fwrite(hdr,1,54,f);
    uint8_t *row=calloc(rs,1);
    for(int y=h-1;y>=0;y--){
        for(int x=0;x<w;x++){
            int d=(y*w+x)*3;
            row[x*3]=px[d+2];row[x*3+1]=px[d+1];row[x*3+2]=px[d];
        }
        fwrite(row,1,rs,f);
    }
    free(row);fclose(f);
}

/* ── analyze image ──────────────────────────────────────── */
static void analyze(const char *label, const uint8_t *img,
                    double blob_thr, double delta_thr,
                    double geo_thr,  double lat_pole_relax)
{
    printf("\n══ %s ══\n", label);

    double blob[TY][TX], delta_[TY][TX];
    double bmin=1e9,bmax=0, bmean=0;

    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            blob[ty][tx]  = tile_blob_proxy(img, tx, ty);
            delta_[ty][tx]= tile_max_delta(img, tx, ty);
            if(blob[ty][tx]<bmin) bmin=blob[ty][tx];
            if(blob[ty][tx]>bmax) bmax=blob[ty][tx];
            bmean+=blob[ty][tx];
        }
    bmean/=(TX*TY);
    printf("  blob_proxy: min=%.1f max=%.1f mean=%.1f\n", bmin, bmax, bmean);

    /* GT from sphere geometry */
    int gt[TY][TX]={0};
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int hits=0;
            for(int py=ty*TILE;py<(ty+1)*TILE;py++)
                for(int px=tx*TILE;px<(tx+1)*TILE;px++)
                    if(is_pent_boundary(pixel_to_sphere(px,py), geo_thr)) hits++;
            gt[ty][tx]=(hits>=(TILE*TILE/6)); /* ≥16% */
        }
    int n_gt=0; for(int i=0;i<TY*TY;i++) n_gt+=((int*)gt)[i];

    /* ── O15 algorithm: threshold on normalized blob_proxy ── */
    /* Normalize blob to match synthetic blob_sz scale (0-255) */
    /* Then apply same thresholds: blob>52, delta>5 + pole correction */
    double norm_scale = bmax>0 ? 100.0/bmax : 1.0;  /* scale so max→100 */

    int result[TY][TX]={0};
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int is_pole=(ty<=1||ty>=TY-2);
            double nb = blob[ty][tx]*norm_scale;
            double lat_c=(1.0-((ty*TILE+TILE/2)+0.5)/H)*PI-PI/2.0;
            double ls=1.0/fmax(cos(lat_c),0.01);
            if(is_pole) nb*=(ls*lat_pole_relax);
            result[ty][tx]=(nb>=blob_thr && delta_[ty][tx]>=delta_thr);
        }

    /* neighbor vote */
    int voted[TY][TX]; memcpy(voted,result,sizeof(result));
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            if(result[ty][tx]) continue;
            int cnt=0;
            int nb[4][2]={{(tx-1+TX)%TX,ty},{(tx+1)%TX,ty},{tx,(ty-1+TY)%TY},{tx,(ty+1)%TY}};
            for(int n=0;n<4;n++) cnt+=result[nb[n][1]][nb[n][0]];
            if(cnt>=2) voted[ty][tx]=1;
        }

    /* evaluate */
    int tp=0,fp=0,fn=0;
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            if(voted[ty][tx]&&gt[ty][tx])  tp++;
            else if(voted[ty][tx]&&!gt[ty][tx]) fp++;
            else if(!voted[ty][tx]&&gt[ty][tx]) fn++;
        }
    double prec=tp/(double)(tp+fp+1e-9);
    double rec =tp/(double)(tp+fn+1e-9);
    double f1  =2*prec*rec/(prec+rec+1e-9);
    printf("  GT boundary tiles: %d\n", n_gt);
    printf("  Precision=%.2f  Recall=%.2f  F1=%.2f  (TP=%d FP=%d FN=%d)\n",
           prec, rec, f1, tp, fp, fn);

    /* gap analysis: blob distribution by GT class */
    printf("\n  blob_proxy distribution:\n");
    printf("  %-12s %-8s %-8s %-8s\n","tile_class","count","mean_b","mean_d");
    double sum_hex_b=0,sum_pent_b=0; int cnt_h=0,cnt_p=0;
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            if(gt[ty][tx]){sum_pent_b+=blob[ty][tx];cnt_p++;}
            else{sum_hex_b+=blob[ty][tx];cnt_h++;}
        }
    printf("  %-12s %-8d %-8.1f\n","hex (GT=0)",cnt_h,cnt_h?sum_hex_b/cnt_h:0);
    printf("  %-12s %-8d %-8.1f\n","pent (GT=1)",cnt_p,cnt_p?sum_pent_b/cnt_p:0);
    printf("  Gap ratio: %.2f×\n", cnt_h&&cnt_p ? (sum_pent_b/cnt_p)/(sum_hex_b/cnt_h) : 0);

    /* render result overlay */
    uint8_t *buf=malloc(W*H*3);
    memcpy(buf,img,W*H*3);  /* start from real image */
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int p=voted[ty][tx], g=gt[ty][tx];
            uint8_t or_,og,ob;
            if     (p&&g) {or_=0;og=220;ob=0;}   /* TP: green tint */
            else if(p&&!g){or_=220;og=0;ob=0;}    /* FP: red tint */
            else if(!p&&g){or_=220;og=220;ob=0;}  /* FN: yellow tint */
            else continue; /* TN: no overlay */
            /* blend tile border */
            for(int side=0;side<2;side++){
                int py=side?((ty+1)*TILE-1):ty*TILE;
                for(int px=tx*TILE;px<(tx+1)*TILE;px++){
                    int d=(py*W+px)*3;
                    buf[d]=(buf[d]+or_)/2; buf[d+1]=(buf[d+1]+og)/2; buf[d+2]=(buf[d+2]+ob)/2;
                }
                int px_=side?((tx+1)*TILE-1):tx*TILE;
                for(int py_=ty*TILE;py_<(ty+1)*TILE;py_++){
                    int d=(py_*W+px_)*3;
                    buf[d]=(buf[d]+or_)/2; buf[d+1]=(buf[d+1]+og)/2; buf[d+2]=(buf[d+2]+ob)/2;
                }
            }
        }

    char fname[64]; snprintf(fname,64,"o16_%s.bmp",label);
    bmp_save(fname,buf,W,H);
    printf("  Saved: %s\n",fname);
    free(buf);
}

/* ── main ────────────────────────────────────────────────── */
int main(void){
    printf("gen_pent_o16 — Real Image Pentagon Classifier Test\n");

    build_icosahedron();
    build_gp20();

    /* load raw RGB binaries (512×512×3) */
    uint8_t *pano  =malloc(W*H*3), *render=malloc(W*H*3);
    FILE *fp;
    fp=fopen("raw_pano.bin","rb");   if(!fp){fprintf(stderr,"no raw_pano.bin\n");return 1;}
    fread(pano,  1,W*H*3,fp); fclose(fp);
    fp=fopen("raw_render.bin","rb"); if(!fp){fprintf(stderr,"no raw_render.bin\n");return 1;}
    fread(render,1,W*H*3,fp); fclose(fp);
    printf("Images loaded: %dx%d raw RGB\n",W,H);

    /*
     * Parameters (O15-derived):
     *   blob_thr     = 35.0  (normalized to max=100 scale)
     *   delta_thr    = 5.0
     *   geo_thr      = 0.08 rad
     *   lat_relax    = 0.3   (pole correction scale-down for real images)
     */
    double blob_thr=35.0, delta_thr=5.0, geo_thr=0.08, lat_relax=0.3;

    analyze("panorama", pano,   blob_thr, delta_thr, geo_thr, lat_relax);
    analyze("render",   render, blob_thr, delta_thr, geo_thr, lat_relax);

    /* cross-image comparison */
    printf("\n══ CROSS-IMAGE GAP COMPARISON ══\n");
    printf("Key question: does gap ratio stay > 1.5× on real images?\n");
    printf("  (Synthetic O14: gap was 52-78B = 1.5× separation)\n");
    printf("  Gap ratio > 1.5 → algorithm is projection-stable\n");
    printf("  Gap ratio < 1.2 → algorithm needs image-adaptive threshold\n");

    free(pano);
    free(render);
    return 0;
}
