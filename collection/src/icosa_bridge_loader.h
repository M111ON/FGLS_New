#ifndef ICOSA_BRIDGE_LOADER_H
#define ICOSA_BRIDGE_LOADER_H

#include <stdint.h>
#include <windows.h>

/* Bermuda GPU route entry (matches device struct) */
typedef struct {
    uint16_t idx_in;
    uint16_t idx_out;
    uint8_t  zone;
    uint8_t  pole;
    uint8_t  shape;
    uint8_t  polarity;
    uint16_t tring_slot;
} BermudaRouteEntry;

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
    int     (*batch_memcpy_h2d)(void *ctx, void *const *dst, const void *const *src, const size_t *sizes, int n);
    int     (*batch_memcpy_d2h)(void *ctx, void *const *dst, const void *const *src, const size_t *sizes, int n);
    int     (*pin_host)(void *ctx, void **ptr, size_t size);
    int     (*unpin_host)(void *ctx, void *ptr);
    /* Bermuda GPU batch traverse */
    int     (*bermuda_dispatch)(void *ctx, const uint16_t *idxs_in,
                                BermudaRouteEntry *out, uint8_t gear, uint8_t mode, uint32_t n);
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
    br->memcpy_h2d    = (int(*)(void*,void*,const void*,size_t))GetProcAddress(br->dll, "icosa_gpu_memcpy_h2d");
    br->sync          = (int(*)(void*))GetProcAddress(br->dll, "icosa_gpu_sync");
    br->batch_memcpy_h2d = (int(*)(void*,void*const*,void const*const*,size_t const*,int))GetProcAddress(br->dll, "icosa_gpu_batch_memcpy_h2d");
    br->batch_memcpy_d2h = (int(*)(void*,void*const*,void const*const*,size_t const*,int))GetProcAddress(br->dll, "icosa_gpu_batch_memcpy_d2h");
    br->pin_host      = (int(*)(void*,void**,size_t))GetProcAddress(br->dll, "icosa_gpu_pin_host");
    br->unpin_host    = (int(*)(void*,void*))GetProcAddress(br->dll, "icosa_gpu_unpin_host");
    br->bermuda_dispatch = (int(*)(void*,const uint16_t*,BermudaRouteEntry*,uint8_t,uint8_t,uint32_t))GetProcAddress(br->dll, "bermuda_gpu_dispatch");
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
