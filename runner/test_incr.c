// test_incr.c — incremental test: find exact crash point in inference path
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "llama.h"

typedef int (*sid_find_fn)(void*, const char*, void**, void**, size_t*);

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model *model = llama_model_load_from_file(argv[1], llama_model_default_params());
    if (!model) { fprintf(stderr, "ERR:load\n"); return 1; }
    fprintf(stderr, "[incr] model loaded\n"); fflush(stderr);

    HMODULE h = LoadLibraryA("sid_tensor_helper.dll");
    sid_find_fn fn = (sid_find_fn)GetProcAddress(h, "sid_tensor_find");
    if (!fn) return 1;
    fprintf(stderr, "[incr] DLL ready\n"); fflush(stderr);

    const char *prompt = "The meaning of life is";

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    fprintf(stderr, "[incr] got vocab\n"); fflush(stderr);

    llama_token tokens[1024];
    int n_tokens = llama_tokenize(vocab, prompt, -1, tokens, 1024, true, false);
    fprintf(stderr, "[incr] tokenized: %d tokens\n", n_tokens); fflush(stderr);

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 512;
    cparams.n_batch = n_tokens;

    fprintf(stderr, "[incr] about to init context...\n"); fflush(stderr);
    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "ERR:ctx init\n"); return 1; }
    fprintf(stderr, "[incr] context created\n"); fflush(stderr);

    llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    fprintf(stderr, "[incr] about to decode...\n"); fflush(stderr);
    if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr, "ERR:decode\n"); llama_free(ctx); return 1;
    }
    fprintf(stderr, "[incr] decode OK\n"); fflush(stderr);

    float *logits = llama_get_logits_ith(ctx, 0);
    fprintf(stderr, "[incr] logits[0]=%f\n", logits[0]); fflush(stderr);

    llama_free(ctx);
    FreeLibrary(h);
    llama_model_free(model);
    llama_backend_free();
    fprintf(stderr, "[incr] done\n");
    return 0;
}
