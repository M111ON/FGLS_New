#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "gguf_index.h"
int main() {
    GGUFTensorIndex idx;
    memset(&idx,0,sizeof(idx));
    int r = gguf_idx_open("I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf", &idx);
    printf("r=%d n_tensors=%llu data_sec_off=%llu\n",
           r, (unsigned long long)idx.n_tensors,
           (unsigned long long)idx.data_sec_off);
    printf("offsets[0]=%llu offsets[1]=%llu\n",
           (unsigned long long)idx.offsets[0],
           (unsigned long long)idx.offsets[1]);
    printf("gguf_idx_tensor_abs_offset(&idx,0)=%llu\n",
           (unsigned long long)gguf_idx_tensor_abs_offset(&idx,0));
    printf("sizes[0]=%zu\n", idx.sizes[0]);
    gguf_idx_close(&idx);
    return 0;
}
