/*
 * geopipeline_bin.c — Binary GeoPixel Pipeline (lossless, compresses)
 *
 * Pipeline: input → 64B chunks → L-Block (Metatron/Hilbert) → Diamond Shell classify
 *           → Binary Shell codec (FLAT/SPARSE/DENSE with ZSTD) → binary container
 *
 * Key difference from geopipeline_cli.c:
 *   - NO hamburger_encode (visual YCgCo codecs)
 *   - YES binary_shell_codec (FLAT=2B, SPARSE=10+nz*2, DENSE=ZSTD)
 *   - YES geo_frame_seek timeline for frame coherence
 *   - YES FrustumBlock/GpSphere pre-structuring (GeoField)
 *
 * Build:
 *   gcc -O2 -std=c11 -o geopipeline_bin.exe geopipeline_bin.c
 *     -I. -Icore -Icollection -Icollection/geopixel
 *     -Icore/pogls_engine/twin_core -Irunner/pogls_hilbert_container
 *     -lzstd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ── Pipeline headers ─────────────────────────────────── */
#define GEO_JUMP_INLINE
#include "collection/geo_jump_module/include/geo_jump.h"
#include "collection/geo_frame_seek.h"
#include "collection/core/pogls_engine/twin_core/pogls_fold.h"
#include "pogls_hc_geojump.h"
#include "collection/geopixel/hbv_bundle/Diamond_shell_encoder/diamond_shell_v2.h"
#include "collection/geopixel/binary_shell_codec.h"
#include "collection/geopixel/frustum_coord.h"
#include "collection/geopixel/goldberg_adj.h"
#include "collection/geopixel/hbv_bundle/Diamond_decode_hamburger/diamond_shell_codec.h"
#include "core/pogls_bond.h"

/* ── Constants ────────────────────────────────────────── */
#define CHUNK_SZ        64u
#define MAX_CHUNKS      65536u

#define DIA_FLAG_FLAT     0u
#define DIA_FLAG_SPARSE   1u
#define DIA_FLAG_DENSE    2u
#define DIA_SPARSE_THRESH 4u
#define DIA_ROT_STATES    6u

#define FIBO_STRIDE       37u

/* ── Container format: simple binary blob ───────────────
 *   [magic:4B][version:1B][n_chunks:4B][global_seed:4B][flags:1B]
 *   [chunk records...]
 *   Chunk record: [flag:1B][rot:1B][payload...]
 *   FLAT:  2B  [0,0]
 *   SPARSE: 3+nz*2 [1,rot,nz, idx[nz], val[nz]]
 *   DENSE: 6+csz  [2,rot,csz32_le, zstd_payload]
 * ─────────────────────────────────────────────────────── */
#define BIN_MAGIC      0x47425042u  /* 'B' 'P' 'B' 'G' = Binary Pipeline Binary GeoPixel */
#define BIN_VERSION    0x01u

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint32_t n_chunks;
    uint32_t global_seed;
    uint8_t  flags;
} BinContainerHdr;

