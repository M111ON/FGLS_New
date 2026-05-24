#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diamond_shell_codec.h"

static void gen_data(uint8_t *buf, size_t n, int pat) {
    for (size_t i = 0; i < n; i++) {
        switch (pat) {
            case 0: buf[i] = (uint8_t)(32 + (i % 90)); break;
            case 1: buf[i] = (uint8_t)((i % 256) ^ (i >> 8)); break;
            case 2: buf[i] = (uint8_t)(i * 2654435761ULL ^ (i >> 3)); break;
            case 3: buf[i] = (uint8_t)(i % 17); break;
            case 4: buf[i] = ((i % 64) < 4) ? (uint8_t)(i & 0xFF) : 0; break;
        }
    }
}

int main(void) {
    printf("Diamond Shell Codec — lossless verify\n\n");

    const uint64_t N = 512;
    uint8_t *original = malloc(N * SHELL_CHUNK_SZ);
    uint8_t *encoded  = malloc(N * 66);   /* worst case 66B/chunk */
    uint8_t *decoded  = malloc(N * SHELL_CHUNK_SZ);

    const char *names[] = {"Text-like","Structured","Pseudo-random","Repetitive","Sparse"};
    int total_pass = 0, total_tests = 0;

    for (int pat = 0; pat < 5; pat++) {
        gen_data(original, N * SHELL_CHUNK_SZ, pat);

        uint64_t enc_bytes = shell_stream_encode(original, N, encoded);
        uint64_t dec_bytes = shell_stream_decode(encoded, N, decoded);

        int match = (memcmp(original, decoded, N * SHELL_CHUNK_SZ) == 0);
        double ratio = (double)(N * SHELL_CHUNK_SZ) / (double)enc_bytes;

        printf("[%s] %-16s enc=%llu dec=%llu ratio=%.2fx  %s\n",
               match ? "PASS" : "FAIL",
               names[pat],
               (unsigned long long)enc_bytes,
               (unsigned long long)dec_bytes,
               ratio,
               match ? "" : "MISMATCH!");

        if (match) total_pass++;
        total_tests++;
    }

    /* edge: single chunk */
    uint8_t one[64], one_enc[66], one_dec[64];
    memset(one, 0xAB, 64);
    shell_stream_encode(one, 1, one_enc);
    shell_stream_decode(one_enc, 1, one_dec);
    int ok = (memcmp(one, one_dec, 64) == 0);
    printf("[%s] single chunk 0xAB×64\n", ok?"PASS":"FAIL");
    if (ok) total_pass++; total_tests++;

    memset(one, 0x00, 64);
    shell_stream_encode(one, 1, one_enc);
    shell_stream_decode(one_enc, 1, one_dec);
    ok = (memcmp(one, one_dec, 64) == 0);
    printf("[%s] single chunk 0x00×64 (FLAT)\n", ok?"PASS":"FAIL");
    if (ok) total_pass++; total_tests++;

    printf("\n%d/%d PASS\n", total_pass, total_tests);

    free(original); free(encoded); free(decoded);
    return (total_pass == total_tests) ? 0 : 1;
}
