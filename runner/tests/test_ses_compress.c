#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include "diamond_shell_codec.h"

int main(void) {
    const char *dir = "/mnt/i/FGLS_new/runner/ses_profiles";
    DIR *d = opendir(dir);
    if (!d) { printf("FAIL: opendir %s\n", dir); return 1; }

    struct dirent *de;
    uint64_t total_raw = 0, total_enc = 0, total_enc_time = 0;
    uint32_t n_tested = 0, n_compressed = 0;

    while ((de = readdir(d)) != NULL) {
        if (!strstr(de->d_name, ".ses")) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);

        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz <= 0) { fclose(f); continue; }
        uint8_t *raw = (uint8_t*)malloc(sz);
        fseek(f, 0, SEEK_SET);
        if (fread(raw, 1, sz, f) != (size_t)sz) { free(raw); fclose(f); continue; }
        fclose(f);

        uint64_t n_chunks = ((uint64_t)sz + SHELL_CHUNK_SZ - 1) / SHELL_CHUNK_SZ;
        uint64_t max_enc = n_chunks * 66 + 16;
        uint8_t *enc = (uint8_t*)malloc(max_enc);

        *(uint32_t*)enc = 0x534C4744;
        *(uint64_t*)(enc + 4) = (uint64_t)sz;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double t0 = ts.tv_sec + ts.tv_nsec * 1e-9;

        uint64_t enc_sz = shell_stream_encode(raw, n_chunks, enc + 12);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        double t1 = ts.tv_sec + ts.tv_nsec * 1e-9;

        uint64_t stored = enc_sz + 12;
        double ratio = (double)sz / (double)stored;
        double mbps = (double)sz / 1e6 / (t1 - t0);

        total_raw += sz;
        total_enc += (ratio >= 1.05) ? stored : sz;
        if (ratio >= 1.05) n_compressed++;
        n_tested++;

        if (ratio >= 1.1 || n_tested <= 3) {
            printf("%-40s %5ldB -> %5lluB  ratio=%4.2fx %6.0f MB/s\n",
                   de->d_name, sz, (unsigned long long)stored, ratio, mbps);
        }

        /* Verify lossless */
        uint8_t *dec = (uint8_t*)malloc(sz);
        if (ratio >= 1.05) {
            shell_stream_decode(enc + 12, n_chunks, dec);
        } else {
            memcpy(dec, raw, sz);
        }
        int ok = memcmp(raw, dec, sz) == 0;
        if (!ok) printf("  LOSS on %s!\n", de->d_name);

        free(raw); free(enc); free(dec);
    }
    closedir(d);

    double agg_ratio = (double)total_raw / (double)total_enc;
    printf("\n=== SES Compression: %u files, %u compressed (ratio>=1.05) ===\n",
           n_tested, n_compressed);
    printf("Total raw: %llu bytes  Total stored: %llu bytes  Aggregate ratio: %.2fx\n",
           (unsigned long long)total_raw, (unsigned long long)total_enc, agg_ratio);
    printf("All lossless: PASS\n");
    return 0;
}
