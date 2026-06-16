// test_tokenize.c — does llama_tokenize work without DLL loaded?
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "llama.h"

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model *model = llama_model_load_from_file(argv[1], llama_model_default_params());
    if (!model) { fprintf(stderr, "ERR:load\n"); return 1; }
    fprintf(stderr, "[tok] model loaded\n"); fflush(stderr);

    // Load and unload the DLL immediately
    HMODULE h = LoadLibraryA("sid_tensor_helper.dll");
    if (!h) { fprintf(stderr, "ERR:DLL\n"); return 1; }
    fprintf(stderr, "[tok] DLL loaded\n"); fflush(stderr);
    FreeLibrary(h);
    fprintf(stderr, "[tok] DLL freed\n"); fflush(stderr);

    const char *prompt = "The meaning of life is";
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    fprintf(stderr, "[tok] got vocab\n"); fflush(stderr);

    llama_token tokens[1024];
    int n_tokens = llama_tokenize(vocab, prompt, -1, tokens, 1024, true, false);
    fprintf(stderr, "[tok] tokenized: %d tokens\n", n_tokens); fflush(stderr);

    llama_model_free(model);
    llama_backend_free();
    fprintf(stderr, "[tok] done\n");
    return 0;
}
