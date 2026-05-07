/*
 * GeoPixel Hybrid — JPEG base + GP metadata residual
 * ════════════════════════════════════════════════════
 *
 * FILE FORMAT (.gph):
 *   Standard JPEG bytes  (q95, full image — any JPEG viewer can open)
 *   ── appended after JPEG EOI (0xFFD9) ──
 *   [4]  magic  "GP20"
 *   [4]  width  uint32 LE
 *   [4]  height uint32 LE
 *   [4]  ntiles uint32 LE  (TW*TH, tile=32)
 *   [ntiles/8 rounded up]  residual_map  (bit=1 → tile has residual)
 *   [8]  res_zstd_size  uint64 LE
 *   [res_zstd_size]  zstd( packed int8 residuals for flagged tiles )
 *                    order: tile scan order, only tiles with bit=1
 *                    each tile: tw*th*3 int8 values (RGB diff)
 *
 * DECODE (lossless):
 *   1. Decode JPEG → base image
 *   2. Find "GP20" marker after EOI
 *   3. For each flagged tile: base[tile] += residual[tile]
 *
 * DECODE (lossy, no GP metadata):
 *   Just decode the JPEG — any standard viewer
 *
 * Goldberg circuit decides which tiles get residual stored:
 *   n_circuits 0-2  → store residual (simple tile, small residual)
 *   n_circuits 3-6  → no residual  (complex tile, JPEG is good enough)
 *   FLAT            → store residual (exact palette, residual near-zero)
 *
 * Compile:
 *   gcc -O3 -o geopixel_hybrid geopixel_hybrid_c.c -lm -lzstd -lpng -ljpeg -lwebp
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

#define JPEG_Q      95
#define TILE        32
#define GP_MAGIC    "GP20"

/* ── helpers ──────────────────────────────────────────── */
static inline void w32(uint8_t *b, int o, uint32_t v){
    b[o]=v; b[o+1]=v>>8; b[o+2]=v>>16; b[o+3]=v>>24;
}
static inline uint32_t r32(const uint8_t *b, int o){
    return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((uint32_t)b[o+3]<<24);
}
static inline void w64(uint8_t *b, int o, uint64_t v){
    for(int i=0;i<8;i++){b[o+i]=(uint8_t)(v&0xFF);v>>=8;}
}
static inline uint64_t r64(const uint8_t *b, int o){
    uint64_t v=0; for(int i=7;i>=0;i--) v=(v<<8)|b[o+i]; return v;
}
static inline int clamp255(int v){ return v<0?0:v>255?255:v; }

/* ── image I/O (BMP/PNG/JPEG/WebP → packed RGB) ──────── */
typedef struct { int w, h; uint8_t *px; } Img;

