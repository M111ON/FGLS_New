/*
 * fgls_framed.c — FRAMED codec using geo_frame_seek + temporal delta
 *
 * Encodes raw bytes as a sequence of 768B frames. For each frame:
 *   - frame 0: stored as 12 DS-encoded chunks (seed)
 *   - frame N: enc(t)=t*37%1440 (2B), then 12 XOR residuals against seed
 *
 * Wire format (GFFRMED v1):
 *   header (16B):
 *     magic     u32 = "FRMD" (0x444D5246)
 *     version   u16 = 1
 *     flags     u16 = 0
 *     orig_size u32
 *     n_frames  u32
 *   body:
 *     seed_chunks: 12 chunks, each [route:1][size:1][payload...]
 *     per frame (n_frames - 1):
 *       enc[2B]
 *       12 residual chunks, each [route:1][size:1][payload...]
 *
 * Lossless: XOR(seed, residual) = original chunk bytes.
 *
 * Build (from project root):
 *   gcc -O2 -std=c11 -fno-strict-aliasing -lm \
 *       -Icollection -Icore -Irunner \
 *       -o fgls_framed.exe pipeline/fgls_framed.c pipeline/fgls_cli.c \
 *       collection/dgls/geo/src/geo_jump.c -lzstd
 *
 * Or use fgls_profile.h's auto-route for chunk-level codec.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "fgls_profile.h"
#include "geo_frame_seek.h"

#define CHUNK_SZ       64u
#define FRAME_CHUNKS   12u
#define FRAME_BYTES    (CHUNK_SZ * FRAME_CHUNKS)   /* 768 */
#define MAX_FRAMES     65536u

/* Diamond Shell flag values (from geofield_full.c) */
#define DS_FLAG_FLAT    0
#define DS_FLAG_SPARSE  0xFD
#define DS_FLAG_RAW     0xFE
#define DS_FLAG_SUB     0xFF
#define DS_SUB_N        8
#define DS_SUB_SZ       8
#define DS_SPARSE_MAX   16u

/* ── Diamond Shell: encode a 64B chunk → variable bytes ── */
static int ds_count_nz(const uint8_t *buf) {
    int n = 0;
    for (int i = 0; i < 64; i++) if (buf[i]) n++;
    return n;
}

static int ds_count_active_subs(const uint8_t *buf) {
    int active = 0;
    for (int s = 0; s < DS_SUB_N; s++) {
        for (int j = 0; j < DS_SUB_SZ; j++) {
            if (buf[s * DS_SUB_SZ + j]) { active++; break; }
        }
    }
    return active;
}

/* Minimal rotation: identity (rotation 0). Full 6-way rotation adds
 * complexity but minimal gain for typical data. */
