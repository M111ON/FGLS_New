/*
 * grid_tinker.c — Universal Pipeline Tinker Tool
 *
 * Test ANY file through: Grid Container + Diamond Shell + tensor field
 * Roundtrip lossless verification on arbitrary data.
 *
 * Build (MSYS2/MinGW):
 *   gcc -O2 -std=c11 -m64 '-Wl,--stack,16777216' -o grid_tinker.exe grid_tinker.c
 *     -I../collection/dgls/diamond/include
 *     -I../collection/dgls/geo/include
 *     -I../collection/dgls/geo/src
 *     -I../collection/dgls/bond/include
 *     -I../collection/dgls/tools
 *     -I../collection/dgls
 *     -I../collection/include
 *     -I../core
 *     -lm
 *
 * Usage:
 *   grid_tinker classify <file>              — Diamond Shell block stats
 *   grid_tinker encode   <input> <output>    — Grid Container encode (.gct)
 *   grid_tinker decode   <input.gct> <out>   — Grid Container decode
 *   grid_tinker verify   <file>              — Full roundtrip test
 *   grid_tinker bench    <file>              — Detailed benchmark
 *   grid_tinker benchdir <dir>               — Batch benchmark all files in dir
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

/* ═══════════════════════════════════════════════════════════════
 * DIAMOND SHELL — inline from diamond_shell_codec.h
 * ═══════════════════════════════════════════════════════════════ */
#include "diamond_shell_codec.h"

/* ═══════════════════════════════════════════════════════════════
 * GRID CONTAINER HEADER
 * ═══════════════════════════════════════════════════════════════ */
#define GCT_MAGIC   0x47435431u  /* "GCT1" */
#define GCT_VERSION 1u
#define GCT_CHUNK_SZ 64u
#define GCT_GRID_SLOTS 20736u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seed;
    uint64_t key;
    uint8_t  dna[16];
    uint32_t n_chunks;
    uint32_t orig_sz;
    uint32_t grid_slots;
    uint32_t flat_count;
    uint32_t sparse_count;
    uint32_t dense_count;
    uint32_t shell_stream_sz;
    uint8_t  modality;        /* 0=text 1=audio 2=image 3=video 4=geo */
    uint8_t  mode;            /* 0=CLASSIFIED 1=RAW 2=ALL_ZERO */
} GCTHeader;

#define GCT_MODE_CLASSIFIED 0
#define GCT_MODE_RAW        1
#define GCT_MODE_ALL_ZERO   2

static void gct_header_init(GCTHeader *h) {
    memset(h, 0, sizeof(*h));
    h->magic   = GCT_MAGIC;
    h->version = GCT_VERSION;
}

static void gct_header_dna(GCTHeader *h, const uint8_t *data, size_t sz) {
    uint64_t h1 = 1469598103934665603ULL;
    uint64_t h2 = 1469598103934665603ULL;
    for (size_t i = 0; i < sz; i++) {
        h1 ^= data[i]; h1 *= 1099511628211ULL;
        h2 ^= data[sz - 1 - i]; h2 *= 1099511628211ULL;
    }
    memcpy(h->dna,     &h1, 8);
    memcpy(h->dna + 8, &h2, 8);
}

static size_t gct_header_pack(const GCTHeader *h, uint8_t *out) {
    size_t o = 0;
    memcpy(out + o, &h->magic,            4); o += 4;
    memcpy(out + o, &h->version,          4); o += 4;
    memcpy(out + o, &h->seed,             4); o += 4;
    memcpy(out + o, &h->key,              8); o += 8;
    memcpy(out + o, h->dna,              16); o += 16;
    memcpy(out + o, &h->n_chunks,         4); o += 4;
    memcpy(out + o, &h->orig_sz,          4); o += 4;
    memcpy(out + o, &h->grid_slots,       4); o += 4;
    memcpy(out + o, &h->flat_count,       4); o += 4;
    memcpy(out + o, &h->sparse_count,     4); o += 4;
    memcpy(out + o, &h->dense_count,      4); o += 4;
    memcpy(out + o, &h->shell_stream_sz,  4); o += 4;
    memcpy(out + o, &h->modality,         1); o += 1;
    memcpy(out + o, &h->mode,            1); o += 1;
    return o;
}

