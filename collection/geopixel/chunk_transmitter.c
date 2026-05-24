/*
 * chunk_transmitter.c — Binary → GeoPixel Transmitter (Session bridge)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Goal: feed arbitrary binary file through geopixel_v21 encode → .gpx
 *       then decode .gpx → reconstruct original binary (lossless).
 *
 * Pipeline:
 *   binary → BMP (synthetic) → geopixel_v21 encode → .gp15
 *   .gp15 → geopixel_v21 decode → BMP → binary reconstruct
 *
 * Encoding layout (per tile, 32×32 = 1024 pixels = 3072 bytes RGB):
 *   pixel 0    : MAGIC + orig_size low byte  (R=0xGE, G=0xO1, B=orig_size & 0xFF)
 *   pixel 1    : orig_size bytes 1,2,3       (R,G,B = bytes 1..3 of orig_size uint32)
 *   pixel 2..N : packed chunk bytes, 3 per pixel
 *   remaining  : zero-padded
 *
 * Tile packing: TILE_PX=32, each tile holds 32×32×3 = 3072 data bytes.
 *   Header uses 6 bytes (2 pixels) → 3066 data bytes per tile.
 *   Tiles are stacked vertically: image = 32 wide × (N_TILES × 32) tall.
 *
 * Reconstruct:
 *   Read pixel 0,1 → extract orig_size.
 *   Read pixels 2..end → reassemble bytes → trim to orig_size.
 *
 * Compile:
 *   gcc -O2 -o chunk_transmitter chunk_transmitter.c \
 *       -I/tmp/geopixel/geopixel \
 *       -lm -lzstd -lpng -lpthread
 *
 * Usage:
 *   ./chunk_transmitter encode_bmp <input.bin>  → input.bin.bmp
 *   ./chunk_transmitter decode_bmp <input.bmp> <orig_size> → input.bmp.out.bin
 *
 * NOTE: geopixel_v21_o25.c is lossless only for FLAT tiles (homogeneous).
 *   Binary data tends to NOISE/EDGE → BMODE_DELTA path.
 *   We verify roundtrip via embedded orig_size + xxh64 footer.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── constants ──────────────────────────────────────────────────────── */
#define TILE_PX       32          /* geopixel tile width/height (frozen)  */
#define TILE_BYTES    (TILE_PX * TILE_PX * 3)   /* 3072 RGB bytes/tile   */
#define HDR_BYTES     6           /* 2 pixels × 3 bytes = file size field */
#define DATA_PER_TILE (TILE_BYTES - HDR_BYTES)   /* 3066 usable bytes     */
#define MAGIC_R       0x47u       /* 'G' */
#define MAGIC_G       0xE1u       /* geo1 */

/* ── xxh64-style hash (mirrors wallet / chunk2svg) ────────────────── */
#define H1  0x9e3779b97f4a7c15ULL
#define H2  0x6c62272e07bb0142ULL

static inline uint64_t rotl64(uint64_t x, int r){
    return (x << r) | (x >> (64 - r));
}
static inline uint64_t hash_update(uint64_t acc, uint64_t w){
    acc ^= (w * H1);
    acc  = rotl64(acc, 27);
    acc  = acc * H2 + 0x94d049bb133111ebULL;
    return acc;
}
static uint64_t xxh64(const uint8_t *data, size_t len){
    uint64_t acc = H1 ^ len;
    size_t i = 0;
    for(; i + 8 <= len; i += 8){
        uint64_t w; memcpy(&w, data+i, 8);
        acc = hash_update(acc, w);
    }
    if(i < len){
        uint64_t tail = 0;
        memcpy(&tail, data+i, len-i);
        acc = hash_update(acc, tail);
    }
    acc ^= (acc >> 33); acc *= H1;
    acc ^= (acc >> 29); acc *= H2;
    acc ^= (acc >> 32);
    return acc;
}