static int load_png(FILE *f, Img *img){
    png_structp ps=png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop pi=png_create_info_struct(ps);
    if(setjmp(png_jmpbuf(ps))){png_destroy_read_struct(&ps,&pi,NULL);return 0;}
    png_init_io(ps,f); png_read_info(ps,pi);
    img->w=(int)png_get_image_width(ps,pi);
    img->h=(int)png_get_image_height(ps,pi);
    png_byte ct=png_get_color_type(ps,pi);
    png_byte bd=png_get_bit_depth(ps,pi);
    if(bd==16) png_set_strip_16(ps);
    if(ct==PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(ps);
    if(ct==PNG_COLOR_TYPE_GRAY||ct==PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(ps);
    if(ct&PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(ps);
    png_read_update_info(ps,pi);
    img->px=malloc(img->w*img->h*3);
    png_bytep *rows=malloc(img->h*sizeof(png_bytep));
    for(int y=0;y<img->h;y++) rows[y]=img->px+y*img->w*3;
    png_read_image(ps,rows); free(rows);
    png_destroy_read_struct(&ps,&pi,NULL); return 1;
}
static int load_jpeg(FILE *f, Img *img){
    struct jpeg_decompress_struct ci; struct jpeg_error_mgr je;
    ci.err=jpeg_std_error(&je);
    jpeg_create_decompress(&ci); jpeg_stdio_src(&ci,f);
    jpeg_read_header(&ci,TRUE); ci.out_color_space=JCS_RGB;
    jpeg_start_decompress(&ci);
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
            img->px[d+0]=row[x*3+2]; img->px[d+1]=row[x*3+1]; img->px[d+2]=row[x*3+0];
        }
    }
    free(row); return 1;
}
static int img_load(const char *path, Img *img){
    const char *dot=strrchr(path,'.');
    if(dot){
        char ext[8]; int i=0;
        for(const char *p=dot+1;*p&&i<7;p++,i++) ext[i]=(char)(*p|0x20);
        ext[i]='\0';
        if(!strcmp(ext,"webp")){
            FILE *f=fopen(path,"rb"); if(!f) return 0;
            fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
            uint8_t *buf=malloc(sz); fread(buf,1,sz,f); fclose(f);
            int w,h; img->px=WebPDecodeRGB(buf,sz,&w,&h); free(buf);
            if(!img->px) return 0; img->w=w; img->h=h; return 1;
        }
    }
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    int ok=0;
    uint8_t sig[4]; fread(sig,1,4,f); rewind(f);
    if(sig[0]==0xFF&&sig[1]==0xD8)          ok=load_jpeg(f,img);
    else if(sig[0]==0x89&&sig[1]=='P')      ok=load_png(f,img);
    else if(sig[0]=='B'&&sig[1]=='M')       ok=load_bmp(f,img);
    else { ok=load_png(f,img); if(!ok){rewind(f);ok=load_jpeg(f,img);} }
    fclose(f); return ok;
}

/* save decoded result as PNG */
static void save_png(const char *path, const uint8_t *px, int w, int h){
    FILE *f=fopen(path,"wb"); if(!f) return;
    png_structp ps=png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop pi=png_create_info_struct(ps);
    if(setjmp(png_jmpbuf(ps))){fclose(f);return;}
    png_init_io(ps,f);
    png_set_IHDR(ps,pi,w,h,8,PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
    png_write_info(ps,pi);
    for(int y=0;y<h;y++) png_write_row(ps,px+y*w*3);
    png_write_end(ps,NULL); png_destroy_write_struct(&ps,&pi); fclose(f);
}

/* ── JPEG encode full image to memory ─────────────────── */
static uint8_t *jpeg_encode_mem(const uint8_t *rgb, int w, int h, size_t *out_sz){
    struct jpeg_compress_struct ci; struct jpeg_error_mgr je;
    unsigned char *buf=NULL; unsigned long sz=0;
    ci.err=jpeg_std_error(&je);
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci,&buf,&sz);
    ci.image_width=w; ci.image_height=h;
    ci.input_components=3; ci.in_color_space=JCS_RGB;
    jpeg_set_defaults(&ci);
    jpeg_set_quality(&ci,JPEG_Q,TRUE);
    jpeg_start_compress(&ci,TRUE);
    while(ci.next_scanline<ci.image_height){
        uint8_t *row=(uint8_t*)rgb+ci.next_scanline*w*3;
        jpeg_write_scanlines(&ci,(JSAMPARRAY)&row,1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    *out_sz=sz; return buf;
}

/* ── JPEG decode from memory ──────────────────────────── */
static uint8_t *jpeg_decode_mem(const uint8_t *data, size_t sz, int *w, int *h){
    struct jpeg_decompress_struct ci; struct jpeg_error_mgr je;
    ci.err=jpeg_std_error(&je);
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci,(unsigned char*)data,(unsigned long)sz);
    jpeg_read_header(&ci,TRUE); ci.out_color_space=JCS_RGB;
    jpeg_start_decompress(&ci);
    *w=ci.output_width; *h=ci.output_height;
    uint8_t *out=malloc((size_t)(*w)*(*h)*3);
    while(ci.output_scanline<ci.output_height){
        uint8_t *row=out+ci.output_scanline*(*w)*3;
        jpeg_read_scanlines(&ci,&row,1);
    }
    jpeg_finish_decompress(&ci); jpeg_destroy_decompress(&ci);
    return out;
}

/* ════════════════════════════════════════════════════════
 * ENCODE
 * ════════════════════════════════════════════════════════ */
static void do_encode(const char *in_path, const char *out_gph){
    Img orig; if(!img_load(in_path,&orig)){
        fprintf(stderr,"Cannot load: %s\n",in_path); return;
    }
    int W=orig.w, H=orig.h;
    int TW=(W+TILE-1)/TILE, TH=(H+TILE-1)/TILE, NT=TW*TH;

    /* Step 1: JPEG q95 encode full image */
    size_t jpeg_sz;
    uint8_t *jpeg_buf = jpeg_encode_mem(orig.px, W, H, &jpeg_sz);

    /* Step 2: JPEG decode back → base image */
    int jw, jh;
    uint8_t *base = jpeg_decode_mem(jpeg_buf, jpeg_sz, &jw, &jh);

    /* Step 3: Per-tile Goldberg scan → decide residual map */
    /* We need iY plane for Goldberg scan — build it */
    int N = W*H;
    int *iY = malloc(N*sizeof(int));
    for(int i=0;i<N;i++){
        int r=orig.px[i*3], g=orig.px[i*3+1], b=orig.px[i*3+2];
        /* simple luma for Goldberg scan (doesn't need to be exact) */
        iY[i] = (r*77 + g*150 + b*29) >> 8;
    }

    /* residual_map: 1 bit per tile */
    int map_bytes = (NT + 7) / 8;
    uint8_t *res_map = calloc(map_bytes, 1);

    /* compute ALL residuals; pack only flagged tiles */
    /* max residual buffer: NT tiles * TILE*TILE*3 bytes */
    size_t res_cap = (size_t)NT * TILE * TILE * 3;
    int8_t *res_buf = malloc(res_cap);
    size_t res_off = 0;

    int n_residual = 0, n_skip = 0;
    int max_diff_stored = 0, max_diff_skipped = 0;

    for(int ty=0; ty<TH; ty++) for(int tx=0; tx<TW; tx++){
        int tid = ty*TW + tx;
        int x0=tx*TILE, x1=x0+TILE; if(x1>W) x1=W;
        int y0=ty*TILE, y1=y0+TILE; if(y1>H) y1=H;
        int tw=x1-x0, th=y1-y0, tn=tw*th;

        /* Goldberg scan */
        GGTileResult gbr = ggt_tile_scan(iY, x0, y0, x1, y1, W);

        /* compute tile residual (orig - jpeg_base) regardless */
        int tile_max = 0;
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int gi=y*W+x;
            for(int c=0;c<3;c++){
                int d=(int)orig.px[gi*3+c]-(int)base[gi*3+c];
                if(abs(d)>tile_max) tile_max=abs(d);
            }
        }

        /* store residual for ALL tiles — lossless guarantee */
        int store = 1;
        if(store){
            res_map[tid>>3] |= (uint8_t)(1u<<(tid&7));
            for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
                int gi=y*W+x;
                for(int c=0;c<3;c++){
                    int d=(int)orig.px[gi*3+c]-(int)base[gi*3+c];
                    if(d>127)d=127; if(d<-128)d=-128;
                    res_buf[res_off++]=(int8_t)d;
                }
            }
            n_residual++;
            if(tile_max>max_diff_stored) max_diff_stored=tile_max;
        } else {
            n_skip++;
            if(tile_max>max_diff_skipped) max_diff_skipped=tile_max;
        }
    }
    free(iY);

    /* Step 4: zstd compress packed residuals */
    size_t res_zbound = ZSTD_compressBound(res_off);
    uint8_t *res_z = malloc(res_zbound);
    size_t res_zsz = (res_off > 0)
        ? ZSTD_compress(res_z, res_zbound, res_buf, res_off, 9)
        : 0;
    free(res_buf);

    /* Step 5: build GP metadata block */
    size_t meta_sz = 4 + 4 + 4 + 4 + map_bytes + 8 + res_zsz;
    uint8_t *meta = malloc(meta_sz);
    int mo = 0;
    memcpy(meta+mo, GP_MAGIC, 4); mo += 4;
    w32(meta, mo, (uint32_t)W);   mo += 4;
    w32(meta, mo, (uint32_t)H);   mo += 4;
    w32(meta, mo, (uint32_t)NT);  mo += 4;
    memcpy(meta+mo, res_map, map_bytes); mo += map_bytes;
    w64(meta, mo, (uint64_t)res_zsz);   mo += 8;
    memcpy(meta+mo, res_z, res_zsz);
    free(res_map); free(res_z);

    /* Step 6: write .gph = JPEG bytes + GP metadata */
    FILE *fo = fopen(out_gph, "wb");
    fwrite(jpeg_buf, 1, jpeg_sz, fo);
    fwrite(meta, 1, meta_sz, fo);
    fclose(fo);

    size_t total = jpeg_sz + meta_sz;
    double pct_res = (double)n_residual / NT * 100.0;

    printf("GeoPixel Hybrid encode: %s (%dx%d)\n", in_path, W, H);
    printf("  Tiles total  : %d  (%dx%d)\n", NT, TW, TH);
    printf("  Tiles w/ res : %d  (%.1f%%)  n_circuits 0-2  max_diff=%d\n",
           n_residual, pct_res, max_diff_stored);
    printf("  Tiles skip   : %d  (%.1f%%)  n_circuits 3-6  max_diff=%d (JPEG)\n",
           n_skip, 100.0-pct_res, max_diff_skipped);
    printf("\n  JPEG base    : %zu KB\n", jpeg_sz/1024);
    printf("  GP metadata  : %zu KB  (map=%dB  res_zstd=%zu KB)\n",
           meta_sz/1024, map_bytes, res_zsz/1024);
    printf("  TOTAL        : %zu KB  (%.2fx vs raw %d KB)\n",
           total/1024, (double)(W*H*3)/total, W*H*3/1024);
    printf("  Saved → %s\n", out_gph);

    free(jpeg_buf); free(base); free(orig.px);
}

