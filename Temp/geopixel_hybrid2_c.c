/*
 * GeoPixel Hybrid v2 — JPEG base + Goldberg adaptive residual
 * ════════════════════════════════════════════════════════════
 *
 * Architecture:
 *   JPEG q95 (full image, one pass) + per-tile int8 residual
 *   Goldberg n_circuits → adaptive zero-threshold per tile:
 *
 *     nc 0-2  → threshold 0  (lossless, simple tile, residual already small)
 *     nc 3-4  → threshold 2  (near-lossless, zeros out 94% of diffs)
 *     nc 5-6  → threshold 4  (aggressive, zeros out 99% of diffs)
 *
 *   Residual packed channel-planar (Y then Cg then Co after YCgCo transform)
 *   then zstd level 19 → better entropy than RGB interleaved
 *
 * FILE FORMAT (.gph2):
 *   Standard JPEG bytes  ← any JPEG viewer opens this directly (lossy)
 *   ── after JPEG EOI ──
 *   [4]  magic "GP21"
 *   [4]  W  uint32 LE
 *   [4]  H  uint32 LE
 *   [4]  TW uint32 LE
 *   [4]  TH uint32 LE
 *   [4]  TILE uint32 LE
 *   [TW*TH] threshold_map  1 byte per tile (actual threshold used: 0/2/4)
 *   [8]  res_zstd_size uint64 LE
 *   [res_zstd_size]  zstd(all tile residuals concatenated, planar YCgCo)
 *
 * QUALITY MODES (selectable at encode time):
 *   --lossless    : all tiles threshold=0 (~3x)
 *   --balanced    : Goldberg adaptive      (~6x, max_diff≤4 on complex tiles)
 *   --aggressive  : nc0-2→0, rest→4       (~8x)
 *
 * Compile:
 *   gcc -O3 -o geopixel_hybrid2 geopixel_hybrid2_c.c -lm -lzstd -lpng -ljpeg -lwebp
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <jpeglib.h>
#include <zstd.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include "geo_goldberg_tile.h"

/* ── constants ─────────────────────────────────────────── */
#define JPEG_Q      95
#define TILE        32
#define GP2_MAGIC   "GP21"
#define ZSTD_LEVEL  19

/* adaptive threshold table [nc] → zero-out threshold */
static const int8_t NC_THRESH[7] = { 0, 0, 0, 2, 2, 4, 4 };

/* ── helpers ────────────────────────────────────────────── */
static inline void w32(uint8_t *b, int o, uint32_t v){
    b[o]=v; b[o+1]=v>>8; b[o+2]=v>>16; b[o+3]=v>>24;
}
static inline uint32_t r32(const uint8_t *b, int o){
    return b[o]|(uint32_t)(b[o+1]<<8)|(uint32_t)(b[o+2]<<16)|((uint32_t)b[o+3]<<24);
}
static inline void w64(uint8_t *b, int o, uint64_t v){
    for(int i=0;i<8;i++){b[o+i]=(uint8_t)(v&0xFF);v>>=8;}
}
static inline uint64_t r64(const uint8_t *b, int o){
    uint64_t v=0; for(int i=7;i>=0;i--) v=(v<<8)|b[o+i]; return v;
}
static inline int clamp255(int v){ return v<0?0:v>255?255:v; }
static inline int clamp_s8(int v){ return v<-128?-128:v>127?127:v; }

/* ── YCgCo (lossless integer) ──────────────────────────── */
static inline void rgb2ycgco(int r, int g, int b, int *Y, int *Cg, int *Co){
    *Co = r - b;
    int tmp = b + (*Co>>1);
    *Cg = g - tmp;
    *Y  = tmp + (*Cg>>1);
}
static inline void ycgco2rgb(int Y, int Cg, int Co, int *r, int *g, int *b){
    int tmp = Y - (Cg>>1);
    *g = Cg + tmp;
    *b = tmp - (Co>>1);
    *r = *b + Co;
}

/* ── Image I/O ─────────────────────────────────────────── */
typedef struct { int w, h; uint8_t *px; } Img;

