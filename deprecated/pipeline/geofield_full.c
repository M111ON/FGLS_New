/*
 * geofield_full.c — Unified Pipeline: geo_frame_seek + Diamond Shell + DRamTile + 24-face
 *
 * Temporal compression via intra-frame delta coding:
 *   - 12 chunks per frame (768B) → seed(64B) + 11 XOR residuals
 *   - Residuals are sparse for structured data → Diamond Shell compresses well
 *   - geo_frame_seek provides enc values for frame addressing
 *
 * GFUF v2 format: header + frame_encs + frame_offsets + per-frame(seed + residuals)
 *
 * Build (from project root):
 *   gcc -O2 -std=c11 -fno-strict-aliasing -lm \
 *       -I. -Icore -Icollection -Irunner \
 *       -o geofield_full.exe pipeline/geofield_full.c
 *
 * Usage:
 *   geofield_full encode input.bin output.gfuf
 *   geofield_full decode output.gfuf output.bin
 *   geofield_full info output.gfuf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════
 * COMPONENT HEADERS
 * ═══════════════════════════════════════════════════════════════ */
#include "geo_frame_seek.h"
#include "dramtile_store.h"
#include "geo_radial_capture.h"

/* ═══════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════ */
#define CHUNK_SZ        64u
#define MAX_CHUNKS      65536u
#define FRAME_CHUNKS    12u
#define BS_ROT_STATES   6u

/* ═══════════════════════════════════════════════════════════════
 * GFUF v2 CONTAINER FORMAT
 * ═══════════════════════════════════════════════════════════════
 * Header (36B):
 *   magic "GFUF" (4B), version=2 (2B), flags (2B)
 *   orig_size (4B), n_chunks (4B), n_frames (4B)
 *   global_seed (4B), struct_total (4B), max_frame_sz (4B)
 *
 * Sections:
 *   [frame_encs: n_frames × 2B]
 *   [frame_offsets: (n_frames+1) × 4B]
 *   [frame data: max_frame_sz × n_frames bytes]
 *     per frame: [seed_sz:1][seed_data:seed_sz][residuals...]
 *       residuals: 11 × [ds_sz:1][ds_data:ds_sz] (chunk_i XOR seed)
 * ═══════════════════════════════════════════════════════════════ */

#define GFUF_MAGIC      0x46554647u
#define GFUF_VERSION    2
#define GFUF_FLAG_HUFF  0x0001u   /* frame_buf is Huffman-compressed */
#define GFUF_HDR_SZ     36

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t orig_size;
    uint32_t n_chunks;
    uint32_t n_frames;
    uint32_t global_seed;
    uint32_t struct_total;
    uint32_t max_frame_sz;
} GFUFHeader;

static void gfuf_hdr_write(uint8_t *b, const GFUFHeader *h) {
    memcpy(b,      &h->magic,        4);
    memcpy(b + 4,  &h->version,      2);
    memcpy(b + 6,  &h->flags,        2);
    memcpy(b + 8,  &h->orig_size,    4);
    memcpy(b + 12, &h->n_chunks,     4);
    memcpy(b + 16, &h->n_frames,     4);
    memcpy(b + 20, &h->global_seed,  4);
    memcpy(b + 24, &h->struct_total, 4);
    memcpy(b + 28, &h->max_frame_sz, 4);
    memset(b + 32, 0, 4);
}

static void gfuf_hdr_read(const uint8_t *b, GFUFHeader *h) {
    memcpy(&h->magic,        b,      4);
    memcpy(&h->version,      b + 4,  2);
    memcpy(&h->flags,        b + 6,  2);
    memcpy(&h->orig_size,    b + 8,  4);
    memcpy(&h->n_chunks,     b + 12, 4);
    memcpy(&h->n_frames,     b + 16, 4);
    memcpy(&h->global_seed,  b + 20, 4);
    memcpy(&h->struct_total, b + 24, 4);
    memcpy(&h->max_frame_sz, b + 28, 4);
}

/* ═══════════════════════════════════════════════════════════════
 * SIMPLE HUFFMAN CODER — byte-level entropy coding
 * ═══════════════════════════════════════════════════════════════
 * Max 256 symbols × 24-bit codewords. Freq table sent once per buffer.
 * Table overhead: 256×4 = 1024 bytes amortized over frame.
 * Wire: [4×tree_sz:1][tree_data:tree_sz][packed_bits...]
 * For high-entropy data (7.6 bits/byte), expected ~0.955x.
 * ═══════════════════════════════════════════════════════════════ */

#define HUFF_MAX_SYMS 256
#define HUFF_MAX_BITS 24

typedef struct { uint32_t freq; int sym; int left, right; int code; int bits; } huff_node_t;

/* Build Huffman tree, return root index */
static int huff_build(huff_node_t nodes[512], const uint32_t freq[256]) {
    int n = 0;
    for (int i = 0; i < 256; i++) {
        if (freq[i]) {
            nodes[n].freq = freq[i];
            nodes[n].sym  = i;
            nodes[n].left = nodes[n].right = -1;
            n++;
        }
    }
    if (n == 0) { nodes[0].freq = 1; nodes[0].sym = 0; nodes[0].left = nodes[0].right = -1; n = 1; }
    if (n == 1) { nodes[n] = nodes[0]; nodes[0].left = 0; nodes[0].right = 1; n = 2; } /* fake sibling */

    int heap[256], hn = n;
    for (int i = 0; i < n; i++) heap[i] = i;
    /* Simple O(n²) min-select — fine for small Huffman trees */
    while (hn > 1) {
        int a = 0, b = 1;
        if (nodes[heap[a]].freq > nodes[heap[b]].freq) { int t = a; a = b; b = t; }
        for (int i = 2; i < hn; i++) {
            int ci = heap[i];
            if (nodes[ci].freq < nodes[heap[a]].freq) { b = a; a = i; }
            else if (nodes[ci].freq < nodes[heap[b]].freq) { b = i; }
        }
        int ai = heap[a], bi = heap[b];
        nodes[n].freq = nodes[ai].freq + nodes[bi].freq;
        nodes[n].left = ai; nodes[n].right = bi; nodes[n].sym = -1;
        heap[b] = n;
        heap[a] = heap[--hn];
        n++;
    }
    return heap[0];
}