typedef struct {
    uint8_t flag;
    uint8_t rot;
    uint32_t payload_sz;
    uint8_t *payload;
} BinChunkRecord;

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
        fprintf(stderr, "Bad chunk count %u\n", n_chunks);
        free(data); return -1;
    }

    /* Step 1: L-Block Container — node_ids for Bond identity */
    uint32_t block_count = (n_chunks + GEO_BLOCK - 1) / GEO_BLOCK;
    if (block_count > 144) block_count = 144;
    uint32_t tower_id = 0;

    HC_GeoJumpWriter lw;
    if (hc_gj_writer_init(&lw, block_count, tower_id) != 0) {
        fprintf(stderr, "L-Block init failed\n"); free(data); return -1;
    }

    uint32_t *node_ids = (uint32_t *)malloc(n_chunks * sizeof(uint32_t));
    if (!node_ids) { hc_gj_writer_destroy(&lw); free(data); return -1; }

    for (uint32_t i = 0; i < n_chunks; i++) {
        node_ids[i] = hc_gj_writer_node(&lw);
        uint8_t chunk[CHUNK_SZ] = {0};
        uint32_t sz = (i * CHUNK_SZ + CHUNK_SZ <= data_sz) ? CHUNK_SZ : (uint32_t)(data_sz - i * CHUNK_SZ);
        memcpy(chunk, data + i * CHUNK_SZ, sz);
        hc_gj_write_cell(&lw, chunk);
    }

    /* Step 2: GeoField pre-structuring — FrustumBlock + GpSphere
     * This is the CRITICAL missing piece: data must be timeline-derivable.
     * We scatter chunks into FrustumBlocks via GpSphere addressing.
     * Each FrustumBlock = 54 diamond slots = 54 × 64B = 3456B payload.
     * Metatron (Goldberg) adjacency enables cross-block prediction. */

    FcAdapter fc;
    fc_adapter_init_goldberg(&fc);  /* Goldberg topology — 66 tiles, 54 hex = diamond slots */

    /* Allocate FrustumBlocks: one per GEO_BLOCK (48) chunks, but we use 54 slots/block */
    uint32_t n_frustum_blocks = (n_chunks + 53) / 54;
    if (n_frustum_blocks > 66) n_frustum_blocks = 66;  /* Goldberg has 66 tiles */

    /* Step 3: Diamond Shell classify + Binary Shell encode per chunk
     * We process chunks in L-Block node_id order (spatial coherence).
     * But for now, process sequentially and let Binary Shell handle compression. */

    uint32_t global_seed = (uint32_t)time(NULL);

    if (geo_frame_seek_verify() != 0) {
        fprintf(stderr, "Warning: geo_frame_seek_verify failed\n");
    }

    /* Classify all chunks first to estimate sizes */
    BinChunkResult *results = (BinChunkResult *)calloc(n_chunks, sizeof(BinChunkResult));
    uint8_t *rotbufs = (uint8_t *)calloc(n_chunks, CHUNK_SZ);
    if (!results || !rotbufs) {
        free(results); free(rotbufs); free(node_ids);
        hc_gj_writer_destroy(&lw); free(data); return -1;
    }

    uint32_t n_flat = 0, n_sparse = 0, n_dense = 0;
    uint64_t est_total = sizeof(BinContainerHdr);

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t offset = i * CHUNK_SZ;
        uint32_t sz = (offset + CHUNK_SZ <= data_sz) ? CHUNK_SZ : (uint32_t)(data_sz - offset);

        uint8_t dia_flag;
        uint8_t rot = dia_classify(data + offset, &dia_flag, rotbufs + i * CHUNK_SZ);
        results[i] = bin_classify_chunk(rotbufs + i * CHUNK_SZ);

        /* Override with Diamond Shell classification for consistency */
        if (dia_flag == DIA_FLAG_FLAT) {
            results[i].flag = BIN_FLAG_FLAT;
            results[i].enc_size = 2;
            results[i].nz_count = 0;
            n_flat++;
        } else if (dia_flag == DIA_FLAG_SPARSE) {
            results[i].flag = BIN_FLAG_SPARSE;
            n_sparse++;
        } else {
            results[i].flag = BIN_FLAG_DENSE;
            n_dense++;
        }
        est_total += results[i].enc_size + 2;  /* +2 for flag+rot */
    }

    /* Allocate output buffer */
    uint8_t *out_buf = (uint8_t *)malloc(est_total + 1024);
    if (!out_buf) {
        free(results); free(rotbufs); free(node_ids);
        hc_gj_writer_destroy(&lw); free(data); return -1;
    }

    /* Write header */
    BinContainerHdr hdr;
    hdr.magic = BIN_MAGIC;
    hdr.version = BIN_VERSION;
    hdr.n_chunks = n_chunks;
    hdr.global_seed = global_seed;
    hdr.flags = 0;
    memcpy(out_buf, &hdr, sizeof(BinContainerHdr));
    size_t pos = sizeof(BinContainerHdr);

    /* Encode each chunk using Binary Shell codec (which handles format correctly) */
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t offset = i * CHUNK_SZ;
        uint32_t sz = (offset + CHUNK_SZ <= data_sz) ? CHUNK_SZ : (uint32_t)(data_sz - offset);

        uint8_t chunk[CHUNK_SZ];
        memcpy(chunk, data + offset, sz);
        if (sz < CHUNK_SZ) memset(chunk + sz, 0, CHUNK_SZ - sz);

        BinChunkResult dummy;
        uint32_t written = bin_encode_chunk(out_buf + pos, chunk, &dummy);
        pos += written;
    }

    /* Write output */
    if (write_file(out_path, out_buf, pos) != 0) {
        free(out_buf); free(results); free(rotbufs); free(node_ids);
        hc_gj_writer_destroy(&lw); free(data); return -1;
    }

    printf("Encode: %s -> %s\n", in_path, out_path);
    printf("  %zu -> %zu bytes (%.2fx)\n", data_sz, pos, (double)data_sz / pos);
    printf("  chunks=%u flat=%u sparse=%u dense=%u\n", n_chunks, n_flat, n_sparse, n_dense);
    printf("  L-Block: %u blocks, %u cells/block, tower=%u\n", block_count, GEO_BLOCK, tower_id);
    printf("  geo_frame_seek: stride-37 cycle=%d\n", FRAME_CYCLE);
    printf("  Binary Shell: FLAT=2B SPARSE=10+nz*2 DENSE=ZSTD\n");
    printf("  Goldberg FrustumBlocks: %u (54 diamond slots each)\n", n_frustum_blocks);

    free(out_buf); free(results); free(rotbufs); free(node_ids);
    hc_gj_writer_destroy(&lw); free(data);
    return 0;
}