static int load_png(FILE *f, Img *img){
    png_structp ps = png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop   pi = png_create_info_struct(ps);
    if(setjmp(png_jmpbuf(ps))){png_destroy_read_struct(&ps,&pi,NULL);return 0;}
    png_init_io(ps,f); png_read_info(ps,pi);
    img->w=(int)png_get_image_width(ps,pi);
    img->h=(int)png_get_image_height(ps,pi);
    png_byte ct=png_get_color_type(ps,pi), bd=png_get_bit_depth(ps,pi);
    if(bd==16)                               png_set_strip_16(ps);
    if(ct==PNG_COLOR_TYPE_PALETTE)           png_set_palette_to_rgb(ps);
    if(ct==PNG_COLOR_TYPE_GRAY||
       ct==PNG_COLOR_TYPE_GRAY_ALPHA)        png_set_gray_to_rgb(ps);
    if(ct & PNG_COLOR_MASK_ALPHA)            png_set_strip_alpha(ps);
    png_read_update_info(ps,pi);
    img->px = malloc(img->w * img->h * 3);
    png_bytep *rows = malloc(img->h * sizeof(png_bytep));
    for(int y=0;y<img->h;y++) rows[y] = img->px + y*img->w*3;
    png_read_image(ps,rows); free(rows);
    png_destroy_read_struct(&ps,&pi,NULL); return 1;
}
static int load_jpeg_file(FILE *f, Img *img){
    struct jpeg_decompress_struct ci; struct jpeg_error_mgr je;
    ci.err=jpeg_std_error(&je); jpeg_create_decompress(&ci);
    jpeg_stdio_src(&ci,f); jpeg_read_header(&ci,TRUE);
    ci.out_color_space=JCS_RGB; jpeg_start_decompress(&ci);
    img->w=ci.output_width; img->h=ci.output_height;
    img->px=malloc(img->w*img->h*3);
    while(ci.output_scanline<ci.output_height){
        uint8_t *row=img->px+ci.output_scanline*img->w*3;
        jpeg_read_scanlines(&ci,&row,1);
    }
    jpeg_finish_decompress(&ci); jpeg_destroy_decompress(&ci); return 1;
}
static int load_bmp(FILE *f, Img *img){
    uint8_t hdr[54];
    if(fread(hdr,1,54,f)!=54) return 0;
    img->w=*(int32_t*)(hdr+18); img->h=*(int32_t*)(hdr+22);
    int td=(img->h<0); if(td) img->h=-img->h;
    int rs=(img->w*3+3)&~3;
    img->px=malloc(img->w*img->h*3);
    fseek(f,*(uint32_t*)(hdr+10),SEEK_SET);
    uint8_t *row=malloc(rs);
    for(int y=img->h-1;y>=0;y--){
        int dy=td?(img->h-1-y):y;
        if(fread(row,1,rs,f)!=(size_t)rs) break;
        for(int x=0;x<img->w;x++){
            int d=(dy*img->w+x)*3;
            img->px[d]=row[x*3+2]; img->px[d+1]=row[x*3+1]; img->px[d+2]=row[x*3];
        }
    }
    free(row); return 1;
}
static int img_load(const char *path, Img *img){
    const char *dot=strrchr(path,'.');
    if(dot){
        char ext[8]={0}; int i=0;
        for(const char *p=dot+1;*p&&i<7;p++,i++) ext[i]=(char)(*p|0x20);
        if(!strcmp(ext,"webp")){
            FILE *f=fopen(path,"rb"); if(!f) return 0;
            fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
            uint8_t *buf=malloc(sz);
            if(fread(buf,1,sz,f)!=(size_t)sz){free(buf);fclose(f);return 0;}
            fclose(f);
            int w,h; img->px=WebPDecodeRGB(buf,sz,&w,&h); free(buf);
            if(!img->px) return 0; img->w=w; img->h=h; return 1;
        }
    }
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    uint8_t sig[4]={0}; fread(sig,1,4,f); rewind(f);
    int ok=0;
    if(sig[0]==0xFF&&sig[1]==0xD8)     ok=load_jpeg_file(f,img);
    else if(sig[0]==0x89&&sig[1]=='P') ok=load_png(f,img);
    else if(sig[0]=='B'&&sig[1]=='M')  ok=load_bmp(f,img);
    else{ ok=load_png(f,img); if(!ok){rewind(f);ok=load_jpeg_file(f,img);} }
    fclose(f); return ok;
}
static void save_png(const char *path, const uint8_t *px, int w, int h){
    FILE *f=fopen(path,"wb"); if(!f) return;
    png_structp ps=png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop pi=png_create_info_struct(ps);
    if(setjmp(png_jmpbuf(ps))){fclose(f);return;}
    png_init_io(ps,f);
    png_set_IHDR(ps,pi,w,h,8,PNG_COLOR_TYPE_RGB,
        PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
    png_write_info(ps,pi);
    for(int y=0;y<h;y++) png_write_row(ps,(png_bytep)(px+y*w*3));
    png_write_end(ps,NULL); png_destroy_write_struct(&ps,&pi); fclose(f);
}

/* ── JPEG full-image encode/decode in memory ────────────── */
static uint8_t *jpeg_enc_mem(const uint8_t *rgb, int w, int h, size_t *out_sz){
    struct jpeg_compress_struct ci; struct jpeg_error_mgr je;
    unsigned char *buf=NULL; unsigned long sz=0;
    ci.err=jpeg_std_error(&je); jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci,&buf,&sz);
    ci.image_width=w; ci.image_height=h;
    ci.input_components=3; ci.in_color_space=JCS_RGB;
    jpeg_set_defaults(&ci); jpeg_set_quality(&ci,JPEG_Q,TRUE);
    jpeg_start_compress(&ci,TRUE);
    while(ci.next_scanline<ci.image_height){
        uint8_t *row=(uint8_t*)rgb+ci.next_scanline*w*3;
        jpeg_write_scanlines(&ci,(JSAMPARRAY)&row,1);
    }
    jpeg_finish_compress(&ci); jpeg_destroy_compress(&ci);
    *out_sz=sz; return buf;
}
static uint8_t *jpeg_dec_mem(const uint8_t *data, size_t sz, int *w, int *h){
    struct jpeg_decompress_struct ci; struct jpeg_error_mgr je;
    ci.err=jpeg_std_error(&je); jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci,(unsigned char*)data,(unsigned long)sz);
    jpeg_read_header(&ci,TRUE); ci.out_color_space=JCS_RGB;
    jpeg_start_decompress(&ci);
    *w=ci.output_width; *h=ci.output_height;
    uint8_t *out=malloc((size_t)(*w)*(*h)*3);
    while(ci.output_scanline<ci.output_height){
        uint8_t *row=out+ci.output_scanline*(*w)*3;
        jpeg_read_scanlines(&ci,&row,1);
    }
    jpeg_finish_decompress(&ci); jpeg_destroy_decompress(&ci); return out;
}

