/*
 * geo_gpx_anim.h — GPX4 animation encoder / decoder  (header-only, v2)
 *
 * Keyframe path: tile RGB → ZSTD blob → O4 geometric encode → PNG
 * Delta path:    per-tile YCgCo int8 diff → ZSTD → concatenated blob
 *
 * GPX4 animation file layout:
 *   AHDR  — Gpx4AnimHdr  (16B, no tile table)
 *   F000  — O4 keyframe  (tile table + PNG blob of 27×H O4 grid)
 *   D001  — DELTA frame  (tile table + concatenated ZSTD delta blobs)
 *   D002  — ...
 *   F016  — O4 keyframe  (every keyframe_interval frames)
 *
 * Dependencies: gpx4_container.h, geo_o4_connector.h, libpng, libzstd
 *
 * Usage:
 *   #define GEO_GPX_ANIM_IMPL
 *   #include "geo_gpx_anim.h"
 *
 *   GpxAnimEncCfg cfg = {24,1,16,9,32};
 *   gpx_anim_encode(frames, n_frames, W, H, &cfg, "out.gpx4");
 *   gpx_anim_decode("out.gpx4", my_cb, userdata);
 */
#ifndef GEO_GPX_ANIM_H
#define GEO_GPX_ANIM_H

#include <stdint.h>
#include "gpx4_container.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t fps_num;
    uint16_t fps_den;
    uint16_t keyframe_interval; /* 0 = only frame 0 is keyframe */
    int      zstd_level;        /* 1-19, default 9              */
    int      tile_sz;           /* must be 32                   */
} GpxAnimEncCfg;

int gpx_anim_encode(uint8_t **frames, int n_frames, int W, int H,
                    const GpxAnimEncCfg *cfg, const char *out_path);

typedef int (*GpxAnimFrameCb)(int frame_idx, uint8_t *rgb, void *ud);
int gpx_anim_decode(const char *path, GpxAnimFrameCb cb, void *ud);
int gpx_anim_info  (const char *path, Gpx4AnimHdr *hdr_out);

#ifdef __cplusplus
}
#endif

#ifdef GEO_GPX_ANIM_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#include <png.h>
#include "geo_o4_connector.h"

#define _GA_TILE        32
#define _GA_MAX_FRAMES  4096

/* YCgCo-R */
static inline void _ga_to_ycgco(int r,int g,int b,int*Y,int*Cg,int*Co){
    *Co=r-b; int t=b+(*Co>>1); *Cg=g-t; *Y=t+(*Cg>>1);
}
static inline void _ga_to_rgb(int Y,int Cg,int Co,int*r,int*g,int*b){
    int t=Y-(Cg>>1); *g=Cg+t; *b=t-(Co>>1); *r=*b+Co;
}
static inline int _ga_clamp(int v){ return v<0?0:v>255?255:v; }
static inline int8_t _ga_ci8(int v){ return (int8_t)(v<-127?-127:v>127?127:v); }

