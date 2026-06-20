#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "llama.h"
#include "ggml.h"

/* Minimal test: save KV state after prompt decode, dump first 512 bytes */
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf\n", argv[0]); return 1; }
    llama_backend_init();
    ggml_backend_load("ggml-cpu-x64.dll");
    
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) { fprintf(stderr, "model load fail\n"); return 1; }
    
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 256;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "context fail\n"); return 1; }
    
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    
    /* Tokenize a short prompt */
    const char *prompt = "Hello";
    int nt = llama_tokenize(vocab, prompt, 5, NULL, 0, true, false);
    int *toks = (int*)malloc((size_t)nt * 4);
    llama_tokenize(vocab, prompt, 5, toks, nt, true, false);
    
    /* Decode prompt */
    struct llama_batch batch = llama_batch_get_one(toks, nt);
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "decode fail\n"); return 1; }
    
    /* Save state */
    size_t sz = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("state size: %zu bytes\n", sz);
    uint8_t *buf = (uint8_t*)malloc(sz);
    llama_state_seq_get_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    
    /* Dump header (first 128 bytes) */
    printf("\n--- Header dump (first 128 bytes) ---\n");
    for (int i = 0; i < 128 && i < (int)sz; i++) {
        printf("%02x", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
        else if ((i + 1) % 2 == 0) printf(" ");
    }
    printf("\n");
    
    /* Look for patterns — f16 data has specific byte patterns */
    /* For K/V f16 data typical values: 0x00 0x3c (~1.0), 0x00 0x00 (0.0), etc. */
    printf("\n--- Searching for KV data patterns ---\n");
    for (int i = 128; i < (int)sz - 8; i++) {
        /* Check if position i looks like start of f16 data block */
        uint16_t v = *(uint16_t*)(buf + i);
        if (v == 0x3C00 || v == 0x0000 || v == 0xBC00) {
            /* Could be f16 = 1.0, 0.0, or -1.0 */
        }
        /* Find position where a large block of f16 values starts */
        if (i % 1000 == 0) {
            printf("offset %d: %02x %02x %02x %02x | %02x %02x %02x %02x\n",
                i, buf[i], buf[i+1], buf[i+2], buf[i+3],
                buf[i+4], buf[i+5], buf[i+6], buf[i+7]);
        }
    }
    
    /* Dump the last 256 bytes */
    printf("\n--- Last 256 bytes ---\n");
    int dump_start = sz > 256 ? (int)sz - 256 : 0;
    for (int i = dump_start; i < (int)sz; i++) {
        printf("%02x", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
        else if ((i + 1) % 2 == 0) printf(" ");
    }
    printf("\n");
    
    /* Now try to restore — this should work with clean data */
    size_t rsz = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    if (rsz == sz) {
        printf("\n✓ Clean restore OK (%zu bytes)\n", rsz);
    } else {
        printf("\n✗ Restore failed: returned %zu\n", rsz);
    }
    
    /* Now try with 1 byte perturbed at offset 256 (far from header) */
    buf[256] ^= 0x01;
    rsz = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("Restore with byte 256 flipped: returned %zu (sz=%zu)\n", rsz, sz);
    buf[256] ^= 0x01; /* restore */
    
    /* Try a byte near the end */
    buf[sz - 100] ^= 0x01;
    rsz = llama_state_seq_set_data_ext(ctx, buf, sz, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    printf("Restore with byte (end-100) flipped: returned %zu (sz=%zu)\n", rsz, sz);
    
    free(buf); free(toks);
    llama_free(ctx); llama_model_free(model); llama_backend_free();
    return 0;
}
