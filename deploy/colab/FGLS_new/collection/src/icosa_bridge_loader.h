#ifndef ICOSA_BRIDGE_LOADER_H
#define ICOSA_BRIDGE_LOADER_H

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  typedef HMODULE DL_HANDLE;
  #define DL_OPEN(n)    LoadLibraryA(n)
  #define DL_SYM(h,n)   GetProcAddress(h,n)
  #define DL_CLOSE(h)   FreeLibrary(h)
#else
  #include <dlfcn.h>
  typedef void* DL_HANDLE;
  #define DL_OPEN(n)    dlopen(n, RTLD_LAZY | RTLD_LOCAL)
  #define DL_SYM(h,n)   dlsym(h,n)
  #define DL_CLOSE(h)   dlclose(h)
#endif

typedef struct {
    DL_HANDLE dll;
    void   *(*create)(uint64_t gen2, uint64_t gen3);
    void    (*destroy)(void *ctx);
    int     (*valid)(void *ctx);
    int     (*dispatch)(void *ctx, const uint64_t *addrs, const uint64_t *values,
                        uint32_t n, uint64_t gen3, uint32_t c144_tag,
                        uint64_t baseline, uint64_t *out_routes, uint8_t *out_events);
} IcosaBridge;

static inline int icosa_bridge_load(IcosaBridge *br, const char *dll_path) {
    memset(br, 0, sizeof(*br));
    br->dll = DL_OPEN(dll_path);
    if (!br->dll) return -1;
    br->create   = (void*(*)(uint64_t,uint64_t))DL_SYM(br->dll, "icosa_gpu_ctx_create");
    br->destroy  = (void(*)(void*))DL_SYM(br->dll, "icosa_gpu_ctx_destroy");
    br->valid    = (int(*)(void*))DL_SYM(br->dll, "icosa_gpu_ctx_valid");
    br->dispatch = (int(*)(void*,const uint64_t*,const uint64_t*,uint32_t,uint64_t,uint32_t,uint64_t,uint64_t*,uint8_t*))DL_SYM(br->dll, "icosa_gpu_dispatch");
    if (!br->create || !br->destroy || !br->valid || !br->dispatch) {
        DL_CLOSE(br->dll);
        memset(br, 0, sizeof(*br));
        return -2;
    }
    return 0;
}

static inline void icosa_bridge_unload(IcosaBridge *br) {
    if (br->dll) DL_CLOSE(br->dll);
    memset(br, 0, sizeof(*br));
}

#endif