/* PNG encode: RGB grid -> memory blob via tmpfile */
static uint8_t* _ga_png_enc(const uint8_t *rgb, uint32_t W, uint32_t H,
                              uint32_t *out_sz)
{
    *out_sz=0;
    FILE *tmp=tmpfile(); if(!tmp) return NULL;
    png_structp pp=png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop   pi=png_create_info_struct(pp);
    if(setjmp(png_jmpbuf(pp))){
        png_destroy_write_struct(&pp,&pi); fclose(tmp); return NULL;
    }
    png_init_io(pp,tmp);
    png_set_IHDR(pp,pi,W,H,8,PNG_COLOR_TYPE_RGB,PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
    png_write_info(pp,pi);
    for(uint32_t r=0;r<H;r++)
        png_write_row(pp,(png_bytep)(rgb+r*W*3));
    png_write_end(pp,pi);
    png_destroy_write_struct(&pp,&pi);
    long psz=ftell(tmp); rewind(tmp);
    uint8_t *buf=(uint8_t*)malloc((size_t)psz);
    if((long)fread(buf,1,(size_t)psz,tmp)!=psz){ free(buf); fclose(tmp); return NULL; }
    fclose(tmp);
    *out_sz=(uint32_t)psz;
    return buf;
}

/* PNG decode: memory blob -> RGB grid via tmpfile */
static uint8_t* _ga_png_dec(const uint8_t *blob, uint32_t bsz,
                              uint32_t grid_w, uint32_t *out_rows)
{
    *out_rows=0;
    FILE *tmp=tmpfile(); if(!tmp) return NULL;
    fwrite(blob,1,bsz,tmp); rewind(tmp);
    png_structp pp=png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop   pi=png_create_info_struct(pp);
    if(setjmp(png_jmpbuf(pp))){
        png_destroy_read_struct(&pp,&pi,NULL); fclose(tmp); return NULL;
    }
    png_init_io(pp,tmp);
    png_read_info(pp,pi);
    uint32_t H=(uint32_t)png_get_image_height(pp,pi);
    uint8_t *rgb=(uint8_t*)malloc(grid_w*H*3);
    for(uint32_t r=0;r<H;r++)
        png_read_row(pp,rgb+r*grid_w*3,NULL);
    png_read_end(pp,pi);
    png_destroy_read_struct(&pp,&pi,NULL);
    fclose(tmp);
    *out_rows=H;
    return rgb;
}

/* Encode tile RGB -> ZSTD YCgCo blob */
static uint8_t* _ga_tile_enc(const uint8_t *img,
                               int x0,int y0,int x1,int y1,int W,
                               int zlvl, uint32_t *out_sz)
{
    int tw=x1-x0,th=y1-y0,tn=tw*th;
    int8_t *raw=(int8_t*)malloc(tn*3);
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
        int gi=(y*W+x)*3,li=(y-y0)*tw+(x-x0);
        int Y,Cg,Co;
        _ga_to_ycgco(img[gi],img[gi+1],img[gi+2],&Y,&Cg,&Co);
        raw[li*3+0]=_ga_ci8(Y);
        raw[li*3+1]=_ga_ci8(Cg);
        raw[li*3+2]=_ga_ci8(Co);
    }
    size_t bound=ZSTD_compressBound((size_t)(tn*3));
    uint8_t *buf=(uint8_t*)malloc(bound);
    size_t csz=ZSTD_compress(buf,bound,raw,(size_t)(tn*3),zlvl);
    free(raw);
    if(ZSTD_isError(csz)){ free(buf); *out_sz=0; return NULL; }
    *out_sz=(uint32_t)csz;
    return buf;
}

/* Decode ZSTD YCgCo blob -> tile pixels */
static void _ga_tile_dec(const uint8_t *blob, uint32_t bsz,
                          int x0,int y0,int x1,int y1,int W, uint8_t *img)
{
    int tw=x1-x0,th=y1-y0,tn=tw*th;
    int8_t *raw=(int8_t*)malloc(tn*3);
    size_t dsz=ZSTD_decompress(raw,(size_t)(tn*3),blob,bsz);
    if(ZSTD_isError(dsz)){ free(raw); return; }
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
        int gi=(y*W+x)*3,li=(y-y0)*tw+(x-x0);
        int r2,g2,b2;
        _ga_to_rgb((int)raw[li*3+0],(int)raw[li*3+1],(int)raw[li*3+2],
                   &r2,&g2,&b2);
        img[gi+0]=(uint8_t)_ga_clamp(r2);
        img[gi+1]=(uint8_t)_ga_clamp(g2);
        img[gi+2]=(uint8_t)_ga_clamp(b2);
    }
    free(raw);
}

