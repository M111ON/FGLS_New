// test_diag.c — diagnostic: find exact crash point after model load
#include <stdio.h>
#include <windows.h>
#include "llama.h"

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model *model = llama_model_load_from_file(argv[1], llama_model_default_params());
    
    // Write directly to disk file (bypass CRT buffering)
    FILE *f = fopen("I:\\FGLS_new\\runner\\diag_log.txt", "w");
    if (f) {
        fprintf(f, "model=%p\n", (void*)model);
        if (model) {
            fprintf(f, "vocab=%p\n", (void*)llama_model_get_vocab(model));
        }
        fclose(f);
    }
    
    // Return immediately without any further llama calls
    if (model) llama_model_free(model);
    llama_backend_free();
    return 0;
}
