// test_swap_all.c — Generic tensor swap test using GGUF metadata + model scan
// Proves: backend reads tensor->data PER-DECODE, not cached at context init.
// Swap tensor->data between llama_decode calls → different logits.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <windows.h>
#include "llama.h"
#include "gguf_index.h"

#define MAX_TENSORS  4096
#define NAME_MAX 64
#define SCAN_LIMIT 0x200000

static int safe_ptr(const void *p) {
    if (!p || (uintptr_t)p < 0x10000) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)
        && mbi.State == MEM_COMMIT
        && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}

static int is_valid_tensor_ptr(const void *cand) {
    if (!cand || (uintptr_t)cand < 0x10000) return 0;
    if (!safe_ptr((const char*)cand + 256 + 63)) return 0;
    const char *name = (const char*)cand + 256;
    if (name[0] < 32 || name[0] > 126) return 0;
    int nlen = (int)strnlen(name, 64);
    if (nlen < 3 || nlen >= 64) return 0;
    for (int i = 0; i < nlen; i++) {
        char c = name[i];
        if (c == '\\' || c == '/' || c == '=' || c == ':' || c == ';') return 0;
    }
    if (!((name[0] >= 'a' && name[0] <= 'z') ||
          (name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= '0' && name[0] <= '9') ||
          name[0] == '_' || name[0] == '.')) return 0;
    int64_t ne0; memcpy(&ne0, (const char*)cand + 16, sizeof(ne0));
    if (ne0 <= 0 || ne0 > 10000000) return 0;
    int type; memcpy(&type, cand, sizeof(type));
    if (type < 0 || type > 43) return 0;
    void *data; memcpy(&data, (const char*)cand + 248, sizeof(data));
    if (!data || (uintptr_t)data < 0x10000) return 0;
    return 1;
}

static const char* tensor_name(const void *tensor) { return (const char*)tensor + 256; }
static void* tensor_data(const void *tensor) { void *d; memcpy(&d, (const char*)tensor + 248, sizeof(d)); return d; }
static void tensor_set_data(void *tensor, void *new_data) { memcpy((char*)tensor + 248, &new_data, sizeof(void*)); }

static int name_wanted(const char *name, const GGUFTensorIndex *gidx) {
    for (uint64_t i = 0; i < gidx->n_tensors; i++)
        if (strcmp(name, gidx->names[i]) == 0) return 1;
    return 0;
}

static int scan_region(const uint8_t *start, const uint8_t *end,
    const GGUFTensorIndex *gidx, void **ptrs, int max, int *count)
{
    for (const uint8_t *p = start; p + 8 < end && *count < max; p += 8) {
        void *cand; memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        if (!is_valid_tensor_ptr(cand)) continue;
        if (!name_wanted(tensor_name(cand), gidx)) continue;
        int dup = 0;
        for (int i = 0; i < *count; i++)
            if (ptrs[i] == cand) { dup = 1; break; }
        if (dup) continue;
        ptrs[(*count)++] = cand;
    }
    return *count;
}

