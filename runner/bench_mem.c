#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include "llama.h"

#pragma comment(lib, "psapi.lib")

static size_t get_working_set_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize / (1024 * 1024);
    return 0;
}

static size_t get_pagefile_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.PagefileUsage / (1024 * 1024);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf\n", argv[0]); return 1; }

    printf("=== Memory Benchmark ===\n");
    printf("Model: %s\n", argv[1]);
    printf("\n");

    struct { int ngl; int n_ctx; const char *label; } configs[] = {
        {0,    4096, "CPU 4k"},
        {0,   16384, "CPU 16k"},
        {0,   32768, "CPU 32k"},
        {99,   4096, "GPU 4k"},
        {99,  16384, "GPU 16k"},
        {99,  32768, "GPU 32k"},
    };
    int n_configs = sizeof(configs) / sizeof(configs[0]);

    printf("%-12s | %7s | %7s | %7s | %7s | %7s | %7s |\n",
           "Config", "RAM-idle", "+Model", "+Ctx", "+Decode", "RAM-tot", "Page");
    printf("-------------|---------|---------|---------|---------|---------|---------|\n");

    size_t ram_idle_ref = 0;

    for (int ci = 0; ci < n_configs; ci++) {
        int ngl = configs[ci].ngl;
        int n_ctx = configs[ci].n_ctx;
        const char *label = configs[ci].label;

        fprintf(stderr, "  Testing %-10s ngl=%d ctx=%d ... ", label, ngl, n_ctx);

        llama_backend_init();

        if (ci == 0) ram_idle_ref = get_working_set_mb();

        struct llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = ngl;
        struct llama_model *model = llama_model_load_from_file(argv[1], mp);
        if (!model) { fprintf(stderr, "FAIL model\n"); continue; }
        size_t ram_model = get_working_set_mb();

        struct llama_context_params cp = llama_context_default_params();
        cp.n_ctx = n_ctx;
        struct llama_context *ctx = llama_init_from_model(model, cp);
        if (!ctx) { fprintf(stderr, "FAIL ctx\n"); llama_model_free(model); continue; }
        size_t ram_ctx = get_working_set_mb();

        /* Small decode to trigger allocations */
        const struct llama_vocab *vocab = llama_model_get_vocab(model);
        const char *test = "Hello world, memory benchmark.";
        int *tokens = NULL; int n_tokens = 0;
        int need = llama_tokenize(vocab, test, (int)strlen(test), NULL, 0, false, false);
        if (need < 0) need = -need;
        if (need > 0) {
            tokens = (int*)malloc((size_t)need * 4);
            n_tokens = llama_tokenize(vocab, test, (int)strlen(test), tokens, need, false, false);
            if (n_tokens < 0) n_tokens = -n_tokens;
        }
        if (n_tokens > 0) {
            llama_decode(ctx, llama_batch_get_one(tokens, n_tokens));
            free(tokens);
        }
        size_t ram_decode = get_working_set_mb();
        size_t page_decode = get_pagefile_mb();

        fprintf(stderr, "OK\n");

        printf("%-12s | %7zu | %7zu | %7zu | %7zu | %7zu | %7zu |\n",
               label,
               ram_idle_ref,
               ram_model - ram_idle_ref,
               ram_ctx - ram_model,
               ram_decode - ram_ctx,
               ram_decode - ram_idle_ref,
               page_decode);

        llama_free(ctx);
        llama_model_free(model);
        llama_backend_free();
    }

    printf("\n");
    printf("Note: RAM-tot = ram_decode - ram_idle (total delta from idle)\n");
    printf("RAM values shown in MB.\n");
    printf("GPU-offload RAM includes Vulkan driver allocations visible in process working set.\n");

    return 0;
}
