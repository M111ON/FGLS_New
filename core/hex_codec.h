#pragma once
// hex_codec.h — single-header Hex 7-cell + Gosper L2 block codec  v3
// Usage: #define HEX_CODEC_IMPL in ONE .c file before including
// No external dependencies.
//
// Layout:      0
//           5     1
//              6        ← center (idx 6), encoded FIRST
//           4     2
//              3
//
// Scan order: center(6) first → ring 0→1→2→3→4→5
// Predictor:  XOR dual (cylinder ring + star radial) superimposed
//
// Tile encode format:
//   FLAT     : [0x00, value]             = 2 bytes
//   non-FLAT : [type, res0..res6]        = 8 bytes  (no stored pred)
//              res0 = center raw, res1-6 = ring residuals
//
// L2 block: [n_bytes(1), tile_data...]   max 57 bytes

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ── constants ────────────────────────────────────────────────
#define HEX_CELLS         7
#define HEX_CENTER        6
#define L2_TILES          7
#define L2_CELLS          49

#define HENC_FLAT         0x00
#define HENC_SMOOTH       0x01
#define HENC_GRADIENT     0x02
#define HENC_EDGE         0x03

#define HEX_TILE_MAXBYTES 8
#define L2_MAXBYTES       (1 + L2_TILES * HEX_TILE_MAXBYTES)  // 57

// scan order: center first, then ring
static const uint8_t HEX_SCAN[HEX_CELLS] = {6, 0, 1, 2, 3, 4, 5};

// ── types ────────────────────────────────────────────────────
typedef struct { uint8_t c[HEX_CELLS]; } HexTile;
typedef struct { HexTile tiles[L2_TILES]; } L2Block;
typedef struct { int flat, smooth, grad, edge; } HexTileStats;

// ── declarations ─────────────────────────────────────────────
uint8_t      hex_tile_classify(const HexTile *t);
int          hex_tile_encode(const HexTile *t, uint8_t *dst);
int          hex_tile_decode(const uint8_t *src, size_t src_len, HexTile *t);
int          l2_encode(const L2Block *blk, uint8_t *dst);
int          l2_decode(const uint8_t *src, size_t src_len, L2Block *blk);
HexTileStats l2_stats(const L2Block *blk);

// ── implementation ───────────────────────────────────────────
#ifdef HEX_CODEC_IMPL

// XOR dual predictor: cylinder(ring) XOR star(radial) superimposed
// scan_pos: 0=center, 1-6=ring pos
static uint8_t _xor_pred(const uint8_t *dec, int scan_pos) {
    if (scan_pos == 0) return 128;  // center: no context

    int ring_pos = scan_pos - 1;    // 0-5
    uint8_t center = dec[6];        // always known (encoded first)

    // cylinder: sliding ring neighbors
    uint8_t cyl;
    if (ring_pos == 0) {
        cyl = center;
    } else {
        uint8_t a = dec[HEX_SCAN[scan_pos - 1]];
        uint8_t b = (ring_pos >= 2) ? dec[HEX_SCAN[scan_pos - 2]] : center;
        cyl = (a + b) >> 1;
    }

    // star: center anchor (radial)
    uint8_t star = center;

    // XOR diff → adaptive blend
    uint8_t diff = cyl ^ star;
    if      (diff == 0)    return cyl;               // FLAT: agree
    else if (diff < 0x10)  return (cyl + star) >> 1; // SMOOTH: blend
    else if (diff < 0x80)  return (cyl + star) >> 1; // GRADIENT: blend
    else                   return (~star) & 0xFF;     // EDGE: invert star
}

// XOR classifier: uses avg XOR diff across ring
uint8_t hex_tile_classify(const HexTile *t) {
    // fast FLAT check
    int same = 1;
    for (int i = 1; i < HEX_CELLS; i++) if (t->c[i] != t->c[0]) { same=0; break; }
    if (same) return HENC_FLAT;

    uint8_t center = t->c[6];
    uint32_t diff_sum = 0;
    uint8_t  prev = center;

    for (int rp = 0; rp < 6; rp++) {
        uint8_t b = (rp >= 2) ? t->c[rp-2] : center;
        uint8_t cyl  = (prev + b) >> 1;
        uint8_t star = center;
        diff_sum += (uint8_t)(cyl ^ star);
        prev = t->c[rp];
    }

    uint8_t avg = (uint8_t)(diff_sum / 6);
    if      (avg < 8)   return HENC_SMOOTH;
    else if (avg < 64)  return HENC_GRADIENT;
    else                return HENC_EDGE;
}

int hex_tile_encode(const HexTile *t, uint8_t *dst) {
    uint8_t type = hex_tile_classify(t);
    dst[0] = type;
    if (type == HENC_FLAT) { dst[1] = t->c[0]; return 2; }

    uint8_t dec[HEX_CELLS] = {0};
    for (int si = 0; si < HEX_CELLS; si++) {
        uint8_t ci   = HEX_SCAN[si];
        uint8_t pred = _xor_pred(dec, si);
        dst[1 + si]  = (uint8_t)((t->c[ci] - pred + 128) & 0xFF);
        dec[ci]      = t->c[ci];
    }
    return 8;  // [type, res_center, res_ring0..res_ring5]
}

int hex_tile_decode(const uint8_t *src, size_t src_len, HexTile *t) {
    if (src_len < 2) return -1;
    if (src[0] == HENC_FLAT) {
        for (int i = 0; i < HEX_CELLS; i++) t->c[i] = src[1];
        return 2;
    }
    if (src_len < 8) return -1;

    uint8_t dec[HEX_CELLS] = {0};
    for (int si = 0; si < HEX_CELLS; si++) {
        uint8_t ci   = HEX_SCAN[si];
        uint8_t pred = _xor_pred(dec, si);
        dec[ci]      = (uint8_t)((src[1 + si] - 128 + pred) & 0xFF);
        t->c[ci]     = dec[ci];
    }
    return 8;
}

int l2_encode(const L2Block *blk, uint8_t *dst) {
    int pos = 1;
    for (int i = 0; i < L2_TILES; i++)
        pos += hex_tile_encode(&blk->tiles[i], dst + pos);
    dst[0] = (uint8_t)(pos - 1);
    return pos;
}

int l2_decode(const uint8_t *src, size_t src_len, L2Block *blk) {
    if (src_len < 2) return -1;
    int payload = src[0];
    if ((int)src_len < 1 + payload) return -1;
    int pos = 1;
    for (int i = 0; i < L2_TILES; i++) {
        int n = hex_tile_decode(src + pos, src_len - pos, &blk->tiles[i]);
        if (n < 0) return -1;
        pos += n;
    }
    return 0;
}

HexTileStats l2_stats(const L2Block *blk) {
    HexTileStats s = {0};
    for (int i = 0; i < L2_TILES; i++) {
        switch (hex_tile_classify(&blk->tiles[i])) {
            case HENC_FLAT:     s.flat++;  break;
            case HENC_SMOOTH:   s.smooth++; break;
            case HENC_GRADIENT: s.grad++;  break;
            default:            s.edge++;  break;
        }
    }
    return s;
}

#endif // HEX_CODEC_IMPL
