#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "llama.h"

typedef int (*sid_fn)(void*);
typedef int (*sid_fn_enum)(void*, char(*)[64], void**, size_t*, int);

int main(int argc, char**argv) {
    if(argc<2){fprintf(stderr,"Usage: %s model.gguf\n",argv[0]);return 1;}
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params mp=llama_model_default_params();
    fprintf(stderr,"[dbg] loading model...\n");
    struct llama_model*model=llama_model_load_from_file(argv[1],mp);
    if(!model){fprintf(stderr,"ERROR: model load\n");llama_backend_free();return 1;}
    fprintf(stderr,"[dbg] model loaded ok\n");

    HMODULE h=LoadLibraryA("sid_tensor_helper.dll");
    if(!h){fprintf(stderr,"ERROR: load DLL\n");return 1;}

    sid_fn c=(sid_fn)GetProcAddress(h,"sid_tensor_count");
    sid_fn ca=(sid_fn)GetProcAddress(h,"sid_tensor_count_all");
    sid_fn_enum e=(sid_fn_enum)GetProcAddress(h,"sid_tensor_enum_all");

    if(!c||!ca){fprintf(stderr,"ERROR: missing symbols\n");return 1;}
    fprintf(stderr,"[dbg] calling tensor count...\n");

    int direct=c(model);
    fprintf(stderr,"[dbg] sid_tensor_count returned %d\n", direct);
    int all=ca(model);
    fprintf(stderr,"[tensor] direct=%d all=%d\n", direct, all);

    if(e){
        char names[2048][64];
        void *datas[2048];
        size_t nbytes[2048];
        int n=e(model,names,datas,nbytes,2048);
        fprintf(stderr,"[tensor] enum=%d tensors:\n", n);
        for(int i=0;i<n&&i<50;i++)fprintf(stderr,"  %s (size=%llu)\n",names[i],(unsigned long long)nbytes[i]);
        if(n>50)fprintf(stderr,"  ... (%d more)\n",n-50);
    }

    FreeLibrary(h);
    llama_model_free(model);llama_backend_free();
    fprintf(stderr,"[dbg] done\n");
    return 0;
}
