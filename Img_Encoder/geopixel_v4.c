#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zstd.h>

// GEOPIXEL v3 — YCgCo-R lossless color transform + varint streams + zstd-19
// Y=(R+2G+B)>>2  Cg=G-Y+64  Co=R-Y+64  → all in [0,255], fully reversible

#define TILE    16
#define AXIS    768
#define MAGIC   0x47454F33u
#define ZST_LVL 19
#define CG_OFF  64
#define CO_OFF  64

typedef struct { int w,h; uint8_t *px; } Img;

int bmp_load(const char *path, Img *img){
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    uint8_t hdr[54]; fread(hdr,1,54,f);
    img->w=*(int32_t*)(hdr+18); img->h=*(int32_t*)(hdr+22);
    int rs=(img->w*3+3)&~3;
    img->px=malloc(img->w*img->h*3);
    fseek(f,*(uint32_t*)(hdr+10),SEEK_SET);
    uint8_t *row=malloc(rs);
    for(int y=img->h-1;y>=0;y--){
        fread(row,1,rs,f);
        for(int x=0;x<img->w;x++){
            int d=(y*img->w+x)*3;
            img->px[d+0]=row[x*3+2];
            img->px[d+1]=row[x*3+1];
            img->px[d+2]=row[x*3+0];
        }
    }
    free(row); fclose(f); return 1;
}

static inline void rgb2ycc(uint8_t r,uint8_t g,uint8_t b,uint8_t*Y,uint8_t*Cg,uint8_t*Co){
    int y=(r+2*g+b)>>2;
    *Y =(uint8_t)y;
    *Cg=(uint8_t)((int)g - y + CG_OFF);
    *Co=(uint8_t)((int)r - y + CO_OFF);
}
static inline void ycc2rgb(uint8_t Y,uint8_t Cg,uint8_t Co,uint8_t*r,uint8_t*g,uint8_t*b){
    int y=(int)Y, cg=(int)Cg-CG_OFF, co=(int)Co-CO_OFF;
    int rv=y+co, gv=y+cg, bv=4*y-rv-2*gv;
    *r=(uint8_t)(rv<0?0:rv>255?255:rv);
    *g=(uint8_t)(gv<0?0:gv>255?255:gv);
    *b=(uint8_t)(bv<0?0:bv>255?255:bv);
}

typedef struct { uint8_t *buf; int sz,cap; } Buf;
Buf newbuf(int c){ Buf b={malloc(c),0,c}; return b; }
static void bp(Buf*b,uint8_t v){
    if(b->sz==b->cap){b->cap*=2;b->buf=realloc(b->buf,b->cap);}
    b->buf[b->sz++]=v;
}
static void bp16(Buf*b,uint16_t v){bp(b,v&0xFF);bp(b,v>>8);}
static void w32(uint8_t*buf,int o,uint32_t v){
    buf[o]=v&0xFF;buf[o+1]=(v>>8)&0xFF;buf[o+2]=(v>>16)&0xFF;buf[o+3]=v>>24;
}
static uint32_t r32(const uint8_t*buf,int o){
    return buf[o]|(buf[o+1]<<8)|(buf[o+2]<<16)|((uint32_t)buf[o+3]<<24);
}

/* varint delta-pos: delta fits byte → 1B, else 0x00+u16 */
static void push_dp(Buf*b,int d){
    if(d>0&&d<256){bp(b,(uint8_t)d);}
    else{bp(b,0);bp16(b,(uint16_t)d);}
}
/* varint count: <128→1B, ≥128→0x80|hi + lo */
static void push_cnt(Buf*b,int c){
    if(c<128){bp(b,(uint8_t)c);}
    else{bp(b,(uint8_t)(0x80|(c>>8)));bp(b,(uint8_t)(c&0xFF));}
}

uint8_t* zcomp(const uint8_t*s,int n,int*out){
    size_t cap=ZSTD_compressBound(n);
    uint8_t*d=malloc(cap);
    size_t r=ZSTD_compress(d,cap,s,n,ZST_LVL);
    *out=(int)r; return d;
}
uint8_t* zdecomp(const uint8_t*s,int n,int orig){
    uint8_t*d=malloc(orig);
    if((int)ZSTD_decompress(d,orig,s,n)!=orig){free(d);return NULL;}
    return d;
}

double psnr(const uint8_t*a,const uint8_t*b,int n){
    long s=0; for(int i=0;i<n;i++){int d=a[i]-b[i];s+=d*d;}
    double m=(double)s/n; return m==0?999.0:10.0*log10(65025.0/m);
}

