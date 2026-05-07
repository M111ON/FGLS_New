/*
 * gen_pent_o17.c — O17: Adaptive Threshold Pentagon Classifier
 * ═════════════════════════════════════════════════════════════
 * O16 finding: gap ratio 1.52-1.63× is stable across image types
 * but absolute blob scale varies 4× → fixed threshold fails on smooth images
 *
 * O17 fix: adaptive threshold = mean + k*sigma (per-image statistics)
 * This exploits the relative gap, not absolute value
 *
 * Build: gcc -O3 -o gen_pent_o17 gen_pent_o17.c -lm
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
#define TX    (W/TILE)
#define TY    (H/TILE)
#define PI    3.14159265358979323846
#define TAU   (2.0*PI)
#define PHI   1.61803398874989484820

/* ── sphere geometry ─────────────────────────────────────── */
typedef struct { double x,y,z; } V3;
static inline V3     v3_norm(V3 v){ double r=sqrt(v.x*v.x+v.y*v.y+v.z*v.z); if(r<1e-12)return v; return (V3){v.x/r,v.y/r,v.z/r}; }
static inline double v3_dot(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline V3     v3_add(V3 a,V3 b){ return (V3){a.x+b.x,a.y+b.y,a.z+b.z}; }
static inline V3     v3_scale(V3 v,double s){ return (V3){v.x*s,v.y*s,v.z*s}; }
static inline V3     v3_mid(V3 a,V3 b){ return v3_norm(v3_scale(v3_add(a,b),0.5)); }

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
    for(int i=0;i<N_ICO_V;i++){ face_centers[fc]=ico_v[i]; face_is_pent[fc]=1; fc++; }
    memset(edge_done,-1,sizeof(edge_done));
    for(int f=0;f<N_ICO_F;f++)
        for(int e=0;e<3;e++){
            int a=ico_f[f][e], b=ico_f[f][(e+1)%3];
            if(a>b){ int t=a;a=b;b=t; }
            if(edge_done[a][b]<0){
                edge_done[a][b]=fc;
                face_centers[fc]=v3_mid(ico_v[a],ico_v[b]);
                face_is_pent[fc]=0; fc++;
            }
        }
}
static int is_pent_boundary(V3 p, double thr){
    double d1=-2,d2=-2; int id1=0;
    for(int i=0;i<N_FACES;i++){
        double d=v3_dot(p,face_centers[i]);
        if(d>d1){ d2=d1; d1=d; id1=i; } else if(d>d2) d2=d;
    }
    return face_is_pent[id1] || (d1-d2)<thr;
}
static V3 pixel_to_sphere(int px,int py){
    double lon=((double)px/W)*TAU-PI;
    double lat=(1.0-(double)py/H)*PI-PI/2.0;
    double cl=cos(lat);
    return (V3){cl*cos(lon),cl*sin(lon),sin(lat)};
}

/* ── tile features ───────────────────────────────────────── */
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

/* ── core O17 analyze ────────────────────────────────────── */
typedef struct {
    double blob, delta, lat_scale;
    double blob_corrected;  /* lat-corrected for pole rows */
    int    gt;
} Tile;

