// test_simple.c — bare minimum to check behavior
#include <stdio.h>
#include <windows.h>
#include "llama.h"

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage\n"); return 1; }
    fprintf(stderr, "[simple] start\n"); fflush(stderr);
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params mp = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    fprintf(stderr, "[simple] model=%p\n", (void*)model); fflush(stderr);
    if (model) {
        const struct llama_vocab *vocab = llama_model_get_vocab(model);
        fprintf(stderr, "[simple] vocab=%p\n", (void*)vocab); fflush(stderr);
        fprintf(stderr, "[simple] freeing\n"); fflush(stderr);
        llama_model_free(model);
    }
    llama_backend_free();
    fprintf(stderr, "[simple] done\n"); fflush(stderr);
    return 0;
}
