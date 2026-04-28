#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// GEOPIXEL ULTRA - High Performance Lossless Encoder
// Key improvements:
// 1. Better color decorrelation (YCgCo-R style)
// 2. 2D spatial prediction (LOCO-I like)
// 3. Golomb-Rice entropy coding with adaptive k
// 4. Bit-packing for compact output
//
// Compile: gcc -O3 -o geopixel_ultra geopixel_ultra.c -lm
// Usage:   ./geopixel_ultra image.bmp

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

// Improved: 2D prediction using left, top, top-left neighbors
static inline int predict_2d(const uint8_t *plane, int w, int h, int x, int y) {
    if (x == 0 && y == 0) return 128;  // Neutral center
    if (x == 0) return plane[(y-1)*w];  // top
    if (y == 0) return plane[y*w + x-1]; // left
    int left = plane[y*w + x-1];
    int top = plane[(y-1)*w + x];
    int tl = plane[(y-1)*w + x-1];
    // LOCO-I / CALIC style gradient-adjusted predictor
    if (left >= tl && top >= tl) return (left < top) ? left : top;
    if (left <= tl && top <= tl) return (left > top) ? left : top;
    return left + top - tl;  // planar
}

// YCgCo-R inspired color transform (reversible, better decorrelation)
static inline void rgb_to_ycc(int r, int g, int b, int *y, int *u, int *v) {
    *y = (r + 2*g + b) >> 2;           // Luma
    *u = r - g;                         // Red-Green diff
    *v = b - g;                         // Blue-Green diff
}

static inline void ycc_to_rgb(int y, int u, int v, int *r, int *g, int *b) {
    *g = y - ((u + v) >> 2);
    *r = u + *g;
    *b = v + *g;
}

// Cylindrical mapping (unchanged)
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

// Golomb-Rice encoding parameters
typedef struct {
    uint8_t *bits;
    int bit_pos;
    int total_bits;
} BitWriter;

void bw_init(BitWriter *bw, int max_bytes) {
    bw->bits = calloc(max_bytes, 1);
    bw->bit_pos = 0;
    bw->total_bits = 0;
}

void bw_write_bits(BitWriter *bw, uint32_t val, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        int bit = (val >> i) & 1;
        bw->bits[bw->bit_pos >> 3] |= (bit << (7 - (bw->bit_pos & 7)));
        bw->bit_pos++;
        bw->total_bits++;
    }
}

void bw_write_gr(BitWriter *bw, int val, int k) {
    // Golomb-Rice: quotient in unary, remainder in binary
    int q = val >> k;
    int r = val & ((1 << k) - 1);
    // Unary quotient
    for (int i = 0; i < q; i++) bw_write_bits(bw, 1, 1);
    bw_write_bits(bw, 0, 1);  // stop bit
    // Binary remainder
    if (k > 0) bw_write_bits(bw, r, k);
}

uint8_t* bw_finalize(BitWriter *bw, int *out_sz) {
    *out_sz = (bw->bit_pos + 7) >> 3;
    return bw->bits;
}

typedef struct {
    const uint8_t *bits;
    int bit_pos;
    int total_bits;
} BitReader;

void br_init(BitReader *br, const uint8_t *bits, int nbits) {
    br->bits = bits;
    br->bit_pos = 0;
    br->total_bits = nbits;
}

uint32_t br_read_bits(BitReader *br, int nbits) {
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++) {
        if (br->bit_pos >= br->total_bits) break;
        int bit = (br->bits[br->bit_pos >> 3] >> (7 - (br->bit_pos & 7))) & 1;
        val = (val << 1) | bit;
        br->bit_pos++;
    }
    return val;
}

int br_read_gr(BitReader *br, int k) {
    // Read unary quotient
    int q = 0;
    while (br_read_bits(br, 1) == 1) q++;
    // Read binary remainder
    int r = 0;
    if (k > 0) r = br_read_bits(br, k);
    return (q << k) | r;
}

// Encode plane with 2D prediction + Golomb-Rice
uint8_t* encode_plane_gr(const uint8_t *plane, int w, int h, int *out_sz) {
    BitWriter bw;
    bw_init(&bw, w * h * 3);  // Allocate buffer
    
    // Adaptive Golomb-Rice: start with k=1 (small errors expected), adapt quickly
    int k = 1;
    int err_sum = 0, count = 0;
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int pred = predict_2d(plane, w, h, x, y);
            int err = (int)plane[y*w + x] - pred;
            // Map signed error to unsigned: 0->0, -1->1, 1->2, -2->3, ...
            uint32_t uerr = (err < 0) ? (-err * 2 - 1) : (err * 2);
            bw_write_gr(&bw, uerr, k);
            
            // Accumulate error stats for adaptation
            err_sum += uerr;
            count++;
            
            // Adapt k every 32 pixels for faster response
            if (count == 32) {
                int avg = err_sum / 32;
                // Optimal k ≈ log2(avg+1), clamp to [0,7]
                if (avg <= 1) k = 0;
                else if (avg <= 3) k = 1;
                else if (avg <= 7) k = 2;
                else if (avg <= 15) k = 3;
                else if (avg <= 31) k = 4;
                else if (avg <= 63) k = 5;
                else if (avg <= 127) k = 6;
                else k = 7;
                err_sum = 0;
                count = 0;
            }
        }
    }
    
    return bw_finalize(&bw, out_sz);
}