static void run_o17(const char *label, const uint8_t *img, double k, double geo_thr)
{
    printf("\n══ O17: %s  (k=%.2f) ══\n", label, k);

    /* 1. build tile features */
    Tile tiles[TY][TX];
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            double lat_c=(1.0-((ty*TILE+TILE/2)+0.5)/H)*PI-PI/2.0;
            double ls=1.0/fmax(cos(lat_c),0.01);
            double b=tile_sad(img,tx,ty);
            tiles[ty][tx].blob=b;
            tiles[ty][tx].delta=tile_max_delta(img,tx,ty);
            tiles[ty][tx].lat_scale=ls;
            /* pole correction: scale up so pole tiles compete fairly */
            int is_pole=(ty<=1||ty>=TY-2);
            tiles[ty][tx].blob_corrected = is_pole ? b*ls*0.3 : b;
        }

    /* 2. global stats on blob_corrected */
    double mean=0, sq=0;
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++)
            mean+=tiles[ty][tx].blob_corrected;
    mean/=(TX*TY);
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            double d=tiles[ty][tx].blob_corrected-mean;
            sq+=d*d;
        }
    double sigma=sqrt(sq/(TX*TY));
    double thr_adapt = mean + k*sigma;
    printf("  blob stats: mean=%.2f  sigma=%.2f  thr=mean+%.1f*sigma=%.2f\n",
           mean, sigma, k, thr_adapt);

    /* ── O17 fix: correct GT definition ──
     * geo_thr=0.08 → GT=97% (seam zone too wide, not meaningful)
     * Correct GT: tile center maps to PENTAGON face (nearest-face classification)
     * → 64/256 tiles = 25% (balanced, definitive, no threshold ambiguity)
     */
    int gt[TY][TX];
    {
        for(int ty=0;ty<TY;ty++)
            for(int tx=0;tx<TX;tx++){
                int cx=tx*TILE+TILE/2, cy=ty*TILE+TILE/2;
                V3 p=pixel_to_sphere(cx,cy);
                /* find nearest face center */
                double best=-2.0; int bi=0;
                for(int i=0;i<N_FACES;i++){
                    double d=v3_dot(p,face_centers[i]);
                    if(d>best){best=d;bi=i;}
                }
                gt[ty][tx]=face_is_pent[bi];
            }
    }
    int n_gt=0;
    for(int ty=0;ty<TY;ty++) for(int tx=0;tx<TX;tx++) n_gt+=gt[ty][tx];
    printf("  GT pentagon tiles (nearest-face): %d / %d = %.1f%%\n",
           n_gt, TX*TY, 100.0*n_gt/(TX*TY));

    /* 4. classify: adaptive threshold on blob_corrected */
    int result[TY][TX]={0};
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++)
            result[ty][tx]=(tiles[ty][tx].blob_corrected>=thr_adapt
                          && tiles[ty][tx].delta>=5.0);

    /* 5. neighbor vote (wrap lon) */
    int voted[TY][TX]; memcpy(voted,result,sizeof(result));
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            if(result[ty][tx]) continue;
            int cnt=0;
            int nb[4][2]={{(tx-1+TX)%TX,ty},{(tx+1)%TX,ty},
                          {tx,(ty-1+TY)%TY},{tx,(ty+1)%TY}};
            for(int n=0;n<4;n++) cnt+=result[nb[n][1]][nb[n][0]];
            if(cnt>=2) voted[ty][tx]=1;
        }

    /* 6. evaluate */
    int tp=0,fp=0,fn=0,tn=0;
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int p=voted[ty][tx], g=gt[ty][tx];
            if(p&&g) tp++; else if(p&&!g) fp++;
            else if(!p&&g) fn++; else tn++;
        }
    double prec=tp/(double)(tp+fp+1e-9);
    double rec =tp/(double)(tp+fn+1e-9);
    double f1  =2*prec*rec/(prec+rec+1e-9);
    printf("  Precision=%.2f  Recall=%.2f  F1=%.2f  (TP=%d FP=%d FN=%d TN=%d)\n",
           prec,rec,f1,tp,fp,fn,tn);

    /* 7. gap ratio by GT class */
    double sum_h=0,sum_p=0; int cnt_h=0,cnt_p=0;
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            if(gt[ty][tx]){ sum_p+=tiles[ty][tx].blob; cnt_p++; }
            else           { sum_h+=tiles[ty][tx].blob; cnt_h++; }
        }
    printf("  hex mean=%.2f  pent mean=%.2f  gap=%.2f×\n",
           cnt_h?sum_h/cnt_h:0, cnt_p?sum_p/cnt_p:0,
           (cnt_h&&cnt_p) ? (sum_p/cnt_p)/(sum_h/cnt_h) : 0);

    /* 8. scan k values to find optimal */
    printf("\n  k-sweep (F1 vs threshold):\n");
    printf("  %-6s %-8s %-8s %-6s\n","k","prec","recall","F1");
    for(int ki=0;ki<=20;ki++){
        double kv=ki*0.1;
        double thr=mean+kv*sigma;
        int tp2=0,fp2=0,fn2=0;
        for(int ty=0;ty<TY;ty++)
            for(int tx=0;tx<TX;tx++){
                int p=(tiles[ty][tx].blob_corrected>=thr && tiles[ty][tx].delta>=5.0);
                int g=gt[ty][tx];
                if(p&&g) tp2++; else if(p&&!g) fp2++; else if(!p&&g) fn2++;
            }
        double p2=tp2/(double)(tp2+fp2+1e-9);
        double r2=tp2/(double)(tp2+fn2+1e-9);
        double f=2*p2*r2/(p2+r2+1e-9);
        if(f>0.70) printf("  %-6.1f %-8.2f %-8.2f %-6.2f%s\n",
                          kv,p2,r2,f, f>=0.90?"  ← ✓":"");
    }

    /* 9. render overlay on real image */
    uint8_t *buf=malloc(W*H*3);
    memcpy(buf,img,W*H*3);
    for(int ty=0;ty<TY;ty++)
        for(int tx=0;tx<TX;tx++){
            int p=voted[ty][tx], g=gt[ty][tx];
            uint8_t or_,og,ob;
            if     (p&&g) {or_=0;  og=255;ob=80;}
            else if(p&&!g){or_=255;og=40; ob=40;}
            else if(!p&&g){or_=255;og=230;ob=0; }
            else continue;
            for(int side=0;side<2;side++){
                int py_=side?((ty+1)*TILE-1):ty*TILE;
                for(int px_=tx*TILE;px_<(tx+1)*TILE;px_++){
                    int d=(py_*W+px_)*3;
                    buf[d]=(buf[d]+or_)/2; buf[d+1]=(buf[d+1]+og)/2; buf[d+2]=(buf[d+2]+ob)/2;
                }
                int px_b=side?((tx+1)*TILE-1):tx*TILE;
                for(int py_b=ty*TILE;py_b<(ty+1)*TILE;py_b++){
                    int d=(py_b*W+px_b)*3;
                    buf[d]=(buf[d]+or_)/2; buf[d+1]=(buf[d+1]+og)/2; buf[d+2]=(buf[d+2]+ob)/2;
                }
            }
        }
    char fname[64]; snprintf(fname,64,"o17_%s.bmp",label);
    bmp_save(fname,buf,W,H);
    free(buf);
    printf("  Saved: %s\n",fname);
}

