#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "addr_space.h"
#include "gguf_reader.h"

int main() {
    GgufReader ggr;
    int r = gguf_open("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", &ggr);
    if (r != 0) { printf("FAIL\n"); return 1; }

    uint32_t n = ggr.n_tensors;

    /* check for address collisions */
    uint8_t *used = (uint8_t*)calloc(20736, 1);
    int collisions = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = addr_from_tensor_name(ggr.names[i], 0);
        if (used[addr]) {
            collisions++;
            printf("COLLISION addr=%u: '%s' vs ", addr, ggr.names[i]);
            for (uint32_t j = 0; j < i; j++) {
                uint32_t addr2 = addr_from_tensor_name(ggr.names[j], 0);
                if (addr2 == addr) { printf("'%s'\n", ggr.names[j]); break; }
            }
        }
        used[addr] = 1;
    }
    printf("Collisions: %d / %u (%.1f%%)\n", collisions, n, 100.0*collisions/n);

    gguf_close(&ggr);
    free(used);
    return 0;
}
