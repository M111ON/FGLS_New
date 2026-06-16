// test_swap_msvc.c — compiled with MSVC to avoid CRT mismatch
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "llama.h"

typedef int (*sid_find_fn)(void*, const char*, void**, void**, size_t*);

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: test_swap_msvc model.gguf\n"); return 1; }
    const char *model_path = argv[1];
    const char *target = argc > 2 ? argv[2] : "blk.0.attn_q.weight";

    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");

    struct llama_model_params mp = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "ERROR: model load\n"); return 1; }
    fprintf(stderr, "[swap] model loaded\n"); fflush(stderr);

    HMODULE h = LoadLibraryA("sid_tensor_mingw.dll");
    if (!h) { fprintf(stderr, "ERROR: load DLL\n"); return 1; }
    sid_find_fn find_fn = (sid_find_fn)GetProcAddress(h, "sid_tensor_find");
    if (!find_fn) { fprintf(stderr, "ERROR: symbol\n"); return 1; }

    void *tensor_ptr = NULL, *tensor_data = NULL;
    size_t tensor_nbytes = 0;
    if (find_fn(model, target, &tensor_ptr, &tensor_data, &tensor_nbytes) != 0) {
        fprintf(stderr, "ERROR: tensor '%s' not found\n", target); return 1;
    }
    fprintf(stderr, "[swap] target '%s': ptr=%p data=%p nbytes=%zu\n",
        target, tensor_ptr, tensor_data, tensor_nbytes); fflush(stderr);

    // Tokenize
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    llama_token tokens[1024];
    int n_tokens = llama_tokenize(vocab, "Hello world", -1, tokens, 1024, true, false);
    if (n_tokens <= 0) { fprintf(stderr, "ERROR: tokenize\n"); return 1; }
    fprintf(stderr, "[swap] tokenized %d tokens\n", n_tokens); fflush(stderr);

    // Inference
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 256;
    cparams.n_batch = n_tokens;
    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "ERROR: context init\n"); return 1; }
    fprintf(stderr, "[swap] context created\n"); fflush(stderr);

    llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "ERROR: decode\n"); return 1; }
    fprintf(stderr, "[swap] baseline decode OK\n"); fflush(stderr);

    int n_vocab = llama_vocab_n_tokens(vocab);
    float *baseline = (float*)malloc(n_vocab * sizeof(float));
    {
        float *logits = llama_get_logits_ith(ctx, 0);
        memcpy(baseline, logits, n_vocab * sizeof(float));
    }
    fprintf(stderr, "[swap] baseline logits[0]=%f\n", baseline[0]); fflush(stderr);
    llama_free(ctx);

    // Flip byte
    uint8_t *target_bytes = (uint8_t*)tensor_data;
    uint8_t orig_byte = target_bytes[0];
    target_bytes[0] ^= 1;
    fprintf(stderr, "[swap] flipped byte 0x%02x->0x%02x\n", orig_byte, target_bytes[0]); fflush(stderr);

    // Test run
    ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "ERROR: ctx 2\n"); return 1; }
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "ERROR: decode 2\n"); return 1; }
    fprintf(stderr, "[swap] test decode OK\n"); fflush(stderr);

    float *test_logits = (float*)malloc(n_vocab * sizeof(float));
    {
        float *logits = llama_get_logits_ith(ctx, 0);
        memcpy(test_logits, logits, n_vocab * sizeof(float));
    }
    fprintf(stderr, "[swap] test logits[0]=%f\n", test_logits[0]); fflush(stderr);
    llama_free(ctx);

    // Compare
    int n_diff = 0;
    double max_diff = 0.0;
    for (int i = 0; i < n_vocab; i++) {
        double d = (double)test_logits[i] - (double)baseline[i];
        if (fabs(d) > 1e-10) { n_diff++; if (fabs(d) > max_diff) max_diff = fabs(d); }
    }

    if (n_diff > 0) {
        fprintf(stderr, "\n*** VERDICT: CPU READS tensor->data FRESH ***\n");
    } else {
        fprintf(stderr, "\n*** VERDICT: backend CACHED pointer ***\n");
    }
    fprintf(stderr, "diff count: %d / %d, max_diff: %g\n", n_diff, n_vocab, max_diff);

    target_bytes[0] = orig_byte;
    free(baseline); free(test_logits);
    FreeLibrary(h);
    llama_model_free(model);
    llama_backend_free();
    return n_diff > 0 ? 0 : 1;
}