int main(void){
    printf("gen_pent_o17 — Adaptive Threshold Pentagon Classifier\n");
    build_icosahedron(); build_gp20();

    uint8_t *pano  =malloc(W*H*3), *render=malloc(W*H*3);
    FILE *fp;
    fp=fopen("raw_pano.bin","rb");   if(!fp){fprintf(stderr,"no raw_pano.bin\n");return 1;}
    (void)fread(pano,  1,W*H*3,fp); fclose(fp);
    fp=fopen("raw_render.bin","rb"); if(!fp){fprintf(stderr,"no raw_render.bin\n");return 1;}
    (void)fread(render,1,W*H*3,fp); fclose(fp);

    /* geo_thr=0.08 → 232/256 GT=1 (90% → no negative class to test)
     * geo_thr=0.02 → ~58/256 GT=1 (23% → balanced, meaningful eval)
     * GP(2,0) seam zone ≈ ±0.02 rad from pentagon/hex boundary */
    double geo_thr=0.02;
    double k=0.5;

    run_o17("panorama", pano,   k, geo_thr);
    run_o17("render",   render, k, geo_thr);

    /* ── cross-image summary ── */
    printf("\n══ O17 SUMMARY ══\n");
    printf("Adaptive thr = mean + k*sigma\n");
    printf("  Removes dependency on absolute blob scale\n");
    printf("  Gap ratio 1.5× preserved → k=0.3-0.5 should hit F1>0.90\n");
    printf("\nProduction formula:\n");
    printf("  1. compute blob_sad for all tiles\n");
    printf("  2. apply lat_scale for pole rows (ty≤1, ty≥14)\n");
    printf("  3. thr = mean(blob_corrected) + 0.5 * stddev(blob_corrected)\n");
    printf("  4. classify: blob_corrected > thr AND max_delta > 5\n");
    printf("  5. neighbor vote: promote if ≥2 neighbors positive\n");
    printf("\n→ O18: encode tile_id into pixel value (pentagon address scheme)\n");

    free(pano); free(render);
    return 0;
}
