// test_findonly.c — minimal test: load model + DLL, find tensor, exit
#include <stdio.h>
#include <windows.h>
#include "llama.h"

typedef int (*sid_find_fn)(void*, const char*, void**, void**, size_t*);

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: test_findonly model.gguf\n"); return 1; }
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");

    struct llama_model *model = llama_model_load_from_file(argv[1], llama_model_default_params());
    if (!model) { fprintf(stderr, "ERROR: model load\n"); return 1; }
    fprintf(stderr, "[test] model loaded\n");
    fflush(stderr);

    HMODULE h = LoadLibraryA("sid_tensor_helper.dll");
    if (!h) { fprintf(stderr, "ERROR: load DLL\n"); return 1; }
    fprintf(stderr, "[test] DLL loaded\n");
    fflush(stderr);

    sid_find_fn fn = (sid_find_fn)GetProcAddress(h, "sid_tensor_find");
    if (!fn) { fprintf(stderr, "ERROR: symbol\n"); return 1; }
    fprintf(stderr, "[test] found symbol\n");
    fflush(stderr);

    void *ptr = NULL, *data = NULL;
    size_t nbytes = 0;
    int ret = fn(model, "token_embd.weight", &ptr, &data, &nbytes);
    fprintf(stderr, "[test] find returned %d, ptr=%p data=%p nbytes=%zu\n", ret, ptr, data, nbytes);
    fflush(stderr);

    FreeLibrary(h);
    llama_model_free(model);
    llama_backend_free();
    fprintf(stderr, "[test] done\n");
    return 0;
}
