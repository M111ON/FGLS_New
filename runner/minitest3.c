#include <stdio.h>
#include <windows.h>
#include "llama.h"

static LONG WINAPI veh_handler(EXCEPTION_POINTERS *ep) {
    fprintf(stderr, "[minitest3] CRASH: code=0x%X at addr=%p\n",
        ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <model>\n", argv[0]); return 1; }
    const char *model_path = argv[1];
    
    AddVectoredExceptionHandler(1, veh_handler);
    
    fprintf(stderr, "[minitest3] step 1: llama_backend_init...\n");
    fflush(stderr);
    llama_backend_init();
    
    fprintf(stderr, "[minitest3] step 2: ggml_backend_load...\n");
    fflush(stderr);
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    
    fprintf(stderr, "[minitest3] step 3: llama_model_default_params...\n");
    fflush(stderr);
    struct llama_model_params mp = llama_model_default_params();
    
    fprintf(stderr, "[minitest3] step 4: llama_model_load_from_file...\n");
    fflush(stderr);
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    
    fprintf(stderr, "[minitest3] step 5: model=%p\n", (void*)model);
    fflush(stderr);
    
    if (model) {
        fprintf(stderr, "[minitest3] step 6: loading context...\n");
        fflush(stderr);
        struct llama_context_params cp = llama_context_default_params();
        cp.n_ctx = 512;
        struct llama_context *ctx = llama_new_context_with_model(model, cp);
        fprintf(stderr, "[minitest3] step 7: ctx=%p\n", (void*)ctx);
        fflush(stderr);
        if (ctx) llama_free(ctx);
        llama_model_free(model);
    }
    
    fprintf(stderr, "[minitest3] done\n");
    RemoveVectoredExceptionHandler(veh_handler);
    return 0;
}
