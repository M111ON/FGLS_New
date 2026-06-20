#ifndef KV_TENSOR_ACCESS_H
#define KV_TENSOR_ACCESS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Get number of KV cache layers and their tensor data pointers.
   Returns 0 on success, -1 if not available (e.g. no KV cache, recurrent-only).
   layer_out[] must have room for at most max_layers entries.
   For each layer with KV cache:
     k_data[l] = pointer to K data buffer
     v_data[l] = pointer to V data buffer
     k_size[l] = K data size in bytes (n_embd_k_gqa * kv_size * sizeof(f16))
     v_size[l] = V data size in bytes
     n_embd_k  = K head dimension
     n_head_kv = number of KV heads in this layer
     layer_id  = model layer index */
int kv_get_cache_tensors(void *ctx,
    void **k_data, void **v_data,
    size_t *k_size, size_t *v_size,
    int *n_embd_k, int *n_head_kv, int *layer_id,
    int max_layers);

#ifdef __cplusplus
}
#endif

#endif /* KV_TENSOR_ACCESS_H */
