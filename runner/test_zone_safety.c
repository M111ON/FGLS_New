// test_zone_safety.c — Empirical Zone Safety Map
// Systematically tests what happens when each tensor zone is corrupted.
// Zones: 0=GREEN(weight), 1=YELLOW(norm.weight), 2=RED(bias), 3=GRAY(other)
//
// Build:
//   gcc -O2 -std=c11 -I. -I../collection -I../collection/src -I../collection/core
//       -I../collection/core/core -I../collection/geopixel
//       -I../collection/geopixel/Metatron/core -I../collection/pogls_engine
//       -I../collection/geo_jump_module/include
//       -II:/llama.cpp/include -II:/llama.cpp/ggml/include
//       -o runner/test_zone_safety.exe runner/test_zone_safety.c
//       I:/llama/llama-b9528-bin-win-vulkan-x64/llama.dll
//       I:/llama/llama-b9528-bin-win-vulkan-x64/ggml.dll
//       I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-base.dll
//       I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-x64.dll
//       -lm
//
// Run:
//   cd runner && PATH=I:/llama/llama-b9528-bin-win-vulkan-x64;$env:PATH
//   ./test_zone_safety.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <windows.h>
#include <signal.h>
#include "llama.h"

/* Crash flag for SEH handling */
static volatile int g_crashed = 0;
#include "gguf_index.h"

#define MAX_TENSORS  4096
#define NAME_MAX 64
#define SCAN_LIMIT 0x200000
#define MAX_SCENARIOS 16

/* Zone classification */
#define ZONE_GREEN  0  /* weight tensors (Q8_0, ≥64KB, attn_/ffn_/output/token_embd) */
#define ZONE_YELLOW 1  /* norm.weight (critical — layer norm scales) */
#define ZONE_RED    2  /* bias (never cached, never swapped) */
#define ZONE_GRAY   3  /* other F32 tensors (non-weight, non-norm, non-bias) */

static const char *zone_names[] = {"GREEN(weight)", "YELLOW(norm.weight)", "RED(bias)", "GRAY(other)"};

typedef struct {
    void *ptr;
    char name[NAME_MAX];
    void *orig_data;
    size_t nbytes;
    int zone;
    int dtype;
} TensorInfo;

static TensorInfo tensors[MAX_TENSORS];
static int n_tensors = 0;

/* Scenario definition */
typedef struct {
    const char *name;
    int (*filter)(int idx);       /* returns 1 if tensor should be corrupted */
    int corrupt_stride;            /* 0 = full replacement with 0x42, >0 = stride XOR */
    int expected_result;           /* 0=unknown, 1=coherent, -1=garbled, -2=crash */
} Scenario;

/* ─── helpers ─── */

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

static const char* tensor_name_p(const void *tensor) { return (const char*)tensor + 256; }
static void* tensor_data_p(const void *tensor) { void *d; memcpy(&d, (const char*)tensor + 248, sizeof(d)); return d; }
static int tensor_dtype(const void *tensor) { int t; memcpy(&t, tensor, sizeof(t)); return t; }
static void tensor_set_data_p(void *tensor, void *new_data) { memcpy((char*)tensor + 248, &new_data, sizeof(void*)); }

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
        if (!name_wanted(tensor_name_p(cand), gidx)) continue;
        int dup = 0;
        for (int i = 0; i < n_tensors; i++)
            if (tensors[i].ptr == cand) { dup = 1; break; }
        if (dup) continue;
        tensors[n_tensors].ptr = cand;
        strncpy(tensors[n_tensors].name, tensor_name_p(cand), NAME_MAX - 1);
        tensors[n_tensors].name[NAME_MAX - 1] = 0;
        tensors[n_tensors].orig_data = tensor_data_p(cand);
        tensors[n_tensors].dtype = tensor_dtype(cand);
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
            if (strncmp(tensor_name_p(sub), "blk.0.", 6) == 0) return cand;
        }
    }
    return NULL;
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

/* ─── zone classifier ─── */
static int classify_zone(const char *name) {
    if (strstr(name, "bias")) return ZONE_RED;
    if (strstr(name, "norm.weight")) return ZONE_YELLOW;
    /* GREEN: weight tensors that go into SID cache */
    if (strstr(name, ".weight") &&
        (strstr(name, "attn_") || strstr(name, "ffn_") ||
         strcmp(name, "output.weight") == 0 ||
         strcmp(name, "token_embd.weight") == 0))
        return ZONE_GREEN;
    return ZONE_GRAY;
}

/* ─── corruption: create perturbed copy ─── */
static uint8_t* make_corrupt_copy(const void *data, size_t nbytes, int stride) {
    if (nbytes == 0) return NULL;
    uint8_t *copy = (uint8_t*)malloc(nbytes);
    if (!copy) return NULL;
    memcpy(copy, data, nbytes);
    if (stride <= 0) {
        /* Full replacement with sentinel */
        copy[0] ^= 0x42;
        if (nbytes > 1) copy[nbytes - 1] ^= 0x42;
    } else {
        /* Stride-based XOR (cosplay-style) */
        for (size_t i = 0; i < nbytes; i += (size_t)stride)
            copy[i] ^= 0x01;
    }
    return copy;
}

