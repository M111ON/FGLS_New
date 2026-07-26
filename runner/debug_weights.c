#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#define GGUF_MAGIC 0x46554747

int main(int argc, char **argv) {
    const char *fn = argv[1];
    FILE *f = fopen(fn, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);

    printf("File size: %ld MB\n", sz / 1048576);
    printf("Magic: 0x%08X\n", *(uint32_t*)data);

    /* Scan for first 5 Q8 blocks */
    int count = 0;
    for (long pos = 0; pos + 34 <= sz && count < 5; pos++) {
        uint16_t sc = (uint16_t)data[pos] | ((uint16_t)data[pos+1] << 8);
        if (sc == 0) continue;
        int same = 1;
        for (int i = 1; i < 32; i++)
            if (data[pos+2+i] != data[pos+2]) { same = 0; break; }
        if (same) continue;

        /* fp16 decode */
        int sign = (sc >> 15) & 1;
        int exp = (sc >> 10) & 0x1F;
        int mant = sc & 0x3FF;
        float scale;
        if (exp == 0) scale = (sign?-1:1) * powf(2,-14) * (mant/1024.0f);
        else if (exp == 31) scale = (sign?-1:1) * 1e30f;
        else scale = (sign?-1:1) * powf(2, (float)(exp-15)) * (1.0f + mant/1024.0f);

        printf("\nBlock at pos=%ld: fp16_raw=0x%04X scale=%.6f\n", pos, sc, scale);
        printf("  int8 values: ");
        for (int i = 0; i < 8; i++) printf("%d ", (int8_t)data[pos+2+i]);
        printf("...\n");
        printf("  dequant[0..3]: ");
        for (int i = 0; i < 4; i++) {
            float w = (float)((int8_t)data[pos+2+i]) * scale;
            printf("%.6f ", w);
        }
        printf("\n");
        count++;
    }
    free(data);
    return 0;
}