static uint32_t ds_classify_simple(uint8_t *out, const uint8_t block[64]) {
    int is_zero = 1;
    for (int i = 0; i < 64; i++) { if (block[i]) { is_zero = 0; break; } }
    if (is_zero) { out[0] = DS_FLAG_FLAT; return 1; }

    int nz = ds_count_nz(block);

    /* SPARSE: ≤16 non-zero bytes */
    if ((uint32_t)nz <= DS_SPARSE_MAX) {
        out[0] = DS_FLAG_SPARSE;
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

    int active = ds_count_active_subs(block);

    /* RAW fallback: dense data */
    if (active >= 7) {
        out[0] = DS_FLAG_RAW;
        out[1] = 0;
        memcpy(out + 2, block, 64);
        return 66;
    }

    /* SUB encoding */
    out[0] = DS_FLAG_SUB;
    out[1] = 0;
    uint8_t sub_flags = 0;
    for (int s = 0; s < DS_SUB_N; s++) {
        for (int j = 0; j < DS_SUB_SZ; j++) {
            if (block[s * DS_SUB_SZ + j]) { sub_flags |= (1u << s); break; }
        }
    }
    out[2] = sub_flags;
    uint32_t pos = 3;
    for (int s = 0; s < DS_SUB_N; s++) {
        if (sub_flags & (1u << s)) {
            memcpy(out + pos, block + s * DS_SUB_SZ, DS_SUB_SZ);
            pos += DS_SUB_SZ;
        }
    }
    return pos;
}

/* DS decode → 64B output, returns bytes consumed */
static uint32_t ds_decode_simple(uint8_t out[64], const uint8_t *in) {
    uint8_t flag = in[0];
    if (flag == DS_FLAG_FLAT) { memset(out, 0, 64); return 1; }
    if (flag == DS_FLAG_SPARSE) {
        uint8_t nz = in[2];
        memset(out, 0, 64);
        for (uint32_t i = 0; i < nz; i++) {
            uint8_t idx = in[3 + i];
            uint8_t val = in[3 + nz + i];
            if (idx < 64) out[idx] = val;
        }
        return 3 + (uint32_t)nz * 2;
    }
    if (flag == DS_FLAG_RAW) {
        memcpy(out, in + 2, 64);
        return 66;
    }
    /* SUB (0xFF) */
    uint8_t sub_flags = in[2];
    memset(out, 0, 64);
    uint32_t pos = 3;
    for (int s = 0; s < DS_SUB_N; s++) {
        if (sub_flags & (1u << s)) {
            memcpy(out + s * DS_SUB_SZ, in + pos, DS_SUB_SZ);
            pos += DS_SUB_SZ;
        }
    }
    return pos;
}

/* ═══════════════════════════════════════════════════════════════
 * GFRMED CONTAINER
 *
 * Format:
 *   header (16B):
 *     magic    u32 = "FRMD" (0x444D5246)
 *     version  u16 = 1
 *     flags    u16 = 0
 *     orig_sz  u32
 *     n_frames u32
 *   seed (12 chunks, each [route:1][size:1][ds_data...])
 *   per frame (n_frames - 1):
 *     enc u16 (2B)
 *     12 residual chunks, each [route:1][size:1][ds_data...]
 * ═══════════════════════════════════════════════════════════════ */

#define FRMD_MAGIC   0x444D5246u
#define FRMD_VERSION 1u
#define FRMD_HDR_SZ  16u

typedef struct {
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

/* ── Encode: data → framed.bin ── */
static int do_encode(const char *in_path, const char *out_path) {
    if (geo_frame_seek_verify() != 0) {
        fprintf(stderr, "ERROR: geo_frame_seek_verify failed\n");
        return -1;
    }

    FILE *fp = fopen(in_path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", in_path); return -1; }
    fseek(fp, 0, SEEK_END);
    uint32_t data_sz = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(data_sz);
    if (!data) { fclose(fp); return -1; }
    if (fread(data, 1, data_sz, fp) != data_sz) {
        free(data); fclose(fp); return -1;
    }
    fclose(fp);

    uint32_t n_frames = (data_sz + FRAME_BYTES - 1) / FRAME_BYTES;
    if (n_frames == 0 || n_frames > MAX_FRAMES) {
        free(data); fprintf(stderr, "n_frames out of range\n"); return -1;
    }

    /* Read frame 0 → 12 chunks → DS encode each */
    uint8_t seed_chunk[FRAME_CHUNKS][CHUNK_SZ];
    for (uint32_t ci = 0; ci < FRAME_CHUNKS; ci++) {
        uint32_t off = ci * CHUNK_SZ;
        uint32_t sz = (off + CHUNK_SZ <= data_sz) ? CHUNK_SZ : data_sz - off;
        memset(seed_chunk[ci], 0, CHUNK_SZ);
        if (sz > 0) memcpy(seed_chunk[ci], data + off, sz);
    }

    /* Worst case output: header + 12 seed × 66B + n_frames × (2B + 12 × 66B) */
    uint32_t out_cap = FRMD_HDR_SZ + 12 * 66 + n_frames * (2 + 12 * 66);
    /* Add safety margin for any overflow */
    out_cap = out_cap + (out_cap >> 4);
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    if (!out_buf) { free(data); return -1; }

    /* Header */
    FrmdHdr hdr;
    hdr.magic     = FRMD_MAGIC;
    hdr.version   = FRMD_VERSION;
    hdr.flags     = 0;
    hdr.orig_size = data_sz;
    hdr.n_frames  = n_frames;
    frmd_hdr_write(out_buf, &hdr);
    uint32_t pos = FRMD_HDR_SZ;

    /* Seed chunks */
    uint32_t seed_bytes = 0;
    for (uint32_t ci = 0; ci < FRAME_CHUNKS; ci++) {
        uint8_t ds_buf[70];
        uint32_t ds_sz = ds_classify_simple(ds_buf, seed_chunk[ci]);
        out_buf[pos++] = DS_FLAG_SUB;  /* route marker (any will do for seed) */
        out_buf[pos++] = (uint8_t)CHUNK_SZ;  /* original size */
        memcpy(out_buf + pos, ds_buf, ds_sz);
        pos += ds_sz;
        seed_bytes += 2 + ds_sz;
    }

    /* Per-frame: enc + 12 XOR residuals */
    uint32_t total_res_bytes = 0;
    for (uint32_t fi = 1; fi < n_frames; fi++) {
        uint16_t enc = frame_enc(fi);
        out_buf[pos++] = (uint8_t)(enc & 0xFF);
        out_buf[pos++] = (uint8_t)(enc >> 8);

        uint32_t frame_off = fi * FRAME_BYTES;
        for (uint32_t ci = 0; ci < FRAME_CHUNKS; ci++) {
            uint32_t off = frame_off + ci * CHUNK_SZ;
            uint32_t sz = (off + CHUNK_SZ <= data_sz) ? CHUNK_SZ : (data_sz > off ? data_sz - off : 0);
            uint8_t chunk[CHUNK_SZ];
            uint8_t residual[CHUNK_SZ];
            memset(chunk, 0, CHUNK_SZ);
            memset(residual, 0, CHUNK_SZ);
            if (sz > 0) memcpy(chunk, data + off, sz);
            for (uint32_t b = 0; b < CHUNK_SZ; b++) {
                residual[b] = chunk[b] ^ seed_chunk[ci][b];
            }
            uint8_t ds_buf[70];
            uint32_t ds_sz = ds_classify_simple(ds_buf, residual);
            /* bounds check */
            if (pos + 2 + ds_sz > out_cap) {
                fprintf(stderr, "ERROR: output buffer overflow at frame %u chunk %d\n", fi, ci);
                free(out_buf); free(data); return -1;
            }
            out_buf[pos++] = DS_FLAG_SUB;
            out_buf[pos++] = (uint8_t)CHUNK_SZ;
            memcpy(out_buf + pos, ds_buf, ds_sz);
            pos += ds_sz;
            total_res_bytes += 2 + ds_sz;
        }
    }

    /* Write */
    FILE *fo = fopen(out_path, "wb");
    if (!fo) { free(out_buf); free(data); return -1; }
    fwrite(out_buf, 1, pos, fo);
    fclose(fo);

    printf("FRAMED encoded: %s → %s\n", in_path, out_path);
    printf("  Input:    %u bytes (%u frames × %uB)\n", data_sz, n_frames, FRAME_BYTES);
    printf("  Output:   %u bytes (%.3fx reduction)\n", pos, (double)data_sz / pos);
    printf("  Seed:     %u bytes (12 DS-encoded chunks)\n", seed_bytes);
    printf("  Frames:   %u × (2B enc + 12 residuals) = %u bytes\n",
           n_frames - 1, total_res_bytes);

    free(out_buf);
    free(data);
    return 0;
}

/* ── Decode: framed.bin → data ── */
static int do_decode(const char *in_path, const char *out_path) {
    FILE *fp = fopen(in_path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", in_path); return -1; }
    fseek(fp, 0, SEEK_END);
    uint32_t file_sz = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(file_sz);
    if (!data) { fclose(fp); return -1; }
    if (fread(data, 1, file_sz, fp) != file_sz) {
        free(data); fclose(fp); return -1;
    }
    fclose(fp);

    if (file_sz < FRMD_HDR_SZ) {
        fprintf(stderr, "File too small\n"); free(data); return -1;
    }
    FrmdHdr hdr;
    frmd_hdr_read(data, &hdr);
    if (hdr.magic != FRMD_MAGIC) {
        fprintf(stderr, "Not a FRMD file (magic=0x%08X)\n", hdr.magic);
        free(data); return -1;
    }

    uint8_t *out_buf = (uint8_t *)calloc(hdr.orig_size, 1);
    if (!out_buf) { free(data); return -1; }

    uint32_t pos = FRMD_HDR_SZ;

    /* Read seed chunks */
    uint8_t seed_chunk[FRAME_CHUNKS][CHUNK_SZ];
    for (uint32_t ci = 0; ci < FRAME_CHUNKS; ci++) {
        if (pos + 2 > file_sz) goto fail;
        uint8_t route = data[pos++];
        uint8_t orig_sz = data[pos++];
        (void)route;
        uint8_t chunk[CHUNK_SZ];
        uint32_t consumed = ds_decode_simple(chunk, data + pos);
        if (ci == 0 && (int)orig_sz != CHUNK_SZ) {
            fprintf(stderr, "Warning: seed chunk %d size %u != %u\n", ci, orig_sz, CHUNK_SZ);
        }
        memcpy(seed_chunk[ci], chunk, CHUNK_SZ);
        pos += consumed;
    }

    /* Write seed frame (frame 0) */
    uint32_t seed_frame_bytes = (hdr.orig_size >= FRAME_BYTES) ? FRAME_BYTES : hdr.orig_size;
    for (uint32_t ci = 0; ci < FRAME_CHUNKS; ci++) {
        uint32_t off = ci * CHUNK_SZ;
        if (off >= seed_frame_bytes) break;
        uint32_t sz = (off + CHUNK_SZ <= seed_frame_bytes) ? CHUNK_SZ : seed_frame_bytes - off;
        memcpy(out_buf + off, seed_chunk[ci], sz);
    }

    /* Read frames 1..n */
    for (uint32_t fi = 1; fi < hdr.n_frames; fi++) {
        if (pos + 2 > file_sz) goto fail;
        uint16_t enc = (uint16_t)(data[pos] | (data[pos + 1] << 8));
        pos += 2;

        /* Verify enc matches stride-37 walk */
        if (enc != frame_enc(fi)) {
            fprintf(stderr, "Warning: frame %u enc=%u != expected %u\n",
                    fi, enc, frame_enc(fi));
        }

        uint32_t frame_off = fi * FRAME_BYTES;
        for (uint32_t ci = 0; ci < FRAME_CHUNKS; ci++) {
            if (pos + 2 > file_sz) goto fail;
            uint8_t route = data[pos++];
            uint8_t orig_sz = data[pos++];
            (void)route; (void)orig_sz;
            uint8_t residual[CHUNK_SZ];
            uint32_t consumed = ds_decode_simple(residual, data + pos);
            pos += consumed;

            uint32_t off = frame_off + ci * CHUNK_SZ;
            uint32_t sz = (off + CHUNK_SZ <= hdr.orig_size) ? CHUNK_SZ
                       : (hdr.orig_size > off ? hdr.orig_size - off : 0);
            for (uint32_t b = 0; b < sz; b++) {
                out_buf[off + b] = residual[b] ^ seed_chunk[ci][b];
            }
        }
    }

    /* Write */
    FILE *fo = fopen(out_path, "wb");
    if (!fo) { free(out_buf); free(data); return -1; }
    fwrite(out_buf, 1, hdr.orig_size, fo);
    fclose(fo);

    printf("FRAMED decoded: %s → %s\n", in_path, out_path);
    printf("  Output: %u bytes (orig size=%u)\n", hdr.orig_size, hdr.orig_size);

    free(out_buf);
    free(data);
    return 0;

fail:
    fprintf(stderr, "ERROR: truncated FRMD file\n");
    free(out_buf);
    free(data);
    return -1;
}

/* ── bench: encode with FRAMED + auto-route, compare ── */
static int do_bench(const char *in_path) {
    FILE *fp = fopen(in_path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", in_path); return -1; }
    fseek(fp, 0, SEEK_END);
    uint32_t data_sz = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(data_sz);
    if (!data) { fclose(fp); return -1; }
    if (fread(data, 1, data_sz, fp) != data_sz) {
        free(data); fclose(fp); return -1;
    }
    fclose(fp);

    FglsProfile p;
    if (fgls_profile(data, data_sz, &p) != 0) {
        free(data); return -1;
    }

    printf("File: %s (%u bytes)\n", in_path, data_sz);
    printf("  entropy=%.3f bits/B | locality=%.3f | nonzero=%u/%u (%.1f%%)\n",
           (double)p.entropy_x1000 / 1000.0,
           (double)p.locality_x1000 / 1000.0,
           p.nonzero_count, p.size,
           (double)p.nonzero_count * 100.0 / p.size);
    printf("  Use `encode` then `ls -la` to compare sizes vs `fgls.exe encode`.\n");

    free(data);
    return 0;
}

/* ── main ── */
static void usage(void) {
    fprintf(stderr,
        "fgls_framed — geo_frame_seek + temporal delta codec\n"
        "Usage:\n"
        "  fgls_framed encode <input> <output>\n"
        "  fgls_framed decode <input> <output>\n"
        "  fgls_framed bench <input>\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 1; }
    if (strcmp(argv[1], "encode") == 0) {
        if (argc < 4) { usage(); return 1; }
        return do_encode(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "decode") == 0) {
        if (argc < 4) { usage(); return 1; }
        return do_decode(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "bench") == 0) {
        return do_bench(argv[2]);
    }
    usage();
    return 1;
}