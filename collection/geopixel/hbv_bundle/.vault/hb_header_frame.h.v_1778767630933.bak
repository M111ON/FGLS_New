#pragma once
/*
 * hb_header_frame.h — Header Frame (frame[0] = directory tile)
 *
 * Purpose: scan frame[0] → know entire sequence structure without reading all tiles.
 *
 * Layout (fixed 64B, stored as raw tile data at tile_id=0):
 *
 *   [0..3]   magic     'HBHF'  (0x48424846)
 *   [4]      version   0x01
 *   [5]      n_cycles  number of cycle files (1..255)
 *   [6..7]   n_layers  logical layers (e.g. Y=1, YCgCo=3)
 *   [8..11]  total_tiles  across all cycles
 *   [12..13] img_w     original image width  (0 if not image)
 *   [14..15] img_h     original image height
 *   [16..17] tile_w
 *   [18..19] tile_h
 *   [20..23] global_seed  (copy from gpx5 header)
 *   [24..27] tick_period  (default 1440)
 *   [28..29] codec_map[0..7]  — codec_id used per shell (shell 0..7)
 *   [36..39] layer_stride  bytes per layer per tile
 *   [40..43] crc32   of bytes [0..39]  (simple checksum)
 *   [44..63] reserved (zero)
 *
 * Total: 64B = fits in one tile slot without compression.
 */

#include <stdint.h>
#include <string.h>

#define HBHF_MAGIC         0x48424846u  /* 'HBHF' */
#define HBHF_VERSION       0x01u
#define HBHF_SZ            64u
#define HBHF_TTYPE         0xF0u        /* special ttype — marks directory tile */

/* GPX5_TTYPE_DIRECTORY — add to gpx5_container.h caller side */
#define GPX5_TTYPE_DIRECTORY  0xF0u

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  n_cycles;       /* number of .gpx5 cycle files          */
    uint16_t n_layers;       /* logical planes (1=Y, 3=YCgCo, etc.)  */
    uint32_t total_tiles;
    uint16_t img_w;
    uint16_t img_h;
    uint16_t tile_w;
    uint16_t tile_h;
    uint32_t global_seed;
    uint32_t tick_period;
    uint8_t  codec_map[8];   /* codec_id per shell 0..7              */
    uint32_t layer_stride;   /* bytes per layer per tile             */
    uint32_t crc32;
    /* 20B reserved */
} HbHeaderFrame;             /* logical — serialised to 64B flat buf */