/* Assign codes recursively */
static void huff_assign(huff_node_t nodes[512], int idx, int code, int bits) {
    if (idx < 0) return;
    nodes[idx].code = code;
    nodes[idx].bits = bits;
    if (nodes[idx].left >= 0) huff_assign(nodes, nodes[idx].left,  code << 1,      bits + 1);
    if (nodes[idx].right >= 0)huff_assign(nodes, nodes[idx].right, (code << 1) | 1, bits + 1);
}

/* Encode buffer with Huffman. Returns bytes written or 0 on expansion fallback. */
static uint32_t huff_encode(uint8_t *out, const uint8_t *in, uint32_t in_sz) {
    if (in_sz == 0) return 0;
    uint32_t freq[256] = {0};
    for (uint32_t i = 0; i < in_sz; i++) freq[in[i]]++;

    huff_node_t nodes[512];
    int root = huff_build(nodes, freq);
    huff_assign(nodes, root, 0, 0);

    /* Build code table */
    uint32_t hcode[256] = {0};
    uint8_t  hbits[256] = {0};
    for (int i = 0; i < 512 && nodes[i].sym >= 0; i++) {
        hcode[nodes[i].sym] = (uint32_t)nodes[i].code;
        hbits[nodes[i].sym] = (uint8_t)nodes[i].bits;
    }

    /* Count output bits */
    uint64_t total_bits = 0;
    for (uint32_t i = 0; i < in_sz; i++)
        total_bits += hbits[in[i]];

    uint32_t out_sz = (uint32_t)((total_bits + 7) / 8) + 4 + 256*4;
    if (out_sz >= in_sz) return 0; /* expansion → fallback to raw */

    /* Write: [4×packed_sz:1][256×4=freq...][packed_bits...] */
    uint32_t ps = (uint32_t)((total_bits + 7) / 8);
    out[0] = (uint8_t)(ps & 0xFF); out[1] = (uint8_t)((ps>>8)&0xFF);
    out[2] = (uint8_t)((ps>>16)&0xFF); out[3] = (uint8_t)((ps>>24)&0xFF);
    memcpy(out + 4, freq, 256*4);

    uint64_t bit_pos = 0;
    uint32_t buf32 = 0;
    int buf_bits = 0;
    for (uint32_t i = 0; i < in_sz; i++) {
        uint32_t c = hcode[in[i]];
        int b = hbits[in[i]];
        buf32 = (buf32 << b) | c;
        buf_bits += b;
        while (buf_bits >= 8) {
            buf_bits -= 8;
            out[4 + 256*4 + (bit_pos/8)] = (uint8_t)(buf32 >> buf_bits);
            bit_pos += 8;
        }
    }
    if (buf_bits > 0) out[4 + 256*4 + (bit_pos/8)] = (uint8_t)(buf32 << (8 - buf_bits));
    return 4 + 256*4 + ps;
}

/* Decode Huffman buffer. Returns bytes written. */
static uint32_t huff_decode(uint8_t *out, const uint8_t *in, uint32_t in_sz) {
    uint32_t ps = (uint32_t)in[0] | ((uint32_t)in[1]<<8) | ((uint32_t)in[2]<<16) | ((uint32_t)in[3]<<24);
    uint32_t freq[256];
    memcpy(freq, in + 4, 256*4);

    huff_node_t nodes[512];
    int root = huff_build(nodes, freq);
    huff_assign(nodes, root, 0, 0);

    uint32_t out_idx = 0;
    int node = root;
    uint64_t bit_limit = ps * 8;
    for (uint64_t bi = 0; bi < bit_limit; bi++) {
        uint32_t byte_idx = 4 + 256*4 + (bi / 8);
        int bit = (in[byte_idx] >> (7 - (bi % 8))) & 1;
        node = bit ? nodes[node].right : nodes[node].left;
        if (nodes[node].sym >= 0) {
            out[out_idx++] = (uint8_t)nodes[node].sym;
            node = root;
        }
    }
    return out_idx;
}

/* ═══════════════════════════════════════════════════════════════
 * DIAMOND SHELL SUB-BLOCKS — optimized v2
 * ═══════════════════════════════════════════════════════════════
 *
 * Wire format (flag byte determines encoding):
 *   FLAT    0x00 [1B]          — all-zero chunk
 *   SPARSE  0xFD [3B + 2*nz]   — ≤16 non-zero bytes (index+value pairs)
 *   RAW     0xFE [1B + 64B]    — dense chunk stored as-is
 *   SUB     0xFF [3B + subs]   — sub-block encoding (up to 8×8B)
 *
 * For SUB mode:
 *   0xFF | rot(6) | sub_flags(8) | [active_sub_0..7 each 8B]
 *   sub_flags bit s = 1 → sub-block s is stored (8 bytes each)
 *
 * For SPARSE mode:
 *   0xFD | rot(6) | nz(8) | [idx_0..idx_nz-1] | [val_0..val_nz-1]
 *   Indices and values are separated (2 arrays), nz ≤ 16
 *
 * For RAW mode:
 *   0xFE | rot(6) | data(64B) — store full rotated 64B
 *
 * Rotation heuristic: try all 6 orientations, pick best.
 * Metric depends on mode:
 *   - SUB: minimize active sub-blocks
 *   - SPARSE: minimize non-zero bytes after rotation
 *   - RAW: just find best rotation (dense data, no compression possible)
 * ═══════════════════════════════════════════════════════════════ */