/* ════════════════════════════════════════════════════════
 * DECODE
 * ════════════════════════════════════════════════════════ */
static void do_decode(const char *in_gph, const char *out_png){
    /* read full file */
    FILE *f = fopen(in_gph,"rb"); if(!f){fprintf(stderr,"Cannot open %s\n",in_gph);return;}
    fseek(f,0,SEEK_END); long fsz=ftell(f); rewind(f);
    uint8_t *raw = malloc(fsz);
    fread(raw,1,fsz,f); fclose(f);

    /* find JPEG EOI (0xFF 0xD9) scanning from end */
    long gp_off = -1;
    for(long i=fsz-4; i>0; i--){
        if(raw[i]==0xFF && raw[i+1]==0xD9 &&
           raw[i+2]=='G' && raw[i+3]=='P'){
            gp_off = i+2; break;
        }
    }
    if(gp_off < 0){
        fprintf(stderr,"No GP metadata found — decoding JPEG only\n");
        /* fallback: decode JPEG as-is */
        int w,h;
        uint8_t *px = jpeg_decode_mem(raw, fsz, &w, &h);
        save_png(out_png, px, w, h); free(px); free(raw);
        return;
    }

    size_t jpeg_sz = (size_t)gp_off - 0;  /* EOI is at gp_off-2 */
    /* actually EOI is the 0xFFD9 two bytes before GP_MAGIC */
    jpeg_sz = (size_t)(gp_off - 2) + 2;   /* include EOI in JPEG */

    /* decode JPEG base */
    int W, H;
    uint8_t *base = jpeg_decode_mem(raw, jpeg_sz, &W, &H);

    /* parse GP metadata */
    const uint8_t *meta = raw + gp_off;
    if(memcmp(meta, GP_MAGIC, 4)!=0){
        fprintf(stderr,"Bad GP magic\n"); free(base); free(raw); return;
    }
    int mo = 4;
    int mW = (int)r32(meta,mo); mo+=4;
    int mH = (int)r32(meta,mo); mo+=4;
    int NT = (int)r32(meta,mo); mo+=4;
    int TW = (mW+TILE-1)/TILE;
    int map_bytes = (NT+7)/8;
    const uint8_t *res_map = meta + mo; mo += map_bytes;
    uint64_t res_zsz = r64(meta, mo); mo += 8;
    const uint8_t *res_z = meta + mo;

    /* decompress residuals */
    /* max uncompressed: NT * TILE*TILE*3 */
    size_t res_cap = (size_t)NT * TILE * TILE * 3;
    int8_t *res_buf = malloc(res_cap);
    size_t res_actual = 0;
    if(res_zsz > 0)
        res_actual = ZSTD_decompress(res_buf, res_cap, res_z, res_zsz);

    /* apply residuals to base */
    size_t res_off = 0;
    for(int ty=0; ty<(mH+TILE-1)/TILE; ty++) for(int tx=0; tx<TW; tx++){
        int tid = ty*TW + tx;
        int x0=tx*TILE, x1=x0+TILE; if(x1>mW) x1=mW;
        int y0=ty*TILE, y1=y0+TILE; if(y1>mH) y1=mH;
        if(!(res_map[tid>>3] & (1u<<(tid&7)))) continue;  /* no residual */
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int gi=y*mW+x;
            for(int c=0;c<3;c++){
                if(res_off >= res_actual) break;
                int v = (int)base[gi*3+c] + (int)res_buf[res_off++];
                base[gi*3+c] = (uint8_t)clamp255(v);
            }
        }
    }

    save_png(out_png, base, mW, mH);
    printf("Decoded: %s → %s (%dx%d)\n", in_gph, out_png, mW, mH);

    free(res_buf); free(base); free(raw);
}