/* Encode keyframe: per-tile ZSTD -> O4 -> packed grid RGB -> PNG blob */
static int _ga_enc_keyframe(
        const uint8_t *img, int W, int H, int TW, int TH, int zlvl,
        Gpx4TileEntry **out_tiles,
        uint8_t **out_png, uint32_t *out_png_sz)
{
    int NT=TW*TH;
    Gpx4TileEntry *tiles=(Gpx4TileEntry*)calloc(NT,sizeof(Gpx4TileEntry));
    O4GridCtx *ctxs=(O4GridCtx*)calloc(NT,sizeof(O4GridCtx));
    uint32_t total_rows=0;

    for(int ti=0;ti<NT;ti++){
        int tx=ti%TW,ty=ti/TW;
        int x0=tx*_GA_TILE,x1=x0+_GA_TILE; if(x1>W)x1=W;
        int y0=ty*_GA_TILE,y1=y0+_GA_TILE; if(y1>H)y1=H;
        uint32_t bsz=0;
        uint8_t *blob=_ga_tile_enc(img,x0,y0,x1,y1,W,zlvl,&bsz);
        o4_encode(blob?blob:(uint8_t*)"",bsz,(uint8_t)tx,(uint8_t)ty,&ctxs[ti]);
        free(blob);
        tiles[ti].rows=(uint16_t)ctxs[ti].grid_h;
        tiles[ti].tflags=GPX4_TILE_PRESENT;
        tiles[ti].blob_sz=bsz;
        total_rows+=ctxs[ti].grid_h;
    }

    /* pack all O4 grids into single RGB buffer */
    uint32_t gr_h=total_rows>0?total_rows:1;
    uint8_t *grid_rgb=(uint8_t*)calloc(O4_GRID_W*gr_h*3,1);
    uint32_t row_cur=0;
    for(int ti=0;ti<NT;ti++){
        uint32_t gh=ctxs[ti].grid_h;
        o4_grid_to_rgb(&ctxs[ti],grid_rgb+row_cur*O4_GRID_W*3,
                       gh*O4_GRID_W*3);
        row_cur+=gh;
    }
    free(ctxs);

    uint8_t *png=_ga_png_enc(grid_rgb,O4_GRID_W,gr_h,out_png_sz);
    free(grid_rgb);
    *out_tiles=tiles;
    *out_png=png;
    return png?0:1;
}

/* Decode keyframe: PNG -> O4 grid -> ZSTD blobs -> pixels */
static int _ga_dec_keyframe(
        const uint8_t *png,uint32_t png_sz,
        const Gpx4TileEntry *tiles,
        int W,int H,int TW,int TH, uint8_t *img)
{
    int NT=TW*TH;
    uint32_t actual_rows=0;
    uint8_t *grid_rgb=_ga_png_dec(png,png_sz,O4_GRID_W,&actual_rows);
    if(!grid_rgb) return 1;

    uint32_t row_cur=0;
    for(int ti=0;ti<NT;ti++){
        int tx=ti%TW,ty=ti/TW;
        int x0=tx*_GA_TILE,x1=x0+_GA_TILE; if(x1>W)x1=W;
        int y0=ty*_GA_TILE,y1=y0+_GA_TILE; if(y1>H)y1=H;
        uint32_t gh=tiles[ti].rows;
        uint32_t bsz=tiles[ti].blob_sz;

        O4GridCtx ctx; memset(&ctx,0,sizeof(ctx));
        ctx.grid_h=gh; ctx.tile_x=(uint8_t)tx; ctx.tile_y=(uint8_t)ty;
        for(uint32_t r=0;r<gh&&r<O4_MAX_GRID_H;r++){
            const uint8_t *rrow=grid_rgb+(row_cur+r)*O4_GRID_W*3;
            for(uint32_t c=0;c<O4_GRID_W;c++){
                ctx.grid[r][c].r=rrow[c*3+0];
                ctx.grid[r][c].g=rrow[c*3+1];
                ctx.grid[r][c].b=rrow[c*3+2];
            }
        }
        row_cur+=gh;

        uint32_t n_chunks=(bsz+O4_CHUNK_BYTES-1)/O4_CHUNK_BYTES;
        ctx.n_slots=n_chunks;
        uint8_t *blob=(uint8_t*)malloc(bsz>0?bsz:1);
        o4_decode(&ctx,blob,n_chunks,bsz);
        _ga_tile_dec(blob,bsz,x0,y0,x1,y1,W,img);
        free(blob);
    }
    free(grid_rgb);
    return 0;
}