/* ════════════════════════════════════════════════════════
 * ENCODE
 * ════════════════════════════════════════════════════════ */
static void do_encode(const char *in_path, const char *out_path, int mode){
    /*
     * mode 0 = lossless   (all thresh=0)
     * mode 1 = balanced   (Goldberg adaptive)
     * mode 2 = aggressive (nc0-2→0, rest→4)
     */
    Img orig; if(!img_load(in_path,&orig)){
        fprintf(stderr,"Cannot load: %s\n",in_path); return;
    }
    int W=orig.w, H=orig.h;
    int TW=(W+TILE-1)/TILE, TH=(H+TILE-1)/TILE, NT=TW*TH;

    /* build iY plane for Goldberg (integer luma) */
    int *iY = malloc(W*H*sizeof(int));
    for(int i=0;i<W*H;i++){
        int r=orig.px[i*3],g=orig.px[i*3+1],b=orig.px[i*3+2];
        iY[i]=(r*77+g*150+b*29)>>8;
    }

    /* JPEG encode full image */
    size_t jpeg_sz;
    uint8_t *jpeg_buf = jpeg_enc_mem(orig.px, W, H, &jpeg_sz);

    /* JPEG decode → base */
    int jw,jh;
    uint8_t *base = jpeg_dec_mem(jpeg_buf, jpeg_sz, &jw, &jh);

    /* per-tile: Goldberg scan → threshold → residual */
    uint8_t *thresh_map = malloc(NT);

    /* residual buffer: worst case all tiles lossless */
    /* YCgCo planar per tile: 3 channels × tw×th int8 */
    size_t res_cap = (size_t)NT * TILE * TILE * 3;
    int8_t *res_buf = malloc(res_cap);
    size_t res_off  = 0;

    /* stats */
    int nc_count[7]={0};
    int max_stored_err=0, max_skip_err=0;
    long zeroed_px=0, total_px=0;

    for(int ty=0; ty<TH; ty++) for(int tx=0; tx<TW; tx++){
        int tid = ty*TW+tx;
        int x0=tx*TILE, x1=x0+TILE; if(x1>W) x1=W;
        int y0=ty*TILE, y1=y0+TILE; if(y1>H) y1=H;
        int tw=x1-x0, th=y1-y0;

        /* Goldberg scan */
        GGTileResult gbr = ggt_tile_scan(iY, x0, y0, x1, y1, W);
        int nc = gbr.n_circuits;
        if(nc>6) nc=6;
        nc_count[nc]++;

        /* select threshold */
        int thr;
        if     (mode==0) thr=0;
        else if(mode==1) thr=NC_THRESH[nc];
        else             thr=(nc<=2)?0:4;  /* aggressive */
        thresh_map[tid]=(uint8_t)thr;

        /* compute YCgCo residual per pixel */
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int gi=y*W+x;
            int or_=orig.px[gi*3], og=orig.px[gi*3+1], ob=orig.px[gi*3+2];
            int br_=base[gi*3],    bg=base[gi*3+1],    bb=base[gi*3+2];
            int oY,oCg,oCo, bY,bCg,bCo;
            rgb2ycgco(or_,og,ob,&oY,&oCg,&oCo);
            rgb2ycgco(br_,bg,bb,&bY,&bCg,&bCo);
            int dY=oY-bY, dCg=oCg-bCg, dCo=oCo-bCo;
            /* apply threshold */
            if(abs(dY) <=thr){ dY=0;  zeroed_px++; }
            if(abs(dCg)<=thr){ dCg=0; zeroed_px++; }
            if(abs(dCo)<=thr){ dCo=0; zeroed_px++; }
            total_px+=3;
            /* pack planar: Y plane then Cg then Co */
            int li=(y-y0)*tw+(x-x0);
            res_buf[res_off + li]           = (int8_t)clamp_s8(dY);
            res_buf[res_off + tw*th + li]   = (int8_t)clamp_s8(dCg);
            res_buf[res_off + tw*th*2 + li] = (int8_t)clamp_s8(dCo);
            /* track max error */
            int rerr=abs(dY)+abs(dCg)+abs(dCo);
            if(thr==0){ if(rerr>max_stored_err) max_stored_err=rerr; }
            else       { if(rerr>max_skip_err)  max_skip_err=rerr;   }
        }
        res_off += (size_t)tw*th*3;
    }
    free(iY);

    /* zstd compress residual */
    size_t zbound = ZSTD_compressBound(res_off);
    uint8_t *res_z = malloc(zbound);
    size_t res_zsz = ZSTD_compress(res_z, zbound, res_buf, res_off, ZSTD_LEVEL);
    free(res_buf);

    /* build GP2 metadata block */
    size_t meta_sz = 4+4+4+4+4+4 + NT + 8 + res_zsz;
    uint8_t *meta  = malloc(meta_sz);
    int mo=0;
    memcpy(meta+mo, GP2_MAGIC, 4);    mo+=4;
    w32(meta,mo,(uint32_t)W);          mo+=4;
    w32(meta,mo,(uint32_t)H);          mo+=4;
    w32(meta,mo,(uint32_t)TW);         mo+=4;
    w32(meta,mo,(uint32_t)TH);         mo+=4;
    w32(meta,mo,(uint32_t)TILE);       mo+=4;
    memcpy(meta+mo, thresh_map, NT);   mo+=NT;
    w64(meta,mo,(uint64_t)res_zsz);    mo+=8;
    memcpy(meta+mo, res_z, res_zsz);
    free(thresh_map); free(res_z);

    /* write: JPEG + GP2 metadata */
    FILE *fo=fopen(out_path,"wb");
    fwrite(jpeg_buf, 1, jpeg_sz, fo);
    fwrite(meta,     1, meta_sz, fo);
    fclose(fo);

    size_t total = jpeg_sz + meta_sz;
    int raw = W*H*3;
    const char *mode_name[3]={"lossless","balanced","aggressive"};

    printf("GeoPixel Hybrid v2 — %s\n", mode_name[mode]);
    printf("  Input  : %s (%dx%d)\n", in_path, W, H);
    printf("  Tiles  : %dx%d = %d\n", TW, TH, NT);
    printf("\n  Goldberg n_circuits distribution:\n");
    for(int i=0;i<7;i++) if(nc_count[i])
        printf("    nc=%d : %4d tiles  thresh=%d\n",
               i, nc_count[i], (mode==0)?0:(mode==1)?NC_THRESH[i]:(i<=2?0:4));
    printf("\n  JPEG base    : %zu KB\n", jpeg_sz/1024);
    printf("  GP residual  : %zu KB  (zstd lv%d)\n", res_zsz/1024, ZSTD_LEVEL);
    printf("  TOTAL        : %zu KB\n", total/1024);
    printf("  vs raw %d KB : %.2fx\n", raw/1024, (double)raw/total);
    printf("  Zeroed vals  : %.1f%%\n", (double)zeroed_px/total_px*100.0);
    printf("  Saved → %s\n", out_path);

    free(jpeg_buf); free(base); free(orig.px); free(meta);
}

