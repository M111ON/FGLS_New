// test_swap_mini.c — simplest test_swap to find crash point
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "llama.h"

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage\n"); return 1; }
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params mp = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) { fprintf(stderr, "ERR:load\n"); return 1; }
    fprintf(stderr, "[mini] model loaded\n"); fflush(stderr);

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    fprintf(stderr, "[mini] got vocab\n"); fflush(stderr);

    const char *prompt = "Hello";
    llama_token tokens[1024];
    int n = llama_tokenize(vocab, prompt, -1, tokens, 1024, true, false);
    fprintf(stderr, "[mini] tokenized %d tokens\n", n); fflush(stderr);

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 256;
    cp.n_batch = n;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    fprintf(stderr, "[mini] ctx=%p\n", (void*)ctx); fflush(stderr);

    llama_batch batch = llama_batch_get_one(tokens, n);
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "ERR:decode\n"); return 1; }
    fprintf(stderr, "[mini] decode ok\n"); fflush(stderr);

    float *logits = llama_get_logits_ith(ctx, 0);
    fprintf(stderr, "[mini] logits[0]=%f logits[1]=%f\n", logits[0], logits[1]); fflush(stderr);

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    fprintf(stderr, "[mini] done\n");
    return 0;
}
