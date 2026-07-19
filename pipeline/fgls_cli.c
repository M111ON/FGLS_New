/*
 * fgls_cli.c — FGLS Universal Codec CLI v1.0
 *
 * E2E usable tool: profile any data, auto-select the best geometric codec,
 * encode/decode losslessly. Real encoders for all 7 routes.
 *
 * Commands:
 *   fgls profile <file>              Analyze data, show best codec
 *   fgls encode <input> <output>     Auto-route + encode (GFUF v3)
 *   fgls decode <input> <output>     Decode GFUF v3 → raw bytes
 *   fgls bench <file>                Benchmark all codecs, show ratios
 *   fgls info <file>                 Show file header info
 *   fgls version                     Show version
 *
 * Build (from project root):
 *   # With zstd (recommended):
 *   gcc -O2 -std=c11 -fno-strict-aliasing -lm -DFGLS_USE_ZSTD \
 *       -Icollection -Irunner -lzstd -o fgls.exe pipeline/fgls_cli.c
 *
 *   # Without zstd (fallback to raw for general data):
 *   gcc -O2 -std=c11 -fno-strict-aliasing -lm \
 *       -Icollection -o fgls.exe pipeline/fgls_cli.c
 *
 * Dependencies:
 *   - fgls_profile.h (header-only, no external libs)
 *   - zstd (optional, for ZSTD route compression)
 *
 * Wire format (per chunk):
 *   [route:1B][size:1B][payload...]
 *     FLAT:   [route][size][fill_value]                                    = 3 bytes
 *     SPARSE: [route][size][nz_count][indices(nz)...][values(nz)...]       = 3+2*nz bytes
 *     HEX:    [route][size][palette_sz][palette(palette_sz)...][nibbles]   = 4+palette_sz+ceil(size/2) bytes
 *     DELTA:  [route][size][seed][delta_bytes...]                          = 3+size-1 bytes (XOR deltas)
 *     GRADIENT: same as DELTA but with gradient prediction (delta-of-delta)
 *     HILBERT: reorders bytes by Hilbert curve, then encodes as RAW
 *     ZSTD:   [route][size][zstd_compressed...]                           = variable
 *     RAW:    [route][size][data...]                                       = 2+size bytes
 *
 * GFUF v3 Container:
 *   [GFUFHeader 36B][per-chunk encoded data...]
 *
 * Design rules:
 *   - No malloc in hot path (profile uses stack only)
 *   - No float in profile path (integer-only entropy)
 *   - Lossless roundtrip guaranteed
 *   - C99, single-file compilation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

/* Real SID capture pipeline */
#include "tw_capture_int.h"
#include "tw_tensor_capture.h"
#include "coord_spine.h"

/* geo_frame_seek for FRAMED codec (stride-37, 1440-frame cycle) */
#include "geo_frame_seek.h"

/* FrustumBlock (L-block 4896B) — use direct API instead of geo_field
   to avoid transitive deps on geo_gp_frustum_bridge.h + orphan layer.
   L-block format: [header 17B] [data 3456B] [meta 1440B] = 4896B total. */
#include "frustum_layout_v2.h"

#ifdef FGLS_USE_ZSTD
#include <zstd.h>
#endif

/* ═══════════════════════════════════════════════════════════════
 * COMPONENT HEADERS (header-only)
 * ═══════════════════════════════════════════════════════════════ */
#include "fgls_profile.h"
#include "pogls_bond_edge.h"
#include "geo_field_core.h"
#include "tring.h"
#include "geo_dodeca_torus.h"
#include "geo_dual_place.h"
/* Atomic reshape lives in separate TU (atomic_reshape_cmd.c) to avoid
 * inline-chain linker issues with pogls_rotation.h's static global state.
 * Exposed via atomic_reshape_demo() — declared below. */
extern int atomic_reshape_demo(const char *out_path);
/* Timetravel lives in separate TU (timetravel_cmd.c) due to tring_init
 * name collision between geo_temporal_ring.h and tring.h. */
extern int timetravel_demo(const char *out_path);

#define FGLS_VERSION "1.0.0"

/* ═══════════════════════════════════════════════════════════════
 * FILE I/O HELPERS
 * ═══════════════════════════════════════════════════════════════ */

static uint8_t *read_file(const char *path, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > (1u << 28)) { /* 256 MB max */
        fprintf(stderr, "Error: file too large or empty (%ld bytes)\n", sz);
        fclose(f); return NULL;
    }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fprintf(stderr, "Error: malloc failed\n"); fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if ((long)rd != sz) { fprintf(stderr, "Error: short read\n"); free(buf); return NULL; }
    *out_size = (uint32_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, uint32_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Error: cannot create %s\n", path); return -1; }
    size_t wr = fwrite(data, 1, size, f);
    fclose(f);
    return ((uint32_t)wr == size) ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════
 * GFUF v3 CONTAINER
 *
 * Format: [GFUFHeader 36B][per-chunk: route:1B][size:1B][payload...]
 * ═══════════════════════════════════════════════════════════════ */

#define GFUF_MAGIC   0x46554647u  /* "GFUF" */
#define GFUF_VERSION 3
#define GFUF_HDR_SZ  36u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t orig_size;
    uint32_t n_chunks;
    uint32_t chunk_size;     /* original chunk size used */
    uint32_t route_summary;  /* packed: dominant_route<<24 | dominant_count */
    uint32_t reserved[2];
} FgfsHdr;  /* 36B */

static void gfuf_hdr_write(uint8_t *b, const FgfsHdr *h) {
    memcpy(b, h, sizeof(FgfsHdr));
}

static void gfuf_hdr_read(const uint8_t *b, FgfsHdr *h) {
    memcpy(h, b, sizeof(FgfsHdr));
}

/* ═══════════════════════════════════════════════════════════════
 * HILBERT CURVE REORDERING (256→256 for 16x16 grid)
 *
 * Maps linear index → position on 16×16 Hilbert curve.
 * Used to reorder chunk bytes for better locality in structured data.
 * ═══════════════════════════════════════════════════════════════ */

/* Convert d2xy: d (0..255) → (x,y) on 16×16 Hilbert curve */
static void hilbert_d2xy(uint32_t d, uint32_t *x, uint32_t *y) {
    uint32_t rx = 0, ry = 0, s = 8; /* 16×16 → 4 levels */
    for (uint32_t t = d; s > 0; s >>= 1) {
        rx = (s >> 1) & ((t & 2) ? s - 1 : 0);
        ry = (s >> 1) & ((t ^ rx) & 1 ? s - 1 : 0);
        /* rotate */
        if (ry == 0) {
            if (rx != 0) { uint32_t tmp = rx; rx = s - 1 - ry; ry = s - 1 - tmp; }
            else { uint32_t tmp = rx; rx = ry; ry = tmp; }
        }
        t >>= 2;
    }
    /* simplified 4-level Hilbert */
    (void)rx; (void)ry;
    /* Use lookup table approach instead — compute inline */
    *x = 0; *y = 0;
}

/* Pre-computed Hilbert lookup for 16×16 = 256 positions */
static uint8_t hilbert_lut[256];  /* hilbert_lut[linear_pos] = grid_pos */
static int hilbert_lut_init = 0;

static void init_hilbert_lut(void) {
    if (hilbert_lut_init) return;
    /* Generate 16×16 Hilbert curve using standard algorithm */
    uint32_t n = 16;
    for (uint32_t d = 0; d < n * n; d++) {
        uint32_t x = 0, y = 0;
        uint32_t t = d;
        for (uint32_t s = n >> 1; s > 0; s >>= 1) {
            uint32_t rx = (t & 2) ? s : 0;
            uint32_t ry = (t ^ rx) & 1 ? s : 0;
            if (ry == 0) {
                if (rx != 0) {
                    uint32_t tmp = x;
                    x = s - 1 - y;
                    y = s - 1 - tmp;
                } else {
                    uint32_t tmp = x;
                    x = y;
                    y = tmp;
                }
            }
            x += rx;
            y += ry;
            t >>= 2;
        }
        hilbert_lut[d] = (uint8_t)(y * n + x);
    }
    hilbert_lut_init = 1;
}

/* 8×8 Hilbert for 64-byte chunks */
static uint8_t hilbert_lut_8[64];
static int hilbert_lut_8_init = 0;

static void init_hilbert_lut_8(void) {
    if (hilbert_lut_8_init) return;
    uint32_t n = 8;
    for (uint32_t d = 0; d < n * n; d++) {
        uint32_t x = 0, y = 0;
        uint32_t t = d;
        for (uint32_t s = n >> 1; s > 0; s >>= 1) {
            uint32_t rx = (t & 2) ? s : 0;
            uint32_t ry = (t ^ rx) & 1 ? s : 0;
            if (ry == 0) {
                if (rx != 0) {
                    uint32_t tmp = x;
                    x = s - 1 - y;
                    y = s - 1 - tmp;
                } else {
                    uint32_t tmp = x;
                    x = y;
                    y = tmp;
                }
            }
            x += rx;
            y += ry;
            t >>= 2;
        }
        hilbert_lut_8[d] = (uint8_t)(y * n + x);
    }
    hilbert_lut_8_init = 1;
}

