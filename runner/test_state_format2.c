#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "llama.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf\n", argv[0]); return 1; }
    
    llama_backend_init();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) { fprintf(stderr, "model load fail\n"); return 1; }
    
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 256;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "context fail\n"); return 1; }
    
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    
    /* Tokenize */
    const char *prompt = "Hello";
    int nt = llama_tokenize(vocab, prompt, 5, NULL, 0, true, false);
    printf("tokenize returned %d\n", nt);
    if (nt <= 0) { fprintf(stderr, "tokenize fail\n"); return 1; }
    
    llama_token *toks = (llama_token*)malloc((size_t)nt * sizeof(llama_token));
    nt = llama_tokenize(vocab, prompt, 5, toks, nt, true, false);
    printf("tokenized to %d tokens: ", nt);
    for (int i = 0; i < nt; i++) printf("%d ", toks[i]);
    printf("\n");
    if (nt <= 0) { fprintf(stderr, "tokenize fill fail\n"); return 1; }
    
    /* Decode prompt */
    struct llama_batch batch = llama_batch_get_one(toks, nt);
    printf("decoding %d tokens...\n", nt);
    int ret = llama_decode(ctx, batch);
    if (ret != 0) { fprintf(stderr, "decode failed: %d\n", ret); return 1; }
    printf("decode OK\n");
    
    /* Save state */
    size_t sz = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("state size: %zu bytes\n", sz);
    uint8_t *buf = (uint8_t*)malloc(sz);
    size_t rsz = llama_state_seq_get_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("state get returned %zu\n", rsz);
    
    /* Dump header */
    printf("\n--- Header dump (first 128 bytes) ---\n");
    for (int i = 0; i < 128 && i < (int)sz; i++) {
        printf("%02x", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
        else if ((i + 1) % 2 == 0) printf(" ");
    }
    printf("\n");

    /* Dump middle section (every 10000 bytes) */
    printf("\n--- Pattern scan ---\n");
    for (int off = 0; off < (int)sz - 8; off += 2000) {
        printf("offset %6d: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            off, buf[off], buf[off+1], buf[off+2], buf[off+3],
            buf[off+4], buf[off+5], buf[off+6], buf[off+7]);
    }
    
    /* Last 64 bytes */
    printf("\n--- Last 64 bytes ---\n");
    int ls = sz > 64 ? (int)sz - 64 : 0;
    for (int i = ls; i < (int)sz; i++) {
        printf("%02x", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
        else if ((i + 1) % 2 == 0) printf(" ");
    }
    printf("\n");
    
    /* Try restore */
    rsz = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("\n✓ Clean restore: %zu (sz=%zu)\n", rsz, sz);
    
    /* Perturb byte at a few places and try restore */
    for (int p = 0; p < (int)sz; p += (int)sz / 6) {
        if (p == 0) continue;
        uint8_t saved = buf[p];
        buf[p] ^= 0x01;
        rsz = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        printf("  perturb at %6d: restore=%zu/%zu %s\n", p, rsz, sz, rsz == sz ? "OK" : "FAIL");
        buf[p] = saved;
    }
    
    free(buf); free(toks);
    llama_free(ctx); llama_model_free(model); llama_backend_free();
    return 0;
}
