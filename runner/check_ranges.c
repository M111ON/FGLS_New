/* Quick check: what are the actual weight ranges? */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

static float q8_dequant(int8_t q, uint16_t sc16) {
    int sign = (sc16 >> 15) & 1;
    int exp = (sc16 >> 10) & 0x1f;
    int mantissa = sc16 & 0x3ff;
    float scale;
    if (exp == 0) scale = (sign ? -1 : 1) * ldexp(mantissa, -24);
    else if (exp == 31) scale = (sign ? -1 : 1) * INFINITY;
    else scale = (sign ? -1 : 1) * ldexp(1.0 + mantissa / 1024.0, exp - 15);
    return q * scale;
}

int main(int argc, char **argv) {
    const char *fn = argv[1];
    FILE *f = fopen(fn, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    fseek(f, 4096, SEEK_SET);

    int count = 0;
    float global_min = 1e30f, global_max = -1e30f;
    float all_scales[100];
    int n_scales = 0;

    uint8_t buf[34];
    int consecutive = 0;

    while (count < 20) {
        if (fread(buf, 1, 34, f) != 34) break;
        uint16_t sc16;
        memcpy(&sc16, buf + 32, 2);
        int exp = (sc16 >> 10) & 0x1f;
        int valid = (sc16 != 0 && sc16 != 0x7c00 && sc16 != 0xfc00 && exp > 0 && exp < 30);
        if (valid) {
            consecutive++;
            if (consecutive < 4) continue;
        } else {
            consecutive = 0;
            continue;
        }

        float scale = q8_dequant(1, sc16); /* just the scale */
        float weights[32];
        float wmin = 1e30f, wmax = -1e30f;
        for (int i = 0; i < 32; i++) {
            weights[i] = q8_dequant((int8_t)buf[i], sc16);
            if (weights[i] < wmin) wmin = weights[i];
            if (weights[i] > wmax) wmax = weights[i];
        }
        if (wmin < global_min) global_min = wmin;
        if (wmax > global_max) global_max = wmax;

        printf("Block %2d: scale=%.6f  range=[%.4f, %.4f]  delta_range=%.4f\n",
               count, scale, wmin, wmax, wmax - wmin);

        if (n_scales < 100) all_scales[n_scales++] = fabsf(scale);
        count++;
    }

    /* Stats */
    float sum = 0;
    for (int i = 0; i < n_scales; i++) sum += all_scales[i];
    float avg_scale = sum / n_scales;

    printf("\nGlobal weight range: [%.4f, %.4f]\n", global_min, global_max);
    printf("Global delta: %.4f\n", global_max - global_min);
    printf("Avg |scale|: %.6f\n", avg_scale);
    printf("Avg delta / avg_scale: %.4f\n", (global_max - global_min) / avg_scale);

    fclose(f);
    return 0;
}