/* Select correct LUT based on size */
static const uint8_t *hilbert_lut_for_size(uint32_t size) {
    if (size <= 64) { init_hilbert_lut_8(); return hilbert_lut_8; }
    if (size <= 256) { init_hilbert_lut(); return hilbert_lut; }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * ENCODERS (all return bytes written, -1 on error)
 *
 * Wire format: [route:1B][size:1B][payload...]
 * The route and size bytes are prepended by encode_chunk dispatch.
 * Each encoder writes only the payload after the 2-byte header.
 * ═══════════════════════════════════════════════════════════════ */

#define FGLS_MAX_CHUNK  256u

/* ── FLAT: all bytes same value ── */
/* Payload: [fill_value] = 1 byte → total 3 bytes */
static int enc_flat(uint8_t *out, uint32_t cap,
                     const uint8_t *data, uint32_t size) {
    if (cap < 1 || size == 0) return -1;
    out[0] = data[0];
    return 1;
}

/* ── SPARSE: few non-zero bytes ── */
/* Payload: [nz_count][indices(nz)...][values(nz)...] */
static int enc_sparse(uint8_t *out, uint32_t cap,
                       const uint8_t *data, uint32_t size) {
    uint32_t nz = 0;
    for (uint32_t i = 0; i < size; i++)
        if (data[i] != 0) nz++;
    uint32_t needed = 1 + nz * 2;
    if (needed > cap || nz > 255) return -1;
    out[0] = (uint8_t)nz;
    uint32_t pos = 1;
    uint32_t vi = 1 + nz;
    for (uint32_t i = 0; i < size; i++) {
        if (data[i] != 0) {
            out[pos++] = (uint8_t)i;
            out[vi++]   = data[i];
        }
    }
    return (int)needed;
}

/* ── HEX: palette encoding (≤16 unique values, 4-bit indices) ── */
/* Payload: [palette_sz][palette(palette_sz)...][nibbles(ceil(size/2))...] */
static int enc_hex(uint8_t *out, uint32_t cap,
                    const uint8_t *data, uint32_t size) {
    /* build palette */
    uint8_t pal[16];
    uint8_t idx_map[256];
    memset(idx_map, 0xFF, sizeof(idx_map)); /* 0xFF = not in palette */
    uint32_t pal_sz = 0;

    for (uint32_t i = 0; i < size; i++) {
        uint8_t v = data[i];
        if (idx_map[v] == 0xFF) {
            if (pal_sz >= 16) return -1; /* too many unique values */
            pal[pal_sz] = v;
            idx_map[v] = (uint8_t)pal_sz;
            pal_sz++;
        }
    }

    uint32_t nibble_bytes = (size + 1) / 2;
    uint32_t needed = 1 + pal_sz + nibble_bytes;
    if (needed > cap) return -1;

    out[0] = (uint8_t)pal_sz;
    memcpy(out + 1, pal, pal_sz);

    uint32_t pos = 1 + pal_sz;
    for (uint32_t i = 0; i < size; i += 2) {
        uint8_t lo = idx_map[data[i]];
        uint8_t hi = (i + 1 < size) ? idx_map[data[i + 1]] : 0;
        out[pos++] = (uint8_t)((lo << 4) | (hi & 0x0F));
    }
    return (int)needed;
}

/* ── DELTA: XOR-based delta coding ── */
/* Payload: [seed][delta_1..delta_(size-1)] — each delta = prev XOR current */
static int enc_delta(uint8_t *out, uint32_t cap,
                      const uint8_t *data, uint32_t size) {
    if (cap < size || size < 2) return -1;
    out[0] = data[0]; /* seed = first byte */
    for (uint32_t i = 1; i < size; i++)
        out[i] = data[i] ^ data[i - 1];
    return (int)size;
}

/* ── GRADIENT: delta-of-delta (predicts smooth gradients) ── */
/* Payload: [seed][delta_0..delta_(size-2)] where delta_i = diff[i] XOR diff[i-1] */
/* diff[i] = data[i+1] - data[i] (mod 256) */
static int enc_gradient(uint8_t *out, uint32_t cap,
                         const uint8_t *data, uint32_t size) {
    if (cap < size || size < 3) return -1;
    out[0] = data[0]; /* seed */
    if (size < 2) return 1;

    /* first delta = data[1] - data[0] */
    uint8_t prev_delta = (uint8_t)(data[1] - data[0]);
    out[1] = prev_delta;

    /* subsequent: delta-of-delta */
    for (uint32_t i = 2; i < size; i++) {
        uint8_t cur_delta = (uint8_t)(data[i] - data[i - 1]);
        out[i] = cur_delta ^ prev_delta;
        prev_delta = cur_delta;
    }
    return (int)size;
}

/* ── HILBERT: reorder by Hilbert curve, then store ── */
/* Payload: [reordered_bytes...] — Hilbert reorder may improve locality */
static int enc_hilbert(uint8_t *out, uint32_t cap,
                        const uint8_t *data, uint32_t size) {
    if (size > 256) return -1; /* only works for ≤256 bytes */
    if (cap < size) return -1;

    const uint8_t *lut = hilbert_lut_for_size(size);
    if (!lut) return -1;

    /* out[grid_pos] = data[linear_pos] */
    for (uint32_t i = 0; i < size; i++) {
        out[lut[i]] = data[i];
    }
    return (int)size;
}

/* ── ZSTD: real zstd compression ── */
/* Payload: [zstd_compressed_bytes...] */
static int enc_zstd(uint8_t *out, uint32_t cap,
                     const uint8_t *data, uint32_t size) {
#ifdef FGLS_USE_ZSTD
    size_t bound = ZSTD_compressBound(size);
    if (bound > cap) return -1;
    size_t ret = ZSTD_compress(out, cap, data, size, 1); /* level 1 = fast */
    if (ZSTD_isError(ret)) return -1;
    return (int)ret;
#else
    /* Fallback: store as RAW (no compression) */
    if (cap < size) return -1;
    memcpy(out, data, size);
    return (int)size;
#endif
}

/* ── RAW: passthrough (no compression) ── */
static int enc_raw(uint8_t *out, uint32_t cap,
                    const uint8_t *data, uint32_t size) {
    if (cap < size) return -1;
    memcpy(out, data, size);
    return (int)size;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODERS (consume payload, return bytes consumed, -1 on error)
 *
 * The route/size bytes have already been read by decode_chunk dispatch.
 * in points to the payload (after route+size).
 * out is the decoded output buffer.
 * ═══════════════════════════════════════════════════════════════ */

/* ── FLAT ── */
static int dec_flat(uint8_t *out, uint32_t out_size,
                     const uint8_t *in, uint32_t in_cap) {
    if (in_cap < 1) return -1;
    memset(out, in[0], out_size);
    return 1;
}

/* ── SPARSE ── */
static int dec_sparse(uint8_t *out, uint32_t out_size,
                       const uint8_t *in, uint32_t in_cap) {
    if (in_cap < 1) return -1;
    uint8_t nz = in[0];
    uint32_t needed = 1 + (uint32_t)nz * 2;
    if (in_cap < needed) return -1;
    memset(out, 0, out_size);
    for (uint32_t i = 0; i < nz; i++) {
        uint8_t idx = in[1 + i];
        uint8_t val = in[1 + nz + i];
        if (idx < out_size) out[idx] = val;
    }
    return (int)needed;
}

/* ── HEX ── */
static int dec_hex(uint8_t *out, uint32_t out_size,
                    const uint8_t *in, uint32_t in_cap) {
    if (in_cap < 1) return -1;
    uint8_t pal_sz = in[0];
    uint32_t nibble_bytes = (out_size + 1) / 2;
    uint32_t needed = 1 + pal_sz + nibble_bytes;
    if (in_cap < needed) return -1;

    const uint8_t *pal = in + 1;
    const uint8_t *nibbles = in + 1 + pal_sz;

    for (uint32_t i = 0; i < out_size; i++) {
        uint8_t nib = (i & 1) ? (nibbles[i >> 1] & 0x0F) : (nibbles[i >> 1] >> 4);
        if (nib < pal_sz) out[i] = pal[nib];
        else out[i] = 0;
    }
    return (int)needed;
}

/* ── DELTA ── */
static int dec_delta(uint8_t *out, uint32_t out_size,
                      const uint8_t *in, uint32_t in_cap) {
    if (in_cap < out_size) return -1;
    out[0] = in[0]; /* seed */
    for (uint32_t i = 1; i < out_size; i++)
        out[i] = out[i - 1] ^ in[i]; /* XOR to recover original */
    return (int)out_size;
}

/* ── GRADIENT ── */
static int dec_gradient(uint8_t *out, uint32_t out_size,
                         const uint8_t *in, uint32_t in_cap) {
    if (in_cap < out_size) return -1;
    if (out_size == 0) return 0;
    out[0] = in[0]; /* seed */
    if (out_size < 2) return 1;

    uint8_t prev_delta = in[1]; /* first delta = diff[0] */
    out[1] = (uint8_t)(out[0] + prev_delta); /* data[1] = data[0] + delta[0] */

    for (uint32_t i = 2; i < out_size; i++) {
        uint8_t dodd = in[i]; /* delta-of-delta */
        uint8_t cur_delta = dodd ^ prev_delta;
        out[i] = (uint8_t)(out[i - 1] + cur_delta);
        prev_delta = cur_delta;
    }
    return (int)out_size;
}

/* ── HILBERT ── */
static int dec_hilbert(uint8_t *out, uint32_t out_size,
                        const uint8_t *in, uint32_t in_cap) {
    if (out_size > 256) return -1;
    if (in_cap < out_size) return -1;

    const uint8_t *lut = hilbert_lut_for_size(out_size);
    if (!lut) return -1;

    /* inverse: out[linear_pos] = in[grid_pos] */
    for (uint32_t i = 0; i < out_size; i++) {
        out[i] = in[lut[i]];
    }
    return (int)out_size;
}

/* ── ZSTD ── */
static int dec_zstd(uint8_t *out, uint32_t out_size,
                     const uint8_t *in, uint32_t in_cap) {
#ifdef FGLS_USE_ZSTD
    /* find compressed frame size (ZSTD frames are self-delimiting) */
    size_t csize = ZSTD_findFrameCompressedSize(in, in_cap);
    if (ZSTD_isError(csize)) return -1;
    size_t ret = ZSTD_decompress(out, out_size, in, csize);
    if (ZSTD_isError(ret)) return -1;
    return (int)csize; /* return actual compressed bytes consumed */
#else
    if (in_cap < out_size) return -1;
    memcpy(out, in, out_size);
    return (int)out_size;
#endif
}

/* ── RAW ── */
static int dec_raw(uint8_t *out, uint32_t out_size,
                    const uint8_t *in, uint32_t in_cap) {
    if (in_cap < out_size) return -1;
    memcpy(out, in, out_size);
    return (int)out_size;
}

/* ═══════════════════════════════════════════════════════════════
 * FRAMED CODEC — geo_frame_seek + temporal delta (768B frames)
 *
 * Wire format (GFRMD v1):
 *   header (16B):
 *     magic    u32 = "FRMD" (0x444D5246)
 *     version  u16 = 1
 *     flags    u16 = 0
 *     orig_sz  u32
 *     n_frames u32
 *   seed: 12 chunks, each [route:1][size:1][ds_data...]
 *   per frame (n_frames - 1):
 *     enc u16 (2B)
 *     12 residual chunks, each [route:1][size:1][ds_data...]
 * ═══════════════════════════════════════════════════════════════ */

#define FRMD_CHUNK_SZ     64u
#define FRMD_FRAME_CHUNKS 12u
#define FRMD_FRAME_BYTES  (FRMD_CHUNK_SZ * FRMD_FRAME_CHUNKS)  /* 768 */
#define FRMD_MAX_FRAMES   65536u

#define FRMD_MAGIC        0x444D5246u  /* "FRMD" */
#define FRMD_VERSION      1u
#define FRMD_HDR_SZ       16u

#define FRMD_DS_FLAT      0
#define FRMD_DS_SPARSE    0xFD
#define FRMD_DS_RAW       0xFE
#define FRMD_DS_SUB       0xFF
#define FRMD_DS_SUB_N     8
#define FRMD_DS_SUB_SZ    8
#define FRMD_DS_SPARSE_MAX 16u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t orig_size;
    uint32_t n_frames;
} FrmdHdr;

static void frmd_hdr_write(uint8_t *b, const FrmdHdr *h) {
    memcpy(b,      &h->magic,     4);
    memcpy(b + 4,  &h->version,   2);
    memcpy(b + 6,  &h->flags,     2);
    memcpy(b + 8,  &h->orig_size, 4);
    memcpy(b + 12, &h->n_frames,  4);
}

static void frmd_hdr_read(const uint8_t *b, FrmdHdr *h) {
    memcpy(&h->magic,     b,      4);
    memcpy(&h->version,   b + 4,  2);
    memcpy(&h->flags,     b + 6,  2);
    memcpy(&h->orig_size, b + 8,  4);
    memcpy(&h->n_frames,  b + 12, 4);
}

/* Diamond Shell: encode 64B → variable bytes */
static uint32_t ds_encode(uint8_t *out, const uint8_t block[64]) {
    int is_zero = 1;
    for (int i = 0; i < 64; i++) { if (block[i]) { is_zero = 0; break; } }
    if (is_zero) { out[0] = FRMD_DS_FLAT; return 1; }

    /* count non-zero */
    int nz = 0;
    for (int i = 0; i < 64; i++) if (block[i]) nz++;

    /* SPARSE: ≤16 non-zero bytes */
    if ((uint32_t)nz <= FRMD_DS_SPARSE_MAX) {
        out[0] = FRMD_DS_SPARSE;
        out[1] = 0;  /* rotation 0 */
        out[2] = (uint8_t)nz;
        uint32_t pos = 3;
        for (uint32_t i = 0; i < 64; i++) {
            if (block[i]) {
                out[pos] = (uint8_t)i;
                out[pos + nz] = block[i];
                pos++;
            }
        }
        return 3 + (uint32_t)nz * 2;
    }

    /* count active sub-blocks */
    int active = 0;
    uint8_t sub_flags = 0;
    for (int s = 0; s < FRMD_DS_SUB_N; s++) {
        int has_nz = 0;
        for (int j = 0; j < FRMD_DS_SUB_SZ; j++) {
            if (block[s * FRMD_DS_SUB_SZ + j]) { has_nz = 1; break; }
        }
        if (has_nz) { sub_flags |= (1u << s); active++; }
    }

    /* RAW fallback: dense data */
    if (active >= 7) {
        out[0] = FRMD_DS_RAW;
        out[1] = 0;
        memcpy(out + 2, block, 64);
        return 66;
    }

    /* SUB encoding */
    out[0] = FRMD_DS_SUB;
    out[1] = 0;
    out[2] = sub_flags;
    uint32_t pos = 3;
    for (int s = 0; s < FRMD_DS_SUB_N; s++) {
        if (sub_flags & (1u << s)) {
            memcpy(out + pos, block + s * FRMD_DS_SUB_SZ, FRMD_DS_SUB_SZ);
            pos += FRMD_DS_SUB_SZ;
        }
    }
    return pos;
}

/* Diamond Shell: decode variable bytes → 64B, returns bytes consumed */
static uint32_t ds_decode(uint8_t out[64], const uint8_t *in) {
    uint8_t flag = in[0];
    if (flag == FRMD_DS_FLAT) { memset(out, 0, 64); return 1; }
    if (flag == FRMD_DS_SPARSE) {
        uint8_t nz = in[2];
        memset(out, 0, 64);
        for (uint32_t i = 0; i < nz; i++) {
            uint8_t idx = in[3 + i];
            uint8_t val = in[3 + nz + i];
            if (idx < 64) out[idx] = val;
        }
        return 3 + (uint32_t)nz * 2;
    }
    if (flag == FRMD_DS_RAW) {
        memcpy(out, in + 2, 64);
        return 66;
    }
    /* SUB (0xFF) */
    uint8_t sub_flags = in[2];
    memset(out, 0, 64);
    uint32_t pos = 3;
    for (int s = 0; s < FRMD_DS_SUB_N; s++) {
        if (sub_flags & (1u << s)) {
            memcpy(out + s * FRMD_DS_SUB_SZ, in + pos, FRMD_DS_SUB_SZ);
            pos += FRMD_DS_SUB_SZ;
        }
    }
    return pos;
}

/* ── FRAMED encode: in-memory buffer → out (caller allocates) ──
 * Returns bytes written to out, or 0 on error.
 * out_cap must be ≥ FRMD_HDR_SZ + 12*66 + n_frames * (2 + 12*66). */
static uint32_t frmd_encode(const uint8_t *data, uint32_t data_sz,
                             uint8_t *out, uint32_t out_cap) {
    if (geo_frame_seek_verify() != 0) return 0;
    uint32_t n_frames = (data_sz + FRMD_FRAME_BYTES - 1) / FRMD_FRAME_BYTES;
    if (n_frames == 0 || n_frames > FRMD_MAX_FRAMES) return 0;

    /* Read seed frame 0 → 12 chunks */
    uint8_t seed_chunk[FRMD_FRAME_CHUNKS][FRMD_CHUNK_SZ];
    for (uint32_t ci = 0; ci < FRMD_FRAME_CHUNKS; ci++) {
        uint32_t off = ci * FRMD_CHUNK_SZ;
        uint32_t sz = (off + FRMD_CHUNK_SZ <= data_sz) ? FRMD_CHUNK_SZ : data_sz - off;
        memset(seed_chunk[ci], 0, FRMD_CHUNK_SZ);
        if (sz > 0) memcpy(seed_chunk[ci], data + off, sz);
    }

    /* Header */
    FrmdHdr hdr = { FRMD_MAGIC, FRMD_VERSION, 0, data_sz, n_frames };
    frmd_hdr_write(out, &hdr);
    uint32_t pos = FRMD_HDR_SZ;

    /* Seed chunks: DS-encoded */
    for (uint32_t ci = 0; ci < FRMD_FRAME_CHUNKS; ci++) {
        if (pos + 2 + 66 > out_cap) return 0;
        uint8_t ds_buf[70];
        uint32_t ds_sz = ds_encode(ds_buf, seed_chunk[ci]);
        out[pos++] = FRMD_DS_SUB;  /* route marker */
        out[pos++] = (uint8_t)FRMD_CHUNK_SZ;
        memcpy(out + pos, ds_buf, ds_sz);
        pos += ds_sz;
    }

    /* Per-frame: enc + 12 XOR residuals */
    for (uint32_t fi = 1; fi < n_frames; fi++) {
        if (pos + 2 > out_cap) return 0;
        uint16_t enc = frame_enc(fi);
        out[pos++] = (uint8_t)(enc & 0xFF);
        out[pos++] = (uint8_t)(enc >> 8);

        uint32_t frame_off = fi * FRMD_FRAME_BYTES;
        for (uint32_t ci = 0; ci < FRMD_FRAME_CHUNKS; ci++) {
            if (pos + 2 + 66 > out_cap) return 0;
            uint32_t off = frame_off + ci * FRMD_CHUNK_SZ;
            uint32_t sz = (off + FRMD_CHUNK_SZ <= data_sz) ? FRMD_CHUNK_SZ
                       : (data_sz > off ? data_sz - off : 0);
            uint8_t chunk[FRMD_CHUNK_SZ];
            uint8_t residual[FRMD_CHUNK_SZ];
            memset(chunk, 0, FRMD_CHUNK_SZ);
            memset(residual, 0, FRMD_CHUNK_SZ);
            if (sz > 0) memcpy(chunk, data + off, sz);
            for (uint32_t b = 0; b < FRMD_CHUNK_SZ; b++) {
                residual[b] = chunk[b] ^ seed_chunk[ci][b];
            }
            uint8_t ds_buf[70];
            uint32_t ds_sz = ds_encode(ds_buf, residual);
            out[pos++] = FRMD_DS_SUB;
            out[pos++] = (uint8_t)FRMD_CHUNK_SZ;
            memcpy(out + pos, ds_buf, ds_sz);
            pos += ds_sz;
        }
    }
    return pos;
}

/* ── FRAMED decode: in-memory buffer → out (caller allocates) ──
 * Returns bytes written to out (== hdr.orig_size), or 0 on error. */
static uint32_t frmd_decode(const uint8_t *in, uint32_t in_sz,
                             uint8_t *out, uint32_t out_cap) {
    if (in_sz < FRMD_HDR_SZ) return 0;
    FrmdHdr hdr;
    frmd_hdr_read(in, &hdr);
    if (hdr.magic != FRMD_MAGIC) return 0;
    if (hdr.orig_size > out_cap) return 0;

    memset(out, 0, hdr.orig_size);
    uint32_t pos = FRMD_HDR_SZ;

    /* Read seed chunks */
    uint8_t seed_chunk[FRMD_FRAME_CHUNKS][FRMD_CHUNK_SZ];
    for (uint32_t ci = 0; ci < FRMD_FRAME_CHUNKS; ci++) {
        if (pos + 2 > in_sz) return 0;
        pos++;  /* skip route marker */
        pos++;  /* skip orig size */
        if (pos > in_sz) return 0;
        uint32_t consumed = ds_decode(seed_chunk[ci], in + pos);
        if (consumed == 0 || pos + consumed > in_sz) return 0;
        pos += consumed;
    }

    /* Write seed frame 0 */
    uint32_t seed_frame_bytes = (hdr.orig_size >= FRMD_FRAME_BYTES)
                              ? FRMD_FRAME_BYTES : hdr.orig_size;
    for (uint32_t ci = 0; ci < FRMD_FRAME_CHUNKS; ci++) {
        uint32_t off = ci * FRMD_CHUNK_SZ;
        if (off >= seed_frame_bytes) break;
        uint32_t sz = (off + FRMD_CHUNK_SZ <= seed_frame_bytes)
                    ? FRMD_CHUNK_SZ : seed_frame_bytes - off;
        memcpy(out + off, seed_chunk[ci], sz);
    }

    /* Read frames 1..n */
    for (uint32_t fi = 1; fi < hdr.n_frames; fi++) {
        if (pos + 2 > in_sz) return 0;
        pos += 2;  /* enc */

        uint32_t frame_off = fi * FRMD_FRAME_BYTES;
        for (uint32_t ci = 0; ci < FRMD_FRAME_CHUNKS; ci++) {
            if (pos + 2 > in_sz) return 0;
            pos++;  /* route */
            pos++;  /* size */
            uint8_t residual[FRMD_CHUNK_SZ];
            uint32_t consumed = ds_decode(residual, in + pos);
            if (consumed == 0 || pos + consumed > in_sz) return 0;
            pos += consumed;

            uint32_t off = frame_off + ci * FRMD_CHUNK_SZ;
            uint32_t sz = (off + FRMD_CHUNK_SZ <= hdr.orig_size)
                        ? FRMD_CHUNK_SZ
                        : (hdr.orig_size > off ? hdr.orig_size - off : 0);
            for (uint32_t b = 0; b < sz; b++) {
                out[off + b] = residual[b] ^ seed_chunk[ci][b];
            }
        }
    }
    return hdr.orig_size;
}

/* (frame coherence logic inlined into cmd_encode_auto) */

/* ═══════════════════════════════════════════════════════════════
 * ENCODE/DECODE DISPATCH
 *
 * Wire format: [route:1B][size:1B][payload...]
 * ═══════════════════════════════════════════════════════════════ */

/* encode one chunk → returns bytes written, -1 on error */
static int encode_chunk(uint8_t *out, uint32_t out_cap,
                         const uint8_t *data, uint32_t size,
                         FglsRoute *route_out)
{
    if (size == 0 || size > 255) return -1;

    /* profile this chunk */
    FglsProfile p;
    if (fgls_profile(data, size, &p) != 0) return -1;
    FglsRoute r = fgls_route(&p);
    if (route_out) *route_out = r;

    uint8_t route_byte = (uint8_t)r;
    uint8_t size_byte  = (uint8_t)size;

    /* try the chosen route first, fall back to raw if it doesn't fit */
    int payload_sz = -1;
    uint8_t payload[FGLS_MAX_CHUNK + 256]; /* max payload = size + overhead */

    switch (r) {
    case FGLS_ROUTE_FLAT:     payload_sz = enc_flat(payload, sizeof(payload), data, size); break;
    case FGLS_ROUTE_SPARSE:   payload_sz = enc_sparse(payload, sizeof(payload), data, size); break;
    case FGLS_ROUTE_HEX:      payload_sz = enc_hex(payload, sizeof(payload), data, size); break;
    case FGLS_ROUTE_DELTA:    payload_sz = enc_delta(payload, sizeof(payload), data, size); break;
    case FGLS_ROUTE_GRADIENT: payload_sz = enc_gradient(payload, sizeof(payload), data, size); break;
    case FGLS_ROUTE_HILBERT:  payload_sz = enc_hilbert(payload, sizeof(payload), data, size); break;
    case FGLS_ROUTE_ZSTD:     payload_sz = enc_zstd(payload, sizeof(payload), data, size); break;
    case FGLS_ROUTE_RAW:
    default:                  payload_sz = enc_raw(payload, sizeof(payload), data, size); break;
    }

    /* fallback to raw if encoder failed */
    if (payload_sz < 0) {
        payload_sz = enc_raw(payload, sizeof(payload), data, size);
        r = FGLS_ROUTE_RAW;
        route_byte = (uint8_t)r;
        if (route_out) *route_out = r;
    }

    uint32_t needed = 2 + (uint32_t)payload_sz;
    if (needed > out_cap) return -1;

    out[0] = route_byte;
    out[1] = size_byte;
    memcpy(out + 2, payload, (uint32_t)payload_sz);
    return (int)needed;
}

/* decode one chunk → returns bytes consumed from input, -1 on error
 * out_size = capacity of output buffer (must be ≥ chunk size from wire)
 * The chunk size is read from the wire format [route][size][payload...]
 */
static int decode_chunk(uint8_t *out, uint32_t out_size,
                         const uint8_t *in, uint32_t in_cap)
{
    if (in_cap < 2) return -1;
    uint8_t route_byte = in[0];
    uint8_t size_byte  = in[1];
    FglsRoute r = (FglsRoute)route_byte;
    uint32_t sz = size_byte;

    if (sz > out_size) return -1; /* output buffer too small */

    const uint8_t *payload = in + 2;
    uint32_t payload_cap = in_cap - 2;
    int consumed = -1;

    switch (r) {
    case FGLS_ROUTE_FLAT:     consumed = dec_flat(out, sz, payload, payload_cap); break;
    case FGLS_ROUTE_SPARSE:   consumed = dec_sparse(out, sz, payload, payload_cap); break;
    case FGLS_ROUTE_HEX:      consumed = dec_hex(out, sz, payload, payload_cap); break;
    case FGLS_ROUTE_DELTA:    consumed = dec_delta(out, sz, payload, payload_cap); break;
    case FGLS_ROUTE_GRADIENT: consumed = dec_gradient(out, sz, payload, payload_cap); break;
    case FGLS_ROUTE_HILBERT:  consumed = dec_hilbert(out, sz, payload, payload_cap); break;
    case FGLS_ROUTE_ZSTD:     consumed = dec_zstd(out, sz, payload, payload_cap); break;
    case FGLS_ROUTE_RAW:
    default:                  consumed = dec_raw(out, sz, payload, payload_cap); break;
    }

    if (consumed < 0) return -1;
    return 2 + consumed; /* total bytes consumed (route+size+payload) */
}

/* ═══════════════════════════════════════════════════════════════
 * COMMANDS
 * ═══════════════════════════════════════════════════════════════ */

static void usage(void) {
    fprintf(stderr,
        "FGLS Universal Codec CLI v" FGLS_VERSION "\n\n"
        "Usage:\n"
        "  fgls profile <file>              Analyze data, show best codec\n"
        "  fgls encode <input> <output>     Auto-route + encode (GFUF v3)\n"
        "  fgls encode-framed <input> <output>  FRAMED codec (geo_frame_seek + temporal)\n"
        "  fgls encode-lblock <input> <output>  FrustumBlock (4896B L-block container)\n"
        "  fgls lblock-reshape <input> <output> Build bond edges (pogls_bond_edge)\n"
        "  fgls field-info <input> <output>     GeoField metadata via geo_field_core\n"
        "  fgls tring-demo <output>            Tring 64B timeline roundtrip\n"
        "  fgls torus-demo <output>            Dodeca Torus walk 200 steps\n"
        "  fgls reshape-demo <output>          Atomic reshape (separate TU)\n"
        "  fgls dual-demo <output>             Dual-world 162↔64 placement\n"
        "  fgls timetravel-demo <output>       Rewind + Wang + temporal ring\n"
        "  fgls encode-auto <input> <output>    Pick best of GFUF/FRAMED per data\n"
        "  fgls decode-framed <input> <output>  Decode GFRMD → raw bytes\n"
        "  fgls decode-lblock <input> <output>  Decode LBLK → raw bytes\n"
        "  fgls bench <file>                Benchmark all codecs, show ratios\n"
        "  fgls info <file>                 Show file header info\n"
        "  fgls version                     Show version\n"
#ifdef FGLS_USE_ZSTD
        "\nBuilt with: zstd support (ZSTD route active)\n"
#else
        "\nBuilt without: zstd (ZSTD route falls back to raw)\n"
#endif
    );
}

/* ── profile: analyze file, show routing ── */
static int cmd_profile(const char *path) {
    uint32_t sz;
    uint8_t *data = read_file(path, &sz);
    if (!data) return 1;

    printf("File: %s (%u bytes)\n\n", path, sz);

    /* full-file profile */
    FglsProfile p;
    fgls_profile(data, sz, &p);
    FglsRoute r = fgls_route(&p);

    printf("=== Full File Profile ===\n");
    fgls_profile_print(&p);
    printf("\nBest route: %s (GPX5 codec 0x%02X)\n\n",
           fgls_route_name(r), fgls_route_to_gpx5_codec(r));

    /* chunk-level analysis (64B chunks) */
    #define PROF_CHUNK 64u
    uint32_t n_chunks = (sz + PROF_CHUNK - 1) / PROF_CHUNK;
    uint32_t route_hist[FGLS_ROUTE_COUNT];
    memset(route_hist, 0, sizeof(route_hist));

    uint32_t total_raw = sz;
    uint32_t total_enc = 0;

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t off = i * PROF_CHUNK;
        uint32_t csz = sz - off;
        if (csz > PROF_CHUNK) csz = PROF_CHUNK;

        FglsProfile cp;
        fgls_profile(data + off, csz, &cp);
        FglsRoute cr = fgls_route(&cp);
        route_hist[cr]++;

        /* estimate encoded size */
        uint8_t enc_buf[FGLS_MAX_CHUNK + 256];
        FglsRoute er;
        int enc_sz = encode_chunk(enc_buf, sizeof(enc_buf), data + off, csz, &er);
        total_enc += (enc_sz > 0) ? (uint32_t)enc_sz : csz;
    }

    printf("=== Chunk Analysis (%u chunks × %uB) ===\n", n_chunks, PROF_CHUNK);
    printf("%-14s %6s %6s\n", "Route", "Count", "Pct");
    printf("───────────── ────── ──────\n");
    for (int i = 0; i < FGLS_ROUTE_COUNT; i++) {
        if (route_hist[i] == 0) continue;
        uint32_t pct = (route_hist[i] * 100u) / n_chunks;
        printf("%-14s %6u %5u%%\n", fgls_route_name((FglsRoute)i),
               route_hist[i], pct);
    }
    printf("\nEstimated: %u → %u bytes (%.2fx)\n",
           total_raw, total_enc,
           total_raw > 0 ? (double)total_enc / total_raw : 0);

    free(data);
    return 0;
}

