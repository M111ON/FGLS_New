#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <windows.h>
#include <psapi.h>
#include "llama.h"

#pragma comment(lib, "psapi.lib")

static size_t get_mem_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize / (1024 * 1024);
    return 0;
}

static int tokenize(const struct llama_vocab *v, const char *t, int len, int **out, int add, int parse) {
    int need = llama_tokenize(v, t, len, NULL, 0, add, parse);
    if (need < 0) need = -need;
    if (need == 0) { *out = NULL; return 0; }
    *out = (int*)malloc((size_t)need * 4);
    int got = llama_tokenize(v, t, len, *out, need, add, parse);
    return got < 0 ? -got : got;
}

/* Perturb K/V data in state buffer (skip metadata) */
static size_t perturb_kv(uint8_t *buf, size_t sz) {
    if (sz < 36) return 0;
    uint32_t n_cells = *(uint32_t*)(buf + 12);
    /* Skip per-cell metadata */
    int cell_end = 16;
    for (uint32_t ci = 0; ci < n_cells && cell_end + 8 <= (int)sz; ci++) {
        uint32_t n_seq_id = *(uint32_t*)(buf + cell_end + 4);
        cell_end += 8 + (int)(n_seq_id * 4);
    }
    int data_hdr = cell_end;
    if (data_hdr + 8 > (int)sz) return 0;
    uint32_t n_layer = *(uint32_t*)(buf + data_hdr + 4);
    
    int off = data_hdr + 8;
    size_t total_perturbed = 0;
    for (uint32_t li = 0; li < n_layer && off + 12 <= (int)sz; li++) {
        uint64_t k_sz_row = *(uint64_t*)(buf + off + 4);
        size_t k_data_sz = (size_t)n_cells * (size_t)k_sz_row;
        int k_data_off = off + 12;
        
        int v_off = k_data_off + (int)k_data_sz;
        uint64_t v_sz_row = *(uint64_t*)(buf + v_off + 4);
        size_t v_data_sz = (size_t)n_cells * (size_t)v_sz_row;
        int v_data_off = v_off + 12;
        
        /* stride-64 XOR on K data */
        for (size_t b = 0; b < k_data_sz; b += 64) {
            buf[k_data_off + (int)b] ^= 0x01;
            total_perturbed++;
        }
        /* stride-64 XOR on V data */
        for (size_t b = 0; b < v_data_sz; b += 64) {
            buf[v_data_off + (int)b] ^= 0x02;
            total_perturbed++;
        }
        
        off = v_data_off + (int)v_data_sz;
    }
    return total_perturbed;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf [--quick]\n", argv[0]); return 1; }
    int quick = argc > 2 && strcmp(argv[2], "--quick") == 0;
    
    printf("=== KV State Memory Test ===\n");
    printf("Step 0 (idle):         %4zu MB\n", get_mem_mb());
    
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) return 1;
    printf("Step 1 (model loaded): %4zu MB\n", get_mem_mb());
    
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = quick ? 64 : 2048;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) return 1;
    printf("Step 2 (context created, n_ctx=%d): %4zu MB\n", cp.n_ctx, get_mem_mb());
    
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    
    /* Tokenize a larger prompt to fill KV cache */
    const char *prompts[] = {
        "The quick brown fox jumps over the lazy dog near the bank of the river.",
        "Machine learning is a subset of artificial intelligence that enables systems to learn.",
        "In the beginning the universe was created with a big bang that expanded rapidly.",
    };
    int n_prompts = quick ? 1 : 3;
    
    /* Decode first prompt */
    int *toks; int nt = tokenize(vocab, prompts[0], (int)strlen(prompts[0]), &toks, false, false);
    printf("Prompt 1: %d tokens\n", nt);
    llama_decode(ctx, llama_batch_get_one(toks, nt));
    free(toks);
    
    /* Decode more prompts for larger KV cache */
    for (int pi = 1; pi < n_prompts; pi++) {
        nt = tokenize(vocab, prompts[pi], (int)strlen(prompts[pi]), &toks, false, false);
        printf("Prompt %d: %d tokens\n", pi+1, nt);
        llama_decode(ctx, llama_batch_get_one(toks, nt));
        free(toks);
    }
    printf("Step 3 (after decode): %4zu MB\n", get_mem_mb());
    
    /* Get state buffer size */
    size_t state_sz = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("State buffer size: %zu bytes (%.2f MB)\n", state_sz, (double)state_sz / (1024*1024));
    
    /* Allocate + save */
    uint8_t *state_buf = (uint8_t*)malloc(state_sz);
    llama_state_seq_get_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("Step 4 (state saved):  %4zu MB (+%zu KB heap)\n", get_mem_mb(), state_sz/1024);
    
    /* Perturb + restore cycle - 100 times */
    printf("\n=== Perturb/Restore Cycle ===\n");
    printf("Cycles | Restore | Perturbed_bytes | Mem\n");
    int n_cycles = quick ? 10 : 100;
    
    printf("=== Perturb/Restore Cycle ===\n");
    printf("Cycles | Restore | Perturbed_bytes | Mem\n");
    
    for (int ci = 0; ci < n_cycles; ci++) {
        size_t n_pert = perturb_kv(state_buf, state_sz);
        size_t r = llama_state_seq_set_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        
        if (ci % 10 == 0 || ci == n_cycles - 1) {
            printf("  %4d | %s | %10zu | %zu MB\n",
                ci+1, r == state_sz ? "OK  " : "FAIL", n_pert, get_mem_mb());
        }
        
        /* Re-save clean state for next cycle */
        llama_state_seq_get_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        
        if (r != state_sz) { printf("FAIL at cycle %d!\n", ci); break; }
    }
    
    /* Now test KV swap with generation */
    printf("\n=== KV Swap + Generation ===\n");
    /* Save clean state */
    llama_state_seq_get_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    
    /* Generate 1 token WITH perturbed KV */
    size_t n_pert = perturb_kv(state_buf, state_sz);
    llama_state_seq_set_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    
    llama_token tid = 0;
    llama_decode(ctx, llama_batch_get_one(&tid, 1));
    float *logits = llama_get_logits_ith(ctx, 0);
    float best_v = -1e30; int best_tok = 0;
    for (int i = 0; i < 10000; i++) {
        if (logits[i] > best_v) { best_v = logits[i]; best_tok = i; }
    }
    
    /* Restore clean state */
    llama_state_seq_get_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    llama_state_seq_set_data_ext(ctx, state_buf, state_sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    
    float *logits2 = llama_get_logits_ith(ctx, 0);
    float best_v2 = -1e30; int best_tok2 = 0;
    llama_decode(ctx, llama_batch_get_one(&tid, 1));
    for (int i = 0; i < 10000; i++) {
        if (logits2[i] > best_v2) { best_v2 = logits2[i]; best_tok2 = i; }
    }
    
    printf("Clean token:    %d (logit=%.4f)\n", best_tok2, best_v2);
    printf("Perturbed token: %d (logit=%.4f)\n", best_tok, best_v);
    printf("Same? %s\n", best_tok == best_tok2 ? "YES" : "NO");
    printf("Perturbed %zu bytes (stride=64 XOR)\n", n_pert);
    printf("Final memory: %zu MB\n", get_mem_mb());
    
    free(state_buf);
    llama_free(ctx); llama_model_free(model); llama_backend_free();
    printf("Cleanup complete: %zu MB\n", get_mem_mb());
    return 0;
}