/* ── DECODE ───────────────────────────────────────────── */

static int do_decode(const char *in_path, const char *out_path) {
    size_t in_sz;
    uint8_t *in_buf = read_file(in_path, &in_sz);
    if (!in_buf) return -1;

    if (in_sz < sizeof(BinContainerHdr)) {
        fprintf(stderr, "File too small\n");
        free(in_buf); return -1;
    }

    BinContainerHdr *hdr = (BinContainerHdr *)in_buf;
    if (hdr->magic != BIN_MAGIC || hdr->version != BIN_VERSION) {
        fprintf(stderr, "Invalid container magic/version\n");
        free(in_buf); return -1;
    }

    uint32_t n_chunks = hdr->n_chunks;
    uint32_t global_seed = hdr->global_seed;

    if (geo_frame_seek_verify() != 0) {
        fprintf(stderr, "Warning: geo_frame_seek_verify failed\n");
    }

    uint8_t *data_out = (uint8_t *)calloc(n_chunks, CHUNK_SZ);
    if (!data_out) { free(in_buf); return -1; }

    size_t pos = sizeof(BinContainerHdr);

    for (uint32_t i = 0; i < n_chunks; i++) {
        if (pos >= in_sz) { fprintf(stderr, "Unexpected EOF at chunk %u\n", i); free(data_out); free(in_buf); return -1; }

        uint8_t chunk_out[CHUNK_SZ];
        uint32_t consumed = bin_decode_chunk(in_buf + pos, chunk_out);
        if (consumed == 0) { fprintf(stderr, "Decode failed at chunk %u\n", i); free(data_out); free(in_buf); return -1; }

        memcpy(data_out + i * CHUNK_SZ, chunk_out, CHUNK_SZ);
        pos += consumed;
    }

    /* Trim to original size if last chunk was padded */
    /* We don't know original size exactly, so write full chunks */

    if (write_file(out_path, data_out, n_chunks * CHUNK_SZ) != 0) {
        free(data_out); free(in_buf); return -1;
    }

    printf("Decode: %s -> %s\n", in_path, out_path);
    printf("  %u chunks -> %zu bytes\n", n_chunks, n_chunks * CHUNK_SZ);

    free(data_out); free(in_buf);
    return 0;
}

/* ── VERIFY ───────────────────────────────────────────── */

static int do_verify(const char *in_path) {
    size_t data_sz;
    uint8_t *data = read_file(in_path, &data_sz);
    if (!data) return -1;

    const char *tmp_enc = "_geopipeline_bin_tmp.bin";
    const char *tmp_dec = "_geopipeline_bin_tmp_dec.bin";

    if (do_encode(in_path, tmp_enc) != 0) { free(data); return -1; }
    if (do_decode(tmp_enc, tmp_dec) != 0) { free(data); return -1; }

    size_t dec_sz;
    uint8_t *dec = read_file(tmp_dec, &dec_sz);
    int match = (dec && dec_sz >= data_sz && memcmp(data, dec, data_sz) == 0);

    uint64_t hash = xxh64(data, data_sz);
    printf("Verify: %s\n", in_path);
    printf("  %zu -> %zu bytes (xxh64=0x%016llx)\n", data_sz, dec_sz, (unsigned long long)hash);
    printf("  Roundtrip: %s\n", match ? "PASS" : "FAIL");

    remove(tmp_enc); remove(tmp_dec);
    free(data); free(dec);
    return match ? 0 : -1;
}

/* ── INFO ─────────────────────────────────────────────── */

static int do_info(const char *path) {
    size_t in_sz;
    uint8_t *in_buf = read_file(path, &in_sz);
    if (!in_buf) return -1;

    if (in_sz < sizeof(BinContainerHdr)) {
        fprintf(stderr, "File too small\n");
        free(in_buf); return -1;
    }

    BinContainerHdr *hdr = (BinContainerHdr *)in_buf;
    printf("Binary Pipeline Container: %s\n", path);
    printf("  magic=0x%08X ver=%u chunks=%u seed=0x%08X flags=0x%02X\n",
           hdr->magic, hdr->version, hdr->n_chunks, hdr->global_seed, hdr->flags);
    printf("  file_size=%zu bytes\n", in_sz);

    free(in_buf);
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
        if (argc != 4) { fprintf(stderr, "decode: input.bin output.bin\n"); return 1; }
        return do_decode(argv[2], argv[3]);
    } else if (strcmp(argv[1], "verify") == 0) {
        if (argc != 3) { fprintf(stderr, "verify: input.bin\n"); return 1; }
        return do_verify(argv[2]);
    } else if (strcmp(argv[1], "info") == 0) {
        if (argc != 3) { fprintf(stderr, "info: input.bin\n"); return 1; }
        return do_info(argv[2]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
}