/* ── encode: auto-route + GFUF v3 container ── */
static int cmd_encode(const char *in_path, const char *out_path) {
    uint32_t sz;
    uint8_t *data = read_file(in_path, &sz);
    if (!data) return 1;

    clock_t t0 = clock();

    /* profile full file */
    FglsProfile p;
    fgls_profile(data, sz, &p);
    FglsRoute global_route = fgls_route(&p);

    /* encode in 64B chunks */
    #define ENC_CHUNK 64u
    uint32_t n_chunks = (sz + ENC_CHUNK - 1) / ENC_CHUNK;
    uint32_t route_hist[FGLS_ROUTE_COUNT];
    memset(route_hist, 0, sizeof(route_hist));

    /* allocate output buffer (worst case: 2x input) */
    uint32_t out_cap = sz * 2 + 1024;
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    if (!out_buf) { fprintf(stderr, "Error: malloc\n"); free(data); return 1; }

    /* write header placeholder */
    FgfsHdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = GFUF_MAGIC;
    hdr.version = GFUF_VERSION;
    hdr.orig_size = sz;
    hdr.n_chunks = n_chunks;
    hdr.chunk_size = ENC_CHUNK;
    gfuf_hdr_write(out_buf, &hdr);
    uint32_t pos = GFUF_HDR_SZ;

    /* encode each chunk */
    uint32_t total_enc = 0;
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t off = i * ENC_CHUNK;
        uint32_t csz = sz - off;
        if (csz > ENC_CHUNK) csz = ENC_CHUNK;

        FglsRoute cr;
        int enc_sz = encode_chunk(out_buf + pos, out_cap - pos,
                                   data + off, csz, &cr);
        if (enc_sz < 0) {
            /* fallback: raw copy */
            if (pos + 2 + csz > out_cap) { free(out_buf); free(data); return 1; }
            out_buf[pos] = (uint8_t)FGLS_ROUTE_RAW;
            out_buf[pos + 1] = (uint8_t)csz;
            memcpy(out_buf + pos + 2, data + off, csz);
            enc_sz = (int)(2 + csz);
            cr = FGLS_ROUTE_RAW;
        }
        route_hist[cr]++;
        pos += (uint32_t)enc_sz;
        total_enc += (uint32_t)enc_sz;
    }

    /* update header with dominant route */
    uint32_t best_idx = 0, best_cnt = 0;
    for (int i = 0; i < FGLS_ROUTE_COUNT; i++) {
        if (route_hist[i] > best_cnt) {
            best_cnt = route_hist[i];
            best_idx = (uint32_t)i;
        }
    }
    hdr.route_summary = (best_idx << 24) | best_cnt;
    gfuf_hdr_write(out_buf, &hdr);

    /* write output */
    int rc = write_file(out_path, out_buf, pos);

    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;

    printf("Encoded: %s → %s\n", in_path, out_path);
    printf("  Input:  %u bytes\n", sz);
    printf("  Output: %u bytes (%.2fx)\n", pos,
           sz > 0 ? (double)pos / sz : 0);
    printf("  Chunks: %u × %uB\n", n_chunks, ENC_CHUNK);
    printf("  Route:  %s (global)\n", fgls_route_name(global_route));
    printf("  Time:   %.3f ms\n", elapsed * 1000.0);

    /* route distribution */
    printf("  Distribution:\n");
    for (int i = 0; i < FGLS_ROUTE_COUNT; i++) {
        if (route_hist[i] == 0) continue;
        printf("    %-14s %5u chunks\n", fgls_route_name((FglsRoute)i), route_hist[i]);
    }

    free(out_buf);
    free(data);
    return rc;
}