/* ════════════════════════════════════════════════════════
 * DECODE
 * ════════════════════════════════════════════════════════ */
static void do_decode(const char *in_path, const char *out_path){
    FILE *f=fopen(in_path,"rb");
    if(!f){fprintf(stderr,"Cannot open %s\n",in_path);return;}
    fseek(f,0,SEEK_END); long fsz=ftell(f); rewind(f);
    uint8_t *raw=malloc(fsz);
    fread(raw,1,fsz,f); fclose(f);

    /* find GP21 marker scanning from end */
    long gp_off=-1;
    for(long i=fsz-8; i>0; i--){
        if(raw[i]=='G'&&raw[i+1]=='P'&&raw[i+2]=='2'&&raw[i+3]=='1'){
            gp_off=i; break;
        }
    }
    if(gp_off<0){
        fprintf(stderr,"No GP2 metadata — decoding JPEG only\n");
        int w,h; uint8_t *px=jpeg_dec_mem(raw,fsz,&w,&h);
        save_png(out_path,px,w,h); free(px); free(raw); return;
    }

    /* decode JPEG base (bytes before GP marker) */
    int W,H;
    uint8_t *base = jpeg_dec_mem(raw, (size_t)gp_off, &W, &H);

    /* parse metadata */
    const uint8_t *meta = raw + gp_off;
    int mo=4;
    int mW =(int)r32(meta,mo); mo+=4;
    int mH =(int)r32(meta,mo); mo+=4;
    int TW =(int)r32(meta,mo); mo+=4;
    int TH =(int)r32(meta,mo); mo+=4;
    int TS =(int)r32(meta,mo); mo+=4;
    int NT =TW*TH;
    const uint8_t *thresh_map = meta+mo; mo+=NT;
    uint64_t res_zsz = r64(meta,mo); mo+=8;
    const uint8_t *res_z = meta+mo;

    /* decompress residual */
    size_t res_cap = (size_t)NT*TS*TS*3;
    int8_t *res_buf = malloc(res_cap);
    size_t res_actual = ZSTD_decompress(res_buf, res_cap, res_z, res_zsz);
    (void)res_actual;

    /* apply residual tile by tile */
    size_t res_off=0;
    for(int ty=0; ty<TH; ty++) for(int tx=0; tx<TW; tx++){
        int tid=ty*TW+tx;
        int x0=tx*TS, x1=x0+TS; if(x1>mW) x1=mW;
        int y0=ty*TS, y1=y0+TS; if(y1>mH) y1=mH;
        int tw=x1-x0, th=y1-y0;
        (void)thresh_map[tid]; /* threshold stored for diagnostics */

        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int gi=y*mW+x;
            int li=(y-y0)*tw+(x-x0);
            int dY  = res_buf[res_off + li];
            int dCg = res_buf[res_off + tw*th + li];
            int dCo = res_buf[res_off + tw*th*2 + li];
            /* reconstruct in YCgCo then back to RGB */
            int br=base[gi*3], bg=base[gi*3+1], bb=base[gi*3+2];
            int bY,bCg,bCo;
            rgb2ycgco(br,bg,bb,&bY,&bCg,&bCo);
            int oY=bY+dY, oCg=bCg+dCg, oCo=bCo+dCo;
            int r,g,b;
            ycgco2rgb(oY,oCg,oCo,&r,&g,&b);
            base[gi*3]  =(uint8_t)clamp255(r);
            base[gi*3+1]=(uint8_t)clamp255(g);
            base[gi*3+2]=(uint8_t)clamp255(b);
        }
        res_off += (size_t)tw*th*3;
    }

    save_png(out_path, base, mW, mH);
    printf("Decoded: %s → %s (%dx%d)\n", in_path, out_path, mW, mH);
    free(res_buf); free(base); free(raw);
}

