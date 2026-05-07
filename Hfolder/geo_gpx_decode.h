/*
 * geo_gpx_decode.h  —  Standalone GPX2 decoder  (header-only)
 *
 * Dependencies:  geo_o4_connector.h, libpng, libzstd
 * Usage:
 *   #define GEO_GPX_DECODE_IMPL   (once, before include)
 *   #include "geo_gpx_decode.h"
 *
 *   int gpx_decode_to_bmp(const char *gpx, const char *bmp_out);
 *   // returns 0 on success, non-zero on error
 */
#ifndef GEO_GPX_DECODE_H
#define GEO_GPX_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public API */
int gpx_decode_to_bmp(const char *gpx_path, const char *out_bmp);

/*
 * Optional: decode into caller-owned RGB buffer (W*H*3, row-major).
 * Caller must free *px_out. *w_out and *h_out are set on success.
 */
int gpx_decode_to_rgb(const char *gpx_path,
                      uint8_t **px_out, int *w_out, int *h_out);

#ifdef __cplusplus
}
#endif

/* ═══════════════════════════════════════════════════════════════════
 * IMPLEMENTATION
 * ═══════════════════════════════════════════════════════════════════ */
#ifdef GEO_GPX_DECODE_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#include <png.h>
#include "geo_o4_connector.h"

/* ── Sacred constants (DO NOT CHANGE) ─────────────────────────────*/
#define _GPX_TILE          32
#define _GPX_MAGIC_0       'G'
#define _GPX_MAGIC_1       'P'
#define _GPX_MAGIC_2       'X'
#define _GPX_MAGIC_3       '2'
#define _GPX_HDR_BYTES     16
#define _GPX_TABLE_ENTRY   6    /* 2B grid_rows + 4B blob_sz */

/* tile blob constants */
#define _TTYPE_FLAT        0
#define _BMODE_LINEAR2     1
#define _BMODE_DELTA       2
#define _BMODE_GRAD9       3
#define _FLAG_RAW_BASE     0x01
#define _PID_LOOSE_FLAG    0x80
#define _PID_MASK          0x7F
#define _PRED_MED          0
#define _PRED_GRAD         1
#define _MAX_FLAT_COLORS   4

/* ── Inline helpers ────────────────────────────────────────────────*/
static inline void _gpx_ycgco_to_rgb(int Y,int Cg,int Co,int*r,int*g,int*b){
    int t=Y-(Cg>>1); *g=Cg+t; *b=t-(Co>>1); *r=*b+Co;
}
static inline int  _gpx_clamp255(int v){return v<0?0:v>255?255:v;}
static inline int16_t _gpx_unzigzag(uint16_t v){return (int16_t)((v>>1)^-(v&1));}
static inline uint16_t _gpx_r16(const uint8_t*b,int o){return(uint16_t)(b[o]|(b[o+1]<<8));}
static inline uint32_t _gpx_r32(const uint8_t*b,int o){
    return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((uint32_t)b[o+3]<<24);
}
static inline int _gpx_unpack_i6(uint8_t v){
    return (v&0x20) ? (int)v-(int)64 : (int)(v&0x3F);
}

/* ── Boundary decode ───────────────────────────────────────────────*/
static void _gpx_bdec_linear2(const uint8_t*blob,int*off,
        int*col,int*row,int*corner,int th,int tw)
{
    int Y0=(int)(int8_t)blob[(*off)++];
    int dx=(int)(int8_t)blob[(*off)++];
    int dy=(int)(int8_t)blob[(*off)++];
    *corner=Y0;
    for(int y=0;y<th;y++) col[y]=Y0+dy*y/16;
    for(int x=0;x<tw;x++) row[x]=Y0+dx*x/16;
}

static void _gpx_bdec_delta(const uint8_t*blob,int*off,
        int*col,int*row,int*corner,int th,int tw)
{
    int c=(int)_gpx_unzigzag(_gpx_r16(blob,*off)); (*off)+=2;
    *corner=c;
    int prev=c;
    for(int y=0;y<th;y++){
        int d=(int)_gpx_unzigzag((uint16_t)blob[(*off)++]);
        prev+=d; col[y]=prev;
    }
    prev=c;
    for(int x=0;x<tw;x++){
        int d=(int)_gpx_unzigzag((uint16_t)blob[(*off)++]);
        prev+=d; row[x]=prev;
    }
}

