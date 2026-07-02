/*
 * gguf_to_pogls.c — Convert GGUF model → POGLS v2 tensor store
 *
 * Reads GGUF directly (no llama DLL), writes .pogls v2 with metadata
 * and optional per-tensor zstd compression.
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -I../collection -I../collection/Hfolder
 *       -DPOGLS_USE_ZSTD
 *       -o gguf_to_pogls.exe gguf_to_pogls.c zstd.dll -lm
 *
 * Usage:
 *   .\gguf_to_pogls.exe model.gguf output.pogls
 *   .\gguf_to_pogls.exe model.gguf output.pogls --compress
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zstd.h>

/* POGLS_USE_ZSTD defined on command line (-DPOGLS_USE_ZSTD) */
#include "pogls_store.h"
#include "pogls_meta.h"
#include "gguf_index.h"

/* Map tensor name → dram_addr (0..20735) using Y-triangle geometry */
static uint32_t name_to_addr(const char *name, uint32_t max_layer) {
    if (!name || name[0] == 0) return 0;
    if (strncmp(name, "blk.", 4) == 0) {
        int layer = 0;
        const char *p = name + 4;
        while (*p >= '0' && *p <= '9') {
            layer = layer * 10 + (*p - '0');
            p++;
        }
        if (*p != '.') return 0;
        p++; /* skip "." after layer number */
        const char *tname = p; /* e.g. "attn_norm.weight" */

        /* Hash remaining name */
        uint32_t h = 0;
        for (const char *q = tname; *q; q++)
            h = h * 131 + (uint8_t)*q;

        if (max_layer == 0) max_layer = 36;
        uint32_t layer_stride = (POGLS_MAX_ADDR) / (max_layer + 1);
        if (layer_stride < 1) layer_stride = 1;
        uint32_t spoke = h % 6;
        uint32_t slot  = (h / 6) % (layer_stride / 6);
        if (slot < 1) slot = 1;
        uint32_t addr = (uint32_t)layer * layer_stride + spoke * (layer_stride / 6) + slot;
        return addr % POGLS_MAX_ADDR;
    }
    /* Non-layer tensors: output.*, token_embd.* */
    uint32_t h = 0;
    for (const char *p = name; *p; p++)
        h = h * 131 + (uint8_t)*p;
    return (h % (POGLS_MAX_ADDR - 1)) + 1; /* avoid addr 0 */
}