/* Encode delta frame */
static int _ga_enc_delta(
        const uint8_t *prev,const uint8_t *curr,
        int W,int H,int TW,int TH,int zlvl,
        Gpx4TileEntry **out_tiles,
        uint8_t **out_blob, uint32_t *out_sz)
{
    int NT=TW*TH;
    Gpx4TileEntry *tiles=(Gpx4TileEntry*)calloc(NT,sizeof(Gpx4TileEntry));
    uint8_t **tblobs=(uint8_t**)calloc(NT,sizeof(uint8_t*));
    uint32_t total=0;

    for(int ti=0;ti<NT;ti++){
        int tx=ti%TW,ty=ti/TW;
        int x0=tx*_GA_TILE,x1=x0+_GA_TILE; if(x1>W)x1=W;
        int y0=ty*_GA_TILE,y1=y0+_GA_TILE; if(y1>H)y1=H;
        int tw=x1-x0,th=y1-y0,tn=tw*th;
        int8_t *raw=(int8_t*)malloc(tn*3);
        int all_zero=1;
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            int gi=(y*W+x)*3,li=(y-y0)*tw+(x-x0);
            int pY,pCg,pCo,cY,cCg,cCo;
            _ga_to_ycgco(prev[gi],prev[gi+1],prev[gi+2],&pY,&pCg,&pCo);
            _ga_to_ycgco(curr[gi],curr[gi+1],curr[gi+2],&cY,&cCg,&cCo);
            raw[li*3+0]=_ga_ci8(cY-pY);
            raw[li*3+1]=_ga_ci8(cCg-pCg);
            raw[li*3+2]=_ga_ci8(cCo-pCo);
            if(cY-pY||cCg-pCg||cCo-pCo) all_zero=0;
        }
        if(all_zero){
            tiles[ti].tflags=GPX4_TILE_SKIP;
            free(raw); continue;
        }
        size_t bound=ZSTD_compressBound((size_t)(tn*3));
        uint8_t *cbuf=(uint8_t*)malloc(bound);
        size_t csz=ZSTD_compress(cbuf,bound,raw,(size_t)(tn*3),zlvl);
        free(raw);
        uint32_t bsz=ZSTD_isError(csz)?0:(uint32_t)csz;
        tblobs[ti]=cbuf;
        tiles[ti].rows=1; tiles[ti].tflags=GPX4_TILE_PRESENT;
        tiles[ti].blob_sz=bsz; total+=bsz;
    }

    uint8_t *blob=total>0?(uint8_t*)malloc(total):NULL;
    uint32_t cur=0;
    for(int ti=0;ti<NT;ti++){
        if(tblobs[ti]){
            if(blob&&tiles[ti].blob_sz>0)
                memcpy(blob+cur,tblobs[ti],tiles[ti].blob_sz);
            cur+=tiles[ti].blob_sz;
            free(tblobs[ti]);
        }
    }
    free(tblobs);
    *out_tiles=tiles; *out_blob=blob; *out_sz=total;
    return 0;
}

/* ════════════════════════════════════════════════════════
 * PUBLIC: gpx_anim_encode
 * ════════════════════════════════════════════════════════ */
