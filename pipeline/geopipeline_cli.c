/*
 * geopipeline_cli.c — GeoPixel Pipeline with geo_frame_seek + HBHF Header
 *
 * Pipeline: input → 64B chunks → L-Block (Metatron/Hilbert)
 *           → Diamond Shell classify (fold_fibo_intersect)
 *           → Bond identity → HbTileIn (tile 0 = HBHF header)
 *           → geo_frame_seek 1440-frame timeline
 *           → hamburger encode (seed + invert chain) → GPX5
 *
 * Key Architecture:
 *   - Tile 0 = HbHeaderFrame (64B directory, ttype=0xF0)
 *   - Tiles 1..N = data chunks
 *   - geo_frame_seek provides deterministic 1440-frame stride-37 walk
 *   - hamburger warm-up (1440 ticks) = one full frame cycle
 *   - Invert chain stores residuals; seed + header → full reconstruction
 *   - HB_INVERT_MAX_TILES=4096 → multi-cycle for >4096 tiles
 *
 * Build:
 *   gcc -O2 -std=c11 -o geopipeline_cli.exe geopipeline_cli.c
 *     -I. -Icore -Icollection -Igeopixel/include
 *     -Igeopixel/include/hamburger -Igeopixel/include/hbv
 *     -Igeopixel/include/geopixel
 *     -Icore/pogls_engine/twin_core
 *     -Irunner/pogls_hilbert_container
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ── Pipeline headers ─────────────────────────────────── */
#define GEO_JUMP_INLINE
#include "geo_jump.h"
#include "geo_frame_seek.h"
#include "pogls_fold.h"
#include "pogls_hc_geojump.h"
#include "geopixel/include/geopixel/geo_pixel.h"
#include "geopixel/include/hamburger/gpx5_container.h"
#include "geopixel/include/hamburger/hamburger_classify.h"
#include "geopixel/include/hamburger/hamburger_pipe.h"
#include "geopixel/include/hamburger/hamburger_encode.h"
#include "collection/geopixel/hb_header_frame.h"
#include "core/pogls_bond.h"

/* ── Constants ────────────────────────────────────────── */
#define CHUNK_SZ        64u
#define MAX_CHUNKS      65536u
#define TILE_CYCLE_MAX  4096u   /* HB_INVERT_MAX_TILES limit */

#define DIA_FLAG_FLAT     0u
#define DIA_FLAG_SPARSE   1u
#define DIA_FLAG_DENSE    2u
#define DIA_SPARSE_THRESH 4u
#define DIA_ROT_STATES    6u

#define FIBO_STRIDE       37u

/* ── Diamond Shell (fold_fibo_intersect) ──────────────── */

static void dia_rotate64(uint8_t out[64], const uint8_t in[64], uint8_t rot) {
    for (uint8_t z = 0; z < 4; z++)
        for (uint8_t y = 0; y < 4; y++)
            for (uint8_t x = 0; x < 4; x++) {
                uint8_t sx, sy, sz;
                switch (rot % DIA_ROT_STATES) {
                    case 0: sx=x; sy=y; sz=z; break;
                    case 1: sx=y; sy=z; sz=x; break;
                    case 2: sx=z; sy=x; sz=y; break;
                    case 3: sx=x; sy=z; sz=3-y; break;
                    case 4: sx=z; sy=y; sz=3-x; break;
                    case 5: sx=3-y; sy=x; sz=z; break;
                }
                out[z*16 + y*4 + x] = in[sz*16 + sy*4 + sx];
            }
}

static void dia_inv_rotate64(uint8_t out[64], const uint8_t in[64], uint8_t rot) {
    uint8_t tmp[64] = {0};
    for (uint8_t z = 0; z < 4; z++)
        for (uint8_t y = 0; y < 4; y++)
            for (uint8_t x = 0; x < 4; x++) {
                uint8_t sx, sy, sz;
                switch (rot % DIA_ROT_STATES) {
                    case 0: sx=x; sy=y; sz=z; break;
                    case 1: sx=y; sy=z; sz=x; break;
                    case 2: sx=z; sy=x; sz=y; break;
                    case 3: sx=x; sy=z; sz=3-y; break;
                    case 4: sx=z; sy=y; sz=3-x; break;
                    case 5: sx=3-y; sy=x; sz=z; break;
                }
                tmp[sz*16 + sy*4 + sx] = in[z*16 + y*4 + x];
            }
    memcpy(out, tmp, 64);
}

