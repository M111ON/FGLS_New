// test_tokenize2.c — isolate tokenize crash
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "llama.h"

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model *model = llama_model_load_from_file(argv[1], llama_model_default_params());
    if (!model) return 1;
    fprintf(stderr, "[t2] model loaded\n"); fflush(stderr);

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    fprintf(stderr, "[t2] vocab=%p\n", (void*)vocab); fflush(stderr);

    int n_vocab = llama_vocab_n_tokens(vocab);
    fprintf(stderr, "[t2] n_vocab=%d\n", n_vocab); fflush(stderr);

    // Try with add_special=false
    llama_token tokens[1024];
    const char *prompt = "Hello";
    int n = llama_tokenize(vocab, prompt, -1, tokens, 1024, false, false);
    fprintf(stderr, "[t2] tokenize returned %d\n", n); fflush(stderr);

    // Try tokenize with BPE pre-tokenizer
    n = llama_tokenize(vocab, prompt, -1, tokens, 1024, true, true);
    fprintf(stderr, "[t2] tokenize(parse) returned %d\n", n); fflush(stderr);

    llama_model_free(model);
    llama_backend_free();
    return 0;
}