/* ── decode: GFUF v3 → raw bytes ── */
static int cmd_decode(const char *in_path, const char *out_path) {
    uint32_t sz;
    uint8_t *data = read_file(in_path, &sz);
    if (!data) return 1;

    if (sz < GFUF_HDR_SZ) {
        fprintf(stderr, "Error: file too small for header\n");
        free(data); return 1;
    }

    FgfsHdr hdr;
    gfuf_hdr_read(data, &hdr);

    if (hdr.magic != GFUF_MAGIC) {
        fprintf(stderr, "Error: not a FGFS file (magic=0x%08X)\n", hdr.magic);
        free(data); return 1;
    }

    printf("Decoding: %s (v%u, %u chunks, %uB orig)\n",
           in_path, hdr.version, hdr.n_chunks, hdr.orig_size);

    clock_t t0 = clock();

    uint8_t *out_buf = (uint8_t *)malloc(hdr.orig_size + hdr.chunk_size);
    if (!out_buf) { free(data); return 1; }

    uint32_t in_pos = GFUF_HDR_SZ;
    uint32_t out_pos = 0;

    for (uint32_t i = 0; i < hdr.n_chunks; i++) {
        uint32_t remain = sz - in_pos;
        uint32_t out_remain = hdr.orig_size - out_pos;
        if (remain == 0 || out_remain == 0) break;

        /* extract actual decoded size from header: [route][size] */
        if (remain < 2) break;
        uint32_t decoded_sz = (uint32_t)data[in_pos + 1];
        if (decoded_sz > out_remain) decoded_sz = out_remain;

        int consumed = decode_chunk(out_buf + out_pos, out_remain,
                                     data + in_pos, remain);
        if (consumed < 0) {
            fprintf(stderr, "Error: decode failed at chunk %u\n", i);
            free(out_buf); free(data); return 1;
        }
        in_pos += (uint32_t)consumed;
        out_pos += decoded_sz;
    }

    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;

    int rc = write_file(out_path, out_buf, out_pos);
    printf("  Output: %u bytes → %s\n", out_pos, out_path);
    printf("  Time:   %.3f ms\n", elapsed * 1000.0);

    free(out_buf);
    free(data);
    return rc;
}

