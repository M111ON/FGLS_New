// test_modelonly.c — ONLY load model, nothing else
#include <stdio.h>
#include <windows.h>
#include "llama.h"

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model *model = llama_model_load_from_file(argv[1], llama_model_default_params());
    fprintf(stderr, "[mo] load returned model=%p\n", (void*)model);
    fflush(stderr);
    if (model) {
        llama_model_free(model);
        fprintf(stderr, "[mo] freed\n");
        fflush(stderr);
    }
    llama_backend_free();
    fprintf(stderr, "[mo] done\n");
    fflush(stderr);
    return model ? 0 : 1;
}
