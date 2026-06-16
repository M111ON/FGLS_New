// sid_tensor_mingw.c — MinGW DLL: find tensors via ggml_context linked list
// Compile: gcc -O2 -shared -o sid_tensor_mingw.dll sid_tensor_mingw.c

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <windows.h>

#define SID_EXPORT __declspec(dllexport)

// ============================================================
// Struct definitions matching b9528 DLL layout
// ============================================================

// ggml_object header (32 bytes)
struct ggml_object {
    size_t offs;
    size_t size;
    struct ggml_object *next;
    int   type;
    char  pad[4];
};

// ggml_context (40 bytes)
struct ggml_context {
    size_t mem_size;
    void  *mem_buffer;
    int    mem_buffer_owned;
    int    no_alloc;
    char   pad0[2];
    int    n_objects;
    void  *objects_begin;
    void  *objects_end;
};

// ggml_tensor (336 bytes, GGML_MAX_SRC=10)
struct ggml_tensor {
    int            type;
    void*          buffer;
    int64_t        ne[4];
    size_t         nb[4];
    int            op;
    int            op_params[16];
    int            flags;
    struct ggml_tensor* src[10];
    struct ggml_tensor* view_src;
    size_t         view_offs;
    void*          data;
    char           name[64];
    void*          extra;
};

struct llama_model; // opaque

#define GGML_OBJECT_TYPE_TENSOR 0

// Safe pointer check
static int safe_ptr(const void *p) {
    if (!p || (uintptr_t)p < 0x10000) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)
        && mbi.State == MEM_COMMIT
        && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}

static int validate_ggml_ctx(const void *cand) {
    int nobj;
    memcpy(&nobj, (const char*)cand + 20, sizeof(nobj));
    if (nobj < 100 || nobj > 10000) return 0;
    void *mbuf; memcpy(&mbuf, (const char*)cand + 8, sizeof(mbuf));
    if (!mbuf || mbuf == cand || (uintptr_t)mbuf < 0x10000 || !safe_ptr(mbuf)) return 0;
    void *obegin; memcpy(&obegin, (const char*)cand + 24, sizeof(obegin));
    if (!obegin || (uintptr_t)obegin < 0x10000 || !safe_ptr(obegin)) return 0;
    size_t offs; memcpy(&offs, obegin, sizeof(offs));
    if (offs > 1048576) return 0;
    size_t osize; memcpy(&osize, (const char*)obegin + 8, sizeof(osize));
    if (osize < 32 || osize > 1048576) return 0;
    int otype; memcpy(&otype, (const char*)obegin + 24, sizeof(otype));
    if (otype < 0 || otype > 2) return 0;
    void *oend; memcpy(&oend, (const char*)cand + 32, sizeof(oend));
    if (!oend || (uintptr_t)oend < 0x10000 || !safe_ptr(oend)) return 0;
    // Verify tensor objects have non-NULL data
    int has_data = 0;
    void *iter = obegin;
    for (int i = 0; i < 5 && iter && !has_data; i++) {
        int tt; memcpy(&tt, (const char*)iter + 24, sizeof(tt));
        if (tt == GGML_OBJECT_TYPE_TENSOR) {
            size_t to; memcpy(&to, iter, sizeof(to));
            void *t = (uint8_t*)mbuf + to;
            void *td; memcpy(&td, (const char*)t + 248, sizeof(td));
            if (td) has_data = 1;
        }
        void *nx; memcpy(&nx, (const char*)iter + 16, sizeof(nx));
        iter = nx;
    }
    if (!has_data && nobj > 50) return 0;
    return nobj;
}

