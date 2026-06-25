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

/* Get ggml_tensor pointers for K/V per layer (for pointer swap eviction).
   k_tensor_ptr[l] = ggml_tensor* for K cache layer l
   v_tensor_ptr[l] = ggml_tensor* for V cache layer l
   These can be used to swap tensor->data directly (zero-copy eviction).
   Returns number of layers, or -1 on error. */
int kv_get_cache_tensor_ptrs(void *ctx,
    void **k_tensor_ptr, void **v_tensor_ptr,
    int max_layers);

#ifdef __cplusplus
}
#endif

#endif /* KV_TENSOR_ACCESS_H */
