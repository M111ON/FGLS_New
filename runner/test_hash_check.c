#include <stdio.h>
#include <string.h>
#include "dramtile_store.h"
int main() {
    const char *names[] = {
        "blk.0.attn_q.weight",
        "blk.0.attn_k.weight",
        "blk.0.ffn_up.weight",
        "token_embd.weight",
        "output_norm.weight"
    };
    for (int n = 0; n < 5; n++) {
        uint32_t h = 0;
        const char *s = names[n];
        while (*s) { h = h * 31 + (unsigned char)*s; s++; }
        fprintf(stderr, "%-30s -> slot %u\n", names[n], h % DT_HASH_SLOTS);
    }
    return 0;
}
