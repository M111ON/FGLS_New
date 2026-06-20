#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "llama.h"

/* Helper: tokenize with b9733 convention (returns -N on need) */
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
    if (!model) { fprintf(stderr, "model load fail\n"); return 1; }
    
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 64;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "context fail\n"); return 1; }
    
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int *toks; int nt = tokenize(vocab, "Hello", 5, &toks, false, false);
    fprintf(stderr, "tokens: ");
    for (int i = 0; i < nt; i++) fprintf(stderr, "%d ", toks[i]);
    fprintf(stderr, "\n");
    
    struct llama_batch batch = llama_batch_get_one(toks, nt);
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "decode fail\n"); return 1; }
    fprintf(stderr, "decode OK\n");
    
    size_t sz = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "state size: %zu bytes\n", sz);
    uint8_t *buf = (uint8_t*)malloc(sz);
    size_t rsz = llama_state_seq_get_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "state get returned %zu\n", rsz);
    
    /* Header dump */
    printf("--- Header (64 bytes) ---\n");
    for (int i = 0; i < 64 && i < (int)sz; i++) {
        printf("%02x", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
        else if ((i + 1) % 2 == 0) printf(" ");
    }
    if (64 % 16 != 0) printf("\n");
    
    /* Pattern scan: each cell = pos(4B) + n_seq_mask? + K data + V data */
    /* Qwen2: 24 layers, 1 token (no prompt). n_embd_k_gqa = 128, f16 = 256B per K per cell per layer */
    /* Expected cell layout: pos(4) + flags(4) + K(256B) + V(256B) = 520B per cell-layer */
    printf("\n--- Cell boundary scan ---\n");
    int n_layers = 24;
    int embd_k = 128;  /* n_embd_k_gqa for Qwen2-0.5B */
    int embd_v = 128;
    int cell_kv = embd_k * 2 + embd_v * 2; /* f16: each is 2 bytes */
    fprintf(stderr, "expected K+V per cell-layer = %d bytes\n", cell_kv);
    
    /* Try to find cell boundaries by looking for small integer patterns */
    int pos_bytes = 4;  /* guess: 4-byte position */
    int meta_bytes = 8; /* guess: pos(4) + flags(4) */
    int cell_total = meta_bytes + cell_kv;  /* 520 */
    
    fprintf(stderr, "guessed cell size = %d bytes (%d meta + %d data)\n", cell_total, meta_bytes, cell_kv);
    fprintf(stderr, "expected total = %d cells\n", n_layers);
    
    /* Dump at each expected cell boundary */
    int report_positions[] = {0, 100, 200, 500, 1000, 2000, 5000, 10000, 0};
    for (int ci = 0; ci < n_layers && ci * cell_total < (int)sz; ci++) {
        int off = ci * cell_total;
        printf("[cell %2d @%5d] ", ci, off);
        for (int j = 0; j < 8; j++) printf("%02x", buf[off+j]);
        printf(" |");
        for (int j = meta_bytes; j < meta_bytes + 8; j++) {
            if (off + j < (int)sz) printf(" %02x", buf[off+j]);
        }
        printf(" |");
        int v_start = meta_bytes + cell_kv/2;
        for (int j = v_start; j < v_start + 8; j++) {
            if (off + j < (int)sz) printf(" %02x", buf[off+j]);
        }
        printf("\n");
    }
    
    /* Try clean restore */
    rsz = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("\nClean restore: %zu/%zu %s\n", rsz, sz, rsz == sz ? "OK" : "FAIL");
    
    /* Perturb trial: flip byte in K data region (skip meta) */
    for (int ci = 0; ci < n_layers && ci * cell_total + meta_bytes + 4 < (int)sz; ci += 6) {
        int off = ci * cell_total + meta_bytes + 4; /* offset into K data */
        uint8_t saved = buf[off];
        buf[off] ^= 0x01;
        rsz = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        printf("  cell %2d | K[+4] @%6d: restore=%zu/%zu %s\n", ci, off, rsz, sz, rsz == sz ? "OK" : "FAIL");
        buf[off] = saved;
    }
    
    /* Now test with meta-only perturbation (should fail) */
    for (int ci = 0; ci < 3 && ci * cell_total + 2 < (int)sz; ci += 1) {
        int off = ci * cell_total + 2; /* inside pos field */
        uint8_t saved = buf[off];
        buf[off] ^= 0x01;
        rsz = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        printf("  cell %2d | pos[+2] @%5d: restore=%zu/%zu %s (expected FAIL)\n", ci, off, rsz, sz, rsz == sz ? "OK" : "FAIL");
        buf[off] = saved;
    }
    
    free(buf); free(toks);
    llama_free(ctx); llama_model_free(model); llama_backend_free();
    return 0;
}