static DiamondBlock dia_chunk_to_block(const uint8_t rotbuf[64],
                                        uint8_t rot, uint32_t chunk_z)
{
    DiamondBlock db;
    memset(&db, 0, sizeof(db));
    memcpy(&db.core.raw, rotbuf, 8);
    db.invert = ~db.core.raw;
    uint8_t face_id   = rot & 0x1F;
    uint8_t engine_id = (uint8_t)(chunk_z & 0x7F);
    uint32_t vpos = ((uint32_t)rotbuf[8] << 16)
                  | ((uint32_t)rotbuf[9] << 8)
                  | (uint32_t)rotbuf[10];
    vpos &= 0xFFFFFF;
    uint8_t quad_flags = (uint8_t)(rotbuf[16] ^ rotbuf[32]);
    db.core.raw = ((uint64_t)(face_id    & 0x1F) << 59)
                | ((uint64_t)(engine_id  & 0x7F) << 52)
                | ((uint64_t)(vpos & 0xFFFFFF)    << 28)
                | ((uint64_t)(1u & 0x0F)          << 24)
                | ((uint64_t)(quad_flags & 0xFF)  << 16)
                | (db.core.raw & 0xFFFF);
    db.invert = ~db.core.raw;
    fold_build_quad_mirror(&db);
    return db;
}

static uint8_t dia_classify(const uint8_t chunk[64],
                              uint8_t *out_flag,
                              uint8_t  rotbuf_out[64])
{
    int all_zero = 1;
    for (uint32_t i = 0; i < CHUNK_SZ; i++)
        if (chunk[i]) { all_zero = 0; break; }
    if (all_zero) {
        *out_flag = DIA_FLAG_FLAT;
        memset(rotbuf_out, 0, CHUNK_SZ);
        return 0;
    }
    uint8_t best_rot = 0, best_buf[64];
    int best_pc = -1;
    for (uint8_t rot = 0; rot < DIA_ROT_STATES; rot++) {
        uint8_t rotbuf[64];
        dia_rotate64(rotbuf, chunk, rot);
        DiamondBlock db = dia_chunk_to_block(rotbuf, rot, 0);
        if ((db.core.raw ^ db.invert) != 0xFFFFFFFFFFFFFFFFull) {
            db.invert = ~db.core.raw;
            fold_build_quad_mirror(&db);
        }
        uint64_t isect = fold_fibo_intersect(&db);
        int pc = __builtin_popcountll(isect);
        if (pc > best_pc) { best_pc = pc; best_rot = rot; memcpy(best_buf, rotbuf, CHUNK_SZ); }
    }
    *out_flag = (best_pc <= DIA_SPARSE_THRESH) ? DIA_FLAG_SPARSE : DIA_FLAG_DENSE;
    memcpy(rotbuf_out, best_buf, CHUNK_SZ);
    return best_rot;
}

static uint8_t dia_to_ttype(uint8_t dia_flag) {
    switch (dia_flag) {
        case DIA_FLAG_FLAT:   return GPX5_TTYPE_FLAT;
        case DIA_FLAG_SPARSE: return GPX5_TTYPE_GRADIENT;
        case DIA_FLAG_DENSE:  return GPX5_TTYPE_EDGE;
        default:              return GPX5_TTYPE_NOISE;
    }
}

/* ── L-Block Container → node_id mapping ─────────────── */

static uint32_t g_lblock_block_count = 0;
static uint32_t g_lblock_tower_id = 0;

/* ── xxh64 ────────────────────────────────────────────── */

