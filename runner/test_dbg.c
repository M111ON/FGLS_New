#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "llama.h"

typedef int (*sid_fn)(void*);
typedef int (*sid_fn_debug)(void*);

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage...\n");return 1;}
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params mp=llama_model_default_params();
    struct llama_model*model=llama_model_load_from_file(argv[1],mp);
    if(!model){fprintf(stderr,"ERROR: model load\n");llama_backend_free();return 1;}
    fprintf(stderr,"Model at %p\n", (void*)model);

    HMODULE h=LoadLibraryA("sid_tensor_helper.dll");
    if(!h){fprintf(stderr,"ERROR: load DLL\n");llama_model_free(model);llama_backend_free();return 1;}

    sid_fn_debug dbg=(sid_fn_debug)GetProcAddress(h,"sid_tensor_debug");
    sid_fn c=(sid_fn)GetProcAddress(h,"sid_tensor_count");
    if(!dbg||!c){fprintf(stderr,"ERROR: missing symbols\n");FreeLibrary(h);llama_model_free(model);llama_backend_free();return 1;}

    fprintf(stderr,"--- model struct dump ---\n");
    dbg(model);
    fprintf(stderr,"--- scan result ---\n");
    int n=c(model);
    fprintf(stderr,"[tensor] direct=%d\n", n);

    FreeLibrary(h);
    llama_model_free(model);llama_backend_free();
    return 0;
}