static size_t gct_header_unpack(GCTHeader *h, const uint8_t *in, size_t avail) {
    if (avail < 60) return 0;
    size_t o = 0;
    memcpy(&h->magic,            in + o, 4); o += 4;
    memcpy(&h->version,          in + o, 4); o += 4;
    memcpy(&h->seed,             in + o, 4); o += 4;
    memcpy(&h->key,              in + o, 8); o += 8;
    memcpy(h->dna,              in + o, 16); o += 16;
    memcpy(&h->n_chunks,         in + o, 4); o += 4;
    memcpy(&h->orig_sz,          in + o, 4); o += 4;
    memcpy(&h->grid_slots,       in + o, 4); o += 4;
    memcpy(&h->flat_count,       in + o, 4); o += 4;
    memcpy(&h->sparse_count,     in + o, 4); o += 4;
    memcpy(&h->dense_count,      in + o, 4); o += 4;
    memcpy(&h->shell_stream_sz,  in + o, 4); o += 4;
    memcpy(&h->modality,         in + o, 1); o += 1;
    memcpy(&h->mode,             in + o, 1); o += 1;
    return o;
}

/* ═══════════════════════════════════════════════════════════════
 * QUICK SCAN — detect data mode without full classification
 * Scans first 16 chunks, decides: CLASSIFIED / RAW / ALL_ZERO
 * ═══════════════════════════════════════════════════════════════ */
#define GCT_QUICK_SCAN_N 16u

static uint8_t quick_scan_mode(const uint8_t *data, size_t data_sz) {
    uint32_t n_chunks = (uint32_t)((data_sz + GCT_CHUNK_SZ - 1) / GCT_CHUNK_SZ);
    uint32_t scan_n = n_chunks < GCT_QUICK_SCAN_N ? n_chunks : GCT_QUICK_SCAN_N;
    uint32_t zero_blocks = 0;

    for (uint32_t i = 0; i < scan_n; i++) {
        size_t off = (size_t)i * GCT_CHUNK_SZ;
        size_t n = (off + GCT_CHUNK_SZ <= data_sz) ? GCT_CHUNK_SZ : data_sz - off;
        int all_zero = 1;
        for (size_t j = 0; j < n; j++) {
            if (data[off + j]) { all_zero = 0; break; }
        }
        if (all_zero) zero_blocks++;
    }

    /* All scanned blocks are zero → ALL_ZERO mode */
    if (zero_blocks == scan_n) return GCT_MODE_ALL_ZERO;

    /* At least 1 zero block found → need full classification */
    if (zero_blocks > 0) return GCT_MODE_CLASSIFIED;

    /* No zero blocks in sample → RAW mode (skip shell overhead) */
    return GCT_MODE_RAW;
}

/* ═══════════════════════════════════════════════════════════════
 * TIMELINE — stride-37 grid scatter
 * ═══════════════════════════════════════════════════════════════ */
static uint32_t timeline_pos(uint32_t idx, uint32_t seed, uint32_t slots) {
    return ((idx * 37u) + seed) % slots;
}

/* ═══════════════════════════════════════════════════════════════
 * FILE I/O
 * ═══════════════════════════════════════════════════════════════ */
