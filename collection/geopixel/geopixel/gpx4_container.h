/*
 * gpx4_container.h — Multi-layer GeoPixel container  (v2)
 *
 * BREAKING CHANGE vs v1: layer entry size field promoted 2B→4B (was capped 64KB)
 *   LAYER_ENTRY_SZ: 12B → 14B
 *     type     1B
 *     flags    1B
 *     name     4B
 *     offset   4B  (unchanged)
 *     size     4B  (was 2B — now full uint32)
 *
 * File layout (all big-endian):
 *
 *   [FILE_HEADER]   16B
 *     magic    4B  'GPX4'
 *     version  1B  0x02        ← bumped from 0x01
 *     flags    1B  reserved=0
 *     n_layers 2B
 *     tw       2B  tile columns
 *     th       2B  tile rows
 *     n_tiles  4B  tw×th
 *
 *   [LAYER_TABLE]   n_layers × 14B each
 *     type     1B  GPX4_LAYER_*
 *     flags    1B  GPX4_LFLAG_*
 *     name     4B  4-char tag
 *     offset   4B  byte offset of layer data from file start
 *     size     4B  byte size of layer data
 *
 *   [TILE_TABLE]    tiled layers only (type != META, type != ANIM_HDR)
 *                   n_tiles × 8B per tiled layer
 *     rows     2B  O4 grid rows (0 = tile absent/noise)
 *     tflags   2B  GPX4_TILE_*
 *     blob_sz  4B  compressed blob size in bytes
 *
 *   [LAYER DATA]    raw bytes at offsets declared in LAYER_TABLE
 *
 * Animation convention:
 *   GPX4_LAYER_ANIM_HDR  name="AHDR"  → Gpx4AnimHdr (16B, no tile table)
 *   GPX4_LAYER_O4        name="F000"  → keyframe 0  (has tile table)
 *   GPX4_LAYER_DELTA     name="D001"  → P-frame 1 delta (has tile table)
 *   GPX4_LAYER_DELTA     name="D002"  → P-frame 2 delta
 *   ...
 *   GPX4_LAYER_O4        name="F016"  → keyframe every keyframe_interval
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── constants ───────────────────────────────────────── */
#define GPX4_MAGIC           0x47505834u   /* 'GPX4' */
#define GPX4_VERSION         0x02          /* v2: 4B size field */
#define GPX4_MAX_LAYERS      64            /* raised for animation */

/* layer types */
#define GPX4_LAYER_O4        0x01   /* O4 geometry grid (PNG blob)    */
#define GPX4_LAYER_FALLBACK  0x02   /* noise blobs, raw               */
#define GPX4_LAYER_DELTA     0x03   /* P-frame delta vs prev frame    */
#define GPX4_LAYER_META      0x04   /* key-value metadata (no tiles)  */
#define GPX4_LAYER_ANIM_HDR  0x05   /* animation header (no tiles)    */

/* layer flags */
#define GPX4_LFLAG_KEYFRAME  0x01   /* this O4 layer is a keyframe    */
#define GPX4_LFLAG_ZSTD      0x02   /* delta data is ZSTD compressed  */

/* tile flags */
#define GPX4_TILE_PRESENT    0x0001
#define GPX4_TILE_REF        0x0002 /* dedup ref                      */
#define GPX4_TILE_SKIP       0x0004 /* delta=0, copy from ref frame   */

/* sizes */
#define GPX4_FILE_HDR_SZ     16
#define GPX4_LAYER_ENTRY_SZ  14    /* v2: +2B for 4B size field */
#define GPX4_TILE_ENTRY_SZ    8
#define GPX4_ANIM_HDR_SZ     16

/* ── byte I/O ────────────────────────────────────────── */
static inline void g4w2(uint8_t *b, uint16_t v)
    { b[0]=(uint8_t)(v>>8); b[1]=(uint8_t)v; }
static inline void g4w4(uint8_t *b, uint32_t v)
    { b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16);
      b[2]=(uint8_t)(v>>8);  b[3]=(uint8_t)v; }
