/* Debug: check beamcode encoding */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

static uint8_t weight_to_beamcode(float weight) {
    int q = (int)roundf(weight);
    if (q < -128) q = -128;
    if (q > 127) q = 127;
    return (uint8_t)(q + 128);
}

static float beamcode_to_weight(uint8_t code) {
    return (float)((int)code - 128);
}

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
    int consecutive = 0;
    uint8_t buf[34];

    printf("Testing BeamCode encoding on Q8_0 blocks:\n\n");

    while (count < 5) {
        if (fread(buf, 1, 34, f) != 34) break;
        uint16_t sc16;
        memcpy(&sc16, buf + 32, 2);
        int exp = (sc16 >> 10) & 0x1f;
        int valid = (sc16 != 0 && sc16 != 0x7c00 && sc16 != 0xfc00 && exp > 0 && exp < 30);
        if (valid) { consecutive++; if (consecutive < 4) continue; }
        else { consecutive = 0; continue; }

        float weights[32];
        for (int i = 0; i < 32; i++)
            weights[i] = q8_dequant((int8_t)buf[i], sc16);

        printf("Block %d:\n", count);
        printf("  Original: ");
        for (int i = 0; i < 8; i++) printf("%.2f ", weights[i]);
        printf("...\n");

        float sum_delta = 0;
        for (int i = 0; i < 32; i++) {
            uint8_t bc = weight_to_beamcode(weights[i]);
            float reconstructed = beamcode_to_weight(bc);
            float delta = fabsf(weights[i] - reconstructed);
            sum_delta += delta;
        }
        printf("  Avg delta: %.4f\n", sum_delta / 32);
        printf("  Scale: %f\n", q8_dequant(1, sc16));
        count++;
    }

    fclose(f);
    return 0;
}