static void _gpx_bdec_grad9(const uint8_t*blob,int*off,
        int*col,int*row,int*corner,int th,int tw)
{
    int Y0=(int)(int8_t)blob[(*off)++];
    uint16_t pk=(uint16_t)(blob[*off]|(blob[(*off)+1]<<8)); (*off)+=2;
    int dx_q=_gpx_unpack_i6((uint8_t)((pk>>10)&0x3F));
    int dy_q=_gpx_unpack_i6((uint8_t)((pk>>4) &0x3F));
    int scale=(int)(pk&0xF); if(scale==0) scale=1;
    int dx=dx_q*scale, dy=dy_q*scale;
    *corner=Y0;
    for(int y=0;y<th;y++) col[y]=Y0+dy*y;
    for(int x=0;x<tw;x++) row[x]=Y0+dx*x;
}

static void _gpx_bdec_ch(int bmode,const uint8_t*blob,int*off,
        int*col,int*row,int*corner,int th,int tw)
{
    if     (bmode==_BMODE_GRAD9)   _gpx_bdec_grad9  (blob,off,col,row,corner,th,tw);
    else if(bmode==_BMODE_LINEAR2) _gpx_bdec_linear2(blob,off,col,row,corner,th,tw);
    else                           _gpx_bdec_delta   (blob,off,col,row,corner,th,tw);
}

/* ── Local predictor ───────────────────────────────────────────────*/
static inline int _gpx_predict(int pid,
        const int*tl,int x,int y,int tw,
        const int*cl,const int*ct,int ctl,int def)
{
    int L = x>0 ? tl[y*tw+x-1]   : (cl?cl[y]:def);
    int T = y>0 ? tl[(y-1)*tw+x] : (ct?ct[x]:def);
    int TL;
    if     (x>0&&y>0) TL=tl[(y-1)*tw+x-1];
    else if(x==0&&y>0) TL=cl?cl[y-1]:def;
    else if(x>0&&y==0) TL=ct?ct[x-1]:def;
    else               TL=ctl;

    if(pid==_PRED_GRAD) return L+T-TL;
    if(pid==_PRED_MED){
        int hi=L>T?L:T, lo=L<T?L:T;
        if(TL>=hi) return lo;
        if(TL<=lo) return hi;
        return L+T-TL;
    }
    return L;  /* PRED_LEFT / fallback */
}