static struct ggml_context* find_context_process_wide(void) {
    int best_nobj = 0;
    struct ggml_context *best_ctx = NULL;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t *addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < (uint8_t*)si.lpMaximumApplicationAddress) {
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.RegionSize >= 4096) {
            uint8_t *start = (uint8_t*)mbi.BaseAddress;
            uint8_t *end = start + mbi.RegionSize;
            for (uint8_t *p = start; p + 40 <= end; p += 8) {
                int nobj;
                memcpy(&nobj, p + 20, sizeof(nobj));
                if (nobj < 100 || nobj > 10000) continue;
                void *mbuf; memcpy(&mbuf, p + 8, sizeof(mbuf));
                if (!mbuf || mbuf == (void*)p || (uintptr_t)mbuf < 0x10000 || !safe_ptr(mbuf)) continue;
                void *obegin; memcpy(&obegin, p + 24, sizeof(obegin));
                if (!obegin || (uintptr_t)obegin < 0x10000 || !safe_ptr(obegin)) continue;
                size_t offs; memcpy(&offs, obegin, sizeof(offs));
                if (offs > 1048576) continue;
                size_t osize; memcpy(&osize, (const char*)obegin + 8, sizeof(osize));
                if (osize < 32 || osize > 1048576) continue;
                int otype; memcpy(&otype, (const char*)obegin + 24, sizeof(otype));
                if (otype < 0 || otype > 2) continue;
                void *oend; memcpy(&oend, p + 32, sizeof(oend));
                if (!oend || (uintptr_t)oend < 0x10000 || !safe_ptr(oend)) continue;
                int has_data = 0;
                void *iter = obegin;
                for (int si = 0; si < 5 && iter && !has_data; si++) {
                    int tt; memcpy(&tt, (const char*)iter + 24, sizeof(tt));
                    if (tt == GGML_OBJECT_TYPE_TENSOR) {
                        size_t to; memcpy(&to, iter, sizeof(to));
                        void *t = (uint8_t*)mbuf + to;
                        void *td; memcpy(&td, (const char*)t + 248, sizeof(td));
                        if (td) has_data = 1;
                    }
                    void *nx; memcpy(&nx, (const char*)iter + 16, sizeof(nx));
                    iter = nx;
                }
                if (!has_data && nobj > 50) continue;
                if (nobj > best_nobj) { best_nobj = nobj; best_ctx = (struct ggml_context*)p; }
            }
        }
        addr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    return best_ctx;
}

static size_t tensor_nbytes_from_fields(const struct ggml_tensor *t) {
    size_t nb = (size_t)t->ne[0] * t->nb[0];
    for (int i = 1; i < 4; i++) {
        size_t nbi = (size_t)t->ne[i] * t->nb[i];
        if (nbi > nb) nb = nbi;
    }
    return nb;
}

SID_EXPORT int sid_tensor_enum(struct llama_model *model,
    void **ptrs_out, char (*names_out)[64], void **datas_out, size_t *nbyteses, int max_count)
{
    if (!model || !ptrs_out || !names_out || !datas_out) return -1;
    struct ggml_context *ctx = find_context_process_wide();
    if (!ctx) return -1;
    int n = 0;
    struct ggml_object *obj = (struct ggml_object*)ctx->objects_begin;
    while (obj && n < max_count) {
        if (obj->type == GGML_OBJECT_TYPE_TENSOR) {
            struct ggml_tensor *t = (struct ggml_tensor*)((char*)ctx->mem_buffer + obj->offs);
            ptrs_out[n] = (void*)t;
            datas_out[n] = t->data;
            if (nbyteses) nbyteses[n] = tensor_nbytes_from_fields(t);
            char *n64 = t->name;
            if (n64[0] >= 32 && n64[0] <= 126)
                strncpy(names_out[n], n64, 63), names_out[n][63] = 0;
            else
                sprintf(names_out[n], "tensor_%d", n);
            n++;
        }
        obj = obj->next;
    }
    return n;
}

SID_EXPORT int sid_tensor_find(struct llama_model *model, const char *name,
    void **struct_out, void **data_out, size_t *nbytes)
{
    if (!model || !name || !struct_out || !data_out) return -1;
    int max = 4096;
    void **ptrs = (void**)calloc(max, sizeof(void*));
    char (*names)[64] = (char(*)[64])calloc(max, 64);
    void **datas = (void**)calloc(max, sizeof(void*));
    size_t *nb = (size_t*)calloc(max, sizeof(size_t));
    if (!ptrs || !names || !datas || !nb) { free(ptrs); free(names); free(datas); free(nb); return -2; }
    int n = sid_tensor_enum(model, ptrs, names, datas, nb, max);
    int ret = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0) {
            *struct_out = ptrs[i]; *data_out = datas[i];
            if (nbytes) *nbytes = nb[i]; ret = 0; break;
        }
    free(ptrs); free(names); free(datas); free(nb);
    return ret;
}

SID_EXPORT int sid_tensor_swap(struct llama_model *model, const char *name,
    void *new_data, void **old_data)
{
    if (!model || !name || !new_data || !old_data) return -1;
    int max = 4096;
    void **ptrs = (void**)calloc(max, sizeof(void*));
    char (*names)[64] = (char(*)[64])calloc(max, 64);
    void **datas = (void**)calloc(max, sizeof(void*));
    if (!ptrs || !names || !datas) { free(ptrs); free(names); free(datas); return -2; }
    int n = sid_tensor_enum(model, ptrs, names, datas, NULL, max);
    int ret = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0) {
            struct ggml_tensor *t = (struct ggml_tensor *)ptrs[i];
            *old_data = t->data; t->data = new_data; ret = 0; break;
        }
    free(ptrs); free(names); free(datas);
    return ret;
}