/* ── simple CRC32 (no table, for portability) ── */
static inline uint32_t hbhf_crc32_bytes(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ── serialise to 64B buffer ── */
static inline void hbhf_write(uint8_t buf[HBHF_SZ], const HbHeaderFrame *f) {
    memset(buf, 0, HBHF_SZ);
    buf[0]  = (uint8_t)(f->magic >> 24);
    buf[1]  = (uint8_t)(f->magic >> 16);
    buf[2]  = (uint8_t)(f->magic >>  8);
    buf[3]  = (uint8_t)(f->magic);
    buf[4]  = f->version;
    buf[5]  = f->n_cycles;
    buf[6]  = (uint8_t)(f->n_layers >> 8);
    buf[7]  = (uint8_t)(f->n_layers);
    buf[8]  = (uint8_t)(f->total_tiles >> 24);
    buf[9]  = (uint8_t)(f->total_tiles >> 16);
    buf[10] = (uint8_t)(f->total_tiles >>  8);
    buf[11] = (uint8_t)(f->total_tiles);
    buf[12] = (uint8_t)(f->img_w >> 8);  buf[13] = (uint8_t)(f->img_w);
    buf[14] = (uint8_t)(f->img_h >> 8);  buf[15] = (uint8_t)(f->img_h);
    buf[16] = (uint8_t)(f->tile_w >> 8); buf[17] = (uint8_t)(f->tile_w);
    buf[18] = (uint8_t)(f->tile_h >> 8); buf[19] = (uint8_t)(f->tile_h);
    buf[20] = (uint8_t)(f->global_seed >> 24);
    buf[21] = (uint8_t)(f->global_seed >> 16);
    buf[22] = (uint8_t)(f->global_seed >>  8);
    buf[23] = (uint8_t)(f->global_seed);
    buf[24] = (uint8_t)(f->tick_period >> 24);
    buf[25] = (uint8_t)(f->tick_period >> 16);
    buf[26] = (uint8_t)(f->tick_period >>  8);
    buf[27] = (uint8_t)(f->tick_period);
    memcpy(buf + 28, f->codec_map, 8);
    buf[36] = (uint8_t)(f->layer_stride >> 24);
    buf[37] = (uint8_t)(f->layer_stride >> 16);
    buf[38] = (uint8_t)(f->layer_stride >>  8);
    buf[39] = (uint8_t)(f->layer_stride);
    /* crc32 over bytes [0..39] */
    uint32_t crc = hbhf_crc32_bytes(buf, 40u);
    buf[40] = (uint8_t)(crc >> 24);
    buf[41] = (uint8_t)(crc >> 16);
    buf[42] = (uint8_t)(crc >>  8);
    buf[43] = (uint8_t)(crc);
}

/* ── deserialise + verify ── */
static inline int hbhf_read(const uint8_t buf[HBHF_SZ], HbHeaderFrame *out) {
    uint32_t magic = ((uint32_t)buf[0]<<24)|((uint32_t)buf[1]<<16)
                   |((uint32_t)buf[2]<<8)|(uint32_t)buf[3];
    if (magic != HBHF_MAGIC)   return -1;  /* not a header frame */
    if (buf[4] != HBHF_VERSION) return -2; /* version mismatch   */

    /* verify crc */
    uint32_t stored_crc = ((uint32_t)buf[40]<<24)|((uint32_t)buf[41]<<16)
                         |((uint32_t)buf[42]<<8)|(uint32_t)buf[43];
    uint8_t tmp[HBHF_SZ];
    memcpy(tmp, buf, HBHF_SZ);
    memset(tmp + 40, 0, 4);
    uint32_t calc_crc = hbhf_crc32_bytes(tmp, 40u);
    if (stored_crc != calc_crc) return -3; /* corrupt */

    out->magic        = magic;
    out->version      = buf[4];
    out->n_cycles     = buf[5];
    out->n_layers     = (uint16_t)((buf[6]<<8)|buf[7]);
    out->total_tiles  = ((uint32_t)buf[8]<<24)|((uint32_t)buf[9]<<16)
                       |((uint32_t)buf[10]<<8)|(uint32_t)buf[11];
    out->img_w        = (uint16_t)((buf[12]<<8)|buf[13]);
    out->img_h        = (uint16_t)((buf[14]<<8)|buf[15]);
    out->tile_w       = (uint16_t)((buf[16]<<8)|buf[17]);
    out->tile_h       = (uint16_t)((buf[18]<<8)|buf[19]);
    out->global_seed  = ((uint32_t)buf[20]<<24)|((uint32_t)buf[21]<<16)
                       |((uint32_t)buf[22]<<8)|(uint32_t)buf[23];
    out->tick_period  = ((uint32_t)buf[24]<<24)|((uint32_t)buf[25]<<16)
                       |((uint32_t)buf[26]<<8)|(uint32_t)buf[27];
    memcpy(out->codec_map, buf + 28, 8);
    out->layer_stride = ((uint32_t)buf[36]<<24)|((uint32_t)buf[37]<<16)
                       |((uint32_t)buf[38]<<8)|(uint32_t)buf[39];
    out->crc32        = stored_crc;
    return 0;
}

/*
 * hbhf_from_gpx5 — populate HbHeaderFrame from open Gpx5File + image meta.
 * Caller fills img_w/h/tile_w/tile_h; rest derived from gf.
 */
static inline void hbhf_from_gpx5(
        HbHeaderFrame    *out,
        const Gpx5File   *gf,
        uint8_t           n_cycles,
        uint32_t          total_tiles,
        uint16_t          img_w,  uint16_t img_h,
        uint16_t          tile_w, uint16_t tile_h)
{
    memset(out, 0, sizeof(*out));
    out->magic        = HBHF_MAGIC;
    out->version      = HBHF_VERSION;
    out->n_cycles     = n_cycles;
    out->n_layers     = 3u;  /* YCgCo */
    out->total_tiles  = total_tiles;
    out->img_w        = img_w;
    out->img_h        = img_h;
    out->tile_w       = tile_w;
    out->tile_h       = tile_h;
    out->global_seed  = gf->hdr.global_seed;
    out->tick_period  = (uint32_t)gf->hdr.tick_max;
    out->layer_stride = (uint32_t)(tile_w * tile_h * 2u); /* 2B per pixel per layer */
    /* codec_map: derive from pipe table (pipe rem 0..2, wrap to 8 shells) */
    for (int s = 0; s < 8; s++)
        out->codec_map[s] = gf->pipes[s % 3u].codec;
}