/* ─── logit comparison ─── */
static int compare_logits(const char *label, const float *a, const float *b, int n) {
    int nd = 0;
    double md = 0.0, mse = 0.0, dot = 0.0, na2 = 0.0, nb2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = (double)b[i] - (double)a[i];
        if (fabs(d) > 1e-10) { nd++; if (fabs(d) > md) md = fabs(d); mse += d * d; }
        dot += (double)a[i] * (double)b[i];
        na2 += (double)a[i] * (double)a[i];
        nb2 += (double)b[i] * (double)b[i];
    }
    mse = sqrt(mse / (n > 0 ? n : 1));
    double cos_sim = (na2 > 0 && nb2 > 0) ? dot / (sqrt(na2) * sqrt(nb2)) : 0.0;
    fprintf(stderr, "[cmp] %-28s diff=%d/%d (%.1f%%) max_diff=%.6f RMSE=%.6f cos=%.6f\n",
        label, nd, n, 100.0 * nd / n, md, mse, cos_sim);
    return nd;
}

/* ─── scenario filters ─── */
static int filter_green(int i)    { return tensors[i].zone == ZONE_GREEN; }
static int filter_yellow(int i)   { return tensors[i].zone == ZONE_YELLOW; }
static int filter_red(int i)      { return tensors[i].zone == ZONE_RED; }
static int filter_gray(int i)     { return tensors[i].zone == ZONE_GRAY; }
static int filter_green_yellow(int i) { return tensors[i].zone == ZONE_GREEN || tensors[i].zone == ZONE_YELLOW; }
static int filter_all(int i)      { return 1; }
static int filter_non_yellow(int i) { return tensors[i].zone != ZONE_YELLOW; }

/* ─── main ─── */
static int g_single_scenario = -1;  /* -1 = run all */