/* ── Core tile blob → pixels ───────────────────────────────────────*/
static void _gpx_decode_tile_blob(
        const uint8_t *blob, int blob_sz,
        int x0,int y0,int x1,int y1,int W,
        uint8_t *px_out)
{
    (void)blob_sz;
    int tw=x1-x0, th=y1-y0, tn=tw*th;
    int off=0;
    int ttype=blob[off++];

    /* ── FLAT ── */
    if(ttype==_TTYPE_FLAT){
        int n_pal=blob[off++];
        int palY[_MAX_FLAT_COLORS],palCg[_MAX_FLAT_COLORS],palCo[_MAX_FLAT_COLORS];
        for(int k=0;k<n_pal;k++){
            palY [k]=(int)_gpx_unzigzag(_gpx_r16(blob,off)); off+=2;
            palCg[k]=(int)_gpx_unzigzag(_gpx_r16(blob,off)); off+=2;
            palCo[k]=(int)_gpx_unzigzag(_gpx_r16(blob,off)); off+=2;
        }
        int li=0;
        while(li<tn&&off<blob_sz-1){
            int run=blob[off++]; int idx=blob[off++];
            if(idx>=n_pal) idx=0;
            for(int k=0;k<run&&li<tn;k++,li++){
                int lx=li%tw, ly=li/tw, gi=(y0+ly)*W+(x0+lx);
                int r,g,b;
                _gpx_ycgco_to_rgb(palY[idx],palCg[idx],palCo[idx],&r,&g,&b);
                px_out[gi*3+0]=(uint8_t)_gpx_clamp255(r);
                px_out[gi*3+1]=(uint8_t)_gpx_clamp255(g);
                px_out[gi*3+2]=(uint8_t)_gpx_clamp255(b);
            }
        }
        return;
    }

    /* ── Predictive ── */
    int bmode    = blob[off++];
    int pid_raw  = blob[off++];
    int pid      = pid_raw & _PID_MASK;
    int is_loose = (pid_raw & _PID_LOOSE_FLAG) != 0;
    uint8_t raw_flags = blob[off++];
    int loose_bias = 0;
    if(is_loose) loose_bias = (int)blob[off++];

    int def_Y=128, def_C=0;
    int meanY =(int)_gpx_unzigzag(_gpx_r16(blob,off)); off+=2;
    int meanCg=(int)_gpx_unzigzag(_gpx_r16(blob,off)); off+=2;
    int meanCo=(int)_gpx_unzigzag(_gpx_r16(blob,off)); off+=2;

    int *cLY =malloc(th*4),*cLCg=malloc(th*4),*cLCo=malloc(th*4);
    int *cTY =malloc(tw*4),*cTCg=malloc(tw*4),*cTCo=malloc(tw*4);
    int tl_Y,tl_Cg,tl_Co;

    _gpx_bdec_ch(bmode,blob,&off,cLY, cTY, &tl_Y, th,tw);
    _gpx_bdec_ch(bmode,blob,&off,cLCg,cTCg,&tl_Cg,th,tw);
    _gpx_bdec_ch(bmode,blob,&off,cLCo,cTCo,&tl_Co,th,tw);

    int zsz[6]; for(int s=0;s<6;s++){zsz[s]=(int)_gpx_r32(blob,off);off+=4;}
    uint8_t *dstr[6];
    for(int s=0;s<6;s++){
        dstr[s]=malloc(tn);
        if(raw_flags&(uint8_t)(_FLAG_RAW_BASE<<s))
            memcpy(dstr[s],blob+off,zsz[s]);
        else
            ZSTD_decompress(dstr[s],tn,blob+off,zsz[s]);
        off+=zsz[s];
    }

    int *locY =malloc(tn*4),*locCg=malloc(tn*4),*locCo=malloc(tn*4);
    for(int y=0;y<th;y++) for(int x=0;x<tw;x++){
        int li=y*tw+x, gi=(y0+y)*W+(x0+x);
        uint16_t zy =(uint16_t)(dstr[1][li]|(dstr[0][li]<<8));
        uint16_t zcg=(uint16_t)(dstr[3][li]|(dstr[2][li]<<8));
        uint16_t zco=(uint16_t)(dstr[5][li]|(dstr[4][li]<<8));
        int pY =_gpx_predict(pid,locY, x,y,tw,cLY, cTY, tl_Y, def_Y-meanY);
        int pCg=_gpx_predict(pid,locCg,x,y,tw,cLCg,cTCg,tl_Cg,def_C-meanCg);
        int pCo=_gpx_predict(pid,locCo,x,y,tw,cLCo,cTCo,tl_Co,def_C-meanCo);
        if(is_loose){ pY+=loose_bias; pCg+=loose_bias; pCo+=loose_bias; }
        locY [li]=(int)_gpx_unzigzag(zy) +pY;
        locCg[li]=(int)_gpx_unzigzag(zcg)+pCg;
        locCo[li]=(int)_gpx_unzigzag(zco)+pCo;
        int r,g,b;
        _gpx_ycgco_to_rgb(locY[li]+meanY,locCg[li]+meanCg,locCo[li]+meanCo,&r,&g,&b);
        px_out[gi*3+0]=(uint8_t)_gpx_clamp255(r);
        px_out[gi*3+1]=(uint8_t)_gpx_clamp255(g);
        px_out[gi*3+2]=(uint8_t)_gpx_clamp255(b);
    }
    for(int s=0;s<6;s++) free(dstr[s]);
    free(locY); free(locCg); free(locCo);
    free(cLY);  free(cLCg);  free(cLCo);
    free(cTY);  free(cTCg);  free(cTCo);
}