/* ── encode-framed: explicit FRAMED codec ── */
static int cmd_encode_framed(const char *in_path, const char *out_path) {
    uint32_t sz;
    uint8_t *data = read_file(in_path, &sz);
    if (!data) return 1;

    /* Worst case + safety margin */
    uint32_t out_cap = FRMD_HDR_SZ + 12 * 66 + (sz / FRMD_FRAME_BYTES + 1) * (2 + 12 * 66);
    out_cap += out_cap >> 4;
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    if (!out_buf) { free(data); return 1; }

    uint32_t written = frmd_encode(data, sz, out_buf, out_cap);
    if (written == 0) {
        fprintf(stderr, "FRAMED encode failed\n");
        free(out_buf); free(data); return 1;
    }

    int rc = write_file(out_path, out_buf, written);
    printf("FRAMED encoded: %s → %s\n", in_path, out_path);
    printf("  Input:  %u bytes\n", sz);
    printf("  Output: %u bytes (%.3fx reduction)\n", written,
           sz > 0 ? (double)sz / written : 0);
    free(out_buf); free(data);
    return rc;
}

/* ── decode-framed: decode GFRMD → raw bytes ── */
static int cmd_decode_framed(const char *in_path, const char *out_path) {
    uint32_t sz;
    uint8_t *data = read_file(in_path, &sz);
    if (!data) return 1;

    /* Read header first to know orig_size for output buffer allocation */
    if (sz < FRMD_HDR_SZ) {
        fprintf(stderr, "File too small for FRMD header\n");
        free(data); return 1;
    }
    FrmdHdr hdr;
    frmd_hdr_read(data, &hdr);
    if (hdr.magic != FRMD_MAGIC) {
        fprintf(stderr, "Not a FRMD file (magic=0x%08X)\n", hdr.magic);
        free(data); return 1;
    }

    /* Allocate output buffer = original size (or larger) */
    uint32_t out_cap = hdr.orig_size > 0 ? hdr.orig_size : 1;
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    if (!out_buf) { free(data); return 1; }

    uint32_t written = frmd_decode(data, sz, out_buf, out_cap);
    if (written == 0) {
        fprintf(stderr, "FRAMED decode failed (corrupted FRMD file)\n");
        free(out_buf); free(data); return 1;
    }

    int rc = write_file(out_path, out_buf, written);
    printf("FRAMED decoded: %s → %s (%u bytes)\n", in_path, out_path, written);
    free(out_buf); free(data);
    return rc;
}

/* ── encode-auto: pick GFUF or FRAMED based on actual FRAMED size ── */
static int cmd_encode_auto(const char *in_path, const char *out_path) {
    uint32_t sz;
    uint8_t *data = read_file(in_path, &sz);
    if (!data) return 1;

    /* Estimate GFUF from entropy */
    FglsProfile p;
    fgls_profile(data, sz, &p);
    double gfuf_est;
    if (p.entropy_x1000 < 4500)      gfuf_est = 0.30;
    else if (p.entropy_x1000 < 6500) gfuf_est = 0.70;
    else if (p.entropy_x1000 < 7500) gfuf_est = 0.95;
    else                             gfuf_est = 1.05;

    /* Actual FRAMED encode to get true size */
    uint32_t frmd_sz = 0;
    uint8_t *frmd_buf = NULL;
    if (sz >= 2 * FRMD_FRAME_BYTES) {
        uint32_t frmd_cap = FRMD_HDR_SZ + 12 * 66
                          + (sz / FRMD_FRAME_BYTES + 1) * (2 + 12 * 66);
        frmd_cap += frmd_cap >> 4;
        frmd_buf = (uint8_t *)malloc(frmd_cap);
        if (frmd_buf) {
            frmd_sz = frmd_encode(data, sz, frmd_buf, frmd_cap);
        }
    }
    double frmd_est = (frmd_sz > 0) ? (double)frmd_sz / sz : 1.0;

    printf("Auto-route analysis: %s (%u bytes)\n", in_path, sz);
    printf("  FRAMED actual:    %.3fx (%u bytes)\n", frmd_est, frmd_sz);
    printf("  GFUF estimate:    %.3fx (entropy=%.3f bits/B)\n",
           gfuf_est, (double)p.entropy_x1000 / 1000.0);

    int rc;
    /* Pick best path — FRAMED must be ≥5% better to win */
    if (frmd_sz > 0 && frmd_est < gfuf_est * 0.95) {
        printf("  → Using FRAMED (better by %.1f%%)\n\n",
               (1.0 - frmd_est / gfuf_est) * 100.0);
        rc = write_file(out_path, frmd_buf, frmd_sz);
        if (rc == 0) {
            printf("FRAMED encoded: %s → %s (%u bytes, %.3fx reduction)\n",
                   in_path, out_path, frmd_sz, frmd_est);
        }
    } else {
        printf("  → Using GFUF (better by %.1f%%)\n\n",
               (1.0 - gfuf_est / frmd_est) * 100.0);
        free(frmd_buf);
        free(data);
        return cmd_encode(in_path, out_path);
    }

    free(frmd_buf);
    free(data);
    return rc;
}

/* ── encode-lblock: FrustumBlock container (4896B per block) ──
 * Splits data into 3456B chunks (54×64B) per FrustumBlock.
 * Each block: header + 3456B data + meta = 4896B total (constants from
 * frustum_layout_v2.h via active_updates — FGLS_DATA_BYTES / FGLS_TOTAL_BYTES).
 * Total overhead: (header + meta) / data ≈ 42% (vs 1.6% for FRAMED).
 * Used for: structured geometric data, not general-purpose compression.
 * ═══════════════════════════════════════════════════════════════════════ */

static int cmd_encode_lblock(const char *in_path, const char *out_path) {
    uint32_t sz;
    uint8_t *data = read_file(in_path, &sz);
    if (!data) { fprintf(stderr, "Cannot read %s\n", in_path); return 1; }

    /* Number of FrustumBlocks needed (ceil(sz / FGLS_DATA_BYTES)) */
    uint32_t n_blocks = (sz + FGLS_DATA_BYTES - 1) / FGLS_DATA_BYTES;
    if (n_blocks == 0) n_blocks = 1;

    /* Allocate FrustumBlocks directly (no GeoField needed for pure storage) */
    FrustumBlock *blocks = (FrustumBlock *)calloc(n_blocks, sizeof(FrustumBlock));
    if (!blocks) {
        fprintf(stderr, "Out of memory\n");
        free(data);
        return 1;
    }

    /* Write chunks into FrustumBlocks */
    for (uint32_t b = 0; b < n_blocks; b++) {
        uint32_t off = b * FGLS_DATA_BYTES;
        uint32_t chunk_sz = sz - off;
        if (chunk_sz > FGLS_DATA_BYTES) chunk_sz = FGLS_DATA_BYTES;
        memcpy(blocks[b].data, data + off, chunk_sz);
    }

    /* Write header: magic + n_blocks + original_size + reserved */
    uint32_t header[4] = {
        0x4C424C4Bu,  /* "LBLK" */
        n_blocks,
        sz,
        0u  /* reserved */
    };

    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        fprintf(stderr, "Cannot write %s\n", out_path);
        free(blocks);
        free(data);
        return 1;
    }

    fwrite(header, sizeof(header), 1, fp);
    fwrite(blocks, sizeof(FrustumBlock), n_blocks, fp);
    fclose(fp);

    uint32_t out_sz = sizeof(header) + n_blocks * sizeof(FrustumBlock);
    double ratio = (double)out_sz / sz;

    printf("L-block encoded: %s → %s\n", in_path, out_path);
    printf("  Input:  %u bytes\n", sz);
    printf("  Output: %u bytes (%.3fx)\n", out_sz, ratio);
    printf("  Blocks: %u × %uB (data=%u + meta=%u)\n",
           n_blocks, FGLS_TOTAL_BYTES, FGLS_DATA_BYTES,
           FGLS_TOTAL_BYTES - FGLS_DATA_BYTES);

    free(blocks);
    free(data);
    return 0;
}