static uint64_t xxh64(const uint8_t *data, size_t len) {
    const uint64_t P1 = 0x9E3779B185EBCA87ULL, P2 = 0x14DEF9DEA2F79CD6ULL;
    const uint64_t P3 = 0x165667B19E3779F9ULL, P4 = 0x85EBCA77C2B2ED6BULL;
    const uint64_t P5 = 0x27D4EB2F165667C5ULL;
    uint64_t v1 = P5 + 8, v2 = P4, v3 = 0, v4 = P1;
    size_t off = 0;
    while (off + 32 <= len) {
        const uint64_t *p = (const uint64_t *)(data + off);
        v1 = ((v1 + p[0] * P2) >> 31) * P1;
        v2 = ((v2 + p[1] * P2) >> 31) * P1;
        v3 = ((v3 + p[2] * P2) >> 31) * P1;
        v4 = ((v4 + p[3] * P2) >> 31) * P1;
        off += 32;
    }
    uint64_t result = len;
    if (off < len) {
        uint64_t buf[4] = {0};
        memcpy(buf, data + off, len - off);
        v1 += buf[0] * P2; v1 = ((v1 >> 31) * P1);
        v2 += buf[1] * P2; v2 = ((v2 >> 31) * P1);
        v3 += buf[2] * P2; v3 = ((v3 >> 31) * P1);
        v4 += buf[3] * P2; v4 = ((v4 >> 31) * P1);
    }
    result = (v1 << 1) + (v2 << 7) + (v3 << 12) + (v4 << 18);
    result = ((result ^ (v1 >> 33)) * P2) + P3;
    result = ((result ^ (v2 >> 29)) * P3) + P4;
    result = ((result ^ (v3 >> 32)) * P4) + P5;
    return result;
}

/* ── File I/O ─────────────────────────────────────────── */

static uint8_t *read_file(const char *path, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_sz = (size_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t sz) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot write %s\n", path); return -1; }
    fwrite(data, 1, sz, f);
    fclose(f);
    return 0;
}

/* ── ENCODE ───────────────────────────────────────────── */