/* ── PNG read from memory buffer via fmemopen ──────────────────────*/
static uint8_t* _gpx_png_decode(const uint8_t*buf, size_t bufsz,
                                 uint32_t *out_rows)
{
    FILE *mf = fmemopen((void*)buf, bufsz, "rb");
    if(!mf) return NULL;
    png_structp pp = png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop   pi = png_create_info_struct(pp);
    if(setjmp(png_jmpbuf(pp))){ png_destroy_read_struct(&pp,&pi,NULL); fclose(mf); return NULL; }
    png_init_io(pp,mf);
    png_read_info(pp,pi);
    uint32_t W = png_get_image_width(pp,pi);
    uint32_t H = png_get_image_height(pp,pi);
    uint8_t *rgb = (uint8_t*)malloc(W*H*3);
    for(uint32_t r=0;r<H;r++)
        png_read_row(pp, rgb+r*W*3, NULL);
    png_read_end(pp,pi);
    png_destroy_read_struct(&pp,&pi,NULL);
    fclose(mf);
    *out_rows = H;
    return rgb;
}

/* ── Public: decode .gpx → RGB buffer ─────────────────────────────*/
int gpx_decode_to_rgb(const char *gpx_path,
                      uint8_t **px_out, int *w_out, int *h_out)
{
    /* 1. Read file */
    FILE *f = fopen(gpx_path,"rb");
    if(!f) return 1;
    fseek(f,0,SEEK_END); long fsz=ftell(f); rewind(f);
    uint8_t *raw=(uint8_t*)malloc((size_t)fsz);
    if((long)fread(raw,1,(size_t)fsz,f)!=fsz){ fclose(f); free(raw); return 1; }
    fclose(f);

    /* 2. Parse header */
    if(fsz<16 || raw[0]!=_GPX_MAGIC_0||raw[1]!=_GPX_MAGIC_1||
                 raw[2]!=_GPX_MAGIC_2||raw[3]!=_GPX_MAGIC_3){
        free(raw); return 2;
    }
    int TW=(raw[4]<<8)|raw[5], TH=(raw[6]<<8)|raw[7];
    int NT=(raw[8]<<24)|(raw[9]<<16)|(raw[10]<<8)|raw[11];
    uint32_t total_rows=(raw[12]<<24)|(raw[13]<<16)|(raw[14]<<8)|raw[15];
    int W=TW*_GPX_TILE, H=TH*_GPX_TILE;

    /* 3. Tile table */
    uint16_t *tile_rows=(uint16_t*)malloc(NT*sizeof(uint16_t));
    uint32_t *tile_bsz =(uint32_t*)malloc(NT*sizeof(uint32_t));
    for(int i=0;i<NT;i++){
        int base=16+i*_GPX_TABLE_ENTRY;
        tile_rows[i]=(uint16_t)((raw[base]<<8)|raw[base+1]);
        tile_bsz[i] =((uint32_t)raw[base+2]<<24)|((uint32_t)raw[base+3]<<16)
                     |((uint32_t)raw[base+4]<<8)|(uint32_t)raw[base+5];
    }
    long png_offset=16+NT*_GPX_TABLE_ENTRY;

    /* 4. PNG → geo RGB grid */
    uint32_t actual_rows=0;
    uint8_t *gpx_rgb=_gpx_png_decode(raw+png_offset,(size_t)(fsz-png_offset),&actual_rows);
    free(raw);
    if(!gpx_rgb||actual_rows!=total_rows){
        free(tile_rows); free(tile_bsz); free(gpx_rgb); return 3;
    }

    /* 5. Per-tile: O4 decode → blob → pixels */
    uint8_t *img=(uint8_t*)calloc((size_t)W*H*3,1);
    uint32_t row_cursor=0;
    for(int i=0;i<NT;i++){
        uint32_t gh=tile_rows[i];
        int tx=i%TW, ty=i/TW;
        int x0=tx*_GPX_TILE, x1=x0+_GPX_TILE; if(x1>W) x1=W;
        int y0=ty*_GPX_TILE, y1=y0+_GPX_TILE; if(y1>H) y1=H;

        O4GridCtx ctx;
        memset(&ctx,0,sizeof(ctx));
        ctx.grid_h=(uint32_t)gh;
        ctx.tile_x=(uint8_t)tx;
        ctx.tile_y=(uint8_t)ty;
        for(uint32_t r=0;r<gh&&r<O4_MAX_GRID_H;r++){
            const uint8_t *row_rgb=gpx_rgb+(row_cursor+r)*O4_GRID_W*3;
            for(uint32_t c=0;c<O4_GRID_W;c++){
                ctx.grid[r][c].r=row_rgb[c*3+0];
                ctx.grid[r][c].g=row_rgb[c*3+1];
                ctx.grid[r][c].b=row_rgb[c*3+2];
            }
        }
        row_cursor+=gh;

        uint32_t n_slots=gh*O4_GRID_W;
        ctx.n_slots=n_slots;
        uint32_t bsz=tile_bsz[i];
        uint8_t *blob=(uint8_t*)malloc(bsz);
        o4_decode(&ctx,blob,n_slots,bsz);
        _gpx_decode_tile_blob(blob,bsz,x0,y0,x1,y1,W,img);
        free(blob);
    }
    free(tile_rows); free(tile_bsz); free(gpx_rgb);

    *px_out=img; *w_out=W; *h_out=H;
    return 0;
}