static void* find_layers_ptr(const void *model_alloc) {
    uint8_t *end = (uint8_t*)model_alloc + 65536;
    for (uint8_t *p = (uint8_t*)model_alloc; p + 8 < end; p += 8) {
        void *cand; memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        if (!safe_ptr(cand) || !safe_ptr((uint8_t*)cand + 4096 - 1)) continue;
        for (int off = 0; off < 4096; off += 8) {
            void *sub; memcpy(&sub, (uint8_t*)cand + off, sizeof(sub));
            if (!is_valid_tensor_ptr(sub)) continue;
            if (strncmp(tensor_name(sub), "blk.0.", 6) == 0) return cand;
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf>\n", argv[0]);
        return 1;
    }
    const char *model_path = argv[1];

    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model *model = llama_model_load_from_file(model_path, llama_model_default_params());
    if (!model) { fprintf(stderr, "ERROR: model load\n"); return 1; }

    GGUFTensorIndex gidx;
    if (gguf_idx_open(model_path, &gidx) != 0) {
        fprintf(stderr, "ERROR: gguf_idx_open\n");
        llama_model_free(model); llama_backend_free(); return 1;
    }
    fprintf(stderr, "[GGUF] %llu tensors\n", (unsigned long long)gidx.n_tensors);

    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(model, &mbi, sizeof(mbi));
    uint8_t *base = (uint8_t*)mbi.BaseAddress;
    uint8_t *base_end = base + mbi.RegionSize;
    if (base_end - base > 65536) base_end = base + 65536;

    void *ptrs[MAX_TENSORS] = {0};
    int n = 0;
    scan_region(base, base_end, &gidx, ptrs, MAX_TENSORS, &n);
    void *layers = find_layers_ptr((const uint8_t*)model);
    if (layers) {
        MEMORY_BASIC_INFORMATION lmbi;
        if (VirtualQuery(layers, &lmbi, sizeof(lmbi)) && lmbi.State == MEM_COMMIT) {
            uint8_t *ls = (uint8_t*)lmbi.BaseAddress;
            uint8_t *le = ls + lmbi.RegionSize;
            if (le > ls + SCAN_LIMIT) le = ls + SCAN_LIMIT;
            scan_region(ls, le, &gidx, ptrs, MAX_TENSORS, &n);
        }
    }
    fprintf(stderr, "[scan] %d/%llu tensors found\n", n, (unsigned long long)gidx.n_tensors);

    // Find output.weight index
    int out_idx = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(tensor_name(ptrs[i]), "output.weight") == 0) { out_idx = i; break; }
    if (out_idx < 0) { fprintf(stderr, "ERROR: output.weight not found\n"); return 1; }

    // Create copy of output.weight data, flip first byte
    size_t out_nbytes = 0;
    { int64_t ne[4]; memcpy(ne, (const char*)ptrs[out_idx] + 16, sizeof(ne));
      size_t nb[4]; memcpy(nb, (const char*)ptrs[out_idx] + 48, sizeof(nb));
      out_nbytes = (size_t)ne[0] * nb[0];
      for (int i = 1; i < 4; i++) { size_t ni = (size_t)ne[i] * nb[i]; if (ni > out_nbytes) out_nbytes = ni; } }

    void *out_data_orig = tensor_data(ptrs[out_idx]);
    void *out_copy = malloc(out_nbytes);
    memcpy(out_copy, out_data_orig, out_nbytes);
    ((unsigned char*)out_copy)[0] ^= 1;
    fprintf(stderr, "[copy] output.weight: %zu bytes, flipped first byte 0x%02x->0x%02x\n",
        out_nbytes, ((unsigned char*)out_data_orig)[0] ^ 1, ((unsigned char*)out_copy)[0]);

    // ----- One context, two decodes -----
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 256; cparams.n_batch = 1;
    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "ERROR: context\n"); return 1; }

    llama_token tokens[1] = {0};
    llama_batch batch = llama_batch_get_one(tokens, 1);
    int n_vocab = llama_vocab_n_tokens(llama_model_get_vocab(model));

    // Decode 1: original data
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "ERROR: decode1\n"); return 1; }
    float *logits_orig = (float*)malloc(n_vocab * sizeof(float));
    memcpy(logits_orig, llama_get_logits_ith(ctx, 0), n_vocab * sizeof(float));
    fprintf(stderr, "[decode1] logits[0]=%f logits[%d]=%f (original data)\n",
        logits_orig[0], n_vocab - 1, logits_orig[n_vocab - 1]);

    // SWAP: output.weight->data = copy (flipped)
    tensor_set_data(ptrs[out_idx], out_copy);

    // Decode 2: swapped data (same context)
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "ERROR: decode2\n"); return 1; }
    float *logits_swap = (float*)malloc(n_vocab * sizeof(float));
    memcpy(logits_swap, llama_get_logits_ith(ctx, 0), n_vocab * sizeof(float));
    fprintf(stderr, "[decode2] logits[0]=%f logits[%d]=%f (swapped output.weight)\n",
        logits_swap[0], n_vocab - 1, logits_swap[n_vocab - 1]);

    // Compare
    int nd = 0; double md = 0.0;
    for (int i = 0; i < n_vocab; i++) {
        double d = (double)logits_swap[i] - (double)logits_orig[i];
        if (fabs(d) > 1e-10) { nd++; if (fabs(d) > md) md = fabs(d); }
    }
    fprintf(stderr, "\n*** VERDICT: %s ***\n",
        nd > 0 ? "SWAP BETWEEN DECODES WORKS (backed reads tensor->data per-decode)"
               : "NO EFFECT (scheduler caches tensor->data at init?)");
    fprintf(stderr, "diff: %d / %d, max_diff: %g\n", nd, n_vocab, md);

    // Restore
    tensor_set_data(ptrs[out_idx], out_data_orig);

    llama_free(ctx);
    free(logits_orig); free(logits_swap); free(out_copy);
    gguf_idx_close(&gidx);
    llama_model_free(model);
    llama_backend_free();
    return nd > 0 ? 0 : 1;
}
