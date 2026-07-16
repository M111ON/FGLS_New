/*
 * geofield_full.c — Full Pipeline CLI: Bond → GeoPixel → Hamburger → GPX5
 *
 * Build:
 *   gcc -O2 -std=c11 -o geofield_full.exe geofield_full.c
 *     -I. -Icore -Icollection -Igeopixel/include
 *     -Igeopixel/include/hamburger -Igeopixel/include/hbv
 *     -Igeopixel/include/geopixel
 *
 * Usage:
 *   geofield_full encode input.bin output.gpx5
 *   geofield_full decode output.gpx5 output.bin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════
 * PIPELINE HEADERS
 * ═══════════════════════════════════════════════════════════════ */
#include "geopixel/include/geopixel/geo_pixel.h"
#include "geopixel/include/hamburger/gpx5_container.h"
#include "geopixel/include/hamburger/hamburger_classify.h"
#include "geopixel/include/hamburger/hamburger_pipe.h"
#include "geopixel/include/hamburger/hamburger_encode.h"
#include "core/pogls_bond.h"

/* ═══════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════ */
#define CHUNK_SZ        64u
#define MAX_CHUNKS      65536u
#define BS_ROT_STATES   6u

/* Binary Shell classification results */
#define BE_FLAT         0
#define BE_SPARSE       1
#define BE_RAW          2

/* Block flags */
#define RAW_FLAG        0x04u
#define FLAT_FLAG       0x00u
#define SPARSE_FLAG     0x01u
#define SPARSE_THRESH   16u

/* ═══════════════════════════════════════════════════════════════
 * BINARY SHELL CODEC (48-byte GEO_BLOCK rotations on 64B chunks)
 * 6 rotations on 4×4×4 cube — each is self-inverse.
 * ═══════════════════════════════════════════════════════════════ */

