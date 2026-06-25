#include "kv_tensor_access.h"
#include "llama.h"
#include <stdio.h>
#include <stdlib.h>
#include <typeinfo>
#include "llama-kv-cache.h"
#include "llama-memory.h"
#include "llama-memory-hybrid.h"

/* Helper: resolve llama_kv_cache from memory object.
 * Handles standard (llama_kv_cache) and hybrid (llama_memory_hybrid). */
static llama_kv_cache * resolve_kv(llama_memory_t mem) {
    /* Standard transformer */
    llama_kv_cache *kv = dynamic_cast<llama_kv_cache*>(mem);
    if (kv) return kv;

    /* Hybrid (attention + recurrent layers, e.g. LFM2) */
    llama_memory_hybrid *hybrid = dynamic_cast<llama_memory_hybrid*>(mem);
    if (hybrid) {
        kv = hybrid->get_mem_attn();
        if (kv) return kv;
    }

    /* TODO: handle llama_kv_cache_dsa (DeepSeekV3) */
    return nullptr;
}

extern "C" {

int kv_get_cache_tensors(void *ctx,
    void **k_data, void **v_data,
    size_t *k_size, size_t *v_size,
    int *n_embd_k, int *n_head_kv, int *layer_id,
    int max_layers) 
{
    if (!ctx) { fprintf(stderr, "[kv-tensor] ctx is NULL\n"); return -1; }

    llama_memory_t mem = llama_get_memory((const struct llama_context *)ctx);
    if (!mem) { fprintf(stderr, "[kv-tensor] llama_get_memory() returned NULL\n"); return -1; }

    llama_kv_cache *kv = resolve_kv(mem);
    if (!kv) { fprintf(stderr, "[kv-tensor] unsupported memory type: %s\n", typeid(*mem).name()); return -1; }
    size_t nlayers = kv->get_n_layers();
    fprintf(stderr, "[kv-tensor] kv=%p, n_layers=%zu\n", (void*)kv, nlayers);
    
    int n = 0;
    for (size_t i = 0; i < nlayers; i++) {
        if (n >= max_layers) break;
        ggml_tensor *tk = kv->get_layer_k(i);
        ggml_tensor *tv = kv->get_layer_v(i);
        if (!tk || !tv) continue;
        
        if (k_data) k_data[n] = tk->data;
        if (v_data) v_data[n] = tv->data;
        if (k_size) k_size[n] = (size_t)tk->nb[1] * tk->ne[1];
        if (v_size) v_size[n] = (size_t)tv->nb[1] * tv->ne[1];
        if (n_embd_k) n_embd_k[n] = (int)tk->ne[0];
        if (n_head_kv) n_head_kv[n] = 0;
        if (layer_id) layer_id[n] = (int)kv->get_layer_il(i);

        if (i < 3) {
            fprintf(stderr, "[kv-tensor-debug] layer %zu K: data=%p ne[0]=%lld ne[1]=%lld nb[1]=%llu view_src=%p view_offs=%zu buf=%p\n",
                i, tk->data, (long long)tk->ne[0], (long long)tk->ne[1], (unsigned long long)tk->nb[1],
                (void*)tk->view_src, (size_t)tk->view_offs, (void*)tk->buffer);
            fprintf(stderr, "[kv-tensor-debug] layer %zu V: data=%p ne[0]=%lld ne[1]=%lld nb[1]=%llu view_src=%p view_offs=%zu buf=%p\n",
                i, tv->data, (long long)tv->ne[0], (long long)tv->ne[1], (unsigned long long)tv->nb[1],
                (void*)tv->view_src, (size_t)tv->view_offs, (void*)tv->buffer);
        }
        n++;
    }
    return n;
}

int kv_get_cache_tensor_ptrs(void *ctx,
    void **k_tensor_ptr, void **v_tensor_ptr,
    int max_layers)
{
    if (!ctx) { fprintf(stderr, "[kv-ptr] ctx is NULL\n"); return -1; }

    llama_memory_t mem = llama_get_memory((const struct llama_context *)ctx);
    if (!mem) { fprintf(stderr, "[kv-ptr] llama_get_memory() returned NULL\n"); return -1; }

    llama_kv_cache *kv = resolve_kv(mem);
    if (!kv) { fprintf(stderr, "[kv-ptr] unsupported memory type: %s\n", typeid(*mem).name()); return -1; }
    size_t nlayers = kv->get_n_layers();
    fprintf(stderr, "[kv-ptr] kv=%p, n_layers=%zu\n", (void*)kv, nlayers);

    int n = 0;
    for (size_t i = 0; i < nlayers; i++) {
        if (n >= max_layers) break;
        ggml_tensor *tk = kv->get_layer_k(i);
        ggml_tensor *tv = kv->get_layer_v(i);
        if (!tk || !tv) continue;

        if (k_tensor_ptr) k_tensor_ptr[n] = (void*)tk;
        if (v_tensor_ptr) v_tensor_ptr[n] = (void*)tv;
        n++;
    }
    return n;
}

} /* extern "C" */
