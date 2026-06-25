#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "gguf_index.h"

static const char *type_name(uint32_t t) {
    switch(t) {
        case 0: return "F32";
        case 1: return "F16";
        case 2: return "Q4_0";
        case 3: return "Q4_1";
        case 6: return "Q5_0";
        case 7: return "Q5_1";
        case 8: return "Q8_0";
        case 9: return "Q8_1";
        case 10: return "Q2_K";
        case 11: return "Q3_K";
        case 12: return "Q4_K";
        case 13: return "Q5_K";
        case 14: return "Q6_K";
        case 15: return "Q8_K";
        default: return "???";
    }
}

int main(void) {
    GGUFTensorIndex idx;
    if (gguf_idx_open("/mnt/i/model/LFM2.5-1.2B-Instruct-Q4_K_M.gguf", &idx) != 0) {
        printf("FAIL open\n"); return 1;
    }
    printf("n_tensors=%llu\n", (unsigned long long)idx.n_tensors);

    unsigned counts[20] = {0};
    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        uint32_t dt = idx.dtypes[i];
        if (dt < 20) counts[dt]++;
    }
    for (uint32_t t = 0; t < 20; t++) {
        if (counts[t]) printf("  type %u (%s): %u\n", t, type_name(t), counts[t]);
    }

    /* Show first few tensors with their types */
    uint64_t show = idx.n_tensors < 10 ? idx.n_tensors : 10;
    for (uint64_t i = 0; i < show; i++) {
        printf("  [%llu] %-40s %s %llu bytes offset=%llu\n",
               (unsigned long long)i, idx.names[i],
               type_name(idx.dtypes[i]),
               (unsigned long long)idx.sizes[i],
               (unsigned long long)idx.offsets[i]);
    }

    gguf_idx_close(&idx);
    return 0;
}