static int do_encode(const char *in_path, const char *out_path) {
    size_t data_sz;
    uint8_t *data = read_file(in_path, &data_sz);
    if (!data) return -1;

    uint32_t n_chunks = (uint32_t)((data_sz + CHUNK_SZ - 1) / CHUNK_SZ);
    if (n_chunks == 0 || n_chunks > MAX_CHUNKS) {
        fprintf(stderr, "Bad chunk count %u (max %u)\n", n_chunks, MAX_CHUNKS);
        free(data); return -1;
    }

    /* Step 1: L-Block Container — Metatron arrangement, Hilbert ordering
     * Collect node_ids for Bond identity. Tiles stay in original order
     * (tile_id = sequential) so hamburger_decode seed derivation matches. */
    uint32_t block_count = (n_chunks + GEO_BLOCK - 1) / GEO_BLOCK;
    if (block_count > 144) block_count = 144;
    uint32_t tower_id = 0;

    HC_GeoJumpWriter lw;
    if (hc_gj_writer_init(&lw, block_count, tower_id) != 0) {
        fprintf(stderr, "L-Block init failed\n"); free(data); return -1;
    }

    uint32_t *node_ids = (uint32_t *)malloc(n_chunks * sizeof(uint32_t));
    if (!node_ids) { hc_gj_writer_destroy(&lw); free(data); fprintf(stderr, "OOM\n"); return -1; }

    for (uint32_t i = 0; i < n_chunks; i++) {
        node_ids[i] = hc_gj_writer_node(&lw);
        uint8_t chunk[CHUNK_SZ] = {0};
        uint32_t sz = (i * CHUNK_SZ + CHUNK_SZ <= data_sz) ? CHUNK_SZ : (uint32_t)(data_sz - i * CHUNK_SZ);
        memcpy(chunk, data + i * CHUNK_SZ, sz);
        hc_gj_write_cell(&lw, chunk);
    }

    g_lblock_block_count = block_count;
    g_lblock_tower_id = tower_id;

    /* Step 2: Diamond Shell classify + build tile array
     * Tile 0 = HBHF header frame (64B, ttype=0xF0)
     * Tiles 1..n_chunks = data chunks */
    uint32_t n_tiles = n_chunks + 1;  // +1 for header
    HbTileIn *tiles = (HbTileIn *)calloc(n_tiles, sizeof(HbTileIn));
    uint8_t  *ttypes = (uint8_t *)calloc(n_tiles, 1);
    if (!tiles || !ttypes) {
        free(ttypes); free(tiles); free(node_ids);
        hc_gj_writer_destroy(&lw); free(data); fprintf(stderr, "OOM\n"); return -1;
    }

    uint32_t global_seed = (uint32_t)time(NULL);
    uint32_t n_flat = 0, n_sparse = 0, n_dense = 0;

    /* Tile 0: HBHF header frame */
    HbHeaderFrame hf;
    memset(&hf, 0, sizeof(hf));
    hf.magic        = HBHF_MAGIC;
    hf.version      = HBHF_VERSION;
    hf.n_cycles     = (uint8_t)((n_tiles + TILE_CYCLE_MAX - 1) / TILE_CYCLE_MAX);
    hf.n_layers     = 1u;  /* single-layer binary data */
    hf.total_tiles  = n_tiles;
    hf.img_w        = 0;   /* not an image */
    hf.img_h        = 0;
    hf.tile_w       = 8;   /* 64B = 8x8 conceptual */
    hf.tile_h       = 8;
    hf.global_seed  = global_seed;
    hf.tick_period  = FRAME_CYCLE;  /* 1440 */
    hf.layer_stride = CHUNK_SZ;
    for (int s = 0; s < 8; s++) hf.codec_map[s] = GPX5_CODEC_RAW;

    uint8_t hf_buf[HBHF_SZ];
    hbhf_write(hf_buf, &hf);

    tiles[0].data    = hf_buf;
    tiles[0].sz      = HBHF_SZ;
    tiles[0].tile_id = 0;
    tiles[0].ttype   = GPX5_TTYPE_DIRECTORY;  /* 0xF0 — marks directory tile */
    ttypes[0]        = GPX5_TTYPE_DIRECTORY;
    n_flat++;  /* header is flat */

    /* Tiles 1..n_chunks: data chunks */
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t offset = i * CHUNK_SZ;
        uint32_t sz = (offset + CHUNK_SZ <= data_sz) ? CHUNK_SZ : (uint32_t)(data_sz - offset);

        uint8_t dia_flag, rotbuf[64];
        dia_classify(data + offset, &dia_flag, rotbuf);
        uint8_t ttype = dia_to_ttype(dia_flag);
        ttypes[i + 1] = ttype;
        if (dia_flag == DIA_FLAG_FLAT) n_flat++;
        else if (dia_flag == DIA_FLAG_SPARSE) n_sparse++;
        else n_dense++;

        /* Bond identity — use node_id from L-Block */
        uint64_t seed_val = pogls_fibo_addr((uint64_t)node_ids[i]);
        uint8_t  axis = (uint8_t)((node_ids[i] % 7) + 1);
        PoglsPiece piece = pogls_make_piece(seed_val, axis);
        (void)piece;

        tiles[i + 1].data    = data + offset;
        tiles[i + 1].sz      = sz;
        tiles[i + 1].tile_id = (uint16_t)(i + 1);
        tiles[i + 1].ttype   = ttype;
    }

    /* Step 3: geo_frame_seek — verify deterministic timeline */
    if (geo_frame_seek_verify() != 0) {
        fprintf(stderr, "Warning: geo_frame_seek_verify failed\n");
    }

    /* Step 4: auto-configure pipes from ttype histogram */
    Gpx5PipeEntry pipes[3];
    hb_auto_pipes(ttypes, n_tiles, pipes);

    /* Force header tile (tile 0) carrier to RAW so its 64B HBHF is preserved exactly.
     * Compute which carrier tile 0 maps to, then override that pipe's codec. */
    {
        uint32_t seed_local  = gpx5_seed_local(global_seed, 0);
        uint16_t hilbert_pos = gpx5_hilbert_entry(seed_local, 0);
        HbDispatch disp = hb_dispatch(hilbert_pos, pipes);
        pipes[disp.carrier_id].codec = GPX5_CODEC_RAW;
    }

    /* Step 5: encode in cycles of TILE_CYCLE_MAX */
    uint32_t n_cycles = (n_tiles + TILE_CYCLE_MAX - 1) / TILE_CYCLE_MAX;
    int result = HB_OK;

    for (uint32_t cyc = 0; cyc < n_cycles && result == HB_OK; cyc++) {
        uint32_t ts = cyc * TILE_CYCLE_MAX;
        uint32_t te = ts + TILE_CYCLE_MAX;
        if (te > n_tiles) te = n_tiles;
        uint32_t ct = te - ts;

        char cyc_path[512];
        if (n_cycles == 1) {
            snprintf(cyc_path, sizeof(cyc_path), "%s", out_path);
        } else {
            const char *dot = strrchr(out_path, '.');
            if (dot) {
                size_t bl = (size_t)(dot - out_path);
                snprintf(cyc_path, sizeof(cyc_path), "%.*s_c%03u%s", (int)bl, out_path, cyc, dot);
            } else {
                snprintf(cyc_path, sizeof(cyc_path), "%s_c%03u", out_path, cyc);
            }
        }

        Gpx5PipeEntry cp[3];
        hb_auto_pipes(ttypes + ts, ct, cp);

        if (getenv("GEOPPIPE_LOSSLESS")) {
            for (int p = 0; p < 3; p++) cp[p].codec = GPX5_CODEC_RAW;
            if (cyc == 0) printf("  [lossless mode: cycle %u pipes forced to RAW]\n", cyc);
        }

        /* Remap tile_ids to cycle-local (0..ct-1) for invert recorder */
        uint16_t saved_ids[4096];
        for (uint32_t j = 0; j < ct && j < TILE_CYCLE_MAX; j++) {
            saved_ids[j] = tiles[ts + j].tile_id;
            tiles[ts + j].tile_id = (uint16_t)j;
        }

        HbEncodeCtx *ctx = (HbEncodeCtx *)calloc(1, sizeof(HbEncodeCtx));
        if (!ctx) {
            for (uint32_t j = 0; j < ct && j < TILE_CYCLE_MAX; j++)
                tiles[ts + j].tile_id = saved_ids[j];
            result = HB_ERR_ALLOC; break;
        }

        hb_encode_init(ctx, global_seed, 1, (uint16_t)ct, cp, 0);
        ctx->n_tiles = ct;

        result = hb_encode_run(ctx, tiles + ts, ct);
        if (result == HB_OK) {
            result = hamburger_encode(cyc_path, ctx, tiles + ts, ct);
        }
        hb_encode_free(ctx); free(ctx);

        /* Restore original tile_ids */
        for (uint32_t j = 0; j < ct && j < TILE_CYCLE_MAX; j++)
            tiles[ts + j].tile_id = saved_ids[j];

        if (result != HB_OK) {
            fprintf(stderr, "Cycle %u encode failed: %d\n", cyc, result);
        }
    }

    /* Stats */
    uint32_t out_sz = 0;
    FILE *fout = fopen(out_path, "rb");
    if (fout) { fseek(fout, 0, SEEK_END); out_sz = (uint32_t)ftell(fout); fclose(fout); }

    printf("Encode: %s -> %s\n", in_path, out_path);
    printf("  %zu -> %u bytes (%.2fx)\n", data_sz, out_sz,
           out_sz > 0 ? (double)data_sz / out_sz : 0.0);
    printf("  chunks=%u tiles=%u (header+data) flat=%u sparse=%u dense=%u\n",
           n_chunks, n_tiles, n_flat, n_sparse, n_dense);
    printf("  L-Block: %u blocks, %u cells/block, tower=%u\n",
           block_count, GEO_BLOCK, tower_id);
    printf("  geo_frame_seek: stride-37 cycle=%d\n", FRAME_CYCLE);
    printf("  Diamond Shell: fold_fibo_intersect popcount discriminator\n");
    printf("  pipes: R(codec=%u) G(codec=%u) B(codec=%u)\n",
           pipes[0].codec, pipes[1].codec, pipes[2].codec);
    printf("  cycles: %u (max %u tiles/cycle)\n", n_cycles, TILE_CYCLE_MAX);

    free(node_ids); free(ttypes); free(tiles);
    hc_gj_writer_destroy(&lw); free(data);
    return (result == HB_OK) ? 0 : -1;
}