/* ── Public: decode .gpx → BMP file ───────────────────────────────*/
int gpx_decode_to_bmp(const char *gpx_path, const char *out_bmp)
{
    uint8_t *img; int W,H;
    int err=gpx_decode_to_rgb(gpx_path,&img,&W,&H);
    if(err) return err;

    int row_bytes=W*3, pad=(4-row_bytes%4)%4;
    int bmp_row=row_bytes+pad, img_sz=bmp_row*H;
    FILE *fo=fopen(out_bmp,"wb");
    if(!fo){ free(img); return 4; }

    /* BMP header */
    uint8_t bh[54]={0};
    bh[0]='B'; bh[1]='M';
    uint32_t fsize=(uint32_t)(54+img_sz);
    memcpy(bh+2,&fsize,4); bh[10]=54;
    uint32_t dib_sz=40; memcpy(bh+14,&dib_sz,4);
    int32_t bW=W, bH=H; /* positive H = bottom-up (standard) */
    memcpy(bh+18,&bW,4); memcpy(bh+22,&bH,4);
    bh[26]=1; bh[28]=24;
    memcpy(bh+34,&img_sz,4);
    fwrite(bh,1,54,fo);

    uint8_t pad_bytes[3]={0,0,0};
    for(int y=H-1;y>=0;y--){   /* bottom-up like original bmp_save */
        for(int x=0;x<W;x++){
            uint8_t r=img[(y*W+x)*3+0];
            uint8_t g=img[(y*W+x)*3+1];
            uint8_t b=img[(y*W+x)*3+2];
            fputc(b,fo); fputc(g,fo); fputc(r,fo);  /* BMP = BGR */
        }
        if(pad) fwrite(pad_bytes,1,pad,fo);
    }
    fclose(fo);
    free(img);
    return 0;
}

/* cleanup internal defines */
#undef _GPX_TILE
#undef _GPX_MAGIC_0
#undef _GPX_MAGIC_1
#undef _GPX_MAGIC_2
#undef _GPX_MAGIC_3
#undef _GPX_HDR_BYTES
#undef _GPX_TABLE_ENTRY
#undef _TTYPE_FLAT
#undef _BMODE_LINEAR2
#undef _BMODE_DELTA
#undef _BMODE_GRAD9
#undef _FLAG_RAW_BASE
#undef _PID_LOOSE_FLAG
#undef _PID_MASK
#undef _PRED_MED
#undef _PRED_GRAD
#undef _MAX_FLAT_COLORS

#endif /* GEO_GPX_DECODE_IMPL */
#endif /* GEO_GPX_DECODE_H */
