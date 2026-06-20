#include "kv_tensor_access.h"
#include "llama.h"
#include "llama-context.h"
#include "llama-kv-cache.h"

extern "C" {

int kv_get_cache_tensors(void *ctx,
    void **k_data, void **v_data,
    size_t *k_size, size_t *v_size,
    int *n_embd_k, int *n_head_kv, int *layer_id,
    int max_layers) 
{
    if (!ctx) return -1;
    llama_context *lctx = (llama_context *)ctx;
    auto *mem = lctx->memory.get();
    if (!mem) return -1;
    
    /* Try dynamic_cast to llama_kv_cache */
    llama_kv_cache *kv = dynamic_cast<llama_kv_cache*>(mem);
    if (!kv) return -1;
    
    int n = 0;
    for (auto &layer : kv->layers) {
        if (n >= max_layers) break;
        if (!layer.k || !layer.v) continue;
        
        if (k_data) k_data[n] = layer.k->data;
        if (v_data) v_data[n] = layer.v->data;
        if (k_size) k_size[n] = (size_t)layer.k->nb[1] * layer.k->ne[1]; /* row_stride * rows */
        if (v_size) v_size[n] = (size_t)layer.v->nb[1] * layer.v->ne[1];
        if (n_embd_k) n_embd_k[n] = (int)layer.k->ne[0];
        if (n_head_kv) n_head_kv[n] = 0; /* not directly available here */
        if (layer_id) layer_id[n] = (int)layer.il;
        n++;
    }
    return n;
}

} /* extern "C" */
