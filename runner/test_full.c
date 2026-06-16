#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "llama.h"

typedef int (*sid_enum_fn)(void*, void**, char(*)[64], void**, size_t*, int);
typedef int (*sid_debug_fn)(void*);

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage: test_full model.gguf\n");return 1;}
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params mp=llama_model_default_params();
    struct llama_model*model=llama_model_load_from_file(argv[1],mp);
    if(!model){fprintf(stderr,"ERROR: model load\n");llama_backend_free();return 1;}
    fprintf(stderr,"Model loaded OK\n");

    HMODULE h=LoadLibraryA("sid_tensor_helper.dll");
    if(!h){fprintf(stderr,"ERROR: load DLL\n");llama_model_free(model);llama_backend_free();return 1;}

    sid_enum_fn enum_fn=(sid_enum_fn)GetProcAddress(h,"sid_tensor_enum");
    sid_debug_fn debug_fn=(sid_debug_fn)GetProcAddress(h,"sid_tensor_debug");
    if(!enum_fn||!debug_fn){fprintf(stderr,"ERROR: missing symbols\n");FreeLibrary(h);llama_model_free(model);llama_backend_free();return 1;}

    // Full tensor enumeration
    void *ptrs[4096];
    char names[4096][64];
    void *datas[4096];
    size_t nbytes[4096];
    int n = enum_fn(model, ptrs, names, datas, nbytes, 4096);
    fprintf(stderr,"[test] enumerated %d tensors\n", n);
    for (int i = 0; i < n && i < 10; i++) {
        fprintf(stderr,"  [%3d] ptr=%p data=%p name='%s' nbytes=%zu\n",
            i, ptrs[i], datas[i], names[i], nbytes[i]);
    }
    if (n > 10) fprintf(stderr,"  ... (%d more)\n", n-10);

    // Debug summary
    debug_fn(model);

    FreeLibrary(h);
    llama_model_free(model);llama_backend_free();
    return n > 0 ? 0 : 1;
}