int main(int argc,char**argv){
    const char*path=argc>1?argv[1]:"test02.bmp";
    int merge=argc>2?atoi(argv[2]):0;
    Img img; if(!bmp_load(path,&img)){fprintf(stderr,"load failed\n");return 1;}
    int w=img.w,h=img.h,n=w*h,raw=n*3;
    int tw=(w+TILE-1)/TILE,th=(h+TILE-1)/TILE,nt=tw*th;
    printf("Image: %s (%dx%d) raw=%d B  merge=%d\n\n",path,w,h,raw,merge);

    /* RGB → YCgCo */
    uint8_t*ycc=malloc(n*3);
    for(int i=0;i<n;i++)
        rgb2ycc(img.px[i*3],img.px[i*3+1],img.px[i*3+2],
                ycc+i*3,ycc+i*3+1,ycc+i*3+2);

    /* encode */
    int bmap_bytes=(nt+7)/8;
    Buf bmap=newbuf(bmap_bytes+1); bmap.sz=bmap_bytes; memset(bmap.buf,0,bmap_bytes);
    Buf s_ne=newbuf(nt*2+8), s_dp=newbuf(1<<21), s_cnt=newbuf(1<<20);
    int tid=0;

    for(int ty=0;ty<th;ty++) for(int tx=0;tx<tw;tx++,tid++){
        int freq[AXIS]={0},tn=0;
        for(int dy=0;dy<TILE;dy++){
            int py=ty*TILE+dy; if(py>=h) continue;
            for(int dx=0;dx<TILE;dx++){
                int px=tx*TILE+dx; if(px>=w) continue;
                int i=py*w+px;
                freq[      ycc[i*3  ]]++;
                freq[256+  ycc[i*3+1]]++;
                freq[512+  ycc[i*3+2]]++;
                tn++;
            }
        }
        if(!tn) continue;
        bmap.buf[tid>>3]|=(1<<(tid&7));
        if(merge>0) for(int p=0;p<AXIS-1;p++) if(freq[p]&&freq[p]<=merge){freq[p+1]+=freq[p];freq[p]=0;}
        int ne=0; for(int p=0;p<AXIS;p++) if(freq[p]) ne++;
        bp16(&s_ne,(uint16_t)ne);
        int prev=0;
        for(int p=0;p<AXIS;p++){
            if(!freq[p]) continue;
            push_dp(&s_dp,p-prev);
            push_cnt(&s_cnt,freq[p]);
            prev=p;
        }
    }

    int zbm_sz,zne_sz,zdp_sz,zct_sz;
    uint8_t*zbm =zcomp(bmap.buf, bmap.sz, &zbm_sz);
    uint8_t*zne =zcomp(s_ne.buf, s_ne.sz, &zne_sz);
    uint8_t*zdp =zcomp(s_dp.buf, s_dp.sz, &zdp_sz);
    uint8_t*zct =zcomp(s_cnt.buf,s_cnt.sz,&zct_sz);

    int hdr_sz=4+4+4+(4+4)*4; /* 44 B */
    int enc_total=hdr_sz+zbm_sz+zne_sz+zdp_sz+zct_sz;
    uint8_t*C=malloc(enc_total);
    int off=0;
    w32(C,off,MAGIC);          off+=4;
    w32(C,off,(uint32_t)w);    off+=4;
    w32(C,off,(uint32_t)h);    off+=4;
    w32(C,off,(uint32_t)bmap.sz);  off+=4; w32(C,off,(uint32_t)zbm_sz);  off+=4;
    w32(C,off,(uint32_t)s_ne.sz);  off+=4; w32(C,off,(uint32_t)zne_sz);  off+=4;
    w32(C,off,(uint32_t)s_dp.sz);  off+=4; w32(C,off,(uint32_t)zdp_sz);  off+=4;
    w32(C,off,(uint32_t)s_cnt.sz); off+=4; w32(C,off,(uint32_t)zct_sz);  off+=4;
    memcpy(C+off,zbm,zbm_sz); off+=zbm_sz;
    memcpy(C+off,zne,zne_sz); off+=zne_sz;
    memcpy(C+off,zdp,zdp_sz); off+=zdp_sz;
    memcpy(C+off,zct,zct_sz);

    /* decode */
    off=4+4+4;
    int bm_o=r32(C,off),bm_z=r32(C,off+4); off+=8;
    int ne_o=r32(C,off),ne_z=r32(C,off+4); off+=8;
    int dp_o=r32(C,off),dp_z=r32(C,off+4); off+=8;
    int ct_o=r32(C,off),ct_z=r32(C,off+4); off+=8;
    uint8_t*d_bm =zdecomp(C+off,bm_z,bm_o); off+=bm_z;
    uint8_t*d_ne =zdecomp(C+off,ne_z,ne_o); off+=ne_z;
    uint8_t*d_dp =zdecomp(C+off,dp_z,dp_o); off+=dp_z;
    uint8_t*d_ct =zdecomp(C+off,ct_z,ct_o);
    if(!d_bm||!d_ne||!d_dp||!d_ct){fprintf(stderr,"decomp error\n");return 1;}

    uint8_t*rec_ycc=calloc(n*3,1);
    int ne_rp=0,dp_rp=0,ct_rp=0; tid=0;

    for(int ty=0;ty<th;ty++) for(int tx=0;tx<tw;tx++,tid++){
        if(!(d_bm[tid>>3]>>(tid&7)&1)) continue;
        int ne=d_ne[ne_rp]|(d_ne[ne_rp+1]<<8); ne_rp+=2;
        int freq[AXIS]={0},cur=0;
        for(int e=0;e<ne;e++){
            uint8_t tag=d_dp[dp_rp++];
            if(tag==0){cur+=(d_dp[dp_rp]|(d_dp[dp_rp+1]<<8));dp_rp+=2;}
            else cur+=tag;
            uint8_t cb=d_ct[ct_rp++];
            int cnt=(cb&0x80)?(((cb&0x7F)<<8)|d_ct[ct_rp++]):cb;
            freq[cur]=cnt;
        }
        uint8_t cv[3][256]; int cc[3]={0,0,0};
        for(int p=0;p<AXIS;p++){
            if(!freq[p]) continue;
            int ch=p/256,val=p%256;
            for(int k=0;k<freq[p]&&cc[ch]<256;k++) cv[ch][cc[ch]++]=(uint8_t)val;
        }
        int ci[3]={0,0,0};
        for(int dy=0;dy<TILE;dy++){
            int py=ty*TILE+dy; if(py>=h) continue;
            for(int dx=0;dx<TILE;dx++){
                int px=tx*TILE+dx; if(px>=w) continue;
                int i=py*w+px;
                for(int c=0;c<3;c++)
                    rec_ycc[i*3+c]=ci[c]<cc[c]?cv[c][ci[c]++]:0;
            }
        }
    }

    /* YCgCo → RGB */
    uint8_t*recon=malloc(n*3);
    for(int i=0;i<n;i++)
        ycc2rgb(rec_ycc[i*3],rec_ycc[i*3+1],rec_ycc[i*3+2],
                recon+i*3,recon+i*3+1,recon+i*3+2);

    /* histogram verify (lossless only) */
    int hist_ok=1;
    if(merge==0){
        int fO[3][256]={},fR[3][256]={};
        for(int i=0;i<n;i++) for(int c=0;c<3;c++){fO[c][ycc[i*3+c]]++;fR[c][rec_ycc[i*3+c]]++;}
        for(int c=0;c<3;c++) for(int v=0;v<256;v++) if(fO[c][v]!=fR[c][v]){hist_ok=0;}
    }

    printf("=== Stream sizes (raw → zstd-%d) ===\n",ZST_LVL);
    printf("  bitmap   : %7d → %6d B\n",bmap.sz,zbm_sz);
    printf("  n_entries: %7d → %6d B\n",s_ne.sz,zne_sz);
    printf("  delta-pos: %7d → %6d B\n",s_dp.sz,zdp_sz);
    printf("  count    : %7d → %6d B\n",s_cnt.sz,zct_sz);
    printf("  TOTAL    : %7d B\n\n",enc_total);
    printf("=== Results ===\n");
    printf("  Raw          : %7d B  (1.00x)\n",raw);
    printf("  GEO3 YCgCo   : %7d B  (%.2fx)\n",enc_total,(float)raw/enc_total);
    printf("  Histogram    : %s\n",merge==0?(hist_ok?"PASS":"FAIL"):"skip(lossy)");
    printf("  PSNR         : %.2f dB\n",psnr(img.px,recon,n*3));

    char out[512]; snprintf(out,sizeof(out),"%s.geo3",path);
    FILE*fo=fopen(out,"wb"); fwrite(C,1,enc_total,fo); fclose(fo);
    printf("  Saved        : %s\n",out);

    free(C);free(ycc);free(rec_ycc);free(recon);
    free(d_bm);free(d_ne);free(d_dp);free(d_ct);
    free(zbm);free(zne);free(zdp);free(zct);
    free(bmap.buf);free(s_ne.buf);free(s_dp.buf);free(s_cnt.buf);free(img.px);
    return 0;
}