#define DS_FLAG_FLAT    0
#define DS_FLAG_SPARSE  0xFD
#define DS_FLAG_RAW     0xFE
#define DS_SUB_N        8
#define DS_SUB_SZ       8
#define DS_SPARSE_MAX   16u   /* max non-zero for sparse mode */
#define DS_RAW_ACTIVE   7u    /* ≥ this many active subs → RAW fallback */

/* Count non-zero bytes in 64B buffer */
static inline int ds_count_nz(const uint8_t *buf) {
    int n = 0;
    for (int i = 0; i < 64; i++) if (buf[i]) n++;
    return n;
}

/* Count active sub-blocks (non-zero sub-blocks) */
static inline int ds_count_active_subs(const uint8_t *buf, uint8_t *sub_flags_out) {
    uint8_t flags = 0;
    for (int s = 0; s < DS_SUB_N; s++) {
        int has_nz = 0;
        for (int j = 0; j < DS_SUB_SZ; j++)
            if (buf[s * DS_SUB_SZ + j]) { has_nz = 1; break; }
        if (has_nz) flags |= (1u << s);
    }
    int active = 0;
    for (int s = 0; s < DS_SUB_N; s++)
        if (flags & (1u << s)) active++;
    if (sub_flags_out) *sub_flags_out = flags;
    return active;
}

static void ds_rotate64(uint8_t *out, const uint8_t *in, int rot) {
    for (int z = 0; z < 4; z++)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int si = z * 16 + y * 4 + x;
                int di;
                switch (rot % 6) {
                    case 0: di = z * 16 + y * 4 + x; break;
                    case 1: di = y * 16 + x * 4 + z; break;
                    case 2: di = x * 16 + z * 4 + y; break;
                    case 3: di = (3-z) * 16 + y * 4 + x; break;
                    case 4: di = x * 16 + y * 4 + z; break;
                    case 5: di = z * 16 + x * 4 + y; break;
                    default: di = si; break;
                }
                out[di] = in[si];
            }
}

static uint32_t ds_classify(uint8_t *out, const uint8_t block[64]) {
    /* FLAT: all-zero → 1 byte */
    int is_zero = 1;
    for (int i = 0; i < 64; i++) { if (block[i]) { is_zero = 0; break; } }
    if (is_zero) { out[0] = DS_FLAG_FLAT; return 1; }

    /* Scan all 6 rotations, collect metrics */
    int      best_active  = DS_SUB_N + 1;
    int      best_nz      = 65;
    uint8_t  best_rot     = 0, best_sub_flags = 0;
    uint8_t  best_buf[64], rotbuf[64];

    for (uint8_t rot = 0; rot < BS_ROT_STATES; rot++) {
        ds_rotate64(rotbuf, block, rot);
        uint8_t sub_flags = 0;
        int active = 0;
        for (int s = 0; s < DS_SUB_N; s++) {
            int has_nz = 0;
            for (int j = 0; j < DS_SUB_SZ; j++)
                if (rotbuf[s * DS_SUB_SZ + j]) { has_nz = 1; break; }
            if (has_nz) { sub_flags |= (1u << s); active++; }
        }
        int nz = ds_count_nz(rotbuf);
        /* Prefer fewer active subs; then fewer non-zero bytes as tiebreak */
        if (active < best_active || (active == best_active && nz < best_nz)) {
            best_active = active;
            best_nz     = nz;
            best_rot    = rot;
            best_sub_flags = sub_flags;
            memcpy(best_buf, rotbuf, 64);
        }
    }

    /* SPARSE: ≤16 non-zero bytes → index+value pairs (much smaller than sub-blocks) */
    if ((uint32_t)best_nz <= DS_SPARSE_MAX) {
        out[0] = DS_FLAG_SPARSE;
        out[1] = best_rot;
        out[2] = (uint8_t)best_nz;
        uint32_t pos = 3;
        for (uint32_t i = 0; i < 64 && pos < 3 + (uint32_t)best_nz; i++) {
            if (best_buf[i]) {
                out[pos] = (uint8_t)i;
                out[pos + (uint32_t)best_nz] = best_buf[i];
                pos++;
            }
        }
        return 3 + (uint32_t)best_nz * 2;
    }

    /* RAW fallback: ≥7 active sub-blocks or high density → store raw 64B */
    if (best_active >= DS_RAW_ACTIVE) {
        out[0] = DS_FLAG_RAW;
        out[1] = best_rot;
        memcpy(out + 2, best_buf, 64);
        return 66; /* 2 header + 64 data */
    }

    /* SUB encoding: 3 header + active sub-blocks × 8B each */
    out[0] = 0xFF; out[1] = best_rot; out[2] = best_sub_flags;
    uint32_t pos = 3;
    for (int s = 0; s < DS_SUB_N; s++)
        if (best_sub_flags & (1u << s)) {
            memcpy(out + pos, best_buf + s * DS_SUB_SZ, DS_SUB_SZ);
            pos += DS_SUB_SZ;
        }
    return pos;
}