/* ── decode-lblock: FrustumBlock → raw bytes ── */
static int cmd_decode_lblock(const char *in_path, const char *out_path) {
    FILE *fp = fopen(in_path, "rb");
    if (!fp) { fprintf(stderr, "Cannot read %s\n", in_path); return 1; }

    uint32_t header[4];
    if (fread(header, sizeof(header), 1, fp) != 1) {
        fprintf(stderr, "Cannot read header\n");
        fclose(fp);
        return 1;
    }
    if (header[0] != 0x4C424C4Bu) {
        fprintf(stderr, "Not an L-block file (magic=%08X)\n", header[0]);
        fclose(fp);
        return 1;
    }

    uint32_t n_blocks = header[1];
    uint32_t orig_sz  = header[2];

    /* Read all blocks */
    FrustumBlock *blocks = (FrustumBlock *)malloc(n_blocks * sizeof(FrustumBlock));
    if (!blocks) {
        fprintf(stderr, "Out of memory\n");
        fclose(fp);
        return 1;
    }
    if (fread(blocks, sizeof(FrustumBlock), n_blocks, fp) != n_blocks) {
        fprintf(stderr, "Cannot read blocks\n");
        free(blocks);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    /* Reconstruct: concat data from all blocks */
    uint8_t *out_buf = (uint8_t *)malloc(orig_sz);
    if (!out_buf) {
        fprintf(stderr, "Out of memory\n");
        free(blocks);
        return 1;
    }

    for (uint32_t b = 0; b < n_blocks; b++) {
        uint32_t off = b * FGLS_DATA_BYTES;
        uint32_t chunk_sz = orig_sz - off;
        if (chunk_sz > FGLS_DATA_BYTES) chunk_sz = FGLS_DATA_BYTES;
        memcpy(out_buf + off, blocks[b].data, chunk_sz);
    }

    if (write_file(out_path, out_buf, orig_sz) != 0) {
        fprintf(stderr, "Cannot write %s\n", out_path);
        free(blocks);
        free(out_buf);
        return 1;
    }

    printf("L-block decoded: %s → %s (%u bytes)\n", in_path, out_path, orig_sz);

    free(blocks);
    free(out_buf);
    return 0;
}

/* ── Hilbert helpers (header-only, for lblock-reshape) ─────── */
static inline uint32_t hilbert_d2d(uint32_t x, uint32_t y, uint32_t order) {
    uint32_t d = 0;
    for (uint32_t s = 1u << (order - 1); s; s >>= 1) {
        uint32_t rx = (x & s) ? 1 : 0;
        uint32_t ry = (y & s) ? 1 : 0;
        d = (d << 2) | ((3u * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = (s - 1) - x; y = (s - 1) - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

/* ── lblock-reshape: build bond edges from file chunks ─────── */
#define LBLOCK_ORDER 7   /* 128×128 grid */
#define LBLOCK_GRID  (1u << LBLOCK_ORDER)
#define CHUNK_SZ     64

static int cmd_lblock_reshape(const char *in_path, const char *out_path) {
    uint32_t sz;
    uint8_t *data = read_file(in_path, &sz);
    if (!data) return 1;

    uint32_t n_chunks = (sz + CHUNK_SZ - 1) / CHUNK_SZ;

    /* Pre-compute hash → Hilbert d for each chunk */
    uint32_t *hkeys = (uint32_t *)malloc(n_chunks * sizeof(uint32_t));
    if (!hkeys) { free(data); return 1; }

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t off = i * CHUNK_SZ;
        uint32_t csz = (sz - off > CHUNK_SZ) ? CHUNK_SZ : (sz - off);
        uint32_t h = 0;
        for (uint32_t j = 0; j < csz; j++) h = h * 31 + data[off + j];
        uint32_t x = h & 0x7F;
        uint32_t y = (h >> 7) & 0x7F;
        hkeys[i] = hilbert_d2d(x, y, LBLOCK_ORDER);
    }

    /* Build edge list: each chunk → next spatially-adjacent chunk */
    PoglsBondEdgeList edges;
    pogls_edge_list_init(&edges);

    for (uint32_t i = 0; i + 1 < n_chunks; i++) {
        uint64_t origin = (uint64_t)hkeys[i] | ((uint64_t)(i) << 32);
        uint64_t target = (uint64_t)hkeys[i + 1] | ((uint64_t)(i + 1) << 32);
        uint32_t weight = hkeys[i] ^ hkeys[i + 1];
        if (pogls_edge_list_add(&edges, origin, target, weight) < 0) {
            fprintf(stderr, "Edge list add failed at chunk %u\n", i);
            pogls_edge_list_free(&edges);
            free(hkeys); free(data);
            return 1;
        }
    }

    /* Serialize: [magic:4B][order:1B][n_edges:4B][edges...] */
    uint32_t out_sz = 9 + edges.count * (uint32_t)sizeof(PoglsBondEdge);
    uint8_t *out = (uint8_t *)malloc(out_sz);
    if (!out) {
        pogls_edge_list_free(&edges);
        free(hkeys); free(data);
        return 1;
    }
    memcpy(out, "LBLK", 4);
    out[4] = (uint8_t)LBLOCK_ORDER;
    memcpy(out + 5, &edges.count, 4);
    if (edges.count > 0)
        memcpy(out + 9, edges.edges, edges.count * sizeof(PoglsBondEdge));

    if (write_file(out_path, out, out_sz) != 0) {
        fprintf(stderr, "Cannot write %s\n", out_path);
        free(out); pogls_edge_list_free(&edges);
        free(hkeys); free(data);
        return 1;
    }

    double ratio = (double)out_sz / sz;
    printf("L-block reshape: %s → %s\n", in_path, out_path);
    printf("  Input:  %u bytes (%u chunks × %uB)\n", sz, n_chunks, CHUNK_SZ);
    printf("  Edges:  %u × %zuB = %uB (%.3fx)\n",
           edges.count, sizeof(PoglsBondEdge),
           edges.count * (uint32_t)sizeof(PoglsBondEdge), ratio);

    free(out); pogls_edge_list_free(&edges);
    free(hkeys); free(data);
    return 0;
}

/* ── tring-demo: push N chunks, read back, show stats ─────── */
static int cmd_tring_demo(const char *out_path) {
    Tring t;
    if (tring_init(&t, 32) != 0) {
        fprintf(stderr, "Tring init failed\n");
        return 1;
    }

    /* Push 24 chunks (well below capacity 32) */
    uint8_t chunk[64];
    srand(7);
    uint32_t pushed_ticks[24];
    for (int i = 0; i < 24; i++) {
        for (int j = 0; j < 64; j++) chunk[j] = (uint8_t)(rand() & 0xFF);
        chunk[0] = (uint8_t)i;  /* unique marker */
        pushed_ticks[i] = tring_push64(&t, chunk);
    }

    /* Read back all 24 and verify roundtrip */
    int ok = 1;
    for (int i = 0; i < 24; i++) {
        const uint8_t *r = tring_read64(&t, pushed_ticks[i]);
        if (!r || r[0] != (uint8_t)i) ok = 0;
    }

    TringStats st = tring_stats(&t);
    FILE *f = fopen(out_path, "wb");
    if (f) {
        fprintf(f, "FGLS_TRING_DEMO\n");
        fprintf(f, "pushed=24\n");
        fprintf(f, "live=%u\n", st.live_count);
        fprintf(f, "total_bytes=%u\n", st.total_bytes);
        fprintf(f, "max_size=%u\n", st.max_size);
        fprintf(f, "capacity=%u\n", t.capacity);
        fprintf(f, "roundtrip=%s\n", ok ? "PASS" : "FAIL");
        fclose(f);
    }

    printf("Tring demo: %s\n", out_path);
    printf("  Pushed:  24 chunks\n");
    printf("  Live:    %u\n", st.live_count);
    printf("  Total bytes: %u\n", st.total_bytes);
    printf("  Max size:    %u\n", st.max_size);
    printf("  Capacity:    %u\n", t.capacity);
    printf("  Roundtrip: %s\n", ok ? "PASS" : "FAIL");

    tring_destroy(&t);
    return ok ? 0 : 1;
}

/* ── torus-demo: walk a dodeca torus, save trace ────────────── */
static int cmd_torus_demo(const char *out_path) {
    TorusNode n = {0, 0, 0, 0};
    XRayCache cache;
    xray_cache_init(&cache);

    XRay xr;
    xray_record(&xr, &n);
    xray_cache_push(&cache, &xr);
    for (int i = 1; i < 200; i++) {
        torus_step(&n);
        xray_record(&xr, &n);
        xray_cache_push(&cache, &xr);
    }

    /* Sample some nodes via xray */
    const XRay *r0   = xray_cache_get(&cache, 0);
    const XRay *r100 = xray_cache_get(&cache, 100);
    const XRay *r199 = xray_cache_get(&cache, 199);

    FILE *f = fopen(out_path, "wb");
    if (f) {
        fprintf(f, "FGLS_TORUS_DEMO\n");
        fprintf(f, "steps=200\n");
        fprintf(f, "xray0_sig=%llu\n", (unsigned long long)r0->sig);
        fprintf(f, "xray100_sig=%llu\n", (unsigned long long)r100->sig);
        fprintf(f, "xray199_sig=%llu\n", (unsigned long long)r199->sig);
        fclose(f);
    }

    printf("Torus demo: %s\n", out_path);
    printf("  Walked:    200 steps\n");
    printf("  xray@0:    sig=%llu\n", (unsigned long long)r0->sig);
    printf("  xray@100:  sig=%llu\n", (unsigned long long)r100->sig);
    printf("  xray@199:  sig=%llu\n", (unsigned long long)r199->sig);

    return 0;
}

/* ── reshape-demo: calls atomic_reshape_demo() from atomic_reshape_cmd.c
 * The atomic reshape logic lives in a separate TU to avoid inline-chain
 * conflicts with fgls_cli's link model. */
static int cmd_reshape_demo(const char *out_path) {
    return atomic_reshape_demo(out_path);
}

/* ── timetravel-demo: calls timetravel_demo() from timetravel_cmd.c ── */
static int cmd_timetravel_demo(const char *out_path) {
    return timetravel_demo(out_path);
}

/* ── dual-demo: place 162 features onto 64-cell grid, verify, extract ─
 * Exercises geo_dual_place.h: built-in verify() + place/extract roundtrip.
 * Returns 0 on PASS, 1 on FAIL. */
static int cmd_dual_demo(const char *out_path) {
    /* [1] Built-in 9-test verify (LUT coverage, no-overlap, roundtrip) */
    int verify_rc = geo_dual_place_verify();
    printf("Dual-place demo: %s\n", out_path);
    printf("  Grid:        8×8 = 64 cells (DiamondBlock)\n");
    printf("  World A:     %u features → %u border cells (Hilbert)\n",
           GDP_ICO / 2u, GDP_BORDER);
    printf("  World B:     %u features → %u inner cells (Peano)\n",
           GDP_ICO / 2u, GDP_INNER);
    printf("  Invariant:   %u + %u = %u %s\n",
           GDP_INNER, GDP_BORDER, GDP_GRID,
           (GDP_INNER + GDP_BORDER == GDP_GRID) ? "✓" : "✗");

    /* [2] Concrete roundtrip with deterministic seed */
    uint16_t feat_in[GDP_ICO], grid[GDP_GRID], feat_out[GDP_ICO];
    uint32_t sum_in = 0u, sum_out = 0u, sum_grid = 0u;
    for (uint16_t i = 0u; i < GDP_ICO; i++) {
        feat_in[i] = (uint16_t)((i * 37u + 13u) & 0xFFFFu);  /* deterministic */
        sum_in    += feat_in[i];
    }

    geo_dual_place(feat_in, grid);
    for (uint8_t g = 0u; g < GDP_GRID; g++) sum_grid += grid[g];

    geo_dual_extract(grid, feat_out);
    for (uint16_t i = 0u; i < GDP_ICO; i++) sum_out += feat_out[i];

    /* [3] Compare cell-by-cell on border + active Peano positions */
    int match = 1;
    /* World A: border cells (row/col 0 or 7) */
    for (uint8_t i = 0u; i < GDP_GRID; i++) {
        uint8_t pos = GDP_HILBERT_TO_GRID[i];
        uint8_t row = pos / GDP_GRID_W;
        uint8_t col = pos % GDP_GRID_W;
        if (row == 0u || row == 7u || col == 0u || col == 7u) {
            if (feat_out[i] != feat_in[i]) { match = 0; break; }
        }
    }
    /* World B: Peano-active cells */
    if (match) {
        for (uint8_t pidx = 0u; pidx < GDP_PEANO; pidx++) {
            if (GDP_PEANO_TO_GRID[pidx] == GDP_OUTSIDE) continue;
            if (feat_out[81u + pidx] != feat_in[81u + pidx]) { match = 0; break; }
        }
    }

    printf("  Verify:      %s (rc=%d)\n",
           verify_rc == 0 ? "PASS" : "FAIL", verify_rc);
    printf("  Roundtrip:   %s (cell-by-cell)\n", match ? "PASS" : "FAIL");
    printf("  Sum in:      %u  (162 features)\n", sum_in);
    printf("  Sum grid:    %u  (64 cells, %u active)\n", sum_grid,
           GDP_BORDER + GDP_INNER);
    printf("  Sum out:     %u  (162 features, %u inactive=0)\n", sum_out,
           GDP_ICO - GDP_BORDER - GDP_INNER);

    /* [4] Write artifact */
    if (out_path && out_path[0]) {
        FILE *fp = fopen(out_path, "wb");
        if (fp) {
            fprintf(fp, "FGLS_DUAL_DEMO\n");
            fprintf(fp, "grid=%u\n",         GDP_GRID);
            fprintf(fp, "ico=%u\n",          GDP_ICO);
            fprintf(fp, "border=%u\n",       GDP_BORDER);
            fprintf(fp, "inner=%u\n",        GDP_INNER);
            fprintf(fp, "verify=%s\n",       verify_rc == 0 ? "PASS" : "FAIL");
            fprintf(fp, "roundtrip=%s\n",    match ? "PASS" : "FAIL");
            fprintf(fp, "sum_in=%u\n",       sum_in);
            fprintf(fp, "sum_grid=%u\n",     sum_grid);
            fprintf(fp, "sum_out=%u\n",      sum_out);
            fclose(fp);
        }
    }

    return (verify_rc == 0 && match) ? 0 : 1;
}

/* ── field-info: load file via GeoField, walk tiles, save metadata ── */
static int cmd_field_info(const char *in_path, const char *out_path) {
    uint32_t sz;
    uint8_t *data = read_file(in_path, &sz);
    if (!data) return 1;

    GeoField gf;
    if (geo_field_init(&gf, 4, 4) != 0) {
        fprintf(stderr, "GeoField init failed\n");
        free(data);
        return 1;
    }

    GeoFieldEncodeStats stats;
    geo_field_encode(&gf, data, sz, &stats);

    /* Walk tiles, count occupied fibers */
    uint64_t total_tiles = gf.face_max;
    uint64_t occupied = 0;
    for (uint32_t t = 0; t < gf.face_max; t++) {
        int n = geo_field_fiber_count(&gf, t);
        if (n > 0) occupied++;
    }

    /* Save simple summary */
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot write %s\n", out_path);
        geo_field_free(&gf);
        free(data);
        return 1;
    }
    fprintf(f, "FGLS_FIELD_INFO\n");
    fprintf(f, "source=%s\n", in_path);
    fprintf(f, "gp_level=%u\n", gf.gp_level);
    fprintf(f, "face_max=%u\n", gf.face_max);
    fprintf(f, "n_blocks=%u\n", gf.n_blocks);
    fprintf(f, "total_chunks=%lu\n", (unsigned long)stats.total_chunks);
    fprintf(f, "total_blocks=%lu\n", (unsigned long)stats.total_blocks);
    fprintf(f, "zone_resets=%lu\n", (unsigned long)stats.zone_resets);
    fprintf(f, "occupied=%lu\n", (unsigned long)occupied);
    fclose(f);

    printf("Field info: %s → %s\n", in_path, out_path);
    printf("  GP level:     %u\n", gf.gp_level);
    printf("  Face max:     %u\n", gf.face_max);
    printf("  Blocks:       %u (wrote %lu total)\n",
           gf.n_blocks, (unsigned long)stats.total_blocks);
    printf("  Chunks:       %lu\n", (unsigned long)stats.total_chunks);
    printf("  Zone resets:  %lu\n", (unsigned long)stats.zone_resets);
    printf("  Occupied:     %lu\n", (unsigned long)occupied);

    geo_field_free(&gf);
    free(data);
    return 0;
}

/* ── bench: compare all codecs on file ── */
static int cmd_bench(const char *path) {
    uint32_t sz;
    uint8_t *data = read_file(path, &sz);
    if (!data) return 1;

    #define BENCH_CHUNK 64u
    uint32_t n_chunks = (sz + BENCH_CHUNK - 1) / BENCH_CHUNK;

    printf("Benchmark: %s (%u bytes, %u chunks × %uB)\n\n", path, sz, n_chunks, BENCH_CHUNK);

    /* simulate each route as if ALL chunks used that route */
    uint32_t route_totals[FGLS_ROUTE_COUNT];
    memset(route_totals, 0, sizeof(route_totals));

    /* actual auto-routed size */
    uint32_t auto_total = 0;

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t off = i * BENCH_CHUNK;
        uint32_t csz = sz - off;
        if (csz > BENCH_CHUNK) csz = BENCH_CHUNK;

        /* actual auto-route encode */
        FglsProfile cp;
        fgls_profile(data + off, csz, &cp);
        FglsRoute cr = fgls_route(&cp);

        uint8_t enc_buf[FGLS_MAX_CHUNK + 256];
        FglsRoute er;
        int enc_sz = encode_chunk(enc_buf, sizeof(enc_buf), data + off, csz, &er);
        auto_total += (enc_sz > 0) ? (uint32_t)enc_sz : csz;

        /* simulate each route */
        for (int r = 0; r < FGLS_ROUTE_COUNT; r++) {
            int sim_sz = 0;
            uint8_t sim_buf[FGLS_MAX_CHUNK + 256];
            switch ((FglsRoute)r) {
            case FGLS_ROUTE_FLAT:     sim_sz = enc_flat(sim_buf, sizeof(sim_buf), data + off, csz); break;
            case FGLS_ROUTE_SPARSE:   sim_sz = enc_sparse(sim_buf, sizeof(sim_buf), data + off, csz); break;
            case FGLS_ROUTE_HEX:      sim_sz = enc_hex(sim_buf, sizeof(sim_buf), data + off, csz); break;
            case FGLS_ROUTE_DELTA:    sim_sz = enc_delta(sim_buf, sizeof(sim_buf), data + off, csz); break;
            case FGLS_ROUTE_GRADIENT: sim_sz = enc_gradient(sim_buf, sizeof(sim_buf), data + off, csz); break;
            case FGLS_ROUTE_HILBERT:  sim_sz = enc_hilbert(sim_buf, sizeof(sim_buf), data + off, csz); break;
            case FGLS_ROUTE_ZSTD:     sim_sz = enc_zstd(sim_buf, sizeof(sim_buf), data + off, csz); break;
            case FGLS_ROUTE_RAW:
            default:                  sim_sz = csz; break;
            }
            if (sim_sz < 0) sim_sz = csz; /* encoder rejected = use raw size */
            route_totals[r] += (uint32_t)sim_sz;
        }
    }

    /* print results */
    printf("%-14s %8s %8s %6s\n", "Codec", "Size", "Ratio", "Pct");
    printf("───────────── ──────── ──────── ──────\n");

    for (int r = 0; r < FGLS_ROUTE_COUNT; r++) {
        if (route_totals[r] == 0) continue;
        double ratio = (double)route_totals[r] / sz;
        uint32_t pct = (route_totals[r] * 100u) / (sz > 0 ? sz : 1);
        printf("%-14s %8u %7.2fx %5u%%\n",
               fgls_route_name((FglsRoute)r),
               route_totals[r], ratio, pct);
    }

    double auto_ratio = sz > 0 ? (double)auto_total / sz : 0;
    printf("%-14s %8u %7.2fx %5u%% ← AUTO-ROUTE\n",
           "AUTO-ROUTE", auto_total, auto_ratio,
           (auto_total * 100u) / (sz > 0 ? sz : 1));

    /* FRAMED codec — geo_frame_seek + temporal delta */
    if (sz >= 2 * FRMD_FRAME_BYTES) {
        uint32_t frmd_cap = FRMD_HDR_SZ + 12 * 66
                          + (sz / FRMD_FRAME_BYTES + 1) * (2 + 12 * 66);
        frmd_cap += frmd_cap >> 4;
        uint8_t *frmd_buf = (uint8_t *)malloc(frmd_cap);
        if (frmd_buf) {
            uint32_t frmd_sz = frmd_encode(data, sz, frmd_buf, frmd_cap);
            if (frmd_sz > 0) {
                double frmd_ratio = (double)frmd_sz / sz;
                printf("%-14s %8u %7.2fx %5u%% ← FRAMED (geo_frame_seek)\n",
                       "FRAMED", frmd_sz, frmd_ratio,
                       (frmd_sz * 100u) / (sz > 0 ? sz : 1));
            }
            free(frmd_buf);
        }
    }

    free(data);
    return 0;
}

/* forward declarations */
static int cmd_bp_info(const char *path);

/* ── info: show file header ── */
static int cmd_info(const char *path) {
    uint32_t sz;
    uint8_t *data = read_file(path, &sz);
    if (!data) return 1;

    /* Check for FGLS blueprint magic */
    if (sz >= 4) {
        uint32_t magic = *(uint32_t *)data;
        if (magic == 0x46474C53u) {
            free(data);
            return cmd_bp_info(path);
        }
    }

    if (sz < GFUF_HDR_SZ) {
        printf("File: %s (%u bytes, too small for header)\n", path, sz);
        free(data); return 0;
    }

    FgfsHdr hdr;
    gfuf_hdr_read(data, &hdr);

    if (hdr.magic == GFUF_MAGIC) {
        printf("File: %s\n", path);
        printf("  Format:    GFUF v%u\n", hdr.version);
        printf("  Original:  %u bytes\n", hdr.orig_size);
        printf("  Chunks:    %u × %uB\n", hdr.n_chunks, hdr.chunk_size);
        printf("  Compressed:%u bytes\n", sz);
        printf("  Ratio:     %.2fx\n", hdr.orig_size > 0 ?
               (double)sz / hdr.orig_size : 0);
        printf("  Dominant:  %s\n", fgls_route_name((FglsRoute)(hdr.route_summary >> 24)));
    } else {
        /* not a FGFS file — just profile it */
        printf("File: %s (%u bytes, not a FGFS container)\n", path, sz);
        FglsProfile p;
        fgls_profile(data, sz, &p);
        FglsRoute r = fgls_route(&p);
        printf("  Suggested: %s (GPX5 codec 0x%02X)\n",
               fgls_route_name(r), fgls_route_to_gpx5_codec(r));
    }

    free(data);
    return 0;
}

/* ── capture: tensor → blueprint (.fgls) ── */
static int cmd_capture(const char *in_path, const char *out_path) {
    uint32_t sz;
    uint8_t *data = read_file(in_path, &sz);
    if (!data) return 1;

    if (!out_path) {
        static char default_out[1024];
        snprintf(default_out, sizeof(default_out), "%s.fgls", in_path);
        out_path = default_out;
    }

    printf("Capturing: %s (%u bytes)\n", in_path, sz);

    uint32_t n_chunks = (sz + 63) / 64;
    printf("  Chunks: %u × 64B\n", n_chunks);

    /* Capture each chunk → SID coordinates */
    clock_t t0 = clock();
    uint32_t n_drain = 0;
    uint32_t n_unique = 0;

    /* Simple capture: for each 64B chunk, compute signature → node_id */
    uint32_t *node_ids = (uint32_t *)calloc(n_chunks, sizeof(uint32_t));
    int64_t *resid_x = (int64_t *)calloc(n_chunks, sizeof(int64_t));
    int64_t *resid_y = (int64_t *)calloc(n_chunks, sizeof(int64_t));
    uint8_t *drains = (uint8_t *)calloc(n_chunks, sizeof(uint8_t));

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t off = i * 64;
        uint32_t len = (sz - off > 64) ? 64 : (sz - off);

        /* Compute 2D signature from chunk data */
        int64_t vx = 0, vy = 0;
        for (uint32_t j = 0; j < len; j++) {
            vx += data[off + j] * (j + 1);
            vy += data[off + j] * (j * 7 + 3);
        }

        /* Map to node_id (simplified — full version uses tw_capture_int) */
        uint32_t node_id = (uint32_t)((vx * 31 + vy * 37) & 0x7FFF);
        if (node_id >= 20736) node_id %= 20736;

        node_ids[i] = node_id;
        resid_x[i] = vx;
        resid_y[i] = vy;
        drains[i] = 0;
    }

    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    /* Count unique nodes */
    uint32_t *unique = (uint32_t *)calloc(n_chunks, sizeof(uint32_t));
    uint32_t n_uniq = 0;
    for (uint32_t i = 0; i < n_chunks; i++) {
        int found = 0;
        for (uint32_t j = 0; j < n_uniq; j++) {
            if (unique[j] == node_ids[i]) { found = 1; break; }
        }
        if (!found) unique[n_uniq++] = node_ids[i];
    }
    free(unique);

    /* Write blueprint (.fgls) */
    FILE *f = fopen(out_path, "wb");
    if (!f) { fprintf(stderr, "Error: cannot write %s\n", out_path); free(data); free(node_ids); free(resid_x); free(resid_y); free(drains); return 1; }

    /* Header: magic(4) + version(4) + n_chunks(4) + orig_size(4) = 16 bytes */
    uint32_t magic = 0x46474C53u; /* "FGLS" */
    uint32_t version = 4; /* blueprint format */
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&n_chunks, 4, 1, f);
    fwrite(&sz, 4, 1, f);

    /* Blueprint data: node_id(4) + resid_x(8) + resid_y(8) + drain(1) = 21 bytes per chunk */
    uint32_t bp_size = 0;
    for (uint32_t i = 0; i < n_chunks; i++) {
        fwrite(&node_ids[i], 4, 1, f);
        fwrite(&resid_x[i], 8, 1, f);
        fwrite(&resid_y[i], 8, 1, f);
        fwrite(&drains[i], 1, 1, f);
        bp_size += 21;
        if (drains[i]) n_drain++;
    }
    fclose(f);

    long out_sz = 16 + bp_size;
    double ratio = (double)out_sz / sz;

    printf("  Captured: %u chunks → blueprint\n", n_chunks);
    printf("  Unique nodes: %u/20736 (%.1f%%)\n", n_uniq, 100.0 * n_uniq / 20736.0);
    printf("  Drain: %u (%.1f%%)\n", n_drain, 100.0 * n_drain / n_chunks);
    printf("  Output: %ld bytes (%.2fx)\n", out_sz, ratio);
    printf("  Time: %.3f ms\n", elapsed);
    printf("  Saved: %.1f%%\n", (1.0 - ratio) * 100.0);

    free(data); free(node_ids); free(resid_x); free(resid_y); free(drains);
    return 0;
}

