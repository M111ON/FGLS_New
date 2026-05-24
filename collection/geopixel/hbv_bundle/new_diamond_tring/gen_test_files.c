#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

int main() {
    srand((unsigned)time(NULL));
    uint8_t buf[65536];

    /* 1) zeros */
    memset(buf, 0, 65536);
    FILE *f = fopen("test_zeros.bin", "wb"); fwrite(buf, 1, 65536, f); fclose(f);

    /* 2) random */
    for (int i = 0; i < 65536; i++) buf[i] = (uint8_t)(rand() & 0xFF);
    f = fopen("test_random.bin", "wb"); fwrite(buf, 1, 65536, f); fclose(f);

    /* 3) repeated pattern */
    for (int i = 0; i < 65536; i++) buf[i] = (uint8_t)(i & 0xFF);
    f = fopen("test_count.bin", "wb"); fwrite(buf, 1, 65536, f); fclose(f);

    /* 4) same byte repeated */
    memset(buf, 0xAB, 65536);
    f = fopen("test_repeat.bin", "wb"); fwrite(buf, 1, 65536, f); fclose(f);

    /* 5) sine wave gradient */
    for (int i = 0; i < 65536; i++) buf[i] = (uint8_t)(128 + 127 * sin(i * 0.1));
    f = fopen("test_sine.bin", "wb"); fwrite(buf, 1, 65536, f); fclose(f);

    /* 6) ramp (0,1,2,...) */
    for (int i = 0; i < 65536; i++) buf[i] = (uint8_t)(i & 0xFF);
    f = fopen("test_ramp.bin", "wb"); fwrite(buf, 1, 65536, f); fclose(f);

    /* 7) alternating pattern */
    for (int i = 0; i < 65536; i++) buf[i] = (i & 1) ? 0xFF : 0x00;
    f = fopen("test_alt.bin", "wb"); fwrite(buf, 1, 65536, f); fclose(f);

    /* 8) small file (127 bytes) */
    for (int i = 0; i < 127; i++) buf[i] = (uint8_t)(i * 2);
    f = fopen("test_small.bin", "wb"); fwrite(buf, 1, 127, f); fclose(f);

    /* 9) tiny file (3 bytes) */
    buf[0] = 'P'; buf[1] = 'K'; buf[2] = 0x03;
    f = fopen("test_tiny.bin", "wb"); fwrite(buf, 1, 3, f); fclose(f);

    /* 10) medium ~10KB */
    for (int i = 0; i < 10000; i++) buf[i] = (uint8_t)(rand());
    f = fopen("test_10k.bin", "wb"); fwrite(buf, 1, 10000, f); fclose(f);

    /* 11) text-like (ASCII printable) */
    for (int i = 0; i < 10000; i++) buf[i] = (uint8_t)(32 + (rand() % 95));
    f = fopen("test_ascii.bin", "wb"); fwrite(buf, 1, 10000, f); fclose(f);

    /* 12) mostly same with rare differences */
    memset(buf, 0x42, 65536);
    for (int i = 0; i < 100; i++) buf[rand() % 65536] = (uint8_t)(rand());
    f = fopen("test_sparse_diff.bin", "wb"); fwrite(buf, 1, 65536, f); fclose(f);

    printf("12 test files created\n");
    return 0;
}