int gpx_anim_encode(uint8_t **frames, int n_frames, int W, int H,
                    const GpxAnimEncCfg *cfg, const char *out_path)
{
    if(!frames||n_frames<=0||!cfg||!out_path) return 1;
    if(n_frames>_GA_MAX_FRAMES) return 1;
    int TW=(W+_GA_TILE-1)/_GA_TILE,TH=(H+_GA_TILE-1)/_GA_TILE;
    int zlvl=cfg->zstd_level>0?cfg->zstd_level:9;
    uint16_t kfi=cfg->keyframe_interval;
    int max_nl=1+n_frames;
    if(max_nl>GPX4_MAX_LAYERS){ fprintf(stderr,"too many frames\n"); return 1; }

    Gpx4LayerDef  *layers=(Gpx4LayerDef*)calloc(max_nl,sizeof(Gpx4LayerDef));
    uint8_t      **fblobs=(uint8_t**)calloc(n_frames,sizeof(uint8_t*));
    Gpx4TileEntry**ftiles=(Gpx4TileEntry**)calloc(n_frames,sizeof(Gpx4TileEntry*));
    int nl=0;

    /* AHDR layer */
    uint8_t *ahdr_buf=(uint8_t*)calloc(GPX4_ANIM_HDR_SZ,1);
    {
        Gpx4AnimHdr ah={
            (uint16_t)n_frames,
            cfg->fps_num?cfg->fps_num:24,
            cfg->fps_den?cfg->fps_den:1,
            kfi,(uint16_t)W,(uint16_t)H,0,0
        };
        gpx4_anim_hdr_write(ahdr_buf,&ah);
    }
    layers[nl].type=GPX4_LAYER_ANIM_HDR; layers[nl].lflags=0;
    memcpy(layers[nl].name,"AHDR",4);
    layers[nl].data=ahdr_buf; layers[nl].size=GPX4_ANIM_HDR_SZ;
    layers[nl].tiles=NULL; nl++;

    for(int fi=0;fi<n_frames;fi++){
        int is_key=(fi==0)||(kfi>0&&(fi%kfi)==0);
        uint8_t *blob=NULL; uint32_t bsz=0;
        Gpx4TileEntry *tiles=NULL;

        if(is_key){
            if(_ga_enc_keyframe(frames[fi],W,H,TW,TH,zlvl,
                                &tiles,&blob,&bsz)!=0){
                fprintf(stderr,"keyframe %d failed\n",fi); continue;
            }
            layers[nl].type=GPX4_LAYER_O4;
            layers[nl].lflags=GPX4_LFLAG_KEYFRAME;
        } else {
            _ga_enc_delta(frames[fi-1],frames[fi],W,H,TW,TH,zlvl,
                          &tiles,&blob,&bsz);
            layers[nl].type=GPX4_LAYER_DELTA;
            layers[nl].lflags=GPX4_LFLAG_ZSTD;
        }
        gpx4_frame_name(layers[nl].name,is_key,fi);
        layers[nl].data=blob; layers[nl].size=bsz;
        layers[nl].tiles=tiles;
        fblobs[fi]=blob; ftiles[fi]=tiles;
        nl++;
    }

    int ret=gpx4_write(out_path,(uint16_t)TW,(uint16_t)TH,layers,nl);
    free(ahdr_buf);
    for(int fi=0;fi<n_frames;fi++){ free(fblobs[fi]); free(ftiles[fi]); }
    free(fblobs); free(ftiles); free(layers);
    return ret;
}

/* ════════════════════════════════════════════════════════
 * PUBLIC: gpx_anim_decode
 * ════════════════════════════════════════════════════════ */