static inline uint16_t g4r2(const uint8_t *b)
    { return (uint16_t)((b[0]<<8)|b[1]); }
static inline uint32_t g4r4(const uint8_t *b)
    { return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|
             ((uint32_t)b[2]<<8)|(uint32_t)b[3]; }

/* ── animation header (stored as layer data, 16B) ────── */
typedef struct {
    uint16_t n_frames;         /* total frame count                  */
    uint16_t fps_num;          /* fps numerator   e.g. 24            */
    uint16_t fps_den;          /* fps denominator e.g.  1            */
    uint16_t keyframe_interval;/* keyframe every N frames (0=first only) */
    uint16_t width_px;         /* image width  in pixels             */
    uint16_t height_px;        /* image height in pixels             */
    uint16_t flags;            /* reserved                           */
    uint16_t reserved;
} Gpx4AnimHdr;                 /* 16B */

static inline void gpx4_anim_hdr_write(uint8_t *b, const Gpx4AnimHdr *h){
    g4w2(b+ 0, h->n_frames);
    g4w2(b+ 2, h->fps_num);
    g4w2(b+ 4, h->fps_den);
    g4w2(b+ 6, h->keyframe_interval);
    g4w2(b+ 8, h->width_px);
    g4w2(b+10, h->height_px);
    g4w2(b+12, h->flags);
    g4w2(b+14, h->reserved);
}
static inline void gpx4_anim_hdr_read(const uint8_t *b, Gpx4AnimHdr *h){
    h->n_frames          = g4r2(b+ 0);
    h->fps_num           = g4r2(b+ 2);
    h->fps_den           = g4r2(b+ 4);
    h->keyframe_interval = g4r2(b+ 6);
    h->width_px          = g4r2(b+ 8);
    h->height_px         = g4r2(b+10);
    h->flags             = g4r2(b+12);
    h->reserved          = g4r2(b+14);
}

/* ── tile entry ──────────────────────────────────────── */
typedef struct {
    uint16_t rows;      /* O4 grid rows (0 = noise/skip)  */
    uint16_t tflags;    /* GPX4_TILE_*                    */
    uint32_t blob_sz;   /* compressed bytes               */
} Gpx4TileEntry;

/* ── layer definition (for write) ───────────────────── */
typedef struct {
    uint8_t  type;      /* GPX4_LAYER_*                   */
    uint8_t  lflags;    /* GPX4_LFLAG_*                   */
    char     name[4];   /* 4-char tag                     */
    uint8_t *data;      /* external — not freed by gpx4_write */
    uint32_t size;
    Gpx4TileEntry *tiles; /* NULL for META / ANIM_HDR     */
} Gpx4LayerDef;

/* ── layer name helpers ──────────────────────────────── */
/* encode frame index into name: keyframe → "F%03d", delta → "D%03d" */
static inline void gpx4_frame_name(char name[4], int is_keyframe, int idx){
    char tmp[8];
    snprintf(tmp, 8, "%c%03d", is_keyframe?'F':'D', idx % 1000);
    memcpy(name, tmp, 4);
}
/* parse frame index from name (returns -1 if not a frame layer) */
static inline int gpx4_frame_index(const char name[4]){
    if(name[0]!='F' && name[0]!='D') return -1;
    int v=0;
    for(int i=1;i<4;i++){
        if(name[i]<'0'||name[i]>'9') return -1;
        v = v*10 + (name[i]-'0');
    }
    return v;
}
static inline int gpx4_is_keyframe_name(const char name[4]){
    return name[0]=='F';
}

/* ══════════════════════════════════════════════════════
 * WRITE
 * ══════════════════════════════════════════════════════ */