/* ── DECODE ───────────────────────────────────────────── */

static int do_decode(const char *in_path, const char *out_path) {
    uint32_t total_tiles = 0;
    uint8_t **all_tiles = NULL;
    uint32_t alloc_cap = 0;

    for (uint32_t cyc = 0; ; cyc++) {
        char cyc_path[512];
        if (cyc == 0) {
            /* Try _c000 first (multi-cycle), then base path */
            const char *dot = strrchr(in_path, '.');
            if (dot) {
                size_t bl = (size_t)(dot - in_path);
                snprintf(cyc_path, sizeof(cyc_path), "%.*s_c%03u%s", (int)bl, in_path, cyc, dot);
            } else {
                snprintf(cyc_path, sizeof(cyc_path), "%s_c%03u", in_path, cyc);
            }
            FILE *fp = fopen(cyc_path, "rb");
            if (!fp) {
                fp = fopen(in_path, "rb");
                if (fp) {
                    fclose(fp);
                    snprintf(cyc_path, sizeof(cyc_path), "%s", in_path);
                }
            } else {
                fclose(fp);
            }
        } else {
            const char *dot = strrchr(in_path, '.');
            if (dot) {
                size_t bl = (size_t)(dot - in_path);
                snprintf(cyc_path, sizeof(cyc_path), "%.*s_c%03u%s", (int)bl, in_path, cyc, dot);
            } else {
                snprintf(cyc_path, sizeof(cyc_path), "%s_c%03u", in_path, cyc);
            }
        }

        FILE *fp = fopen(cyc_path, "rb");
        if (!fp) break;
        fclose(fp);

        Gpx5File gf;
        if (gpx5_open(cyc_path, &gf) != 0) break;
        uint32_t n_tiles = gf.hdr.n_tiles;
        gpx5_close(&gf);
        if (n_tiles == 0) break;

        uint32_t need = total_tiles + n_tiles;
        if (need > alloc_cap) {
            alloc_cap = need + 1024;
            all_tiles = (uint8_t **)realloc(all_tiles, alloc_cap * sizeof(uint8_t *));
            if (!all_tiles) { fprintf(stderr, "OOM\n"); return -1; }
        }
        for (uint32_t i = 0; i < n_tiles; i++) {
            all_tiles[total_tiles + i] = (uint8_t *)calloc(1, HB_TILE_SZ_MAX);
            if (!all_tiles[total_tiles + i]) {
                for (uint32_t j = 0; j < total_tiles + i; j++) free(all_tiles[j]);
                free(all_tiles); fprintf(stderr, "OOM\n"); return -1;
            }
        }

        uint32_t out_n = 0, out_sz = 0;
        int r = hamburger_decode(cyc_path, all_tiles + total_tiles, &out_n, &out_sz);
        if (r != HB_OK || out_n == 0) {
            fprintf(stderr, "hamburger_decode cycle %u failed: %d\n", cyc, r);
            for (uint32_t i = 0; i < total_tiles + n_tiles; i++) free(all_tiles[i]);
            free(all_tiles); return -1;
        }

        printf("  cycle %u: %u tiles decoded from %s\n", cyc, out_n, cyc_path);
        total_tiles += out_n;
    }

    if (total_tiles == 0) {
        fprintf(stderr, "No tiles decoded\n");
        if (all_tiles) free(all_tiles);
        return -1;
    }

    /* Tile 0 = HBHF header — extract metadata */
    HbHeaderFrame hf;
    if (hbhf_read(all_tiles[0], &hf) != 0) {
        fprintf(stderr, "Warning: failed to read HBHF from tile 0\n");
    } else {
        printf("  HBHF: cycles=%u layers=%u tiles=%u seed=0x%08x\n",
               hf.n_cycles, hf.n_layers, hf.total_tiles, hf.global_seed);
    }

    /* Reconstruct: data starts from tile 1 (tile 0 is header) */
    uint32_t data_tiles = (total_tiles > 1) ? total_tiles - 1 : 0;
    uint32_t recon_sz = data_tiles * CHUNK_SZ;
    uint8_t *recon = (uint8_t *)calloc(1, recon_sz);
    if (!recon) {
        for (uint32_t i = 0; i < total_tiles; i++) free(all_tiles[i]);
        free(all_tiles); fprintf(stderr, "OOM\n"); return -1;
    }

    for (uint32_t i = 0; i < data_tiles; i++) {
        memcpy(recon + i * CHUNK_SZ, all_tiles[i + 1],
               CHUNK_SZ < HB_TILE_SZ_MAX ? CHUNK_SZ : HB_TILE_SZ_MAX);
    }

    if (write_file(out_path, recon, (size_t)recon_sz) != 0) {
        free(recon);
        for (uint32_t i = 0; i < total_tiles; i++) free(all_tiles[i]);
        free(all_tiles); return -1;
    }

    printf("Decode: %s -> %s\n", in_path, out_path);
    printf("  %u bytes (%u data tiles, +1 header)\n", recon_sz, data_tiles);

    free(recon);
    for (uint32_t i = 0; i < total_tiles; i++) free(all_tiles[i]);
    free(all_tiles);
    return 0;
}