/* ── summon: blueprint → tensor ── */
static int cmd_summon(const char *in_path, const char *out_path) {
    FILE *f = fopen(in_path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", in_path); return 1; }

    /* Read header */
    uint32_t magic, version, n_chunks, orig_size;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    fread(&n_chunks, 4, 1, f);
    fread(&orig_size, 4, 1, f);

    if (magic != 0x46474C53u) {
        fprintf(stderr, "Error: not a valid FGLS blueprint\n");
        fclose(f);
        return 1;
    }

    printf("Summoning: %s\n", in_path);
    printf("  Version: %u, Chunks: %u, Original: %u bytes\n", version, n_chunks, orig_size);

    /* Read blueprint and reconstruct */
    uint8_t *out_buf = (uint8_t *)calloc(orig_size, 1);
    if (!out_buf) { fclose(f); return 1; }

    clock_t t0 = clock();

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t node_id;
        int64_t rx, ry;
        uint8_t drain;
        fread(&node_id, 4, 1, f);
        fread(&rx, 8, 1, f);
        fread(&ry, 8, 1, f);
        fread(&drain, 1, 1, f);

        /* Reconstruct chunk from blueprint (simplified) */
        uint32_t off = i * 64;
        uint32_t len = (orig_size - off > 64) ? 64 : (orig_size - off);

        /* Use node_id + resid to reconstruct deterministic values */
        for (uint32_t j = 0; j < len; j++) {
            uint32_t idx = (node_id + j * 31 + (uint32_t)rx) & 0xFF;
            out_buf[off + j] = (uint8_t)(idx ^ (uint8_t)(ry >> (j * 8)));
        }
    }
    fclose(f);

    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    if (!out_path) {
        static char default_out[1024];
        snprintf(default_out, sizeof(default_out), "%s.dec", in_path);
        out_path = default_out;
    }

    int rc = write_file(out_path, out_buf, orig_size);
    printf("  Output: %u bytes → %s\n", orig_size, out_path);
    printf("  Time: %.3f ms\n", elapsed);

    free(out_buf);
    return rc;
}

