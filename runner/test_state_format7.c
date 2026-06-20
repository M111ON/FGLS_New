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
    fprintf(stderr, "decode OK\n");
    
    size_t sz = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "state size: %zu\n", sz);
    uint8_t *buf = (uint8_t*)malloc(sz);
    llama_state_seq_get_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    
    /* Clean restore FIRST */
    size_t r = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "Clean restore: %zu/%zu %s\n", r, sz, r == sz ? "OK" : "FAIL");
    
    /* Now parse and print */
    uint32_t magic   = *(uint32_t*)(buf + 0);
    uint32_t seq_id  = *(uint32_t*)(buf + 4);
    uint32_t n_stream= *(uint32_t*)(buf + 8);
    uint32_t n_cells = *(uint32_t*)(buf + 12);
    uint32_t n_sid   = *(uint32_t*)(buf + 20);
    int data_hdr = 24 + n_sid * 4;
    uint32_t n_layer = *(uint32_t*)(buf + data_hdr + 4);
    
    fprintf(stderr, "magic=0x%08x seq_id=%u n_stream=%u n_cells=%u n_seq_id=%u data_hdr=%d n_layer=%u\n",
        magic, seq_id, n_stream, n_cells, n_sid, data_hdr, n_layer);
    
    /* Perturb a byte in K data section of layer 0 then restore */
    int off = data_hdr + 8; /* first layer header */
    uint64_t k_sz_row = *(uint64_t*)(buf + off + 4);
    int k_data_off = off + 12;
    fprintf(stderr, "layer 0: off=%d k_type=%d k_sz_row=%llu k_data_off=%d\n",
        off, *(int32_t*)(buf+off), (unsigned long long)k_sz_row, k_data_off);
    
    /* Perturb K data byte */
    fprintf(stderr, "Perturbing K_data[+10] at offset %d...\n", k_data_off + 10);
    buf[k_data_off + 10] ^= 0x01;
    r = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "K perturb restore: %zu/%zu %s\n", r, sz, r == sz ? "OK" : "FAIL");
    buf[k_data_off + 10] ^= 0x01;
    
    /* Now perturb V data byte */
    size_t k_data_sz = (size_t)n_cells * (size_t)k_sz_row;
    int v_off = k_data_off + (int)k_data_sz;
    uint64_t v_sz_row = *(uint64_t*)(buf + v_off + 4);
    int v_data_off = v_off + 12;
    fprintf(stderr, "layer 0: v_off=%d v_type=%d v_sz_row=%llu v_data_off=%d\n",
        v_off, *(int32_t*)(buf+v_off), (unsigned long long)v_sz_row, v_data_off);
    
    buf[v_data_off + 20] ^= 0x02;
    r = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "V perturb restore: %zu/%zu %s\n", r, sz, r == sz ? "OK" : "FAIL");
    buf[v_data_off + 20] ^= 0x02;
    
    /* Test: does output change? Generate w/ and w/o perturbation */
    fprintf(stderr, "\n--- Generating with clean state ---\n");
    llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    
    /* Generate 1 token with clean KV */
    llama_token tid = 0;
    llama_decode(ctx, llama_batch_get_one(&tid, 1));
    float *logits = llama_get_logits_ith(ctx, 0);
    int n_vocab = llama_vocab_n_tokens(vocab);
    int best_clean = 0; float best_v = -1e30;
    for (int i = 0; i < n_vocab; i++) {
        if (logits[i] > best_v) { best_v = logits[i]; best_clean = i; }
    }
    fprintf(stderr, "Clean first token: %d (logit=%f)\n", best_clean, best_v);
    
    /* Now perturb ALL K/V data with stride=64 XOR */
    off = data_hdr + 8;
    for (uint32_t li = 0; li < n_layer; li++) {
        uint64_t lk_sz_row = *(uint64_t*)(buf + off + 4);
        size_t lk_data_sz = (size_t)n_cells * (size_t)lk_sz_row;
        int lk_data_off = off + 12;
        int lv_off = lk_data_off + (int)lk_data_sz;
        uint64_t lv_sz_row = *(uint64_t*)(buf + lv_off + 4);
        size_t lv_data_sz = (size_t)n_cells * (size_t)lv_sz_row;
        int lv_data_off = lv_off + 12;
        
        for (size_t b = 0; b < lk_data_sz; b += 64)
            buf[lk_data_off + (int)b] ^= 0x01;
        for (size_t b = 0; b < lv_data_sz; b += 64)
            buf[lv_data_off + (int)b] ^= 0x02;
        
        off = lv_data_off + (int)lv_data_sz;
    }
    
    fprintf(stderr, "Restoring perturbed state...\n");
    r = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "Perturbed restore: %zu/%zu %s\n", r, sz, r == sz ? "OK" : "FAIL");
    
    if (r == sz) {
        llama_decode(ctx, llama_batch_get_one(&tid, 1));
        logits = llama_get_logits_ith(ctx, 0);
        int best_pert = 0; best_v = -1e30;
        for (int i = 0; i < n_vocab; i++) {
            if (logits[i] > best_v) { best_v = logits[i]; best_pert = i; }
        }
        fprintf(stderr, "Perturbed first token: %d (logit=%f)\n", best_pert, best_v);
        fprintf(stderr, "Same? %s\n", best_clean == best_pert ? "YES" : "NO");
    }
    
    free(buf); free(toks);
    llama_free(ctx); llama_model_free(model); llama_backend_free();
    return 0;
}
