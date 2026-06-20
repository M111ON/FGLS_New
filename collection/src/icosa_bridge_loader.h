#ifndef ICOSA_BRIDGE_LOADER_H
#define ICOSA_BRIDGE_LOADER_H

#include <stdint.h>
#include <windows.h>

typedef struct {
    HMODULE  dll;
    void   *(*create)(uint64_t gen2, uint64_t gen3);
    void    (*destroy)(void *ctx);
    int     (*valid)(void *ctx);
    int     (*dispatch)(void *ctx, const uint64_t *addrs, const uint64_t *values,
                        uint32_t n, uint64_t gen3, uint32_t c144_tag,
                        uint64_t baseline, uint64_t *out_routes, uint8_t *out_events);
    void   *(*alloc)(void *ctx, size_t size);
    int     (*free)(void *ctx, void *ptr);
    int     (*memcpy_h2d)(void *ctx, void *dst, const void *src, size_t size);
    int     (*sync)(void *ctx);
} IcosaBridge;

static inline int icosa_bridge_load(IcosaBridge *br, const char *dll_path) {
    memset(br, 0, sizeof(*br));
    br->dll = LoadLibraryA(dll_path);
    if (!br->dll) return -1;
    br->create    = (void*(*)(uint64_t,uint64_t))GetProcAddress(br->dll, "icosa_gpu_ctx_create");
    br->destroy   = (void(*)(void*))GetProcAddress(br->dll, "icosa_gpu_ctx_destroy");
    br->valid     = (int(*)(void*))GetProcAddress(br->dll, "icosa_gpu_ctx_valid");
    br->dispatch  = (int(*)(void*,const uint64_t*,const uint64_t*,uint32_t,uint64_t,uint32_t,uint64_t,uint64_t*,uint8_t*))GetProcAddress(br->dll, "icosa_gpu_dispatch");
    br->alloc     = (void*(*)(void*,size_t))GetProcAddress(br->dll, "icosa_gpu_alloc");
    br->free      = (int(*)(void*,void*))GetProcAddress(br->dll, "icosa_gpu_free");
    br->memcpy_h2d= (int(*)(void*,void*,const void*,size_t))GetProcAddress(br->dll, "icosa_gpu_memcpy_h2d");
    br->sync      = (int(*)(void*))GetProcAddress(br->dll, "icosa_gpu_sync");
    if (!br->create || !br->destroy || !br->valid || !br->dispatch) {
        FreeLibrary(br->dll);
        memset(br, 0, sizeof(*br));
        return -2;
    }
    return 0;
}

static inline void icosa_bridge_unload(IcosaBridge *br) {
    if (br->dll) FreeLibrary(br->dll);
    memset(br, 0, sizeof(*br));
}

#endif /* ICOSA_BRIDGE_LOADER_H */