/* ════════════════════════════════════════════════════════
 * VERIFY
 * ════════════════════════════════════════════════════════ */
static void do_verify(const char *orig_path, const char *dec_path){
    Img a, b;
    if(!img_load(orig_path,&a)){fprintf(stderr,"Cannot load %s\n",orig_path);return;}
    if(!img_load(dec_path, &b)){fprintf(stderr,"Cannot load %s\n",dec_path);return;}
    if(a.w!=b.w||a.h!=b.h){
        printf("FAIL: size mismatch %dx%d vs %dx%d\n",a.w,a.h,b.w,b.h);
        free(a.px); free(b.px); return;
    }
    int N=a.w*a.h*3, max_d=0, ndiff=0;
    for(int i=0;i<N;i++){
        int d=abs((int)a.px[i]-(int)b.px[i]);
        if(d>max_d) max_d=d;
        if(d) ndiff++;
    }
    if(max_d==0)
        printf("VERIFY OK: pixel-perfect lossless (0 diff)\n");
    else
        printf("VERIFY: max_diff=%d  %d/%d pixels differ (%.3f%%)\n",
               max_d, ndiff/3, N/3, (double)(ndiff/3)/(N/3)*100.0);
    free(a.px); free(b.px);
}

/* ── main ─────────────────────────────────────────────── */
int main(int argc, char **argv){
    if(argc<2){
        printf("GeoPixel Hybrid\n"
               "  enc    : %s enc <input.{png,jpg,webp,bmp}> <out.gph>\n"
               "  dec    : %s dec <in.gph> <out.png>\n"
               "  verify : %s verify <orig> <decoded.png>\n",
               argv[0],argv[0],argv[0]);
        return 1;
    }
    clock_t t0=clock();
    if     (!strcmp(argv[1],"enc")    && argc>=4) do_encode(argv[2],argv[3]);
    else if(!strcmp(argv[1],"dec")    && argc>=4) do_decode(argv[2],argv[3]);
    else if(!strcmp(argv[1],"verify") && argc>=4) do_verify(argv[2],argv[3]);
    else { fprintf(stderr,"Unknown command\n"); return 1; }
    printf("  Time : %.2f ms\n",(double)(clock()-t0)/CLOCKS_PER_SEC*1000.0);
    return 0;
}