/* ── BMP writer (24-bit, no compression) ─────────────────────────── */
static int bmp_write(const char *path, const uint8_t *rgb,
                     int w, int h){
    int row_bytes = w * 3;
    int pad       = (4 - (row_bytes % 4)) % 4;
    int stride    = row_bytes + pad;
    int pix_sz    = stride * h;
    int file_sz   = 54 + pix_sz;

    FILE *f = fopen(path, "wb");
    if(!f){ perror(path); return -1; }

    uint8_t bfh[14] = {
        'B','M',
        (uint8_t)(file_sz),(uint8_t)(file_sz>>8),
        (uint8_t)(file_sz>>16),(uint8_t)(file_sz>>24),
        0,0,0,0,
        54,0,0,0
    };
    fwrite(bfh, 1, 14, f);

    uint8_t dib[40] = {0};
    dib[0]=40;
    *(int32_t*)(dib+4)  = w;
    *(int32_t*)(dib+8)  = -h;  /* top-down */
    dib[12]=1; dib[14]=24;
    fwrite(dib, 1, 40, f);

    uint8_t *row = calloc(stride, 1);
    for(int y=0; y<h; y++){
        for(int x=0; x<w; x++){
            const uint8_t *p = rgb + (y*w+x)*3;
            row[x*3+0] = p[2];  /* B */
            row[x*3+1] = p[1];  /* G */
            row[x*3+2] = p[0];  /* R */
        }
        fwrite(row, 1, stride, f);
    }
    free(row);
    fclose(f);
    return 0;
}

/* ── BMP reader ──────────────────────────────────────────────────── */
static uint8_t *bmp_read(const char *path, int *w_out, int *h_out){
    FILE *f = fopen(path, "rb");
    if(!f){ perror(path); return NULL; }

    uint8_t bfh[14]; fread(bfh,1,14,f);
    if(bfh[0]!='B'||bfh[1]!='M'){ fclose(f); return NULL; }

    uint8_t dib[40]; fread(dib,1,40,f);
    int32_t w, h_raw;
    memcpy(&w,    dib+4, 4);
    memcpy(&h_raw,dib+8, 4);
    int h = h_raw < 0 ? -h_raw : h_raw;
    int top_down = h_raw < 0;
    *w_out = w; *h_out = h;

    int row_bytes = w * 3;
    int pad       = (4 - (row_bytes % 4)) % 4;
    int stride    = row_bytes + pad;

    uint32_t pix_off; memcpy(&pix_off, bfh+10, 4);
    fseek(f, pix_off, SEEK_SET);

    uint8_t *rgb = malloc((size_t)w * h * 3);
    uint8_t *row = malloc(stride);
    for(int y=0; y<h; y++){
        fread(row, 1, stride, f);
        int dst_y = top_down ? y : (h-1-y);
        for(int x=0; x<w; x++){
            uint8_t *p = rgb + (dst_y*w+x)*3;
            p[0] = row[x*3+2];
            p[1] = row[x*3+1];
            p[2] = row[x*3+0];
        }
    }
    free(row);
    fclose(f);
    return rgb;
}

/* ══════════════════════════════════════════════════════════════════
 * ENCODE: binary → BMP
 * ══════════════════════════════════════════════════════════════════ */
static int cmd_encode_bmp(const char *in_path, const char *bmp_path){
    FILE *f = fopen(in_path, "rb");
    if(!f){ perror(in_path); return 1; }
    fseek(f, 0, SEEK_END);
    size_t orig_size = (size_t)ftell(f);
    rewind(f);
    uint8_t *data = malloc(orig_size);
    fread(data, 1, orig_size, f);
    fclose(f);

    uint64_t digest = xxh64(data, orig_size);

    size_t n_tiles = (orig_size + DATA_PER_TILE - 1) / DATA_PER_TILE;
    if(n_tiles == 0) n_tiles = 1;

    int img_w = TILE_PX;
    int img_h = (int)(n_tiles * TILE_PX);

    uint8_t *rgb = calloc((size_t)img_w * img_h * 3, 1);

    for(size_t ti = 0; ti < n_tiles; ti++){
        size_t base_px = ti * TILE_PX * TILE_PX;
        uint8_t *tp = rgb + base_px * 3;

        tp[0] = MAGIC_R;
        tp[1] = MAGIC_G;
        tp[2] = (uint8_t)(orig_size & 0xFF);
        tp[3] = (uint8_t)((orig_size >>  8) & 0xFF);
        tp[4] = (uint8_t)((orig_size >> 16) & 0xFF);
        tp[5] = (uint8_t)((orig_size >> 24) & 0xFF);

        size_t src_off = ti * DATA_PER_TILE;
        uint8_t *dp = tp + HDR_BYTES;
        size_t   avail = DATA_PER_TILE;
        if(src_off < orig_size){
            size_t copy = orig_size - src_off;
            if(copy > avail) copy = avail;
            memcpy(dp, data + src_off, copy);
        }
    }

    /* store digest in last 8 pixels of last tile */
    size_t last_base = ((n_tiles-1) * TILE_PX * TILE_PX + TILE_PX*TILE_PX - 8) * 3;
    for(int b=0; b<8; b++){
        rgb[last_base + b*3 + 0] = (uint8_t)(digest >> (b*8));
        rgb[last_base + b*3 + 1] = 0xD6;
        rgb[last_base + b*3 + 2] = 0x57;
    }

    int r = bmp_write(bmp_path, rgb, img_w, img_h);
    if(r == 0)
        printf("encode_bmp  %s → %s  (%zuB, %zu tiles, xxh64=%016llX)\n",
               in_path, bmp_path, orig_size, n_tiles,
               (unsigned long long)digest);
    free(rgb); free(data);
    return r;
}

