#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "llama.h"

static int tokenize(const struct llama_vocab *v, const char *t, int len, int **out, int add, int parse) {
    int need = llama_tokenize(v, t, len, NULL, 0, add, parse);
    if (need < 0) need = -need;
    if (need == 0) { *out = NULL; return 0; }
    *out = (int*)malloc((size_t)need * 4);
    int got = llama_tokenize(v, t, len, *out, need, add, parse);
    return got < 0 ? -got : got;
}

/* Parse state buffer and print layer layout */
static void parse_state(uint8_t *buf, size_t sz) {
    if (sz < 16) { printf("buffer too small\n"); return; }
    
    uint32_t magic   = *(uint32_t*)(buf + 0);
    uint32_t seq_id  = *(uint32_t*)(buf + 4);
    uint32_t n_stream= *(uint32_t*)(buf + 8);
    uint32_t n_cells = *(uint32_t*)(buf + 12);
    
    printf("magic=0x%08x seq_id=%u n_stream=%u n_cells=%u\n", magic, seq_id, n_stream, n_cells);
    
    if (n_cells == 0) return;
    
    /* Cell 0 metadata */
    int32_t pos = *(int32_t*)(buf + 16);
    uint32_t n_seq_id = *(uint32_t*)(buf + 20);
    printf("cell[0]: pos=%d n_seq_id=%u", pos, n_seq_id);
    
    int meta_offset = 24; /* after pos(4) + n_seq_id(4) */
    for (uint32_t j = 0; j < n_seq_id && j < 10; j++) {
        printf(" seq_id[%u]=%d", j, *(int32_t*)(buf + meta_offset + j*4));
    }
    printf("\n");
    
    /* Data header starts after all cell metadata */
    int data_hdr = 24 + n_seq_id * 4;  /* after pos + n_seq_id + seq_ids */
    uint32_t v_trans = *(uint32_t*)(buf + data_hdr);
    uint32_t n_layer = *(uint32_t*)(buf + data_hdr + 4);
    printf("v_trans=%u n_layer=%u\n", v_trans, n_layer);
    
    /* Per-layer */
    int off = data_hdr + 8;
    for (uint32_t li = 0; li < n_layer && off + 12 <= (int)sz; li++) {
        int32_t  k_type  = *(int32_t*)(buf + off);
        uint64_t k_sz_row= *(uint64_t*)(buf + off + 4);
        int k_data_off = off + 12;
        size_t k_data_sz = (size_t)n_cells * (size_t)k_sz_row;
        
        int v_off = k_data_off + (int)k_data_sz;
        int32_t  v_type  = *(int32_t*)(buf + v_off);
        uint64_t v_sz_row= *(uint64_t*)(buf + v_off + 4);
        int v_data_off = v_off + 12;
        size_t v_data_sz = (size_t)n_cells * (size_t)v_sz_row;
        
        printf("  layer %2u: K_type=%d K_row=%llu K_data=[%6d..%6d] | V_type=%d V_row=%llu V_data=[%6d..%6d]\n",
            li, k_type, (unsigned long long)k_sz_row,
            k_data_off, k_data_off + (int)k_data_sz - 1,
            v_type, (unsigned long long)v_sz_row,
            v_data_off, v_data_off + (int)v_data_sz - 1);
        
        off = v_data_off + (int)v_data_sz;
    }
    printf("total state = %zu bytes\n", sz);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf\n", argv[0]); return 1; }
    
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) return 1;
    
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 64;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) return 1;
    
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int *toks; int nt = tokenize(vocab, "Hello", 5, &toks, false, false);
    struct llama_batch batch = llama_batch_get_one(toks, nt);
    if (llama_decode(ctx, batch) != 0) return 1;
    fprintf(stderr, "decode OK (%d tokens)\n", nt);
    
    size_t sz = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("state size: %zu bytes\n", sz);
    uint8_t *buf = (uint8_t*)malloc(sz);
    llama_state_seq_get_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    
    parse_state(buf, (int)sz);
    
    /* Now test: perturb ONLY K/V data bytes (skip metadata) */
    printf("\n--- Perturbation tests ---\n");
    
    /* We know the layout now. Build list of "safe" data ranges */
    int data_hdr = 24 + *(uint32_t*)(buf + 20);  /* 24 + n_seq_id * 4 */
    uint32_t n_layer = *(uint32_t*)(buf + data_hdr + 4);
    
    int off = data_hdr + 8;
    for (uint32_t li = 0; li < n_layer; li++) {
        uint64_t k_sz_row= *(uint64_t*)(buf + off + 4);
        size_t k_data_sz = (size_t)*(uint32_t*)(buf + 12) * (size_t)k_sz_row;
        int k_data_off = off + 12;
        
        int v_off = k_data_off + (int)k_data_sz;
        uint64_t v_sz_row= *(uint64_t*)(buf + v_off + 4);
        size_t v_data_sz = (size_t)*(uint32_t*)(buf + 12) * (size_t)v_sz_row;
        int v_data_off = v_off + 12;
        
        /* Flip a byte in K data */
        if (k_data_sz > 4) {
            uint8_t saved = buf[k_data_off + 10];
            buf[k_data_off + 10] ^= 0x01;
            size_t r = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
            printf("  L%02u K[+10]: %s (restore=%zu/%zu)\n", li,
                r == sz ? "OK" : "FAIL", r, sz);
            buf[k_data_off + 10] = saved;
        }
        
        /* Flip a byte in V data */
        if (v_data_sz > 4) {
            uint8_t saved = buf[v_data_off + 20];
            buf[v_data_off + 20] ^= 0x02;
            size_t r = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
            printf("  L%02u V[+20]: %s (restore=%zu/%zu)\n", li,
                r == sz ? "OK" : "FAIL", r, sz);
            buf[v_data_off + 20] = saved;
        }
        
        /* Flip a byte in k_type (should FAIL) */
        uint8_t saved = buf[off];
        buf[off] ^= 0x01;
        size_t r = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        printf("  L%02u K_type: %s (restore=%zu/%zu) [expect FAIL]\n", li,
            r == sz ? "OK" : "FAIL", r, sz);
        buf[off] = saved;
        
        off = v_data_off + (int)v_data_sz;
    }
    
    /* Now test: does K/V perturbation actually CHANGE output? */
    /* Generate token WITHOUT perturbation */
    printf("\n--- Output comparison ---\n");
    
    /* Clean decode (no perturbation) */
    llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    int n_toks = 1;
    int *gen_toks = (int*)malloc(4);
    for (int i = 0; i < n_toks; i++) {
        llama_token new_id = 0;
        batch = llama_batch_get_one(&new_id, 1);
        llama_decode(ctx, batch);
        float *logits = llama_get_logits(ctx);
        /* Find best token */
        int n_vocab = llama_vocab_n_tokens(vocab);
        float best_val = -1e30; int best_idx = 0;
        for (int v = 0; v < n_vocab && v < 10000; v++) {
            if (logits[v] > best_val) { best_val = logits[v]; best_idx = v; }
        }
        gen_toks[i] = best_idx;
    }
    
    /* Perturb K data for all layers and generate again */
    off = data_hdr + 8;
    for (uint32_t li = 0; li < n_layer; li++) {
        uint64_t k_sz_row= *(uint64_t*)(buf + off + 4);
        size_t k_data_sz = (size_t)*(uint32_t*)(buf + 12) * (size_t)k_sz_row;
        int k_data_off = off + 12;
        int v_off = k_data_off + (int)k_data_sz;
        uint64_t v_sz_row= *(uint64_t*)(buf + v_off + 4);
        size_t v_data_sz = (size_t)*(uint32_t*)(buf + 12) * (size_t)v_sz_row;
        int v_data_off = v_off + 12;
        
        /* XOR 0x01 at stride 64 on K data */
        for (size_t b = 0; b < k_data_sz; b += 64)
            buf[k_data_off + (int)b] ^= 0x01;
        /* XOR 0x01 at stride 64 on V data */    
        for (size_t b = 0; b < v_data_sz; b += 64)
            buf[v_data_off + (int)b] ^= 0x02;
        
        off = v_data_off + (int)v_data_sz;
    }
    
    /* Restore perturbed state */
    size_t r = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("  Perturbed state restore: %s (%zu/%zu)\n", r == sz ? "OK" : "FAIL", r, sz);
    
    if (r == sz) {
        /* Generate with perturbed KV */
        for (int i = 0; i < n_toks; i++) {
            llama_token new_id = 0;
            batch = llama_batch_get_one(&new_id, 1);
            llama_decode(ctx, batch);
            float *logits = llama_get_logits(ctx);
            int n_vocab = llama_vocab_n_tokens(vocab);
            float best_val = -1e30; int best_idx = 0;
            for (int v = 0; v < n_vocab && v < 10000; v++) {
                if (logits[v] > best_val) { best_val = logits[v]; best_idx = v; }
            }
            printf("  Baseline token:  %d\n", gen_toks[0]);
            printf("  Perturbed token: %d\n", best_idx);
            printf("  Same? %s\n", gen_toks[0] == best_idx ? "YES" : "NO");
        }
    }
    
    free(gen_toks); free(buf); free(toks);
    llama_free(ctx); llama_model_free(model); llama_backend_free();
    return 0;
}