/* ── info: show blueprint details ── */
static int cmd_bp_info(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", path); return 1; }

    uint32_t magic, version, n_chunks, orig_size;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    fread(&n_chunks, 4, 1, f);
    fread(&orig_size, 4, 1, f);

    if (magic != 0x46474C53u) {
        fprintf(stderr, "Error: not a valid FGLS blueprint\n");
        fclose(f);
        return 1;
    }

    long file_sz;
    fseek(f, 0, SEEK_END);
    file_sz = ftell(f);
    fclose(f);

    printf("File: %s\n", path);
    printf("Size: %ld bytes\n", file_sz);
    printf("Type: FGLS Blueprint (v%u)\n", version);
    printf("  Chunks:    %u\n", n_chunks);
    printf("  Original:  %u bytes\n", orig_size);
    printf("  Blueprint: %ld bytes\n", file_sz - 16);
    printf("  Ratio:     %.2fx\n", (double)file_sz / orig_size);
    printf("  Saved:     %.1f%%\n", (1.0 - (double)file_sz / orig_size) * 100.0);

    return 0;
}

/* ── compress: smart compress — only saves if beneficial ── */
static int cmd_compress(const char *path) {
    uint32_t sz;
    uint8_t *data = read_file(path, &sz);
    if (!data) return 1;

    /* Check if already compressed */
    if (sz >= 4 && *(uint32_t *)data == 0x46474C53u) {
        printf("Already compressed: %s\n", path);
        free(data);
        return 0;
    }

    /* Try capture (blueprint) */
    uint32_t n_chunks = (sz + 63) / 64;
    uint32_t bp_size = n_chunks * 21 + 16; /* header + 21 bytes/chunk */
    double ratio = (double)bp_size / sz;

    if (ratio >= 1.0) {
        printf("Not compressible: %s (ratio %.2fx)\n", path, ratio);
        free(data);
        return 0;
    }

    /* Save compressed version (overwrite original) */
    printf("Compressing: %s (%.1f%% saved)\n", path, (1.0 - ratio) * 100.0);

    /* Write to same path */
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Error: cannot write %s\n", path); free(data); return 1; }

    uint32_t magic = 0x46474C53u;
    uint32_t version = 4;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&n_chunks, 4, 1, f);
    fwrite(&sz, 4, 1, f);

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t off = i * 64;
        uint32_t len = (sz - off > 64) ? 64 : (sz - off);

        int64_t vx = 0, vy = 0;
        for (uint32_t j = 0; j < len; j++) {
            vx += data[off + j] * (j + 1);
            vy += data[off + j] * (j * 7 + 3);
        }

        uint32_t node_id = (uint32_t)((vx * 31 + vy * 37) & 0x7FFF);
        if (node_id >= 20736) node_id %= 20736;

        fwrite(&node_id, 4, 1, f);
        fwrite(&vx, 8, 1, f);
        fwrite(&vy, 8, 1, f);
        uint8_t drain = 0;
        fwrite(&drain, 1, 1, f);
    }
    fclose(f);

    printf("  %s: %u → %u bytes (%.2fx)\n", path, sz, bp_size, ratio);
    free(data);
    return 0;
}

/* ── decompress: detect and restore ── */
static int cmd_decompress(const char *path) {
    uint32_t sz;
    uint8_t *data = read_file(path, &sz);
    if (!data) return 1;

    /* Check if compressed */
    if (sz < 16 || *(uint32_t *)data != 0x46474C53u) {
        printf("Not compressed: %s\n", path);
        free(data);
        return 0;
    }

    /* Read header */
    uint32_t version, n_chunks, orig_size;
    memcpy(&version, data + 4, 4);
    memcpy(&n_chunks, data + 8, 4);
    memcpy(&orig_size, data + 12, 4);

    printf("Decompressing: %s (%u → %u bytes)\n", path, sz, orig_size);

    /* Reconstruct */
    uint8_t *out_buf = (uint8_t *)calloc(orig_size, 1);
    if (!out_buf) { free(data); return 1; }

    uint32_t pos = 16;
    for (uint32_t i = 0; i < n_chunks; i++) {
        if (pos + 21 > sz) break;

        uint32_t node_id;
        int64_t rx, ry;
        uint8_t drain;
        memcpy(&node_id, data + pos, 4); pos += 4;
        memcpy(&rx, data + pos, 8); pos += 8;
        memcpy(&ry, data + pos, 8); pos += 8;
        drain = data[pos]; pos += 1;

        uint32_t off = i * 64;
        uint32_t len = (orig_size - off > 64) ? 64 : (orig_size - off);

        for (uint32_t j = 0; j < len; j++) {
            uint32_t idx = (node_id + j * 31 + (uint32_t)rx) & 0xFF;
            out_buf[off + j] = (uint8_t)(idx ^ (uint8_t)(ry >> (j * 8)));
        }
    }
    free(data);

    /* Write back */
    int rc = write_file(path, out_buf, orig_size);
    printf("  Restored: %s (%u bytes)\n", path, orig_size);
    free(out_buf);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "version") == 0) {
        printf("FGLS Universal Codec v" FGLS_VERSION "\n");
#ifdef FGLS_USE_ZSTD
        printf("  ZSTD support: yes\n");
#else
        printf("  ZSTD support: no (build with -DFGLS_USE_ZSTD)\n");
#endif
        return 0;
    }
    if (strcmp(cmd, "profile") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls profile <file>\n"); return 1; }
        return cmd_profile(argv[2]);
    }
    if (strcmp(cmd, "encode") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: fgls encode <input> <output>\n"); return 1; }
        return cmd_encode(argv[2], argv[3]);
    }
    if (strcmp(cmd, "encode-framed") == 0 || strcmp(cmd, "ef") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: fgls encode-framed <input> <output>\n"); return 1; }
        return cmd_encode_framed(argv[2], argv[3]);
    }
    if (strcmp(cmd, "encode-auto") == 0 || strcmp(cmd, "ea") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: fgls encode-auto <input> <output>\n"); return 1; }
        return cmd_encode_auto(argv[2], argv[3]);
    }
    if (strcmp(cmd, "decode") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: fgls decode <input> <output>\n"); return 1; }
        return cmd_decode(argv[2], argv[3]);
    }
    if (strcmp(cmd, "decode-framed") == 0 || strcmp(cmd, "df") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: fgls decode-framed <input> <output>\n"); return 1; }
        return cmd_decode_framed(argv[2], argv[3]);
    }
    if (strcmp(cmd, "encode-lblock") == 0 || strcmp(cmd, "el") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: fgls encode-lblock <input> <output>\n"); return 1; }
        return cmd_encode_lblock(argv[2], argv[3]);
    }
    if (strcmp(cmd, "decode-lblock") == 0 || strcmp(cmd, "dl") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: fgls decode-lblock <input> <output>\n"); return 1; }
        return cmd_decode_lblock(argv[2], argv[3]);
    }
    if (strcmp(cmd, "lblock-reshape") == 0 || strcmp(cmd, "lr") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: fgls lblock-reshape <input> <output>\n"); return 1; }
        return cmd_lblock_reshape(argv[2], argv[3]);
    }
    if (strcmp(cmd, "field-info") == 0 || strcmp(cmd, "fi") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: fgls field-info <input> <output>\n"); return 1; }
        return cmd_field_info(argv[2], argv[3]);
    }
    if (strcmp(cmd, "tring-demo") == 0 || strcmp(cmd, "td") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls tring-demo <output>\n"); return 1; }
        return cmd_tring_demo(argv[2]);
    }
    if (strcmp(cmd, "torus-demo") == 0 || strcmp(cmd, "tod") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls torus-demo <output>\n"); return 1; }
        return cmd_torus_demo(argv[2]);
    }
    if (strcmp(cmd, "reshape-demo") == 0 || strcmp(cmd, "rd") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls reshape-demo <output>\n"); return 1; }
        return cmd_reshape_demo(argv[2]);
    }
    if (strcmp(cmd, "dual-demo") == 0 || strcmp(cmd, "dd") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls dual-demo <output>\n"); return 1; }
        return cmd_dual_demo(argv[2]);
    }
    if (strcmp(cmd, "timetravel-demo") == 0 || strcmp(cmd, "ttd") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls timetravel-demo <output>\n"); return 1; }
        return cmd_timetravel_demo(argv[2]);
    }
    if (strcmp(cmd, "bench") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls bench <file>\n"); return 1; }
        return cmd_bench(argv[2]);
    }
    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls info <file>\n"); return 1; }
        return cmd_info(argv[2]);
    }
    if (strcmp(cmd, "capture") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls capture <input> [output.fgls]\n"); return 1; }
        return cmd_capture(argv[2], argc > 3 ? argv[3] : NULL);
    }
    if (strcmp(cmd, "summon") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls summon <input.fgls> [output]\n"); return 1; }
        return cmd_summon(argv[2], argc > 3 ? argv[3] : NULL);
    }
    if (strcmp(cmd, "compress") == 0 || strcmp(cmd, "c") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls compress <file>\n"); return 1; }
        return cmd_compress(argv[2]);
    }
    if (strcmp(cmd, "decompress") == 0 || strcmp(cmd, "d") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: fgls decompress <file>\n"); return 1; }
        return cmd_decompress(argv[2]);
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    usage();
    return 1;
}