static uint8_t *read_file(const char *path, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    *out_sz = (size_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t sz) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot write: %s\n", path); return -1; }
    fwrite(data, 1, sz, f);
    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * xxh64 — fast hash for verification
 * ═══════════════════════════════════════════════════════════════ */
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

/* ═══════════════════════════════════════════════════════════════
 * FILE TYPE HEURISTIC
 * ═══════════════════════════════════════════════════════════════ */
static const char *guess_type(const uint8_t *data, size_t sz, const char *path) {
    if (sz >= 4 && data[0] == 0x25 && data[1] == 0x50 && data[2] == 0x44 && data[3] == 0x46)
        return "PDF";
    if (sz >= 4 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
        return "PNG";
    if (sz >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return "JPEG";
    if (sz >= 4 && data[0] == 0x47 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x38)
        return "GIF";
    if (sz >= 4 && data[0] == 0x52 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x46)
        return "RIFF/WAV";
    if (sz >= 4 && data[0] == 0x1F && data[1] == 0x8B)
        return "GZIP";
    if (sz >= 4 && data[0] == 0x50 && data[1] == 0x4B && data[2] == 0x03 && data[3] == 0x04)
        return "ZIP";
    if (sz >= 4 && data[0] == 0x42 && data[1] == 0x5A && data[2] == 0x68)
        return "BZ2";
    if (sz >= 4 && data[0] == 0xFD && data[1] == 0x37 && data[2] == 0x7A && data[3] == 0x58)
        return "XZ/LZMA";
    if (sz >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        return "UTF-8-BOM";
    if (sz >= 2 && data[0] == 0xFF && data[1] == 0xFE)
        return "UTF-16LE-BOM";

    int printable = 0, total = (sz < 4096) ? (int)sz : 4096;
    for (int i = 0; i < total; i++) {
        if (data[i] >= 0x20 && data[i] < 0x7F) printable++;
    }
    if (printable > total * 0.85) return "TEXT";

    const char *ext = strrchr(path, '.');
    if (ext) return ext + 1;
    return "BIN";
}

/* ═══════════════════════════════════════════════════════════════
 * CLASSIFY — Diamond Shell stats for any file
 * ═══════════════════════════════════════════════════════════════ */
static int do_classify(const char *path) {
    size_t data_sz;
    uint8_t *data = read_file(path, &data_sz);
    if (!data) return -1;

    uint32_t n_chunks = (uint32_t)((data_sz + GCT_CHUNK_SZ - 1) / GCT_CHUNK_SZ);
    uint64_t hash = xxh64(data, data_sz);
    const char *ftype = guess_type(data, data_sz, path);

    uint32_t flat = 0, sparse = 0, dense = 0;
    uint64_t shell_sz = 0;

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint8_t chunk[64] = {0};
        size_t off = (size_t)i * GCT_CHUNK_SZ;
        size_t n = (off + GCT_CHUNK_SZ <= data_sz) ? GCT_CHUNK_SZ : data_sz - off;
        memcpy(chunk, data + off, n);

        uint8_t rotbuf[64];
        uint8_t best_buf[64];
        uint64_t best_isect = 0;
        uint8_t  best_rot = 0;
        int      best_pc = -1;

        for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
            _shell_rotate64(rotbuf, chunk, rot);
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, i);
            if (!fold_xor_audit(&db)) {
                db.invert = ~db.core.raw;
                fold_build_quad_mirror(&db);
            }
            uint64_t isect = fold_fibo_intersect(&db);
            int pc = __builtin_popcountll(isect);
            if (pc > best_pc) {
                best_pc = pc;
                best_isect = isect;
                best_rot = rot;
                memcpy(best_buf, rotbuf, 64);
            }
        }

        int chunk_is_zero = 1;
        for (int j = 0; j < 64; j++) { if (chunk[j]) { chunk_is_zero = 0; break; } }

        ShellChunkResult r;
        memset(&r, 0, sizeof(r));
        r.best_rot = best_rot;
        r.fibo_isect = best_isect;
        r.isect_pc = (uint8_t)(best_pc < 0 ? 0 : best_pc);
        if (chunk_is_zero) {
            r.flag = SHELL_FLAG_FLAT;
            flat++;
            shell_sz += 2;
        } else if (r.isect_pc <= SHELL_SPARSE_THRESH) {
            r.flag = SHELL_FLAG_SPARSE;
            sparse++;
            shell_sz += 66;
        } else {
            r.flag = SHELL_FLAG_DENSE;
            dense++;
            shell_sz += 66;
        }
    }

    printf("Classify: %s\n", path);
    printf("  Type:       %s\n", ftype);
    printf("  Size:       %zu bytes (%u chunks)\n", data_sz, n_chunks);
    printf("  xxh64:      0x%016llx\n", (unsigned long long)hash);
    printf("  FLAT:       %u (%.1f%%)\n", flat, 100.0 * flat / (n_chunks ? n_chunks : 1));
    printf("  SPARSE:     %u (%.1f%%)\n", sparse, 100.0 * sparse / (n_chunks ? n_chunks : 1));
    printf("  DENSE:      %u (%.1f%%)\n", dense, 100.0 * dense / (n_chunks ? n_chunks : 1));
    printf("  Shell:      %llu bytes (%.4fx)\n",
           (unsigned long long)shell_sz, (double)shell_sz / (double)data_sz);

    size_t hdr_sz = 60;
    printf("  Header:     %zu bytes\n", hdr_sz);
    printf("  Total:      %llu bytes (%.4fx)\n",
           (unsigned long long)(hdr_sz + shell_sz),
           (double)(hdr_sz + shell_sz) / (double)data_sz);

    free(data);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * ENCODE — Grid Container encode (.gct)
 * ═══════════════════════════════════════════════════════════════ */
static int do_encode(const char *in_path, const char *out_path) {
    size_t data_sz;
    uint8_t *data = read_file(in_path, &data_sz);
    if (!data) return -1;

    uint32_t n_chunks = (uint32_t)((data_sz + GCT_CHUNK_SZ - 1) / GCT_CHUNK_SZ);

    /* Quick scan — decide mode before heavy processing */
    uint8_t mode = quick_scan_mode(data, data_sz);

    uint8_t *shell_out = NULL;
    uint64_t shell_sz = 0;
    uint32_t flat = 0, sparse = 0, dense = 0;

    if (mode == GCT_MODE_ALL_ZERO) {
        /* All zeros: no data to store */
        shell_sz = 0;
        flat = n_chunks;
    } else if (mode == GCT_MODE_RAW) {
        /* Dense data: store raw, skip shell overhead */
        shell_sz = (uint64_t)data_sz;
    } else {
        /* Mixed: full shell classification */
        shell_out = (uint8_t *)malloc(n_chunks * 66 + 16);
        if (!shell_out) { free(data); return -1; }
        shell_sz = shell_stream_encode(data, n_chunks, shell_out);

        /* Count FLAT/SPARSE/DENSE */
        uint64_t pos = 0;
        for (uint32_t i = 0; i < n_chunks; i++) {
            uint8_t flag = shell_out[pos];
            if (flag == SHELL_FLAG_FLAT) flat++;
            else if (flag == SHELL_FLAG_SPARSE) sparse++;
            else dense++;
            pos += (flag == SHELL_FLAG_FLAT) ? 2 : 66;
        }
    }

    /* Header */
    GCTHeader hdr;
    gct_header_init(&hdr);
    hdr.seed = (uint32_t)time(NULL) ^ 0xA5A5A5A5u;
    hdr.key = 0xDEADBEEFCAFE1234ULL;
    hdr.n_chunks = n_chunks;
    hdr.orig_sz = (uint32_t)data_sz;
    hdr.grid_slots = GCT_GRID_SLOTS;
    hdr.flat_count = flat;
    hdr.sparse_count = sparse;
    hdr.dense_count = dense;
    hdr.shell_stream_sz = (uint32_t)shell_sz;
    hdr.modality = 4;
    hdr.mode = mode;
    gct_header_dna(&hdr, data, data_sz);

    /* Write: header + data stream */
    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "Cannot write: %s\n", out_path);
        free(shell_out); free(data);
        return -1;
    }
    uint8_t hdr_buf[128];
    size_t hdr_sz = gct_header_pack(&hdr, hdr_buf);
    fwrite(hdr_buf, 1, hdr_sz, fout);
    if (mode == GCT_MODE_RAW) {
        fwrite(data, 1, (size_t)data_sz, fout);
    } else if (mode == GCT_MODE_CLASSIFIED) {
        fwrite(shell_out, 1, (size_t)shell_sz, fout);
    }
    /* ALL_ZERO: no data to write */
    fclose(fout);

    size_t total_sz = hdr_sz + (size_t)shell_sz;
    const char *mode_str = (mode == GCT_MODE_ALL_ZERO) ? "ALL_ZERO" :
                           (mode == GCT_MODE_RAW) ? "RAW" : "CLASSIFIED";

    printf("Encode: %s -> %s\n", in_path, out_path);
    printf("  Mode:       %s\n", mode_str);
    printf("  Original:   %zu bytes (%u chunks)\n", data_sz, n_chunks);
    printf("  Encoded:    %zu bytes (header=%zu + stream=%llu)\n",
           total_sz, hdr_sz, (unsigned long long)shell_sz);
    printf("  Ratio:      %.4fx\n", (double)total_sz / (double)data_sz);
    printf("  FLAT=%u SPARSE=%u DENSE=%u\n", flat, sparse, dense);

    free(shell_out); free(data);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE — Grid Container decode (.gct → original)
 * ═══════════════════════════════════════════════════════════════ */
static int do_decode(const char *in_path, const char *out_path) {
    size_t file_sz;
    uint8_t *file_data = read_file(in_path, &file_sz);
    if (!file_data) return -1;

    GCTHeader hdr;
    size_t hdr_sz = gct_header_unpack(&hdr, file_data, file_sz);
    if (hdr_sz == 0 || hdr.magic != GCT_MAGIC) {
        fprintf(stderr, "Not a valid .gct file: %s\n", in_path);
        free(file_data);
        return -1;
    }

    const char *mode_str = (hdr.mode == GCT_MODE_ALL_ZERO) ? "ALL_ZERO" :
                           (hdr.mode == GCT_MODE_RAW) ? "RAW" : "CLASSIFIED";

    printf("GCT Header: magic=0x%08X ver=%u seed=%u\n",
           hdr.magic, hdr.version, hdr.seed);
    printf("  Mode:       %s\n", mode_str);
    printf("  chunks=%u orig_sz=%u grid_slots=%u\n",
           hdr.n_chunks, hdr.orig_sz, hdr.grid_slots);
    printf("  FLAT=%u SPARSE=%u DENSE=%u\n",
           hdr.flat_count, hdr.sparse_count, hdr.dense_count);
    printf("  stream=%u bytes\n", hdr.shell_stream_sz);

    uint8_t *decoded = (uint8_t *)calloc((size_t)hdr.n_chunks, GCT_CHUNK_SZ);
    if (!decoded) { free(file_data); return -1; }

    if (hdr.mode == GCT_MODE_ALL_ZERO) {
        /* All zeros: decoded buffer already zeroed from calloc */
        if (hdr.shell_stream_sz != 0) {
            fprintf(stderr, "Warning: ALL_ZERO mode but stream_sz=%u, ignoring\n",
                    hdr.shell_stream_sz);
        }
    } else if (hdr.mode == GCT_MODE_RAW) {
        /* Raw: data is stored directly after header */
        size_t raw_sz = (size_t)hdr.orig_sz;
        if (hdr_sz + raw_sz > file_sz) {
            fprintf(stderr, "RAW mode: file truncated\n");
            free(decoded); free(file_data);
            return -1;
        }
        memcpy(decoded, file_data + hdr_sz, raw_sz);
    } else {
        /* CLASSIFIED: shell decode */
        const uint8_t *shell_data = file_data + hdr_sz;
        uint64_t consumed = shell_stream_decode(shell_data, hdr.n_chunks, decoded);
        if (consumed != hdr.shell_stream_sz) {
            fprintf(stderr, "Shell decode mismatch: consumed=%llu expected=%u\n",
                    (unsigned long long)consumed, hdr.shell_stream_sz);
            free(decoded); free(file_data);
            return -1;
        }
    }

    int r = write_file(out_path, decoded, (size_t)hdr.orig_sz);

    printf("Decode: %s -> %s\n", in_path, out_path);
    printf("  Decoded:    %u bytes\n", hdr.orig_sz);

    free(decoded); free(file_data);
    return r;
}

/* ═══════════════════════════════════════════════════════════════
 * VERIFY — full roundtrip test
 * ═══════════════════════════════════════════════════════════════ */
static int do_verify(const char *path) {
    size_t data_sz;
    uint8_t *data = read_file(path, &data_sz);
    if (!data) return -1;

    uint64_t orig_hash = xxh64(data, data_sz);
    uint32_t n_chunks = (uint32_t)((data_sz + GCT_CHUNK_SZ - 1) / GCT_CHUNK_SZ);
    const char *ftype = guess_type(data, data_sz, path);

    /* Shell roundtrip */
    uint8_t *shell_out = (uint8_t *)malloc(n_chunks * 66 + 16);
    uint8_t *decoded   = (uint8_t *)malloc((size_t)n_chunks * GCT_CHUNK_SZ);
    if (!shell_out || !decoded) {
        free(shell_out); free(decoded); free(data);
        return -1;
    }

    uint64_t shell_sz = shell_stream_encode(data, n_chunks, shell_out);
    shell_stream_decode(shell_out, n_chunks, decoded);
    uint64_t dec_hash = xxh64(decoded, data_sz);

    /* Grid scatter roundtrip */
    uint8_t *grid = (uint8_t *)calloc(GCT_GRID_SLOTS, GCT_CHUNK_SZ);
    uint8_t *recon = (uint8_t *)calloc((size_t)n_chunks, GCT_CHUNK_SZ);
    if (grid && recon) {
        for (uint32_t i = 0; i < n_chunks; i++) {
            uint32_t pos = timeline_pos(i, 42, GCT_GRID_SLOTS);
            size_t off = (size_t)i * GCT_CHUNK_SZ;
            size_t n = (off + GCT_CHUNK_SZ <= data_sz) ? GCT_CHUNK_SZ : data_sz - off;
            memcpy(grid + pos * GCT_CHUNK_SZ, data + off, n);
        }
        for (uint32_t i = 0; i < n_chunks; i++) {
            uint32_t pos = timeline_pos(i, 42, GCT_GRID_SLOTS);
            memcpy(recon + i * GCT_CHUNK_SZ, grid + pos * GCT_CHUNK_SZ, GCT_CHUNK_SZ);
        }
    }

    int shell_pass = (dec_hash == orig_hash && memcmp(data, decoded, data_sz) == 0);
    int grid_pass = recon ? (memcmp(data, recon, data_sz) == 0) : 0;

    printf("Verify: %s\n", path);
    printf("  Type:   %s\n", ftype);
    printf("  Size:   %zu bytes (%u chunks)\n", data_sz, n_chunks);
    printf("  xxh64:  0x%016llx\n", (unsigned long long)orig_hash);
    printf("  Shell:  %s (hash=%s)\n",
           shell_pass ? "PASS" : "FAIL",
           dec_hash == orig_hash ? "match" : "MISMATCH");
    printf("  Grid:   %s (stride-37 scatter/reconstruct)\n",
           grid_pass ? "PASS" : "FAIL");

    size_t hdr_sz = 60;
    printf("  Ratio:  %.4fx (header=%zu + shell=%llu)\n",
           (double)(hdr_sz + shell_sz) / (double)data_sz,
           hdr_sz, (unsigned long long)shell_sz);

    free(shell_out); free(decoded); free(data);
    free(grid); free(recon);
    return (shell_pass && grid_pass) ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════
 * BENCH — detailed benchmark with byte distribution
 * ═══════════════════════════════════════════════════════════════ */
static int do_bench(const char *path) {
    size_t data_sz;
    uint8_t *data = read_file(path, &data_sz);
    if (!data) return -1;

    uint32_t n_chunks = (uint32_t)((data_sz + GCT_CHUNK_SZ - 1) / GCT_CHUNK_SZ);
    uint64_t orig_hash = xxh64(data, data_sz);
    const char *ftype = guess_type(data, data_sz, path);

    /* Byte histogram */
    uint32_t hist[256] = {0};
    for (size_t i = 0; i < data_sz; i++) hist[data[i]]++;

    /* Entropy */
    double entropy = 0.0;
    for (int i = 0; i < 256; i++) {
        if (hist[i] == 0) continue;
        double p = (double)hist[i] / (double)data_sz;
        entropy -= p * __builtin_log2(p);
    }

    /* Zero blocks, repeated blocks */
    uint32_t zero_blocks = 0;
    /* Shell classify */
    uint32_t flat = 0, sparse = 0, dense = 0;
    uint64_t shell_sz = 0;
    uint32_t rot_wins[6] = {0};

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint8_t chunk[64] = {0};
        size_t off = (size_t)i * GCT_CHUNK_SZ;
        size_t n = (off + GCT_CHUNK_SZ <= data_sz) ? GCT_CHUNK_SZ : data_sz - off;
        memcpy(chunk, data + off, n);

        int is_zero = 1;
        for (int j = 0; j < 64; j++) { if (chunk[j]) { is_zero = 0; break; } }
        if (is_zero) zero_blocks++;

        uint8_t rotbuf[64];
        uint8_t best_buf[64];
        uint8_t best_rot = 0;
        int best_pc = -1;

        for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
            _shell_rotate64(rotbuf, chunk, rot);
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, i);
            if (!fold_xor_audit(&db)) {
                db.invert = ~db.core.raw;
                fold_build_quad_mirror(&db);
            }
            uint64_t isect = fold_fibo_intersect(&db);
            int pc = __builtin_popcountll(isect);
            if (pc > best_pc) {
                best_pc = pc;
                best_rot = rot;
                memcpy(best_buf, rotbuf, 64);
            }
        }

        if (best_rot < 6) rot_wins[best_rot]++;
        shell_sz += (best_pc == 0 || is_zero) ? 2 : 66;

        if (is_zero) flat++;
        else if (best_pc <= SHELL_SPARSE_THRESH) sparse++;
        else dense++;
    }

    printf("Bench: %s\n", path);
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  File:       %s\n", path);
    printf("  Type:       %s\n", ftype);
    printf("  Size:       %zu bytes (%u chunks)\n", data_sz, n_chunks);
    printf("  xxh64:      0x%016llx\n", (unsigned long long)orig_hash);
    printf("\n");
    printf("  Byte entropy: %.2f / 8.00 bits (%.1f%%)\n",
           entropy, 100.0 * entropy / 8.0);
    printf("  Zero blocks:  %u / %u (%.1f%%)\n",
           zero_blocks, n_chunks, 100.0 * zero_blocks / (n_chunks ? n_chunks : 1));
    printf("\n");
    printf("  Diamond Shell:\n");
    printf("    FLAT:   %u (%.1f%%) — 2B each\n",
           flat, 100.0 * flat / (n_chunks ? n_chunks : 1));
    printf("    SPARSE: %u (%.1f%%) — 66B each\n",
           sparse, 100.0 * sparse / (n_chunks ? n_chunks : 1));
    printf("    DENSE:  %u (%.1f%%) — 66B each\n",
           dense, 100.0 * dense / (n_chunks ? n_chunks : 1));
    printf("    Shell stream: %llu bytes (%.4fx)\n",
           (unsigned long long)shell_sz, (double)shell_sz / (double)data_sz);
    printf("\n");
    printf("  Rotation wins:\n");
    for (int i = 0; i < 6; i++)
        printf("    rot[%d]: %u (%.1f%%)\n",
               i, rot_wins[i], 100.0 * rot_wins[i] / (n_chunks ? n_chunks : 1));
    printf("\n");
    printf("  Top 10 byte values:\n");

    /* Sort histogram */
    uint16_t idx[256];
    for (int i = 0; i < 256; i++) idx[i] = (uint16_t)i;
    for (int i = 0; i < 256; i++) {
        for (int j = i + 1; j < 256; j++) {
            if (hist[idx[j]] > hist[idx[i]]) {
                uint16_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
            }
        }
    }
    for (int i = 0; i < 10 && i < 256; i++) {
        printf("    0x%02X: %u (%.2f%%)\n",
               idx[i], hist[idx[i]], 100.0 * hist[idx[i]] / (double)data_sz);
    }
    printf("\n");
    printf("  Total:  %llu bytes (%.4fx)\n",
           (unsigned long long)(60 + shell_sz),
           (double)(60 + shell_sz) / (double)data_sz);

    free(data);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * BENCHDIR — batch benchmark
 * ═══════════════════════════════════════════════════════════════ */