int main(int argc, char **argv) {
    int use_compress = 0;
    const char *model_path = NULL;
    const char *out_path   = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--compress") == 0 || strcmp(argv[i], "-c") == 0)
            use_compress = 1;
        else if (!model_path)
            model_path = argv[i];
        else if (!out_path)
            out_path = argv[i];
    }
    if (!model_path || !out_path) {
        fprintf(stderr, "Usage: gguf_to_pogls.exe model.gguf output.pogls [--compress]\n");
        return 1;
    }

    /* Open GGUF */
    GGUFTensorIndex idx;
    memset(&idx, 0, sizeof(idx));
    if (gguf_idx_open(model_path, &idx) != 0) {
        fprintf(stderr, "ERROR: cannot open %s\n", model_path);
        return 1;
    }
    fprintf(stderr, "[pogls] GGUF has %llu tensors\n",
            (unsigned long long)idx.n_tensors);

    FILE *f = fopen(model_path, "rb");
    if (!f) { gguf_idx_close(&idx); return 1; }

    /* Discover max layer number */
    uint32_t max_layer = 0;
    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        if (strncmp(idx.names[i], "blk.", 4) == 0) {
            int layer = atoi(idx.names[i] + 4);
            if ((uint32_t)layer > max_layer) max_layer = (uint32_t)layer;
        }
    }
    fprintf(stderr, "[pogls] max layer = %u\n", max_layer);

    /* Allocate buffers */
    uint64_t total_raw = 0;
    for (uint64_t i = 0; i < idx.n_tensors; i++)
        total_raw += idx.sizes[i];

    uint8_t *tensor_data = (uint8_t*)malloc(total_raw);
    uint8_t *comp_buf    = (uint8_t*)malloc(ZSTD_compressBound(total_raw));
    PoglsTensorMeta *meta_arr = (PoglsTensorMeta*)calloc(idx.n_tensors, sizeof(PoglsTensorMeta));

    if (!tensor_data || !comp_buf || !meta_arr) {
        fprintf(stderr, "ERROR: OOM (%llu MB)\n",
                (unsigned long long)(total_raw >> 20));
        fclose(f); gguf_idx_close(&idx);
        free(tensor_data); free(comp_buf); free(meta_arr);
        return 1;
    }

    /* Phase 1: Read all tensor data + build metadata */
    fprintf(stderr, "[pogls] reading %llu bytes from GGUF...\n",
            (unsigned long long)total_raw);

    uint64_t data_pos = 0;
    uint32_t meta_count = 0;
    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        uint32_t addr = name_to_addr(idx.names[i], max_layer);
        if (addr >= POGLS_MAX_ADDR) continue;

        /* Read tensor data */
        size_t sz = idx.sizes[i];
        if (data_pos + sz > total_raw) break;

        uint64_t abs_off = gguf_idx_tensor_abs_offset(&idx, i);
        fseek(f, (long)abs_off, SEEK_SET);
        if (fread(tensor_data + data_pos, sz, 1, f) != 1) continue;

        /* Fill meta entry */
        PoglsTensorMeta *m = &meta_arr[meta_count];
        m->addr          = addr;
        m->dtype         = idx.dtypes[i];
        m->ndim          = 2; /* simplified */
        m->nbytes_orig   = (uint32_t)sz;
        m->dims[0]       = (uint32_t)(sz > 0 ? 1 : 0);
        m->dims[1]       = (uint32_t)sz;
        snprintf(m->name, sizeof(m->name), "%s", idx.names[i]);

        if (use_compress) {
            /* Try zstd compression */
            pogls_compress_tensor(comp_buf + data_pos,
                                  ZSTD_compressBound(sz),
                                  tensor_data + data_pos, sz, m);
            meta_count++;
        } else {
            m->comp_type   = POGLS_COMP_RAW;
            m->comp_nbytes = (uint32_t)sz;
            meta_count++;
        }

        data_pos += sz;
    }

    fclose(f);

    /* Phase 2: Build header */
    PoglsStoreHeader hdr;
    pogls_meta_header_init(&hdr);
    hdr.n_tensors          = meta_count;
    hdr.flags              = POGLS_FLAG_HAS_TMETA;
    hdr.tensor_meta_off    = sizeof(hdr) + POGLS_INDEX_SZ;
    hdr.tensor_meta_count  = meta_count;

    uint64_t data_off = pogls_meta_data_off(&hdr);

    /* Phase 3: Write output file */
    fprintf(stderr, "[pogls] writing %s with %u tensors%s...\n",
            out_path, meta_count, use_compress ? " (compressed)" : "");

    /* Build raw data section (either raw or compressed) */
    uint64_t out_data_sz = 0;
    data_pos = 0;
    /* First pass: compute total output size */
    for (uint32_t i = 0; i < meta_count; i++) {
        uint32_t stored_sz = meta_arr[i].comp_nbytes;
        out_data_sz += stored_sz;
    }

    uint8_t *out_data = (uint8_t*)calloc(out_data_sz, 1);
    if (!out_data) { fprintf(stderr, "ERROR: OOM\n"); goto cleanup; }

    uint64_t out_pos = 0;
    uint64_t src_pos = 0;
    for (uint64_t gi = 0, mi = 0; gi < idx.n_tensors && mi < meta_count; gi++) {
        uint32_t addr = name_to_addr(idx.names[gi], max_layer);
        if (addr >= POGLS_MAX_ADDR) continue;
        PoglsTensorMeta *m = &meta_arr[mi];

        if (m->comp_type == POGLS_COMP_RAW) {
            memcpy(out_data + out_pos, tensor_data + src_pos, m->nbytes_orig);
        } else {
            memcpy(out_data + out_pos, comp_buf + src_pos, m->comp_nbytes);
        }
        out_pos += m->comp_nbytes;
        src_pos += m->nbytes_orig;
        mi++;
    }

    /* Write file */
    FILE *out = fopen(out_path, "wb");
    if (!out) { fprintf(stderr, "ERROR: can't write %s\n", out_path); goto cleanup; }

    fwrite(&hdr, sizeof(hdr), 1, out);
    /* Write empty index (same size as v1) */
    {
        uint8_t zero[4096] = {0};
        uint64_t remaining = POGLS_INDEX_SZ;
        while (remaining > 0) {
            uint64_t chunk = remaining;
            if (chunk > sizeof(zero)) chunk = sizeof(zero);
            fwrite(zero, chunk, 1, out);
            remaining -= chunk;
        }
    }
    /* Write tensor meta */
    fwrite(meta_arr, sizeof(PoglsTensorMeta), meta_count, out);
    /* Write data */
    fwrite(out_data, out_data_sz, 1, out);
    fclose(out);

    {
        uint64_t file_sz = data_off + out_data_sz;
        double ratio = total_raw > 0 ? (double)total_raw / (double)out_data_sz : 1.0;
        fprintf(stderr, "[pogls] wrote %llu bytes (%.1f MB), ratio=%.2f\n",
                (unsigned long long)file_sz, file_sz / (1024.*1024), ratio);
        if (use_compress) {
            uint64_t saved = total_raw - out_data_sz;
            fprintf(stderr, "[pogls] compression saved %llu MB (%.1f%%)\n",
                    (unsigned long long)(saved >> 20),
                    100.0 * (1.0 - (double)out_data_sz / (double)total_raw));
        }
    }

cleanup:
    free(tensor_data);
    free(comp_buf);
    free(meta_arr);
    free(out_data);
    gguf_idx_close(&idx);
    return 0;
}