uint8_t* decode_plane_gr(const uint8_t *buf, int w, int h, int buf_sz) {
    BitReader br;
    br_init(&br, buf, buf_sz * 8);
    
    uint8_t *plane = malloc(w * h);
    int k = 1;  // Match encoder
    int err_sum = 0, count = 0;
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t uerr = br_read_gr(&br, k);
            int err = (uerr & 1) ? -((uerr + 1) >> 1) : (uerr >> 1);
            int pred = predict_2d(plane, w, h, x, y);
            int val = pred + err;
            plane[y*w + x] = (uint8_t)(val < 0 ? 0 : (val > 255 ? 255 : val));
            
            // Same adaptation logic as encoder
            err_sum += uerr;
            count++;
            if (count == 32) {
                int avg = err_sum / 32;
                if (avg <= 1) k = 0;
                else if (avg <= 3) k = 1;
                else if (avg <= 7) k = 2;
                else if (avg <= 15) k = 3;
                else if (avg <= 31) k = 4;
                else if (avg <= 63) k = 5;
                else if (avg <= 127) k = 6;
                else k = 7;
                err_sum = 0;
                count = 0;
            }
        }
    }
    
    return plane;
}

// Simple entropy estimator
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
    int w=img.w,h=img.h,n=w*h,raw=n*3;
    printf("Image: %s (%dx%d)\n\n",path,w,h);

    // Color transform: RGB -> Y, U=R-G, V=B-G
    uint8_t *y=malloc(n),*u=malloc(n),*v=malloc(n);
    for(int i=0;i<n;i++){
        int rv=img.px[i*3],gv=img.px[i*3+1],bv=img.px[i*3+2];
        int yy, uu, vv;
        rgb_to_ycc(rv, gv, bv, &yy, &uu, &vv);
        // Shift to unsigned
        y[i] = (uint8_t)yy;
        u[i] = (uint8_t)(uu + 128);
        v[i] = (uint8_t)(vv + 128);
    }

    // Encode 3 planes with Golomb-Rice
    uint8_t *planes[3]={y,u,v};
    uint8_t *enc[3]; int enc_sz[3];
    clock_t t0 = clock();
    for(int c=0;c<3;c++) enc[c]=encode_plane_gr(planes[c],w,h,&enc_sz[c]);
    clock_t t1 = clock();
    
    // Decode + verify
    uint8_t *dec[3];
    for(int c=0;c<3;c++) dec[c]=decode_plane_gr(enc[c],w,h,enc_sz[c]);
    clock_t t2 = clock();

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
        int yy = dec[0][i];
        int uu = dec[1][i] - 128;
        int vv = dec[2][i] - 128;
        int rr, gg, bb;
        ycc_to_rgb(yy, uu, vv, &rr, &gg, &bb);
        recon[i*3+0]=(uint8_t)(rr<0?0:rr>255?255:rr);
        recon[i*3+1]=(uint8_t)(gg<0?0:gg>255?255:gg);
        recon[i*3+2]=(uint8_t)(bb<0?0:bb>255?255:bb);
    }

    // Size analysis
    const char *pname[3]={"Y  ","U  ","V  "};
    long total_enc=0;
    printf("=== Streams ===\n");
    for(int c=0;c<3;c++){
        printf("  [%s] encoded=%dB  (%.3f bpp)\n",
               pname[c],enc_sz[c],(float)enc_sz[c]*8/n);
        total_enc+=enc_sz[c];
    }

    printf("\n=== Results ===\n");
    printf("  Raw          : %7d B  (1.00x)\n",raw);
    printf("  Encoded      : %7ld B  (%.2fx)\n",total_enc,(float)raw/total_enc);
    printf("  PNG          : ~393847 B  (reference)\n");
    printf("  Plane verify : %s\n",all_ok?"PASS":"FAIL");
    printf("  PSNR         : %.2f dB\n",psnr(img.px,recon,n*3));
    printf("  Encode time  : %.2f ms\n",(double)(t1-t0)/CLOCKS_PER_SEC*1000);
    printf("  Decode time  : %.2f ms\n",(double)(t2-t1)/CLOCKS_PER_SEC*1000);

    for(int c=0;c<3;c++){free(enc[c]);free(dec[c]);}
    free(y);free(u);free(v);free(recon);free(img.px);
    return 0;
}
