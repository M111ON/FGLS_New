// test_phase_b.c — Phase B: Corrupt hot vs cold zone, compare logits
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
#define SCAN_LIMIT 0x200000

typedef struct { void *ptr; char name[64]; void *orig_data; int nbytes; float hotness; int is_norm; } TensorInfo;
static TensorInfo T[MAX_TENSORS];
static int nT = 0;

static int safe_ptr(const void *p) {
    if (!p || (uintptr_t)p < 0x10000) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT
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

static void* tensor_data_p(const void *t) { void *d; memcpy(&d, (const char*)t + 248, sizeof(d)); return d; }
static void tensor_set_data_p(void *t, void *nd) { memcpy((char*)t + 248, &nd, sizeof(void*)); }

static int name_wanted(const char *name, const GGUFTensorIndex *g) {
    for (uint64_t i = 0; i < g->n_tensors; i++) if (strcmp(name, g->names[i]) == 0) return 1;
    return 0;
}

static void scan(const uint8_t *s, const uint8_t *e, const GGUFTensorIndex *g, int max) {
    for (const uint8_t *p = s; p + 8 < e && nT < max; p += 8) {
        void *cand; memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        if (!is_valid_tensor_ptr(cand)) continue;
        if (!name_wanted((const char*)cand + 256, g)) continue;
        int dup = 0; for (int i = 0; i < nT; i++) if (T[i].ptr == cand) { dup = 1; break; }
        if (dup) continue;
        T[nT].ptr = cand; strncpy(T[nT].name, (const char*)cand + 256, 63); T[nT].name[63] = 0;
        T[nT].orig_data = tensor_data_p(cand); nT++;
    }
}

static void* find_layers(const void *mod) {
    uint8_t *end = (uint8_t*)mod + 65536;
    for (uint8_t *p = (uint8_t*)mod; p + 8 < end; p += 8) {
        void *cand; memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        if (!safe_ptr(cand) || !safe_ptr((uint8_t*)cand + 4096 - 1)) continue;
        for (int off = 0; off < 4096; off += 8) {
            void *sub; memcpy(&sub, (uint8_t*)cand + off, sizeof(sub));
            if (!is_valid_tensor_ptr(sub)) continue;
            if (strncmp((const char*)sub + 256, "blk.0.", 6) == 0) return cand;
        }
    }
    return NULL;
}

// Scan model struct: from VirtualQuery base, limit to mbi.RegionSize (max 64KB)
static void scan_model_struct(const void *model, const GGUFTensorIndex *g) {
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(model, &mbi, sizeof(mbi));
    uint8_t *base = (uint8_t*)mbi.BaseAddress;
    size_t scan_sz = mbi.RegionSize;
    if (scan_sz > 65536) scan_sz = 65536;
    uint8_t *end = base + scan_sz;
    fprintf(stderr, "[scan] model struct: base=%p model=%p end=%p region=%zu\n",
        (void*)base, model, (void*)end, (size_t)mbi.RegionSize);
    scan(base, end, g, MAX_TENSORS);
    fprintf(stderr, "[scan] after model struct: %d tensors\n", nT);
}

// Scan the layers heap allocation
static void scan_layers_heap(void *layers, const GGUFTensorIndex *g) {
    MEMORY_BASIC_INFORMATION lmbi;
    if (VirtualQuery(layers, &lmbi, sizeof(lmbi)) && lmbi.State == MEM_COMMIT) {
        uint8_t *ls = (uint8_t*)lmbi.BaseAddress;
        uint8_t *le = ls + lmbi.RegionSize;
        if (le > ls + 0x200000) le = ls + 0x200000;
        fprintf(stderr, "[scan] layers heap: base=%p end=%p\n", (void*)ls, (void*)le);
        scan(ls, le, g, MAX_TENSORS);
    }
}

// Also try scanning up to 256KB forward from model for non-layer tensors
static void scan_model_forward(const void *model, const GGUFTensorIndex *g) {
    uint8_t *start = (uint8_t*)model;
    uint8_t *end = start + 0x40000; // 256KB
    scan(start, end, g, MAX_TENSORS);
}

static int extract_layer(const char *name) {
    if (strncmp(name, "blk.", 4) != 0) return -1;
    int l = 0; for (const char *p = name + 4; *p >= '0' && *p <= '9'; p++) l = l * 10 + (*p - '0');
    return l;
}

static int compute_nbytes(const void *t) {
    int64_t ne[4]; memcpy(ne, (const char*)t + 16, sizeof(ne));
    size_t nb[4]; memcpy(nb, (const char*)t + 48, sizeof(nb));
    size_t total = (size_t)ne[0] * nb[0];
    for (int i = 1; i < 4; i++) { size_t ni = (size_t)ne[i] * nb[i]; if (ni > total) total = ni; }
    return (int)total;
}

// ---- Save/compare ----
static void save_logits(struct llama_context *ctx, const char *path, int *n_out) {
    const struct llama_vocab *v = llama_model_get_vocab(llama_get_model(ctx));
    int nv = llama_vocab_n_tokens(v);
    float *l = llama_get_logits_ith(ctx, 0);
    FILE *fp = fopen(path, "wb"); fwrite(&nv, sizeof(nv), 1, fp); fwrite(l, sizeof(float), nv, fp); fclose(fp);
    fprintf(stderr, "[save] %s (%d logits, first=%.4f)\n", path, nv, l[0]);
    if (n_out) *n_out = nv;
}

static int compare(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { fprintf(stderr, "ERROR: open\n"); return 0; }
    int na, nb; fread(&na, sizeof(na), 1, fa); fread(&nb, sizeof(nb), 1, fb);
    if (na != nb) { fprintf(stderr, "vocab mismatch %d vs %d\n", na, nb); fclose(fa); fclose(fb); return 0; }
    float *la = malloc(na * sizeof(float)), *lb = malloc(nb * sizeof(float));
    fread(la, sizeof(float), na, fa); fread(lb, sizeof(float), nb, fb);
    fclose(fa); fclose(fb);
    int nd = 0; double md = 0.0, mse = 0.0;
    for (int i = 0; i < na; i++) {
        double d = (double)lb[i] - (double)la[i];
        if (fabs(d) > 1e-10) { nd++; if (fabs(d) > md) md = fabs(d); mse += d * d; }
    }
    mse = sqrt(mse / na);
    printf("[cmp] %s vs %s: diff=%d/%d (%.1f%%) max_diff=%.6f RMSE=%.6f\n",
        a, b, nd, na, 100.0 * nd / na, md, mse);
    free(la); free(lb);
    return nd;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <model.gguf>\n", argv[0]); return 1; }
    setbuf(stderr, NULL);

    llama_backend_init();
    // Force CPU: load CPU backend before model load to avoid Vulkan
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params lmp = llama_model_default_params();
    lmp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], lmp);
    if (!model) { fprintf(stderr, "ERROR: model\n"); return 1; }

    GGUFTensorIndex gidx;
    if (gguf_idx_open(argv[1], &gidx) != 0) { fprintf(stderr, "ERROR: gguf\n"); return 1; }
    fprintf(stderr, "[GGUF] %llu tensors\n", (unsigned long long)gidx.n_tensors);

    // Scan — multiple tiers to catch all tensor structs
    scan_model_struct(model, &gidx);
    void *layers = find_layers((const uint8_t*)model);
    if (layers) {
        fprintf(stderr, "[scan] layers heap found at %p\n", layers);
        scan_layers_heap(layers, &gidx);
    } else {
        fprintf(stderr, "[scan] No layers heap found, scanning forward\n");
        scan_model_forward(model, &gidx);
    }
    fprintf(stderr, "[scan] %d/%llu tensors found\n", nT, (unsigned long long)gidx.n_tensors);

    // Hex grid classification
    int n_layers = 0;
    for (int i = 0; i < nT; i++) { int l = extract_layer(T[i].name); if (l + 1 > n_layers) n_layers = l + 1; }
    fprintf(stderr, "[hex] n_layers=%d\n", n_layers);

    HexGridState hg; hex_grid_init(&hg, 2, (float)n_layers);
    const char *hn[MAX_TENSORS]; for (int i = 0; i < nT; i++) hn[i] = T[i].name;
    HexBondGraph hbg; hex_bond_graph_init(&hbg, nT * 8 > 32768 ? 32768 : nT * 8);
    hex_discover_bonds(&hbg, hn, nT, &hg, 8);
    float *hotness = hex_predict_hotness(&hbg, hn, nT, n_layers);
    hex_predict_print(hn, nT, hotness);
    hex_bond_graph_free(&hbg);
    for (int i = 0; i < nT; i++) { T[i].hotness = hotness[i]; }
    free(hotness);

    // Get sizes + create corrupted copies for all tensors
    for (int i = 0; i < nT; i++) {
        T[i].nbytes = compute_nbytes(T[i].ptr);
    }

    // Count groups
    int n_hot = 0, n_cold = 0;
    for (int i = 0; i < nT; i++) { if (T[i].hotness >= 0.3f) n_hot++; else n_cold++; }
    fprintf(stderr, "[group] hot+warm=%d cold=%d (threshold=0.3)\n", n_hot, n_cold);

    // Count groups WITHOUT norm.weight bias
    int n_hot_raw = 0, n_cold_raw = 0;
    for (int i = 0; i < nT; i++) {
        if (strstr(T[i].name, "norm.weight") != NULL) continue;
        if (T[i].hotness >= 0.3f) n_hot_raw++; else n_cold_raw++;
    }
    fprintf(stderr, "[group] (non-norm) hot+warm=%d cold=%d\n", n_hot_raw, n_cold_raw);

    // Use MIN(hot, cold) as corrupt count for fair comparison
    int n_corrupt = n_hot_raw < n_cold_raw ? n_hot_raw : n_cold_raw;
    fprintf(stderr, "[group] will corrupt %d tensors in each zone (fair pairs)\n", n_corrupt);

    // ---- Corrupted copies ----
    // For fair comparison, we corrupt a COPY of tensor data (not original)
    // Each test uses its own copy with same corruption pattern
    uint8_t **corrupt_copy = malloc(nT * sizeof(uint8_t*));
    for (int i = 0; i < nT; i++) {
        if (strstr(T[i].name, "norm.weight") != NULL) { corrupt_copy[i] = NULL; continue; }
        corrupt_copy[i] = calloc(1, T[i].nbytes);
        corrupt_copy[i][0] = 0x42; // synthetic corrupt: not zeros, not original
    }
    fprintf(stderr, "[copy] %d synthetic corrupt copies created\n", nT - 24);

    // ---- Four contexts, one per scenario ----
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 256; cp.n_batch = 1;
    llama_token tok = 0;
    llama_batch batch = llama_batch_get_one(&tok, 1);

    // Scenario 1: Baseline (no corruption)
    {
        struct llama_context *ctx = llama_init_from_model(model, cp);
        llama_decode(ctx, batch);
        save_logits(ctx, "base.bin", NULL);
        llama_free(ctx);
    }

    // Scenario 2: Corrupt HOT zone only
    {
        struct llama_context *ctx = llama_init_from_model(model, cp);
        int done = 0;
        for (int i = 0; done < n_corrupt && i < nT; i++) {
            if (strstr(T[i].name, "norm.weight") != NULL) continue;
            if (T[i].hotness >= 0.3f && corrupt_copy[i]) {
                tensor_set_data_p(T[i].ptr, corrupt_copy[i]);
                done++;
            }
        }
        fprintf(stderr, "[corrupt] hot zone: swapped %d tensors\n", done);
        llama_decode(ctx, batch);
        save_logits(ctx, "hot_corrupt.bin", NULL);
        llama_free(ctx);
        for (int i = 0; i < nT; i++) tensor_set_data_p(T[i].ptr, T[i].orig_data);
    }

    // Scenario 3: Corrupt COLD zone only
    {
        struct llama_context *ctx = llama_init_from_model(model, cp);
        int done = 0;
        for (int i = 0; done < n_corrupt && i < nT; i++) {
            if (strstr(T[i].name, "norm.weight") != NULL) continue;
            if (T[i].hotness < 0.3f && corrupt_copy[i]) {
                tensor_set_data_p(T[i].ptr, corrupt_copy[i]);
                done++;
            }
        }
        fprintf(stderr, "[corrupt] cold zone: swapped %d tensors\n", done);
        llama_decode(ctx, batch);
        save_logits(ctx, "cold_corrupt.bin", NULL);
        llama_free(ctx);
        for (int i = 0; i < nT; i++) tensor_set_data_p(T[i].ptr, T[i].orig_data);
    }

    // Scenario 4: Corrupt ALL (sanity check)
    {
        struct llama_context *ctx = llama_init_from_model(model, cp);
        for (int i = 0; i < nT; i++) {
            if (strstr(T[i].name, "norm.weight") != NULL) continue;
            if (corrupt_copy[i]) tensor_set_data_p(T[i].ptr, corrupt_copy[i]);
        }
        fprintf(stderr, "[corrupt] all zone: swapped all non-norm tensors\n");
        llama_decode(ctx, batch);
        save_logits(ctx, "all_corrupt.bin", NULL);
        llama_free(ctx);
        for (int i = 0; i < nT; i++) tensor_set_data_p(T[i].ptr, T[i].orig_data);
    }

    // ---- COMPARE ----
    printf("\n===== PHASE B RESULTS =====\n");
    compare("base.bin", "base.bin");
    compare("base.bin", "hot_corrupt.bin");
    compare("base.bin", "cold_corrupt.bin");
    compare("base.bin", "all_corrupt.bin");
    printf("===========================\n");

    for (int i = 0; i < nT; i++) if (corrupt_copy[i]) free(corrupt_copy[i]);
    free(corrupt_copy);
    gguf_idx_close(&gidx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