static inline int gpx4_write(
        const char *path,
        uint16_t tw, uint16_t th,
        Gpx4LayerDef *layers, int nl)
{
    if(nl > GPX4_MAX_LAYERS) return -1;
    uint32_t nt = (uint32_t)tw * th;

    int n_tiled = 0;
    for(int i = 0; i < nl; i++) if(layers[i].tiles) n_tiled++;

    uint32_t data_start = GPX4_FILE_HDR_SZ
                        + (uint32_t)nl * GPX4_LAYER_ENTRY_SZ
                        + (uint32_t)n_tiled * nt * GPX4_TILE_ENTRY_SZ;

    uint32_t *offsets = (uint32_t*)calloc(nl, sizeof(uint32_t));
    uint32_t cur = data_start;
    for(int i = 0; i < nl; i++){ offsets[i] = cur; cur += layers[i].size; }

    FILE *f = fopen(path, "wb");
    if(!f){ free(offsets); return -1; }

    /* FILE_HEADER */
    uint8_t fh[GPX4_FILE_HDR_SZ] = {
        'G','P','X','4', GPX4_VERSION, 0,
        (uint8_t)(nl>>8),(uint8_t)nl,
        (uint8_t)(tw>>8),(uint8_t)tw,
        (uint8_t)(th>>8),(uint8_t)th,
        (uint8_t)(nt>>24),(uint8_t)(nt>>16),(uint8_t)(nt>>8),(uint8_t)nt
    };
    fwrite(fh, 1, GPX4_FILE_HDR_SZ, f);

    /* LAYER_TABLE  (14B each) */
    for(int i = 0; i < nl; i++){
        uint8_t lb[GPX4_LAYER_ENTRY_SZ];
        lb[0]=layers[i].type; lb[1]=layers[i].lflags;
        lb[2]=layers[i].name[0]; lb[3]=layers[i].name[1];
        lb[4]=layers[i].name[2]; lb[5]=layers[i].name[3];
        g4w4(lb+6,  offsets[i]);
        g4w4(lb+10, layers[i].size);  /* 4B now */
        fwrite(lb, 1, GPX4_LAYER_ENTRY_SZ, f);
    }
    free(offsets);

    /* TILE_TABLE */
    for(int i = 0; i < nl; i++){
        if(!layers[i].tiles) continue;
        for(uint32_t t = 0; t < nt; t++){
            uint8_t tb[GPX4_TILE_ENTRY_SZ];
            g4w2(tb+0, layers[i].tiles[t].rows);
            g4w2(tb+2, layers[i].tiles[t].tflags);
            g4w4(tb+4, layers[i].tiles[t].blob_sz);
            fwrite(tb, 1, GPX4_TILE_ENTRY_SZ, f);
        }
    }

    /* LAYER DATA */
    for(int i = 0; i < nl; i++)
        if(layers[i].data && layers[i].size > 0)
            fwrite(layers[i].data, 1, layers[i].size, f);

    fclose(f); return 0;
}

/* ══════════════════════════════════════════════════════
 * READ
 * ══════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  type;
    uint8_t  lflags;
    char     name[4];
    uint32_t offset;
    uint32_t size;
} Gpx4LayerInfo;

typedef struct {
    uint16_t tw, th, n_layers;
    uint32_t n_tiles;
    Gpx4LayerInfo  *layers;
    Gpx4TileEntry **tile_tables;   /* [n_layers][n_tiles], NULL if untiled */
    uint8_t        *raw;
    uint32_t        raw_sz;
} Gpx4File;

