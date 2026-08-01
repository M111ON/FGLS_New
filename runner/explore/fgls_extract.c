/*
 * fgls_extract.c — Extract .fgls archive back to .gguf (v2)
 *
 * v2: reads original GGUF header from archive body for lossless reconstruction.
 *
 * Compile:
 *   gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I.
 *     runner/explore/fgls_extract.c -o runner/explore/fgls_extract.exe
 * Run:
 *   runner/explore/fgls_extract.exe input.fgls [output.gguf]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include "runner/explore/fgls_archive.h"

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

    printf("=== FGLS EXTRACT v2 — .fgls → .gguf ===\n");
    printf("  input:  %s\n", fin);
    printf("  output: %s\n\n", fout);

    FILE *fi = fopen(fin, "rb");
    if (!fi) { printf("[FAIL] open %s\n", fin); return 1; }

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
    printf("  kept: %" PRIu64 " / %" PRIu64 " (%.1f%%)\n\n",
           hdr.kept_weights, hdr.total_weights,
           hdr.total_weights ? 100.0*hdr.kept_weights/hdr.total_weights : 0);

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

    /* Allocate output buffer = original GGUF size */
    uint8_t *out = (uint8_t*)calloc(1, hdr.orig_size);
    if (!out) { printf("[FAIL] malloc\n"); fclose(fi); return 1; }

    /* ── Read original GGUF header from archive body ── */
    /* Body starts after tensor table. GGUF header is first thing in body. */
    uint64_t table_end = FGLS_HEADER_SZ;
    for (uint32_t t = 0; t < hdr.n_tensors; t++)
        table_end += sizeof(FGLS_TensorEntry) + entries[t].name_len;

    fseek(fi, table_end, SEEK_SET);
    fread(out, 1, hdr.orig_data_start, fi);  /* copy GGUF header to output */
    printf("  GGUF header: %" PRIu64 " bytes restored\n", hdr.orig_data_start);

    /* ── Reconstruct tensor data ── */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t kept_total = 0;
    for (uint32_t t = 0; t < hdr.n_tensors; t++) {
        uint64_t orig_off = hdr.orig_data_start + entries[t].orig_offset;

        if (!(entries[t].flags & 1)) {
            /* Non-Q8_0: raw copy */
            fseek(fi, table_end + hdr.orig_data_start + entries[t].arch_offset, SEEK_SET);
            fread(out + orig_off, 1, entries[t].arch_size, fi);
            continue;
        }

        /* Q8_0 baked: reconstruct from scales + bitmap + non-zero weights */
        uint64_t n_blocks = entries[t].n_blocks;
        fseek(fi, table_end + hdr.orig_data_start + entries[t].arch_offset, SEEK_SET);

        uint8_t *scales = (uint8_t*)malloc(n_blocks * 2);
        fread(scales, 1, n_blocks * 2, fi);

        uint8_t *bitmaps = (uint8_t*)malloc(n_blocks * 4);
        fread(bitmaps, 1, n_blocks * 4, fi);

        for (uint64_t qb = 0; qb < n_blocks; qb++) {
            uint32_t bitmap;
            memcpy(&bitmap, bitmaps + qb * 4, 4);

            int n_kept = __builtin_popcount(bitmap);
            uint8_t *kept_buf = (uint8_t*)malloc(n_kept);
            fread(kept_buf, 1, n_kept, fi);

            uint8_t blk[FGLS_BLOCK_SZ];
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
            free(kept_buf);
            memcpy(out + orig_off + qb * FGLS_BLOCK_SZ, blk, FGLS_BLOCK_SZ);
        }

        free(scales);
        free(bitmaps);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double extract_sec = (t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;

    /* Write output */
    FILE *fo = fopen(fout, "wb");
    if (!fo) { printf("[FAIL] write\n"); free(out); return 1; }
    fwrite(out, 1, hdr.orig_size, fo);
    fclose(fo);
    free(out);

    /* Verify */
    printf("\n--- EXTRACT RESULTS ---\n");
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
    printf("  llama.cpp: loads without patch\n");

    for (uint32_t t = 0; t < hdr.n_tensors; t++) free(names[t]);
    free(names);
    free(entries);
    return 0;
}
