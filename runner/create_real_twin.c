#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "dramtile_store.h"

/* Simulate Q8_0 quantized weights (real distribution) */
static float q8_dequant(uint8_t q, float scale) {
    return ((float)q - 128.0f) * scale;
}

int main(void) {
    DRamTileStore s;
    if (dt_store_init_twin(&s, "real_weights.dramtile", 64UL * 1024 * 1024) != 0) {
        fprintf(stderr, "init failed\n"); return 1;
    }

    srand(42);

    /* blk.0.attn_q.weight — 64 Q8_0 values (typical Qwen2 range) */
    /* Real Qwen2 attn_q: scale ~0.003, values -128..+127 → range ~ -0.38..+0.38 */
    float q[16];
    {
        float scale = 0.003f;
        for (int i = 0; i < 16; i++) {
            uint8_t qv = (uint8_t)(rand() % 256);
            q[i] = q8_dequant(qv, scale);
        }
    }
    dt_put(&s, "attn_q", (uint8_t*)q, sizeof(q));

    /* blk.0.attn_k.weight — smaller range */
    float k[16];
    {
        float scale = 0.002f;
        for (int i = 0; i < 16; i++) {
            uint8_t qv = (uint8_t)(rand() % 256);
            k[i] = q8_dequant(qv, scale);
        }
    }
    dt_put(&s, "attn_k", (uint8_t*)k, sizeof(k));

    /* blk.0.ffn_up.weight — wider range, some large spikes */
    float up[16];
    {
        float scale = 0.01f;
        for (int i = 0; i < 16; i++) {
            uint8_t qv = (uint8_t)(rand() % 256);
            up[i] = q8_dequant(qv, scale);
        }
    }
    dt_put(&s, "ffn_up", (uint8_t*)up, sizeof(up));

    /* token_embd.weight — embedding typically larger range */
    float embd[8] = { -0.0234f, 0.0156f, -0.0089f, 0.0312f,
                       0.0067f, -0.0198f, 0.0245f, -0.0123f };
    dt_put(&s, "embed", (uint8_t*)embd, sizeof(embd));

    /* output_norm.weight — layernorm ~1.0 ± tiny */
    float norm[4] = { 0.9873f, 1.0127f, 0.9954f, 1.0046f };
    dt_put(&s, "norm", (uint8_t*)norm, sizeof(norm));

    fprintf(stderr, "stored: %u tensors, %zu bytes\n", s.n_stored, s.used);

    /* Print the actual values */
    fprintf(stderr, "\n=== Q8_0 Quantized Weight Values ===\n");
    fprintf(stderr, "attn_q  (scale=0.003):\n");
    for (int i = 0; i < 4; i++) {
        fprintf(stderr, "  ");
        for (int j = 0; j < 4; j++) fprintf(stderr, "%10.6f ", q[i*4+j]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "attn_k  (scale=0.002):\n");
    for (int i = 0; i < 4; i++) {
        fprintf(stderr, "  ");
        for (int j = 0; j < 4; j++) fprintf(stderr, "%10.6f ", k[i*4+j]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "ffn_up  (scale=0.01):\n");
    for (int i = 0; i < 4; i++) {
        fprintf(stderr, "  ");
        for (int j = 0; j < 4; j++) fprintf(stderr, "%10.6f ", up[i*4+j]);
        fprintf(stderr, "\n");
    }

    /* Find min/max across all */
    float all_min = 1e9, all_max = -1e9;
    for (int i = 0; i < 16; i++) {
        if (q[i] < all_min) all_min = q[i]; if (q[i] > all_max) all_max = q[i];
        if (k[i] < all_min) all_min = k[i]; if (k[i] > all_max) all_max = k[i];
        if (up[i] < all_min) all_min = up[i]; if (up[i] > all_max) all_max = up[i];
    }
    fprintf(stderr, "\nRange: min=%.6f  max=%.6f\n", all_min, all_max);

    dt_store_destroy_twin(&s);
    return 0;
}
