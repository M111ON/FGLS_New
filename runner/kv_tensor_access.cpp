#include "kv_tensor_access.h"
#include <stdio.h>

/* Stub: avoids linking against private llama_kv_cache C++ internals.
 * Real implementation needs full llama.cpp source built with symbol exports. */

extern "C" {

int kv_get_cache_tensors(void *ctx,
    void **k_data, void **v_data,
    size_t *k_size, size_t *v_size,
    int *n_embd_k, int *n_head_kv, int *layer_id,
    int max_layers)
{
    (void)ctx; (void)k_data; (void)v_data;
    (void)k_size; (void)v_size;
    (void)n_embd_k; (void)n_head_kv; (void)layer_id;
    (void)max_layers;
    fprintf(stderr, "[kv-tensor] stub: returning 0 (KV remap disabled)\n");
    return 0;
}

int kv_get_cache_tensor_ptrs(void *ctx,
    void **k_tensor_ptr, void **v_tensor_ptr,
    int max_layers)
{
    (void)ctx; (void)k_tensor_ptr; (void)v_tensor_ptr;
    (void)max_layers;
    fprintf(stderr, "[kv-ptr] stub: returning 0 (KV remap disabled)\n");
    return 0;
}

} /* extern "C" */