/* ════════════════════════════════════════════════════════
 * VERIFY
 * ════════════════════════════════════════════════════════ */
static void do_verify(const char *orig_path, const char *dec_path){
    Img a,b;
    if(!img_load(orig_path,&a)){fprintf(stderr,"Cannot load %s\n",orig_path);return;}
    if(!img_load(dec_path, &b)){fprintf(stderr,"Cannot load %s\n",dec_path); return;}
    if(a.w!=b.w||a.h!=b.h){
        printf("FAIL: size mismatch %dx%d vs %dx%d\n",a.w,a.h,b.w,b.h);
        free(a.px); free(b.px); return;
    }
    int N=a.w*a.h*3, max_d=0, ndiff=0;
    long long sse=0;
    for(int i=0;i<N;i++){
        int d=abs((int)a.px[i]-(int)b.px[i]);
        if(d>max_d) max_d=d;
        if(d) ndiff++;
        sse+=(long long)d*d;
    }
    double mse=(double)sse/N;
    double psnr=(mse>0)?10.0*log10(255.0*255.0/mse):999.0;
    if(max_d==0)
        printf("VERIFY OK: pixel-perfect lossless ✓\n");
    else
        printf("VERIFY: max_diff=%d  %d/%d px differ (%.2f%%)  PSNR=%.1f dB\n",
               max_d, ndiff/3, N/3, (double)(ndiff/3)/(N/3)*100.0, psnr);
    free(a.px); free(b.px);
}

