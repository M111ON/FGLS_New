/*
 * gguf_to_pogls.c — Convert GGUF model → POGLS flat tensor store
 *
 * Build: gcc -O2 -std=c11 -I. -I../collection -I../collection/geopixel/geofield
 *        -II:/llama.cpp/include -II:/llama.cpp/ggml/include
 *        -o gguf_to_pogls.exe gguf_to_pogls.c llama.dll ggml.dll
 *
 * Usage: gguf_to_pogls.exe model.gguf output.pogls
 *
 * Reads GGUF via llama.h API, for each tensor:
 *   name → dram_addr via dt_name_to_addr()
 *   raw data → PoglsStore slot
 * Writes flat .pogls file indexed by dram_addr.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "llama.h"
#include "pogls_store.h"

/* dram_addr mapping (from geo_jump dt_name_to_addr — simplified) */
static uint32_t name_to_addr(const char *name) {
    /* blk.N.name → hash to 0..20735 */
    if (!name || name[0] == 0) return 0;
    const char *dot = strchr(name, '.');
    if (!dot) return 0;
    int layer = 0;
    if (name[3] >= '0' && name[3] <= '9') {
        layer = atoi(name + 4); /* skip "blk." */
    }
    /* skip past "blk.N." to get tensor type */
    const char *t = dot + 1;
    t = strchr(t, '.');
    if (!t) return 0;
    t++; /* past second dot */

    /* Hash the remaining name to a spoke+slot within layer range */
    uint32_t h = 0;
    for (const char *p = t; *p; p++)
        h = h * 131 + *p;

    uint32_t layer_stride = 20736 / 37; /* 560 */
    uint32_t spoke = h % 6;
    uint32_t slot  = (h / 6) % 96;
    uint32_t addr  = (uint32_t)layer * layer_stride + spoke * 96 + slot;
    return addr % 20736;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: gguf_to_pogls.exe model.gguf output.pogls\n");
        return 1;
    }
    const char *model_path = argv[1];
    const char *out_path   = argv[2];

    /* Init llama backend */
    llama_backend_init();

    /* Load model params */
    llama_model_params mparams = llama_model_default_params();
    mparams.use_mmap  = false;  /* need data accessible */
    mparams.use_mlock = false;

    llama_model *model = llama_load_model_from_file(model_path, mparams);
    if (!model) {
        fprintf(stderr, "ERROR: failed to load model %s\n", model_path);
        llama_backend_free();
        return 1;
    }

    /* Enumerate tensors from model */
    int n_tensors = llama_model_n_tensors(model);
    fprintf(stderr, "[pogls] model has %d tensors\n", n_tensors);

    /* Build store */
    PoglsStore store;
    pogls_store_init(&store);

    uint64_t data_off = sizeof(PoglsStore);
    uint32_t n_written = 0;
    uint32_t n_skipped = 0;

    for (int i = 0; i < n_tensors; i++) {
        struct ggml_tensor *t = llama_model_get_tensor(model, i);
        if (!t) continue;

        const char *name = ggml_get_name(t);
        size_t sz = ggml_nelements(t) * ggml_type_size(ggml_get_type(t));
        uint32_t addr = name_to_addr(name);

        if (addr >= POGLS_MAX_ADDR) { n_skipped++; continue; }
        if (store.idx[addr].offset != 0) { n_skipped++; continue; }

        /* Read tensor data from model */
        uint8_t *buf = (uint8_t*)malloc(sz);
        if (!buf) continue;

        /* Copy from tensor data (may be on GPU or mmap) */
        memcpy(buf, t->data, sz);

        store.idx[addr].offset  = data_off;
        store.idx[addr].nbytes  = (uint32_t)sz;
        store.n_tensors++;
        n_written++;

        data_off += sz;
        /* The data will be written sequentially after the header+index */

        fprintf(stderr, "\r[pogls] %6d/%d tensors written (addr=%5u %-48s sz=%-10zu)",
                i+1, n_tensors, addr, name, sz);

        free(buf);
    }

    fprintf(stderr, "\n[pogls] done: %u written, %u skipped\n", n_written, n_skipped);

    /* Write store to file */
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot write %s\n", out_path);
        llama_free_model(model);
        llama_backend_free();
        return 1;
    }

    /* Write header + index */
    fwrite(&store, sizeof(store), 1, f);

    /* Write tensor data sequentially */
    for (uint32_t a = 0; a < POGLS_MAX_ADDR; a++) {
        if (store.idx[a].offset == 0 || store.idx[a].nbytes == 0) continue;
        struct ggml_tensor *t = llama_model_get_tensor(model, (int)a);
        if (!t) continue;
        fwrite(t->data, 1, store.idx[a].nbytes, f);
    }

    fclose(f);

    /* Verify */
    uint64_t file_sz = data_off;
    fprintf(stderr, "[pogls] wrote %s: %llu bytes (%llu MiB)\n",
            out_path, (unsigned long long)file_sz, (unsigned long long)(file_sz >> 20));

    /* Cleanup */
    llama_free_model(model);
    llama_backend_free();

    fprintf(stderr, "[pogls] OK\n");
    return 0;
}