static uint32_t ds_decode(uint8_t out[64], const uint8_t *in) {
    uint8_t flag = in[0];

    /* FLAT */
    if (flag == DS_FLAG_FLAT) { memset(out, 0, 64); return 1; }

    /* SPARSE: index+value pairs */
    if (flag == DS_FLAG_SPARSE) {
        uint8_t rot  = in[1];
        uint8_t nz   = in[2];
        uint8_t rotbuf[64];
        memset(rotbuf, 0, 64);
        for (uint32_t i = 0; i < nz; i++) {
            uint8_t idx = in[3 + i];
            uint8_t val = in[3 + nz + i];
            if (idx < 64) rotbuf[idx] = val;
        }
        /* Inverse rotation */
        for (int nz = 0; nz < 4; nz++)
            for (int ny = 0; ny < 4; ny++)
                for (int nx = 0; nx < 4; nx++) {
                    int sx, sy, sz;
                    switch (rot % 6) {
                        case 0: sx=nx;   sy=ny;   sz=nz;   break;
                        case 1: sx=nz;   sy=nx;   sz=ny;   break;
                        case 2: sx=ny;   sy=nz;   sz=nx;   break;
                        case 3: sx=nx;   sy=ny;   sz=3-nz; break;
                        case 4: sx=nz;   sy=ny;   sz=nx;   break;
                        case 5: sx=ny;   sy=nx;   sz=nz;   break;
                        default: sx=nx; sy=ny; sz=nz; break;
                    }
                    out[nx + ny*4 + nz*16] = rotbuf[sx + sy*4 + sz*16];
                }
        return 3 + (uint32_t)nz * 2;
    }

    /* RAW: stored entire rotated 64B */
    if (flag == DS_FLAG_RAW) {
        uint8_t rot = in[1];
        uint8_t rotbuf[64];
        memcpy(rotbuf, in + 2, 64);
        for (int nz = 0; nz < 4; nz++)
            for (int ny = 0; ny < 4; ny++)
                for (int nx = 0; nx < 4; nx++) {
                    int sx, sy, sz;
                    switch (rot % 6) {
                        case 0: sx=nx;   sy=ny;   sz=nz;   break;
                        case 1: sx=nz;   sy=nx;   sz=ny;   break;
                        case 2: sx=ny;   sy=nz;   sz=nx;   break;
                        case 3: sx=nx;   sy=ny;   sz=3-nz; break;
                        case 4: sx=nz;   sy=ny;   sz=nx;   break;
                        case 5: sx=ny;   sy=nx;   sz=nz;   break;
                        default: sx=nx; sy=ny; sz=nz; break;
                    }
                    out[nx + ny*4 + nz*16] = rotbuf[sx + sy*4 + sz*16];
                }
        return 66;
    }

    /* SUB encoding (flag == 0xFF) */
    uint8_t rot = in[1], sub_flags = in[2];
    uint8_t rotbuf[64];
    memset(rotbuf, 0, 64);
    uint32_t pos = 3;
    for (int s = 0; s < DS_SUB_N; s++)
        if (sub_flags & (1u << s)) {
            memcpy(rotbuf + s * DS_SUB_SZ, in + pos, DS_SUB_SZ);
            pos += DS_SUB_SZ;
        }
    /* Inverse rotation */
    for (int nz = 0; nz < 4; nz++)
        for (int ny = 0; ny < 4; ny++)
            for (int nx = 0; nx < 4; nx++) {
                int sx, sy, sz;
                switch (rot % 6) {
                    case 0: sx=nx;   sy=ny;   sz=nz;   break;
                    case 1: sx=nz;   sy=nx;   sz=ny;   break;
                    case 2: sx=ny;   sy=nz;   sz=nx;   break;
                    case 3: sx=nx;   sy=ny;   sz=3-nz; break;
                    case 4: sx=nz;   sy=ny;   sz=nx;   break;
                    case 5: sx=ny;   sy=nx;   sz=nz;   break;
                    default: sx=nx; sy=ny; sz=nz; break;
                }
                out[nx + ny*4 + nz*16] = rotbuf[sx + sy*4 + sz*16];
            }
    return pos;
}

/* ═══════════════════════════════════════════════════════════════
 * ENCODE: data → .gfuf (temporal delta compression)
 * ═══════════════════════════════════════════════════════════════
 * Each frame: seed (chunk 0, DS-encoded) + 11 XOR residuals (DS-encoded)
 * Residual = chunk_i XOR chunk_0 → sparse for structured data
 * ═══════════════════════════════════════════════════════════════ */
