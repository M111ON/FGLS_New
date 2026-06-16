// test_hex_swap.c — Hex grid inference test (Phase B)
// Compare logits across 4 scenarios:
//   1. Baseline (no swap)
//   2. Swap cold tensors only (hot+warm intact)
//   3. Swap hot+warm tensors only (cold intact)
//   4. Swap ALL tensors (sanity check)
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <windows.h>
#include "llama.h"
#include "gguf_index.h"
#include "hex_grid.h"

#define MAX_TENSORS  4096
#define NAME_MAX 64
#define SCAN_LIMIT 0x200000

typedef struct {
    void *ptr;
    char name[NAME_MAX];
    void *orig_data;
    void *copy_data;
    size_t nbytes;
    int is_norm;
    float hotness;
} TensorInfo;

static TensorInfo tensors[MAX_TENSORS];
static int n_tensors = 0;

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
    const GGUFTensorIndex *gidx, int max)
{
    for (const uint8_t *p = start; p + 8 < end && n_tensors < max; p += 8) {
        void *cand; memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        if (!is_valid_tensor_ptr(cand)) continue;
        if (!name_wanted(tensor_name(cand), gidx)) continue;
        int dup = 0;
        for (int i = 0; i < n_tensors; i++)
            if (tensors[i].ptr == cand) { dup = 1; break; }
        if (dup) continue;
        tensors[n_tensors].ptr = cand;
        strncpy(tensors[n_tensors].name, tensor_name(cand), NAME_MAX - 1);
        tensors[n_tensors].name[NAME_MAX - 1] = 0;
        tensors[n_tensors].orig_data = tensor_data(cand);
        n_tensors++;
    }
    return n_tensors;
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

static int extract_layer(const char *name) {
    if (strncmp(name, "blk.", 4) != 0) return -1;
    int l = 0;
    for (const char *p = name + 4; *p >= '0' && *p <= '9'; p++)
        l = l * 10 + (*p - '0');
    return l;
}

static int is_norm(const char *name) {
    return strstr(name, "norm.weight") != NULL;
}

static int compute_nbytes(const void *tensor) {
    int64_t ne[4]; memcpy(ne, (const char*)tensor + 16, sizeof(ne));
    size_t nb[4]; memcpy(nb, (const char*)tensor + 48, sizeof(nb));
    size_t total = (size_t)ne[0] * nb[0];
    for (int i = 1; i < 4; i++) {
        size_t ni = (size_t)ne[i] * nb[i];
        if (ni > total) total = ni;
    }
    return (int)total;
}

static int compare_logits(const char *label, const float *a, const float *b, int n) {
    int nd = 0;
    double md = 0.0, mse = 0.0;
    for (int i = 0; i < n; i++) {
        double d = (double)b[i] - (double)a[i];
        if (fabs(d) > 1e-10) { nd++; if (fabs(d) > md) md = fabs(d); mse += d * d; }
    }
    mse = sqrt(mse / (n > 0 ? n : 1));
    fprintf(stderr, "[cmp] %-20s diff=%d/%d max_diff=%.6f RMSE=%.6f\n",
        label, nd, n, md, mse);
    return nd;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <model.gguf>\n", argv[0]); return 1; }
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

    // ---- Scan for tensor pointers ----
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(model, &mbi, sizeof(mbi));
    uint8_t *base = (uint8_t*)mbi.BaseAddress;
    uint8_t *base_end = base + mbi.RegionSize;
    if (base_end - base > 65536) base_end = base + 65536;
    scan_region(base, base_end, &gidx, MAX_TENSORS);
    void *layers = find_layers_ptr((const uint8_t*)model);
    if (layers) {
        MEMORY_BASIC_INFORMATION lmbi;
        if (VirtualQuery(layers, &lmbi, sizeof(lmbi)) && lmbi.State == MEM_COMMIT) {
            uint8_t *ls = (uint8_t*)lmbi.BaseAddress;
            uint8_t *le = ls + lmbi.RegionSize;
            if (le > ls + SCAN_LIMIT) le = ls + SCAN_LIMIT;
            scan_region(ls, le, &gidx, MAX_TENSORS);
        }
    }
    fprintf(stderr, "[scan] %d/%llu tensors found\n", n_tensors,
        (unsigned long long)gidx.n_tensors);

    // ---- Hex grid classification ----
    int n_layers = 0;
    for (int i = 0; i < n_tensors; i++) {
        int l = extract_layer(tensors[i].name);
        if (l + 1 > n_layers) n_layers = l + 1;
    }
    fprintf(stderr, "[hex] n_layers=%d\n", n_layers);

    HexGridState hg;
    hex_grid_init(&hg, 2, (float)n_layers);

    const char *hnames[MAX_TENSORS];
    for (int i = 0; i < n_tensors; i++) hnames[i] = tensors[i].name;

    HexBondGraph hbg;
    int max_bonds = n_tensors * 8;
    if (max_bonds > 32768) max_bonds = 32768;
    hex_bond_graph_init(&hbg, max_bonds);
    hex_discover_bonds(&hbg, hnames, n_tensors, &hg, 8);

    fprintf(stderr, "[hex] bonds=%d\n", hbg.n_bonds);
    float *hotness = hex_predict_hotness(&hbg, hnames, n_tensors, n_layers);
    hex_predict_print(hnames, n_tensors, hotness);
    hex_bond_graph_free(&hbg);

    for (int i = 0; i < n_tensors; i++) {
        tensors[i].hotness = hotness[i];
        tensors[i].is_norm = is_norm(tensors[i].name);
    }
    free(hotness);

    // ---- Allocate copies with sentinel flip ----
    int total_copied = 0;
    for (int i = 0; i < n_tensors; i++) {
        tensors[i].nbytes = compute_nbytes(tensors[i].ptr);
        if (tensors[i].nbytes <= 0) continue;
        tensors[i].copy_data = malloc(tensors[i].nbytes);
        if (!tensors[i].copy_data) { fprintf(stderr, "malloc fail %d\n", i); continue; }
        memcpy(tensors[i].copy_data, tensors[i].orig_data, tensors[i].nbytes);
        ((unsigned char*)tensors[i].copy_data)[0] ^= 1;  // sentinel flip
        total_copied++;
    }
    fprintf(stderr, "[copy] %d/%d tensor copies created\n", total_copied, n_tensors);

    // ---- Group classification ----
    int n_hot = 0, n_cold = 0;
    for (int i = 0; i < n_tensors; i++) {
        if (tensors[i].hotness >= 0.3f) n_hot++;
        else n_cold++;
    }
    fprintf(stderr, "[group] hot+warm=%d cold=%d (threshold=0.3)\n", n_hot, n_cold);

    // ---- Logit capture function ----
    const float *get_logits(struct llama_context *ctx, int *n_out) {
        const struct llama_vocab *vocab = llama_model_get_vocab(model);
        *n_out = llama_vocab_n_tokens(vocab);
        return llama_get_logits_ith(ctx, 0);
    }

    float *logits[4] = {0}; // 0=baseline, 1=swap_cold, 2=swap_hot, 3=swap_all
    int n_vocab = 0;
    const char *scenarios[] = {"baseline", "swap_cold", "swap_hot", "swap_all"};

    for (int s = 0; s < 4; s++) {
        struct llama_context_params cp = llama_context_default_params();
        cp.n_ctx = 256; cp.n_batch = 1;
        struct llama_context *ctx = llama_init_from_model(model, cp);
        if (!ctx) { fprintf(stderr, "ERROR: context %d\n", s); return 1; }

        // Apply swap for this scenario
        if (s >= 1) {
            for (int i = 0; i < n_tensors; i++) {
                int should_swap = 0;
                if (s == 1) should_swap = (tensors[i].hotness < 0.3f);      // swap cold
                else if (s == 2) should_swap = (tensors[i].hotness >= 0.3f); // swap hot
                else if (s == 3) should_swap = 1;                            // swap all

                if (should_swap && tensors[i].copy_data) {
                    tensor_set_data(tensors[i].ptr, tensors[i].copy_data);
                }
            }
        }

        llama_token token = 0; // BOS
        llama_batch batch = llama_batch_get_one(&token, 1);
        if (llama_decode(ctx, batch) != 0) {
            fprintf(stderr, "ERROR: decode %d\n", s); return 1;
        }

        // Restore original data
        for (int i = 0; i < n_tensors; i++) {
            if (tensors[i].copy_data) {
                tensor_set_data(tensors[i].ptr, tensors[i].orig_data);
            }
        }

        int nv;
        const float *l = get_logits(ctx, &nv);
        if (s == 0) n_vocab = nv;
        logits[s] = (float*)malloc(nv * sizeof(float));
        memcpy(logits[s], l, nv * sizeof(float));

        fprintf(stderr, "[%s] decode OK, logits[0]=%f logits[%d]=%f\n",
            scenarios[s], logits[s][0], nv - 1, logits[s][nv - 1]);

        llama_free(ctx);
    }

    // ---- Comparisons ----
    fprintf(stderr, "\n===== LOGIT COMPARISON =====\n");
    compare_logits("cold_vs_baseline", logits[0], logits[1], n_vocab);
    compare_logits("hot_vs_baseline",  logits[0], logits[2], n_vocab);
    compare_logits("all_vs_baseline",  logits[0], logits[3], n_vocab);

    // ---- Free ----
    for (int s = 0; s < 4; s++) if (logits[s]) free(logits[s]);
    for (int i = 0; i < n_tensors; i++) if (tensors[i].copy_data) free(tensors[i].copy_data);
    gguf_idx_close(&gidx);
    llama_model_free(model);
    llama_backend_free();

    fprintf(stderr, "\n===== DONE =====\n");
    return 0;
}
