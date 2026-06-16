#include <stdio.h>
#include <stddef.h>
#include "llama.h"
int main() {
    printf("sizeof(llama_model_params)=%zu\n", sizeof(struct llama_model_params));
    printf("offsetof(devices)=%zu\n", offsetof(struct llama_model_params, devices));
    printf("offsetof(n_gpu_layers)=%zu\n", offsetof(struct llama_model_params, n_gpu_layers));
    printf("offsetof(split_mode)=%zu\n", offsetof(struct llama_model_params, split_mode));
    printf("offsetof(vocab_only)=%zu\n", offsetof(struct llama_model_params, vocab_only));
    printf("offsetof(no_alloc)=%zu\n", offsetof(struct llama_model_params, no_alloc));
    return 0;
}
