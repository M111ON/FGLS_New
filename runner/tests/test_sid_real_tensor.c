#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define _POSIX_C_SOURCE 199309L
#include <time.h>
#include "sid_cache.h"
#include "gguf_index.h"

int main(void) {
    const char *path = "/mnt/i/model/LFM2.5-1.2B-Instruct-Q4_K_M.gguf";

    GGUFTensorIndex idx;
    if (gguf_idx_open(path, &idx) != 0) {
        printf("FAIL: cannot open %s\n", path); return 1;
    }
    printf("Model: %s\n", path);
    printf("Tensors: %llu\n", (unsigned long long)idx.n_tensors);

    const char *targets[] = {
        "blk.0.attn_norm.weight",    /* F32, 8192 bytes */
        "blk.0.shortconv.conv.weight", /* F32, 24576 bytes */
        "blk.0.ffn_down.weight",     /* Q6_K, 13.76 MB */
        "token_embd.weight",          /* Q6_K, 110 MB */
        "token_embd_norm.weight",     /* F32, 8192 bytes */
        "output_norm.weight",         /* F32, 8192 bytes (if exists) */
        NULL
    };

    /* Also test all F32 tensors we find */
    #define MAX_F32 64
    const char *f32_names[MAX_F32];
    uint64_t   f32_sizes[MAX_F32];
    uint32_t   f32_count = 0;
    for (uint64_t i = 0; i < idx.n_tensors && f32_count < MAX_F32; i++) {
        if (idx.dtypes[i] == 0) {
            f32_names[f32_count] = idx.names[i];
            f32_sizes[f32_count] = idx.sizes[i];
            f32_count++;
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: fopen\n"); gguf_idx_close(&idx); return 1; }

    SIDCache cache;
    sid_cache_init(&cache, 384u << 20);

    for (int t = 0; targets[t]; t++) {
        int64_t ti = -1;
        for (uint64_t i = 0; i < idx.n_tensors; i++) {
            if (strcmp(idx.names[i], targets[t]) == 0) { ti = (int64_t)i; break; }
        }
        if (ti < 0) { printf("SKIP: %s (not found)\n", targets[t]); continue; }

        uint64_t sz = idx.sizes[ti];
        uint64_t off = gguf_idx_tensor_abs_offset(&idx, (uint64_t)ti);
        uint8_t *raw = (uint8_t*)malloc(sz);
        fseek(f, (long)off, SEEK_SET);
        if (fread(raw, 1, sz, f) != sz) {
            printf("FAIL: read %s\n", targets[t]); free(raw); continue;
        }

        printf("\n%s (type=%u):\n", targets[t], idx.dtypes[ti]);
        printf("  raw: %llu bytes (%.2f MB)\n",
               (unsigned long long)sz, (double)sz / 1e6);

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double t0 = ts.tv_sec + ts.tv_nsec * 1e-9;

        int r = sid_cache_put_compressed(&cache, targets[t], (uint32_t)ti, raw, sz);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        double t1 = ts.tv_sec + ts.tv_nsec * 1e-9;

        size_t stored_sz = 0;
        for (uint32_t i = 0; i < SID_CACHE_MAX_ENTRIES; i++) {
            if (cache.entries[i].node_id != SID_NODE_SLOTS &&
                strcmp(cache.entries[i].name, targets[t]) == 0) {
                stored_sz = cache.entries[i].size;
                break;
            }
        }

        double ratio = (double)sz / (double)stored_sz;
        double enc_sec = t1 - t0;
        double enc_mbps = (double)sz / 1e6 / enc_sec;

        printf("  compressed: %llu bytes (%.2f MB) ratio: %.2fx\n",
               (unsigned long long)stored_sz, (double)stored_sz / 1e6, ratio);
        printf("  encode: %.3f sec (%.0f MB/s)\n", enc_sec, enc_mbps);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        t0 = ts.tv_sec + ts.tv_nsec * 1e-9;

        uint8_t *got; size_t got_sz;
        r = sid_cache_get(&cache, targets[t], &got, &got_sz);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        t1 = ts.tv_sec + ts.tv_nsec * 1e-9;

        if (r != 0) { printf("  RETRIEVE FAILED\n"); free(raw); continue; }

        double dec_sec = t1 - t0;
        double dec_mbps = (double)sz / 1e6 / dec_sec;
        printf("  decode: %.3f sec (%.0f MB/s)\n", dec_sec, dec_mbps);

        int ok = (got_sz == sz && memcmp(raw, got, sz) == 0);
        printf("  lossless: %s\n", ok ? "PASS" : "FAIL");

        free(raw);
    }

    printf("\n--- F32 tensor batch test (%u tensors) ---\n", f32_count);
    uint64_t total_raw = 0, total_comp = 0;
    for (uint32_t t = 0; t < f32_count; t++) {
        int64_t ti = -1;
        for (uint64_t i = 0; i < idx.n_tensors; i++) {
            if (strcmp(idx.names[i], f32_names[t]) == 0) { ti = (int64_t)i; break; }
        }
        if (ti < 0 || idx.dtypes[ti] != 0) continue;

        uint64_t sz = idx.sizes[ti];
        uint64_t off = gguf_idx_tensor_abs_offset(&idx, (uint64_t)ti);
        uint8_t *raw = (uint8_t*)malloc(sz);
        fseek(f, (long)off, SEEK_SET);
        if (fread(raw, 1, sz, f) != sz) { free(raw); continue; }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double t0 = ts.tv_sec + ts.tv_nsec * 1e-9;

        sid_cache_put_compressed(&cache, f32_names[t], (uint32_t)ti, raw, sz);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        double t1 = ts.tv_sec + ts.tv_nsec * 1e-9;

        size_t stored_sz = 0;
        for (uint32_t i = 0; i < SID_CACHE_MAX_ENTRIES; i++) {
            if (cache.entries[i].node_id != SID_NODE_SLOTS &&
                strcmp(cache.entries[i].name, f32_names[t]) == 0) {
                stored_sz = cache.entries[i].size;
                break;
            }
        }

        double ratio = (double)sz / (double)stored_sz;
        double enc_sec = t1 - t0;

        uint8_t *got; size_t got_sz;
        sid_cache_get(&cache, f32_names[t], &got, &got_sz);
        int ok = (got_sz == sz && memcmp(raw, got, sz) == 0);

        if (!ok) printf("  LOSS on %s!\n", f32_names[t]);

        total_raw += sz;
        total_comp += stored_sz;

        if (ratio >= 1.05 || sz < 100000) {
            double enc_mbps = (double)sz / 1e6 / enc_sec;
            printf("  %-40s %6lluB -> %6lluB  ratio=%5.2fx %6.0f MB/s  %s\n",
                   f32_names[t], (unsigned long long)sz, (unsigned long long)stored_sz,
                   ratio, enc_mbps, ok ? "OK" : "FAIL");
        }
        free(raw);
    }

    printf("\nTotal F32: raw=%llu MB compressed=%llu MB ratio=%.2fx (all lossless)\n",
           (unsigned long long)(total_raw / 1000000),
           (unsigned long long)(total_comp / 1000000),
           (double)total_raw / (double)total_comp);

    fclose(f);
    gguf_idx_close(&idx);
    sid_cache_clear(&cache);
    printf("\n=== ALL DONE ===\n");
    return 0;
}