/* ── VERIFY ───────────────────────────────────────────── */

static int do_verify(const char *in_path) {
    size_t data_sz;
    uint8_t *data = read_file(in_path, &data_sz);
    if (!data) return -1;

    const char *tmp_path = "_geopipeline_verify_tmp.gpx5";
    if (do_encode(in_path, tmp_path) != 0) { free(data); return -1; }

    const char *tmp_out = "_geopipeline_verify_tmp.bin";
    if (do_decode(tmp_path, tmp_out) != 0) { free(data); return -1; }

    size_t dec_sz;
    uint8_t *dec = read_file(tmp_out, &dec_sz);
    int match = (dec && dec_sz == data_sz && memcmp(data, dec, data_sz) == 0);

    uint64_t hash = xxh64(data, data_sz);
    printf("Verify: %s\n", in_path);
    printf("  %zu -> %zu bytes (xxh64=0x%016llx)\n",
           data_sz, dec_sz, (unsigned long long)hash);
    printf("  Roundtrip: %s\n", match ? "PASS" : "FAIL");

    remove(tmp_path);
    remove(tmp_out);
    for (uint32_t c = 0; c < 16; c++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "_geopipeline_verify_tmp_c%03u.gpx5", c);
        remove(buf);
    }
    free(data); free(dec);
    return match ? 0 : -1;
}