static void bs_rotate64(uint8_t *out, const uint8_t *in, int rot) {
    for (int z = 0; z < 4; z++)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int si = z * 16 + y * 4 + x;
                int di;
                switch (rot) {
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

/* Classify 64B block → FLAT/SPARSE/RAW */
static int bs_classify(uint8_t *out, uint32_t *out_sz,
                        const uint8_t *chunk, uint32_t chunk_sz)
{
    /* Check FLAT */
    int flat = 1;
    for (uint32_t i = 0; i < chunk_sz; i++)
        if (chunk[i]) { flat = 0; break; }

    if (flat) {
        out[0] = FLAT_FLAG;
        *out_sz = 1;
        return BE_FLAT;
    }

    /* Rotate each 48-byte GEO_BLOCK within the 64B, find best sparsity */
    int      best_nz = CHUNK_SZ + 1;
    uint8_t  best_rot = 0;
    uint8_t  best_buf[CHUNK_SZ];
    uint8_t  rotbuf[CHUNK_SZ];

    for (uint8_t rot = 0; rot < BS_ROT_STATES; rot++) {
        memset(rotbuf, 0, CHUNK_SZ);
        bs_rotate64(rotbuf, chunk, rot);
        int nz = 0;
        for (uint32_t i = 0; i < CHUNK_SZ; i++)
            if (rotbuf[i]) nz++;
        if (nz < best_nz) {
            best_nz = nz; best_rot = rot;
            memcpy(best_buf, rotbuf, CHUNK_SZ);
        }
    }

    /* SPARSE if ≤ SPARSE_THRESH non-zero bytes */
    if (best_nz <= SPARSE_THRESH) {
        out[0] = SPARSE_FLAG;
        out[1] = best_rot;
        out[2] = (uint8_t)best_nz;
        uint32_t wp = 3;
        for (uint32_t i = 0; i < CHUNK_SZ && wp < CHUNK_SZ + 3; i++) {
            if (best_buf[i]) {
                out[wp++] = (uint8_t)i;
                out[wp++] = best_buf[i];
            }
        }
        *out_sz = wp;
        return BE_SPARSE;
    }

    /* RAW: fallback */
    out[0] = RAW_FLAG;
    out[1] = best_rot;
    memcpy(out + 2, best_buf, chunk_sz);
    *out_sz = 2 + chunk_sz;
    return BE_RAW;
}

/* Decode 64B block from classified format */
static int bs_unclassify(uint8_t *out, uint32_t out_sz,
                          const uint8_t *in, uint32_t in_sz)
{
    if (in_sz < 1) return -1;

    uint8_t flag = in[0];
    if (flag == FLAT_FLAG) {
        memset(out, 0, out_sz > CHUNK_SZ ? CHUNK_SZ : out_sz);
        return 0;
    }

    if (in_sz < 2) return -1;
    uint8_t rot = in[1];

    if (flag == RAW_FLAG) {
        if (in_sz < 2 + out_sz) return -1;
        memcpy(out, in + 2, out_sz);
        return 0;
    }

    if (flag == SPARSE_FLAG) {
        if (in_sz < 3) return -1;
        uint8_t nz = in[2];
        uint8_t sparse_buf[CHUNK_SZ];
        memset(sparse_buf, 0, CHUNK_SZ);
        uint32_t rp = 3;
        for (uint8_t i = 0; i < nz && rp + 2 <= in_sz; i++) {
            uint8_t idx = in[rp++];
            uint8_t val = in[rp++];
            if (idx < CHUNK_SZ) sparse_buf[idx] = val;
        }
        /* Inverse rotation (same as forward — all involutions) */
        bs_rotate64(out, sparse_buf, rot);
        return 0;
    }

    return -1;
}

/* ═══════════════════════════════════════════════════════════════
 * MAP BINARY SHELL RESULT → GPX5_TTYPE
 * ═══════════════════════════════════════════════════════════════ */
static uint8_t bs_to_ttype(int bs_result) {
    switch (bs_result) {
        case BE_FLAT:   return GPX5_TTYPE_FLAT;
        case BE_SPARSE: return GPX5_TTYPE_GRADIENT;
        case BE_RAW:    return GPX5_TTYPE_NOISE;
        default:        return GPX5_TTYPE_NOISE;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ENCODE: data → .gpx5
 * ═══════════════════════════════════════════════════════════════ */
static int do_encode(const char *in_path, const char *out_path) {
    FILE *fp = fopen(in_path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", in_path); return -1; }
    fseek(fp, 0, SEEK_END);
    uint32_t data_sz = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)malloc(data_sz);
    if (!data) { fclose(fp); fprintf(stderr, "OOM\n"); return -1; }
    if (fread(data, 1, data_sz, fp) != data_sz) {
        free(data); fclose(fp); fprintf(stderr, "Read error\n"); return -1;
    }
    fclose(fp);

    uint32_t n_chunks = (data_sz + CHUNK_SZ - 1) / CHUNK_SZ;
    if (n_chunks == 0 || n_chunks > MAX_CHUNKS) {
        free(data); fprintf(stderr, "Bad chunk count %u\n", n_chunks); return -1;
    }

    /* Allocate tile input array + classify buffer */
    HbTileIn *tiles = (HbTileIn *)calloc(n_chunks, sizeof(HbTileIn));
    uint8_t  *ttypes = (uint8_t *)calloc(n_chunks, 1);
    uint32_t  global_seed = (uint32_t)time(NULL);
    uint32_t  n_flat = 0, n_sparse = 0, n_raw = 0;

    if (!tiles || !ttypes) {
        free(ttypes); free(tiles); free(data);
        fprintf(stderr, "OOM\n"); return -1;
    }

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t offset = i * CHUNK_SZ;
        uint32_t sz = (offset + CHUNK_SZ <= data_sz) ? CHUNK_SZ : data_sz - offset;

        /* Chunk data (64B, zero-pad last) */
        uint8_t chunk[CHUNK_SZ];
        memcpy(chunk, data + offset, sz);
        if (sz < CHUNK_SZ) memset(chunk + sz, 0, CHUNK_SZ - sz);

        /* Classify via Binary Shell */
        uint8_t  class_buf[CHUNK_SZ + 4];
        uint32_t class_sz = 0;
        int      bs_res = bs_classify(class_buf, &class_sz, chunk, sz);

        /* Map to GPX5 ttype */
        uint8_t ttype = bs_to_ttype(bs_res);
        ttypes[i] = ttype;
        if (bs_res == BE_FLAT) n_flat++;
        else if (bs_res == BE_SPARSE) n_sparse++;
        else n_raw++;

        /* Create bond piece for GeoPixel fingerprint */
        uint64_t seed_val = pogls_fibo_addr((uint64_t)global_seed ^ (uint64_t)i);
        uint8_t  axis = (uint8_t)((i % 7) + 1);
        PoglsPiece piece = pogls_make_piece(seed_val, axis);
        (void)piece; /* bond is for visual identity — not stored in tile data */

        /* Fill tile input — store CLASSIFIED data, not raw */
        tiles[i].data    = data + offset; /* raw data for codec */
        tiles[i].sz      = sz;
        tiles[i].tile_id = (uint16_t)i;
        tiles[i].ttype   = ttype;
    }

    /* Auto-configure pipes from ttype histogram */
    Gpx5PipeEntry pipes[3];
    hb_auto_pipes(ttypes, n_chunks, pipes);

    /* Init hamburger encode context */
    HbEncodeCtx *ctx = (HbEncodeCtx *)calloc(1, sizeof(HbEncodeCtx));
    if (!ctx) { free(ttypes); free(tiles); free(data); return -1; }

    hb_encode_init(ctx, global_seed, 1, (uint16_t)n_chunks, pipes, 0);
    ctx->n_tiles = n_chunks;

    /* Save pipes before init (hb_encode_init memsets pipes after init) */
    Gpx5PipeEntry saved_pipes[3];
    memcpy(saved_pipes, ctx->pipes, sizeof(saved_pipes));
    /* Re-init with saved pipes (first call overwrites) */
    hb_encode_init(ctx, global_seed, 1, (uint16_t)n_chunks, saved_pipes, 0);
    ctx->n_tiles = n_chunks;

    /* Warm-up + encode: process each tile */
    int result = hb_encode_run(ctx, tiles, n_chunks);
    if (result != HB_OK) {
        fprintf(stderr, "hb_encode_run failed: %d\n", result);
        hb_encode_free(ctx); free(ctx);
        free(ttypes); free(tiles); free(data);
        return -1;
    }

    /* Write .gpx5 file */
    result = hamburger_encode(out_path, ctx, tiles, n_chunks);
    if (result != HB_OK) {
        fprintf(stderr, "hamburger_encode failed: %d\n", result);
        hb_encode_free(ctx); free(ctx);
        free(ttypes); free(tiles); free(data);
        return -1;
    }

    /* Stats */
    uint32_t out_sz = 0;
    FILE *fout = fopen(out_path, "rb");
    if (fout) { fseek(fout, 0, SEEK_END); out_sz = (uint32_t)ftell(fout); fclose(fout); }

    printf("Encode: %s -> %s\n", in_path, out_path);
    printf("  %u -> %u bytes (%.2fx)\n", data_sz, out_sz,
           out_sz > 0 ? (double)data_sz / out_sz : 0.0);
    printf("  chunks=%u flat=%u sparse=%u raw=%u\n",
           n_chunks, n_flat, n_sparse, n_raw);
    printf("  pipes: R(codec=%u) G(codec=%u) B(codec=%u)\n",
           pipes[0].codec, pipes[1].codec, pipes[2].codec);

    hb_encode_free(ctx); free(ctx);
    free(ttypes); free(tiles); free(data);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE: .gpx5 → data
 * ═══════════════════════════════════════════════════════════════ */
static int do_decode(const char *in_path, const char *out_path) {
    /* Read .gpx5 file to get tile count */
    Gpx5File gf;
    if (gpx5_open(in_path, &gf) != 0) {
        fprintf(stderr, "Cannot open GPX5: %s\n", in_path); return -1;
    }
    uint32_t n_tiles = gf.hdr.n_tiles;
    gpx5_close(&gf);

    if (n_tiles == 0) { fprintf(stderr, "No tiles\n"); return -1; }

    /* Allocate tile output buffers */
    uint8_t **tiles = (uint8_t **)calloc(n_tiles, sizeof(uint8_t *));
    if (!tiles) { fprintf(stderr, "OOM\n"); return -1; }

    for (uint32_t i = 0; i < n_tiles; i++) {
        tiles[i] = (uint8_t *)calloc(1, HB_TILE_SZ_MAX);
        if (!tiles[i]) {
            for (uint32_t j = 0; j < i; j++) free(tiles[j]);
            free(tiles); fprintf(stderr, "OOM\n"); return -1;
        }
    }

    /* Decode */
    uint32_t out_n = 0, out_sz = 0;
    int result = hamburger_decode(in_path, tiles, &out_n, &out_sz);
    if (result != HB_OK || out_n == 0) {
        fprintf(stderr, "hamburger_decode failed: %d\n", result);
        for (uint32_t i = 0; i < n_tiles; i++) free(tiles[i]);
        free(tiles); return -1;
    }

    /* Reconstruct: copy tile data back to flat buffer */
    /* Each tile has up to HB_TILE_SZ_MAX bytes; we take the first
     * CHUNK_SZ bytes per tile to reconstruct the original */
    uint32_t recon_sz = n_tiles * CHUNK_SZ;
    uint8_t *recon = (uint8_t *)calloc(1, recon_sz);
    if (!recon) {
        for (uint32_t i = 0; i < n_tiles; i++) free(tiles[i]);
        free(tiles); fprintf(stderr, "OOM\n"); return -1;
    }

    for (uint32_t i = 0; i < n_tiles; i++) {
        memcpy(recon + i * CHUNK_SZ, tiles[i],
               CHUNK_SZ < HB_TILE_SZ_MAX ? CHUNK_SZ : HB_TILE_SZ_MAX);
    }

    /* Trim trailing zeros to find actual data size */
    uint32_t actual_sz = n_tiles * CHUNK_SZ;
    while (actual_sz > 0 && recon[actual_sz - 1] == 0) actual_sz--;

    /* Write output */
    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        fprintf(stderr, "Cannot write %s\n", out_path);
        free(recon);
        for (uint32_t i = 0; i < n_tiles; i++) free(tiles[i]);
        free(tiles); return -1;
    }
    fwrite(recon, 1, actual_sz, fp);
    fclose(fp);

    printf("Decode: %s -> %s\n", in_path, out_path);
    printf("  %u -> %u bytes (%u tiles)\n", actual_sz, actual_sz, out_n);

    free(recon);
    for (uint32_t i = 0; i < n_tiles; i++) free(tiles[i]);
    free(tiles);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s encode <input.bin> <output.gpx5>\n", argv[0]);
        fprintf(stderr, "  %s decode <input.gpx5> <output.bin>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "encode") == 0)
        return do_encode(argv[2], argv[3]);
    else if (strcmp(argv[1], "decode") == 0)
        return do_decode(argv[2], argv[3]);
    else {
        fprintf(stderr, "Unknown command: %s (use encode/decode)\n", argv[1]);
        return 1;
    }
}