/* ══════════════════════════════════════════════════════════════════
 * DECODE BMP: BMP → original binary
 * ══════════════════════════════════════════════════════════════════ */
static int cmd_decode_bmp(const char *bmp_path, const char *out_path){
    int w, h;
    uint8_t *rgb = bmp_read(bmp_path, &w, &h);
    if(!rgb){ fprintf(stderr,"bmp_read failed: %s\n",bmp_path); return 1; }

    if(w != TILE_PX){
        fprintf(stderr,"unexpected width %d (expected %d)\n",w,TILE_PX);
        free(rgb); return 1;
    }

    int n_tiles = h / TILE_PX;

    uint8_t *tp0 = rgb;
    if(tp0[0] != MAGIC_R || tp0[1] != MAGIC_G){
        fprintf(stderr,"magic mismatch: %02X %02X\n",tp0[0],tp0[1]);
        free(rgb); return 1;
    }
    size_t orig_size = (size_t)tp0[2]
                     | ((size_t)tp0[3] << 8)
                     | ((size_t)tp0[4] << 16)
                     | ((size_t)tp0[5] << 24);

    size_t last_base = ((size_t)(n_tiles-1) * TILE_PX * TILE_PX
                       + TILE_PX*TILE_PX - 8) * 3;
    uint64_t stored_digest = 0;
    for(int b=0; b<8; b++)
        stored_digest |= ((uint64_t)rgb[last_base + b*3]) << (b*8);

    size_t total_avail = (size_t)n_tiles * DATA_PER_TILE;
    uint8_t *out = malloc(total_avail);
    size_t pos = 0;

    for(int ti=0; ti<n_tiles; ti++){
        uint8_t *tp = rgb + (size_t)ti * TILE_PX * TILE_PX * 3;
        uint8_t *dp = tp + HDR_BYTES;
        memcpy(out + pos, dp, DATA_PER_TILE);
        pos += DATA_PER_TILE;
    }

    if(orig_size > total_avail){
        fprintf(stderr,"orig_size %zu > total_avail %zu\n",orig_size,total_avail);
        free(rgb); free(out); return 1;
    }

    uint64_t got = xxh64(out, orig_size);
    if(got != stored_digest){
        fprintf(stderr,"FAIL xxh64: got=%016llX stored=%016llX\n",
                (unsigned long long)got, (unsigned long long)stored_digest);
        free(rgb); free(out); return 2;
    }

    FILE *fout = fopen(out_path, "wb");
    if(!fout){ perror(out_path); free(rgb); free(out); return 1; }
    fwrite(out, 1, orig_size, fout);
    fclose(fout);

    printf("decode_bmp  %s → %s  (%zuB, xxh64 OK %016llX)\n",
           bmp_path, out_path, orig_size,
           (unsigned long long)got);

    free(rgb); free(out);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char **argv){
    if(argc < 3){
        fprintf(stderr,
            "Usage:\n"
            "  %s encode_bmp  <input.bin>  [out.bmp]\n"
            "  %s decode_bmp  <input.bmp>  [out.bin]\n"
            "\n"
            "Then feed out.bmp through geopixel_v21 encode → .gpx,\n"
            "and decode .gpx → bmp, then decode_bmp → original binary.\n",
            argv[0], argv[0]);
        return 1;
    }

    const char *cmd = argv[1];
    const char *src = argv[2];

    if(strcmp(cmd, "encode_bmp") == 0){
        char def[512]; snprintf(def, sizeof(def), "%s.bmp", src);
        const char *dst = (argc >= 4) ? argv[3] : def;
        return cmd_encode_bmp(src, dst);
    }
    if(strcmp(cmd, "decode_bmp") == 0){
        char def[512]; snprintf(def, sizeof(def), "%s.out", src);
        const char *dst = (argc >= 4) ? argv[3] : def;
        return cmd_decode_bmp(src, dst);
    }

    fprintf(stderr, "unknown command: %s\n", cmd);
    return 1;
}
