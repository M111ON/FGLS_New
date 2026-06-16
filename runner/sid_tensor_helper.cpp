// sid_tensor_helper.cpp - MSVC DLL: find tensors via ggml_context linked list
// Compile: cl /LD /O2 /I"include" sid_tensor_helper.cpp

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>

#define MAX_MODEL_TENSORS 512

struct llama_model;

// Globals for ggml_context mem_buffer (set by find_context_process_wide)
static uint8_t *g_ctx_mem_start = NULL;
static size_t g_ctx_mem_size = 0;
static void *g_ggml_context = NULL;

// ============================================================
// Struct definitions matching the actual b9528 DLL layout
// ============================================================

// ggml_object header (32 bytes) - precedes each allocated object in the linear buffer
struct ggml_object {
    size_t offs;                // 0:8  - offset within mem_buffer
    size_t size;                // 8:8  - allocated size (GGML_MEM_ALIGN padded)
    struct ggml_object * next;  // 16:8 - linked list next
    int   type;                 // 24:4 - 0=TENSOR, 1=GRAPH, 2=WORK_BUFFER
    char  pad[4];               // 28:4 - align to 32
}; // total: 32 bytes

// ggml_context (40 bytes) - linear allocator containing all tensors
struct ggml_context {
    size_t mem_size;            // 0:8
    void  *mem_buffer;          // 8:8
    bool   mem_buffer_owned;    // 16:1
    bool   no_alloc;            // 17:1
    char   pad0[2];             // 18:2
    int    n_objects;           // 20:4
    void  *objects_begin;       // 24:8 - linked list head (ggml_object*)
    void  *objects_end;         // 32:8 - linked list tail (ggml_object*)
}; // total: 40 bytes

// ggml_tensor (336 bytes, GGML_MAX_SRC=10)
struct ggml_tensor {
    int            type;        // 0:4
    void*          buffer;      // 8:8
    int64_t        ne[4];       // 16:32
    size_t         nb[4];       // 48:32
    int            op;          // 80:4
    int            op_params[16]; // 84:64
    int            flags;       // 148:4
    struct ggml_tensor* src[10]; // 152:80
    struct ggml_tensor* view_src; // 232:8
    size_t         view_offs;   // 240:8
    void*          data;        // 248:8
    char           name[64];    // 256:64
    void*          extra;       // 320:8
    char           pad[8];      // 328:8
}; // total: 336 bytes

#define SID_EXPORT __declspec(dllexport)
#define SCAN_MODEL_SIZE 4096
#define GGML_OBJECT_TYPE_TENSOR 0

// ============================================================
// Memory helpers
// ============================================================

static int safe_ptr(void *p) {
    if (!p || (uintptr_t)p < 0x10000) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    return (mbi.State == MEM_COMMIT) ? 1 : 0;
}

static int find_mmap_region(void **base_out, size_t *size_out) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t *addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < si.lpMaximumApplicationAddress) {
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize > 500ULL * 1024 * 1024 &&
            mbi.Type == MEM_MAPPED && (mbi.Protect & PAGE_READONLY)) {
            *base_out = mbi.BaseAddress;
            *size_out = mbi.RegionSize;
            return 0;
        }
        addr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    return -1;
}

// ============================================================
// Find ggml_context by scanning model struct for n_objects=291
// ============================================================