int gpx_anim_decode(const char *path, GpxAnimFrameCb cb, void *ud)
{
    Gpx4File gf;
    if(gpx4_open(path,&gf)!=0) return 1;
    uint32_t ahdr_sz=0;
    const uint8_t *ahdr_data=gpx4_layer_data_by_name(&gf,"AHDR",&ahdr_sz);
    if(!ahdr_data||ahdr_sz<GPX4_ANIM_HDR_SZ){ gpx4_close(&gf); return 1; }
    Gpx4AnimHdr ah; gpx4_anim_hdr_read(ahdr_data,&ah);
    int W=ah.width_px,H=ah.height_px,n_frames=ah.n_frames;
    int TW=(W+_GA_TILE-1)/_GA_TILE,TH=(H+_GA_TILE-1)/_GA_TILE;

    uint8_t *cur_rgb=(uint8_t*)calloc((size_t)W*H*3,1);
    int ret=0;

    for(int fi=0;fi<n_frames;fi++){
        int is_key=(fi==0)||(ah.keyframe_interval>0
                             &&(fi%ah.keyframe_interval)==0);
        char name[4]; gpx4_frame_name(name,is_key,fi);
        uint32_t lsz=0;
        const uint8_t *ldata=gpx4_layer_data_by_name(&gf,name,&lsz);
        Gpx4TileEntry *ltiles=gpx4_tile_table_by_name(&gf,name);
        if(!ldata){ ret=1; break; }

        if(is_key){
            if(_ga_dec_keyframe(ldata,lsz,ltiles,W,H,TW,TH,cur_rgb)!=0){
                ret=1; break;
            }
        } else {
            int NT=TW*TH; uint32_t blob_cur=0;
            for(int ti=0;ti<NT;ti++){
                int tx=ti%TW,ty=ti/TW;
                int x0=tx*_GA_TILE,x1=x0+_GA_TILE; if(x1>W)x1=W;
                int y0=ty*_GA_TILE,y1=y0+_GA_TILE; if(y1>H)y1=H;
                int tw=x1-x0,th_t=y1-y0,tn=tw*th_t;
                if(!ltiles||(ltiles[ti].tflags&GPX4_TILE_SKIP)) continue;
                uint32_t bsz=ltiles[ti].blob_sz;
                if(!bsz||blob_cur+bsz>lsz){ blob_cur+=bsz; continue; }
                int8_t *raw=(int8_t*)malloc(tn*3);
                size_t dsz=ZSTD_decompress(raw,(size_t)(tn*3),
                                            ldata+blob_cur,bsz);
                blob_cur+=bsz;
                if(ZSTD_isError(dsz)){ free(raw); continue; }
                for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
                    int gi=(y*W+x)*3,li=(y-y0)*tw+(x-x0);
                    int pY,pCg,pCo;
                    _ga_to_ycgco(cur_rgb[gi],cur_rgb[gi+1],cur_rgb[gi+2],
                                 &pY,&pCg,&pCo);
                    int r2,g2,b2;
                    _ga_to_rgb(pY+(int)raw[li*3+0],
                               pCg+(int)raw[li*3+1],
                               pCo+(int)raw[li*3+2],&r2,&g2,&b2);
                    cur_rgb[gi+0]=(uint8_t)_ga_clamp(r2);
                    cur_rgb[gi+1]=(uint8_t)_ga_clamp(g2);
                    cur_rgb[gi+2]=(uint8_t)_ga_clamp(b2);
                }
                free(raw);
            }
        }
        if(cb&&cb(fi,cur_rgb,ud)!=0) break;
    }
    free(cur_rgb);
    gpx4_close(&gf);
    return ret;
}

/* ════════════════════════════════════════════════════════
 * PUBLIC: gpx_anim_info
 * ════════════════════════════════════════════════════════ */
int gpx_anim_info(const char *path, Gpx4AnimHdr *hdr_out)
{
    Gpx4File gf;
    if(gpx4_open(path,&gf)!=0) return 1;
    uint32_t sz=0;
    const uint8_t *d=gpx4_layer_data_by_name(&gf,"AHDR",&sz);
    if(!d||sz<GPX4_ANIM_HDR_SZ){ gpx4_close(&gf); return 1; }
    gpx4_anim_hdr_read(d,hdr_out);
    gpx4_close(&gf);
    return 0;
}

#undef _GA_TILE
#undef _GA_MAX_FRAMES

#endif /* GEO_GPX_ANIM_IMPL */
#endif /* GEO_GPX_ANIM_H */
