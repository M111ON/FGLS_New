/*
 * fgls_extract.c — Extract .fgls archive back to .gguf (v3, multi-codec)
 *
 * Codecs supported:
 *   RAW (0):      v2 bitmap + scales reconstruction
 *   ZSTD (1):     v3 zstd body, same as RAW reconstruction
 *   UNIVERSAL (2): Global S-Curve + Per-cell Delta reconstruction
 *
 * Compile:
 *   gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I.
 *     -Irunner/explore/zstd_inc runner/explore/fgls_extract.c
 *     C:/mingw64/lib/libzstd.a -lssp -o runner/explore/fgls_extract.exe
 * Run:
 *   runner/explore/fgls_extract.exe input.fgls [output.gguf]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include <zstd.h>
#include "runner/explore/fgls_archive.h"

/* Universal codec constants */
#define UNI_GRID   20736
#define UNI_STRIDE 37
#define UNI_VALS   256

/* Delta reader */
static uint8_t *uni_buf = NULL;
static size_t uni_pos = 0;

static int8_t  read_i8(void)  { return (int8_t)uni_buf[uni_pos++]; }
static int16_t read_i16(void) { int16_t v = uni_buf[uni_pos]|(uni_buf[uni_pos+1]<<8); uni_pos+=2; return v; }
static int32_t read_i32(void) { int32_t v = uni_buf[uni_pos]|(uni_buf[uni_pos+1]<<8)|
    (uni_buf[uni_pos+2]<<16)|(uni_buf[uni_pos+3]<<24); uni_pos+=4; return v; }
static int32_t read_delta(void) {
    int8_t tag = read_i8();
    if (tag >= -126) return tag;       /* -126..127 → 1 byte */
    if (tag == -128) return read_i16(); /* int16 follows */
    return read_i32();                  /* tag == -127 → int32 follows */
}