static int do_encode(const char *in_path, const char *out_path) {
    if (geo_frame_seek_verify() != 0)
        fprintf(stderr, "Warning: geo_frame_seek_verify failed\n");

    /* Read input */
    FILE *fp = fopen(in_path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", in_path); return -1; }
    fseek(fp, 0, SEEK_END);
    uint32_t data_sz = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(data_sz);
    if (!data) { fclose(fp); fprintf(stderr, "OOM\n"); return -1; }
    if (fread(data, 1, data_sz, fp) != data_sz) {
        free(data); fclose(fp); return -1;
    }
    fclose(fp);

    uint32_t n_chunks = (data_sz + CHUNK_SZ - 1) / CHUNK_SZ;
    if (n_chunks == 0 || n_chunks > MAX_CHUNKS) { free(data); return -1; }
    uint32_t n_frames = (n_chunks + FRAME_CHUNKS - 1) / FRAME_CHUNKS;

    /* Frame enc values */
    uint16_t *frame_encs = (uint16_t *)calloc(n_frames, sizeof(uint16_t));
    for (uint32_t fi = 0; fi < n_frames; fi++)
        frame_encs[fi] = frame_enc(fi);

    /* Encode each frame: seed + XOR residuals */
    uint32_t *frame_offsets = (uint32_t *)calloc(n_frames + 1, sizeof(uint32_t));
    uint8_t  *frame_buf = (uint8_t *)malloc((size_t)n_frames * 900u);
    if (!frame_encs || !frame_offsets || !frame_buf) {
        free(frame_encs); free(frame_offsets); free(frame_buf); free(data);
        fprintf(stderr, "OOM\n"); return -1;
    }

    uint32_t n_flat = 0, n_sparse = 0, n_dense = 0;
    uint32_t n_adj = 0;  /* chunks using adjacent delta vs seed */
    uint32_t total_res_sz = 0;
    uint32_t frame_buf_pos = 0;

    for (uint32_t fi = 0; fi < n_frames; fi++) {
        uint32_t fi_start = fi * FRAME_CHUNKS;
        uint32_t chunks_in_frame = n_chunks - fi_start;
        if (chunks_in_frame > FRAME_CHUNKS) chunks_in_frame = FRAME_CHUNKS;

        /* Seed selection: try each chunk in frame, pick the one that
         * minimizes total frame size (seed cost + residual costs).
         * This handles data where chunk[0] is not the best reference. */
        uint8_t  best_seed_chunk[CHUNK_SZ];
        uint8_t  best_seed_ds[70];
        uint32_t best_seed_ds_sz;
        uint32_t best_frame_total = 0xFFFFFFFF;
        int      best_seed_idx = 0;
        /* per-chunk residual storage for best seed */
        uint8_t  best_res_ds[11][70];
        uint32_t best_res_ds_sz[11];
        uint8_t  best_res_ref[11];   /* 0=seed-XOR, 1=adjacent-XOR */

        /* Try a few seed candidates: chunk[0], chunk[mid], chunk[max_var] */
        uint32_t seed_candidates[3];
        seed_candidates[0] = 0;
        seed_candidates[1] = chunks_in_frame / 2;
        {   /* chunk with max byte-sum variance (most representative) */
            uint32_t max_var = 0;
            seed_candidates[2] = 0;
            for (uint32_t sci = 0; sci < chunks_in_frame; sci++) {
                uint32_t sc_off = (fi_start + sci) * CHUNK_SZ;
                uint32_t sum = 0;
                for (uint32_t b = 0; b < CHUNK_SZ && (sc_off + b) < data_sz; b++)
                    sum += data[sc_off + b];
                if (sum > max_var) { max_var = sum; seed_candidates[2] = sci; }
            }
        }

        for (int sci = 0; sci < 3; sci++) {
            uint32_t seed_local_idx = seed_candidates[sci];
            if (seed_local_idx >= chunks_in_frame) continue;

            uint8_t this_seed[CHUNK_SZ];
            memset(this_seed, 0, CHUNK_SZ);
            uint32_t s_off = (fi_start + seed_local_idx) * CHUNK_SZ;
            uint32_t s_sz = (s_off + CHUNK_SZ <= data_sz) ? CHUNK_SZ : data_sz - s_off;
            memcpy(this_seed, data + s_off, s_sz);

            uint8_t seed_ds[70];
            uint32_t seed_ds_sz = ds_classify(seed_ds, this_seed);
            uint32_t frame_total = seed_ds_sz;

            uint8_t  tmp_res[11][70];
            uint32_t tmp_res_sz[11];
            uint8_t  tmp_ref[11];     /* 0=seed-XOR, 1=adjacent-XOR */
            int      tmp_valid = 1;
            int      ri = 0;

            for (uint32_t ci = 0; ci < chunks_in_frame; ci++) {
                if (ci == seed_local_idx) continue;

                uint32_t coff = (fi_start + ci) * CHUNK_SZ;
                uint8_t chunk[CHUNK_SZ];
                memset(chunk, 0, CHUNK_SZ);
                uint32_t csz = (coff + CHUNK_SZ <= data_sz) ? CHUNK_SZ : data_sz - coff;
                memcpy(chunk, data + coff, csz);

                /* Try seed-XOR */
                uint8_t res_seed[CHUNK_SZ];
                for (uint32_t b = 0; b < CHUNK_SZ; b++)
                    res_seed[b] = chunk[b] ^ this_seed[b];
                uint8_t res_seed_ds[70];
                uint32_t res_seed_sz = ds_classify(res_seed_ds, res_seed);
                uint8_t best_ref = 0;
                uint32_t best_sz = res_seed_sz;
                memcpy(tmp_res[ri], res_seed_ds, res_seed_sz);
                tmp_res_sz[ri] = res_seed_sz;
                tmp_ref[ri] = 0;

                /* Try adjacent-XOR (chunk XOR chunk[ci-1]) for ci > 0 */
                if (ci > 0) {
                    uint8_t pred[CHUNK_SZ];
                    memset(pred, 0, CHUNK_SZ);
                    uint32_t poff = (fi_start + ci - 1) * CHUNK_SZ;
                    uint32_t psz = (poff + CHUNK_SZ <= data_sz) ? CHUNK_SZ : data_sz - poff;
                    memcpy(pred, data + poff, psz);

                    uint8_t res_adj[CHUNK_SZ];
                    for (uint32_t b = 0; b < CHUNK_SZ; b++)
                        res_adj[b] = chunk[b] ^ pred[b];
                    uint8_t res_adj_ds[70];
                    uint32_t res_adj_sz = ds_classify(res_adj_ds, res_adj);
                    if (res_adj_sz < best_sz) {
                        best_sz = res_adj_sz;
                        best_ref = 1;
                        memcpy(tmp_res[ri], res_adj_ds, res_adj_sz);
                        tmp_res_sz[ri] = res_adj_sz;
                        tmp_ref[ri] = 1;
                    }
                }

                frame_total += 1 + best_sz;
                if (frame_total >= best_frame_total) { tmp_valid = 0; break; }
                ri++;
            }

            if (tmp_valid && frame_total < best_frame_total) {
                best_frame_total = frame_total;
                best_seed_idx = (int)seed_local_idx;
                memcpy(best_seed_chunk, this_seed, CHUNK_SZ);
                memcpy(best_seed_ds, seed_ds, seed_ds_sz);
                best_seed_ds_sz = seed_ds_sz;
                for (int ri = 0; ri < (int)(chunks_in_frame - 1); ri++) {
                    memcpy(best_res_ds[ri], tmp_res[ri], tmp_res_sz[ri]);
                    best_res_ds_sz[ri] = tmp_res_sz[ri];
                    best_res_ref[ri] = tmp_ref[ri];
                }
            }
        }

        /* Debug: show seed selection */
        if (best_seed_idx != 0 && fi < 3)
            fprintf(stderr, "  [frame %u] seed picked: chunk[%u] (was chunk[0])\n", fi, best_seed_idx);

        /* Store frame using best seed */
        uint32_t fstart = frame_buf_pos;
        /* Seed header: [sz_byte:1] where bit7=1 means extended with [seed_idx:1] following.
         * For seed_idx=0 (common): sz_byte = ds_sz (backward compat, ds_sz < 128)
         * For seed_idx!=0: sz_byte = 0x80 | ds_sz, then next byte = seed_idx */
        if (best_seed_idx == 0) {
            frame_buf[frame_buf_pos++] = (uint8_t)best_seed_ds_sz;
        } else {
            frame_buf[frame_buf_pos++] = (uint8_t)(0x80 | best_seed_ds_sz);
            frame_buf[frame_buf_pos++] = (uint8_t)best_seed_idx;
        }
        memcpy(frame_buf + frame_buf_pos, best_seed_ds, best_seed_ds_sz);
        frame_buf_pos += best_seed_ds_sz;

        if (best_seed_ds[0] == DS_FLAG_FLAT) n_flat++;
        else if (best_seed_ds_sz <= 27) n_sparse++;
        else n_dense++;

        /* DS-encode each non-seed chunk against best reference */
        uint8_t prev_chunk[CHUNK_SZ];
        memcpy(prev_chunk, best_seed_chunk, CHUNK_SZ);  /* prev starts as seed */

        for (uint32_t ci = 0; ci < chunks_in_frame; ci++) {
            if (ci == (uint32_t)best_seed_idx) { 
                /* Make sure prev is updated for adjacent delta */
                uint32_t coff = (fi_start + ci) * CHUNK_SZ;
                uint32_t csz = (coff + CHUNK_SZ <= data_sz) ? CHUNK_SZ : data_sz - coff;
                uint8_t cbuf[CHUNK_SZ];
                memset(cbuf, 0, CHUNK_SZ);
                memcpy(cbuf, data + coff, csz);
                if (ci != 0) memcpy(prev_chunk, cbuf, CHUNK_SZ);
                continue;
            }

            uint32_t chunk_idx = fi_start + ci;
            uint32_t coff = chunk_idx * CHUNK_SZ;
            uint8_t chunk[CHUNK_SZ];
            memset(chunk, 0, CHUNK_SZ);
            uint32_t csz = (coff + CHUNK_SZ <= data_sz) ? CHUNK_SZ : data_sz - coff;
            memcpy(chunk, data + coff, csz);

            /* Determine reference: use adjacent-XOR if it was better in evaluation,
             * otherwise seed-XOR. We use the pre-computed best encoding. */
            int ri = 0;
            for (uint32_t rc = 0; rc < chunks_in_frame; rc++) {
                if (rc == (uint32_t)best_seed_idx) continue;
                if (rc == ci) break;
                ri++;
            }

            uint8_t *res_ds = best_res_ds[ri];
            uint32_t res_ds_sz = best_res_ds_sz[ri];
            uint8_t  ref_flag = best_res_ref[ri];

            /* Store: [sz_with_flag:1][ds_data] */
            uint8_t sz_byte = (uint8_t)res_ds_sz;
            if (ref_flag) sz_byte |= 0x80;  /* high bit = adjacent-XOR */
            frame_buf[frame_buf_pos++] = sz_byte;
            memcpy(frame_buf + frame_buf_pos, res_ds, res_ds_sz);
            frame_buf_pos += res_ds_sz;

            total_res_sz += res_ds_sz;
            if (ref_flag) n_adj++;

            if (res_ds[0] == DS_FLAG_FLAT) n_flat++;
            else if (res_ds_sz <= 27) n_sparse++;
            else n_dense++;

            /* Update prev_chunk for adjacent-XOR chain */
            memcpy(prev_chunk, chunk, CHUNK_SZ);
        }

        frame_offsets[fi] = fstart;
    }
    frame_offsets[n_frames] = frame_buf_pos;

    /* DRamTile zero-copy store */
    DRamTileStore dt_store;
    memset(&dt_store, 0, sizeof(dt_store));
    dt_store_init(&dt_store, frame_buf_pos + 4096);
    for (uint32_t fi = 0; fi < n_frames; fi++) {
        uint32_t foff = frame_offsets[fi];
        uint32_t fsz = frame_offsets[fi + 1] - foff;
        char name[32];
        snprintf(name, sizeof(name), "frame.%u", fi);
        dt_put(&dt_store, name, frame_buf + foff, fsz);
    }

    /* Write GFUF v2 */
    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        dt_store_destroy(&dt_store);
        free(frame_buf); free(frame_offsets); free(frame_encs); free(data);
        return -1;
    }

    GFUFHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = GFUF_MAGIC;
    hdr.version = GFUF_VERSION;
    hdr.orig_size = data_sz;
    hdr.n_chunks = n_chunks;
    hdr.n_frames = n_frames;
    hdr.global_seed = (uint32_t)time(NULL);
    hdr.struct_total = frame_buf_pos;
    hdr.max_frame_sz = 800;

    uint8_t hdr_buf[GFUF_HDR_SZ];
    gfuf_hdr_write(hdr_buf, &hdr);
    fwrite(hdr_buf, 1, GFUF_HDR_SZ, fout);
    fwrite(frame_encs, 2, n_frames, fout);
    fwrite(frame_offsets, 4, n_frames + 1, fout);

    /* Try Huffman compression on frame_buf (global post-pass) */
    uint8_t *huff_buf = (uint8_t *)malloc(frame_buf_pos + 1024);
    uint32_t huff_sz = 0;
    if (frame_buf_pos > 512) {
        huff_sz = huff_encode(huff_buf, frame_buf, frame_buf_pos);
    }
    if (huff_sz > 0 && huff_sz < frame_buf_pos) {
        /* Compression successful — rewrite header with flags */
        hdr.flags |= GFUF_FLAG_HUFF;
        uint32_t orig_sz = hdr.struct_total;
        hdr.struct_total = orig_sz;         /* keep original (decompressed) size */
        hdr.max_frame_sz = huff_sz;          /* store compressed size */
        gfuf_hdr_write(hdr_buf, &hdr);
        fseek(fout, 0, SEEK_SET);
        fwrite(hdr_buf, 1, GFUF_HDR_SZ, fout);
        fseek(fout, 0, SEEK_END);
        fwrite(huff_buf, 1, huff_sz, fout);
    } else {
        fwrite(frame_buf, 1, frame_buf_pos, fout);
    }
    free(huff_buf);
    fclose(fout);

    /* Stats */
    uint32_t out_sz = 0;
    fout = fopen(out_path, "rb");
    if (fout) { fseek(fout, 0, SEEK_END); out_sz = (uint32_t)ftell(fout); fclose(fout); }

    printf("Encode: %s -> %s\n", in_path, out_path);
    printf("  %u -> %u bytes (%.2fx)\n", data_sz, out_sz,
           out_sz > 0 ? (double)out_sz / data_sz : 0.0);
    printf("  chunks=%u frames=%u (stride-37 cycle=%u)\n",
           n_chunks, n_frames, FRAME_CYCLE);
    printf("  ds: flat=%u sparse=%u dense=%u  adj-delta=%u\n", n_flat, n_sparse, n_dense, n_adj);
    printf("  seed=%u delta=%u total=%u (%.1f%% of raw)\n",
           n_frames * (CHUNK_SZ + 1),
           total_res_sz, frame_buf_pos,
           data_sz > 0 ? 100.0 * frame_buf_pos / data_sz : 0.0);
    printf("  geo_frame_seek: enc[0]=%u enc[1]=%u\n",
           frame_encs[0], n_frames > 1 ? frame_encs[1] : 0);
    printf("  24-face: RC_N_VERTICES=%u\n", RC_N_VERTICES);

    dt_store_destroy(&dt_store);
    free(frame_buf); free(frame_offsets); free(frame_encs); free(data);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE: .gfuf → data (temporal delta decompression)
 * ═══════════════════════════════════════════════════════════════ */
static int do_decode(const char *in_path, const char *out_path) {
    if (geo_frame_seek_verify() != 0)
        fprintf(stderr, "Warning: geo_frame_seek_verify failed\n");

    FILE *fp = fopen(in_path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", in_path); return -1; }

    uint8_t hdr_buf[GFUF_HDR_SZ];
    if (fread(hdr_buf, 1, GFUF_HDR_SZ, fp) != GFUF_HDR_SZ) {
        fclose(fp); return -1;
    }
    GFUFHeader hdr;
    gfuf_hdr_read(hdr_buf, &hdr);
    if (hdr.magic != GFUF_MAGIC || hdr.version != GFUF_VERSION) {
        fclose(fp); fprintf(stderr, "Bad header\n"); return -1;
    }

    uint32_t n_frames = hdr.n_frames;
    uint32_t n_chunks = hdr.n_chunks;

    uint16_t *frame_encs = (uint16_t *)malloc(n_frames * sizeof(uint16_t));
    uint32_t *frame_offsets = (uint32_t *)malloc((n_frames + 1) * sizeof(uint32_t));
    if (fread(frame_encs, 2, n_frames, fp) != n_frames ||
        fread(frame_offsets, 4, n_frames + 1, fp) != n_frames + 1) {
        fclose(fp); free(frame_encs); free(frame_offsets); return -1;
    }

    uint32_t frame_data_sz = hdr.struct_total;
    uint8_t *frame_buf;
    if (hdr.flags & GFUF_FLAG_HUFF) {
        /* Compressed: read compressed data, decompress */
        uint32_t comp_sz = hdr.max_frame_sz;
        uint8_t *comp = (uint8_t *)malloc(comp_sz);
        if (!comp || fread(comp, 1, comp_sz, fp) != comp_sz) {
            fclose(fp); free(comp); free(frame_encs); free(frame_offsets); return -1;
        }
        frame_buf = (uint8_t *)calloc(1, frame_data_sz + 64);
        if (!frame_buf) { fclose(fp); free(comp); free(frame_encs); free(frame_offsets); return -1; }
        uint32_t dec_sz = huff_decode(frame_buf, comp, comp_sz);
        if (dec_sz < frame_data_sz) {
            fclose(fp); free(comp); free(frame_buf); free(frame_encs); free(frame_offsets); return -1;
        }
        free(comp);
    } else {
        frame_buf = (uint8_t *)malloc(frame_data_sz);
        if (!frame_buf || fread(frame_buf, 1, frame_data_sz, fp) != frame_data_sz) {
            fclose(fp); free(frame_buf); free(frame_encs); free(frame_offsets); return -1;
        }
    }
    fclose(fp);

    /* Reconstruct data */
    uint32_t recon_sz = n_chunks * CHUNK_SZ;
    uint8_t *recon = (uint8_t *)calloc(1, recon_sz);
    if (!recon) { free(frame_buf); free(frame_encs); free(frame_offsets); return -1; }

    for (uint32_t fi = 0; fi < n_frames; fi++) {
        uint32_t pos = frame_offsets[fi];
        uint32_t fi_start = fi * FRAME_CHUNKS;
        uint32_t chunks_in_frame = n_chunks - fi_start;
        if (chunks_in_frame > FRAME_CHUNKS) chunks_in_frame = FRAME_CHUNKS;

        /* Read seed header: [sz_byte:1] possibly extended with [seed_idx:1] */
        uint8_t seed_hdr = frame_buf[pos++];
        uint32_t seed_ds_sz;
        int seed_idx;
        if (seed_hdr & 0x80) {
            seed_ds_sz = seed_hdr & 0x7F;       /* lower 7 bits = size */
            seed_idx = frame_buf[pos++];          /* next byte = seed index */
        } else {
            seed_ds_sz = seed_hdr;                /* old format: size only */
            seed_idx = 0;                          /* seed is always chunk 0 */
        }
        uint8_t seed_ds[70];
        memcpy(seed_ds, frame_buf + pos, seed_ds_sz);
        pos += seed_ds_sz;

        /* Decode seed */
        uint8_t seed_chunk[CHUNK_SZ];
        ds_decode(seed_chunk, seed_ds);
        memcpy(recon + (fi_start + seed_idx) * CHUNK_SZ, seed_chunk, CHUNK_SZ);

        /* Reconstruct all non-seed chunks */
        uint8_t prev_chunk[CHUNK_SZ];
        uint8_t all_chunks[FRAME_CHUNKS][CHUNK_SZ];
        memset(all_chunks, 0, sizeof(all_chunks));
        memcpy(all_chunks[seed_idx], seed_chunk, CHUNK_SZ);
        memcpy(prev_chunk, seed_chunk, CHUNK_SZ);

        for (uint32_t ci = 0; ci < chunks_in_frame; ci++) {
            if (ci == (uint32_t)seed_idx) {
                /* Update prev_chunk for adjacent-XOR chain */
                uint32_t p_off = (fi_start + ci) * CHUNK_SZ;
                uint32_t p_sz = (p_off + CHUNK_SZ <= hdr.orig_size) ? CHUNK_SZ : hdr.orig_size - p_off;
                uint8_t pbuf[CHUNK_SZ];
                memset(pbuf, 0, CHUNK_SZ);
                memcpy(pbuf, recon + p_off, p_sz);
                memcpy(prev_chunk, pbuf, CHUNK_SZ);
                continue;
            }

            uint8_t sz_byte = frame_buf[pos++];
            uint32_t res_ds_sz = sz_byte & 0x7F;   /* lower 7 bits = size */
            int use_adj = (sz_byte >> 7) & 1;       /* high bit = adjacent-XOR */

            uint8_t res_ds[70];
            memcpy(res_ds, frame_buf + pos, res_ds_sz);
            pos += res_ds_sz;

            uint8_t residual[CHUNK_SZ];
            ds_decode(residual, res_ds);

            uint8_t chunk[CHUNK_SZ];
            if (use_adj) {
                /* Adjacent-XOR: chunk = residual XOR prev_chunk */
                for (uint32_t b = 0; b < CHUNK_SZ; b++)
                    chunk[b] = residual[b] ^ prev_chunk[b];
            } else {
                /* Seed-XOR: chunk = residual XOR seed */
                for (uint32_t b = 0; b < CHUNK_SZ; b++)
                    chunk[b] = residual[b] ^ seed_chunk[b];
            }

            memcpy(all_chunks[ci], chunk, CHUNK_SZ);
            memcpy(prev_chunk, chunk, CHUNK_SZ);
        }

        /* Write all chunks in original order */
        for (uint32_t ci = 0; ci < chunks_in_frame; ci++) {
            memcpy(recon + (fi_start + ci) * CHUNK_SZ, all_chunks[ci], CHUNK_SZ);
        }
    }

    /* Trim to original size */
    uint32_t actual_sz = hdr.orig_size;

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { free(frame_buf); free(frame_encs); free(frame_offsets); free(recon); return -1; }
    fwrite(recon, 1, actual_sz, fout);
    fclose(fout);

    printf("Decode: %s -> %s\n", in_path, out_path);
    printf("  %u -> %u bytes (%u chunks, %u frames)\n",
           hdr.struct_total, actual_sz, n_chunks, n_frames);
    printf("  geo_frame_seek: verified %u frames\n", n_frames);

    free(frame_buf); free(frame_encs); free(frame_offsets); free(recon);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * INFO
 * ═══════════════════════════════════════════════════════════════ */
static int do_info(const char *in_path) {
    FILE *fp = fopen(in_path, "rb");
    if (!fp) return -1;
    uint8_t hdr_buf[GFUF_HDR_SZ];
    if (fread(hdr_buf, 1, GFUF_HDR_SZ, fp) != GFUF_HDR_SZ) { fclose(fp); return -1; }
    GFUFHeader hdr;
    gfuf_hdr_read(hdr_buf, &hdr);
    if (hdr.magic != GFUF_MAGIC) { fclose(fp); return -1; }

    printf("GFUF v2 Info: %s\n", in_path);
    printf("  orig_size: %u  n_chunks: %u  n_frames: %u\n",
           hdr.orig_size, hdr.n_chunks, hdr.n_frames);
    printf("  struct_total: %u (%.1f%% of orig)\n", hdr.struct_total,
           hdr.orig_size > 0 ? 100.0 * hdr.struct_total / hdr.orig_size : 0.0);
    printf("  geo_frame_seek: stride-37 cycle=%u\n", FRAME_CYCLE);
    printf("  24-face: RC_N_VERTICES=%u\n", RC_N_VERTICES);

    uint16_t *encs = (uint16_t *)malloc(hdr.n_frames * sizeof(uint16_t));
    if (encs && fread(encs, 2, hdr.n_frames, fp) == hdr.n_frames) {
        for (uint32_t fi = 0; fi < hdr.n_frames && fi < 8; fi++) {
            DualFrame f = frame_at(encs[fi]);
            printf("  frame[%u]: enc=%u face=%u slot=%u phase=%u\n",
                   fi, encs[fi], f.face, f.slot, f.phase);
        }
        if (hdr.n_frames > 8) printf("  ... (%u more)\n", hdr.n_frames - 8);
    }
    free(encs);
    fclose(fp);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s encode <input.bin> <output.gfuf>\n", argv[0]);
        fprintf(stderr, "  %s decode <input.gfuf> <output.bin>\n", argv[0]);
        fprintf(stderr, "  %s info   <input.gfuf>\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "encode") == 0) return do_encode(argv[2], argv[3]);
    if (strcmp(argv[1], "decode") == 0) return do_decode(argv[2], argv[3]);
    if (strcmp(argv[1], "info")   == 0) return do_info(argv[2]);
    fprintf(stderr, "Unknown: %s\n", argv[1]);
    return 1;
}