// Check if a pointer looks like a valid ggml_context
static int is_valid_context(void *cand) {
    if (!cand || (uintptr_t)cand < 0x10000) return 0;
    if (!safe_ptr(cand)) return 0;
    __try {
        // Read struct fields
        int nobj; memcpy(&nobj, (const char*)cand + 20, sizeof(nobj));
        if (nobj < 10 || nobj > 10000) return 0; // need at least 10 objects to be plausible
        void *mbuf; memcpy(&mbuf, (const char*)cand + 8, sizeof(mbuf));
        if (mbuf == cand) return 0; // mem_buffer must differ from context address
        if (!mbuf || (uintptr_t)mbuf < 0x10000) return 0;
        if (!safe_ptr(mbuf)) return 0;
        void *obegin; memcpy(&obegin, (const char*)cand + 24, sizeof(obegin));
        if (!obegin || (uintptr_t)obegin < 0x10000) return 0;
        if (!safe_ptr(obegin)) return 0;
        // Verify objects_begin looks like a ggml_object:
        // First field (offs) should be reasonable, type should be 0-2
        size_t offs; memcpy(&offs, obegin, sizeof(offs));
        if (offs > 1024ULL * 1024) return 0; // offset within mem_buffer can't be huge
        size_t osize; memcpy(&osize, (const char*)obegin + 8, sizeof(osize));
        if (osize < 32 || osize > 1024ULL * 1024) return 0; // object size 32 bytes min
        int otype; memcpy(&otype, (const char*)obegin + 24, sizeof(otype));
        if (otype < 0 || otype > 2) return 0; // must be TENSOR, GRAPH, or WORK_BUFFER
        void *oend; memcpy(&oend, (const char*)cand + 32, sizeof(oend));
        if (!oend || (uintptr_t)oend < 0x10000 || !safe_ptr(oend)) return 0;
        return nobj;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Process-wide scan for ggml_context
static struct ggml_context* find_context_process_wide() {
    int best_nobj = 0;
    struct ggml_context *best_ctx = NULL;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t *addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD scan_types[] = {MEM_PRIVATE};
    uint64_t hits = 0, regions = 0;
    while (addr < si.lpMaximumApplicationAddress) {
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        regions++;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            mbi.RegionSize >= 4096) {
            uint8_t *start = (uint8_t*)mbi.BaseAddress;
            uint8_t *end = start + mbi.RegionSize;
            // Scan every 8 bytes
            for (uint8_t *p = start; p + 40 <= end; p += 8) {
                int nobj;
                __try {
                    memcpy(&nobj, p + 20, sizeof(nobj));
                } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (nobj < 100 || nobj > 10000) continue;
                // Quick sanity: check mem_buffer
                void *mbuf; memcpy(&mbuf, p + 8, sizeof(mbuf));
                if (!mbuf || mbuf == (void*)p) continue;
                if ((uintptr_t)mbuf < 0x10000) continue;
                // Full validation
                if (!safe_ptr(mbuf)) continue;
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
                // Verify at least first few tensor objects have non-NULL data
                int has_data = 0;
                void *ob_iter = obegin;
                for (int oi = 0; oi < 5 && ob_iter && has_data == 0; oi++) {
                    int ttype; memcpy(&ttype, (const char*)ob_iter + 24, sizeof(ttype));
                    if (ttype == GGML_OBJECT_TYPE_TENSOR) {
                        size_t toffs; memcpy(&toffs, ob_iter, sizeof(toffs));
                        void *tensor = (uint8_t*)mbuf + toffs;
                        void *tdata; memcpy(&tdata, (const char*)tensor + 248, sizeof(tdata));
                        if (tdata) has_data = 1;
                    }
                    void *next; memcpy(&next, (const char*)ob_iter + 16, sizeof(next));
                    ob_iter = next;
                }
                if (!has_data && nobj > 50) continue; // skip metadata-only contexts

                hits++;
                if (nobj > best_nobj) {
                    best_nobj = nobj;
                    best_ctx = (struct ggml_context*)p;
                    g_ggml_context = (void*)p;
                    void *mbuf; memcpy(&mbuf, (const char*)p + 8, sizeof(mbuf));
                    g_ctx_mem_start = (uint8_t*)mbuf;
                    g_ctx_mem_size = 0x200000; // 2MB generous
                }
            }
        }
        addr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    fprintf(stderr, "[ctx] Scanned %llu regions, hit %llu candidates, best n_objects=%d",
        (unsigned long long)regions, (unsigned long long)hits, best_nobj);
    if (best_ctx)
        fprintf(stderr, " at %p\n", best_ctx);
    else
        fprintf(stderr, " (none found)\n");
    return best_ctx;
}



// ============================================================
// Enumerate all tensors via context's linked list
// ============================================================

// Try to read tensor name from the struct. Try both GGML_MAX_SRC=10 (offset 256) and =9 (offset 248)
static int read_tensor_name(const struct ggml_tensor *t, char *out, int maxlen) {
    out[0] = 0;
    // Try offset 256 (GGML_MAX_SRC=10)
    const char *name_at = (const char*)t + 256;
    __try {
        if (name_at[0] >= 32 && name_at[0] <= 126) {
            strncpy(out, name_at, maxlen-1); out[maxlen-1]=0;
            return 1;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    // Try offset 248 (GGML_MAX_SRC=9)
    name_at = (const char*)t + 248;
    __try {
        if (name_at[0] >= 32 && name_at[0] <= 126) {
            strncpy(out, name_at, maxlen-1); out[maxlen-1]=0;
            return 1;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

static size_t tensor_nbytes_from_fields(const struct ggml_tensor *t) {
    size_t nb = (size_t)t->ne[0] * t->nb[0];
    for (int i = 1; i < 4; i++) {
        size_t nbi = (size_t)t->ne[i] * t->nb[i];
        if (nbi > nb) nb = nbi;
    }
    return nb;
}

static int enumerate_tensors(struct ggml_context *ctx, void *mmap_base, size_t mmap_size,
    void **ptrs_out, char (*names_out)[64], void **datas_out, size_t *nbyteses, int max_count)
{
    if (!ctx || !ctx->mem_buffer || !ctx->objects_begin) return -1;
    int n = 0;
    struct ggml_object *obj = (struct ggml_object*)ctx->objects_begin;
    while (obj && n < max_count) {
        __try {
            if (obj->type == GGML_OBJECT_TYPE_TENSOR) {
                struct ggml_tensor *t = (struct ggml_tensor*)((char*)ctx->mem_buffer + obj->offs);
                if (ptrs_out) ptrs_out[n] = (void*)t;
                if (datas_out) datas_out[n] = t->data;
                if (nbyteses) nbyteses[n] = tensor_nbytes_from_fields(t);
                if (names_out) {
                    if (!read_tensor_name(t, names_out[n], 64))
                        snprintf(names_out[n], 64, "tensor_%d", n);
                }
                n++;
            }
            obj = obj->next;
        } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
    }
    return n;
}

// ============================================================
// Exported API
// ============================================================

extern "C" {

SID_EXPORT int sid_tensor_enum(struct llama_model * model, void **ptrs_out,
    char (*names_out)[64], void ** datas_out, size_t * nbyteses, int max_count)
{
    if (!model || !ptrs_out || !names_out || !datas_out) return -1;
    void *mmap_base = NULL; size_t mmap_size = 0;
    find_mmap_region(&mmap_base, &mmap_size); // non-fatal if not found

    fprintf(stderr, "[tensor] searching for ggml_context (process-wide)...\n");
    struct ggml_context *ctx = find_context_process_wide();
    if (!ctx) {
        fprintf(stderr, "[tensor] ggml_context NOT FOUND\n");
        return -1;
    }
    fprintf(stderr, "[tensor] ggml_context: mem_buffer=%p n_objects=%d objects_begin=%p\n",
        ctx->mem_buffer, ctx->n_objects, ctx->objects_begin);

    int n = enumerate_tensors(ctx, mmap_base, mmap_size,
        ptrs_out, names_out, datas_out, nbyteses, max_count);
    fprintf(stderr, "[tensor] enumerated %d tensors via context linked list\n", n);
    if (n > 0) {
        for (int i = 0; i < n && i < 5; i++)
            fprintf(stderr, "[tensor]   [%d] ptr=%p data=%p name='%s'\n",
                i, ptrs_out[i], datas_out[i], names_out[i]);
        if (n > 5) fprintf(stderr, "[tensor]   ... (%d more)\n", n - 5);
    }
    return n;
}

SID_EXPORT int sid_tensor_find(struct llama_model * model, const char * name,
    void ** struct_out, void ** data_out, size_t * nbytes)
{
    if (!model || !name || !struct_out || !data_out) return -1;
    const int max = 4096;
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

SID_EXPORT int sid_tensor_swap(struct llama_model * model, const char * name,
    void * new_data, void ** old_data)
{
    if (!model || !name || !new_data || !old_data) return -1;
    const int max = 4096;
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

// Swap ALL tensors' data pointers in one batch
// names[i], new_datas[i]: arrays of length n
// old_datas[i]: output array (can be NULL)
// Returns number of tensors swapped
SID_EXPORT int sid_tensor_swap_all(struct llama_model * model,
    const char **names, void **new_datas,
    void **old_datas, int n)
{
    if (!model || !names || !new_datas) return -1;
    const int max = 4096;
    void **ptrs = (void**)calloc(max, sizeof(void*));
    char (*tnames)[64] = (char(*)[64])calloc(max, 64);
    void **datas = (void**)calloc(max, sizeof(void*));
    if (!ptrs || !tnames || !datas) { free(ptrs); free(tnames); free(datas); return -2; }
    int total = sid_tensor_enum(model, ptrs, tnames, datas, NULL, max);
    if (total < 0) { free(ptrs); free(tnames); free(datas); return -1; }
    int swapped = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < total; j++) {
            if (strcmp(tnames[j], names[i]) == 0) {
                struct ggml_tensor *t = (struct ggml_tensor *)ptrs[j];
                if (old_datas) old_datas[i] = t->data;
                t->data = new_datas[i];
                swapped++;
                break;
            }
        }
    }
    free(ptrs); free(tnames); free(datas);
    return swapped;
}

// Debug: just call sid_tensor_enum and print summary
SID_EXPORT int sid_tensor_debug(struct llama_model * model) {
    const int max = 4096;
    void **ptrs = (void**)calloc(max, sizeof(void*));
    char (*names)[64] = (char(*)[64])calloc(max, 64);
    void **datas = (void**)calloc(max, sizeof(void*));
    size_t *nbytes = (size_t*)calloc(max, sizeof(size_t));
    if (!ptrs || !names || !datas || !nbytes) { free(ptrs); free(names); free(datas); free(nbytes); return -2; }
    int n = sid_tensor_enum(model, ptrs, names, datas, nbytes, max);
    fprintf(stderr, "[debug] %d tensors\n", n);
    size_t total = 0;
    for (int i = 0; i < n && i < 5; i++) {
        fprintf(stderr, "[debug]   [%d] ptr=%p data=%p name='%s' nbytes=%zu\n",
            i, ptrs[i], datas[i], names[i], nbytes[i]);
        total += nbytes[i];
    }
    if (n > 5) {
        for (int i = 5; i < n; i++) total += nbytes[i];
        fprintf(stderr, "[debug]   ... (%d more, total_data=%.2f MB)\n", n-5, total/1048576.0);
    }
    free(ptrs); free(names); free(datas); free(nbytes);
    return n;
}

SID_EXPORT int sid_tensor_restore_all(struct llama_model * model) { return 0; }

// ============================================================
// Model-struct scan: find ggml_tensor* by scanning llama_model at 8-byte offsets
// bypassing ggml_context linked list. This is the approach that works.
// ============================================================

// Check if pointer is any valid ggml_tensor (no name filter)
static int is_valid_tensor_ptr(const char *cand) {
    __try {
        // Read name at offset 256 (GGML_MAX_SRC=10)
        const char *name = cand + 256;
        if (name[0] < 32 || name[0] > 126) return 0;
        int nlen = (int)strnlen(name, 64);
        if (nlen < 3 || nlen >= 64) return 0;
        // Reject if name contains path chars (=, \, /) or non-tensor chars
        for (int i = 0; i < nlen; i++) {
            char c = name[i];
            if (c == '\\' || c == '/' || c == '=' || c == ':') return 0;
        }
        // Verify first char is alphanumeric or underscore (not a separator)
        if (!((name[0] >= 'a' && name[0] <= 'z') ||
              (name[0] >= 'A' && name[0] <= 'Z') ||
              (name[0] >= '0' && name[0] <= '9') ||
              name[0] == '_' || name[0] == '.')) return 0;
        // Sanity: ne[0] should be reasonable
        int64_t ne0; memcpy(&ne0, cand + 16, sizeof(ne0));
        if (ne0 <= 0 || ne0 > 10000000) return 0;
        // type should be in range
        int type; memcpy(&type, cand, sizeof(type));
        if (type < 0 || type > 43) return 0;
        // data non-NULL
        void *data; memcpy(&data, cand + 248, sizeof(data));
        if (!data || (uintptr_t)data < 0x10000) return 0;
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Check pointer looks like a valid ggml_tensor with given name
// Rejects names containing path characters
static int valid_tensor_name_only(const char *name, int nlen) {
    if (nlen < 3 || nlen >= 64) return 0;
    for (int i = 0; i < nlen; i++) {
        char c = name[i];
        if (c == '\\' || c == '/' || c == '=' || c == ':' || c == ';' || c == '|') return 0;
    }
    if (!((name[0] >= 'a' && name[0] <= 'z') ||
          (name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= '0' && name[0] <= '9') ||
          name[0] == '_' || name[0] == '.')) return 0;
    return 1;
}

static int is_tensor_with_name(const char *cand, const char *target_name) {
    __try {
        // Read name at offset 256 (GGML_MAX_SRC=10)
        const char *name = cand + 256;
        // Check name starts with printable ASCII
        if (name[0] < 32 || name[0] > 126) return 0;
        // Null-terminated check
        int nlen = (int)strnlen(name, 64);
        if (!valid_tensor_name_only(name, nlen)) return 0;
        if (strcmp(name, target_name) != 0) return 0;
        // Sanity check: ne[0] should be reasonable (positive, not huge)
        int64_t ne0; memcpy(&ne0, cand + 16, sizeof(ne0));
        if (ne0 <= 0 || ne0 > 10000000) return 0;
        // Sanity check: type should be 0..43 (ggml_type range)
        int type; memcpy(&type, cand, sizeof(type));
        if (type < 0 || type > 43) return 0;
        // data pointer should be non-NULL and look valid
        void *data; memcpy(&data, cand + 248, sizeof(data));
        if (!data || (uintptr_t)data < 0x10000) return 0;
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

SID_EXPORT int sid_tensor_find_in_model(struct llama_model * model,
    const char * name, void ** struct_out, void ** data_out, size_t * nbytes)
{
    if (!model || !name || !struct_out || !data_out) return -1;
    int found = 0;

    // Scan model memory region
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(model, &mbi, sizeof(mbi)) != sizeof(mbi)) return -1;

    uint8_t *start = (uint8_t*)mbi.AllocationBase;
    uint8_t *end = start + mbi.RegionSize;
    // Scan 64KB for tensor pointers (model struct and nearby)
    uint8_t *scan_end = start + 65536;
    if (scan_end > end) scan_end = end;

    for (uint8_t *p = start; p + 8 <= scan_end; p += 8) {
        void *cand;
        memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        // Check if this pointer looks like a valid tensor with our name
        if (is_tensor_with_name((const char*)cand, name)) {
            struct_out[0] = cand;
            memcpy(&data_out[0], (const char*)cand + 248, sizeof(void*));
            if (nbytes) {
                // Calculate nbytes from ne[] and nb[]
                int64_t ne[4]; memcpy(ne, (const char*)cand + 16, sizeof(ne));
                size_t nb[4]; memcpy(nb, (const char*)cand + 48, sizeof(nb));
                size_t total = nb[0] * ne[0];
                for (int i = 1; i < 4; i++) {
                    size_t ni = nb[i] * ne[i];
                    if (ni > total) total = ni;
                }
                *nbytes = total;
            }
            found = 1;
            break;
        }
    }

    if (!found) {
        // Fallback: scan nearby heap regions within 1MB of model allocation
        uint8_t *heap_start = start;
        MEMORY_BASIC_INFORMATION hmbi;
        for (uint8_t *ha = heap_start; ha < start + 0x100000; ) {
            if (VirtualQuery(ha, &hmbi, sizeof(hmbi)) != sizeof(hmbi)) break;
            if (hmbi.State == MEM_COMMIT && hmbi.Type == MEM_PRIVATE) {
                uint8_t *rend = (uint8_t*)hmbi.BaseAddress + hmbi.RegionSize;
                if (rend > start + 0x100000) rend = start + 0x100000;
                for (uint8_t *p = (uint8_t*)hmbi.BaseAddress; p + 8 <= rend && !found; p += 8) {
                    __try {
                        void *cand; memcpy(&cand, p, sizeof(cand));
                        if (!cand || (uintptr_t)cand < 0x10000) continue;
                        if (is_tensor_with_name((const char*)cand, name)) {
                            struct_out[0] = cand;
                            memcpy(&data_out[0], (const char*)cand + 248, sizeof(void*));
                            if (nbytes) {
                                int64_t ne[4]; memcpy(ne, (const char*)cand + 16, sizeof(ne));
                                size_t nb[4]; memcpy(nb, (const char*)cand + 48, sizeof(nb));
                                size_t total = nb[0] * ne[0];
                                for (int i = 1; i < 4; i++) {
                                    size_t ni = nb[i] * ne[i];
                                    if (ni > total) total = ni;
                                }
                                *nbytes = total;
                            }
                            found = 1;
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                }
            }
            ha = (uint8_t*)hmbi.BaseAddress + hmbi.RegionSize;
        }
    }

done:
    fprintf(stderr, "[model_scan] find '%s': %s tensor=%p data=%p nbytes=%zu\n",
        name, found ? "FOUND" : "NOT FOUND",
        struct_out[0], data_out[0], nbytes ? *nbytes : 0);
    return found ? 0 : -1;
}

// ============================================================
// Find ALL model-struct tensor copies
// Scans model struct + layers allocation only (no process-wide scan)
// ============================================================

SID_EXPORT int sid_tensor_enum_model(struct llama_model * model,
    void ** ptrs_out, char names_out[][64], void ** datas_out,
    size_t * nbytes_out, int max_count)
{
    if (!model || !ptrs_out || !datas_out) return -1;

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(model, &mbi, sizeof(mbi)) != sizeof(mbi)) return -1;
    uint8_t *model_base = (uint8_t*)mbi.AllocationBase;

    int count = 0;
    void *found_ptrs[MAX_MODEL_TENSORS] = {0};

    // First: get ggml_context tensors for name reference
    void *ctx_ptrs[MAX_MODEL_TENSORS] = {0};
    char ctx_names[MAX_MODEL_TENSORS][64] = {{0}};
    void *ctx_datas[MAX_MODEL_TENSORS] = {0};
    size_t ctx_nbytes[MAX_MODEL_TENSORS] = {0};
    int ctx_n = sid_tensor_enum(model, ctx_ptrs, ctx_names, ctx_datas, ctx_nbytes, MAX_MODEL_TENSORS);

    if (ctx_n <= 0) {
        fprintf(stderr, "[model_enum] ERROR: no ggml_context tensors\n");
        return -1;
    }

    // Tier 1: Scan model struct allocation for direct tensor pointers
    uint8_t *model_end = model_base + 65536;
    for (uint8_t *p = model_base; p + 8 < model_end && count < MAX_MODEL_TENSORS; p += 8) {
        void *cand; memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        if (!is_valid_tensor_ptr((const char*)cand)) continue;
        // Check name matches known tensor
        const char *name = (const char*)cand + 256;
        int known = 0;
        for (int i = 0; i < ctx_n && !known; i++)
            if (strcmp(name, ctx_names[i]) == 0) known = 1;
        if (!known) continue;
        // Dedup
        int dup = 0;
        for (int i = 0; i < count && !dup; i++)
            if (found_ptrs[i] == cand) dup = 1;
        if (dup) continue;
        found_ptrs[count++] = cand;
        fprintf(stderr, "[tensor]   [%d] ptr=%p data=%p name='%s'\n",
            count-1, cand, *(void**)((char*)cand+248), name);
    }

    // Tier 2: Find layers array in model struct
    void *layers_ptr = NULL;
    for (uint8_t *p = model_base; p + 8 < model_end; p += 8) {
        void *cand; memcpy(&cand, p, sizeof(cand));
        if (!cand || (uintptr_t)cand < 0x10000) continue;
        // Check if cand is within heap
        MEMORY_BASIC_INFORMATION cmbi;
        if (VirtualQuery(cand, &cmbi, sizeof(cmbi)) != sizeof(cmbi)) continue;
        if (cmbi.State != MEM_COMMIT) continue;
        // Scan for "blk.0." tensor name within first 4096 bytes of cand
        for (int off = 0; off < 4096; off += 8) {
            void *subcand; memcpy(&subcand, (char*)cand + off, sizeof(subcand));
            if (!subcand || (uintptr_t)subcand < 0x10000) continue;
            if (!is_valid_tensor_ptr((const char*)subcand)) continue;
            const char *name = (const char*)subcand + 256;
            if (strncmp(name, "blk.0.", 6) == 0) { layers_ptr = cand; break; }
        }
        if (layers_ptr) break;
    }

    // Tier 3: Scan layers allocation for tensor pointers
    if (layers_ptr) {
        MEMORY_BASIC_INFORMATION lmbi;
        if (VirtualQuery(layers_ptr, &lmbi, sizeof(lmbi)) == sizeof(lmbi) &&
            lmbi.State == MEM_COMMIT) {
            uint8_t *lays = (uint8_t*)lmbi.BaseAddress;
            uint8_t *laye = lays + lmbi.RegionSize;
            if (laye > lays + 0x200000) laye = lays + 0x200000;
            for (uint8_t *p = lays; p + 8 < laye && count < MAX_MODEL_TENSORS; p += 8) {
                void *cand; memcpy(&cand, p, sizeof(cand));
                if (!cand || (uintptr_t)cand < 0x10000) continue;
                if (!is_valid_tensor_ptr((const char*)cand)) continue;
                // Check name matches
                const char *name = (const char*)cand + 256;
                // Must start with "blk." for layer tensors
                if (strncmp(name, "blk.", 4) != 0) continue;
                int known = 0;
                for (int i = 0; i < ctx_n && !known; i++)
                    if (strcmp(name, ctx_names[i]) == 0) known = 1;
                if (!known) continue;
                // Dedup
                int dup = 0;
                for (int i = 0; i < count && !dup; i++)
                    if (found_ptrs[i] == cand) dup = 1;
                if (dup) continue;
                found_ptrs[count++] = cand;
            }
            fprintf(stderr, "[model_enum] layers region: %p..%p\n", lays, laye);
        }
    }

    // Fill output arrays
    int out_count = count;
    if (max_count > 0 && max_count < count) out_count = max_count;
    for (int i = 0; i < out_count; i++) {
        ptrs_out[i] = found_ptrs[i];
        memcpy(&datas_out[i], (const char*)found_ptrs[i] + 248, sizeof(void*));
        // Copy datas for name lookup, then find matching ctx name for names_out/nbytes_out
        int found_idx = -1;
        const char *tname = (const char*)found_ptrs[i] + 256;
        for (int j = 0; j < ctx_n && found_idx < 0; j++)
            if (strcmp(tname, ctx_names[j]) == 0) found_idx = j;
        if (names_out && found_idx >= 0)
            strncpy(names_out[i], ctx_names[found_idx], 63);
        else if (names_out)
            { strncpy(names_out[i], tname, 63); names_out[i][63] = 0; }
        if (nbytes_out)
            nbytes_out[i] = (found_idx >= 0) ? ctx_nbytes[found_idx] : 0;
    }

    fprintf(stderr, "[model_enum] found %d tensors total\n", out_count);
    return out_count;
}

// Export for external access to ggml_context mem_buffer
SID_EXPORT void* sid_tensor_get_ctx_mem() { return g_ctx_mem_start; }

} // extern "C"