/* ── Universal codec: reconstruct GGUF from per-cell counts ── */
static int extract_universal(uint8_t *body, uint64_t body_sz,
                             FGLS_Header *hdr, FGLS_TensorEntry *entries,
                             const char *fout) {
    printf("  [UNIVERSAL] Parse universal blob\n");

    /* The universal blob is the body itself (after optional GGUF header) */
    uint8_t *p = body;
    uint64_t consumed = 0;

    /* Check if there's a GGUF header prefix (orig_data_start > 0) */
    if (hdr->orig_data_start > 0 && hdr->orig_data_start < body_sz) {
        printf("  [UNIVERSAL] GGUF header prefix: %" PRIu64 " bytes\n", hdr->orig_data_start);
        consumed = hdr->orig_data_start;
        p += consumed;
    }

    /* Parse universal header */
    if (body_sz - consumed < 4*3 + UNI_VALS*4 + UNI_GRID*2) {
        printf("[FAIL] universal blob too small\n");
        return 1;
    }

    uint32_t grid = *(uint32_t*)p; p += 4;
    uint32_t stride = *(uint32_t*)p; p += 4;
    uint32_t total_w = *(uint32_t*)p; p += 4;
    uint32_t global[UNI_VALS];
    memcpy(global, p, UNI_VALS*4); p += UNI_VALS*4;
    uint16_t *cell_weight = (uint16_t*)p; p += UNI_GRID*2;

    printf("  [UNIVERSAL] grid=%u stride=%u total=%u\n", grid, stride, total_w);
    if (grid != UNI_GRID || stride != UNI_STRIDE) {
        printf("[FAIL] bad universal header (grid=%u stride=%u)\n", grid, stride);
        return 1;
    }

    /* Deltas are stored raw inside the blob (body already zstd-compressed) */
    uint64_t delta_sz = body_sz - (uint64_t)(p - body);
    printf("  [UNIVERSAL] deltas: %" PRIu64 " bytes (raw)\n", (uint64_t)delta_sz);

    /* Reconstruct per-cell local counts from deltas */
    uni_buf = p;
    uni_pos = 0;

    uint32_t *cell_local = (uint32_t*)calloc((size_t)UNI_GRID * UNI_VALS, sizeof(uint32_t));
    int cells_ok = 0;
    for (int cell = 0; cell < UNI_GRID; cell++) {
        if (cell_weight[cell] == 0) continue;
        cells_ok++;
        for (int v = 0; v < UNI_VALS; v++) {
            int32_t delta = read_delta();
            int64_t expected = (int64_t)global[v] * cell_weight[cell] / total_w;
            int32_t count = (int32_t)expected + delta;
            if (count < 0) {
                printf("[FAIL] negative count cell=%d val=%d\n", cell, v-128);
                free(cell_local);
                return 1;
            }
            cell_local[(size_t)cell * UNI_VALS + v] = (uint32_t)count;
        }
    }
    printf("  [UNIVERSAL] reconstructed %d cells from deltas\n", cells_ok);

    /* Allocate output GGUF = original size */
    uint64_t orig_sz = hdr->orig_size;
    uint8_t *out = (uint8_t*)calloc(1, (size_t)orig_sz);
    if (!out) { printf("[FAIL] malloc output\n"); free(cell_local); return 1; }

    /* Restore GGUF header from body prefix */
    if (hdr->orig_data_start > 0 && hdr->orig_data_start <= body_sz) {
        memcpy(out, body, (size_t)hdr->orig_data_start);
        printf("  [UNIVERSAL] GGUF header restored: %" PRIu64 " bytes\n", hdr->orig_data_start);
    }

    /* Reconstruct Q8_0 tensors using stride-37 placement */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t gtotal = 0;
    uint64_t weights_placed = 0;
    for (uint32_t t = 0; t < hdr->n_tensors; t++) {
        uint64_t orig_off = hdr->orig_data_start + entries[t].orig_offset;

        if (!(entries[t].flags & 1)) {
            /* Non-Q8_0: copy from body if available */
            if (entries[t].arch_size > 0 && entries[t].arch_offset > 0) {
                /* arch_offset in universal mode points to raw data in body */
            }
            continue;
        }

        /* Q8_0: reconstruct from per-cell counts */
        uint64_t n_blocks = entries[t].n_blocks;
        for (uint64_t qb = 0; qb < n_blocks; qb++) {
            /* Block = 2B scale + 32B weights = 34B total */
            uint8_t blk[34];
            /* Scale: fill with 0 (universal codec doesn't store scales — see note) */
            blk[0] = 0; blk[1] = 0;

            for (int i = 0; i < 32; i++) {
                int cell = (int)((gtotal * UNI_STRIDE) % UNI_GRID);
                /* Find first value with count > 0 */
                int8_t w = 0;
                int found = 0;
                for (int v = 0; v < UNI_VALS; v++) {
                    if (cell_local[(size_t)cell * UNI_VALS + v] > 0) {
                        cell_local[(size_t)cell * UNI_VALS + v]--;
                        w = (int8_t)(v - 128);
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    printf("[FAIL] no value for cell=%d at gtotal=%" PRIu64 "\n", cell, gtotal);
                    free(cell_local); free(out);
                    return 1;
                }
                blk[2 + i] = (uint8_t)w;
                gtotal++;
                weights_placed++;
            }

            /* Write block to output */
            uint64_t blk_off = orig_off + qb * 34;
            if (blk_off + 34 <= orig_sz) {
                memcpy(out + blk_off, blk, 34);
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;

    /* Verify: check all counts consumed */
    int all_zero = 1;
    for (int cell = 0; cell < UNI_GRID; cell++) {
        for (int v = 0; v < UNI_VALS; v++) {
            if (cell_local[(size_t)cell * UNI_VALS + v] != 0) {
                if (all_zero) printf("[WARN] leftover counts: cell=%d val=%d count=%u\n",
                    cell, v-128, cell_local[(size_t)cell * UNI_VALS + v]);
                all_zero = 0;
            }
        }
    }

    /* Write output */
    FILE *fo = fopen(fout, "wb");
    if (!fo) { printf("[FAIL] write\n"); free(cell_local); free(out); return 1; }
    fwrite(out, 1, (size_t)orig_sz, fo);
    fclose(fo);

    printf("\n--- EXTRACT RESULTS (universal) ---\n");
    printf("  time:     %.3f sec\n", sec);
    printf("  placed:   %" PRIu64 " weights (%.1fM)\n", weights_placed, weights_placed/1048576.0);
    printf("  all counts consumed: %s\n", all_zero ? "YES" : "NO (partial)");
    printf("  output:   %" PRIu64 " bytes (%.1f MB)\n", orig_sz, orig_sz/1048576.0);

    free(cell_local);
    free(out);
    return 0;
}

/* ── Main: standard extract path (ZSTD/RAW) ── */
int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf.fgls";
    char fout[512];
    if (argc > 2) {
        snprintf(fout, sizeof(fout), "%s", argv[2]);
    } else {
        snprintf(fout, sizeof(fout), "%s", fin);
        char *dot = strrchr(fout, '.');
        if (dot && strcmp(dot, ".fgls") == 0) strcpy(dot, ".gguf");
        else strcat(fout, ".restored.gguf");
    }

    printf("=== FGLS EXTRACT v3 — .fgls -> .gguf ===\n");
    printf("  input:  %s\n", fin);
    printf("  output: %s\n\n", fout);

    FILE *fi = fopen(fin, "rb");
    if (!fi) { printf("[FAIL] open %s\n", fin); return 1; }
    fseek(fi, 0, SEEK_END);
    long fsz = ftell(fi);
    fseek(fi, 0, SEEK_SET);

    /* Read FGLS header */
    FGLS_Header hdr;
    if (fread(&hdr, 1, FGLS_HEADER_SZ, fi) != FGLS_HEADER_SZ) {
        printf("[FAIL] read header\n"); fclose(fi); return 1;
    }
    if (hdr.magic != FGLS_MAGIC) {
        printf("[FAIL] bad magic: 0x%08X\n", hdr.magic); fclose(fi); return 1;
    }
    printf("  FGLS v%u, %u tensors (%u baked)\n", hdr.version, hdr.n_tensors, hdr.n_baked);
    printf("  original: %.1f MB\n", hdr.orig_size / 1048576.0);

    /* Read tensor table */
    FGLS_TensorEntry *entries = (FGLS_TensorEntry*)calloc(hdr.n_tensors, sizeof(FGLS_TensorEntry));
    char **names = (char**)calloc(hdr.n_tensors, sizeof(char*));
    for (uint32_t t = 0; t < hdr.n_tensors; t++) {
        if (fread(&entries[t], 1, sizeof(FGLS_TensorEntry), fi) != sizeof(FGLS_TensorEntry)) {
            printf("[FAIL] read entry %u\n", t); fclose(fi); return 1;
        }
        names[t] = (char*)malloc(entries[t].name_len + 1);
        fread(names[t], 1, entries[t].name_len, fi);
        names[t][entries[t].name_len] = '\0';
    }
    printf("  tensor table: OK\n");

    /* Locate body + codec */
    uint64_t table_end = FGLS_HEADER_SZ;
    for (uint32_t t = 0; t < hdr.n_tensors; t++)
        table_end += sizeof(FGLS_TensorEntry) + entries[t].name_len;

    uint64_t body_raw_size;
    int codec;
    if (hdr.version >= 3) {
        body_raw_size = fgls_body_raw_size(&hdr);
        codec = fgls_body_codec(&hdr);
    } else {
        body_raw_size = (uint64_t)fsz - table_end;
        codec = FGLS_CODEC_RAW;
    }
    printf("  body codec:   %s (%u), raw %.1f MB\n",
           codec == FGLS_CODEC_ZSTD ? "ZSTD" :
           codec == FGLS_CODEC_UNIVERSAL ? "UNIVERSAL" : "RAW",
           codec, body_raw_size/1048576.0);

    /* Read body into RAM (decompress if zstd or universal) */
    uint8_t *body = (uint8_t*)malloc((size_t)body_raw_size);
    if (!body) { printf("[FAIL] malloc body\n"); fclose(fi); return 1; }

    if (codec == FGLS_CODEC_ZSTD || codec == FGLS_CODEC_UNIVERSAL) {
        uint64_t comp_size = (uint64_t)fsz - table_end;
        uint8_t *comp = (uint8_t*)malloc((size_t)comp_size);
        fseek(fi, (long)table_end, SEEK_SET);
        fread(comp, 1, (size_t)comp_size, fi);
        size_t r = ZSTD_decompress(body, (size_t)body_raw_size, comp, (size_t)comp_size);
        if (ZSTD_isError(r)) {
            printf("[FAIL] zstd decompress: %s\n", ZSTD_getErrorName(r));
            free(comp); free(body); fclose(fi); return 1;
        }
        free(comp);
        printf("  decompress: OK (%" PRIu64 " bytes)\n", (uint64_t)r);
    } else {
        fseek(fi, (long)table_end, SEEK_SET);
        fread(body, 1, (size_t)body_raw_size, fi);
    }
    fclose(fi);

    /* ── UNIVERSAL CODEC PATH ── */
    if (codec == FGLS_CODEC_UNIVERSAL) {
        int rc = extract_universal(body, body_raw_size, &hdr, entries, fout);
        free(body);
        for (uint32_t t = 0; t < hdr.n_tensors; t++) free(names[t]);
        free(names); free(entries);
        return rc;
    }

    /* ── STANDARD (ZSTD/RAW) PATH: bitmap + scales reconstruction ── */
    uint8_t *out = (uint8_t*)calloc(1, (size_t)hdr.orig_size);
    if (!out) { printf("[FAIL] malloc\n"); free(body); return 1; }

    memcpy(out, body, (size_t)hdr.orig_data_start);
    printf("  GGUF header: %" PRIu64 " bytes restored\n", hdr.orig_data_start);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t kept_total = 0;
    for (uint32_t t = 0; t < hdr.n_tensors; t++) {
        uint64_t orig_off = hdr.orig_data_start + entries[t].orig_offset;
        const uint8_t *tb = body + hdr.orig_data_start + entries[t].arch_offset;

        if (!(entries[t].flags & 1)) {
            memcpy(out + orig_off, tb, (size_t)entries[t].arch_size);
            continue;
        }

        uint64_t n_blocks = entries[t].n_blocks;
        const uint8_t *scales = tb;
        const uint8_t *bitmaps = tb + n_blocks * 2;
        const uint8_t *kept = tb + n_blocks * 2 + n_blocks * 4;
        uint8_t kept_buf[32];

        for (uint64_t qb = 0; qb < n_blocks; qb++) {
            uint32_t bitmap;
            memcpy(&bitmap, bitmaps + qb * 4, 4);

            int n_kept = __builtin_popcount(bitmap);
            memcpy(kept_buf, kept, n_kept);
            kept += n_kept;

            uint8_t blk[34];
            memcpy(blk, scales + qb * 2, 2);

            int ki = 0;
            for (int i = 0; i < 32; i++) {
                if (bitmap & (1u << i)) {
                    blk[2 + i] = kept_buf[ki++];
                    kept_total++;
                } else {
                    blk[2 + i] = 0;
                }
            }
            memcpy(out + orig_off + qb * 34, blk, 34);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double extract_sec = (t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;

    /* Write output */
    FILE *fo = fopen(fout, "wb");
    if (!fo) { printf("[FAIL] write\n"); free(out); free(body); return 1; }
    fwrite(out, 1, (size_t)hdr.orig_size, fo);
    fclose(fo);

    printf("\n--- EXTRACT RESULTS (standard) ---\n");
    printf("  time:     %.3f sec\n", extract_sec);
    printf("  restored: %" PRIu64 " / %" PRIu64 " weights (%.1f%%)\n",
           kept_total, hdr.total_weights,
           hdr.total_weights ? 100.0*kept_total/hdr.total_weights : 0);

    FILE *fv = fopen(fout, "rb");
    uint32_t magic;
    fread(&magic, 4, 1, fv);
    fclose(fv);
    printf("  magic:    0x%08X (%s)\n", magic, magic == 0x46554747 ? "GGUF OK" : "BAD");
    printf("  output:   %.1f MB\n", hdr.orig_size / 1048576.0);

    free(out);
    free(body);
    for (uint32_t t = 0; t < hdr.n_tensors; t++) free(names[t]);
    free(names); free(entries);
    return 0;
}
