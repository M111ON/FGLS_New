#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "dramtile_store.h"

int main(void) {
    DRamTileStore s;
    if (dt_store_init_twin(&s, "test_model.dramtile", 64UL * 1024 * 1024) != 0) {
        fprintf(stderr, "init failed\n"); return 1;
    }
    uint8_t w1[256]; for (int i = 0; i < 256; i++) w1[i] = (uint8_t)i;
    uint8_t w2[128]; memset(w2, 0x42, 128);
    uint8_t w3[64];  memset(w3, 0xAB, 64);
    uint8_t b1[16];  memset(b1, 0, 16);
    dt_put(&s, "blk.0.attn_q.weight", w1, 256);
    dt_put(&s, "blk.0.attn_k.weight", w2, 128);
    dt_put(&s, "blk.0.attn_v.weight", w3, 64);
    dt_put(&s, "blk.0.attn_output.weight", b1, 16);
    dt_put(&s, "blk.0.ffn_gate.weight", w1, 256);
    dt_put(&s, "blk.1.attn_q.weight", w2, 128);
    dt_put(&s, "blk.1.attn_k.weight", w3, 64);
    dt_put(&s, "token_embd.weight", w1, 256);
    dt_put(&s, "output_norm.weight", b1, 16);
    dt_store_destroy_twin(&s);
    fprintf(stderr, "created test_model.dramtile with 9 tensors\n");
    return 0;
}