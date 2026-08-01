/*
 * fgls_archive.c — Create .fgls archive from GGUF (v2)
 *
 * v2: stores original GGUF header in body for lossless reconstruction.
 *     arch_offset is relative to tensor data start (after GGUF header in body).
 *
 * Compile:
 *   gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I.
 *     runner/explore/fgls_archive.c -o runner/explore/fgls_archive.exe
 * Run:
 *   runner/explore/fgls_archive.exe input.gguf [output.fgls]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include <sys/stat.h>
#include "beam_addressing/gguf_reader.h"
#include "runner/explore/fgls_archive.h"

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    char fout[512];
    if (argc > 2) {
        snprintf(fout, sizeof(fout), "%s", argv[2]);
    } else {
        snprintf(fout, sizeof(fout), "%s.fgls", fin);
    }

    printf("=== FGLS ARCHIVE v2 ===\n");
    printf("  input:  %s\n", fin);
    printf("  output: %s\n\n", fout);

    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }
    printf("  GGUF v%u, %" PRIu64 " tensors\n", gf->version, gf->tensor_count);

    FILE *fp = fopen(fin, "rb");
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    printf("  size: %.1f MB\n", fsz / 1048576.0);
    printf("  GGUF header: %" PRIu64 " bytes (%.1f MB)\n\n",
           gf->tensor_data_start, gf->tensor_data_start / 1048576.0);

    /* Count Q8_0 tensors */
    int n_baked = 0;
    uint64_t total_weights = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type == 8) {
            n_baked++;
            total_weights += (gf->tensors[t].size_bytes / FGLS_BLOCK_SZ) * FGLS_WEIGHTS_PER_BLOCK;
        }
    }
    printf("  Q8_0 tensors: %d\n\n", n_baked);

    /* Phase 1: Scan Q8_0 tensors */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t *tensor_arch_offsets = (uint64_t*)calloc(gf->tensor_count, sizeof(uint64_t));
    uint64_t *tensor_arch_sizes = (uint64_t*)calloc(gf->tensor_count, sizeof(uint64_t));
    uint8_t **tensor_bitmaps = (uint8_t**)calloc(gf->tensor_count, sizeof(uint8_t*));
    uint8_t **tensor_scales = (uint8_t**)calloc(gf->tensor_count, sizeof(uint8_t*));

    uint64_t tensor_body_size = 0;  /* size of tensor data only (after GGUF header in body) */
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        tensor_arch_offsets[t] = tensor_body_size;

        if (gf->tensors[t].type != 8) {
            tensor_arch_sizes[t] = gf->tensors[t].size_bytes;
            tensor_body_size += gf->tensors[t].size_bytes;
            continue;
        }

        uint64_t n_blocks = gf->tensors[t].size_bytes / FGLS_BLOCK_SZ;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;

        tensor_bitmaps[t] = (uint8_t*)calloc(n_blocks * 4, 1);
        tensor_scales[t] = (uint8_t*)malloc(n_blocks * 2);

        uint64_t kept = 0;
        for (uint64_t qb = 0; qb < n_blocks; qb++) {
            uint8_t blk[FGLS_BLOCK_SZ];
            fseek(fp, off + qb * FGLS_BLOCK_SZ, SEEK_SET);
            fread(blk, 1, FGLS_BLOCK_SZ, fp);

            memcpy(tensor_scales[t] + qb * 2, blk, 2);

            uint32_t bitmap = 0;
            for (int i = 0; i < 32; i++) {
                int8_t w = (int8_t)blk[2 + i];
                if (fgls_keep_w(w)) {
                    bitmap |= (1u << i);
                    kept++;
                }
            }
            memcpy(tensor_bitmaps[t] + qb * 4, &bitmap, 4);
        }

        tensor_arch_sizes[t] = n_blocks * 2 + n_blocks * 4 + kept;
        tensor_body_size += tensor_arch_sizes[t];
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("  scan time: %.3f sec\n", (t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9);
    printf("  tensor data: %" PRIu64 " bytes (%.1f MB)\n\n", tensor_body_size, tensor_body_size/1048576.0);

    /* Phase 2: Write archive */
    FILE *fo = fopen(fout, "wb");
    if (!fo) { printf("[FAIL] write\n"); return 1; }

    /* Placeholder header */
    FGLS_Header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FGLS_MAGIC;
    hdr.version = FGLS_VERSION;
    hdr.n_tensors = (uint32_t)gf->tensor_count;
    hdr.n_baked = (uint32_t)n_baked;
    hdr.orig_size = (uint64_t)fsz;
    hdr.orig_data_start = gf->tensor_data_start;
    hdr.total_weights = total_weights;
    fwrite(&hdr, 1, FGLS_HEADER_SZ, fo);

    /* Tensor table */
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        FGLS_TensorEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.name_len = (uint16_t)(strlen(gf->tensors[t].name) + 1);
        entry.ndim = (uint8_t)gf->tensors[t].n_dims;
        for (int d = 0; d < 4; d++) entry.dims[d] = (uint32_t)gf->tensors[t].dims[d];
        entry.ggml_type = (uint8_t)gf->tensors[t].type;
        entry.n_elements = gf->tensors[t].n_weights;
        entry.orig_offset = gf->tensors[t].offset;
        entry.arch_offset = tensor_arch_offsets[t];
        entry.arch_size = tensor_arch_sizes[t];
        entry.n_blocks = (gf->tensors[t].type == 8) ?
            (uint32_t)(gf->tensors[t].size_bytes / FGLS_BLOCK_SZ) : 0;
        entry.flags = (gf->tensors[t].type == 8) ? 1 : 0;
        fwrite(&entry, 1, sizeof(entry), fo);
        fwrite(gf->tensors[t].name, 1, entry.name_len, fo);
    }

    /* ── Body: first the original GGUF header ── */
    {
        uint8_t *gguf_hdr = (uint8_t*)malloc(gf->tensor_data_start);
        fseek(fp, 0, SEEK_SET);
        fread(gguf_hdr, 1, gf->tensor_data_start, fp);
        fwrite(gguf_hdr, 1, gf->tensor_data_start, fo);
        free(gguf_hdr);
    }

    /* ── Body: then tensor data ── */
    uint64_t kept_total = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) {
            uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
            uint64_t sz = gf->tensors[t].size_bytes;
            fseek(fp, off, SEEK_SET);
            uint8_t *buf = (uint8_t*)malloc(sz);
            fread(buf, 1, sz, fp);
            fwrite(buf, 1, sz, fo);
            free(buf);
            continue;
        }

        uint64_t n_blocks = gf->tensors[t].size_bytes / FGLS_BLOCK_SZ;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;

        fwrite(tensor_scales[t], 1, n_blocks * 2, fo);
        fwrite(tensor_bitmaps[t], 1, n_blocks * 4, fo);

        for (uint64_t qb = 0; qb < n_blocks; qb++) {
            uint8_t blk[FGLS_BLOCK_SZ];
            fseek(fp, off + qb * FGLS_BLOCK_SZ, SEEK_SET);
            fread(blk, 1, FGLS_BLOCK_SZ, fp);

            uint32_t bitmap;
            memcpy(&bitmap, tensor_bitmaps[t] + qb * 4, 4);

            for (int i = 0; i < 32; i++) {
                if (bitmap & (1u << i)) {
                    fwrite(&blk[2 + i], 1, 1, fo);
                    kept_total++;
                }
            }
        }

        free(tensor_bitmaps[t]);
        free(tensor_scales[t]);
    }

    /* Rewrite header with correct kept_weights + arch_size */
    struct stat st;
    stat(fout, &st);
    hdr.kept_weights = kept_total;
    hdr.arch_size = (uint64_t)st.st_size;
    fseek(fo, 0, SEEK_SET);
    fwrite(&hdr, 1, FGLS_HEADER_SZ, fo);
    fclose(fo);
    fclose(fp);


    printf("--- ARCHIVE RESULTS ---\n");
    printf("  archive size: %" PRId64 " bytes (%.1f MB)\n", (int64_t)st.st_size, st.st_size/1048576.0);
    printf("  original:     %ld bytes (%.1f MB)\n", fsz, fsz/1048576.0);
    printf("  ratio:        %.2fx (%.1f%% of original)\n",
           (double)st.st_size/fsz, 100.0*st.st_size/fsz);
    printf("  saved:        %.1f MB (%.1f%%)\n",
           (fsz-st.st_size)/1048576.0, 100.0*(fsz-st.st_size)/fsz);
    printf("  kept weights: %" PRIu64 " / %" PRIu64 " (%.1f%%)\n",
           kept_total, total_weights,
           total_weights ? 100.0*kept_total/total_weights : 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double total_sec = (t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
    printf("  time:         %.3f sec\n", total_sec);

    free(tensor_arch_offsets);
    free(tensor_arch_sizes);
    return 0;
}