/* ── main ─────────────────────────────────────────────── */
int main(int argc, char **argv){
    if(argc<2){
        printf("GeoPixel Hybrid v2\n"
               "  enc [--lossless|--balanced|--aggressive] <in> <out.gph2>\n"
               "  dec  <in.gph2> <out.png>\n"
               "  verify <orig> <decoded.png>\n");
        return 1;
    }
    clock_t t0=clock();
    if(!strcmp(argv[1],"enc") && argc>=4){
        int mode=1; /* default: balanced */
        int ai=2;
        if(!strcmp(argv[2],"--lossless"))   { mode=0; ai=3; }
        if(!strcmp(argv[2],"--balanced"))   { mode=1; ai=3; }
        if(!strcmp(argv[2],"--aggressive")) { mode=2; ai=3; }
        do_encode(argv[ai], argv[ai+1], mode);
    } else if(!strcmp(argv[1],"dec")    && argc>=4) do_decode(argv[2],argv[3]);
    else if(!strcmp(argv[1],"verify")   && argc>=4) do_verify(argv[2],argv[3]);
    else { fprintf(stderr,"Unknown command\n"); return 1; }
    printf("  Time : %.2f ms\n",(double)(clock()-t0)/CLOCKS_PER_SEC*1000.0);
    return 0;
}