int main(int argc, char **argv) {
    const char *model_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            g_single_scenario = atoi(argv[++i]);
        } else if (model_path == NULL) {
            model_path = argv[i];
        }
    }
    if (!model_path) { fprintf(stderr, "Usage: %s [--scenario N] <model.gguf>\n", argv[0]); return 1; }
    setbuf(stderr, NULL);

    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params lmp = llama_model_default_params();
    lmp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, lmp);
    if (!model) { fprintf(stderr, "ERROR: model load\n"); return 1; }

    GGUFTensorIndex gidx;
    if (gguf_idx_open(model_path, &gidx) != 0) {
        fprintf(stderr, "ERROR: gguf_idx_open\n");
        llama_model_free(model); llama_backend_free(); return 1;
    }
    fprintf(stderr, "[GGUF] %llu tensors\n", (unsigned long long)gidx.n_tensors);

    /* ── Scan for tensor pointers ── */
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

    /* ── Classify & count each zone ── */
    int zone_counts[4] = {0};
    for (int i = 0; i < n_tensors; i++) {
        tensors[i].nbytes = compute_nbytes(tensors[i].ptr);
        tensors[i].zone = classify_zone(tensors[i].name);
        zone_counts[tensors[i].zone]++;
    }
    fprintf(stderr, "\n===== ZONE COUNTS =====\n");
    for (int z = 0; z < 4; z++)
        fprintf(stderr, "  %s: %d tensors\n", zone_names[z], zone_counts[z]);
    fprintf(stderr, "  Total: %d\n", n_tensors);

    /* ── Print tensor list by zone ── */
    fprintf(stderr, "\n===== TENSOR LIST BY ZONE =====\n");
    for (int z = 0; z < 4; z++) {
        fprintf(stderr, "\n--- %s ---\n", zone_names[z]);
        for (int i = 0; i < n_tensors; i++) {
            if (tensors[i].zone == z)
                fprintf(stderr, "  %s (dtype=%d, %zu bytes)\n",
                    tensors[i].name, tensors[i].dtype, tensors[i].nbytes);
        }
    }

    /* ── Define scenarios ── */
    Scenario scenarios[] = {
        {"01-green_stride64",    filter_green,  64,  1},  /* known safe */
        {"02-green_stride32",    filter_green,  32, -1},  /* experiment showed garbled */
        {"03-green_stride8",     filter_green,   8, -1},  /* aggressive */
        {"04-green_fullcorrupt", filter_green,   0,  0},  /* full sentinel flip */
        {"05-yellow_norm_weight", filter_yellow, 0,  0},  /* UNKNOWN — critical zone test */
        {"06-red_bias",          filter_red,     0,  0},  /* UNKNOWN */
        {"07-gray_other",        filter_gray,    0,  0},  /* UNKNOWN */
        {"08-green+yellow",      filter_green_yellow, 64, 0},  /* green safe + yellow */
        {"09-all_non_yellow",    filter_non_yellow, 64,  0},  /* everything except norm */
        {"10-all_tensors",       filter_all,    64,  0},  /* EVERYTHING */
    };
    int n_scenarios = sizeof(scenarios) / sizeof(scenarios[0]);

    fprintf(stderr, "\n===== SCENARIO PLAN =====\n");
    for (int s = 0; s < n_scenarios; s++)
        fprintf(stderr, "  %s: %s (stride=%d)\n",
            scenarios[s].name, zone_names[s < 4 ? ZONE_GREEN : (s-4 < 4 ? (s-4) : 0)],
            scenarios[s].corrupt_stride);

    /* ── Run each scenario ── */
    fprintf(stderr, "\n===== RUNNING SCENARIOS =====\n\n");

    float *baseline_logits = NULL;
    int n_vocab = 0;

    for (int s = 0; s < n_scenarios; s++) {
        if (g_single_scenario >= 0 && s != g_single_scenario) continue;
        /* Create perturbed copies for this scenario */
        uint8_t **corrupt_copies = (uint8_t**)calloc((size_t)n_tensors, sizeof(uint8_t*));
        int n_corrupted = 0;

        fprintf(stderr, "\n--- Scenario %s ---\n", scenarios[s].name);

        for (int i = 0; i < n_tensors; i++) {
            if (!scenarios[s].filter(i)) continue;
            corrupt_copies[i] = make_corrupt_copy(tensors[i].orig_data,
                tensors[i].nbytes, scenarios[s].corrupt_stride);
            if (corrupt_copies[i]) n_corrupted++;
        }
        fprintf(stderr, "[setup] corrupting %d tensors\n", n_corrupted);

        /* Create context & apply corruption */
        struct llama_context_params cp = llama_context_default_params();
        cp.n_ctx = 256; cp.n_batch = 1;
        cp.n_threads = 4; cp.n_threads_batch = 4;

        struct llama_context *ctx = llama_init_from_model(model, cp);
        if (!ctx) {
            fprintf(stderr, "[%s] CRASH at context creation!\n", scenarios[s].name);
            for (int i = 0; i < n_tensors; i++) free(corrupt_copies[i]);
            free(corrupt_copies);
            continue;
        }

        /* Apply corruption via pointer swap */
        for (int i = 0; i < n_tensors; i++) {
            if (corrupt_copies[i])
                tensor_set_data_p(tensors[i].ptr, corrupt_copies[i]);
        }

        /* Decode */
        llama_token token = 0;
        llama_batch batch = llama_batch_get_one(&token, 1);
        int decode_ok = (llama_decode(ctx, batch) == 0);

        /* Restore original data immediately */
        for (int i = 0; i < n_tensors; i++) {
            if (corrupt_copies[i])
                tensor_set_data_p(tensors[i].ptr, tensors[i].orig_data);
        }

        if (!decode_ok) {
            fprintf(stderr, "[%s] CRASH (return code) at llama_decode!\n", scenarios[s].name);
            scenarios[s].expected_result = -2;
            llama_free(ctx);
            for (int i = 0; i < n_tensors; i++) free(corrupt_copies[i]);
            free(corrupt_copies);
            continue;
        }

        /* Get logits */
        const struct llama_vocab *lv = llama_model_get_vocab(model);
        int nv = llama_vocab_n_tokens(lv);
        const float *logits = llama_get_logits_ith(ctx, 0);

        /* Token sampling */
        int sampled_token = -1;
        float max_logit = -1e30f;
        for (int j = 0; j < nv; j++) {
            if (logits[j] > max_logit) { max_logit = logits[j]; sampled_token = j; }
        }
        const char *tok_str = llama_vocab_get_text(lv, (llama_token)sampled_token);

        fprintf(stderr, "[%s] decode OK, logits[0]=%.4f first_token=%d '%s'\n",
            scenarios[s].name, logits[0], sampled_token, tok_str);

        /* Compare to baseline (scenario 0) */
        if (s == 0) {
            baseline_logits = (float*)malloc((size_t)nv * sizeof(float));
            memcpy(baseline_logits, logits, (size_t)nv * sizeof(float));
            n_vocab = nv;
        } else {
            compare_logits(scenarios[s].name, baseline_logits, logits, n_vocab);
        }

        llama_free(ctx);

        /* Free corrupt copies */
        for (int i = 0; i < n_tensors; i++) free(corrupt_copies[i]);
        free(corrupt_copies);
    }

    /* ── Summary table ── */
    fprintf(stderr, "\n\n========================================\n");
    fprintf(stderr, "      ZONE SAFETY TEST — RESULTS       \n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "%-30s %-12s %-12s\n", "Scenario", "Crash?", "Coherent?");
    fprintf(stderr, "%-30s %-12s %-12s\n", "--------", "-------", "---------");
    for (int s = 0; s < n_scenarios; s++) {
        const char *crash_str = scenarios[s].expected_result == -2 ? "CRASH" : "OK";
        const char *coherent_str = "?";
        fprintf(stderr, "%-30s %-12s %-12s\n", scenarios[s].name, crash_str, coherent_str);
    }
    fprintf(stderr, "========================================\n\n");

    /* ── Cleanup ── */
    free(baseline_logits);
    gguf_idx_close(&gidx);
    llama_model_free(model);
    llama_backend_free();
    fprintf(stderr, "===== DONE =====\n");
    return 0;
}