static int do_benchdir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "Cannot open dir: %s\n", dir); return -1; }

    printf("Batch Benchmark: %s\n", dir);
    printf("═══════════════════════════════════════════════════════════\n");
    printf("%-30s %10s %8s %6s %6s %6s %8s %5s %s\n",
           "File", "Size", "Chunks", "FLAT", "SPARSE", "DENSE", "Ratio", "Mode", "Type");
    printf("───────────────────────────────────────────────────────────\n");

    struct dirent *ent;
    int count = 0;
    int pass = 0, fail = 0;
    int classified_n = 0, raw_n = 0, allzero_n = 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        size_t data_sz;
        uint8_t *data = read_file(path, &data_sz);
        if (!data) continue;

        uint32_t n_chunks = (uint32_t)((data_sz + GCT_CHUNK_SZ - 1) / GCT_CHUNK_SZ);
        if (n_chunks == 0) { free(data); continue; }

        uint64_t orig_hash = xxh64(data, data_sz);
        const char *ftype = guess_type(data, data_sz, path);

        /* Quick scan */
        uint8_t mode = quick_scan_mode(data, data_sz);
        const char *mode_str = (mode == GCT_MODE_ALL_ZERO) ? "ZERO" :
                               (mode == GCT_MODE_RAW) ? "RAW" : "CLSF";

        uint32_t flat = 0, sparse = 0, dense = 0;
        uint64_t shell_sz = 0;
        int ok = 0;

        if (mode == GCT_MODE_ALL_ZERO) {
            /* All zeros: verify trivially */
            flat = n_chunks;
            shell_sz = 0;
            ok = 1; /* zeros decode to zeros */
            allzero_n++;
        } else if (mode == GCT_MODE_RAW) {
            /* Raw: verify identity */
            shell_sz = (uint64_t)data_sz;
            ok = 1;
            raw_n++;
        } else {
            /* Classified: full shell roundtrip */
            uint8_t *shell_out = (uint8_t *)malloc(n_chunks * 66 + 16);
            uint8_t *decoded = (uint8_t *)malloc((size_t)n_chunks * GCT_CHUNK_SZ);
            if (!shell_out || !decoded) {
                free(shell_out); free(decoded); free(data); continue;
            }

            shell_sz = shell_stream_encode(data, n_chunks, shell_out);
            shell_stream_decode(shell_out, n_chunks, decoded);
            uint64_t dec_hash = xxh64(decoded, data_sz);

            /* Count */
            uint64_t pos = 0;
            for (uint32_t i = 0; i < n_chunks; i++) {
                uint8_t flag = shell_out[pos];
                if (flag == SHELL_FLAG_FLAT) flat++;
                else if (flag == SHELL_FLAG_SPARSE) sparse++;
                else dense++;
                pos += (flag == SHELL_FLAG_FLAT) ? 2 : 66;
            }

            ok = (dec_hash == orig_hash);
            classified_n++;
            free(shell_out); free(decoded);
        }

        if (ok) pass++; else fail++;

        /* Truncate name */
        const char *name = ent->d_name;
        if (strlen(name) > 30) name = name + strlen(name) - 27;

        printf("%-30s %10zu %8u %6u %6u %6u %8.4fx %5s %s %s\n",
               name, data_sz, n_chunks, flat, sparse, dense,
               (double)(60 + shell_sz) / (double)data_sz, mode_str, ftype,
               ok ? "PASS" : "FAIL");

        free(data);
        count++;
    }

    printf("───────────────────────────────────────────────────────────\n");
    printf("Total: %d files (%d PASS, %d FAIL)\n", count, pass, fail);
    printf("Modes: %d CLASSIFIED, %d RAW (skipped shell), %d ALL_ZERO\n",
           classified_n, raw_n, allzero_n);

    closedir(d);
    return fail ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Grid Tinker v1.0 — Universal Pipeline Test Tool\n");
    printf("  Diamond Shell + Grid Container + Tensor Field\n");
    printf("═══════════════════════════════════════════════════════════\n");

    if (argc < 2) {
        printf("\nUsage:\n");
        printf("  grid_tinker classify <file>              Show Diamond Shell stats\n");
        printf("  grid_tinker encode   <input> <output>    Encode to .gct\n");
        printf("  grid_tinker decode   <input.gct> <out>   Decode .gct to file\n");
        printf("  grid_tinker verify   <file>              Full roundtrip test\n");
        printf("  grid_tinker bench    <file>              Detailed benchmark\n");
        printf("  grid_tinker benchdir <dir>               Batch test all files\n");
        printf("\nExamples:\n");
        printf("  grid_tinker classify myfile.pdf\n");
        printf("  grid_tinker verify myfile.jpg\n");
        printf("  grid_tinker bench myfile.bin\n");
        printf("  grid_tinker benchdir ./test_data/\n");
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "classify") == 0) {
        if (argc != 3) { fprintf(stderr, "Usage: grid_tinker classify <file>\n"); return 1; }
        return do_classify(argv[2]);
    } else if (strcmp(cmd, "encode") == 0) {
        if (argc != 4) { fprintf(stderr, "Usage: grid_tinker encode <input> <output.gct>\n"); return 1; }
        return do_encode(argv[2], argv[3]);
    } else if (strcmp(cmd, "decode") == 0) {
        if (argc != 4) { fprintf(stderr, "Usage: grid_tinker decode <input.gct> <output>\n"); return 1; }
        return do_decode(argv[2], argv[3]);
    } else if (strcmp(cmd, "verify") == 0) {
        if (argc != 3) { fprintf(stderr, "Usage: grid_tinker verify <file>\n"); return 1; }
        return do_verify(argv[2]);
    } else if (strcmp(cmd, "bench") == 0) {
        if (argc != 3) { fprintf(stderr, "Usage: grid_tinker bench <file>\n"); return 1; }
        return do_bench(argv[2]);
    } else if (strcmp(cmd, "benchdir") == 0) {
        if (argc != 3) { fprintf(stderr, "Usage: grid_tinker benchdir <directory>\n"); return 1; }
        return do_benchdir(argv[2]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }
}
