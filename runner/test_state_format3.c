#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "llama.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf\n", argv[0]); return 1; }
    
    fprintf(stderr, "llama_backend_init...\n");
    llama_backend_init();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    fprintf(stderr, "loading model...\n");
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) { fprintf(stderr, "model load fail\n"); return 1; }
    
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 64;
    fprintf(stderr, "creating context...\n");
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "context fail\n"); return 1; }
    fprintf(stderr, "context OK\n");
    
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    if (!vocab) { fprintf(stderr, "vocab null\n"); return 1; }
    fprintf(stderr, "vocab OK: %p\n", (void*)vocab);
    
    /* Tokenize with explicit error check */
    const char *prompt = "Hello";
    fprintf(stderr, "tokenizing '%s' (len=%zu)...\n", prompt, strlen(prompt));
    int nt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
    fprintf(stderr, "llama_tokenize returned %d\n", nt);
    if (nt < 0) {
        fprintf(stderr, "tokenize error!\n");
        fprintf(stderr, "trying without add_special...\n");
        nt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, false, false);
        fprintf(stderr, "llama_tokenize (no special) returned %d\n", nt);
    }
    if (nt <= 0) {
        fprintf(stderr, "still failing, trying shorter string...\n");
        nt = llama_tokenize(vocab, "a", 1, NULL, 0, false, false);
        fprintf(stderr, "tokenize 'a' returned %d\n", nt);
    }
    if (nt <= 0) { fprintf(stderr, "can't tokenize\n"); return 1; }
    
    llama_token *toks = (llama_token*)malloc((size_t)nt * sizeof(llama_token));
    nt = llama_tokenize(vocab, prompt, 5, toks, nt, true, false);
    fprintf(stderr, "fill: %d tokens: ", nt);
    for (int i = 0; i < nt; i++) fprintf(stderr, "%d ", toks[i]);
    fprintf(stderr, "\n");
    if (nt <= 0) { fprintf(stderr, "fill fail\n"); return 1; }
    
    /* Decode prompt -- use proper batch with positions */
    struct llama_batch batch = llama_batch_get_one(toks, nt);
    fprintf(stderr, "decoding %d tokens...\n", nt);
    int ret = llama_decode(ctx, batch);
    if (ret != 0) { fprintf(stderr, "decode failed: %d\n", ret); return 1; }
    fprintf(stderr, "decode OK\n");
    
    /* Save state */
    size_t sz = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "state size: %zu bytes\n", sz);
    uint8_t *buf = (uint8_t*)malloc(sz);
    size_t rsz = llama_state_seq_get_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    fprintf(stderr, "state get returned %zu\n", rsz);
    
    /* Dump first 64 bytes */
    printf("--- Header (64 bytes) ---\n");
    int nprint = sz < 64 ? (int)sz : 64;
    for (int i = 0; i < nprint; i++) {
        printf("%02x", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
        else if ((i + 1) % 2 == 0) printf(" ");
    }
    if (nprint % 16 != 0) printf("\n");

    /* Pattern scan: every 5000 bytes look for f16 patterns */
    printf("--- Pattern scan ---\n");
    for (int off = 0; off < (int)sz - 4; ) {
        uint16_t v0 = *(uint16_t*)(buf + off);
        uint16_t v1 = *(uint16_t*)(buf + off + 2);
        float f0 = *(float*)(buf + off);
        printf("  %6d: %04x %04x = %g\n", off, v0, v1, f0);
        off += sz / 20;
        if (off < 1) off = 1;
    }
    
    /* Last 48 bytes */
    printf("--- Tail (48 bytes) ---\n");
    int tail = sz > 48 ? (int)sz - 48 : 0;
    for (int i = tail; i < (int)sz; i++) {
        printf("%02x", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
        else if ((i + 1) % 2 == 0) printf(" ");
    }
    if ((sz - tail) % 16 != 0) printf("\n");
    printf("Total size = %zu\n", sz);
    
    /* Try clean restore */
    rsz = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("Clean restore: %zu/%zu %s\n", rsz, sz, rsz == sz ? "OK" : "FAIL");
    
    /* Try single-byte flip at various positions */
    for (int iter = 0; iter < 10; iter++) {
        int p = 64 + (int)((sz - 64) * (iter + 1) / 11);
        if (p >= (int)sz) continue;
        uint8_t saved = buf[p];
        buf[p] ^= 0x01;
        size_t r = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        printf("  flip byte %6d: restore=%zu/%zu %s\n", p, r, sz, r == sz ? "OK" : "FAIL");
        buf[p] = saved;
    }
    
    free(buf); free(toks);
    llama_free(ctx); llama_model_free(model); llama_backend_free();
    return 0;
}