static inline int gpx4_open(const char *path, Gpx4File *gf)
{
    memset(gf, 0, sizeof(*gf));
    FILE *f = fopen(path, "rb");
    if(!f) return -1;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    gf->raw = (uint8_t*)malloc((size_t)fsz);
    gf->raw_sz = (uint32_t)fsz;
    if((long)fread(gf->raw,1,(size_t)fsz,f)!=fsz){
        fclose(f); free(gf->raw); return -1;
    }
    fclose(f);

    uint8_t *p = gf->raw;
    if(p[0]!='G'||p[1]!='P'||p[2]!='X'||p[3]!='4'){ free(gf->raw); return -1; }
    /* accept v1 (12B entry) and v2 (14B entry) */
    uint8_t ver = p[4];
    int entry_sz = (ver >= 0x02) ? GPX4_LAYER_ENTRY_SZ : 12;

    gf->n_layers = g4r2(p+6);
    gf->tw       = g4r2(p+8);
    gf->th       = g4r2(p+10);
    gf->n_tiles  = g4r4(p+12);

    int nl = gf->n_layers;
    uint32_t nt = gf->n_tiles;
    gf->layers      = (Gpx4LayerInfo*)calloc(nl, sizeof(Gpx4LayerInfo));
    gf->tile_tables = (Gpx4TileEntry**)calloc(nl, sizeof(Gpx4TileEntry*));

    /* LAYER_TABLE */
    uint8_t *lp = p + GPX4_FILE_HDR_SZ;
    for(int i = 0; i < nl; i++){
        gf->layers[i].type   = lp[0];
        gf->layers[i].lflags = lp[1];
        memcpy(gf->layers[i].name, lp+2, 4);
        gf->layers[i].offset = g4r4(lp+6);
        gf->layers[i].size   = (ver >= 0x02) ? g4r4(lp+10) : (uint32_t)g4r2(lp+10);
        lp += entry_sz;
    }

    /* TILE_TABLE — skip META and ANIM_HDR */
    uint8_t *tp = p + GPX4_FILE_HDR_SZ + (uint32_t)nl * entry_sz;
    for(int i = 0; i < nl; i++){
        uint8_t t = gf->layers[i].type;
        if(t == GPX4_LAYER_META || t == GPX4_LAYER_ANIM_HDR) continue;
        gf->tile_tables[i] = (Gpx4TileEntry*)malloc(nt * sizeof(Gpx4TileEntry));
        for(uint32_t tt = 0; tt < nt; tt++){
            gf->tile_tables[i][tt].rows    = g4r2(tp+0);
            gf->tile_tables[i][tt].tflags  = g4r2(tp+2);
            gf->tile_tables[i][tt].blob_sz = g4r4(tp+4);
            tp += GPX4_TILE_ENTRY_SZ;
        }
    }
    return 0;
}

/* lookup by type (returns first match) */
static inline const uint8_t* gpx4_layer_data(
        const Gpx4File *gf, uint8_t type, uint32_t *out_sz)
{
    for(int i = 0; i < gf->n_layers; i++)
        if(gf->layers[i].type == type){
            if(out_sz) *out_sz = gf->layers[i].size;
            return gf->raw + gf->layers[i].offset;
        }
    return NULL;
}

/* lookup by name (4 chars, exact) */
static inline const uint8_t* gpx4_layer_data_by_name(
        const Gpx4File *gf, const char name[4], uint32_t *out_sz)
{
    for(int i = 0; i < gf->n_layers; i++)
        if(memcmp(gf->layers[i].name, name, 4)==0){
            if(out_sz) *out_sz = gf->layers[i].size;
            return gf->raw + gf->layers[i].offset;
        }
    return NULL;
}

/* lookup tile table by type (first match) */
static inline Gpx4TileEntry* gpx4_tile_table(
        const Gpx4File *gf, uint8_t type)
{
    for(int i = 0; i < gf->n_layers; i++)
        if(gf->layers[i].type == type)
            return gf->tile_tables[i];
    return NULL;
}

/* lookup tile table by name */
static inline Gpx4TileEntry* gpx4_tile_table_by_name(
        const Gpx4File *gf, const char name[4])
{
    for(int i = 0; i < gf->n_layers; i++)
        if(memcmp(gf->layers[i].name, name, 4)==0)
            return gf->tile_tables[i];
    return NULL;
}

/* layer index by name (-1 if not found) */
static inline int gpx4_layer_index(const Gpx4File *gf, const char name[4]){
    for(int i = 0; i < gf->n_layers; i++)
        if(memcmp(gf->layers[i].name, name, 4)==0) return i;
    return -1;
}

static inline void gpx4_close(Gpx4File *gf){
    for(int i = 0; i < gf->n_layers; i++) free(gf->tile_tables[i]);
    free(gf->layers); free(gf->tile_tables); free(gf->raw);
    memset(gf, 0, sizeof(*gf));
}
