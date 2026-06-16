#include <stdio.h>
#include <windows.h>
#include "llama.h"

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <model>\n", argv[0]); return 1; }
    const char *model_path = argv[1];
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    fprintf(stderr, "[minitest] loading model...\n");
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    fprintf(stderr, "[minitest] model=%p\n", (void*)model);
    if (model) {
        struct llama_context_params cp = llama_context_default_params();
        struct llama_context *ctx = llama_new_context_with_model(model, cp);
        fprintf(stderr, "[minitest] ctx=%p\n", (void*)ctx);
        if (ctx) llama_free(ctx);
        llama_model_free(model);
    }
    fprintf(stderr, "[minitest] done\n");
    return 0;
}