/* ── INFO ─────────────────────────────────────────────── */

static int do_info(const char *path) {
    Gpx5File gf;
    if (gpx5_open(path, &gf) != 0) {
        fprintf(stderr, "Cannot open GPX5: %s\n", path); return -1;
    }
    printf("GPX5 Info: %s\n", path);
    printf("  magic=0x%08X ver=%u flags=0x%02X n_sets=%u\n",
           gf.hdr.magic, gf.hdr.version, gf.hdr.flags, gf.hdr.n_sets);
    printf("  grid=%ux%u n_tiles=%u seed=0x%08X tick_max=%u\n",
           gf.hdr.tw, gf.hdr.th, gf.hdr.n_tiles, gf.hdr.global_seed, gf.hdr.tick_max);
    for (int i = 0; i < 3; i++) {
        printf("  pipe[%d]: carrier=%u codec=%u hilbert_mod=%u hilbert_rem=%u\n",
               i, gf.pipes[i].carrier_id, gf.pipes[i].codec,
               gf.pipes[i].hilbert_mod, gf.pipes[i].hilbert_rem);
    }
    gpx5_close(&gf);
    return 0;
}

/* ── MAIN ─────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <encode|decode|verify|info> <input> [output]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "encode") == 0) {
        if (argc != 4) { fprintf(stderr, "encode: input output\n"); return 1; }
        return do_encode(argv[2], argv[3]);
    } else if (strcmp(argv[1], "decode") == 0) {
        if (argc != 4) { fprintf(stderr, "decode: input.gpx5 output.bin\n"); return 1; }
        return do_decode(argv[2], argv[3]);
    } else if (strcmp(argv[1], "verify") == 0) {
        if (argc != 3) { fprintf(stderr, "verify: input.bin\n"); return 1; }
        return do_verify(argv[2]);
    } else if (strcmp(argv[1], "info") == 0) {
        if (argc != 3) { fprintf(stderr, "info: input.gpx5\n"); return 1; }
        return do_info(argv[2]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
}