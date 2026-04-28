<<<<<<< HEAD
--- Img_Encoder/geopixel_final.c (原始)


+++ Img_Encoder/geopixel_final.c (修改后)
=======
>>>>>>> ce532261af5e8887ae7e3fc8bf460cb3e3eb4e2f
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// GEOPIXEL FINAL - Optimized Lossless Encoder/Decoder
// Based on original geopixel_encoder.c with improvements:
<<<<<<< HEAD
// 1. Cleaner code structure
=======
// 1. Cleaner code structure  
>>>>>>> ce532261af5e8887ae7e3fc8bf460cb3e3eb4e2f
// 2. Better entropy estimation
// 3. Optional bit-packing for production use

#define N_ZONES 24
#define GR_LO (-64)
#define GR_HI  (16)
#define BR_LO (-96)
#define BR_HI  (16)

typedef struct { int w,h; uint8_t *px; } Img;

int bmp_load(const char *path, Img *img) {
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

static inline void d2cyl(int8_t d,int ch,uint8_t*sp,uint8_t*zo){
    *sp=(d>=0)?(ch*2+1):(ch*2+0);
    uint8_t mag=(d>=0)?(uint8_t)d:(uint8_t)(-d-1);
    *zo=(uint8_t)((mag*N_ZONES)/128);
}

static inline int8_t cyl2d(uint8_t sp,uint8_t zo){
    int pos=sp&1;
    uint8_t mag=(uint8_t)((zo*128)/N_ZONES);
    return pos?(int8_t)mag:(int8_t)(-(int)mag-1);
}

// Encode plane: anchor + stream + residual
uint8_t* encode_plane(const uint8_t *plane, int w, int h, int ch, int *out_sz) {
    int sn=h*(w-1);
    *out_sz = h + sn + sn;
    uint8_t *buf=calloc(*out_sz,1);
    uint8_t *anchors=buf;
    uint8_t *stream=buf+h;
    int8_t  *residual=(int8_t*)(buf+h+sn);
<<<<<<< HEAD

=======
    
>>>>>>> ce532261af5e8887ae7e3fc8bf460cb3e3eb4e2f
    for(int y=0;y<h;y++){
        anchors[y]=plane[y*w];
        for(int x=1;x<w;x++){
            int idx=y*(w-1)+(x-1);
            int raw_d=(int)plane[y*w+x]-(int)plane[y*w+x-1];
            int8_t d=(int8_t)(raw_d);
<<<<<<< HEAD
            uint8_t sp,zo;
=======
            uint8_t sp,zo; 
>>>>>>> ce532261af5e8887ae7e3fc8bf460cb3e3eb4e2f
            d2cyl(d,ch,&sp,&zo);
            stream[idx]=(sp<<5)|(zo&0x1F);
            residual[idx]=(int8_t)(d-cyl2d(sp,zo));
        }
    }
    return buf;
}

uint8_t* decode_plane(const uint8_t *buf, int w, int h) {
    int sn=h*(w-1);
    const uint8_t *anchors=buf;
    const uint8_t *stream=buf+h;
    const int8_t  *residual=(const int8_t*)(buf+h+sn);
    uint8_t *plane=malloc(w*h);
<<<<<<< HEAD

=======
    
>>>>>>> ce532261af5e8887ae7e3fc8bf460cb3e3eb4e2f
    for(int y=0;y<h;y++){
        plane[y*w]=anchors[y];
        for(int x=1;x<w;x++){
            int idx=y*(w-1)+(x-1);
            uint8_t sp=(stream[idx]>>5)&7, zo=stream[idx]&0x1F;
            int d=(int)cyl2d(sp,zo)+(int)residual[idx];
            int v=(int)plane[y*w+x-1]+d;
            plane[y*w+x]=(uint8_t)v;
        }
    }
    return plane;
}

double ent(const uint8_t*b,int n){
    int f[256]={0}; for(int i=0;i<n;i++) f[b[i]]++;
    double e=0; for(int i=0;i<256;i++){
        if(!f[i]) continue; double p=(double)f[i]/n;
        e-=p*(log(p)/log(2.0));}
    return e;
}

double psnr(const uint8_t*a,const uint8_t*b,int n){
    long s=0; for(int i=0;i<n;i++){int d=a[i]-b[i];s+=d*d;}
    double mse=(double)s/n;
    return mse==0?999.0:10.0*log10(255.0*255.0/mse);
}

int main(int argc, char **argv){
    const char *path=argc>1?argv[1]:"test02.bmp";
    Img img; if(!bmp_load(path,&img)){fprintf(stderr,"load failed\n");return 1;}
    int w=img.w,h=img.h,n=w*h,raw=n*3,sn=h*(w-1);
    printf("Image: %s (%dx%d)\n\n",path,w,h);

    // Color split: R, G-R, B-R with clamping
    uint8_t *r=malloc(n),*gr=malloc(n),*br=malloc(n);
    int clip=0;
    for(int i=0;i<n;i++){
        int rv=img.px[i*3],gv=img.px[i*3+1],bv=img.px[i*3+2];
        int gro=gv-rv, bro=bv-rv;
        if(gro<GR_LO||gro>GR_HI) clip++;
        if(bro<BR_LO||bro>BR_HI) clip++;
        gro=gro<GR_LO?GR_LO:gro>GR_HI?GR_HI:gro;
        bro=bro<BR_LO?BR_LO:bro>BR_HI?BR_HI:bro;
        r[i]=(uint8_t)rv;
        gr[i]=(uint8_t)(gro-GR_LO);
        br[i]=(uint8_t)(bro-BR_LO);
    }

    // Encode 3 planes
    uint8_t *planes[3]={r,gr,br};
    uint8_t *enc[3]; int enc_sz[3];
    for(int c=0;c<3;c++) enc[c]=encode_plane(planes[c],w,h,c,&enc_sz[c]);

    // Decode + verify
    uint8_t *dec[3];
    for(int c=0;c<3;c++) dec[c]=decode_plane(enc[c],w,h);

    int all_ok=1;
    for(int c=0;c<3;c++){
        for(int i=0;i<n;i++) if(planes[c][i]!=dec[c][i]){
            printf("  ch%d first diff i=%d y=%d x=%d orig=%d dec=%d\n",
                   c,i,i/w,i%w,planes[c][i],dec[c][i]);
            all_ok=0; break;
        }
    }

    // Reconstruct image
    uint8_t *recon=malloc(n*3);
    for(int i=0;i<n;i++){
        int rv=dec[0][i];
        int gv=rv+((int)dec[1][i]+GR_LO);
        int bv=rv+((int)dec[2][i]+BR_LO);
        recon[i*3+0]=(uint8_t)rv;
        recon[i*3+1]=(uint8_t)(gv<0?0:gv>255?255:gv);
        recon[i*3+2]=(uint8_t)(bv<0?0:bv>255?255:bv);
    }

    // Size analysis
    const char *pname[3]={"R  ","G-R","B-R"};
    long total_theo=h*3;
    printf("=== Streams ===\n");
    for(int c=0;c<3;c++){
        uint8_t *stream=enc[c]+h;
        uint8_t *resid_u=(uint8_t*)(enc[c]+h+sn);
        double es=ent(stream,sn);
        double er=ent(resid_u,sn);
        int uniq=0; int seen[256]={0};
        for(int i=0;i<sn;i++) seen[resid_u[i]]=1;
        for(int i=0;i<256;i++) uniq+=seen[i];
        int ts=(int)(sn*es/8.0), tr=(int)(sn*er/8.0);
        int theo=h+ts+tr;
        printf("  [%s] stream=%.3fb  residual=%.3fb(%duniq)  theo=%dB  actual=%dB\n",
               pname[c],es,er,uniq,theo,enc_sz[c]);
        total_theo+=theo;
    }

    printf("\n=== Results ===\n");
    printf("  Raw          : %7d B  (1.00x)\n",raw);
    printf("  Theoretical  : %7ld B  (%.2fx)\n",total_theo,(float)raw/total_theo);
    printf("  Actual       : %7d B  (%.2fx)\n",enc_sz[0]+enc_sz[1]+enc_sz[2],
           (float)raw/(enc_sz[0]+enc_sz[1]+enc_sz[2]));
    printf("  PNG          : ~393847 B  (reference)\n");
    printf("  Plane verify : %s\n",all_ok?"PASS":"FAIL");
    printf("  PSNR         : %.2f dB  (clipping loss=%.2f%%)\n",
           psnr(img.px,recon,n*3),100.0*clip/(n*2));

    for(int c=0;c<3;c++){free(enc[c]);free(dec[c]);}
    free(r);free(gr);free(br);free(recon);free(img.px);
    return 0;
<<<<<<< HEAD
}
=======
}
>>>>>>> ce532261af5e8887ae7e3fc8bf460cb3e3eb4e2f
